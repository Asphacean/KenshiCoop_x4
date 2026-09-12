// Replicator - the per-tick sync orchestrator.
//
// Owns the flow between the engine and the network for replicated entities:
//   * publishOwned(): capture the locally-owned squad and hand it to the net
//     thread (called BEFORE the engine tick so the snapshot is current).
//   * ingest(): drain received entity transforms into per-entity interpolation
//     buffers (called BEFORE the engine tick so apply targets are current).
//   * applyTargets(): sample each buffer at the render time and apply the
//     interpolated pose to the resolved local Character (called AFTER the engine
//     tick so our write is the last word the renderer samples, beating the local
//     AI that re-decides each frame).
//
// Stage 2 routes apply through the interpolation buffer (smooth gliding); the
// transform is still committed via a raw teleport (no walk animation yet - that
// is Stage 3).

#ifndef KENSHICOOP_REPLICATOR_H
#define KENSHICOOP_REPLICATOR_H

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Interp.h"
#include "../../netproto/Wire.h"
#include "../core/Inbound.h"
#include "../net/NetLink.h"
#include "SyncContext.h" // Phase 6: per-tick channel call environment
#include "SyncTuning.h"  // Phase 6d: owned per-channel send-cadence tunables
#include "PinOwner.h"    // 06-01 (GAP-3/PLAY-03): pinPeer_ author bookkeeping
#include "XferCommit.h"  // Phase 7 (INV-02/INV-04): host-committed transfer state machine
#include "ClaimArbiter.h" // Phase 7 Plan 02 (INV-03): claim-contention arbiter
#include "CellMap.h"      // Phase 8 Plan 02 (WORLD-03): host-side cell-claim-map reduce
#include "MoneyFold.h"    // Phase 9 Plan 01 (CONS-01): shared-money-pool arbiter
#include "SpeedVote.h"    // Phase 9 Plan 02 (CONS-02): N-player speed min-vote reduce

class GameWorld;
class Character;
class RootObject;

namespace coop {

// Phase 3 (OWN-01/OWN-02) PlayerId sentinel: "not a tracked squad member" -
// returned by ownerOfHand() when a hand is not currently in the squad
// roster (world NPC/container/vendor never match). Namespace-scope (not a
// Replicator member) so Replicator::Driven's constructor member-init list
// can reference it directly - a C++03 build must not rely on the C++11 rule
// that a nested class is implicitly a friend of its enclosing class.
const coop::u32 OWNER_NONE = (coop::u32)-1;

class Replicator {
public:
    Replicator();

    // Stage 1/2 stream only the squad leader; later stages stream the whole squad.
    void setLeaderOnly(bool v) { leaderOnly_ = v; }

    // Stage 4: also stream nearby host-authoritative world NPCs (host side).
    void setStreamNpcs(bool v) { streamNpcs_ = v; }

    // Protocol 36 live-tuning knobs (KENSHICOOP_INTERP_* / _CATCHUP_K /
    // _SNAP_DIST): override the interp buffer's delay/extrapolation window and
    // the walk-drive's hard-snap / catch-up gains for WAN A/B runs without a
    // rebuild. Defaults match the historical constants.
    void setInterpConfig(const InterpConfig& c) { cfg_ = c; }
    void setDriveTuning(float catchupK, float snapDist, float snapSeconds) {
        if (catchupK > 0.0f) catchupK_ = catchupK;
        if (snapDist > 0.0f) snapDist_ = snapDist;
        if (snapSeconds > 0.0f) snapSeconds_ = snapSeconds;
    }
    // Combat-drive convergence bands (2026-07-16 smoothness pass). 0 keeps the
    // ReplicatorUtil constant default; > 0 overrides for a sweep without a rebuild.
    void setCombatTuning(float softDist, float snapDist, float bigSnapDist,
                         float slideMax, unsigned long convergeMs) {
        if (softDist > 0.0f)    combatSoftDist_    = softDist;
        if (snapDist > 0.0f)    combatSnapDist_    = snapDist;
        if (bigSnapDist > 0.0f) combatBigSnapDist_ = bigSnapDist;
        if (slideMax > 0.0f)    combatSlideMax_    = slideMax;
        if (convergeMs > 0)     combatConvergeMs_  = convergeMs;
    }
    // KENSHICOOP_SEND_STAMP=0: ignore the batch sendMs and index interp rings
    // on arrival time (the legacy scheme) - a receiver-local A/B lever.
    void setSendStamp(bool v) { sendStamp_ = v; }
    // KENSHICOOP_STARVE_HOLD_MS: how long a driven body whose stream went
    // STALE keeps its mutation guards (AI suspend + damage guard) before it
    // is released to local simulation. A brief WAN stall must not become an
    // authority transfer (architecture review 2026-07-10); 0 restores the
    // legacy release-on-stale behavior.
    void setStarveHold(unsigned long ms) { starveHoldMs_ = ms; }

    // Bidirectional presence: the SQUAD-TAB ranks (distinct hand-containers, sorted)
    // this client OWNS (controls locally + streams). The peer owns the other tabs and
    // we drive those from its stream. Host defaults to {0}, join to {1}. Runs on BOTH
    // clients now, so each streams a disjoint set of squad tabs from the one shared
    // squad. On a single-tab save only rank 0 exists -> the join owns nothing and the
    // prior one-directional behaviour is preserved.
    void setOwnRanks(const std::set<unsigned int>& r) { ownRanks_ = r; }

    // Phase 3 Plan 03 (OWN-01/OWN-03): apply the host-announced authoritative
    // map<PlayerId,set<rank>> (decoded from PKT_OWN_RANKS). Replaces the whole
    // map - the host always broadcasts the FULL roster, never a delta.
    // resolveRankOwner()/handOwner_ consult it, falling back to the
    // deterministic rank=playerId default (ownerForRank) when empty. Body in
    // ReplicatorCore.cpp (session lifecycle TU, alongside resolveRankOwner()).
    void setAllOwnRanks(const std::map<u32, std::set<unsigned int> >& m);

    // Phase 3 Plan 03: track the connected non-host roster (called from
    // Plugin.cpp's processNetEvents connect/leave drain) so the HOST can build
    // the authoritative ownership announcement in announceOwnRanks() below.
    // Harmless bookkeeping on a join (announceOwnRanks is host-gated).
    // Phase 8 review IN-02: zeroing claimMapMs_ forces assertDue on the
    // host's next computeAndBroadcastCellMap, so a fresh join adopts the
    // cell map immediately instead of failing every cell open to host for
    // up to CELL_ASSERT_MS (5 s) - a window in which OTHER joins' census
    // rows for cells they genuinely own were culled on the newcomer
    // (briefly ghosting bodies in that region). Mirrors announceOwnRanks'
    // connect-edge re-announce; harmless no-op on a join (the broadcast is
    // host-gated).
    void notePeerConnected(u32 id) { knownPeers_.insert(id); claimMapMs_ = 0; }
    void notePeerLeft(u32 id) { knownPeers_.erase(id); }

    // Protocol 58 (Phase 7 review CR-01): purge every conservation-plane
    // record keyed to `authorId`'s PREVIOUS connection the moment its
    // PlayerId slot is (re)assigned - called from Plugin.cpp's
    // processNetEvents CONNECT loop, which runs BEFORE tickReplicateApply
    // drains any intent/verdict from the new connection, so the new
    // connection's restarted per-sender counters (transferId/netId/dropId/
    // pickupId all restart at 1 in a fresh client process) can never collide
    // with stale "commit is final" / dedup memory from the dead connection.
    // Purging at the CONNECT edge - not at disconnect - is deliberate: it
    // preserves host-final-else-rollback for the ORIGINAL transfers (late
    // duplicates still in flight right after a leave are still answered
    // "commit is final"), lets claim windows the departed author's items had
    // open finalize normally among the surviving claimants, and still sweeps
    // the final records those post-departure finalizations minted. A no-op
    // on a first-ever connect (nothing recorded for that id). Phase 8 review
    // WR-01: the CHANNEL seq guards are in scope too - the rejoining
    // author's per-sender seqSeen entry is erased from every doorRows_/
    // bdoorRows_/facRows_/deedRows_ row (same seq-restart rationale), and
    // the prodRows_ scalar is reset when the restarted author is the host
    // (the only author that channel ever has). Body in ReplicatorCore.cpp
    // (session lifecycle TU).
    void purgeAuthorConservationState(u32 authorId);

    // WR-02 fix: a UI-driven disconnect (coopUiDisconnect/sessionResetForUi)
    // stops the net thread BEFORE any ENET_EVENT_TYPE_DISCONNECT is drained
    // through processNetEvents()'s leave loop, so notePeerLeft() never runs
    // for whichever peers were connected at disconnect time - knownPeers_ and
    // allOwnRanks_ (neither cleared by resetSession(), which deliberately
    // preserves them across a WORLD-RELOAD) would otherwise leak stale
    // PlayerIds into the next hosted session in the same process. A full
    // disconnect is a session BOUNDARY, not a reload, so this state must not
    // survive it. Safe to call on either role (harmless no-op on a join,
    // exactly like notePeerConnected/notePeerLeft above).
    void clearKnownPeers() { knownPeers_.clear(); allOwnRanks_.clear(); }

    // Phase 3 Plan 03 (OWN-01/OWN-03, T-03-06): HOST ONLY - build & broadcast
    // the authoritative map<PlayerId,set<rank>> via NetLink::broadcastOwnRanks().
    // No-op if this instance is not currently the host (isHostRole()). Called
    // from Plugin.cpp on a roster edge (connect/leave) and from publishOwned
    // (ReplicatorPublish.cpp) whenever the tab set changes (tabsChanged_).
    void announceOwnRanks(NetLink& net, u32 ownerId);

    // Phase 3 (OWN-01/OWN-02): who owns hand h? Returns the owning PlayerId,
    // or OWNER_NONE if h is not a currently-tracked squad member (world
    // NPC/container/vendor). Backed by handOwner_, which publishOwned()
    // refreshes every tick from the tab-rank scan (deterministic default:
    // ownerForRank(rank) == rank; a host-authoritative announcement may
    // override that mapping in a later plan without changing this API).
    // Const + map-lookup only, so it is safe to call from the engine tick
    // (the UI-drag detour runs on the same main thread).
    u32 ownerOfHand(const unsigned int h[5]) const {
        Key k; k.t = h[0]; k.c = h[1]; k.cs = h[2]; k.i = h[3]; k.s = h[4];
        std::map<Key, u32>::const_iterator it = handOwner_.find(k);
        return it == handOwner_.end() ? OWNER_NONE : it->second;
    }

    // Phase 3 (OWN-02): is hand h one of THIS client's own squad members
    // (locally owned, streamed by publishOwned, never driven by remote
    // replication)? Const + set-lookup only, so it is safe to call from the
    // engine tick.
    bool isMine(const unsigned int h[5]) const {
        Key k; k.t = h[0]; k.c = h[1]; k.cs = h[2]; k.i = h[3]; k.s = h[4];
        return isMineKey(k);
    }

    // Cross-owner trade veto classifier (engine InvOwnerClassFn) - now a thin
    // ADAPTER over ownerOfHand()/isMine() that preserves the exact 0/1/2
    // return the engine's InvOwnerClassFn contract relies on (Engine.h:733-
    // 735: "A move is cross-owner iff one end is 1 and the other is 2.").
    // Given a save-stable owner hand (readObjectHand layout [type,container,
    // containerSerial,index,serial]) returns 0 = not a player-squad member,
    // 1 = a squad member owned by THIS client, 2 = a squad member owned by
    // someone else (formerly "the PEER"; now any other tracked owner).
    int ownerClassForHand(const unsigned int h[5]) const {
        if (ownerOfHand(h) == OWNER_NONE) return 0;
        return isMine(h) ? 1 : 2;
    }

    // Scenario support (assault_mint): the hand of the MINTED proxy nearest the
    // body at refHand - a body this client had no local copy of and had to create
    // from the peer's spawn INFO. Only the proxy table knows this; the arm-time
    // NPC census cannot stand in for it, because a resident that wanders into
    // range later is also "new" and picking one of those quietly turns the
    // reproduction back into assault_town (measured, run 20260805_224826).
    // outLocal is the hand that RESOLVES HERE (what an attack order needs);
    // outCanon is the peer's key the body is streamed by (what the logs and the
    // oracle pair on) - a proxy has both, and they are never the same. outDist
    // is the planar distance to the reference body. Hands are readObjectHand
    // layout [type,container,containerSerial,index,serial].
    bool pickMintedProxyNear(GameWorld* gw, const unsigned int refHand[5],
                             unsigned int outLocal[5], unsigned int outCanon[5],
                             float* outDist) const;

    // AI-gating probe (join side): recruit diverged NPCs into the player squad to
    // validate the "inhabit" lever (stops self-tasking + becomes drivable).
    void setProbeRecruit(bool v) { probeRecruit_ = v; }

    // AI-suspend (join side, default ON): suspend the AI decision layer for
    // host-driven NPCs (faction-safe) so they stop self-tasking but keep
    // animating, and let the host stream be the sole task authority. The
    // universal quieting layer; per-class apply levers sit on top.
    void setAiSuspend(bool v) { aiSuspend_ = v; }

    // Step-2 experiment (KENSHICOOP_NO_DETACH=1): skip the sitter detachFromTownAI
    // in applyRest, betting that AI-suspend alone stops town-AI re-tasking. Off by
    // default; flip for a manual A/B before deleting the detach lever for good.
    void setNoDetach(bool v) { noDetach_ = v; }

    // Damage guard (join side, default ON): every driven (non-owned) body joins the
    // hitByMeleeAttack-suppressed set each tick, so the join's cosmetic fights apply
    // no local damage - outcomes stay host-authoritative (KO/death events).
    void setDamageGuard(bool v) { dmgGuard_ = v; }

    // Step 4 (KENSHICOOP_GATE_AUTHORITY=1, default off): divergence-gated authority.
    // A world NPC whose LOCAL AI has agreed with the host's rawTask for a sustained
    // streak while staying in position enters TRUSTED mode - no suspend, no drive,
    // cheap monitor only; re-engaged instantly on divergence or drift (doctrine 18).
    void setGateAuthority(bool v) { gateAuthority_ = v; }

    // Carried-body sync (protocol 18, default ON): reliable pickup/drop edges +
    // self-healing carried state for player-squad members, executed engine-
    // native on each machine's local pair. KENSHICOOP_CARRY_SYNC=0 disables.
    void setCarrySync(bool v) { carrySync_ = v; }

    // Peer-left sweep (carried-body sync): any driven (peer-owned) copy still
    // carrying a body after its owner disconnected gets a local drop, so the
    // carried body is released back to the ordinary KO/down channels.
    void sweepCarries(GameWorld* gw);

    // Furniture occupancy sync (protocol 19, default ON): reliable enter/exit
    // edges + self-healing BODY_IN_BED/BODY_IN_CAGE state, executed engine-
    // native (setBedMode/setPrisonMode) on each machine's local pair.
    // KENSHICOOP_FURN_SYNC=0 disables.
    void setFurnSync(bool v) { furnSync_ = v; }

    // Chained/pole prisoner sync (protocol 41, default ON): rides the furniture
    // pipeline as kind=3 (Character::isChained -> setChainedMode). A captive on
    // a prisoner pole is shackled, not caged, so the cage path never saw it.
    // KENSHICOOP_CHAIN_SYNC=0 disables JUST the chain kind (beds/cages keep
    // working) - the A/B escape hatch if it ever freezes a walking slave.
    void setChainSync(bool v) { chainSync_ = v; }

    // Stealth sync (protocol 20, default ON): continuous BODY_SNEAK posture
    // apply on driven copies (engine-native setStealthMode) + the detection-
    // indicator feedback stream. KENSHICOOP_STEALTH_SYNC=0 disables.
    void setStealthSync(bool v) { stealthSync_ = v; }

    // Prone posture sync (protocol 53, default ON): continuous ProneState apply
    // on driven copies (engine-native setProneState), so an injured crawler is
    // posed as one instead of being walk-driven upright. The crippled FLAG (the
    // cause) rides the medical channel. KENSHICOOP_PRONE_SYNC=0 disables.
    void setProneSync(bool v) { proneSync_ = v; }

    // Runtime-spawn proxy replication (protocol 21, default ON): the join
    // requests a description for any streamed hand it cannot resolve (a host
    // RUNTIME spawn) and mints a local proxy body bound at the applyTargets
    // resolve choke point. KENSHICOOP_SPAWN_SYNC=0 disables (and spawn_probe
    // forces it off to baseline the failure modes).
    void setSpawnSync(bool v) { spawnSync_ = v; }

