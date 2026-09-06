// NetLink - owns ENet on a dedicated background thread.
//
// Threading contract:
//   * The net thread EXCLUSIVELY owns the ENetHost; the game thread never
//     touches ENet.
//   * Inbound events (peer connect/leave, received EntityState) are handed to
//     the game thread via the Inbound queue.
//   * The game thread publishes this peer's owned entities via setOwnedEntities();
//     the net thread reads the latest snapshot and transmits it each tick.
//
// VS2010 (v100) compatible: Win32 threads + CRITICAL_SECTION (no std::thread).

#ifndef KENSHICOOP_NETLINK_H
#define KENSHICOOP_NETLINK_H

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <enet/enet.h>

#include "../../netproto/Wire.h"
#include "../core/Inbound.h"

namespace coop {

// Relay-dispatch action (Phase 3), keyed by docs/ROUTING_MATRIX.md's Class
// column via NetLink::routingClassOf(). Drives the host's receive-switch relay
// decision for every packet type - see routingClassOf()'s definition in
// NetLink.cpp for the full per-type table (matrix row citations included).
enum RelayClass {
    RELAY_BROADCAST_EXCEPT = 0, // Class A: fresh-copy broadcastExcept(author)
    RELAY_NONE             = 1, // Class B/C, or already-correct-as-is (Class D
                                 // PKT_TIME_PONG's inline unicast, Class F
                                 // HELLO/WELCOME's inline unicast) - never
                                 // dispatched through the relay layer
    RELAY_UNICAST          = 2, // Class D/E that already carry a routable
                                 // destination PlayerId field on the wire
                                 // (WorldItemClaimHeader::authorId,
                                 // InvXferAckPacket::xferOwnerId) - fresh-copy
                                 // sendTo(destId)
    RELAY_FAILSAFE_LOG     = 3  // Class D/E/F whose destination cannot be
                                 // resolved on the net thread this phase (needs
                                 // a game-thread hand->owner lookup, or the wire
                                 // struct carries no destination field at all -
                                 // RESEARCH.md Pitfall 4). Never blind-broadcast
                                 // (Security note: a leaked Class D/E is
                                 // information disclosure) - logs and drops.
};

class NetLink {
public:
    NetLink();
    ~NetLink();

    // Start as host on 'port' / as client to 'ip:port'. Inbound events go to
    // 'inbound'. Returns false if ENet init or the thread launch failed.
    bool startHost(int port, Inbound* inbound);
    bool startClient(const std::string& ip, int port, Inbound* inbound);
    void stop();

    // MAIN thread: publish this peer's owned entities (copied under lock). The
    // net thread re-broadcasts the latest snapshot each tick. Pass count 0 to
    // publish nothing.
    void setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count);

    // MAIN thread: queue a reliable one-shot event (KO/death/revive). The net thread
    // drains and sends it on the RELIABLE channel next tick (host broadcasts to all
    // peers; client sends to the host). Thread-safe; copied under lock.
    void queueEvent(const EventPacket& ev);

    // MAIN thread: queue a reliable container-contents snapshot (Phase 4a). The net
    // thread serializes [InvSnapshotHeader][InvItemEntry*count] and sends it on the
    // RELIABLE channel next tick. count may be 0 ("container now empty"). Copied
    // under lock; only enqueued on content-change so the reliable channel stays cheap.
    // keyKind (protocol 34): 0 = cKey is the raw container hand, 1 = cKey is the
    // protocol-27 placer key of a session-placed building (receiver translates).
    // `flags` carries INV_FLAG_TRUNCATED when the capture overflowed INV_ITEMS_MAX, so
    // the receiver reconciles additive-only instead of deleting past the cap.
    void queueInvSnapshot(u32 ownerId, u8 keyKind, const u32 cKey[5],
                          const InvItemEntry* items, unsigned int count, u8 flags = 0);