    // BEFORE engine (protocol 21 -> 23, both clients): the describe/mint channel,
    // BIDIRECTIONAL since protocol 23 (a join RECRUIT of a runtime-born NPC mints
    // a hand the host cannot resolve). Each side both:
    //  * sends debounced PKT_SPAWN_REQ for hands applyTargets recorded as
    //    unresolved near the squad; drains PKT_SPAWN_INFO replies and mints local
    //    proxy bodies (engine::spawnProxyNpc), binding them in proxyByKey_;
    //  * drains PKT_SPAWN_REQ, resolves + describes the body
    //    (engine::describeCharacter) and replies PKT_SPAWN_INFO (throttled per
    //    hand; found=0 = negative reply, stops the peer's retries).
    void syncSpawns(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

    // Phase 4a: register a container (by hand) this client is AUTHORITATIVE for, so
    // publishInventories streams its contents and applyInventories never reconciles
    // it back onto us. hand layout = [type, container, containerSerial, index, serial].
    // v1 registers a single host-owned baked storage container; clears prior set.
    void setOwnedContainerHand(const unsigned int hand[5]);
    void clearOwnedContainers() { ownedContainers_.clear(); }

    // BEFORE engine: drain received entities into their interpolation buffers.
    void ingest(Inbound& in);

    // BEFORE engine: drain received container-contents snapshots (Phase 4a) into the
    // per-container latest-snapshot cache (marks them dirty for applyInventories).
    void ingestInv(Inbound& in);

    // BEFORE engine (join side): drain reliable transition events and latch them
    // onto the matching tracked body (death = held down permanently; revive clears).
    // gw is needed by the EVT_RECRUIT re-key (restoring a suppressed local body).
    void applyEvents(GameWorld* gw, Inbound& in);

    // BEFORE engine: capture the locally-owned squad and publish it (host side).
    // Also detects per-entity bodyState transitions (KO/death/revive) and queues the
    // matching reliable event on 'net'.
    void publishOwned(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine: for each owned container, capture its contents and queue a
    // reliable snapshot when the content fingerprint changed (or a periodic safety
    // resend elapsed). No-op when no owned container is registered / resolves.
    void publishInventories(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (BOTH clients since the W1 bidir fix): scan the interest sphere for
    // free ground items WE author (peer proxies are filtered by the echo guard), assign/
    // reuse a netId per item (keyed by its local engine hand), and queue a reliable
    // snapshot for new/changed items + a reliable cull for items that vanished. A settled
    // world produces no traffic (change-detected), with a slow periodic safety resend.
    // Also emits the protocol-47 CLAIM half: any proxy WE hold that has just entered a
    // local bag is reported to its author, which then destroys its real ground copy. Before
    // this, a non-gear pickup left the author's item lying there indefinitely (its own cull
    // is liveness-based on that still-live object) - the item was duplicated, and the host
    // "kept seeing" what the join had already picked up.
    void publishWorldItems(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (BOTH clients): drain received world-item snapshots/culls and
    // reconcile local proxies - spawn a proxy for a new (ownerId, netId), move it if it
    // changed, destroy it on cull. netId spaces are per-sender, culls owner-scoped.
    void applyWorldItems(GameWorld* gw, Inbound& in);

    // AFTER engine (protocol 47, SUPERSEDED by protocol 58's applyClaimVerdict
    // below, Phase 7 Plan 02): dormant since Plan 02 - no longer called from
    // Plugin.cpp. The old body destroyed our real ground object unconditionally
    // on ANY notice - the "no track; already gone" silent no-op this left when
    // a SECOND simultaneous claimant's notice arrived after the first already
    // destroyed the object is exactly the 2-player dup this plan fixes (two
    // claimants each kept a copy). Kept only as a drain-and-discard stub so a
    // stray claim received from a mismatched build (impossible - PROTOCOL_VERSION
    // gates the handshake) cannot grow InboundWorldClaim's queue unbounded.
    void applyWorldClaims(GameWorld* gw, Inbound& in, u32 localId);

    // AFTER engine, HOST ONLY (protocol 58, Phase 7 Plan 02 INV-03): drain
    // received claim INTENTS (PKT_WORLD_ITEM_CLAIM) and feed ClaimArbiter.h's
    // bounded contention window, keyed (authorId, netId). Each claim's
    // authorClaimMs is mapped into the host's local clock via the existing
    // per-owner peerClock_ offset (mappable=false when the claimant has no
    // peerClock_ entry yet - treated as an automatic tie-band member, never
    // unusable). Every tick, finalizes any window whose contention length has
    // elapsed (coop::claimWindowExpired) and broadcasts exactly one
    // ClaimVerdictPacket via net.queueClaimVerdict naming the deterministic
    // winner (coop::finalizeClaim). A claim for an already-committed identity
    // (coop::claimCommitted) is answered with the SAME prior verdict re-sent
    // (commit is final - never re-arbitrated, never re-broadcast).
    void applyClaimIntents(GameWorld* gw, Inbound& in, NetLink& net, u32 localId);

    // AFTER applyClaimIntents (protocol 58, Phase 7 Plan 02 INV-03, ALL
    // clients): drain PKT_CLAIM_VERDICT - by construction every entry here is
    // host-authored (NetLink's receive branch only pushes PKT_CLAIM_VERDICT on
    // a client, `!isHost_`, and REJECTS + logs one a client tries to author,
    // the PKT_XFER_COMMIT/PKT_OWN_RANKS precedent). If I am the item's AUTHOR
    // (authorId==localId): destroy my real ground object for netId (the verdict-
    // gated replacement for applyWorldClaims' old unconditional destroy) and
    // leave the track so the ordinary cull converges third peers. If I am a
    // LOSING claimant (I claimed this identity but winnerPlayerId != me): roll
    // back my optimistic local pickup - the ONE allowed reverse mutation
    // (research Pitfall 1) - by removing the exact claimed instance from my
    // claiming character's bag. If I am the WINNER: no-op (I already keep it).
    void applyClaimVerdict(GameWorld* gw, Inbound& in, u32 localId);

    // BEFORE engine (Phase W2/W3, runs on EVERY client): diff each OWNED character's WEAPON
    // census. A sustained count DECREASE is a DROP (the weapon left the bag; we never mutate
    // owned inventories ourselves) - author a reliable DROP intent at the owner position (the
    // engine's spatial item query can't find town drops, so we don't require it). A count
    // INCREASE is a PICKUP - author a reliable PICKUP intent. Tracks the real dropped Item*
    // handle per sid so the conservation channel can re-home the exact object on pickup.
    void detectAndPublishWeaponDrops(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (Phase W2): drain received DROP intents and relocate THIS client's own
    // copy of the weapon to the ground (Inventory::dropItem, no fabrication). Idempotent by
    // (ownerId,dropId); never acts on a character we own (we already dropped it locally).
    // Runs BEFORE applyInventories so the relocation beats the debounced removal reconcile.
    // Tracks the relocated Item* so a later pickup can re-home that exact object.
    void applyWeaponDrops(GameWorld* gw, Inbound& in);

    // AFTER engine (Phase W3): drain received PICKUP intents and re-home THIS client's tracked
    // ground copy of the weapon back into the picking character's bag (no fabrication, no
    // spatial re-query - uses the remembered Item* handle). Idempotent by (ownerId,pickupId);
    // never acts on a character we own (we already picked it up locally). Runs BEFORE
    // applyInventories so the re-home beats the inventory reconcile.
    void applyWeaponPickups(GameWorld* gw, Inbound& in);

    // BEFORE engine, every tick (Phase W3 convergence): keep groundedWeapons_ honest.
    // Nothing used to prune it, so a cached ground Item* survived the object's despawn and
    // a later re-home handed a dead pointer to the engine. Two jobs:
    //   1) PRUNE tracks whose object no longer resolves as a free ground item. Nothing is
    //      left on the ground, so there is nothing to duplicate.
    //   2) RETRY a re-home that failed (bag refused the object) and, once past
    //      WD_REHOME_MAX_MS, resolve the split - but ONLY after reading the item in the
    //      target container, so the ground copy is destroyed strictly as a duplicate and
    //      never as the last instance.
    void reconcileGroundGear(GameWorld* gw);

    // BEFORE engine, after publishInventories (protocol 37, BOTH clients): detect a
    // COMPLETED cross-owner drag. Every tracked container (own + received) keeps a
    // per-item-total baseline; a scan (~2.5 Hz) that finds a LOSS in one container
    // and the matching GAIN in another - stable for a settle window, at least one
    // end peer-authored - is a direct UI trade the single-writer snapshots cannot
    // represent. Author ONE reliable PKT_INV_XFER (src+dst+item+qty), latch the
    // pending transfer so applyInventories cannot reconcile it back while the
    // owner's snapshots are still stale, and (for gear) suppress the W2 weapon-
    // census drop/pickup reading of the same count edges. Own<->own moves and
    // unpaired diffs are folded into the baseline (own snapshots already carry
    // them; a lone loss is a drop/consume, not a trade).
    void detectAndPublishTransfers(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (protocol 37, SUPERSEDED by protocol 58's applyXferCommit
    // below, Phase 7): dormant since Plan 01 Task 2 - no longer called from
    // Plugin.cpp. Kept only as a drain-and-discard stub so a stray intent
    // received from a mismatched build (impossible - PROTOCOL_VERSION gates
    // the handshake) cannot grow InboundInvXfer's queue unbounded.
    void applyTransfers(GameWorld* gw, Inbound& in, NetLink& net, u32 localId);

    // AFTER applyTransfers (protocol 50, SUPERSEDED by protocol 58's
    // applyXferCommit below, Phase 7): dormant since Plan 01 Task 2 - no
    // longer called from Plugin.cpp. Kept only as a drain-and-discard stub.
    void applyXferAcks(GameWorld* gw, Inbound& in, u32 localId);

    // AFTER engine, HOST ONLY (protocol 58, Phase 7 INV-02): drain received
    // transfer intents (PKT_INV_XFER) and arbitrate them via XferCommit.h's
    // pendingXfer_ state machine, keyed (authorId, transferId). Validates the
    // author actually owns the source OR destination domain
    // (ownerOfHand(src)==author or ownerOfHand(dst)==author), decides the
    // outcome ONCE (relocate the real item / fabricate a shortfall under the
    // same backpack-never-fabricate + KENSHICOOP_WEAPON_FAB gate applyTransfers
    // used / reject) and broadcasts exactly one XferCommitPacket per fresh
    // commit via net.queueXferCommit. Host-as-participant: applies its own
    // relocation in the SAME pass (so the host's own containers are correct
    // immediately) but still broadcasts the identical commit, so every
    // client's log evidence is the same regardless of who the host is.
    void processXferIntents(GameWorld* gw, Inbound& in, NetLink& net, u32 localId);

    // AFTER processXferIntents (protocol 58, Phase 7 INV-02/INV-04, ALL
    // clients): drain PKT_XFER_COMMIT - by construction every entry here is
    // host-authored (NetLink's receive branch only pushes PKT_XFER_COMMIT on
    // a client, `!isHost_`, and REJECTS + logs one a client tries to author,
    // the PKT_OWN_RANKS T-03-06 precedent; a client's single ENet connection
    // to the host is also its own topological guarantee - there is no other
    // peer it could have come from). Dedup on
    // (authorId, transferId) via appliedXfers_ (rekeyed to the commit's
    // authorId so N authors' transferId=1 never collide - the Phase 5 rule).
    // For RELOCATE/FABRICATE: reuses the relocate+fabricate body verbatim
    // (driven by the commit's outcome/applied, never an independent
    // per-receiver fabricate decision), creates/adjusts xferLatch_ on both
    // peer ends and RELEASES it on this commit (not on a peer ACK), calls
    // xferRebase on both keys and registers wdSuppress_ for gear. For REJECT:
    // releases every latch for (author,transferId) and does NOTHING else -
    // the next authoritative snapshot reconciles the optimistic local move
    // away through applyInventories (the existing rollback path, no second
    // mutation). A participant additionally authors a XferCommitAckPacket
    // (bookkeeping/audit only). This REPLACES applyTransfers/applyXferAcks as
    // the settle path.
    void applyXferCommit(GameWorld* gw, Inbound& in, NetLink& net, u32 localId);

    // AFTER applyXferCommit, HOST ONLY (protocol 58): drain transfer-commit
    // bookkeeping acks and log for audit/correlation. No state change.
    void applyXferCommitAck(Inbound& in, u32 localId);

    // BEFORE engine, after publishOwned (phase 2 vitals sync): stream each OWNED
    // player-squad member's medical model (blood, bleed, limb flesh/bandaging,
    // flags) on the RELIABLE channel - change-gated by a quantized fingerprint,
    // ~2 Hz sampling, periodic safety resend. World NPCs are never included
    // (events-only model). Uses the ownHands_ set publishOwned just refreshed.
    void publishMedical(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (phase 2): drain received medical snapshots and write them
    // onto our DRIVEN copies (never our own bodies), remembering the last
    // received bandage levels per body. Also runs the treatment DETECTOR: local
    // bandaging risen ABOVE the last received level means first aid happened on
    // this machine on a driven copy - forward the levels reliably to the owner.
    void applyMedical(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId);

    // BEFORE engine (phase 2): drain received treatment deltas; each subject we
    // OWN gets the bandage levels applied raise-only (idempotent). The vitals
    // stream then mirrors the healed state back to everyone.
    void applyTreatments(GameWorld* gw, Inbound& in);

    // JOIN only (protocol 45): forward the join-dealt damage accumulated on driven
    // world-NPC copies (drained into pendingHits_ during applyTargets) to the host
    // as reliable CombatHitPackets. The join's local swing is guarded (cosmetic),
    // so this is the only path by which the join PC actually wounds the host's NPC.
    void publishCombatHits(GameWorld* gw, NetLink& net, u32 ownerId);

    // HOST only (protocol 45): drain received join-dealt damage reports and apply
    // them AUTHORITATIVELY to the world NPC the host owns (blood loss + a frontal
    // flesh wound via woundSubjectLimbs). Ignores reports for bodies the host is
    // not the combat authority for (partition safety, like applyTreatments).
    // Phase 4 Plan 03 Task 2 (T-04-08, dormant N>2 fail-safe): also refuses a
    // report whose target hand is ANY tracked squad member (allSquad_) other
    // than the host's own - at N>=3 this could otherwise be another player's
    // squad body (join2 vs join3 PvP), which the host is never authoritative
    // for. Currently unreachable (ReplicatorDrive.cpp's join-side capture
    // already filters isSquad out before pendingHits_ is ever populated), but
    // the host-side apply had no equivalent backstop of its own - this closes
    // that asymmetry defense-in-depth, log-and-drop, never silently applied.
    void applyCombatHits(GameWorld* gw, Inbound& in);

    // Enable join-dealt damage reporting (join only; see publishCombatHits).
    void setReportCombat(bool v) { reportCombat_ = v; }

    // AFTER publishOwned (protocol 17, both clients): stream each OWNED
    // player-squad member's CharStats (attributes/skills/xp) on the RELIABLE
    // channel - change-gated by a quantized fingerprint, ~1 Hz floor, periodic
    // safety resend. The peer's engine resolves REAL fights with its copy of
    // these numbers, so staleness is a gameplay bug, not a display one.
    void publishStats(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 17): drain received stats snapshots and write
    // them onto our DRIVEN copies (never our own bodies) + recalculate the
    // derived caches. Also self-heals the junk XP a driven copy's cosmetic
    // fights accumulate locally (the owner's stream overwrites it).
    void applyStats(GameWorld* gw, Inbound& in);

    // BOTH clients (protocol 52): sample the player's SHARED money pool
    // (readPlayerWallet) against our baseline and turn any local movement -
    // purchase, sale, loot, bounty, hire - into wire traffic. One int read a
    // tick catches every economy path without hooking each of them.
    //   host: its engine wallet IS the pool, so a local move just re-publishes
    //         the authoritative total (change-gated, safety resend).
    //   join: a local move becomes a signed PKT_MONEY_DELTA held pending until
    //         the host acks it.
    void publishMoneyPool(const SyncContext& ctx);

    // BEFORE engine (protocol 52): the receive half of the pool.
    //   host: fold each inbound delta into the pool (clamped at 0 - the
    //         both-players-spent-the-same-cats race), write it back and
    //         re-publish so the join converges on the authoritative total.
    //   join: adopt the host's total PLUS our own still-unacked deltas, so a
    //         purchase from a moment ago is not reverted and then re-applied.
    void applyMoneyPool(const SyncContext& ctx);

    // Shared money-pool sync master enable (KENSHICOOP_MONEY_SYNC).
    void setMoneySync(bool v) { moneySync_ = v; }

    // BEFORE engine (protocol 23, both clients): drain the engine's recruit-edge
    // queue (the PlayerInterface::recruit detour records every successful LOCAL
    // recruit) and author one reliable EVT_RECRUIT per edge (subject = the OLD
    // hand, actor = the NEW hand). Also pins each recruited hand into
    // pinOwned_ so publishOwned streams it even when the engine parked it
    // in a tab RANK the partition says the peer owns (recruit_probe: a join
    // recruit landed in the host's rank-0 container).
    void publishRecruits(GameWorld* gw, NetLink& net, u32 ownerId);

    // Recruitment sync master enable (KENSHICOOP_RECRUIT_SYNC).
    void setRecruitSync(bool v) { recruitSync_ = v; }

    // BEFORE engine (protocol 35, both clients): poll the roster's pointer ->
    // hand baseline (~2 Hz; the Character* survives a squad-tab move, only its
    // hand changes - squad_probe) and author one reliable EVT_SQUAD_MOVE per
    // detected edge (subject = OLD hand, actor = NEW hand; zeroed actor = the
    // body left the roster). Pins the new hand into pinOwned_ BEFORE the wire
    // so publishOwned keeps streaming the moved member no matter which tab
    // rank its new container latched to. The receive half (shared EVT_RECRUIT
    // re-key) lives in applyEvents.
    void publishSquadMoves(GameWorld* gw, NetLink& net, u32 ownerId);

    // Squad management sync master enable (KENSHICOOP_SQUAD_SYNC). Also gates
    // the container-rank LATCH: with it on, tab ranks are assigned once at
    // first census and newly-seen containers APPEND (a mid-session tab can
    // never reshuffle existing ranks / flip whole-tab ownership); off = the
    // legacy per-tick sorted ranking (the squad_probe baseline).
    void setSquadSync(bool v) { squadSync_ = v; }

    // BEFORE engine (protocol 24, both clients): sample the player-faction
    // relation table (~1 Hz, immediately when the affectRelations detour saw a
    // mutation) and stream every row whose value MOVED since the seeded
    // baseline (change-gated reliable, per-sid safety resend). Both clients
    // run the same detector, so a change replicates from whichever engine it
    // happened on; applying a received row updates the local baseline, which
    // is what makes the channel echo-free.
    void publishFactions(const SyncContext& ctx);

    // BEFORE engine (protocol 24): drain received relation rows and write each
    // one onto BOTH local table directions via FactionRelations::setRelation
    // (the probe showed the engine keeps them mirrored). Stale rows (per-sid
    // seq guard) and already-converged rows are skipped.
    void applyFactions(const SyncContext& ctx);

    // Faction-relation sync master enable (KENSHICOOP_FACTION_SYNC).
    void setFactionSync(bool v) { factionSync_ = v; }

    // BEFORE engine (protocol 26, both clients): sample baked doors near the
    // interest centers (~1 Hz) and stream every row whose (open, locked)
    // moved since the seeded per-hand baseline (change-gated reliable,
    // per-hand safety resend for rows ever sent). Both clients run the same
    // detector, so a door change replicates from whichever engine it
    // happened on (a local click, an NPC, or the write applyDoors made).
    void publishDoors(const SyncContext& ctx);

    // BEFORE engine (protocol 26): drain received door rows; each one that
    // resolves locally is applied through the engine's own door actions
    // (openDoor/closeDoor + lockDoor/unlockDoor). The baseline updates
    // BEFORE the write (echo-free); stale rows (per-hand seq guard) and
    // already-converged rows are skipped; unresolvable hands are skipped
    // silently (out-of-interest or runtime door - accepted edge).
    void applyDoors(const SyncContext& ctx);

    // Door-state sync master enable (KENSHICOOP_DOOR_SYNC).
    void setDoorSync(bool v) { doorSync_ = v; }

    // BEFORE engine (protocol 27, both clients): drain local placement edges
    // (the placeFinalPreviewBuilding detour + programmatic scenario places)
    // into PKT_BUILD_PLACE announcements keyed by OUR hand, then sample every
    // building WE placed (~1 Hz) and stream change-gated PKT_BUILD_STATE
    // progress rows (10 s safety resend while incomplete; the final
    // complete=1 row latches the channel silent for that site).
    void publishBuilds(const SyncContext& ctx);

    // BEFORE engine (protocol 27): drain received announcements - each new
    // key mints a local construction site through the same createBuilding
    // factory (town-rule-free, probe-proven) and enters the key -> local-hand
    // translation map (a refused mint is remembered so resends don't retry) -
    // then apply received progress rows through the engine's own
    // setConstructionProgress via that map (per-key seq guard drops stale
    // rows; unknown keys are skipped silently).
    void applyBuilds(const SyncContext& ctx);

    // Placed-building sync master enable (KENSHICOOP_BUILD_SYNC).
    void setBuildSync(bool v) { buildSync_ = v; }

    // BEFORE engine (protocol 28, both clients): sample the doors of every
    // building in the session build maps (~1 Hz) and stream change-gated
    // PKT_BUILD_DOOR rows on the TRANSLATED identity (placer's building hand
    // + door index; own placements key by our hand, minted proxies through
    // the reverse map) - the protocol-26 symmetric door shape, so a placed
    // door replicates from whichever engine moved it.
    void publishBuildDoors(const SyncContext& ctx);

    // BEFORE engine (protocol 28): drain received placed-door rows; resolve
    // the key through the build maps (own hand or minted local hand), read
    // door #index of that building, and apply through the engine's own door
    // actions. Baseline updates BEFORE the write (echo-free); per-key seq
    // guard; unknown/tombstoned keys skip silently.
    void applyBuildDoors(const SyncContext& ctx);

    // Placed-building door + removal sync master enable (KENSHICOOP_BDOOR_SYNC).
    void setBdoorSync(bool v) { bdoorSync_ = v; }

    // Hunger sync enable (protocol 29, KENSHICOOP_HUNGER_SYNC): whether the
    // hunger/fed scalars ride the medical snapshot (OFF sends/applies them
    // as -1 = not carried; the rest of the stream is untouched).
    void setHungerSync(bool v) { hungerSync_ = v; }

    // BEFORE engine (protocol 33, HOST only - the world-simulation
    // authority): sample machine-class buildings near the interest centers
    // (~1 Hz) and stream a PKT_PROD row per machine - the first sight of a
    // machine SENDS (the host's state IS the baseline, unlike the symmetric
    // door channel), then change-gated on the quantized fields with a 10 s
    // safety resend (which is also what keeps a join whose own engine
    // drifted converging). Identity: baked hand (keyKind=0) or protocol-27
    // placer key through the build maps (keyKind=1).
    void publishProd(const SyncContext& ctx);

    // BEFORE engine (protocol 33, join side): drain received machine rows;
    // resolve the key (baked hand directly; placer key through ownBuilds_/
    // peerBuilds_), read the local machine, and apply only what actually
    // diverged through the engine's own levers (switchPowerOn / direct
    // amount + farm-float writes; a still-null output buffer is first
    // MATERIALIZED via the native setProductionItem). Per-key seq guard
    // drops stale rows; unresolvable keys skip silently (out-of-interest).
    void applyProd(const SyncContext& ctx);

    // Production machine sync master enable (KENSHICOOP_PROD_SYNC).
    void setProdSync(bool v) { prodSync_ = v; }

    // BEFORE engine (protocol 38, HOST only - the tech-tree authority):
    // sample the Research store's known set ~1 Hz (Research::isKnown over
    // the shared RESEARCH GameData enumeration) and stream one PKT_RESEARCH
    // row per known sid - first sight sends (the host's known set IS the
    // session baseline), then a safety resend covers a lost row / a join
    // whose apply lever needed prerequisites that arrived later.
    void publishResearch(const SyncContext& ctx);

    // BEFORE engine (protocol 38, join side): drain received known-research
    // rows; sids already known locally are skipped (idempotent), the rest
    // apply through Research::startResearch - the exact lever a research-UI
    // click commits (flips isKnown in the same tick, spike 401). Per-sid
    // seq guard drops stale rows.
    void applyResearch(const SyncContext& ctx);

    // Research tech-tree sync master enable (KENSHICOOP_RESEARCH_SYNC).
    void setResearchSync(bool v) { researchSync_ = v; }

    // BEFORE engine (protocol 54, BOTH clients - either may buy): sample the
    // player faction's owned-BUILDING set ~1 Hz and stream one PKT_DEED row per
    // owned hand - first sight sends (the loaded save's owned set IS the shared
    // baseline, so this is near-free), then a safety resend that doubles as the
    // retry for a peer whose copy of the building was not loaded when the row
    // first landed. An un-own row is published ONLY from an observed transition
    // on a building that still resolves locally and reads not-owned: a hand that
    // merely stopped resolving means "unloaded", never "sold", so it stays
    // latched and silent (the protocol 46/47 additive-only rule).
    void publishDeeds(const SyncContext& ctx);

    // BEFORE engine (protocol 54): drain received deed rows and apply each as a
    // PURE STATE WRITE (setFaction + add/removeOwnedObject) - never
    // Building::buyMeCallback, which would debit the cats a second time and be
    // re-published by publishMoneyPool's wallet sampler as a fresh local spend.
    // Per-hand seq guard drops stale rows; the baseline is updated BEFORE the
    // engine write (the echo guard). A hand that does not resolve yet leaves the
    // row unapplied so the safety resend retries it - the one place this channel
    // must NOT behave like the door channel, which drops such a row for good.
    void applyDeeds(const SyncContext& ctx);

    // Property-deed sync master enable (KENSHICOOP_DEED_SYNC).
    void setDeedSync(bool v) { deedSync_ = v; }

    // AFTER engine (protocol 55): announce the container/machine-class fixtures
    // near our interest centers so the peer can pair our hand for each with its
    // own copy. Rows are STATIC (position + template), so this is first-sight
    // plus a slow safety resend - the resend doubles as the retry for a peer
    // whose zone was not loaded when the row first landed. Runs on BOTH sides:
    // the pose path is bidirectional, and protocol 33/34 are host-authored, so
    // each direction needs the other's key space.
    void publishFixtures(const SyncContext& ctx);

    // BEFORE engine (protocol 55): drain received fixture rows and pair each
    // sender hand with the LOCAL hand of the same physical object, matched on
    // position with the template sid and building class as guards. Stores only
    // identity - no gameplay state is written here. An unmatched row records
    // nothing and waits for the safety resend.
    void applyFixtures(const SyncContext& ctx);

    // Runtime-fixture sync master enable (KENSHICOOP_FIXTURE_SYNC).
    void setFixtureSync(bool v) { fixtureSync_ = v; }

    // Phase 6c: drive the change-gated SAMPLED channels (faction, doors, placed
    // buildings, placed-building doors, production, research) from one
    // channel-descriptor registry. Replaces the per-channel if-blocks the tick
    // used to inline: the table owns each channel's master enable, direction
    // (symmetric vs host-authoritative), and cadence ORDER (which must match the
    // historical publish sequence). Called once per pre-engine tick with the
    // SyncContext; each row's own body still guards on its enable + gameplay
    // liveness. Money/recruit/squad/stealth/speed/time stay explicit (different
    // cadence positions / patterns).
    void driveSampledChannels(const SyncContext& ctx);

    // Storage/machine container sync (protocol 34, KENSHICOOP_STORE_SYNC):
    // when set (HOST only - host-authoritative world containers), a ~1 Hz
    // census of container-bearing buildings (STORAGE + the machine classes)
    // in the interest spheres auto-registers each as an AUTHORED container,
    // so publishInventories streams it through the existing change-gated
    // hash+settle logic. Session-placed buildings translate to the wire via
    // the protocol-27 build maps (keyKind=1); the receiver resolves back in
    // ingestInv. Layered on invSync (publishInventories is the carrier).
    void setStoreSync(bool v) { storeSync_ = v; }

    // Connect-edge resync (protocol 30, no wire change): called from the
    // game-thread connect drain (processNetEvents) when a peer appears (host
    // on HELLO, join on WELCOME - covers late joins AND quick reconnects).
    // Re-announces every live placed building's PKT_BUILD_PLACE (+ a
    // PKT_BUILD_REMOVE for removed ones; the receiver's session maps dedupe
    // duplicates) and un-latches their STATE rows, then forces an immediate
    // resend pass over every change-gated channel cache by aging lastSendMs
    // on rows EVER SENT (each channel's own safety-resend condition fires on
    // its next sample). Rows never sent stay silent - the shared-save
    // baseline is not traffic. Edge-only caches (weaponCensus_, hostBody_)
    // are deliberately untouched: re-seeding would author phantom edges.
    // A thin wrapper over resyncPeer() below (kept as its own name/call site
    // since it fires on the generic connect edge, not the joiner-unicast
    // bootstrap's post-load edge).
    void onPeerConnected(NetLink& net, u32 ownerId);

    // Phase 10 Plan 02 (SAVE-04): the SAME resend pass as onPeerConnected
    // above, exposed as its own entry point so Plugin.cpp can re-run it a
    // SECOND time for a bootstrap-pushed joiner, once that joiner's
    // coordinated load truly completes (the positive PKT_LOAD_ACK's
    // LC_LOADED edge, Plan 01) - closing the two catch-up windows
    // 10-RESEARCH.md's channel table named: one-shot PLACE/EVT broadcasts
    // that queued at the joiner's title screen through a multi-minute wait
    // (risking BACKLOG OVERFLOW loss) and rows (research) committed between
    // the bake and the load. A second pass for the SAME peer is idempotent
    // by construction - PLACE/REMOVE replays dedupe receiver-side, and aging
    // lastSendMs to 1 just re-fires each channel's own safety resend a
    // little early; calling it twice in a row is never wrong, only
    // (harmlessly) redundant on every channel except the one that actually
    // had something new to converge. `ownerId` is the CALLER's own local id
    // (the packets' authorship tag), exactly like onPeerConnected - this is
    // NOT a per-target-peer filter (the caches hold no per-peer state to
    // scope by), so calling it broadcasts a resend to every connected peer,
    // survivors included; that is expected and harmless (the same broadcast
    // every existing connect edge already triggers).
    void resyncPeer(NetLink& net, u32 ownerId);

    // AFTER publishOwned (protocol 20, HOST only - the world-detection
    // authority): for every DRIVEN copy currently in stealth mode, read its
    // whoSeesMeSneaking map and stream a PKT_STEALTH snapshot back to the
    // sneaker's OWNER (~4 Hz while active, change-gated; one final empty
    // snapshot when the sneak ends / the map empties so stale arrows clear).
    void publishStealth(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 20): drain received detection snapshots; each
    // subject we OWN gets the entries replayed between the LOCAL pair via
    // notifyICanSeeYouSneaking, so the marker arrows render natively on the
    // owner's screen. Never applied to driven copies.
    void applyStealthFeedback(GameWorld* gw, Inbound& in);

    // BEFORE engine (consensus game-speed sync, runs on BOTH clients):
    //  * detect a LOCAL user speed click (current state != what WE last applied)
    //    and turn it into a request (pause = speed 0);
    //  * join: send PKT_SPEED_REQ (change-gated + 3 s safety resend) and apply
    //    any received PKT_SPEED_SET;
    //  * host: consume its own request locally, drain peer requests, arbitrate
    //    effective = min(requests) capped at 1x while either player squad is in
    //    combat (own flag from ownHands_+readCombat, peer flag from its REQ),
    //    apply locally and broadcast PKT_SPEED_SET on change (+ safety resend).
    void syncSpeed(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

    // BEFORE engine, AFTER syncSpeed (protocol 25 game-clock sync). Both sides
    // broadcast their absolute in-game clock at ~1 Hz, and both can correct -
    // in the one direction each has room for:
    //  * join: offset = hostHours - localHours, steer timeSlew_ UP to catch up
    //    (proportional, capped at 2x, with a disengage deadband);
    //  * host: lag = localHours - joinHours, steer timeSlew_ DOWN to be caught,
    //    but only while the join has no headroom of its own left (the clock
    //    brake - see syncTime; at the 5x ceiling the join's lever does nothing
    //    and this is the only one that works).
    // Either way the slew is LOCAL: the speed layer's quiet writes apply it on
    // top of the arbitrated consensus, which is what goes on the wire. The slew
    // composes with speed consensus rather than fighting its continuous
    // enforcement; when speedSync is off the channel degrades to offset logging
    // (no lever to compose with).
    void syncTime(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

    // Game-clock sync master enable (KENSHICOOP_TIME_SYNC).
    void setTimeSync(bool v) { timeSync_ = v; }

    // KENSHICOOP_TIME_BRAKE (default ON): the host half of the correction -
    // slowing its own sim while the join is behind and out of headroom. "0"
    // restores the join-only slew, which is join-catches-up or nothing.
    void setTimeBrake(bool v) { timeBrake_ = v; }

    // Session reset (protocol 32): called on the world-reload edge (gameplay
    // non-live -> live after a mid-session load). The old world is GONE - every
    // raw Character*/RootObject* cache is dangling and every session map
    // (builds, proxies, change-gate baselines, interp buffers) describes a
    // world that no longer exists. Clears all of it back to as-if-freshly-
    // launched state while PRESERVING: the config gates (set* levers), the
    // ownership partition (ownRanks_), and every OUTBOUND sequence counter
    // (facSeqOut_, doorSeqOut_, buildSeqOut_, bdoorSeqOut_, prodSeqOut_,
    // speedSeqOut_, timeSeqOut_, nextEventId_/netId/dropId/pickupId/treatId)
    // - a peer that
    // did NOT reload keeps its per-sender stale-row guards, so restarting a
    // counter would make every new row look stale to it.
    void resetSession();

    // Peer-leave cleanup (Phase 2 crash hardening; Phase 3 Plan 04 owner-scoped
    // rewrite, PEER-02/03): called ONCE PER DEPARTING id from the leave-loop
    // when a peer disconnects mid-session (distinct from a world-reload).
    // OWNER-SCOPED: destroys only `departing`'s minted proxy bodies (SEH-
    // guarded via destroyIfMinted; ADOPTED bodies are released, not
    // destroyed) and world-item proxies (worldProxies_ is keyed by
    // (ownerId, netId), so ownership needs no lookup), then erases only
    // `departing`'s targets_ entries (identified by the Driven.owner tag
    // Plan 02 set from ingest()'s ownerId). Every OTHER connected peer's and
    // the local player's replicated state is left untouched. Unlike the old
    // single-parameter version, this does NOT call resetSession() - that
    // global wipe stays reserved for the session-end / world-reload path
    // (sessionResetForUi's full-disconnect loop, sessionResetForWorldReload).
    // Safe if reconnect follows: the departing id's hands re-census and
    // re-mint from scratch on the next tick.
    void clearPeerReplicationState(GameWorld* gw, u32 departing);

    // AFTER engine: sample + apply the interpolated pose for every tracked entity.
    void applyTargets(GameWorld* gw);

    // AFTER engine: reconcile each peer-owned container we received a (dirty) snapshot
    // for to its desired contents. Skips containers we own (we author those).
    void applyInventories(GameWorld* gw);

    // AFTER applyTargets: make the region's AUTHOR authoritative for world NPCs.
    // Any nearby NPC the author is NOT streaming this tick is hidden + frozen so
    // our local AI can't run a divergent copy (the standing-on-a-host-seat
    // double / extra-NPC problem). Suppressed NPCs are restored if the author
    // later streams them.
    //
    // Without cellAuth_ the author is always the host, so this stays what it
    // was: a join-only pass judging everything against the host's census. With
    // it on, both sides run it and each skips the cells IT owns - judging your
    // own authored bodies against a peer that never claimed them is exactly the
    // "NPCs vanish as I walk up to them" report.
    void enforceHostAuthority(GameWorld* gw, u32 localId);

    // HOST, ~1 Hz (protocol 36): publish the wide-radius NPC existence census
    // (hands only) so the join can cull local-only ghosts far beyond the
    // positional stream bubble. No-op unless streamNpcs_ and censusRadius_ > 0.
    void publishNpcCensus(GameWorld* gw, NetLink& net, u32 ownerId);

    // JOIN (Task 3, WORLD-03: EVERY client now - PKT_NPC_CENSUS is relayed
    // join->join): drain received censuses into the per-owner census_ map
    // (map<ownerId, CensusSet>) consumed by enforceHostAuthority's
    // wide-radius pass. gw is for the per-row ownership check (see the
    // definition): a census row only speaks for a cell the SENDER owns,
    // judged against OUR map. A single drain can legitimately hold DISTINCT
    // owners' packets (N-1 publishers @ 1 Hz) - every owner's LATEST entry
    // this drain is applied, never just the deque's back().
    void applyNpcCensus(GameWorld* gw, Inbound& in);
    // Shared per-owner apply primitive applyNpcCensus calls once per
    // distinct owner in a drain. (InboundNpcCensus forward-declared via
    // Inbound.h; Key/CensusPos/CensusSet-typed helpers - censusHasAny/
    // censusPosFor/censusTotalCount - are declared further down, after
    // Key/CensusSet themselves are defined.)
    void applyOneNpcCensus(GameWorld* gw, const InboundNpcCensus& nc);

    // Camera hint channel (protocol 43, camera-anchored interest). SYMMETRIC:
    // each side reads its local camera center (engine::cameraCenter), publishes
    // it as a local anchor, ships it to the peer at ~1 Hz (PKT_CAM_HINT,
    // unreliable latest-wins), and drains the peer's into peerCam_/peerCamMs_
    // for engine::setPeerCamHint. Was join -> host only until the attention
    // gate: dormancy is a predicate both clients must agree on, so neither
    // camera can stay private.
    void syncCamHint(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId);

    // KENSHICOOP_CENSUS_RADIUS: wide-radius existence culling reach (units);
    // <= 0 disables the census channel on both sides.
    void setCensusRadius(float r) { censusRadius_ = r; }

    // KENSHICOOP_SPAWN_MINT_RADIUS (2026-07-11 "NPCs spawn on top of the join
    // player" fix): how far from our own squad a census-missing host NPC may
    // be PROXY-MINTED. The census-missing scan asks about every census hand
    // with no local body; the host's reply carries the authoritative position
    // and the mint happens only inside this radius (the duplicate guard: a
    // baked NPC in a loaded block always resolves, so a resolve-fail inside
    // render range is a genuine host runtime spawn). <= 0 restores the legacy
    // stream-bubble-only minting.
    void setSpawnMintRadius(float r) { spawnMintRadius_ = r; }

    // KENSHICOOP_ADOPT_RADIUS (zone-load population parity, 2026-08-06): how far
    // from a census row's position the join looks for one of its OWN bodies of the
    // same template+faction to BIND to that row, rather than minting a proxy next
    // to it. Zone-generated town population is the case: both engines create it
    // from the same spawn points when the zone streams in, with different engine
    // hands, so nothing resolves by hand and the pre-adoption code built a second
    // town on top of the first and hid the first. <= 0 restores that behaviour.
    void setAdoptRadius(float r) { adoptRadius_ = r; }

    // KENSHICOOP_CENSUS_PARK (v38 pack-hidden fix): how far a census-PRESENT
    // local copy may drift from the host's census position before the join
    // parks it back onto the host's spot. <= 0 disables parking (existence-
    // only census, the v37 behavior).
    void setCensusParkDist(float d) { censusParkDist_ = d; }

    // KENSHICOOP_CENSUS_WALK: the ceiling of the band in which a diverged
    // census-band body is WALKED back onto the host's position instead of
    // teleported there. <= 0 disables the walk and restores teleport-only
    // correction at every distance.
    void setCensusWalkDist(float d) { censusWalkDist_ = d; }

    // KENSHICOOP_CENSUS_FREEZE_AI (default ON): the join freezes the local AI
    // of a census-band body (census-present, unstreamed) that DIVERGES past
    // censusParkDist_ - the position park teleports it back, but its local AI
    // kept re-deciding to flee/fight, so a captive/working slave ran and
    // aggroed the join's guards while the host had it working. Divergence-
    // gated: well-tracking census NPCs never trip it and keep their local AI.
    void setCensusFreezeAi(bool v) { censusFreezeAi_ = v; }

    // KENSHICOOP_ATTENTION_RADIUS (attention-gated reconciliation): how close
    // an interest anchor must be to a body before the two worlds are
    // reconciled there. <= 0 disables the gate - everything counts as
    // observed, which is exactly the pre-gate behaviour.
    void setAttentionRadius(float r) { attentionRadius_ = r; }

    // travel_parity worldstate rows: when enabled, both sides dump one
    // "SCENARIO WNPC hand=.. pos=.. cls=.. name=.." row per enumerated world
    // NPC every ~5 s - the host from its census walk (cls=host), the join
    // from the existence audit with each NPC's authority class (drv/cen/hid/
    // ghost) - so Test-TravelParity can cross-compare the two worlds while
    // the players travel. Off by default (log volume).
    void setAuditRows(bool on) { auditRows_ = on; }

    // Jail put-to-work desync spike (KENSHICOOP_JAIL_PROBE, read-only): emit
    // correlated [jail] STATE traces for captive bodies - the owned PC in
    // publishOwned (side=own) and each driven copy in applyTargets (side=drv)
    // - so the brief cage-exit/re-cage twitch can be pinned. Off by default.
    void setJailProbe(bool on) { jailProbe_ = on; }

    // Phase A jail-observe (KENSHICOOP_JAIL_OBSERVE, read-only): on the host,
    // stop driving/suspending/self-healing a peer-owned captive so the host's
    // local sim runs it unopposed, and log its trajectory ([jail] OBSERVE) to
    // classify the guard's "put to work" intent (relocate vs walk-round). Off
    // by default; a spike lever, never shipped on.
    void setJailObserve(bool on) { jailObserve_ = on; }

    unsigned int targetCount() const { return (unsigned int)targets_.size(); }

    // Stage 2 smoothness oracle: emit a "SCENARIO SMOOTH ..." summary describing
    // how often the driven body moved per frame while its source was in motion.
    // Per-frame interpolation keeps zeroFrac low; raw 20 Hz stepping makes it high.
    void logSmoothSummary();

private:
    struct Key {
        u32 t, c, cs, i, s;
        bool operator<(const Key& o) const {
            if (t != o.t) return t < o.t;
            if (c != o.c) return c < o.c;
            if (cs != o.cs) return cs < o.cs;
            if (i != o.i) return i < o.i;
            return s < o.s;
        }
    };
    static Key keyOf(const EntityState& e) {
        Key k; k.t = e.hType; k.c = e.hContainer; k.cs = e.hContainerSerial;
        k.i = e.hIndex; k.s = e.hSerial; return k;
    }

    // Same predicate as the public isMine(), for call sites (applyTargets'
    // drive loop, keyed on targets_) that already hold a Key - avoids
    // marshaling back through the unsigned int[5] hand array only to
    // immediately reconstruct the Key isMine() would build from it.
    bool isMineKey(const Key& k) const { return ownHands_.find(k) != ownHands_.end(); }

    struct Driven {
        EntityInterp interp;
        bool         fresh;          // host streamed a non-stale sample this tick
        bool         haveActual;     // lx/ly/lz hold a valid previous actual pos
        float        lx, ly, lz;     // last actual (rendered) position
        bool         parked;         // settled at rest (avoid re-halting the clip)
        bool         haveDest;       // dx/dy/dz hold a previously issued destination
        float        dx, dy, dz;     // last issued walk destination
        bool         walkHalted;     // the outstanding order was cancelled on arrival (one-shot,
                                     //   re-armed by the next real walkTo) - see the walk branch
        unsigned int walkStallF;     // consecutive frames the ordered body advanced ~nothing
        bool         suppressed;     // NPC pulled off the local AI update list
        unsigned long lastSeenMs;    // last ingest for this hand (stale-entry pruning)
        u32          owner;          // Phase 3 (OWN-02): PlayerId whose stream last
                                     //   drove this entry (OWNER_NONE until the first
                                     //   ingest). Lets a client tell distinct remote
                                     //   squads apart while interpolating multiple
                                     //   owners simultaneously (POC-01).
        // Stage 5 rest-pose reproduction.
        u16          issuedTask;     // task we last committed (TASK_NONE = none)
        bool         taskApplied;    // a fixture-resolved pose is currently held
        bool         taskBad;        // task not reproducible here (fixture missing/drift)
        unsigned long taskTick;      // when the task was issued (drift grace timer)
        unsigned int taskRetries;    // far-fixture (r=3) attempts (interp-lag retry)
        bool         fixtureXlateLogged; // one [pose] fixture-xlate line per body
        unsigned long taskNoneTick;  // first tick of the current sustained host->NONE
                                     //   streak (0 = not in one); after TASK_CLEAR_MS
                                     //   of continuous NONE the held pose is released
                                     //   (debounced job-removal detector - mirrors
                                     //   carryNoSeeTick/furnNoSeeTick)
        bool         detached;       // I9: detached from town-AI (separateIntoMyOwnSquad) once
        bool         downApplied;     // Stage 2: body is currently held in ragdoll (host says down)
        bool         koLatched;       // a reliable EVT_KNOCKOUT pinned this body down
        bool         deathLatched;    // a reliable EVT_DEATH pinned this body down PERMANENTLY
        bool         carriedDown;     // a KO'd passenger, remembered ACROSS the drop. Leaving a
                                      //   shoulder is a physical transient: the body detaches and
                                      //   falls for a few frames before the engine re-flags it
                                      //   down, and the streamed sample can be mid-transient too,
                                      //   so neither side reads down on the release tick. This
                                      //   bridges that hole - see the carried carve-out.
        bool         combatArmed;     // Stage 3c: a melee-attack order is currently issued
        unsigned long combatTick;     // when the attack order was (re-)issued (re-arm throttle)
        unsigned int combatOrders;    // orders issued this combat episode (re-issue backoff)
        u32          combatTgtIdx;    // target hand (index/serial) of the last issued order,
        u32          combatTgtSer;    //   so a host-side retarget re-issues immediately
        unsigned long combatSeenTick; // last tick the host stream reported combat (the
                                      //   disarm DEBOUNCE: a 1-batch gap must not reset the AI)
        unsigned long combatSnapTick; // last hard snap (cooldown; a failed teleport must not
                                      //   re-fire every frame - walk-converge between snaps)
        unsigned long combatSnapCount;// cumulative combat hard-snaps on this hand (Phase 1 warp
                                      //   diagnosis): a body that keeps snapping is PERSISTENTLY
                                      //   divergent (wrong target / wrong place), not occasionally
                                      //   churning - surfaced as n= in the [combat] snap line and
                                      //   as maxPersist in the [combat] stats rollup
        unsigned long combatOverTick; // Phase 1 hysteresis (2026-07-16 smoothness pass): first
                                      //   tick the drift went OVER the snap band this spell
                                      //   (0 = under). A copy only INSTANT-teleports after the
                                      //   drift PERSISTS COMBAT_CONVERGE_MS on a moving source;
                                      //   a momentary interp/footwork spike fast-slides back
                                      //   instead of warping
        unsigned long npcSnapTick;    // locomotion-path hard snap cooldown (Phase 2): a far
                                      //   NPC whose teleport can't stick (seat AI, unloaded
                                      //   terrain) walk-converges between snaps instead of
                                      //   teleporting every frame
        bool         goalsCleared;    // rest-entry AI-goal clear done (once per rest episode;
                                      // re-cleared only after the body genuinely moves again)
        // Step 4 divergence-gated authority (world NPCs, behind gateAuthority_):
        bool         trusted;         // local AI agreed long enough -> not driven/suspended
        unsigned int agreeStreak;     // consecutive agreeing+in-position samples
        // Carried-body sync (protocol 18):
        unsigned long carryHealTick;  // last self-heal pickup attempt (throttle)
        unsigned long carryNoSeeTick; // first tick a locally-carrying copy's stream
                                      //   stopped reporting TASK_CARRY_BODY (the
                                      //   debounced host-side-drop detector)
        // Furniture occupancy sync (protocol 19):
        unsigned long furnHealTick;   // last self-heal enter attempt (throttle)
        unsigned long furnNoSeeTick;  // first tick a locally-occupying copy's stream
                                      //   stopped reporting the occupancy bit (the
                                      //   debounced owner-side-exit detector)
        // Third-party placement (protocol 36): last time the host authored a
        // PEER-ENTER for this peer-owned driven body (re-author throttle).
        unsigned long furnPeerTick;
        // Chained/pole prisoner (protocol 41): the OWNER hand last seen for this
        // body while it was locally chained, so a lost/late reliable ENTER (or
        // an AI break-out) can be self-healed by re-applying setChainedMode -
        // the continuous BODY_CHAINED bit carries no owner (unlike a cage, whose
        // building the self-heal re-finds by name near the streamed position).
        unsigned int  chainOwner[5];
        bool          haveChainOwner;
        // Phase 6b (protocol 42): last non-owner unlock-guard re-assert attempt
        // (throttle). A caged prisoner (streamKind=2) is also shackled, but the
        // furniture kind priority hides the chained state from the occupancy
        // self-heal; this guard re-asserts setChainedMode independently so a peer
        // PC's local lockpick can't leave the owner's prisoner unlocked on the peer.
        unsigned long chainHealTick;
        // Stealth sync (protocol 20):
        unsigned long sneakTick;      // last setStealthMode apply (mode-flap throttle)
        // Prone posture sync (protocol 53):
        unsigned long proneTick;      // last setProneState apply (posture-flap throttle)
        bool          crawlDrive;     // in the kinematic crawl regime (regime-edge log)
        // Velocity-aware snap gate (2026-07-11): slow-decaying peak of the
        // source's wall-clock speed. The instantaneous 2-sample estimate
        // collapses to ~0 the moment the source halts, deflating the snap
        // gate while the driven body is still converging on the stop point.
        float        velPeak;
        // Walk/rest debounce (2026-07-11 choppiness fix): last tick a sample
        // showed genuine translation; the walking verdict holds for a fixed
        // time after it so mid-walk velocity dips can't flap the classifier.
        unsigned long moveSeenMs;
        // The classifier's previous verdict, so genuine walk->rest
        // transitions can be counted (restFlipNpc_) as a flap-rate signal.
        bool         wasMoving;
        // March attribution: when this body last ENTERED the rest branch, and
        // which branch it took last frame (so the entry edge can be detected).
        unsigned long restEnterMs;
        bool         walkBranchPrev;
        // Per-hand smoothness attribution (Phase 2 diagnosis): cumulative
        // active/zero-step frames this body contributed to the oracle, so a
        // zeroFrac regression can name its worst offenders in the stat line.
        unsigned long zeroF;
        unsigned long activeF;
        unsigned long advMs;   // last tick this body actually advanced (zeroFrac audit)
        // Motion-onset attribution (2026-08-10). The oracle's "source moving"
        // test is the INSTANTANEOUS stream velocity, so a body whose velocity
        // hovers near NPC_MOVE_VEL enters and leaves the scored population
        // repeatedly, and every re-entry is a fresh onset the join's render has
        // not caught up to yet. These two fields edge-detect that so the zero
        // frames can be bucketed by age-since-onset and the hypothesis tested.
        // Reported only - nothing here changes what is scored.
        bool          oracleActivePrev;
        unsigned long oracleOnsetMs;
        int          prevInterpMode; // last tick's EntityInterp::lastMode(). lastMode is a
                                     //   STICKY classification of the newest sample, not an
                                     //   event: it keeps reading SM_SEG_SNAP for every frame
                                     //   after a source teleport until a new sample lands.
                                     //   Anything that must happen ONCE per teleport has to
                                     //   edge-detect against this (a cooldown-exempt combat
                                     //   snap keyed on the level fired 67x at drift 0.0).
        // Last tick this body's stream cadence classified MID-tier. A body
        // that just handed off mid -> near (raid walking into the 20 Hz
        // bubble) may owe one large reconciliation snap for divergence
        // accrued under sparse mid coverage - classed to the mid ledger
        // (like young-ring coverage snaps), not steady-state near tracking.
        unsigned long midSeenMs;
        Driven() : fresh(false), haveActual(false), lx(0), ly(0), lz(0), parked(false),
                   haveDest(false), dx(0), dy(0), dz(0), walkHalted(false), walkStallF(0),
                   suppressed(false), lastSeenMs(0), owner(OWNER_NONE),
                   issuedTask(TASK_NONE), taskApplied(false), taskBad(false),
                   taskTick(0), taskRetries(0), fixtureXlateLogged(false),
                   taskNoneTick(0), detached(false), downApplied(false),
                   koLatched(false), deathLatched(false), carriedDown(false),
                   combatArmed(false), combatTick(0), combatOrders(0),
                   combatTgtIdx(0), combatTgtSer(0),
                   combatSeenTick(0), combatSnapTick(0), combatSnapCount(0),
                   combatOverTick(0), npcSnapTick(0),
                   goalsCleared(false),
                   trusted(false), agreeStreak(0),
                   carryHealTick(0), carryNoSeeTick(0),
                   furnHealTick(0), furnNoSeeTick(0), furnPeerTick(0),
                   haveChainOwner(false), chainHealTick(0),
                   sneakTick(0), proneTick(0), crawlDrive(false),
                   velPeak(0.0f), moveSeenMs(0), wasMoving(false),
                   restEnterMs(0), walkBranchPrev(false),
                   zeroF(0), activeF(0), advMs(0),
                   oracleActivePrev(false), oracleOnsetMs(0),
                   prevInterpMode(-1), midSeenMs(0) {
            chainOwner[0] = chainOwner[1] = chainOwner[2] = chainOwner[3] = chainOwner[4] = 0;
        }
    };

    // Reproduce the host's rest pose on a driven body: if it carries a task whose
    // fixture resolves locally, commit it ONCE (seated/idle at the same object);
    // otherwise quiet the AI and park at the host transform. Drift-guarded; re-arms
    // when the host task changes. Used by both the NPC and squad at-rest branches.
    // isSquad: player-squad members are NEVER town-AI-detached (the detach
    // re-containers the body OUT of its squad tab - a new hand - which breaks
    // every hand-keyed protocol on a squad member; observed on bed_pose).
    void applyRest(Character* c, Driven& d, const EntityState& out,
                   bool haveActual, float ax, float ay, float az, unsigned long now,
                   bool isSquad);

    // Drive-tick epilogue (Phase 7 Workstream C): the self-contained post-loop
    // passes split out of applyTargets. Each reads/writes only Replicator members
    // and the tick clock (`now`) - never any per-body loop local - so the split is
    // behavior/byte identical. Called in order at the tail of applyTargets.
    //   * prune the recently-driven grace map (drivenSeen_/canonicalOf_);
    void pruneDriveGrace(unsigned long now);
    //   * emit the ~3-5 s throttled telemetry rollups
    //     ([ai]/[interp]/worstZero/[combat]/[trust] - the oracle stat lines);
    void logDriveTelemetry(unsigned long now);
    //   * age out long-stale targets_ entries (reliable-event latches preserved).
    void ageOutStaleTargets(unsigned long now);

    std::map<Key, Driven> targets_;
    // Host side: last bodyState we published per owned entity (+ when it was last
    // captured, so long-gone entities age out), so we can emit a reliable event on
    // the rising/falling edge (KO/death/revive) exactly once.
    struct HostBody {
        u16 bs; unsigned long seenMs;
        // Carried-body sync (protocol 18): last published carry relationship of
        // an OWNED member, so publishOwned can emit the reliable pickup/drop
        // edge exactly once per transition. carried = the carried body's hand
        // (readObjectHand layout), meaningful only while carrying.
        bool carrying; unsigned int carried[5];
        // Furniture occupancy (protocol 19): last published occupancy of this
        // entity (0 none / 1 bed / 2 cage) + the furniture's hand (captured
        // from the local Character on the ENTER edge), so publishOwned can
        // emit the reliable enter/exit edge exactly once per transition and
        // still name the furniture on EXIT.
        int furnKind; unsigned int furn[5];
        HostBody() : bs(0), seenMs(0), carrying(false), furnKind(0) {
            carried[0] = carried[1] = carried[2] = carried[3] = carried[4] = 0;
            furn[0] = furn[1] = furn[2] = furn[3] = furn[4] = 0;
        }
    };
    std::map<Key, HostBody> hostBody_;
    // Host side: who is attacking whom (victim key -> {attacker key, lastSeenMs}), built
    // from the combat intents in the captured set (task==TASK_COMBAT_MELEE, subject =
    // the target). STICKY with a recency window: the attacker drops its target the instant
    // the victim falls, so the down/death edge would otherwise see no current attacker -
    // we keep the last attacker for ATTR_WINDOW_MS and stamp it as the event's ACTOR
    // (causality: "downed BY"), so combat KO/death events carry who-did-it.
    std::map<Key, std::pair<Key, unsigned long> > attackerOf_;
    // Capture-side combat telemetry: last "[combat] CAP" emit per OWNED hand
    // (throttle). Records what this client SENDS on the wire - the missing
    // half of the audit trail for join-initiated fights.
    std::map<Key, unsigned long> combatCapMs_;
    // Same throttle for the "[build] POSE-CAP xlate" line, keyed by wire site key.
    std::map<Key, unsigned long> buildPoseCapMs_;
    u32                   nextEventId_;
    // Phase 5 (ID-02): (ownerId, eventId) dedup guard applied at the top of
    // applyEvents, ahead of the koLatched/deathLatched per-subject-hand latch
    // switch. Bounded, additive defense-in-depth implementing the CONTEXT
    // "receivers dedup on (ownerId, eventId)" decision - mirrors the
    // appliedDrops_/appliedPickups_/appliedXfers_ composite-key idiom
    // (foldOnce, FoldDedup.h) so a reliable resend/replay of the same
    // sender's eventId is skipped before any latch mutation, while three
    // different owners' eventId=1 still fold as three distinct events.
    std::set<std::pair<u32, u32> > appliedEvents_;
    // Stage 6: world NPCs we've hidden+frozen on the join because the host isn't
    // streaming them. Keyed by hand so we restore the exact body when it re-enters
    // the host's streamed set.
    std::map<Key, Character*> suppressed_;
    // Phase 2 (runtime-spawn sync): last time the suppressed set was re-hidden.
    // The engine can undo a one-shot hide on its own (ambush dialog/combat
    // re-tasks the body, zone streaming re-adds it to the update list), so the
    // hide is re-asserted on a ~2 s cadence while the hand stays unstreamed.
    // Lifetime guard (2026-07-11 join crash, dump: SensoryData::periodicUpdate
    // walking a freed body during zone streaming with 93 suppressed wildlife
    // booked): the ENGINE owns every suppressed body and is free to despawn it
    // at any time. The re-assert pass re-resolves each entry's hand FIRST; a
    // null/different resolution means the stored Character* is stale, and the
    // entry (plus its marker/counters) is pruned without touching the pointer.
    unsigned long authReassertMs_;
    unsigned long authPruned_;      // stale suppressed entries dropped (despawned bodies)
    // Bodies applyTargets actually drove this tick, by POINTER: a combat-driven
    // world NPC is detached into its own squad (container changes), so its local
    // enumeration key no longer matches the host's streamed key - the hand-keyed
    // `keep` set can't recognise it and enforceHostAuthority would hide+freeze a
    // body mid-fight. Pointer identity survives the re-containering.
    std::set<Character*>      drivenChars_;
    // Recently-driven grace (Phase 2 mid-band tier): last tick each body was
    // driven. drivenChars_ is per-tick, but a mid-tier body's samples arrive
    // on the round-robin cadence and a rotation hiccup empties its interp for
    // a beat - the wide authority pass must not judge it as unstreamed during
    // such a gap (run 20260712_103044: driven bodies culled mid-drive, then
    // restored by the reassert pass's driven exemption, 6 churn cycles per
    // hand). Pointers are compared, never dereferenced; timestamp-pruned.
    std::map<Character*, unsigned long> drivenSeen_;
    // Reverse identity map for CAPTURE translation (join-initiated town combat,
    // run 20260712_180913): body pointer -> the canonical key the PEER streams
    // it under. A driven body's LOCAL hand can diverge from its wire identity
    // (the engine separateIntoMyOwnSquad's a town NPC into a fresh local
    // container when a fight starts; minted proxies never had the peer's hand
    // at all), so a captured combat intent's SUBJECT must be published under
    // the canonical key or the peer's applyCombat can't resolve the target
    // (r=1 forever - the fight only renders on the attacker's client).
    // Refilled every drive tick; pruned alongside drivenSeen_.
    std::map<Character*, Key> canonicalOf_;
    // Last LOCAL hand reported for each wire key, so the [rekey] translation
    // line is emitted on change rather than every drive tick.
    std::map<Key, Key>        rekeyLogged_;
    // Step-5 hysteresis: consecutive-frame counters per hand so a brief stream
    // hiccup doesn't suppress (needs ~1 s unstreamed) and a boundary NPC doesn't
    // flicker back (needs ~2 s streamed dwell to restore). Spike 18: the hard
    // interest edge had no dwell band, so boundary patrollers churned.
    struct AuthCount { unsigned int unstreamed; unsigned int streamed;
                       AuthCount() : unstreamed(0), streamed(0) {} };
    std::map<Key, AuthCount>  authCount_;
    unsigned long             authSuppresses_; // churn counters (split_interest metric)
    unsigned long             authRestores_;
    // NPC existence census (protocol 36): the host's 1 Hz wide-radius hand
    // list. On the join, censusHands_ is the latest received existence set and
    // censusRecvMs_ its arrival time - enforceHostAuthority culls local NPCs
    // OUTSIDE the stream bubble that are absent from it (with the same
    // hysteresis), but only while the census is FRESH (staleness disables
    // wide culling rather than mass-suppressing on a silent host).
    float                     censusRadius_;  // 0 = census disabled
    unsigned long             censusSendMs_;  // host: last census publish
    // AGGREGATE arrival tracker: the last time we received ANY owner's
    // census (any sender). Drives the "is any census fresh enough to
    // trust" gates (censusFresh, the wide-radius pass enable, staleness
    // diagnostics) - unchanged shape from before the Task 3 per-owner
    // refactor. PER-OWNER freshness lives in each CensusSet.recvMs below.
    unsigned long             censusRecvMs_;  // join: last census arrival (any owner)
    // Camera hint channel (protocol 43): join sends its camera center at
    // ~1 Hz; the host keeps the latest hint + arrival stamp (stale hints are
    // dropped from the anchor set rather than pinning interest forever).
    unsigned long             camHintSendMs_; // join: last hint send
    float                     peerCam_[3];    // host: latest peer camera center
    unsigned long             peerCamMs_;     // host: its arrival time (0 = none)
    unsigned long             censusCulls_;   // join: wide-radius suppress count
    // Rows dropped because OUR map does not put the sender's cell in the
    // sender's hands. A steady nonzero here is the two maps disagreeing, which
    // is the condition the 2026-08-08 dual drive needed.
    unsigned long             censusOffCell_;
    // Echo yields: world NPCs the join could have authored but left to the host
    // because the host was already streaming that hand. See the echo guard in
    // publishOwned. Non-zero means the join stopped feeding a body's own state
    // back to the client that owns it.
    unsigned long             cellYields_;
    std::set<Key>             cellYieldLogged_;   // one [cell] yield line per hand
    // Our own owner id, latched by publishOwned (which is handed it every frame
    // and runs before applyTargets in the tick). The drive path needs to know
    // whether it is the host to apply the host half of the tie-break, and has no
    // NetLink of its own to ask. 0xFFFFFFFF until the first publish, which keeps
    // the guard inert rather than letting a join mistake itself for the host.
    u32                       localId_;
    // Peer samples the host declined because the body stands in a cell the host
    // authors - the host half of the tie-break. Pairs with cellYields_, which
    // counts the join half.
    unsigned long             hostDriveRefusals_;
    // How long a peer sample keeps a hand "theirs" for the echo guard. Two net
    // ticks (50 ms each) plus slack: long enough that a single dropped snapshot
    // does not hand the body back and forth, short enough that a real handover
    // - the peer walking out of the cell and falling silent - completes within
    // a beat instead of leaving the body unwritten.
    enum { PEER_STREAM_FRESH_MS = 1000 };
    // Phase 0.5 census diagnostics (2026-08-02 field report: "join sees local
    // NPCs the host does not, worsening over long travel"). Four mechanisms in
    // this file can produce that symptom and the logs could not tell them
    // apart: the enumerations truncate silently at their caps, the wide pass
    // switches OFF entirely on a stale census, and anything past censusRadius_
    // is never enumerated so it cannot even be counted as a ghost. These feed
    // the appended fields on the 5 s [audit] exist line.
    bool                      censusPubTrunc_;   // host: last publish hit NPC_CENSUS_MAX
    bool                      censusFreshPrev_;  // join: freshness at the last sample
    unsigned long             censusFreshChkMs_; // join: when we last sampled it
    unsigned long             censusStaleMs_;    // join: cumulative stale time
    unsigned long             censusStaleEdges_; // join: fresh -> stale transitions
    unsigned long             proxyDriftLogMs_;  // join: last [proxy] drift sample sweep
    // Phase 2 mid-band streaming tier (HOST): census-walk NPCs OUTSIDE the
    // ~200/260 u stream bubble, nearest-first, refreshed at the 1 Hz census
    // cadence. publishOwned round-robins a small slice of them through the
    // entity batch every frame (quota sized so each NPC hits the 20 Hz wire
    // at ~MID_HZ aggregate) - between census beats a far NPC keeps receiving
    // real positions instead of freezing under divergent local AI (the
    // "zombie NPC" report). Keys only (no Character*): each publish resolves
    // the hand fresh, so a despawn between census walks degrades to a skip.
    struct MidBandEntry {
        Key k; float dist;
        bool operator<(const MidBandEntry& o) const { return dist < o.dist; }
    };
    std::vector<MidBandEntry> midBand_;
    unsigned int              midCursor_;  // start of the CURRENT slice
    unsigned long             midSliceMs_; // last slice advance (50 ms cadence:
                                           // the slice must persist across a
                                           // whole net tick to be sampled)
    unsigned int              midFastPromoted_; // mid bodies streamed at the full
                                           // near-band rate last tick because they
                                           // were RUNNING (see the promotion pass)
    // v38 census position parking (pack-hidden investigation, 2026-07-11):
    // the host position per census row. A census-PRESENT NPC is exempt from
    // culling, but its two locally-simulated copies can wander arbitrarily
    // far apart - the join saw a pack somewhere the host's copy wasn't.
    // enforceHostAuthority parks a local copy that diverged past
    // censusParkDist_ back onto the host's spot (0 disables).
    struct CensusPos { float x, y, z; };
    // Per-owner census (Task 3, WORLD-03): PKT_NPC_CENSUS moves from Class B
    // (host-only) to Class A (relayed join->join), so the intake must
    // represent MULTIPLE simultaneous authors - the single censusHands_/
    // censusOwner_/censusPos_/censusPrev_/censusPrevMs_ set this replaces
    // WIPED every other author's existence claims the instant a second
    // owner's 1 Hz census arrived (ReplicatorAuthority.cpp's old
    // applyNpcCensus, :84-92 pre-Task-3). Keyed by ownerId; each owner's
    // slice is a drop-in replacement for the old bare members (hands/pos/
    // prev/prevMs fields keep the exact same per-row shape and semantics -
    // only the outer map adds the owner dimension).
    struct CensusSet {
        std::set<Key>            hands;  // that owner's latest existence set
        std::map<Key, CensusPos> pos;    // that owner: host pos per census row
        std::map<Key, CensusPos> prev;   // that owner's PREVIOUS packet's rows
        unsigned long             recvMs;  // that owner: last census arrival
        unsigned long             prevMs;  // that owner: previous arrival stamp
        CensusSet() : recvMs(0), prevMs(0) {}
    };
    std::map<u32, CensusSet>  census_;
    // Phase 8 review WR-02: the per-owner staleness horizon the union
    // helpers below apply per CensusSet. Pre-Task-3 the single set was
    // WIPED by whichever packet arrived next, so a stale row lived ~1 s;
    // post-refactor each owner's slice persists until that owner's next
    // packet or its disconnect - so a connected-but-silent owner (mid
    // coordinated load, long pause) would keep vouching existence and
    // positions for bodies it no longer speaks for, indefinitely, while the
    // AGGREGATE censusRecvMs_ stayed fresh off everyone else's arrivals.
    // Value matches the aggregate censusFresh threshold (5000 ms) exactly:
    // with a single publishing peer (the 2-player case) the per-owner stamp
    // IS the aggregate stamp, so behavior there is bit-identical.
    enum { CENSUS_OWNER_STALE_MS = 5000 };
    // Existence-membership union across every owner's CensusSet.hands - the
    // per-owner replacement for the old bare censusHands_.find(k)!=end()
    // test. A key can legitimately appear in more than one owner's set only
    // transiently (a body mid-handover); any owner vouching for it is enough
    // to say it exists. WR-02: an owner whose recvMs is older than
    // CENSUS_OWNER_STALE_MS no longer vouches (its slice is skipped).
    bool censusHasAny(const Key& k) const;
    // First matching owner's CensusPos for `k` (existence rows normally
    // belong to exactly one owner's cell) - the per-owner replacement for
    // censusPos_.find(k). Returns false if no owner's census carries `k`.
    // WR-02: a stale owner's slice is skipped, same as censusHasAny.
    bool censusPosFor(const Key& k, CensusPos* out) const;
    // Sum of every owner's CensusSet.hands size - the per-owner replacement
    // for the old bare censusHands_.size() in diagnostic log lines only.
    unsigned int censusTotalCount() const;
    std::map<Key, CensusPos>  proxyDriftPrev_; // join: census pos at the last [proxy]
                                              //   drift sweep (owner-motion estimate)
    float                     censusParkDist_;
    unsigned long             censusParks_;   // join: divergence-park count
    std::map<Key, unsigned long> parkMs_;     // join: per-key park cooldown
    // Per-key state for the two ways a census row can correct its local body:
    // the walk that converges it, and the teleport of last resort.
    //
    // px/pz/fails are the futile-park detector. engine::park reports that it
    // WROTE the transform, not that the body stayed there, so a correction the
    // engine undoes (a height the local collision re-grounds, a fixture that
    // re-anchors) is indistinguishable from one that worked until the next
    // tick's position is read back - and a body standing exactly where we last
    // teleported it did not get there by walking.
    //
    // dx/dz/haveDest throttle the walk order (re-issuing a destination every
    // frame restarts the path and renders as stutter), and walkMs tells the
    // divergence freeze that this body is under a walk order it must not halt.
    struct CensusFix {
        CensusFix() : px(0), pz(0), fails(0), haveDest(false),
                      dx(0), dz(0), walkMs(0) {}
        float         px, pz;
        unsigned int  fails;
        bool          haveDest;
        float         dx, dz;
        unsigned long walkMs;
    };
    std::map<Key, CensusFix>  censusFix_;
    float                     censusWalkDist_; // walk-converge band ceiling
    unsigned long             censusWalks_;    // join: walk-converge orders issued
    // Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI): join-side flag +
    // per-key last-diverge tick. A census-band body that drifts past
    // censusParkDist_ is added here and AI-suspended; the hold (~5 s) keeps it
    // quiesced after the park zeroes its drift, then re-checks (release if it
    // settled, re-freeze if it diverges again).
    bool                      censusFreezeAi_;
    std::map<Key, unsigned long> censusFrozen_; // join: per-key freeze-hold tick
    // Attention gate (KENSHICOOP_ATTENTION_RADIUS). Reconciliation follows
    // attention: a body no interest anchor is within attentionRadius_ of is
    // DORMANT, and neither side speaks for it - the host omits it from the
    // census, the join never suppresses it for being census-absent. Anchors
    // are the same four interestCenters both clients compute (both tab
    // leaders, own camera, peer camera hint), so the two sides evaluate the
    // predicate on near-identical inputs. attnObs_ carries the per-hand
    // hysteresis bit; both roles use it, pruned against each tick's
    // enumeration.
    float                     attentionRadius_; // 0 = gate off (all observed)
    std::map<Key, bool>       attnObs_;         // per-hand observed latch
    // The publish-side latch, for the same predicate asked about the PEER.
    // Separate from attnObs_ because two predicates over one latch would fight
    // each other's hysteresis every tick.
    std::map<Key, bool>       attnObsPeer_;

    // --- Presence authority (protocol 49) --------------------------------
    // WHO authors a region, as opposed to WHEN it is reconciled. Attention is
    // distance-shaped because it models what a player can see; authority is
    // CELL-shaped because it needs an exhaustive, mutually exclusive partition.
    // Two owners means two clients drive the same body and zero owners means
    // nobody authors it, and "whoever is closest wins" compares two continuous
    // values derived from drifting inputs (interp error, tick skew, a camera
    // hint up to 3 s old) - near the midpoint that comparison flips
    // independently on each side. A discrete label has no such window, and a
    // published claim needs a small enumerable token to key a map on anyway.
    bool cellAuth_;   // off => authorityFor is always the host (fail-open)
    // Co-location collapse KNOB (KENSHICOOP_CELL_COLLAPSE). Task 2 (protocol
    // 59, WORLD-03) retires the independent claimsCoLocated() authority path
    // this used to gate - the per-cell reduce (CellMap.h) already resolves a
    // co-located cell to host via its host-if-party fresh-contest rule, so
    // collapse is structural now, not a second boolean that could disagree
    // with the map. cellCollapse_ is READ but no longer consumed by any
    // authority decision; kept only so the env knob still parses (Plan 03's
    // gate pins KENSHICOOP_CELL_COLLAPSE=0, which is now a no-op either way).
    bool cellCollapse_;
    // DERIVED diagnostic only (Task 2): true when every claimedCells_ entry
    // resolved to CELL_OWNER_HOST, recomputed by computeAndBroadcastCellMap
    // (host) / applyCellMap (client) alongside the map itself - never a
    // separately-evaluated verdict that could disagree with it. Feeds only
    // the [cell] MAP dump's collapse= field and the [census] auth dump's
    // AUTHSRC_COLLAPSE bucket (now permanently 0 - see authoritySrc).
    bool collapsed_;
    struct CellClaim {
        int          cx, cz;
        u32          seq;
        unsigned long recvMs;
    };
    // (ownerId, tabRank) -> that slot's current claim. A tab walking into a new
    // cell supersedes its own claim, which is why there is no release message.
    std::map<std::pair<u32, u32>, CellClaim> claimSlots_;
    // Derived from claimSlots_ every time one changes: cell -> owning player.
    // Host wins a contested cell (two tabs in one cell is the players standing
    // together, where host-wins is the right answer anyway).
    std::map<std::pair<int, int>, u32> claimedCells_;
    // Cells that have EVER been claimed, and by whom last. A claim is a tab's
    // current position, so a tab walking out of a town drops that cell out of
    // claimedCells_ entirely - and an unclaimed cell fail-opens to the host.
    // That turns "the join walked out of its own town" into "the host abruptly
    // authors that town", over bodies the host had spent the last half-minute
    // driving from the join's stream. Measured 2026-08-03: the join's tab left
    // cell 21,32, the map went {21,31=join, 21,33=host} with 21,32 unowned, and
    // the host faulted 241 ms later. Authorship follows PRESENCE, and the last
    // client present is still the better author of a region neither is standing
    // in, so a vacated cell keeps its owner until somebody else claims it.
    // Fail-open to host survives for cells nobody has ever been to.
    //
    // This is LOCAL HISTORY, which is a real hazard for a value both sides must
    // agree on: our own claim lands in claimSlots_ the instant we send it while
    // the peer's arrives a round trip later, so a tab that enters a cell and
    // leaves inside that window leaves the two maps permanently disagreeing.
    // Replacing it with a neighbourhood radius over the live claim map was tried
    // on 2026-08-08 (run 134911) and is much worse: derived ownership reaches
    // every cell near a claim, so the host went from authoring 9 unclaimed rows
    // a tick to 37, the join went to zero, and the boundary between the two
    // territories then MOVED with every claim - dual_drive went from 0 shared
    // hands to 14, with one body held by both sides for 51 s. A stale entry is
    // one cell wrong; a moving midline is wrong across the whole neighbourhood
    // every time either squad steps. Keep the history and make the receiver
    // tolerant of it instead (see applyNpcCensus).
    std::map<std::pair<int, int>, u32> cellLastOwner_;
    // Publisher-side dwell: a tab must read the same NEW cell this many
    // consecutive samples before we claim it, so a body walking the boundary
    // does not hand authority back and forth once a second.
    struct CellDwell { int cx, cz; unsigned int n; };
    // Bodies we are DECLINING to author because the peer's stream already drives
    // them (incumbent holds - see authorHoldsBody). Membership only exists to log
    // the yield once per body instead of once per tick; the decision itself is
    // recomputed from the drive state every pass, so a stale entry costs a line.
    std::set<Key>            cellYield_;
    std::map<u32, CellDwell> claimDwell_;   // our tabRank -> pending cell
    std::map<u32, u32>       claimSeqOut_;  // our tabRank -> last seq sent
    unsigned long            claimSendMs_;  // 1 Hz sample throttle
    unsigned long            claimAssertMs_; // periodic re-assert
    unsigned long            claimMapMs_;    // [cell] MAP dump / PKT_CELL_MAP broadcast cadence
    // HOST ONLY (protocol 59, WORLD-03): CellMapPacket::seq, the host's own
    // monotonic publish counter (log correlation only - clients adopt the
    // map wholesale regardless of seq, the PKT_XFER_COMMIT/PKT_CLAIM_VERDICT
    // precedent of not needing a per-packet dedup key for a host-single-
    // sender broadcast).
    u32                      cellMapSeqOut_;
    enum {
        CELL_OWNER_HOST   = 0,    // NetLink gives the host id 0
        CELL_DWELL_N      = 3,    // samples at 1 Hz
        CELL_ASSERT_MS    = 5000  // re-assert cadence (reconnect insurance)
    };
    // Phase B attach measurement. A dormant -> observed transition forces a
    // reconciliation burst: everything the gate had been leaving alone gets
    // judged in one frame, and the mints deferred past mintR fire together as
    // the camera closes. Size that burst from real runs before deciding
    // whether it needs a pre-warm band or a rate limit. The window opens on
    // the first attach and reports what the attaches cost.
    unsigned long             attnFlips_;      // hands attached this window
    unsigned long             attnWinMs_;      // window open stamp (0 = closed)
    unsigned long             attnBaseSupp_;   // authSuppresses_ at open
    unsigned long             attnBaseCull_;   // censusCulls_ at open
    unsigned int              attnBaseProxy_;  // proxyByKey_.size() at open
    // Cached zone-loaded verdict for the anchor set (Phase C veto). The query
    // takes an engine lock the zone-loader thread writes, so the authority pass
    // reuses a verdict for ATTN_VETO_MS unless an anchor jumped.
    //
    // What is cached is the VERDICT - which anchor indices passed - and never
    // their positions. Caching the surviving positions is what this used to do,
    // and it meant attention was judged against where the players were up to
    // 500 ms ago: at run_apart's ~570 u/s that is ~285 u of error, and 40 u of
    // it was enough to read a body 991 u from a player character as 1031 u from
    // the anchor set and leave it dormant beside its own squad (run
    // 20260804_125739: toPc=991 toAttn=1031 toRaw=991 with anchors=3/3, i.e.
    // nothing vetoed - purely stale coordinates). A verdict is what actually
    // ages slowly, since zones are 4608 u across and the jump check bounds how
    // far an anchor can drift before we ask again.
    unsigned long             attnVetoMs_;     // last query stamp (0 = never)
    float                     attnVetoRaw_[12];// anchors the verdict was for
    unsigned int              attnVetoRawN_;
    unsigned int              attnVetoMask_;   // bit a = raw anchor a survived
    bool                      auditRows_;     // travel_parity worldstate rows
    bool                      jailProbe_;     // KENSHICOOP_JAIL_PROBE [jail] STATE
    bool                      jailObserve_;   // KENSHICOOP_JAIL_OBSERVE [jail] OBSERVE
    // Per-captive last-logged sample for the jail-observe spike (kind + pos +
    // ms), so [jail] OBSERVE lines fire on change/move/timeout, not every tick.
    struct JailObs { int kind; float x, y, z; unsigned long ms;
                     JailObs() : kind(0), x(0), y(0), z(0), ms(0) {} };
    std::map<Key, JailObs>    jailObs_;
    // Third-party furniture placement (protocol 36): ENTER edges detected on
    // peer-owned driven bodies in applyTargets (a guard jailing an arrested
    // player runs purely on the host sim, so the occupant's owner can never
    // author the designed occupant-owner edge). Buffered here because
    // applyTargets has no NetLink; publishOwned drains them onto the wire.
    struct PendFurnEnter { Key occ; unsigned int furn[5]; int kind; };
    std::vector<PendFurnEnter> furnPeerPend_;
    // When WE last authored an owner-side EXIT for an own hand: an in-flight
    // (5 s re-author window) PEER-ENTER must not re-jail a body its owner
    // just freed - the exit-vs-reauthor race guard.
    std::map<Key, unsigned long> ownFurnExit_;
    InterpConfig          cfg_;
    float                 catchupK_;  // walk-drive gap-proportional speed gain
    float                 snapDist_;  // moving-body hard-snap distance floor (u)
    float                 snapSeconds_; // velocity-aware snap gate: teleport only
                                        // when the body trails the newest sample
                                        // by more than this much TRAVEL TIME (the
                                        // fixed distance gate false-fired on
                                        // sprinters and any game speed > 2x)
    float                 combatSoftDist_;    // combat converge soft band (u)
    float                 combatSnapDist_;    // combat churn ceiling / slide trigger (u)
    float                 combatBigSnapDist_; // combat true-leave instant-teleport dist (u)
    float                 combatSlideMax_;    // combat fast-slide speed cap floor (u/s)
    unsigned long         combatConvergeMs_;  // combat snap hysteresis window (ms)
    bool                  sendStamp_; // index interp rings on sender stamps (v35)
    unsigned long         starveHoldMs_;  // guard-hold window past staleness
    unsigned int          starveHeldNow_; // bodies in the hold this tick (stat line)
    bool                  leaderOnly_;
    bool                  streamNpcs_;

    // Bidirectional ownership partition. ownRanks_ = the squad-TAB ranks (distinct
    // sorted hand-containers) we own; ownHands_ = the resolved owned hand keys,
    // refreshed every publishOwned. applyTargets skips any tracked hand in ownHands_
    // (never drive a body we own + simulate locally), defense-in-depth on the partition.
    std::set<unsigned int> ownRanks_;
    std::set<Key>          ownHands_;
    // Every player-squad member key (own + peer), refreshed each publishOwned from
    // the pre-ownership-filter roster. Lets the trade veto tell a squad character
    // apart from a world NPC / container (which are never blocked).
    std::set<Key>          allSquad_;
    // Phase 3 (OWN-01): the PlayerId owner of every player-squad member key
    // (own + peer), refreshed alongside allSquad_/ownHands_ each publishOwned
    // tick from the tab-rank scan (deterministic default: ownerForRank(rank)
    // == rank, OVERRIDDEN by allOwnRanks_ below when non-empty via
    // resolveRankOwner()). Backs ownerOfHand(); ownHands_ still answers
    // isMine() alone - this map generalizes "is it mine" into "who owns it".
    std::map<Key, u32>     handOwner_;
    // Phase 3 Plan 03 (OWN-01/OWN-03): the host-announced authoritative
    // map<PlayerId,set<rank>> (PKT_OWN_RANKS), applied via setAllOwnRanks().
    // Empty until the first announcement is received (client) or built (host);
    // resolveRankOwner() falls back to the rank=playerId default while empty,
    // so a session before the first announcement behaves exactly as Plan 02.
    std::map<u32, std::set<unsigned int> > allOwnRanks_;
    // Phase 3 Plan 03: rank -> owner resolution, consulting allOwnRanks_ first
    // (when non-empty) and falling back to ownerForRank(rank). Backs
    // handOwner_ population in publishOwned (ReplicatorPublish.cpp).
    u32 resolveRankOwner(unsigned int rank) const;

    // Phase 4a inventory state. ownedContainers_ = containers we author (publish, never
    // reconcile back onto us). invPub_ = last published content fingerprint + send time
    // per owned container (resend only on change / periodic). invRecv_ = latest received
    // snapshot per peer-owned container, applied (reconciled) when dirty.
    // hash      = last fingerprint actually SENT to peers.
    // lastSendMs = when we last sent (for the periodic safety resend).
    // pendingHash/pendingSince = the most recent captured fingerprint and when it first
    //   appeared; a change is only SENT once it has been STABLE for INV_SETTLE_MS. This
    //   debounce suppresses sub-second mid-drag transients (e.g. a weapon held by the
    //   cursor briefly leaves the inventory entirely) that would otherwise tell the peer
    //   to DESTROY a worn item it cannot refabricate (createItemAndAdd fails for weapons),
    //   losing it permanently. Settled states (the only ones a user actually rests at)
    //   still replicate, just ~one settle-window later.
    // lastSentN = entry count of the last SENT snapshot (retained for the log fields).
    // lastSentUnits = total UNIT count (sum of stack quantities) of the last SENT snapshot.
    //   A capture with FEWER units is a REMOVAL (something left the inventory) and must
    //   settle far longer than an addition or an equip<->loose MOVE (same units): mid-drag
    //   the cursor holds the item OUT of the inventory for up to ~1 s, a transient "gone"
    //   the peer would act on by DESTROYING a worn item it cannot refabricate. Equip and
    //   unequip-to-bag keep the unit total, so they still replicate fast; only genuine
    //   removals (and the cursor flicker) wait.
    //   Units rather than ENTRIES (protocol 46): the entry count is clamped at
    //   INV_ITEMS_MAX, so a container sitting at the cap kept the SAME entry count across
    //   a removal and silently took the fast path - which is what let a bag snapshot beat
    //   the debounced W2 drop intent and lose the dropped item. Unit totals also catch a
    //   partial-stack removal (drop 5 of 10), which never changed the entry count at all.
    struct InvPub { u32 hash; unsigned long lastSendMs; u32 pendingHash; unsigned long pendingSince; unsigned int lastSentN; unsigned int lastSentUnits; };
    // truncated (protocol 46): the author's container did not fit in INV_ITEMS_MAX, so
    // `items` is an INCOMPLETE description - reconcile additive-only, never delete.
    struct InvRecv { u32 ownerId; std::vector<InvItemEntry> items; bool dirty; bool truncated; };
    std::set<Key>          ownedContainers_;
    std::map<Key, InvPub>  invPub_;
    std::map<Key, InvRecv> invRecv_;
    // Protocol 34: the host's ~1 Hz container census result (LOCAL hands of
    // complete STORAGE/machine-class buildings in the interest spheres).
    // Folded into the authored set each publishInventories pass while
    // storeSync_ is on; per-container change-gate state lives in invPub_ as
    // usual. Refreshed wholesale each census (leaving interest just stops
    // the captures; the invPub_ baseline survives for the return).
    bool           storeSync_;
    unsigned long  contCensusMs_;
    std::set<Key>  censusContainers_;

    // Phase W1 world-item state.
    // HOST: worldTrack_ maps a ground item's LOCAL engine hand (Key) to its assigned
    //   netId + last-sent content+pos hash + last send time. nextWorldNetId_ hands out
    //   fresh ids. Items in the map but not seen this scan have left the world -> culled.
    // JOIN: worldProxies_ maps a host netId to the LOCAL proxy object we spawned for it,
    //   plus the last applied pos/hash so we only re-place it on real change.
    // stringID/itemType/quantity/quality are captured at DISCOVERY (a spatial-scan
    // row or a query-free drop-hook edge) so the item can be streamed and refreshed
    // by HANDLE resolution alone - no rescan needed. `seen` is retained only for the
    // legacy scan path; culling is now handle-based (engine::groundItemLiveness).
    // `baseline` (Phase 3 item-dup fix): a ground item present at the FIRST
    // post-load spatial scan is a SHARED save-native - both clients already hold
    // it identically, so it must never be streamed (authoring it mints a proxy
    // on top of the peer's own native = the rejoin/reload duplication). Baseline
    // tracks are recorded for identity/liveness but skipped in the emit path;
    // only items appearing AFTER the baseline (session drops, runtime spawns)
    // stream. Re-seeded after every reload via worldSeeded_ in resetSession().
    struct WorldTrack {
        u32 netId; u32 hash; unsigned long lastSendMs; float x, y, z; bool seen;
        char stringID[48]; u32 itemType; u16 quantity; u16 quality; bool baseline;
    };
    // `hand` is the proxy's engine identity, captured (and round-trip verified) at
    // mint. The engine owns the object's lifetime: when the block it stands in
    // unloads - which is what travelling far DOES - the item is destroyed and `obj`
    // becomes freed memory we still hold. Every use goes through liveWorldProxy(),
    // which re-resolves the hand and hands back only a pointer the engine still
    // owns. An all-zero hand means the round-trip failed at mint; those fall back
    // to the raw pointer, because dropping them instead would leak the proxy.
    struct WorldProxy { RootObject* obj; unsigned int hand[5]; float x, y, z; u32 hash; };
    // The one gate between a stored proxy pointer and the engine. Returns 0 once
    // the object is gone (hand no longer resolves, or its table slot was recycled
    // by a different object), in which case the caller must drop its mapping and
    // touch nothing: the engine already destroyed the item, and destroying,
    // moving or even READING it a second time is a use-after-free.
    RootObject* liveWorldProxy(const WorldProxy& wp);
    std::map<Key, WorldTrack> worldTrack_;
    // Spawned proxies for PEER-authored ground items, keyed by (ownerId, netId):
    // W1 is bidirectional (each client streams the free ground items it authors),
    // so netId spaces are per-sender and culls are scoped to their owner.
    std::map<std::pair<u32, u32>, WorldProxy> worldProxies_;
    u32                        nextWorldNetId_;
    // Protocol 58 (Phase 7 Plan 02, INV-03): the item identity of a claim WE
    // authored, keyed (authorId, netId) - captured (engine::readItemIdentity)
    // at the SAME moment the claim's proxy tracking is erased (publishWorldItems'
    // claim pass), because that is the only tick the object is still resolvable.
    // applyClaimVerdict consumes this to perform the loser rollback (remove-by-
    // sid from one of OUR OWNED hands) without needing to re-derive the identity
    // from an object that may no longer even exist by verdict time. Erased once
    // the verdict for that key lands (win or lose - the outcome is decided).
    struct PendingClaimIdentity { char stringID[48]; u32 itemType; u16 quantity; };
    std::map<std::pair<u32, u32>, PendingClaimIdentity> myClaims_;
    // Phase 7 review WR-03: ALL-clients dedup of RECEIVED claim verdicts,
    // keyed (authorId, netId) - the appliedXfers_ idiom applied to
    // PKT_CLAIM_VERDICT. The host's CLAIM-LATE path re-BROADCASTS the prior
    // verdict to every client (a genuinely new packet, not an ENet resend),
    // and without dedup a duplicate verdict drained before the author's next
    // publish/cull pass found no resolvable object (`ro == 0`), took the
    // "could not destroy" branch, and erased the author's track WITHOUT any
    // cull ever being sent - a permanent ghost proxy on every third client.
    // Verdicts are commit-final one-shots per (authorId, netId), so a seen
    // key is always a duplicate. Reuses ClaimFinal (winner + insertion seq)
    // so cap eviction is insertion-ordered (WR-05). Cleared in resetSession()
    // and author-purged on a peer's (re)connect (CR-01) exactly like
    // claimFinals_ - a stale entry would otherwise SWALLOW the verdict for a
    // rejoined author's recycled netId.
    std::map<std::pair<u32, u32>, coop::ClaimFinal> appliedClaimVerdicts_;
    u32                                             appliedClaimVerdictSeq_;
    // First-scan-baseline latch (Phase 3): false after launch/reload; the first
    // publishWorldItems pass seeds every in-range save-native as a baseline track
    // (never-emit) and sets this true. resetSession() clears it so the reloaded
    // world (which may now contain previously-dropped-then-baked items) re-baselines
    // rather than re-streaming - closing the per-reload duplicate layering.
    bool                       worldSeeded_;

    // Phase W2 conservation-drop state.
    // weaponCensus_: per OWNED character hand, the last-tick set of WEAPON copies it held,
    //   keyed by "sid|type" -> {provenance + count}. A count DECREASE that coincides with a
    //   free ground weapon near the character is a drop. seeded=false on first sight (we
    //   record the baseline without emitting a spurious drop). nextDropId_ hands out per-
    //   sender monotonic ids. appliedDrops_ dedupes received intents by (ownerId,dropId).
    struct WCensusItem { char manufacturer[48]; char material[48]; u16 quality; int count; unsigned int itemType; };
    // NOTE the constructor. weaponCensus_[key] VALUE-initializes this struct, but because
    // the std::map members make its implicit default constructor non-trivial, the C++03
    // rules this toolchain follows do NOT zero the scalars first - `seeded` came up as
    // whatever the allocator's memory held. A garbage TRUE meant the census skipped its
    // baseline entirely and then read the character's whole worn kit as INCREASES, firing
    // one identity-less PICKUP intent per gear item (and it fired again later in a session,
    // as heap reuse made the garbage more likely). Zeroed pages hid this most of the time.
    struct WCensus {
        WCensus() : seeded(false), zeroReads(0) {}
        std::map<std::string, WCensusItem> items;
        std::map<std::string, int>         retries; // per-sid ground-correlation retry budget
        // Wall-clock ms at which each sid's retry budget was FIRST armed. The budget is
        // frame-denominated and xferPendingLoss re-arms it, so it can cycle for as long as
        // the transfer detector holds an unresolved loss (observed: 29->1->29 repeatedly).
        // That is fine for the drop verdict itself, but wdPendingDrop gates the whole
        // inventory snapshot on it, so the hold needs an absolute ceiling - see WD_HOLD_MAX_MS.
        std::map<std::string, unsigned long> retryArmMs;
        std::map<std::string, std::deque<void*> > ptrs;        // current weapon Item* per sid
        bool seeded;
        // Consecutive ZERO-ROW reads of this container. A zero-row read carries no
        // information (mid-reload), so it is never committed as a baseline - but a
        // genuinely empty character reads zero forever and still needs one, so an empty
        // baseline is accepted only after the read REPEATS (WD_EMPTY_SEED_TICKS).
        unsigned int zeroReads;
    };
    std::map<Key, WCensus>            weaponCensus_;
    u32                               nextDropId_;
    std::set<std::pair<u32, u32> >    appliedDrops_;

    // Phase W3 conservation-pickup state.
    // groundedWeapons_: the REAL ground Item* handles THIS client is tracking per sid (from
    //   its own UI drop OR from relocating a peer's drop). A received PICKUP re-homes one of
    //   these into the picking character's bag; a local pickup (census increase) consumes one.
    // Each entry also carries the drop's GLOBALLY-SHARED identity (dropOwnerId, dropId) - both
    //   clients agree on it from the DROP packet, so a PICKUP can name the EXACT instance to
    //   re-home. This disambiguates two same-sid weapons on the ground (one dropped by each
    //   client): FIFO-by-sid would otherwise re-home the wrong copy on the peer.
    // nextPickupId_ hands out per-sender monotonic ids; appliedPickups_ dedupes received ones.
    // A cached ground Item* goes STALE (the engine despawns/streams the object out) and
    // nothing used to prune this list, so a later re-home handed addItemPtrToInventory a
    // dead pointer. `hand` is the object's save-stable engine hand, so the re-home can
    // RE-RESOLVE the object and test its liveness instead of trusting `item`.
    //
    // pending*: a re-home that FAILED (the object is alive but the bag refused it - an
    // occupied equip slot, no room, a container still resolving). The entry is retried by
    // reconcileGroundGear until pendingSinceMs + WD_REHOME_MAX_MS, and only then may the
    // author destroy its ground copy - and ONLY after it has SEEN the item in the target
    // container. Destroying on faith would lose the item outright wherever the reconcile
    // cannot recreate it (a truncated read, KENSHICOOP_WEAPON_FAB=0, or a backpack, which
    // must never be fabricated because the copy would be empty).
    struct GroundWeapon {
        u32           dropOwnerId;
        u32           dropId;
        void*         item;
        unsigned int  hand[5];
        unsigned int  pendingHand[5];   // re-home target container (all-zero = none pending)
        unsigned int  pendingType;      // itemType of the pending re-home
        unsigned long pendingSinceMs;   // when the first re-home attempt failed
        // Consecutive ticks this object failed to read as a live free ground item. A track is
        // only dropped once that REPEATS: discarding it on a single bad read is how the author
        // ended up with no answer for a real pickup, and the cost of being wrong is asymmetric
        // (a forgotten track means a permanent duplicate, whereas keeping a dead one costs a
        // pointer until the next read).
        unsigned int  deadReads;
        // ...and WHEN the current dead streak began, because the tick count alone is a ~25 ms
        // tolerance at engine tick rate - far too short to distinguish "gone" from "streamed out
        // for a moment". Zero while the last read was live. createdMs/everLive bound the case
        // where the object never resolved even once, which has no ground copy to protect.
        unsigned long firstDeadMs;
        unsigned long createdMs;
        bool          everLive;
    };
    std::map<std::string, std::deque<GroundWeapon> > groundedWeapons_;
    u32                               nextPickupId_;
    std::set<std::pair<u32, u32> >    appliedPickups_;

    // An IDENTIFIED pickup intent we could not satisfy the moment it arrived, because the named
    // track had just been retired or we never held it. Concluding from that single attempt is
    // not neutral: a real object left the ground over there, so our own copy is now a duplicate
    // for the rest of the session. The only fallback is a site-anchored spatial scan, and that
    // is the query that fails in towns - which is precisely where the player hit this. So park
    // the intent instead and retry it from reconcileGroundGear: by the named track if it
    // resolves again (a streamed-out object usually does), by the site scan otherwise, until
    // WD_REHOME_MAX_MS. Only identified intents are ever parked; an identity-less one still
    // gets nothing, because we cannot know which object it took.
    struct PendingPickup {
        unsigned int  targetHand[5];
        std::string   sid;
        unsigned int  itemType;
        u32           refOwnerId;
        u32           refDropId;
        unsigned long sinceMs;
    };
    std::deque<PendingPickup> pendingPickups_;

    // Start tracking a just-grounded conservation item: records the drop identity both
    // clients share, the raw Item*, AND the object's engine hand (so a later re-home
    // re-resolves rather than trusting the pointer), with the pending re-home state clear.
    // `authored` distinguishes a drop WE detected locally from one we mirrored for a peer;
    // only the test forget-lever reads it (it must blind the author without blinding the
    // picker, or no identified pickup is ever sent at all).
    void trackGroundGear(const std::string& sid, u32 dropOwnerId, u32 dropId, void* item,
                         bool authored);

    // Park an identified pickup we could not satisfy on arrival (see PendingPickup), and the
    // per-tick retry that drains it. Idempotent in (refOwnerId, refDropId) so a reliable resend
    // of the same intent does not stack retries.
    void parkPendingPickup(const unsigned int targetHand[5], const char* sid,
                           unsigned int itemType, u32 refOwnerId, u32 refDropId);
    void retryPendingPickups(GameWorld* gw);

    // Protocol 37 cross-owner transfer state.
    // xferBase_: last-known per-item totals (sid,type)->qty per tracked container - the
    //   reference the drag detector diffs against. Rebased after every mutation WE make
    //   (reconcile, transfer apply, W2/W3 relocation) so only USER actions register.
    // xferPend_: currently-observed unpaired diffs + when each first appeared (a pair of
    //   matching loss/gain older than the settle window becomes an intent; a diff that
    //   never pairs is folded back into the baseline after a timeout).
    // xferLatch_: per PEER container, the pending local delta a sent/applied transfer
    //   implies. While active, applyInventories ADJUSTS the received snapshot by it
    //   (suppressing the reconcile-back that caused the dupe/wipe); cleared when the
    //   owner's snapshot catches up to the local total, or on deadline.
    // wdSuppress_: (character key, sid) -> expiry; a gear transfer must not be read by
    //   the W2 weapon census as a ground drop / pickup of that sid on those characters.
    typedef std::pair<std::string, u32> XKey; // (stringID, itemType)
    struct XferPend  { int delta; unsigned long sinceMs; };
    struct XferLatch { int delta; unsigned long deadlineMs; };
    std::map<Key, std::map<XKey, int> >       xferBase_;
    std::map<Key, bool>                       xferSeeded_;
    std::map<Key, std::map<XKey, XferPend> >  xferPend_;
    std::map<Key, std::map<XKey, XferLatch> > xferLatch_;
    // Per peer container: when applyInventories first saw a LOCAL diff vs the
    // detector baseline (an unadjudicated user mutation - possibly one end of a
    // cross-owner drag). The reconcile DEFERS while it is younger than the defer
    // window, giving the detector time to pair the drag and author the intent
    // (otherwise the reconcile undoes the drag and the rebase erases the evidence).
    std::map<Key, unsigned long>              xferDefer_;
    u32                                       nextXferId_;
    std::set<std::pair<u32, u32> >            appliedXfers_;
    // SUPERSEDED (protocol 50 -> 58, Phase 7 Plan 01 Task 2): dormant, no
    // longer written or read anywhere - the AUTHOR's own release now reads
    // the release magnitude straight off the host's PKT_XFER_COMMIT
    // (applyXferCommit/processXferIntents' isAuthor branch), keyed
    // (authorId,transferId), so nothing needs to be remembered locally
    // between the intent and its verdict anymore. Kept declared (not
    // deleted) purely to minimize this plan's diff footprint; safe to
    // remove in a later cleanup pass.
    struct XferOut {
        Key           src;
        Key           dst;
        XKey          key;
        int           qty;
        bool          srcPeer;   // we latched src (it is peer-authored)
        bool          dstPeer;
        unsigned long sentMs;
    };
    std::map<u32, XferOut>                    xferOut_;
    // Undo one intent's contribution to a peer end's reconcile-suppression
    // latch (protocol 50). See the definition for why every verdict does this.
    void releaseXferLatch(const Key& k, const XKey& key, int delta);
    // Protocol 58 (Phase 7 INV-02/INV-04): HOST-ONLY pending-transfer table
    // (XferCommit.h's state machine), keyed (authorId, transferId). Written
    // only by processXferIntents (xferBegin/xferCommitOnce) and erased
    // owner-scoped on disconnect by clearPeerReplicationState
    // (ReplicatorCore.cpp, via xferEraseOwner) - the transfer-table carve-out
    // out of the otherwise-untouched xfer state, mirroring pinnedOwner_'s
    // Phase 6 precedent. Never read or written by a client's apply path.
    // pendingXferSeq_ is the monotonic insertion counter cap-eviction orders
    // by (Phase 7 review WR-05 - see XferCommit.h::XferPendingEntry).
    std::map<std::pair<u32, u32>, coop::XferPendingEntry> pendingXfer_;
    u32                                       pendingXferSeq_;
    // Protocol 58 (Phase 7 Plan 02, INV-03): HOST-ONLY claim-contention state
    // (ClaimArbiter.h), keyed (authorId, netId) - the item's shared identity
    // (the SAME key W1 claims already use). claimWindows_ holds every OPEN
    // contention window; claimFinals_ records every FINALIZED (authorId,netId)
    // WITH its winner in one record (Phase 7 review WR-05: the old separate
    // claimCommitted_ set + claimWinners_ map were capped independently and
    // could diverge - "committed but winner evicted" answered winner=0) so a
    // later-arriving claim for an already-decided identity is answered with
    // the SAME prior winner, never re-arbitrated ("commit is final" -
    // INV-03). Written only by applyClaimIntents; claimWindows_ entries are
    // additionally owner-scoped on disconnect by clearPeerReplicationState
    // (via coop::claimEraseClaimant), mirroring pendingXfer_'s Phase 7
    // Plan 01 precedent. Never read or written by a client's apply path.
    // claimFinalSeq_ is claimFinals_'s monotonic insertion counter (WR-05).
    std::map<std::pair<u32, u32>, coop::ClaimWindow> claimWindows_;
    std::map<std::pair<u32, u32>, coop::ClaimFinal>  claimFinals_;
    u32                                              claimFinalSeq_;
    // Protocol 58 shared apply primitive: attempts to relocate the REAL item
    // from sHand to dHand (up to `quantity` units) and - ONLY when
    // `permission` is XFER_COMMIT_FABRICATE - fills any remaining shortfall
    // via the same backpack-never-fabricate + KENSHICOOP_WEAPON_FAB-gated
    // mint the old applyTransfers used (never called at all for a REJECT
    // outcome - the caller skips it entirely). Used by BOTH the HOST
    // deciding the outcome (processXferIntents, called with permission =
    // XFER_COMMIT_FABRICATE - full authority; the actual RELOCATE/FABRICATE/
    // REJECT split is read back from *outMoved/*outFab afterward) and every
    // OTHER client executing the host's already-decided outcome
    // (applyXferCommit, called with permission == the commit's own outcome
    // field - RELOCATE never attempts fabrication, matching research
    // Pitfall 2: fabrication is a host decision, never an independent
    // per-receiver one). Returns moved+fab.
    int relocateOrFabricate(GameWorld* gw, const unsigned int sHand[5],
                            const unsigned int dHand[5], const char* sid,
                            u32 itemType, int quantity, u16 quality, u8 level,
                            const char* manufacturer, const char* material,
                            u8 permission, int* outMoved, int* outFab);
    // Protocol 58 (Phase 7 Plan 02, INV-03) shared apply primitive: applies
    // ONE finalized claim verdict from `localId`'s own point of view - author
    // destroys its real ground object (verdict-gated); a losing claimant rolls
    // back its optimistic pickup; the winner (or a non-participant) no-ops.
    // Called by applyClaimIntents for the HOST's OWN perspective (the host
    // never receives its own PKT_CLAIM_VERDICT broadcast back over the wire,
    // exactly like PKT_XFER_COMMIT - processXferIntents's own precedent) and
    // by applyClaimVerdict for every OTHER client's received verdict.
    void applyClaimOutcome(GameWorld* gw, u32 authorId, u32 netId,
                           u32 winnerPlayerId, u32 localId);
    std::map<std::pair<Key, std::string>, unsigned long> wdSuppress_;
    unsigned long                             xferScanMs_; // last detector scan
    // Recapture `k`'s container and overwrite its baseline (clears its pends): call
    // after ANY local mutation we make ourselves so the detector only sees the user.
    void xferRebase(GameWorld* gw, const Key& k);
    // True while the transfer detector is watching an unresolved LOSS of `sid` from
    // container `k` - the W2 census fallback defers its drop verdict for it.
    bool xferPendingLoss(const Key& k, const char* sid);
    bool wdSuppressed(const Key& k, const char* sid, unsigned long now);
    // True while the W2 gear census still has an unresolved DECREASE for container `k`
    // (a drop intent is debouncing its ground correlation). publishInventories holds the
    // container's snapshot until then, so the post-drop bag can never reach the peer
    // ahead of the intent that explains it.
    bool wdPendingDrop(const Key& k) const;

    // Smoothness accumulators (measured from the body's actual motion while its
    // source is moving): how often did the rendered body advance per frame?
    unsigned long activeFrames_;
    unsigned long zeroWhileActive_;
    float         maxStep_;
    unsigned long slewSkipFrames_; // active frames excluded while clock-slewing
    // zeroFrac population audit. A frozen-while-active frame is only a DEFECT if
    // the body could have walked that frame; the metric currently scores bodies
    // that structurally cannot (player_ko reads 1.000 with maxStep 0.0 - exactly
    // what a knocked-out body must read). Bucket every scored zero frame by what
    // the body WAS, so the share that is unfixable-by-definition is visible before
    // anyone tries to move the number. Emitted as "SCENARIO ZEROPOP".
    unsigned long zpDown_;       // down / ragdoll / dead
    unsigned long zpCarried_;    // on someone's shoulder
    unsigned long zpFurn_;       // in a bed or cage
    unsigned long zpChain_;      // shackled
    unsigned long zpCrawl_;      // crippled, moving under its own power
    unsigned long zpSneak_;      // crouched
    unsigned long zpSquadIdle_;  // squad body scored active by cSpeed alone: hostMoving
                                 //   ORs currentlyMoving with cSpeed > MOVE_EPS, and
                                 //   cSpeed is the locomotion speed SETTING, which stays
                                 //   high on a standing body (a parked Garru streams
                                 //   15.2) - so a squad body at rest is scored active
    // Upright and free - the whole population, as it turned out: the audit measured
    // 100% of scored zero frames here and 0 in every state above, because a body
    // that cannot walk `continue`s out of the drive long before the scoring block.
    // So the two live explanations are both about the free body, and they call for
    // opposite responses:
    unsigned long zpAlias_;      //   it advanced within the last ZERO_ALIAS_MS - the render
                                 //   frame simply outran the engine's character-update step,
                                 //   so the per-frame sample aliased. Invisible to a player.
    unsigned long zpStall_;      //   it has not advanced for ZERO_ALIAS_MS or more: a real
                                 //   freeze while the source walks. The defect.

    // Motion-onset audit (2026-08-10). smoothness fails as a function of HOW
    // MANY frames it was measured on: corr(log(active), zeroFrac) = -0.603 over
    // 91 npc_sync runs, and the median moves with the sample (0.483 under 1500
    // scored frames against 0.321 at or above), which is a bias rather than the
    // wider scatter a small sample alone would give. Suspicion fell on the
    // oracle's own gate: NPCs are judged on INSTANTANEOUS stream velocity, so a
    // body near NPC_MOVE_VEL re-enters the scored population over and over and
    // pays a fresh render catch-up each time. Bucket every scored frame by its
    // age since the body's oracle-active onset to find out.
    //
    // It measured true, and larger than expected. Over 4 npc_sync runs (which
    // reproduced the historical bias at corr = -0.583):
    //   - earlyFrac is a CONSTANT 0.954-0.965 while whole-run zeroFrac ranges
    //     0.282-0.614. The first 100 ms after an onset is a fixed toll, not a
    //     quality signal - it reads ~0.96 whether the run passes or fails.
    //   - zeroFrac is then just the MIX: corr(earlyShare, zeroFrac) = 0.998.
    //   - re-entries stay ~230-300 per run no matter how long the run scored,
    //     so that toll is a near-fixed COUNT of zero frames divided by however
    //     much sustained motion the run happened to catch. That quotient is the
    //     sample-size bias, arithmetically.
    //   - most onsets are too short to be walking at all: ~3-4 scored frames
    //     per re-entry, and midN is small enough that only a handful of the
    //     ~250 episodes per run survive 100 ms. The gate is admitting fidgets
    //     and turns-in-place, where the join body correctly does not translate.
    // Excluding the early bucket turns both failures into passes (0.511->0.209,
    // 0.614->0.284) and preserves the runs' ranking, so the gate keeps its
    // discrimination. combat_kill is the control: squad bodies are gated on
    // hostMoving instead, do not flicker (42-58 re-entries, earlyShare 0.007 -
    // 0.067), and show no inflation - its 0.307 is real.
    // Emitted as "SCENARIO ONSET". Reported, never gated: this measures the
    // cause so the eventual fix is chosen on evidence, following ZEROPOP.
    unsigned long onsetReentries_;  // oracle-active false->true edges, all bodies
    unsigned long onActiveEarly_;   // scored frames  < ONSET_EARLY_MS since onset
    unsigned long onActiveMid_;     //   ONSET_EARLY_MS .. ONSET_SETTLE_MS
    unsigned long onActiveSteady_;  //   ONSET_SETTLE_MS or older: settled motion
    unsigned long onZeroEarly_;     // of those, the ones that rendered no advance
    unsigned long onZeroMid_;
    unsigned long onZeroSteady_;

    // Interp/drive telemetry (protocol 36 jumpiness instrumentation): per-sample
    // regime counts from EntityInterp::lastMode() (EXTRAP/CLAMP = the buffer
    // starved; SEG = a source teleport crossed the ring), hard-snap and
    // walk-reissue counts from the drive layer, split squad (the other player's
    // characters - the user-facing jumpiness) vs world NPCs. Surfaced on a ~5 s
    // "[interp]" stat line + a "SCENARIO INTERP" summary in logSmoothSummary.
    unsigned long interpLerp_;
    unsigned long interpSingle_;
    unsigned long interpClampOld_;
    unsigned long interpExtrap_;
    unsigned long interpSegSnap_;
    unsigned long hardSnapSquad_;   // SNAP_DIST applyRaw on a moving squad body
    unsigned long hardSnapNpc_;     // SNAP_DIST applyRaw on a moving world NPC (near tier)
    unsigned long hardSnapMid_;     // same, MID-tier bodies (Phase 2): counted apart so
                                    //   the snap-rate gate keeps guarding the validated
                                    //   20 Hz pipeline; a newly covered mid NPC's one-time
                                    //   reconciliation snap is correct, not rubber-banding
    unsigned long walkReissueSquad_;
    unsigned long walkReissueNpc_;
    unsigned long restFlipNpc_;     // walk->rest classifier transitions (flap telemetry)
    unsigned long restFlipMid_;     // same, mid-tier bodies (kept out of the near gate)
    // Combat warp diagnosis (Phase 1): cumulative combat-drive corrections, surfaced
    // on the ~5 s "[combat] stats" line and diffed by the combat_snap_rate oracle.
    // A driven combatant is engine-driven locally (its own footwork), so position is
    // corrected in graded bands; these count each band so a session log tells warp
    // (snap) from smooth convergence (softWalk) and names WHY (wrongTgt).
    unsigned long combatSnapTotal_; // combat hard teleports (applyRaw) - the visible warp
    unsigned long combatSoftWalk_;  // combat soft walk-converge corrections (no teleport)
    unsigned long combatSlide_;     // combat FAST-SLIDE corrections: drift past the old snap
                                    //   band (COMBAT_SNAP_DIST) converged with a quick walk
                                    //   instead of a teleport (the smoothness-pass win - these
                                    //   used to all be visible warps)
    unsigned long combatOrder_;     // combat attack (re-)issues (order/target reconcile)
    unsigned long combatWrongTgt_;  // hard snaps fired while fighting the WRONG local target
    unsigned long combatLogTick_;
    unsigned long interpLogTick_;

    // Protocol 36 (wire v35): per-sender clock mapping for the batch send
    // stamp. offsetMs tracks the MINIMUM observed (arrival - sendMs) - the
    // fastest packet carries only the base path delay + clock difference, so
    // min-tracking strips queueing jitter; mapped time = sendMs + offsetMs
    // reconstructs the sender's true capture spacing in the local clock. The
    // offset creeps up slowly (~2 ms/s) so a route change that RAISES the
    // base delay re-converges instead of reading as permanent starvation.
    struct PeerClock {
        long offsetMs; bool have; unsigned long lastCreepMs;
        PeerClock() : offsetMs(0), have(false), lastCreepMs(0) {}
    };
    std::map<u32, PeerClock> peerClock_;

    // Anim-truth accumulators (the float-bug oracle): of the frames where the
    // body's actual position translated, how many reported a real walk state
    // (currentlyMoving && currentSpeed>0)? A high floatFrac == translating while
    // reporting idle == the float bug.
    unsigned long translateFrames_;
    unsigned long walkTruthFrames_;

    // March-in-place oracle (the INVERSE of the float bug): of the frames where the
    // host is AT REST (not translating), how many had the driven body reporting a
    // walk state (currentlyMoving && currentSpeed>0) while NOT actually moving? A
    // high marchFrac == the body is playing a walk clip in place == the "walking on
    // the spot where the host sits" bug. This is the failure anim-truth cannot see.
    unsigned long restSampleFrames_;
    unsigned long marchFrames_;
    // Attribution of those march frames, so a fix can target the right site
    // instead of re-tuning the walk hold (which an A/B on 2026-08-01 measured as
    // the wrong lever - see the note at the walk/rest classifier):
    //   hold    - the drive was still in the WALK branch (the hold outliving the
    //             source's stop), so it is walk-ordering an arrived body
    //   settle  - within MARCH_SETTLE_MS of entering rest: endAction has fired but
    //             the engine has not yet dropped the walk state
    //   relapse - at rest well past the settle: the body RE-ACQUIRED a walk while
    //             parked (the class quietRelapse_ re-quiets)
    unsigned long marchHold_;
    unsigned long marchSettle_;
    unsigned long marchRelapse_;
    // The hold bucket split by WHY the hold was still up, which decides whether a
    // march frame is a real defect or the oracle disagreeing with the debounce:
    //   dip  - a fast sample arrived moments ago, so the source is genuinely
    //          WALKING and this is the sample-boundary velocity dip the hold was
    //          built to bridge. The oracle calls the host "at rest" on that same
    //          instantaneous velocity, so it scores a correctly-walking body.
    //   stop - no fast sample for a long time: the hold is outliving a real stop
    //          and the body is being walk-ordered where it already stands. THIS is
    //          the visible idle jitter.
    unsigned long marchHoldDip_;
    unsigned long marchHoldStop_;

    // AI-gating spike: how often does a driven NPC's LOCAL task match the host's
    // streamed rawTask? High agreement => we can gate the local AI (freeze when it
    // matches, release when it diverges) instead of replicating animation data.
    unsigned long gateSamples_;
    unsigned long gateAgree_;
    unsigned long gateLogTick_;

    // AI-gating probe state: recruit each diverged NPC at most once, capped so the
    // experiment stays observable rather than absorbing the whole bar.
    bool                 probeRecruit_;
    std::set<Key>        probed_;
    unsigned int         probedCount_;

    // AI-suspend state (default-on quieting layer).
    bool                 aiSuspend_;
    unsigned long        aiLogTick_;

    // Quieting-patchwork counters (step-2 pruning evidence). With AI-suspend as the
    // default quieting layer these mechanisms should be dead code; the counters
    // MEASURE that claim per run ("SCENARIO QUIET ..." in logSmoothSummary) so the
    // eventual deletion is evidence-gated rather than hopeful.
    unsigned long        quietRelapse_;   // I11: endAction re-fired on a march relapse
    // Protocol 53: CharMovement::restore calls on a driven crawler whose physics
    // character the engine tore down at collapse and (being AI-suspended) never
    // re-created. Nonzero means the "mesh frozen at the collapse spot while the
    // nametag follows the stream" repair actually fired.
    unsigned long        crawlPhysRestore_;
    unsigned long        sitOrders_;      // applyTaskOrder issues (the sit/work APPLY lever)
    unsigned long        detachUses_;     // detachFromTownAI calls from applyRest (sitter path)
    bool                 noDetach_;       // KENSHICOOP_NO_DETACH=1: skip sitter detach (A/B experiment)

    // Phase 4 Plan 03 Task 1 audit (POC-02, N=4 combat/medical state-keying
    // review): every combat/medical replication field below this point
    // (pendingHits_, attackerOf_, combatCapMs_, medPub_/medRecv_, statsPub_,
    // targets_'s Driven::combat*/koLatched/deathLatched/carriedDown fields,
    // hostBody_) is keyed by Key (the body's own hand) or by (peer's hand ->
    // {attacker hand, ms}), never by a bare per-peer scalar. This is already
    // N-safe by construction: two joins' combat/medical state can never
    // collide because their squad members and any world NPC they touch are
    // always DISTINCT hands, regardless of how many peers are connected - the
    // established std::map<Key,...>/map<PlayerId,...> idiom this project
    // already uses for allOwnRanks_/epochSeen_/proxyByKey_ generalizes here
    // with no code change required. PKT_TREATMENT's one genuine N>2 gap (a
    // join-authored treatment intent that the host neither applied - the
    // authority guard below skips a body we don't own - nor relayed, so it
    // was silently lost at N>2) was CLOSED in Phase 6 (06-01, GAP-1):
    // NetLink::routingClassOf(PKT_TREATMENT) is now RELAY_BROADCAST_EXCEPT
    // (Class A), correct precisely because this file's own applyTreatments
    // authority guard already ignores a relayed delta for a body it doesn't
    // own - see docs/TWO_PLAYER_ASSUMPTIONS.md's Phase 6 per-channel audit
    // section for the full verdict table.
    // The one confirmed singular-per-peer field found during this audit was
    // speedPeerReq_/speedPeerCombat_ below (a HOST-only scalar tracking "the
    // join's" request/combat flag - genuinely last-writer-wins across 3
    // joins at N=4): CONTEXT.md scoped "speed cap CONS-02" to a later
    // Milestone B phase at the time (deferred: N-player voting), documented
    // here and in 04-03-SUMMARY.md rather than converted. RESOLVED by Phase 9
    // Plan 02 (09-02, CONS-02): speedPeerReq_/speedPeerCombat_ are replaced
    // by the per-owner speedVotes_ map (SpeedVote.h) - see
    // docs/TWO_PLAYER_ASSUMPTIONS.md's "Phase 9 (09-02, CONS-02)" section.
    // See Task 2 (applyCombatHits below) for the one real N>2-only gap this
    // audit found and fixed: a defense-in-depth fail-safe for a combat-hit
    // report whose target hand resolves to a DIFFERENT player's squad member
    // (not the host's own, not a world NPC) - unreachable today because the
    // JOIN-side capture already filters isSquad out (ReplicatorDrive.cpp),
    // but the HOST-side apply had no equivalent backstop of its own.

    // Damage-guard state (join side): suppress local melee damage on driven bodies.
    bool                 dmgGuard_;

    // Join-dealt authoritative damage report (protocol 45). reportCombat_ is set on
    // the JOIN only; while ON, applyTargets drains the guard's accumulated per-copy
    // damage into pendingHits_ (keyed by the copy's canonical hand), and
    // publishCombatHits forwards it to the host. nextHitId_ is a per-sender counter
    // for log correlation.
    struct PendingHit { float flesh; float blood; PendingHit() : flesh(0.0f), blood(0.0f) {} };
    bool                 reportCombat_;
    std::map<Key, PendingHit> pendingHits_;
    unsigned int         nextHitId_;

    // Carried-body sync (protocol 18): master enable (KENSHICOOP_CARRY_SYNC).
    bool                 carrySync_;
    bool                 furnSync_;
    // Chained/pole prisoner sync (protocol 41): master enable
    // (KENSHICOOP_CHAIN_SYNC). Sub-gate within the furniture pipeline.
    bool                 chainSync_;
    // Stealth sync (protocol 20): master enable (KENSHICOOP_STEALTH_SYNC).
    bool                 stealthSync_;
    // Prone posture sync (protocol 53): master enable (KENSHICOOP_PRONE_SYNC).
    bool                 proneSync_;
    // Host-side detection-feedback publish state per DRIVEN sneaker: last sent
    // map fingerprint + send time (~4 Hz change-gated), and whether the last
    // snapshot was ACTIVE (so the end of a sneak authors exactly one clearing
    // empty snapshot).
    struct StealthPub {
        u32 hash; unsigned long lastSendMs; bool active;
        StealthPub() : hash(0), lastSendMs(0), active(false) {}
    };
    std::map<Key, StealthPub> stealthPub_;
    // Receive-side per-OWNED-sneaker detection state (Phase 6 review WR-01):
    // with the PKT_STEALTH Class A relay, SEVERAL authorities (the host's
    // world + every other join's locally-simulated world) can publish
    // detection maps for the SAME owned sneaker. An empty falling-edge
    // snapshot from author A must not clearStealthSeers() away the entries
    // author B replayed moments ago (latest-wins across authors would read
    // "clear" while genuinely detected in B's world). Track the last author
    // whose ACTIVE (n>0) snapshot we replayed and when: an n==0 clear is
    // honored only from that same author, or once every author has gone
    // quiet past the STEALTH_RESEND_MS safety-resend horizon.
    struct StealthRecv {
        u32 lastAuthor; unsigned long lastActiveMs;
        StealthRecv() : lastAuthor(0), lastActiveMs(0) {}
    };
    std::map<Key, StealthRecv> stealthRecv_;

    // Phase-2 medical channel state.
    // medPub_: per OWNED member, the last SENT quantized fingerprint + send time
    //   (change gate + periodic safety resend, the inventory-snapshot pattern).
    // medRecv_: per DRIVEN body, the last RECEIVED bandage levels + when we last
    //   forwarded a treatment (throttle). The detector compares the copy's LOCAL
    //   bandaging against these: local > received means first aid happened HERE
    //   (the stream can only set it back to the owner's level, so the comparison
    //   has no race). sentBand[] remembers what we already forwarded so an
    //   unacknowledged rise isn't re-sent every tick.
    // limbPrev[]: the last PUBLISHED LimbStates (0xFF = never read) - an edge to
    //   STUMP/CRUSHED authors the reliable EVT_AMPUTATE/EVT_CRUSH transition
    //   (doctrine 16; the packet's limbState[] is the self-heal).
    struct MedPub  {
        u32 hash; unsigned long lastSendMs; unsigned char limbPrev[4];
        MedPub() : hash(0), lastSendMs(0) {
            for (int i = 0; i < 4; ++i) limbPrev[i] = 0xFF;
        }
    };
    // Protocol 16: bandage levels are tracked per ANATOMY PART (12 slots), not
    // per limb - first aid on a head wound forwards too.
    struct MedRecv {
        float recvBand[12]; float sentBand[12]; unsigned long lastFwdMs; bool have;
        MedRecv() : lastFwdMs(0), have(false) {
            for (int i = 0; i < 12; ++i) { recvBand[i] = -1.0f; sentBand[i] = -1.0f; }
        }
    };
    std::map<Key, MedPub>  medPub_;
    std::map<Key, MedRecv> medRecv_;
    u32                    nextTreatId_;
    // Protocol 17: per OWNED member, the last SENT quantized stats fingerprint
    // + send time (the medPub_ pattern; stats creep slowly so the channel is
    // near-silent between level-ups).
    struct StatsPub {
        u32 hash; unsigned long lastSendMs;
        StatsPub() : hash(0), lastSendMs(0) {}
    };
    std::map<Key, StatsPub> statsPub_;
    // Phase 6d: owned send-cadence tunables for the change-gated sampled channels
    // (money + the six registry channels). Default-constructed to today's tuned
    // values; the channel bodies bind their old local SAMPLE_MS/MIN_SEND_MS/
    // RESEND_MS names to these fields, so behavior is unchanged - this is the one
    // place their cadence now lives. See SyncTuning.h.
    SyncTuning tuning_;
    // Protocol 52 shared money pool; protocol 60 (Phase 9 Plan 01, CONS-01)
    // generalizes it to N>=3 - see MoneyFold.h for the arbitration contract.
    // poolSeen_   = the wallet value we last observed/wrote, i.e. the baseline a
    //               local engine move is detected against. -1 = not yet seeded
    //               (fresh session or post-reload), which suppresses the first
    //               sample so a seed is never mistaken for a purchase.
    // poolSent_/poolSentMs_ = host change gate + safety resend for the total.
    // poolSeq_    = join's monotonic delta sequence (last used).
    // poolAcked_  = JOIN-ONLY (protocol 60): this join's own adopted ack,
    //               read from the MoneyPacket::acks[] Entry whose ownerId ==
    //               ctx.localId - NEVER a foreign owner's entry (the at-N
    //               bounce/double-count MoneyFold.h::moneyShouldPop's own doc
    //               comment names). A join has exactly one host, so this
    //               stays a legitimate singular scalar; the HOST no longer
    //               writes it (the host broadcasts the full ack vector from
    //               moneyFold_.processed instead).
    // poolFoldSeen_ = Phase 5 (ID-01) fold-dedup high-water, VESTIGIAL as of
    //               protocol 60: moneyFold_.processed below is now the
    //               single source of truth for the host's per-owner ack
    //               vector (it also covers REJECTED seqs, which
    //               poolFoldSeen_ never tracked). Kept declared (unused by
    //               applyMoneyPool's new logic) rather than deleted, so its
    //               connect-edge purge stays a defensive no-op instead of a
    //               dangling reference - see purgeAuthorConservationState.
    // moneyFold_  = Phase 9 Plan 01 (CONS-01): the HOST's MoneyFoldState
    //               (MoneyFold.h) - the per-owner processed high-water
    //               (folded OR rejected, the ack vector's source), the
    //               overdraft window, and its queued candidates. Absent on a
    //               join (host-only state, harmless default-constructed).
    // poolPending_ = join's unacked deltas, re-applied on top of the host's
    //               total so a just-made purchase does not visibly bounce.
    // poolTotal_  = host's authoritative pool.
    // moneyDeferred_ = HOST-only (Phase 9 review WR-03): deltas drained from
    //               Inbound on a tick where the unseeded pool's wallet read
    //               SEH-failed, retained here for the next tick instead of
    //               being dropped mid-drain (the reliable channel never
    //               redelivers a drained packet; a dropped delta would later
    //               be popped-and-forgotten once the same owner's NEXT delta
    //               advanced moneyFold_.processed past the lost seq -
    //               silently reverting the join's spend while the purchased
    //               goods remain). A Replicator-side member rather than an
    //               Inbound re-push deliberately: re-pushing would append the
    //               OLD deltas behind any NEWER ones the net thread pushed
    //               since the drain, breaking per-owner seq order (the newer
    //               seq would fold first and the older would then read as
    //               MONEY_DUP - the exact loss shape this fixes). Game-thread
    //               only; prepended to the next drain so per-owner order is
    //               preserved.
    struct PoolDelta {
        u32 seq; int delta;
        PoolDelta() : seq(0), delta(0) {}
        PoolDelta(u32 s, int d) : seq(s), delta(d) {}
    };
    int  poolSeen_;
    int  poolSent_;
    unsigned long poolSentMs_;
    int  poolTotal_;
    u32  poolSeq_;
    u32  poolAcked_;
    std::map<u32, u32> poolFoldSeen_;
    coop::MoneyFoldState moneyFold_;
    std::deque<PoolDelta> poolPending_;
    std::deque<InboundMoneyDelta> moneyDeferred_;
    bool moneySync_;
    // Fills pkt's ack vector (ackCount + acks[]) from moneyFold_.processed,
    // bounded MAX_PLAYERS - the host's single source of truth for "highest
    // seq PROCESSED (folded OR rejected)" per connected owner. Shared by
    // publishMoneyPool's periodic/change-gated send and applyMoneyPool's
    // immediate post-fold republish so both sites build the vector
    // identically (ReplicatorChannels.cpp).
    void fillMoneyAcks(MoneyPacket& pkt) const;
    // Protocol 24 faction-relation sync state, per faction sid.
    // known      = our current baseline (seeded on first sight, updated on every
    //              local change we sent and every received row we applied - the
    //              echo guard: an applied row is never re-detected as local).
    // lastSendVal/lastSendMs = change gate + safety resend (rows never sent
    //              never resend, so a settled diplomacy is silent).
    // seqSeen    = newest per-sender seq applied (stale-row guard). Phase 8
    //              review CR-01: keyed PER SENDER (ownerId -> last-accepted
    //              seq), the DoorRow shape - PKT_FACTION is a host-terminated
    //              intent (WORLD-02), so the HOST intake hears up to N-1
    //              joins, each with an independent facSeqOut_ counter; a bare
    //              scalar compared join B's genuinely-newer intent against
    //              join A's higher counter and silently dropped it at N>=3.
    //              On a join this degenerates to one entry (the host's), so
    //              2-player parity is unchanged.
    struct FacRow {
        float known; float lastSendVal; unsigned long lastSendMs;
        std::map<u32, u32> seqSeen; bool seeded;
        FacRow() : known(0), lastSendVal(0), lastSendMs(0), seeded(false) {}
    };
    std::map<std::string, FacRow> facRows_;
    u32           facSeqOut_;
    unsigned long facSampleMs_;
    bool          factionSync_;
    // Protocol 26 door-state sync, per door hand (the faction shape: known =
    // baseline, updated on every local change sent AND every received row
    // applied - the echo guard; lastSendMs = change gate + safety resend;
    // seqSeen = stale-row guard). Phase 8 (WORLD-01): seqSeen is keyed PER
    // SENDER (ownerId -> last-accepted seq), not a bare scalar - doors are a
    // SYMMETRIC channel (any client may author the same hand), so a shared
    // counter would silently drop a second author's genuinely-newer row at
    // N>=3 (gateSeqAcceptPerSender, ChangeGate.h).
    struct DoorRow {
        int knownOpen; int knownLocked; unsigned long lastSendMs;
        std::map<u32, u32> seqSeen; bool seeded;
        DoorRow() : knownOpen(-1), knownLocked(-1), lastSendMs(0),
                    seeded(false) {}
    };
    std::map<Key, DoorRow> doorRows_;
    u32           doorSeqOut_;
    unsigned long doorSampleMs_;
    bool          doorSync_;
    // Protocol 27 placed-building sync. OwnBuild = a building WE placed
    // (registered from the local placement edge; we are its progress
    // authority): lastProg/lastComplete = change gate, lastSendMs = safety
    // resend, doneSent latches the channel silent once the final complete row
    // went out. PeerBuild = a building the PEER placed (learned from
    // PKT_BUILD_PLACE): the key -> local-hand translation entry; minted=0
    // remembers a refused mint so resends don't retry the factory forever;
    // seqSeen = stale STATE-row guard.
    struct OwnBuild {
        unsigned int hand[5];
        float lastProg; int lastComplete; unsigned long lastSendMs;
        bool doneSent;
        bool removed; // dismantled/destroyed + REMOVE announced: silent forever
        // Protocol 30: the PLACE announcement captured at edge-drain time, so
        // a connect-edge resync can re-announce without re-reading the engine
        // (the template sid lives in the edge, not on the Building).
        BuildPlacePacket ann;
        bool haveAnn;
        OwnBuild() : lastProg(-1.0f), lastComplete(-1), lastSendMs(0),
                     doneSent(false), removed(false), haveAnn(false) {
            memset(hand, 0, sizeof(hand));
            memset(&ann, 0, sizeof(ann));
        }
    };
    struct PeerBuild {
        unsigned int localHand[5];
        int minted; u32 seqSeen;
        bool removed; // proxy destroyed on a REMOVE: tombstone (rows skip)
        PeerBuild() : minted(0), seqSeen(0), removed(false) { memset(localHand, 0, sizeof(localHand)); }
    };
    std::map<Key, OwnBuild>  ownBuilds_;
    std::map<Key, PeerBuild> peerBuilds_;
    // Reverse translation (protocol 28): local minted building hand -> the
    // placer's wire key. Lets the door sampler express a MINTED proxy's door
    // in the placer's key space, and the protocol-26 filter recognize placed
    // doors by their parent hand.
    std::map<Key, Key> mintByLocal_;
    // Build-site POSE subject translation (2026-08-01 construction animation).
    // A placed site's hand differs per client, so a streamed BUILD/JOB_BUILDER
    // pose whose subject is that site is meaningless on the peer unless it is
    // carried in the placer's key space. These are the two directions:
    //
    //   buildKeyForLocalHand - PUBLISH: our builder's local subject -> wire key.
    //     Only rewrites a hand we MINTED (mintByLocal_); a site WE placed is
    //     already the key. Returns false when the subject is not a placed site
    //     at all (a save-resident building), which needs no translation.
    //   localHandForBuildKey - APPLY: wire key -> the hand that site has HERE,
    //     whether we placed it (ownBuilds_) or minted it (peerBuilds_).
    bool buildKeyForLocalHand(const Key& local, Key& outWire) const;
    bool localHandForBuildKey(const Key& wire, unsigned int out[5]) const;
    u32           buildSeqOut_;
    unsigned long buildSampleMs_;
    bool          buildSync_;
    // Protocol 28 placed-door rows, keyed by (placer building key, door
    // index) - the protocol-26 DoorRow shape on the translated identity.
    // Phase 8 (WORLD-01): seqSeen is per-sender (ownerId -> last-accepted
    // seq), the same fix as DoorRow above - build-doors stay Class A
    // symmetric, so a bare shared counter has the identical N>=3 collision.
    struct BdoorRow {
        int knownOpen; int knownLocked; unsigned long lastSendMs;
        std::map<u32, u32> seqSeen; bool seeded;
        BdoorRow() : knownOpen(-1), knownLocked(-1), lastSendMs(0),
                     seeded(false) {}
    };
    std::map<std::pair<Key, int>, BdoorRow> bdoorRows_;
    u32           bdoorSeqOut_;
    unsigned long bdoorSampleMs_;
    bool          bdoorSync_;
    // Protocol 29: hunger fold-in enable (rides the medical snapshot).
    bool          hungerSync_;
    // Protocol 33 machine rows, keyed by the WIRE identity (keyKind, key) so
    // a baked hand can never collide with a placer key. HOST: lastSend* =
    // the change gate + safety resend (sent = the row went out at least
    // once - first sight sends, see publishProd). JOIN: seqSeen = stale-row
    // guard (the join never publishes, so the send fields stay idle).
    struct ProdRow {
        int knownPower; int knownState;
        int qOut; int qIn0; int qIn1;          // quantized amounts (x100)
        int qGrown; int qDied; int qGrowStart; int qHarv;
        unsigned long lastSendMs;
        u32 seqSeen; bool sent;
        ProdRow() : knownPower(-2), knownState(-2), qOut(-200), qIn0(-200),
                    qIn1(-200), qGrown(-200), qDied(-200), qGrowStart(-200),
                    qHarv(-200), lastSendMs(0), seqSeen(0), sent(false) {}
    };
    std::map<std::pair<int, Key>, ProdRow> prodRows_;
    u32           prodSeqOut_;
    unsigned long prodSampleMs_;
    bool          prodSync_;
    // Protocol 38 known-research rows, keyed by the RESEARCH stringID (the
    // cross-client-stable wire identity, spike 401). HOST: sent/lastSendMs =
    // first-sight send + safety resend (the authoritative broadcast every
    // join converges against). JOIN: seqSeen = stale-row guard, applied = the
    // local startResearch landed (isKnown flipped) so resends stop
    // re-applying. Phase 8 (WORLD-02): a JOIN also reuses `sent` on the SAME
    // row for its own one-shot outbound intent bookkeeping (own-local-unlock
    // -> host) - `sent`=true means "already accounted for" from EITHER the
    // silent baseline seed OR a genuinely-sent post-baseline intent; a join
    // never resends its own intent (CH_RELIABLE delivery + the host's own
    // resendUnsent=true broadcast is the convergence safety net once
    // committed - see publishResearch).
    struct ResearchRow {
        unsigned long lastSendMs;
        u32  seqSeen;
        bool sent;
        bool applied;
        ResearchRow() : lastSendMs(0), seqSeen(0), sent(false), applied(false) {}
    };
    std::map<std::string, ResearchRow> researchRows_;
    u32           researchSeqOut_;
    unsigned long researchSampleMs_;
    bool          researchSync_;
    // Phase 8 (WORLD-02): one-time gate for a JOIN's silent known-set
    // baseline seed (the facRows_ `seeded` idiom, but a SINGLE flag rather
    // than per-row: research's first publishResearch pass as a join marks
    // EVERY currently-known sid `sent` in one bulk pass WITHOUT queueing
    // anything, so a join never dumps its ~384-sid baseline at connect; only
    // a sid discovered AFTER this one-time seed is a genuine post-baseline
    // unlock worth sending as an intent. Unused on the host (always false).
    bool          researchIntentSeeded_;
    // Protocol 54 property-deed rows, keyed by the building's save-stable hand
    // (both clients loaded the same save, so the key resolves on both - the
    // door/furniture precedent, not the protocol-27 runtime-mint one).
    // knownOwned is the last value we SENT or APPLIED: the change gate for the
    // publisher AND the echo guard for the receiver (-1 = no baseline yet).
    // sent = the row went out at least once; applied = the state write landed,
    // so a resend is a no-op. The row shape is the protocol-38 latched-set one:
    // an unresolvable hand leaves applied false and the resend retries it.
    // EXPLICIT ctor, not value-init: the protocol-47 narrative records that
    // under this toolchain a struct reached through std::map is not reliably
    // zero-initialized, and a garbage latch flag cost a debugging session.
    // Phase 8 review CR-01: seqSeen is keyed PER SENDER (ownerId ->
    // last-accepted seq), the DoorRow/FacRow shape - PKT_DEED is a
    // host-terminated intent (WORLD-02), so the HOST intake hears up to N-1
    // joins with independent deedSeqOut_ counters; a bare scalar dropped a
    // second join's purchase intent for a hand a first join had authored at
    // a higher seq - money debited, deed never recorded (the item/money-loss
    // class). One entry on a join (the host's): 2-player parity unchanged.
    struct DeedRow {
        unsigned long lastSendMs;
        std::map<u32, u32> seqSeen;
        bool sent;
        bool applied;
        int  knownOwned;
        DeedRow() : lastSendMs(0), sent(false), applied(false),
                    knownOwned(-1) {}
    };
    std::map<Key, DeedRow> deedRows_;
    u32           deedSeqOut_;
    unsigned long deedSampleMs_;
    unsigned long deedAuditMs_; // last wide-state dump (owner-vs-peer diff)
    bool          deedSync_;
    // Protocol 55 runtime-fixture identity. A mine is instantiated per client
    // for a terrain node, so the same physical fixture has a different hand on
    // each side; without a pairing the pose inject resolves nothing (the miner
    // parks with no animation) and every prod/container row keyed to it is
    // dropped. Two halves:
    //
    //   fixtureOut_  - PUBLISH bookkeeping: which of OUR fixtures we have
    //     announced and when, so the channel is first-sight + safety resend
    //     rather than a per-sample broadcast.
    //   fixtureMap_  - APPLY result: the PEER's key -> the hand that fixture has
    //     HERE. Read through localHandForFixtureKey by the pose inject and by
    //     the prod/container channels. Absent or unresolved means "no pairing
    //     known", and every caller then falls back to the raw wire hand, which
    //     is still correct for a save-baked machine.
    struct FixOut {
        unsigned long lastSendMs; bool sent;
        FixOut() : lastSendMs(0), sent(false) {}
    };
    struct FixtureRow {
        unsigned int hand[5]; bool resolved; u32 seqSeen; unsigned long lastMissMs;
        FixtureRow() : resolved(false), seqSeen(0), lastMissMs(0) {
            hand[0] = hand[1] = hand[2] = hand[3] = hand[4] = 0;
        }
    };
    std::map<Key, FixOut>     fixtureOut_;
    std::map<Key, FixtureRow> fixtureMap_;
    u32           fixtureSeqOut_;
    unsigned long fixtureSampleMs_;
    bool          fixtureSync_;
    // Translate a peer fixture key to the local hand for the same object.
    // False when we have no pairing (yet) - the caller keeps the raw hand.
    bool localHandForFixtureKey(const Key& wire, unsigned int out[5]) const;
    // Release the taskBad park on driven bodies after a new pairing lands, so a
    // pose that failed while the fixture was unpaired gets one more chance.
    void unparkForNewFixture();
    // The LOCAL hand to hand the engine for a container key that came off the
    // wire. Identity for anything save-baked (a squad member's own hand, a
    // chest); the protocol-55 pairing for a fixture whose hand is client-local,
    // where using the key verbatim reads and writes nothing. Keys WE author are
    // already local and are never translated, so an unlucky collision between
    // our hand for one fixture and the peer's for another cannot bite.
    void handForContainerKey(const Key& k, unsigned int out[5]) const {
        if (ownedContainers_.count(k) == 0 && ownHands_.count(k) == 0 &&
            localHandForFixtureKey(k, out))
            return;
        out[0] = k.t; out[1] = k.c; out[2] = k.cs; out[3] = k.i; out[4] = k.s;
    }
    // Protocol 23 recruitment sync state.
    bool recruitSync_;
    // Ownership PINS (protocols 23 + 35): per-hand overrides layered on the
    // tab-rank partition. pinOwned_ = hands WE authored (our recruits, our
    // squad moves): publishOwned streams them regardless of which tab rank
    // their container maps to. pinPeer_ = hands the PEER authored (learned
    // from EVT_RECRUIT / EVT_SQUAD_MOVE): never publish them even if a local
    // census would rank them into a tab we own. This is how an appended
    // (mid-session) tab inherits its authoring side's ownership.
    std::set<Key> pinOwned_;
    std::set<Key> pinPeer_;
    // 06-01 (GAP-3/PLAY-03): ADDITIVE author record for pinPeer_ - written
    // alongside every pinPeer_ insert/erase via PinOwner.h's pinRecord/
    // pinForget, read ONLY by clearPeerReplicationState's owner-scoped pin
    // erase. pinPeer_ itself remains the sole authoritative membership/veto
    // set (ReplicatorPublish.cpp:94 and friends are completely unchanged) -
    // this map never influences a publish/apply decision, only which pins a
    // departing peer's disconnect is allowed to release. pinOwned_ (hands WE
    // authored) deliberately has no equivalent map: it is always "me", so an
    // author field would be redundant (06-RESEARCH open question 4).
    std::map<Key, u32> pinnedOwner_;
    // Roster edges WE caused. insertPeerMember re-containers a peer's body into
    // our squad so we can drive it, and the engine cannot tell that apart from
    // the user dragging a member between tabs - so publishSquadMoves echoed the
    // insertion back as OUR move and pinned the peer's character owned.
    // Measured 2026-08-03: the host inserted the join's character, published the
    // insertion, and the join then pinned its own character peer-owned, drove it
    // from our stream, vetoed its death mid-tick and faulted the engine twenty
    // times in 230 ms. Keyed on the character SERIAL - the one hand field a
    // re-container preserves - and consumed on the first matching edge.
    std::map<u32, unsigned long> moveEcho_;
    // Hands we published as having LEFT our squad. The exit clears the pins and
    // captureSquad stops returning the body, so both drive guards (ownHands_,
    // pinOwned_) go quiet at once and the peer's in-flight tail for that hand
    // can start driving our own just-dead body - which then un-kills it.
    std::map<Key, unsigned long> exitedOwn_;
    enum {
        SQUAD_ECHO_MS      = 2000,  // an insertion echoes on the very next poll
        SQUAD_EXIT_GRACE_MS = 30000 // matches the targets_ prune
    };
    // Protocol 35 squad management sync state. tabRank_ is the container ->
    // rank LATCH: seeded from the first census in sorted order (identical to
    // the legacy ranking at session start), then newly-seen containers append
    // - a mid-session tab cannot reshuffle existing ranks. Cleared on session
    // reset (the reloaded save re-seeds it). rekeyedOld_ stamps hands a
    // re-key RETIRED: their stale stream tail must never author a spawn REQ
    // or accept a mint (the duplicate-proxy race, run 192211).
    bool          squadSync_;
    std::map<std::pair<u32, u32>, unsigned int> tabRank_;
    std::map<Key, unsigned long> rekeyedOld_;
    // Per-TAB ownership verdict, keyed on the container hand and decided ONCE
    // when the tab is first seen. The rank latch above stops ranks reshuffling,
    // but ownership was still read as "is my rank in ownRanks_", and ownRanks_
    // only ever holds 0 (host) or 1 (join) - so a tab appended mid-session got
    // rank 2+ and belonged to NOBODY. Measured 2026-08-03: the join latched
    // rank 2 and rank 3, published neither, and was left with no controllable
    // character. The per-hand pins were supposed to cover this ("an appended tab
    // inherits its authoring side's ownership"), but a pin is per HAND and an
    // EVT_SQUAD_MOVE exit CLEARS it - after which the body fell back to the
    // rank rule and was orphaned. A verdict on the CONTAINER outlives the pins.
    std::map<std::pair<u32, u32>, bool> tabOwned_;
    // Number of tabs present at session-start seeding, i.e. the length of the
    // rank prefix that means the same thing on both clients. Ranks past it are
    // assigned in local first-seen order and DIVERGE (measured: host rank 2 =
    // container 237, join rank 2 = container 443), so rank-keyed WIRE channels
    // must stay inside the prefix even though ownership no longer needs it.
    unsigned int  tabsSeeded_;
    // Phase 3 Plan 03: set true whenever decideTabs() assigns a NEW tab
    // ownership verdict (session-start seeding or a later dynamic tab) - the
    // per-tick signal publishOwned (ReplicatorPublish.cpp) checks to trigger
    // announceOwnRanks() on the host, without re-announcing every tick when
    // nothing changed. Consumed (reset false) the same tick it fires.
    bool          tabsChanged_;
    // Phase 3 Plan 03: the connected non-host roster (Plugin.cpp's
    // notePeerConnected/notePeerLeft), used by announceOwnRanks() to build the
    // authoritative map's base rank=playerId entries. Host-only in practice
    // (unused on a join, since announceOwnRanks no-ops there).
    std::set<u32> knownPeers_;
    // Fold this tick's distinct sorted containers into the latch (append-only;
    // no-op when squadSync_ is off) and rank one container: latched rank when
    // the gate is on, the legacy per-tick sorted rank otherwise.
    void latchTabs(const std::vector<std::pair<u32, u32> >& ctnrs);
    unsigned int tabRankFor(const std::pair<u32, u32>& key,
                            const std::vector<std::pair<u32, u32> >& ctnrs) const;
    // Decide (once per container) who owns each tab in this tick's census.
    // Session-start tabs use the historical rank rule so the standard two-tab
    // save behaves exactly as before; a tab that appears LATER is attributed to
    // whichever side authored the bodies in it, and falls back to the host so
    // that no tab is ever ownerless.
    void decideTabs(const EntityState* raw, unsigned int nSquad,
                    const std::vector<std::pair<u32, u32> >& ctnrs);
    bool ownsTab(const std::pair<u32, u32>& key, unsigned int rank) const;
    // Is this rank the same tab on both clients? Only true inside the seeded
    // prefix (see tabsSeeded_).
    bool rankIsShared(unsigned int rank) const {
        return tabsSeeded_ != 0 && rank < tabsSeeded_;
    }
    // Replicator carries no role flag - the role IS the ownership partition
    // Plugin.cpp installs (host {0}, join {1}), so owning rank 0 is being host.
    bool isHostRole() const {
        return ownRanks_.empty() || ownRanks_.count(0u) != 0;
    }
    // Shared EVT_RECRUIT / EVT_SQUAD_MOVE receive half: pin the new hand as
    // peer-authored and re-key our local copy of the old hand onto it in
    // proxyByKey_ (restoring it first if host-authority had suppressed it).
    // tag selects the log prefix ("recruit" / "squad"). ownerId (06-01
    // GAP-3): the authenticated event author (already forgery-checked by
    // rejectIfForgedOwner on the relay) - recorded into pinnedOwner_
    // alongside every pinPeer_ insert this function performs.
    void rekeyPeerBody(GameWorld* gw, const Key& oldK, const Key& newK,
                       const char* tag, u32 ownerId);
    // Phase 1b (cross-game recruit membership): insert the re-keyed body 'c'
    // into THIS client's player squad at the tab named by newK's container, so a
    // recruit/transfer shows in the panel on the PEER too. ownIt selects the
    // ownership pin for the body's ACTUAL local hand: false (default) pins it
    // PEER-owned so control stays with the author (fresh recruit / move into the
    // peer's tab); true pins it OWNED so THIS client controls + streams it (a
    // transfer INTO a tab we own - the control hand-off). Idempotent + tab-aware
    // (a squad-move re-containers an existing member). Shared by the recruit ok=1
    // path, the ok=0 post-mint drain, and the control-flip transfer. ownerId
    // (06-01 GAP-3): the pin's author, recorded into pinnedOwner_ alongside
    // any pinPeer_ insert performed here.
    void insertPeerMember(GameWorld* gw, Character* c, const Key& newK,
                          const char* tag, bool ownIt, u32 ownerId);
    // Hard-snap attribution diagnostics (rubber-banding investigation): one
    // throttled [snap] line per applyRaw teleport with everything needed to
    // classify the cause (gap, source speed+velocity, game speed, slew,
    // whether a walk destination was live). ~4 lines/s max; skipped snaps are
    // counted into the next line.
    void logHardSnap(Character* c, const EntityState& out, const char* kind,
                     float gap, float srcVel, float gate, bool hadDest);

    // ---- Phase 3: unified entity lifecycle (join side) ---------------------
    // One explicit, logged state per hand replacing the implicit union of
    // suppressed_/proxyByKey_/drivenChars_/censusHands_ membership tests as
    // the AUDIT view of an entity's journey. The mechanics stay where they
    // were validated (authority passes, mint pipeline, drive tiers); every
    // decision point REPORTS its outcome here, so the log tells one coherent
    // story per hand ("LIFE hand=.. from=.. to=.. reason=..") and the
    // lifecycle oracle can gate on illegal journeys (e.g. a hand stuck in
    // DISCOVERED while mintable, or a body driven while culled).
    enum LifeState {
        LIFE_UNKNOWN = 0,   // never judged (or pruned after leaving interest)
        LIFE_DISCOVERED,    // known to exist (census) but NO local body yet
        LIFE_RESOLVED,      // local body bound (baked resolve / minted proxy),
                            //   not driven this tick
        LIFE_HI,            // driven at near-tier (20 Hz) cadence; PCs live
                            //   here permanently (Phase 3 unification)
        LIFE_MID,           // driven at mid-tier (round-robin) cadence
        LIFE_PARKED,        // local-AI copy under census existence (park
                            //   fallback owns divergence repair)
        LIFE_CULLED         // suppressed (hidden + frozen; census-absent ghost)
    };
    struct Lifecycle {
        u8            state;
        unsigned long sinceMs;   // when the current state was entered
        unsigned long touchMs;   // last confirmation (stale-entry pruning)
        unsigned long stuckLogMs;// last STUCK self-audit line for this hand
        Lifecycle() : state(LIFE_UNKNOWN), sinceMs(0), touchMs(0),
                      stuckLogMs(0) {}
    };
    std::map<Key, Lifecycle> life_;
    static const char* lifeName(int s);
    // Record a state observation for a hand: logs one LIFE line on CHANGE,
    // refreshes touchMs always. Returns the previous state. The UNKNOWN ->
    // PARKED edge is recorded silently (every wilderness NPC's boring birth
    // would otherwise burst hundreds of lines at session start).
    int lifeSet(const Key& k, int to, const char* reason);
    // ~5 s sweep: drop entries not touched for a while (left interest) and
    // self-audit DISCOVERED entries older than STUCK_MS whose census position
    // sits in a locally LOADED zone (mintable but never minted - the
    // "join never sees the raid the host is fighting" failure the lifecycle
    // oracle gates on; an unloaded far zone is a legitimate indefinite defer).
    void lifeSweep(GameWorld* gw, unsigned long now);
    unsigned long lifeSweepMs_;

    // Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS=1, spike-47 substrate):
    // pin a colored label to each judged body so authority states are visible
    // live - green DRV = host-driven, red HID = suppressed/culled, yellow
    // LOC = local-sim copy that exists in the host census. No-op (single env
    // check) unless the flag is set. Labels are created once per body and
    // re-captioned only on state change.
    // hand carries the identity the KEY cannot: the engine frees Characters and
    // recycles the addresses, so an entry found at a given Character* may belong
    // to an entirely different body, and re-captioning it would drive the
    // previous occupant's label. index/serial name the body the label was made
    // for, and a mismatch means start over.
    struct DebugMarker { void* label; int color; u32 index; u32 serial; };
    std::map<Character*, DebugMarker> debugMarkers_;
    void debugMark(Character* c, int colorId, const char* tag);
    // Lifetime guard (2026-07-11 join crash): the map is keyed by raw
    // Character* the engine can free (and REUSE for a new body, silently
    // stealing the old label). enforceHostAuthority prunes entries whose
    // pointer wasn't vouched live this pass (enumerations / driven / proxy /
    // validated suppressed).
    //
    // Pruning alone is NOT sufficient and 2026-08-03 proved it: it runs on the
    // slow authority cadence, so between a body despawning and the next pass the
    // stale entry sits there as bait for the next allocation at that address -
    // which a mint burst supplies within milliseconds. debugMark therefore checks
    // BOTH halves per use (hand identity here, engine::markerAlive for the label
    // itself, since the label is the GUI's object to destroy, not ours).
    void pruneDebugMarkers(const std::set<Character*>& live);
    // world_parity roster rows: one "SCENARIO WNPC" line for a body, with the
    // task/pelvis/mv/carry parity fields appended (world_parity oracle) after
    // the legacy hand/pos/cls/name schema (travel_parity's parser ignores the
    // extras). cls=pc rows carry the player characters - the class the legacy
    // dumps EXCLUDED, which is how a diverged host-PC position stayed
    // invisible to every automated gate. carry= marks a PASSENGER, whose
    // position is owned by its carrier on both clients and therefore says
    // nothing about its own tracking (see the emitWnpcRow comment). Shared by
    // the host (Publish) and join (Authority) dump sites.
    void emitWnpcRow(Character* c, const EntityState& st, const char* cls);
    // PLATOON FIRST-SIGHT probe (2026-08-02 split_far root cause). The two
    // clients diverge at platoon granularity, not per body: the join's set of
    // hand containers is a strict SUPERSET of the host's (runs 123408/124423:
    // +12 and +7 platoons, none the other way), which at a settlement reads as
    // a duplicate of every role - two Barmen where there is one bar. What that
    // evidence cannot say is WHEN a platoon arrives, and the two explanations
    // need opposite fixes: a platoon present from t=0 on the join alone means
    // the HOST never instantiated it, while one appearing mid-run means the
    // JOIN spawned it live. Logs one line per (container, containerSerial) the
    // first time this client enumerates it, with the elapsed time, the place,
    // and the distance to the nearest interest anchor. Called from both sides'
    // enumeration so the two logs can be diffed directly.
    void notePlatoons(GameWorld* gw, const EntityState* sts, unsigned int n,
                      const char* side);
    std::set<std::pair<unsigned int, unsigned int> > seenPlatoons_;
    unsigned long platoonT0_;   // first notePlatoons call - the elapsed base
    // world_parity PC rows: capture every playerCharacter (both tabs) and
    // emit each as a cls=pc WNPC row. Called inside the auditRows_ dumps on
    // both sides.
    void emitPcRows(GameWorld* gw);
    // v38 census position parking: snap a census-present unstreamed local
    // copy back onto the host's census position once it diverges past
    // censusParkDist_. Called from both authority passes with this tick's
    // live enumeration pointer. Returns the measured drift (u) from the host's
    // census position, or -1 if unknown (no census row / parking disabled) -
    // the census-band AI-freeze uses it to decide when a body is diverging.
    float parkDivergedCopy(Character* c, const EntityState& st, const Key& k);
    // The same reconciliation for a MINTED PROXY, whose census row is keyed by
    // the stream key that minted it rather than by its local hand. Both
    // authority passes exempt proxies from existence judgment; without this
    // they were exempt from position correction as well, and a proxy the host
    // has stopped streaming answers to nothing at all.
    void reconcileProxy(Character* c, const EntityState& st,
                        const std::map<Character*, Key>& keyOf);
    // Phase C capability veto: filter an interest-anchor set down to the
    // anchors this client can actually speak for. An anchor whose zone the
    // LOCAL engine has not loaded (or is still streaming in) is dropped, so
    // the region around it reads as dormant here - the host publishes no
    // census over it and the join suppresses nothing in it. Existence
    // authority over a block your own engine has not materialised is a claim
    // made out of ignorance, and the census carries it as fact.
    //
    // Both clients apply this to the SAME anchor set, so like the attention
    // radius it is evaluated near-identically on each side. If every anchor
    // is vetoed the result is empty and observedAt falls open, which is
    // today's behaviour - the degradation is toward doing what we did before,
    // not toward silence.
    //
    // Writes up to four centers into out[12]; returns the count.
    unsigned int attentionAnchors(GameWorld* gw, const float* raw,
                                  unsigned int nRaw, float* out);
    // Attention gate predicate: is anyone watching (x,y,z)? True if any
    // interest anchor is within attentionRadius_. Anchors come from the
    // caller because they are computed once per tick (engine::interestAnchors
    // writes up to four centers into a float[12]).
    //
    // Hysteresis is asymmetric and latched per hand in attnObs_: a body
    // becomes observed at attentionRadius_ but stays observed out to
    // ATTN_LEAVE_SCALE times that. The two clients evaluate this from anchor
    // sets that are only near-identical (squads move between ticks, camera
    // hints arrive up to a second apart and expire after three), and a body
    // sitting on the threshold would otherwise flip dormant/observed out of
    // phase between them - which is the one state this design cannot hold,
    // since it leaves the join waiting on a census the host stopped writing.
    //
    // attentionRadius_ <= 0 returns true for everything: gate off.
    bool observedAt(const Key& k, const float* anchors, unsigned int nAnchor,
                    float x, float y, float z);
    // The same predicate asked about the PEER rather than about us, over
    // peerAnchors() and its own latch. A census row exists for the peer's
    // benefit, so whether to write one is the peer's attention question - our
    // own verdict is not evidence about what they can see.
    bool observedByPeer(const Key& k, const float* anchors, unsigned int nAnchor,
                        float x, float y, float z);
    bool observedIn(std::map<Key, bool>& obs, bool countFlips, const Key& k,
                    const float* anchors, unsigned int nAnchor,
                    float x, float y, float z);
    // KENSHICOOP_DEBUG_CENSUS=1 existence-decision trace (Phase 12 plan 02,
    // DIAGNOSTIC ONLY - reads state, decides nothing). Prints the INPUTS of an
    // existence verdict, not just its outcome: which census owners were
    // consulted, whether any of them vouched for this key, how fresh their
    // slices were, how far the CONSULTED attention anchor set actually reached
    // toward the body, and what radius that distance was compared against.
    // Behind the same read-once static env gate publishNpcCensus' per-row dump
    // uses, and hard-capped per verdict per window so an enabled gate cannot
    // turn this per-tick loop into a log flood (T-12-05).
    //
    // `verdict` is one of the greppable words DECIDE (judged, cull skipped),
    // DROP (culled) or KEEP (host vouches, body kept); `pass` is near or wide.
    void logCensusDecision(const char* verdict, const char* pass,
                           const Key& k, Character* c, const EntityState& st,
                           bool censusFresh, bool streamed, bool driven,
                           bool exists, bool observed,
                           const float* attnAnch, unsigned int nAttnAnch,
                           unsigned int nRawAnch, unsigned int unstreak,
                           unsigned int suppressAfter);
    // Where the PEER is: one center per squad tab we do NOT own, plus their
    // camera hint. No zone veto - the veto asks "can I enumerate here", and by
    // the time this is consulted we have already enumerated the body. Writes up
    // to four centers into out[12]; returns the count. Zero means we know
    // nothing about the peer, and every caller must fail OPEN on that.
    unsigned int peerAnchors(GameWorld* gw, float* out);
public:
    // Presence authority (protocol 49). Publish a claim for each cell our own
    // tabs stand in and drain the peer's, both at ~1 Hz on the reliable
    // channel. No-op with cellAuth_ off, so nothing is on the wire until the
    // feature is armed.
    void syncCellClaims(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId);
    // HOST ONLY (protocol 59, WORLD-03): run CellMap.h's engine-free
    // reduceCellMap over claimSlots_/the connected-owner set, write the
    // verdict into claimedCells_/cellLastOwner_ directly (the host never
    // receives its own broadcast back - the queueClaimVerdict/
    // processXferIntents precedent), and broadcast ONE CellMapPacket on
    // change or a slow re-assert cadence. No-op (and clears state) with
    // cellAuth_ off or on a join - mirrors syncCellClaims' own early-return
    // shape. localId is passed so the host can tell it IS the host without
    // an isHostRole() call that depends on ownRanks_ having been set yet.
    void computeAndBroadcastCellMap(GameWorld* gw, NetLink& net, u32 localId);
    // ALL clients (host included, where the queue is always empty): drain
    // any received PKT_CELL_MAP and adopt it WHOLESALE into claimedCells_/
    // cellLastOwner_ - a join's own claimSlots_-derived reduce is never run;
    // the host's map is the SOLE verdict (a cell absent from it fail-opens
    // to host, never to the local sender's own optimistic claim).
    void applyCellMap(GameWorld* gw, Inbound& in, u32 localId);
    void setCellAuth(bool on) { cellAuth_ = on; }
    void setCellCollapse(bool on) { cellCollapse_ = on; }
    // Defeat the 1x-during-combat leg of speed arbitration. See Config::
    // speedCombatCap: run_apart's subject is distance, and the cap turned its
    // crossing of bandit country into a five-fold slower run for no benefit,
    // since the squad is fleeing the fight rather than resolving it.
    void setSpeedCombatCap(bool on) { speedCombatCap_ = on; }
    // Who authors the region containing (x,z)? The claimant, or the HOST when
    // the feature is off, the cell is unclaimed, or the mapping is unavailable.
    // Never returns "nobody" - an unowned region is the case this exists to
    // eliminate.
    u32 authorityFor(GameWorld* gw, float x, float z) const;
    // authorityFor with its reasoning exposed (KENSHICOOP_DEBUG_CENSUS): a
    // client publishing rows for cells it does not stand in is either
    // exercising the deliberate vacated-cell rule or misattributing, and the
    // verdict alone cannot tell those apart. Same answer as authorityFor, which
    // is a thin wrapper over it, plus the cell and one of AUTHSRC_*.
    enum {
        AUTHSRC_NOMAP  = 0,   // no cell mapping - fail-open to host
        AUTHSRC_CLAIM  = 1,   // a tab is standing there now
        AUTHSRC_VACATE = 2,   // cellLastOwner_: claimed once, since left
        AUTHSRC_OPEN   = 3,   // never claimed by anyone - fail-open to host
        AUTHSRC_COLLAPSE = 4, // RETIRED (Task 2, WORLD-03): authoritySrc never
                              // sets this anymore - a co-located cell now
                              // resolves to host via the ordinary AUTHSRC_CLAIM
                              // path (CellMap.h's host-if-party rule). Kept as
                              // a dead enum value only so AUTHSRC_N and the
                              // [census] auth dump's bySrc[] array stay stable.
        AUTHSRC_N      = 5
    };
    u32 authoritySrc(GameWorld* gw, float x, float z,
                     int* cx, int* cz, int* src) const;
    // Do WE author it? localId comes from the caller because the Replicator has
    // no NetLink handle of its own.
    bool weAuthor(GameWorld* gw, u32 localId, float x, float z) const {
        return authorityFor(gw, x, z) == localId;
    }
    // Is the PEER currently writing this hand to us? A fresh entry in targets_
    // is the strongest available statement that they consider the body theirs -
    // stronger than a census row, because it is the 20 Hz channel that actually
    // drives the body rather than the 1 Hz one that merely lists it.
    bool peerStreamFresh(const Key& k, unsigned long now) const {
        std::map<Key, Driven>::const_iterator it = targets_.find(k);
        if (it == targets_.end() || it->second.lastSeenMs == 0) return false;
        return (now - it->second.lastSeenMs) <= (unsigned long)PEER_STREAM_FRESH_MS;
    }
private:
    // rebuildClaimedCells + claimsCoLocated (the per-instance std::map-
    // iteration-order reduce + co-location collapse heuristic) are RETIRED
    // as of Task 2 (protocol 59, WORLD-03) - superseded by CellMap.h's
    // host-side reduceCellMap, computeAndBroadcastCellMap, and applyCellMap.
    // Shared [cell] MAP log-dump helper (protocol 49, extended protocol 59
    // WORLD-03 with a src= provenance token): src is "local" (the host's own
    // compute) or "host" (a client's adopted map) - appended AFTER the
    // existing tokens so Get-CellMap's tail parser (which takes the line's
    // tail and scans it for 'x,z=owner' triples) is unaffected.
    void logCellMapDump(const char* src) const;
    // Should enforcement leave this body alone? True when we author its cell,
    // or when its author has sent us no census to judge against. Restores the
    // body first if a previous author's verdict had it hidden. Always false
    // with cellAuth_ off, which is what keeps the pass bit-identical.
    bool authorHoldsBody(GameWorld* gw, u32 localId, const Key& k,
                         Character* c, float x, float z);
    // Drop attnObs_ latches for hands this tick's enumeration no longer sees,
    // so a long session's map cannot grow without bound.
    void pruneAttention(const std::set<Key>& seen);
    // Phase B: close the attach window if it is due and log what the burst
    // cost. Called once per authority tick.
    void measureAttach(unsigned long now);
    // Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI): suspend the local AI
    // of a census-band body whose drift crossed censusParkDist_, held ~5 s past
    // the last over-threshold tick so the position park can't oscillate it back
    // into fleeing. Divergence-gated: leaves well-tracking census NPCs alone.
    void censusFreezeDivergedAi(Character* c, const Key& k, float drift);
    // Phase B: combat-scoped world-NPC vitals (host side). Keys of streamed
    // NPCs that are fighting / being fought / down, with last-qualified time;
    // publishMedical streams their vitals at ~1 Hz while fresh. The join's
    // driven copies are damage-guarded, so the write has nothing local to
    // fight. Also the authority set for TREATMENTS on world NPCs (a join medic
    // bandaging a downed host NPC forwards here).
    std::map<Key, unsigned long> medNpc_;

    // Consensus game-speed sync state. Requests and applied values use ONE
    // number: the multiplier, with 0 meaning paused (min() then gives "either
    // can pause, both must raise"). -1 = not yet known.
    float         speedLastApplied_;   // what WE last wrote (own-write vs user-click detector)
    float         speedMyReq_;         // this client's current request
    // Phase 9 Plan 02 (CONS-02): the OLD speedPeerReq_/speedPeerCombat_ bare
    // scalars (last-writer-wins across every join at N>=3, the documented
    // Phase 4 deferral) are replaced by a per-owner vote map (SpeedVote.h) -
    // HOST-only, empty on a join. speedReduce() is the pure min-vote reduce
    // over this map; a disconnect's speedVotes_.erase(departing)
    // (ReplicatorCore.cpp clearPeerReplicationState) plus the next
    // arbitration tick's reduce() IS the "instant vote drop" - no separate
    // disconnect-branch logic needed here.
    std::map<u32, coop::SpeedVoteRec> speedVotes_;
    bool          speedCombatCap_;     // false = do NOT pin to 1x while fighting
    bool          speedMyCombat_;      // own-squad in-combat flag (~1 Hz sample)
    float         speedLastSet_;       // host: last broadcast effective; join: last received
    u32           speedSeqOut_;        // per-sender monotonic seq for REQ/SET we send
    // JOIN-only now (Phase 9 Plan 02, CONS-02): a join hears only the host's
    // single SET stream, so the pre-split scalar stale guard stays exactly
    // as it was. The HOST's per-sender guard lives inside each
    // SpeedVoteRec.seqSeen (speedVotes_) instead - see the drain loop in
    // syncSpeed.
    u32           speedSeqSeen_;
    unsigned long speedLastSendMs_;    // last REQ (join) / SET (host) send, safety resend
    unsigned long speedCombatSampleMs_;// last own-combat sample time
    unsigned long speedCombatHoldMs_;  // last time own-squad combat read TRUE (cap hysteresis)

    // Protocol 25 game-clock sync state. timeSlew_ is the join's correction
    // multiplier (1.0 = no correction); the speed layer applies effective *
    // timeSlew_ in its quiet writes so the slew and the consensus compose.
    bool          timeSync_;
    bool          timeBrake_;         // host may slow itself for a lagging join
    float         timeSlew_;          // local slew: join >= 1 (catch up),
                                      //   host <= 1 (be caught)
    u32           timeSeqOut_;        // monotonic seq for the PKT_TIME we send
    // JOIN-only now (Phase 9 Plan 02, CONS-03): a join hears only the host's
    // single broadcast stream, so the pre-split scalar stays exactly as it
    // was. The HOST's per-owner guard lives inside each TimeReport.seqSeen
    // (timeReports_) instead - see the drain loop in syncTime.
    u32           timeSeqSeen_;
    // Phase 9 Plan 02 (CONS-03): the OLD single timeSeqSeen_ scalar (used for
    // BOTH the host's incoming-report guard and the join's incoming-broadcast
    // guard) collided across join reports at N>=3 - whichever join's counter
    // was highest silently censored the others', and the brake (below) then
    // saw only the LAST accepted report (`jn`) instead of the MOST-BEHIND
    // connected join. This per-owner map replaces the host's use of that
    // scalar: each reporting join gets its own {gameHours, recvMs, seqSeen}
    // record, and the brake computes lag against the WORST (min-gameHours)
    // entry. HOST-only; empty on a join. A disconnect's
    // timeReports_.erase(departing) (ReplicatorCore.cpp
    // clearPeerReplicationState) stops a departed laggard from braking the
    // host forever - the same fix shape as speedVotes_.
    struct TimeReport {
        double        gameHours;
        unsigned long recvMs;
        u32           seqSeen;
        TimeReport() : gameHours(-1.0), recvMs(0), seqSeen(0) {}
    };
    std::map<u32, TimeReport> timeReports_;
    unsigned long timeLastSendMs_;    // last clock broadcast/report
    unsigned long timeLastLogMs_;     // offset/brake log throttle
    // The engine speed the slewed value was derived from; lets the enforcement
    // distinguish "our slewed write" from a real user click.
    float         timeSlewApplied_;   // join: last effective*slew written (-1 = none)
    // The consensus effective with the clock slew folded in (what the quiet
    // writes actually drive; clamped to the engine's sane speed range).
    float slewedEffective(float eff) const {
        if (eff <= 0.01f) return eff; // paused: never slewed
        float s = eff * timeSlew_;
        if (s > 5.0f)  s = 5.0f;
        if (s < 0.05f) s = 0.05f;
        return s;
    }

    // Protocol 21 runtime-spawn proxy replication state.
    bool spawnSync_;
    // JOIN: streamed hand -> locally-minted proxy body. Checked at the
    // applyTargets resolve choke point when engine::resolve fails, so a bound
    // proxy inherits the ENTIRE world-NPC drive path (AI-suspend, damage
    // guard, combat rendering, down/death latches). The proxy's own LOCAL
    // hand never matches the streamed key; enforceHostAuthority recognises it
    // by pointer via drivenChars_ (hide on stale stream / restore on return
    // come free from the existing hysteresis).
    std::map<Key, Character*> proxyByKey_;

    // Bodies THIS client created (successful mints only). Bound proxies come from
    // two places now - minted, or ADOPTED from the population we generated
    // ourselves - and every teardown that destroys a proxy must tell them apart:
    // destroying an adopted body deletes one of our own townspeople, and the
    // peer-left path would have deleted a whole town's worth at once. Keyed by
    // pointer rather than by stream key deliberately: re-key migrations move keys
    // between bodies, but "did we create this" is a property of the body.
    std::set<Character*> mintedBodies_;

    // Destroy 'c' only if we minted it, and say whether we did. An adopted body is
    // left standing and merely unbound - the local engine owns it again, and its
    // AI resumes on the next tick that does not drive it.
    bool destroyIfMinted(GameWorld* gw, Character* c);
    // Lifetime guard (2026-07-11 join crash): the ENGINE owns the proxy body
    // and can despawn it (zone streaming, cleanup) while proxyByKey_ still
    // holds the pointer - every later touch would be a use-after-free.
    // syncSpawns runs a ~1 s liveness sweep: SEH-read each pointer's current
    // hand and resolve it back; anything but the same pointer unbinds the
    // entry untouched.
    // JOIN: per-hand request state - debounce, retry cap, negative-reply
    // backoff (deniedMs = when the host said "can't resolve either" or the
    // local proxy spawn failed; retried only after a long cooldown).
    struct SpawnReqState {
        unsigned long lastSendMs; unsigned int sends; unsigned long deniedMs;
        // Census-mint far-deferral (2026-07-11): the host's reply resolved the
        // hand but its position was outside the mint radius. NOT a denial -
        // the NPC may be walking toward us - so re-ask on a short cadence
        // instead of the long deniedMs cooldown, and reset the send cap so a
        // slow approach can't exhaust it.
        unsigned long farMs;
        // Phase 1 spawn parity: the REQ came from the census-missing scan (the
        // hand is host-census-vouched). Census-sourced replies may FAR-MINT
        // beyond the radius gate when the reply position sits in a locally
        // LOADED zone (an unresolvable hand there is a genuine runtime spawn).
        bool fromCensus;
        // First time the census-missing scan saw this hand unresolvable. The
        // far mint requires a sustained miss (FAR_MINT_ARM_MS): a zone can
        // report loaded a few seconds before its baked bodies resolve, and
        // minting inside that window stands a short-lived double (run
        // 20260712_092921: 8/12 far mints DUPE-HEALed within 3 s). Runtime
        // spawns never resolve locally, so they pass the dwell unharmed.
        unsigned long firstMissMs;
        // Phase 1b: this REQ fulfills a force-REQ recruit/move (rekeyPeerBody
        // ok=0). It rides the fromCensus proximity bypass, but a reliable
        // EVT_RECRUIT PROVES it is a distinct host body, so it must SKIP the
        // census same-template dupe guard (which is for UNcorrelated census
        // hands). Without this the runtime recruit defers forever behind a
        // same-template twin (recruit_sync run 094401).
        bool forceReq;
        SpawnReqState() : lastSendMs(0), sends(0), deniedMs(0), farMs(0),
                          fromCensus(false), firstMissMs(0), forceReq(false) {}
    };
    std::map<Key, SpawnReqState> spawnReq_;
    // JOIN: hands applyTargets failed to resolve this tick, with the streamed
    // position (the request-authoring queue; proximity-gated in syncSpawns so
    // a far unloaded BAKED zone doesn't breed duplicate proxies). fromCensus
    // entries came from the census-missing scan instead of the stream: they
    // carry NO position (the census is a bare hand list), so the send-side
    // proximity gate is skipped and the mint decision moves to the reply
    // (which carries the host's authoritative position).
    struct UnresolvedHand {
        float x, y, z; bool fromCensus;
        // Phase 1b: force-REQ recruit/move hand (reliable-edge correlated) -
        // skips the mint-side same-template dupe guard.
        bool forceReq;
        UnresolvedHand() : x(0), y(0), z(0), fromCensus(false), forceReq(false) {}
    };
    std::map<Key, UnresolvedHand> unresolvedHands_;
    // JOIN: recruited/re-keyed hands whose rekeyPeerBody landed ok=0 (no local
    // body to bind - the recruit fired while this hand was outside our interest,
    // the interest-split "join never saw Ruka" report). A reliable EVT_RECRUIT
    // PROVES the hand is a legit host body that must appear, so these force a
    // spawn REQ regardless of the send-side proximity gate (a far recruit is
    // still streamed as a host-owned member, so it must resolve). Entries clear
    // once the hand binds a proxy or resolves to a real local body.
    std::set<Key> forceReqHands_;
    // JOIN: census-mint reach (see setSpawnMintRadius) + the census-missing
    // scan throttle (the resolve sweep over censusHands_ runs at ~0.5 Hz, not
    // per tick).
    float         spawnMintRadius_;
    float         adoptRadius_;
    // Adoptions performed (census row bound to a body we already had). Reported
    // on the proxy telemetry line so a run says how much of a town was adopted
    // rather than minted.
    unsigned long censusAdopts_;
    unsigned long censusScanMs_;
    // Phase 0 telemetry: hands already reported unresolved (log once per hand).
    std::set<Key> spawnLogged_;
    // HOST: per-hand reply throttle (a re-request within the window is a
    // duplicate in flight, not a new question).
    std::map<Key, unsigned long> spawnReplyMs_;
    // JOIN: last "SCENARIO PROXY ..." telemetry emit (~2 Hz while proxies live).
    unsigned long spawnPosLogMs_;

    // Divergence-gated authority state (step 4).
    bool                 gateAuthority_;
    unsigned long        trustLogTick_;
    unsigned long        trustGrants_;   // trusted-mode entries this run
    unsigned long        trustRevokes_;  // trusted-mode exits (divergence/drift)
};

} // namespace coop

#endif // KENSHICOOP_REPLICATOR_H