    // MAIN thread: queue a reliable world-item snapshot (Phase W1). The net thread
    // serializes [WorldItemSnapshotHeader][WorldItemEntry*count] and sends it on the
    // RELIABLE channel next tick. Only enqueued for new/changed ground items, so the
    // channel stays quiet for a settled world. Copied under lock.
    void queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count);

    // MAIN thread: queue a reliable world-item cull (Phase W1) - the netIds of ground
    // items that left the world / interest sphere. [WorldItemRemoveHeader][u32*count].
    void queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count);

    // MAIN thread: queue a reliable world-item claim INTENT (protocol 58: claimant
    // -> host, host-terminated) - the netIds whose proxies WE just consumed. The
    // netIds live in the AUTHOR's space, hence authorId; authorClaimMs (trailing,
    // default 0 - a caller built before Phase 7 Plan 02 still compiles) is OUR own
    // nowMs() at the claim-detection edge (same frame as EntityBatchHeader.sendMs),
    // which the host's peerClock_ maps for ClaimArbiter.h's contention window.
    // [WorldItemClaimHeader][u32*count].
    void queueWorldClaim(u32 ownerId, u32 authorId, const u32* netIds,
                         unsigned int count, u32 authorClaimMs = 0);

    // MAIN thread, HOST ONLY (protocol 58): queue the reliable host-committed
    // claim-contention verdict - the single authoritative winner for one item
    // identity, host-broadcast on CH_RELIABLE (Class B, like PKT_XFER_COMMIT/
    // PKT_OWN_RANKS). A join must never author this.
    void queueClaimVerdict(const ClaimVerdictPacket& pkt);

    // MAIN thread: queue a reliable wide-radius NPC existence census (protocol
    // 36, host -> join, 1 Hz). 'hands' is count*5 u32s (readObjectHand layout);
    // 'pos' is count*3 floats (v38: host position per row, park authority).
    // [NpcCensusHeader][u32 hand[5] * count][f32 pos[3] * count].
    void queueNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                        unsigned int count);

    // MAIN thread: queue a reliable conservation DROP intent (Phase W2). A fixed-size POD
    // (like an event), sent once on the RELIABLE channel; the peer relocates its own copy
    // of the weapon to the ground. Copied under lock.
    void queueWorldDrop(const WorldDropPacket& pkt);

    // MAIN thread: queue a reliable conservation PICKUP intent (Phase W3), mirror of the
    // drop. The peer re-homes its tracked ground copy back into the character's bag.
    void queueWorldPickup(const WorldPickupPacket& pkt);

    // MAIN thread: queue a reliable cross-owner TRANSFER intent (protocol 37). The peer
    // relocates the real item between its own copies of the two containers.
    void queueInvXfer(const InvXferPacket& pkt);

    // MAIN thread: queue the reliable VERDICT for an intent we just applied
    // (protocol 50, SUPERSEDED by queueXferCommit below) - how many units
    // actually landed here.
    void queueInvXferAck(const InvXferAckPacket& pkt);

    // MAIN thread, HOST ONLY (protocol 58): queue the reliable host-committed
    // transfer verdict - the single authoritative outcome for a transfer
    // intent, host-broadcast on CH_RELIABLE (Class B, like PKT_OWN_RANKS). A
    // join must never author this; if a join calls it by mistake the net
    // thread's isHost_ branch sends it toward the host instead of broadcasting
    // it (same inert-mis-call shape as broadcastOwnRanks's own doc comment).
    void queueXferCommit(const XferCommitPacket& pkt);

    // MAIN thread: queue the reliable transfer-commit bookkeeping ack
    // (protocol 58) - audit/correlation only, never settles anything.
    void queueXferCommitAck(const XferCommitAckPacket& pkt);

    // MAIN thread: queue a reliable owner-authoritative medical snapshot (phase 2,
    // player-squad only). Change-gated by the caller so the channel stays quiet.
    void queueMedical(const MedicalPacket& pkt);

    // MAIN thread: queue a reliable treatment delta (first aid administered on a
    // driven copy, forwarded to the body's owner).
    void queueTreatment(const TreatmentPacket& pkt);

    // MAIN thread: queue a reliable join-dealt damage report (join -> host). The
    // join's melee on a driven world-NPC copy is suppressed locally; the host
    // applies the reported damage to the authoritative body.
    void queueCombatHit(const CombatHitPacket& pkt);

    // MAIN thread: queue a reliable game-speed packet (REQ join->host, SET
    // host->join). Change-gated by the caller; pkt.type selects the direction.
    void queueSpeed(const SpeedPacket& pkt);

    // MAIN thread: queue a reliable owner-authoritative character-stats snapshot
    // (protocol 17, player-squad only). Change-gated by the caller.
    void queueStats(const StatsPacket& pkt);

    // MAIN thread: queue the reliable host-authoritative money-pool total
    // (protocol 52, host -> join). Change-gated by the caller.
    void queueMoney(const MoneyPacket& pkt);

    // MAIN thread: queue a reliable money-pool delta (protocol 52, join ->
    // host). Emitted once per observed local change; ordered delivery is what
    // makes the host fold it exactly once.
    void queueMoneyDelta(const MoneyDeltaPacket& pkt);

    // MAIN thread, HOST ONLY (protocol 60, CONS-01): queue the reliable
    // insufficient-funds VERDICT - the single authoritative reject for one
    // (buyerId, seq), host-broadcast on CH_RELIABLE (Class B, like
    // PKT_CLAIM_VERDICT/PKT_CELL_MAP/PKT_XFER_COMMIT). A join must never
    // author this.
    void queueMoneyReject(const MoneyRejectPacket& pkt);
    void queueFaction(const FactionPacket& pkt);
    void queueTime(const TimePacket& pkt);
    void queueDoor(const DoorPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative machine state row
    // (protocol 33). Change-gated + safety-resent by the caller.
    void queueProd(const ProdPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative known-research row
    // (protocol 38). First-sight sent + safety-resent by the caller.
    void queueResearch(const ResearchPacket& pkt);
    // MAIN thread: queue a reliable property-deed ownership row (protocol 54).
    // Symmetric (either client may buy); first-sight sent + safety-resent by the
    // caller, so a party that buys nothing is silent after the baseline.
    void queueDeed(const DeedPacket& pkt);
    // MAIN thread: queue a reliable runtime-fixture identity row (protocol 55).
    // Symmetric and static (a fixture's position/template never change), so this
    // is first-sight plus a slow safety resend and idles at zero traffic.
    void queueFixture(const FixturePacket& pkt);
    void queueBuildPlace(const BuildPlacePacket& pkt);
    void queueBuildState(const BuildStatePacket& pkt);
    void queueBuildDoor(const BuildDoorPacket& pkt);
    void queueBuildRemove(const BuildRemovePacket& pkt);

    // MAIN thread: queue an UNRELIABLE stealth detection-map snapshot (protocol
    // 20, host -> the sneaker's owner). Latest wins; change-gated + throttled by
    // the caller, so loss just delays an arrow update one snapshot.
    void queueStealth(const StealthPacket& pkt);

    // MAIN thread: queue an UNRELIABLE camera hint (protocol 43, either
    // direction, ~1 Hz). Latest wins; loss just delays the anchor one hint.
    void queueCamHint(const CamHintPacket& pkt);

    // MAIN thread: queue a RELIABLE cell claim (protocol 49, either direction,
    // ~1 Hz). Reliable because a dropped claim reverts the cell to host
    // authority until the next re-assert, and that window is a duplicate-
    // authorship window.
    void queueCellClaim(const CellClaimPacket& pkt);

    // MAIN thread, HOST ONLY (protocol 59, WORLD-03): queue the reliable
    // host-committed cell-claim map - the single authoritative verdict,
    // host-broadcast on CH_RELIABLE (Class B, like PKT_CLAIM_VERDICT/
    // PKT_XFER_COMMIT/PKT_OWN_RANKS). A join must never author this; if a
    // join calls it by mistake the net thread's isHost_ branch sends it
    // toward the host instead of broadcasting it (same inert-mis-call shape
    // as broadcastOwnRanks's own doc comment) and the host's receive branch
    // rejects it outright.
    void queueCellMap(const CellMapPacket& pkt);

    // MAIN thread: queue a reliable runtime-spawn query (protocol 21, join ->
    // host). Debounced per hand by the caller.
    void queueSpawnReq(const SpawnReqPacket& pkt);

    // MAIN thread: queue a reliable runtime-spawn description (protocol 21,
    // host -> join). Reply-cached by the caller.
    void queueSpawnInfo(const SpawnInfoPacket& pkt);

    // MAIN thread: coordinated-save packets (protocol 31). REQ join -> host
    // (a suppressed local save forwarded for arbitration); BEGIN/FILE/DONE
    // host -> join (the paced folder transfer; FILE is variable-length:
    // header + relative path + payload, serialized by the net thread); ACK
    // join -> host (staged save verified + committed). All CH_RELIABLE - the
    // ordered stream is what makes the chunk protocol stateless per chunk.
    // destId (Phase 10 Plan 01, SAVE-01/SAVE-04): OWNER_ID_ALL (default) is
    // the historical enet_host_broadcast; a specific PlayerId routes via
    // sendTo instead - transport-level only, no wire-struct change.
    void queueSaveReq(const SaveReqPacket& pkt);
    void queueSaveBegin(const SaveBeginPacket& pkt, u32 destId = OWNER_ID_ALL);
    void queueSaveFile(const SaveFileHeader& hdr, const char* relPath,
                       const unsigned char* data, unsigned int dataLen,
                       u32 destId = OWNER_ID_ALL);
    void queueSaveDone(const SaveDoneHeader& hdr, const u32* crcs, unsigned int count,
                       u32 destId = OWNER_ID_ALL);
    void queueSaveAck(const SaveAckPacket& pkt);

    // MAIN thread: coordinated-load packets (protocol 32). GO host -> join
    // (load this save now, fingerprint attached); REQ join -> host (a
    // suppressed local load forwarded for arbitration); NACK join -> host
    // (copy missing/diverged - answer with a SaveXfer). All CH_RELIABLE.
    // destId: same OWNER_ID_ALL-broadcast-or-sendTo convention as the save
    // packets above (Plan 02 scopes the connect-push GO to the joiner).
    void queueLoadGo(const LoadGoPacket& pkt, u32 destId = OWNER_ID_ALL);
    void queueLoadReq(const LoadReqPacket& pkt);
    void queueLoadNack(const LoadNackPacket& pkt);
    // MAIN thread (Phase 10 Plan 01, SAVE-02): join -> host positive
    // coordinated-load completion (the missing half of the NACK-only pair).
    void queueLoadAck(const LoadAckPacket& pkt);
    // MAIN thread, HOST ONLY (Phase 10 Plan 01, SAVE-03): queue the reliable
    // first-wins arbitration reject - Class D unicast to the rejected
    // requester only (the InvXferAckPacket/PKT_CLAIM_VERDICT precedent). A
    // join must never author this; the net thread's isHost_ receive-branch
    // guard rejects a client-authored one exactly like PKT_CLAIM_VERDICT/
    // PKT_MONEY_REJECT.
    void queueCoordReject(const CoordRejectPacket& pkt);

    // MAIN thread (Phase 10 Plan 01, SAVE-01): request a marshaled disconnect
    // of 'playerId' - the net-thread-purity drop lever for the save/load
    // coordinator's bounded-retry-then-drop policy. Queues under outCs_ (the
    // queueClaimVerdict lock idiom); the net thread drains it next tick and
    // calls enet_peer_disconnect on the registry peer (the same primitive the
    // handshake-reject sites already use, NetLink.cpp:816,841). The resulting
    // DISCONNECT event then drives the EXISTING leave edge (pushLeave ->
    // clearPeerReplicationState + roster LEFT) - no new cleanup path is
    // created. A no-op (net thread finds nothing in the registry) if
    // 'playerId' is not currently connected.
    void kickPeer(u32 playerId);

    // MAIN thread, HOST ONLY (protocol 57, Phase 3 Plan 03): queue the
    // reliable ownership-rank announcement - the host's authoritative
    // map<PlayerId,set<rank>>, host-broadcast on CH_RELIABLE (Class B, like
    // PKT_PLAYER_JOINED/LEFT). A join must never author this (no authority to
    // announce); the net thread's isHost_ guard drops it rather than ever
    // emitting a peer-authored ownership claim onto the wire (T-03-06).
    void broadcastOwnRanks(const OwnRanksPacket& pkt);

    // Debug WAN simulation. When delayMs > 0, received entity batches are held in a
    // net-thread queue and delivered to the game thread only after delayMs +/- jitter
    // has elapsed (lossPct of them are dropped outright). Must be called before
    // startHost/startClient. All-zero = disabled (immediate delivery). See Config.
    void setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct);

    // Steam P2P transport: tunnel the ENet protocol over Steam P2P to 'peerSteamId'
    // (steamid64) instead of UDP. The wire protocol, channels, reliability and
    // reconnect logic are unchanged - only the datagram pipe differs (ENet socket
    // hooks installed on the net thread; MTU clamped to Steam's 1200-byte
    // unreliable ceiling). Must be called before startHost/startClient. 0 = UDP.
    void setSteamTransport(unsigned long long peerSteamId);

    // MAIN thread: advance this peer's session epoch (protocol 44). Called on
    // every session-reset edge (coordinated world reload, connect/disconnect
    // teardown). Subsequent entity batches carry the new epoch, so the peer
    // drops any still-in-flight batch from the prior session; the pending owned-
    // entity snapshot is also dropped so a stale one is not re-stamped with the
    // new epoch and mistaken for fresh. Thread-safe (InterlockedIncrement + the
    // publish lock for the snapshot clear).
    void bumpSessionEpoch();

    bool isRunning() const { return running_ != 0; }
    // host = 0; client = id from WELCOME. myId_ is written by the NET thread when
    // the WELCOME arrives and read here on the MAIN thread, so it is a volatile
    // LONG written via InterlockedExchange; an aligned 32-bit volatile read is
    // atomic on x86/x64 and the volatile bars the compiler from caching a stale
    // value (Phase 4: myId_ cross-thread safety).
    u32  localId()   const { return (u32)myId_; }

    // NET thread only (Phase 2): the host-side send primitives Phase 3's relay
    // layer calls. Built on registry_ (below) and the existing
    // enet_peer_send/enet_host_broadcast calls already used by every outbound
    // drain in threadLoop() (the two-branch shape repeated ~40 times). Phase 3
    // Plan 01 wires broadcastExcept into every Class A receive branch and
    // sendTo into the two already-addressable Class D/E branches via
    // relayDispatch() below - the outbound queueX() drains further down in
    // this header are a separate, not-yet-migrated call-site group.
    //
    // In all three, the caller relinquishes ownership of 'pkt': it is either
    // handed to ENet (which frees it once every send completes - ENet
    // refcounts a packet internally, so sending the SAME ENetPacket* to
    // multiple peers in a loop is the documented-safe pattern) or destroyed
    // here if it has nowhere to go. Never a crash, never a leak.
    //
    //   sendTo:          delivers to exactly the one peer registered for
    //                    'playerId'; destroys 'pkt' if that id is not
    //                    currently registered or its peer isn't connected.
    //   broadcast:       host-only concept - on the host, reaches every
    //                    connected peer via enet_host_broadcast; destroys
    //                    'pkt' instead of misrouting it if called when
    //                    !isHost_.
    //   broadcastExcept: reaches every registry_ peer except 'playerId' via a
    //                    real per-peer enet_peer_send loop over the SAME
    //                    ENetPacket* - never enet_host_broadcast-then-"unsend"
    //                    (ENet has no such retraction). Destroys 'pkt' if the
    //                    loop sends to zero peers.
    void sendTo(u32 playerId, ENetPacket* pkt, int channel);
    void broadcast(ENetPacket* pkt, int channel);
    void broadcastExcept(u32 playerId, ENetPacket* pkt, int channel);

    // Pure, static, game-free lookup (Phase 3): the relay-dispatch table keyed
    // by docs/ROUTING_MATRIX.md's Class column. No NetLink instance state is
    // touched - safe to call from any thread, including nettest's Task 3
    // drift oracle calling it directly with no NetLink constructed at all.
    // See NetLink.cpp for the definition (full per-PacketType table, with
    // matrix row citations for every case).
    static RelayClass routingClassOf(u8 packetType);

    // TEST HOOK (Phase 2, CR-02 regression): CLIENT only. Requests that the net
    // thread resend a fresh PKT_HELLO on the already-established connection to
    // the host, exactly mirroring the one automatically sent at connect time.
    // Production code never calls this - a client only ever sends HELLO once
    // (threadLoop()'s CONNECT handler). It exists solely so nettest can prove
    // the host's duplicate-HELLO guard: a second successful HELLO on a
    // connection that already has a PlayerId must be ignored, not re-scanned
    // into a second id that orphans the first (see NetLink.cpp's
    // `ev.peer->data != 0` guard in the PKT_HELLO receive branch). Thread-safe
    // (InterlockedExchange flag; consumed once by the net thread next tick).
    void debugResendHelloForTest();

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();

    // Net-thread-only: route a received entity through the WAN sim (delay/drop) when
    // enabled, else deliver immediately. flushDelayed() releases matured entries.
    void deliverEntity(u32 ownerId, u32 sendMs, const EntityState& e);
    void flushDelayed();

    // Net-thread-only (protocol 44): gate an incoming entity batch by its session
    // epoch. Returns false (drop) if 'epoch' is older than the newest accepted
    // from 'ownerId'; otherwise records it and returns true. epochSeen_ is reset
    // at every connection edge so a reconnecting peer restarting at epoch 0 is
    // never locked out.
    bool acceptEpoch(u32 ownerId, u32 epoch);

    // NET thread, host-only (Phase 3): validated relay dispatch for a packet
    // just received, keyed by routingClassOf(packetType). No-op if !isHost_ -
    // joins never relay. Called AFTER each receive branch's own local-apply +
    // bounds/epoch guards, once per branch, with that branch's own
    // already-parsed sender-claimed 'claimedOwnerId' (validated against
    // 'sourcePlayerId' - the host-assigned ev.peer->data - exactly like the
    // tracer's Class A check) and, for the two already-addressable Class D/E
    // packets (RELAY_UNICAST), the destination PlayerId their own payload
    // carries in 'destId' (ignored for every other class). Relays a FRESH
    // packet copy of 'ev.packet' (never ev.packet itself - the receive case's
    // single unconditional enet_packet_destroy(ev.packet) would double-free
    // it), on the same channel the packet arrived on.
    void relayDispatch(u8 packetType, u32 sourcePlayerId, u32 claimedOwnerId,
                        u32 destId, const ENetEvent& ev);

    // NET thread, host-only (Phase 3 CR-01 fix): the ownerId-vs-domain guard
    // for every Class A/E receive branch that carries a client-controlled
    // owner field, called BEFORE that branch's own local-apply push
    // (deliverEntity()/inbound_->push*()) - never after. Previously this
    // exact check only ran inside relayDispatch(), which is called AFTER the
    // local-apply; that let a forged ownerId corrupt the HOST'S OWN state
    // (Inbound -> Replicator::ingest() -> Driven.owner) even though the
    // relay-boundary check correctly stopped the forgery from reaching a
    // THIRD client. Returns true (REJECT: skip local-apply AND relay) when
    // isHost_ and claimedOwnerId != sourcePlayerId, logging the same
    // "relay REJECT ..." line relayDispatch() already used for this case (so
    // the log format callers/oracles grep for is unchanged). Returns false
    // (ACCEPT) on a client (isHost_ == false) unconditionally - a client only
    // ever receives host-authored payloads under this architecture's threat
    // model, exactly like relayDispatch()'s own isHost_ guard.
    bool rejectIfForgedOwner(u32 sourcePlayerId, u32 claimedOwnerId);

    bool        isHost_;
    std::string ip_;
    int         port_;

    ENetHost*   enetHost_;   // net thread only
    ENetPeer*   serverPeer_; // client only; net thread only
    Inbound*    inbound_;

    CRITICAL_SECTION         outCs_;
    std::vector<EntityState> out_;
    u32                      outOwner_;
    u32                      outStampMs_; // capture-time stamp for the batch header (v35)
    bool                     haveOut_;
    // Reliable events queued by the main thread, drained + sent by the net thread.
    // Guarded by outCs_ (same publish lock as out_).
    std::vector<EventPacket> outEvents_;
    // Reliable container-contents snapshots queued by the main thread, drained +
    // serialized by the net thread. Variable-length, so each carries its own item
    // list. Guarded by outCs_.
    struct OutInv {
        u32                       ownerId;
        u8                        keyKind; // protocol 34: 0 raw hand, 1 placer key
        u8                        flags;   // protocol 46: INV_FLAG_TRUNCATED
        u32                       cKey[5];
        std::vector<InvItemEntry> items;
    };
    std::vector<OutInv>      outInv_;
    // Reliable world-item snapshots / culls queued by the main thread (Phase W1),
    // drained + serialized by the net thread. Guarded by outCs_.
    struct OutWorldItems { u32 ownerId; std::vector<WorldItemEntry> items; };
    struct OutWorldRemove { u32 ownerId; std::vector<u32> netIds; };
    struct OutWorldClaim { u32 ownerId; u32 authorId; u32 authorClaimMs; std::vector<u32> netIds; };
    std::vector<OutWorldItems>  outWorldItems_;
    std::vector<OutWorldRemove> outWorldRemove_;
    std::vector<OutWorldClaim>  outWorldClaim_;
    // Reliable host-committed claim-contention verdicts (protocol 58). Host-only;
    // guarded by outCs_.
    std::vector<ClaimVerdictPacket> outClaimVerdict_;
    // Reliable NPC existence census (protocol 36): 5xu32 hands, flat. Guarded
    // by outCs_. 1 Hz from the host, so at most a couple pending at once.
    struct OutNpcCensus { u32 ownerId; std::vector<u32> hands; std::vector<float> pos; };
    std::vector<OutNpcCensus> outNpcCensus_;
    // Reliable conservation DROP intents (Phase W2), fixed-size PODs. Guarded by outCs_.
    std::vector<WorldDropPacket> outWorldDrops_;
    std::vector<WorldPickupPacket> outWorldPickups_;
    // Reliable cross-owner transfer intents (protocol 37). Guarded by outCs_.
    std::vector<InvXferPacket>   outInvXfers_;
    // Reliable transfer verdicts (protocol 50, superseded). Guarded by outCs_.
    std::vector<InvXferAckPacket> outInvXferAcks_;
    // Reliable host-committed transfer verdicts (protocol 58). Host-only;
    // guarded by outCs_.
    std::vector<XferCommitPacket> outXferCommit_;
    // Reliable transfer-commit bookkeeping acks (protocol 58). Guarded by outCs_.
    std::vector<XferCommitAckPacket> outXferCommitAck_;
    // Reliable medical snapshots + treatment deltas (phase 2). Guarded by outCs_.
    std::vector<MedicalPacket>   outMedical_;
    std::vector<TreatmentPacket> outTreatments_;
    std::vector<CombatHitPacket> outCombatHits_;
    // Reliable game-speed REQ/SET packets (consensus speed sync). Guarded by outCs_.
    std::vector<SpeedPacket>     outSpeed_;
    // Reliable character-stats snapshots (protocol 17). Guarded by outCs_.
    std::vector<StatsPacket>     outStats_;
    // Reliable money-pool totals + join deltas (protocol 52). Guarded by outCs_.
    std::vector<MoneyPacket>     outMoney_;
    std::vector<MoneyDeltaPacket> outMoneyDelta_;
    // Reliable insufficient-funds verdicts (protocol 60, CONS-01, host-only broadcast). Guarded by outCs_.
    std::vector<MoneyRejectPacket> outMoneyReject_;
    std::vector<FactionPacket>   outFaction_;
    std::vector<TimePacket>      outTime_;
    std::vector<DoorPacket>      outDoor_;
    // Reliable machine state rows (protocol 33). Guarded by outCs_.
    std::vector<ProdPacket>      outProd_;
    // Reliable known-research rows (protocol 38). Guarded by outCs_.
    std::vector<ResearchPacket>  outResearch_;
    // Reliable property-deed ownership rows (protocol 54). Guarded by outCs_.
    std::vector<DeedPacket>      outDeed_;
    // Reliable runtime-fixture identity rows (protocol 55). Guarded by outCs_.
    std::vector<FixturePacket>   outFixture_;
    std::vector<BuildPlacePacket> outBuildPlace_;
    std::vector<BuildStatePacket> outBuildState_;
    std::vector<BuildDoorPacket>  outBuildDoor_;
    std::vector<BuildRemovePacket> outBuildRemove_;
    // Unreliable stealth detection-map snapshots (protocol 20). Guarded by outCs_.
    std::vector<StealthPacket>   outStealth_;
    // Unreliable camera hints (protocol 43, ~1 Hz latest-wins). Guarded by outCs_.
    std::vector<CamHintPacket>   outCamHint_;
    // Reliable cell claims (protocol 49, ~1 Hz on change + re-assert). Guarded by outCs_.
    std::vector<CellClaimPacket> outCellClaim_;
    std::vector<CellMapPacket>   outCellMap_;
    // Reliable runtime-spawn query/description packets (protocol 21). Guarded by outCs_.
    std::vector<SpawnReqPacket>  outSpawnReq_;
    std::vector<SpawnInfoPacket> outSpawnInfo_;
    // Reliable coordinated-save packets (protocol 31). FILE carries its
    // variable tail (relative path + payload) pre-flattened; DONE carries its
    // CRC table. Each carries a queue-side destId (Phase 10 Plan 01):
    // OWNER_ID_ALL broadcasts (the historical behavior), a specific
    // PlayerId routes via sendTo - transport-level only, no wire change.
    // Guarded by outCs_.
    struct OutSaveBegin { SaveBeginPacket pkt; u32 destId; };
    struct OutSaveFile  { SaveFileHeader hdr; std::vector<u8> tail; u32 destId; };
    struct OutSaveDone  { SaveDoneHeader hdr; std::vector<u32> crcs; u32 destId; };
    std::vector<SaveReqPacket>   outSaveReq_;
    std::vector<OutSaveBegin>    outSaveBegin_;
    std::vector<OutSaveFile>     outSaveFile_;
    std::vector<OutSaveDone>     outSaveDone_;
    std::vector<SaveAckPacket>   outSaveAck_;
    // Reliable coordinated-load packets (protocol 32). LoadGo carries a
    // queue-side destId like the save packets above. Guarded by outCs_.
    struct OutLoadGo { LoadGoPacket pkt; u32 destId; };
    std::vector<OutLoadGo>       outLoadGo_;
    std::vector<LoadReqPacket>   outLoadReq_;
    std::vector<LoadNackPacket>  outLoadNack_;
    // Reliable positive coordinated-load ACKs (protocol 61, join -> host).
    // Guarded by outCs_.
    std::vector<LoadAckPacket>   outLoadAck_;
    // Reliable first-wins arbitration rejects (protocol 61, host -> the
    // rejected requester only, Class D unicast). Host-only in practice;
    // guarded by outCs_.
    std::vector<CoordRejectPacket> outCoordReject_;
    // Marshaled kickPeer requests (Phase 10 Plan 01, SAVE-01): PlayerIds the
    // game thread wants the net thread to enet_peer_disconnect next tick.
    // Guarded by outCs_.
    std::vector<u32>             outKick_;
    // Reliable ownership-rank announcements (protocol 57). Host-only; guarded
    // by outCs_ like every other fixed-POD queue.
    std::vector<OwnRanksPacket>  outOwnRanks_;

    HANDLE        thread_;
    volatile LONG running_;
    volatile LONG stopFlag_;
    // Written by the NET thread on WELCOME (InterlockedExchange) and read on the
    // MAIN thread via localId(); volatile LONG so the read is atomic + uncached.
    volatile LONG myId_;

    // Session epoch (protocol 44). sendEpoch_ is bumped by the MAIN thread
    // (InterlockedIncrement in bumpSessionEpoch) and read by the NET thread when
    // it stamps an outgoing entity batch - a volatile LONG, so the read is atomic
    // + uncached. epochSeen_ is NET-thread-only (touched only in the receive
    // ladder + connect/disconnect handlers), so it needs no lock.
    volatile LONG        sendEpoch_;
    std::map<u32, u32>   epochSeen_; // newest accepted epoch per ownerId

    // TEST HOOK (Phase 2, CR-02 regression) - see debugResendHelloForTest()
    // above. Set by the main thread via InterlockedExchange, consumed (reset
    // to 0) by the net thread the next time it drains its per-tick queues.
    volatile LONG        debugResendHello_;

    // Host-side peer registry (Phase 2). NET-thread-only (touched only in the
    // receive ladder + connect/disconnect handlers), so it needs no lock - same
    // justification as epochSeen_ above. PlayerId 0 is reserved for the host;
    // joins occupy the lowest free slot in [1, MAX_PLAYERS).
    struct PeerState { ENetPeer* peer; /* + per-player bookkeeping added in later plans */ };
    std::map<u32 /*PlayerId*/, PeerState> registry_;

    // Steam P2P transport (set before launch; read-only on the net thread
    // thereafter). 0 = stock UDP transport.
    unsigned long long steamPeer_;

    // WAN sim config (set before launch; read-only on the net thread thereafter).
    unsigned int  simDelayMs_;
    unsigned int  simJitterMs_;
    unsigned int  simLossPct_;
    // Held-back inbound entities awaiting their simulated arrival time. Net-thread
    // only (received and flushed on the same thread), so it needs no lock.
    struct Delayed { DWORD releaseTick; u32 ownerId; u32 sendMs; EntityState e; };
    std::deque<Delayed> delayed_;

    NetLink(const NetLink&);
    NetLink& operator=(const NetLink&);
};

} // namespace coop

#endif // KENSHICOOP_NETLINK_H
