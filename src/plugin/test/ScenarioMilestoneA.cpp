// ScenarioMilestoneA.cpp - milestone_a_gate (Phase 4 plan 04, POC-02/POC-03): the
// ONE master 10-step Definition-of-Done scenario every one of the 4 processes
// runs for the Milestone A gate. Branches ONLY on ctx.isHost and this client's
// own rank (ctx.localId - the host-authoritative rank=playerId identity a baked
// 4-squad start keeps exact at N=4, see the A4 note below), never a new
// host/join1/join2/join3 enum: "a join" behaves generically, addressed
// individually only where the DoD needs one specific join (the KO victim, keyed
// by a fixed rank constant, not by connection order).
//
// A4 decision (this plan): the dynamic-tab ownership fallback documented at
// ReplicatorPublish.cpp:895-921 attributes EVERY dynamically-created tab beyond
// the seeded pair to "the lowest known peer id" - which COLLIDES the moment more
// than one dynamic tab exists at once (exactly the N>2 case this gate has to
// prove is correct, not merely non-empty). A baked 4-squad start
// (tools/MultiplayerStartGen's new "Multiplayer (Wanderer x4)") seeds all four
// ranks instead, so ownerForRank(rank)=rank stays exact and this scenario's
// ownRank_ = ctx.localId is authoritative from tick 1 - see Replicator.h's own
// audit comment ("rank=playerId stays correct for every ordinary session
// including N>2" for a seeded, non-host-owned rank).
//
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md) -
// plan 05's oracle keys on the "SCENARIO MAGATE ..." lines below plus the
// existing MEMBER/RECV/VITALS/WNPC/COMBATSTATE schemas this scenario reuses
// unmodified.

#include "ScenarioSupport.h"

namespace coop {
namespace {

// milestone_a_gate: drives + self-observes all 10 DoD steps on every one of the
// 4 processes. See the file header for the A4 rationale and the ownRank_ =
// ctx.localId identity this scenario relies on.
class MilestoneAScenario : public TimedScenario {
public:
    MilestoneAScenario()
        : TimedScenario("milestone_a_gate", 1000),
          ownRank_(0), haveOwn_(false), sx_(0.0f), sy_(0.0f), sz_(0.0f),
          hostileSpawned_(false), nHostile_(0), lastCombatOrderMs_(0),
          koDone_(false), koRevived_(false), lastKoAssertMs_(0),
          koTargetRank_(0), koTargetLatched_(false),
          haveBaseline_(false), recvCount_(0) {
        memset(ownHand_, 0, sizeof(ownHand_));
        memset(hostileHand_, 0, sizeof(hostileHand_));
        for (unsigned int i = 0; i < MAX_PLAYERS; ++i) lastPeerSet_[i] = false;
    }

    // Step 1 (connection) is already proven by arming itself (peerReady/timeout,
    // Scenario.h's own contract) - nothing extra to do here beyond announcing
    // our own rank, resolved from the host-authoritative ownerForRank identity
    // (OwnRanks.h) rather than a hardcoded isHost binary.
    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.localId;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE start ownRank=%u host=%d",
                  ownRank_, ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Phase 11 plan 02 (TEST-01): keep the adaptive rank roster maximally
        // fresh every tick, before any rank-gated leg below reads
        // koTargetRank() - see ScenarioSupport.h's RankRoster doc comment.
        if (ctx.connectedPeers) roster_.observe(ownRank_, ctx);

        if (evidenceDue(ctx.elapsedMs)) tickEvidence(ctx);

        // Step 5 (combat): host only, spawns + keeps hostiles ordered at its own
        // leader. A host-owned victim is all-local (PlayerCombatScenario's
        // Window B precedent) - no cross-owner order is needed to prove real
        // combat replicates.
        if (ctx.isHost && haveOwn_ && ctx.elapsedMs >= COMBAT_AT_MS &&
            ctx.elapsedMs < COMBAT_STOP_AT_MS)
            tickCombat(ctx);

        // Step 6 (KO/death): the client that owns koTargetRank() downs +
        // revives its OWN leader (player_ko's proven pattern, generalized off
        // an adaptive rank - the highest rank this client has ever seen
        // connected, RankRoster::highest() - rather than a hardcoded
        // host/join branch or a fixed N=4-only constant. Cross-owner body
        // manipulation is not reliable/authoritative, so every OTHER client
        // just observes via the MEMBER/RECV/VITALS series tickEvidence
        // already logs, which carries the bs= down/up transition for free).
        // At N=4, once every join has connected, this resolves to exactly 3 -
        // today's frozen KO_TARGET_RANK value - so the N=4 gate re-runs
        // against byte-unchanged behavior (tickKo's own KO_AT_MS/
        // KO_REVIVE_AT_MS timeline gates are tens of seconds past arm, ample
        // margin for the roster to have converged by then even under a
        // staggered join launch).
        //
        // Phase 11 review WR-05: resolve the target rank ONCE, at the KO
        // leg's own arming edge (the first tick at/after KO_AT_MS), instead
        // of re-reading roster_.highest() inside the dispatch condition every
        // tick. Re-resolution meant a peer whose first connect landed AFTER
        // this client's own KO_AT_MS flipped the dispatch mid-sequence: the
        // client that had already DOWNED its leader stopped matching the
        // condition and never ran its KO_REVIVE leg (a permanently-downed
        // leader plus an unexplainable second bs= down transition for the
        // oracles once the late peer ran its own tickKo). Latching at the
        // leg's arm keeps the whole down->hold->revive sequence on one stable
        // actor per run, with maximal roster convergence time (KO_AT_MS is
        // the latest point a single resolution can happen), and is byte-
        // identical to the frozen constant once the roster has converged.
        if (!koTargetLatched_ && ctx.elapsedMs >= KO_AT_MS) {
            koTargetLatched_ = true;
            koTargetRank_ = roster_.highest();
            char kb[96];
            _snprintf(kb, sizeof(kb) - 1,
                      "SCENARIO MAGATE KO-TARGET rank=%u ownRank=%u",
                      koTargetRank_, ownRank_);
            kb[sizeof(kb) - 1] = '\0'; coop::logLine(kb);
        }
        if (koTargetLatched_ && ownRank_ == koTargetRank_ && haveOwn_) tickKo(ctx);

        // Steps 7-8 (disconnect/reconnect): every client watches the connected-
        // peer set continuously. The scenario never disconnects anyone itself -
        // that is the harness's job (plan 02's run_test4 exits one instance) -
        // this just detects and asserts on whichever edge actually happens.
        tickPeerWatch(ctx);

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : joinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // Local legs only (script-ran verdict) - plan 05's oracle judges the
            // cross-client log evidence for the real DoD pass/fail.
            passed_ = haveOwn_ && (ctx.isHost ? hostileSpawned_ : (recvCount_ >= 1));
            return true;
        }
        return false;
    }

private:
    // ---- Step 2 (ownership) + Step 3 (movement) + Step 4 (NPC visibility
    // anchor) + continuous MEMBER/RECV/VITALS/COMBATSTATE evidence. ------------
    void tickEvidence(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, /*leaderOnly*/ false, sq, MAX_SQUAD);

        // Resolve + log our own leader ONCE (the "resolved rank->squad" line
        // must_haves calls for), then latch the start position for movement.
        int ownIdx = tabLeaderIdx(sq, n, ownRank_);
        if (ownIdx >= 0) {
            handFromEntity(sq[ownIdx], ownHand_);
            if (!haveOwn_) {
                haveOwn_ = true;
                sx_ = sq[ownIdx].x; sy_ = sq[ownIdx].y; sz_ = sq[ownIdx].z;
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE OWNRANK rank=%u hand=%u,%u",
                          ownRank_, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // The FULL observed rank->player map (must_haves: not merely a
        // non-empty check) - one TABMAP line per distinct tab THIS client can
        // currently see, host or join alike. Cross-referenced by plan 05's
        // oracle against the host's own "player=X rank=Y" PKT_OWN_RANKS lines
        // (ReplicatorPublish.cpp) to assert exactly one owner per squad.
        logTabMap(sq, n);

        // Step 3: oscillate our own leader for a window, then settle back to
        // the start (coop_presence's proven pattern, generalized to N ranks -
        // "our tab" vs "every other tab" rather than a hardcoded host/join
        // pair).
        if (haveOwn_) {
            Character* oc = engine::resolveCharByHand(ownHand_[3], ownHand_[4],
                                                       ownHand_[0], ownHand_[1],
                                                       ownHand_[2]);
            if (oc) {
                if (ctx.elapsedMs < MOVE_MS) {
                    bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                    engine::orderMoveTo(oc, legB ? sx_ + LEG : sx_, sy_,
                                            legB ? sz_ + LEG : sz_);
                } else {
                    engine::orderMoveTo(oc, sx_, sy_, sz_);
                }
            }
        }

        // MEMBER/RECV/VITALS for every squad member across every rank we can
        // see - generalizes coop_presence's 2-tab partition to however many
        // tabs are actually connected (tabRankOf already scans up to 32
        // distinct tabs, so N=4 needs no new helper).
        bool sawPeer = false;
        for (unsigned int i = 0; i < n; ++i) {
            int r = tabRankOf(sq, n, i);
            if (r < 0) continue;
            logScenarioEntity(((unsigned int)r == ownRank_) ? "MEMBER" : "RECV", sq[i]);
            if ((unsigned int)r != ownRank_) sawPeer = true;
            unsigned int h[5]; handFromEntity(sq[i], h);
            logVitalsLine(h, ctx.elapsedMs);
        }
        if (sawPeer) ++recvCount_;

        // Step 4 (NPC visibility): the replicator's own auditRows dump (enabled
        // for this scenario name in Plugin.cpp) emits the "SCENARIO WNPC" rows
        // analyze_wnpc_diff4.ps1 reads on its own 5 s cadence; this is just a
        // stable per-DoD-step anchor line tying a census count to this client's
        // clock.
        EntityState npcs[MAX_LOG];
        unsigned int nn = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
        char nb[64];
        _snprintf(nb, sizeof(nb) - 1, "SCENARIO MAGATE NPCCENSUS n=%u", nn);
        nb[sizeof(nb) - 1] = '\0'; coop::logLine(nb);

        // 04-07 gap-closure diagnostics: tabsSeen (raw playerCharacters count)
        // vs. centersResolved (distinct squad-tab-leader interest spheres) at
        // the exact moment captureNpcs() just ran above. If any client logs
        // tabsSeen<4 or centersResolved<4 while a 4-squad start is loaded,
        // playerCharacters is incomplete at capture time (the progressive
        // roster-sync race hypothesis) - directly observable per tick, per
        // client, oracle-greppable.
        unsigned int tabsSeen = 0, centersResolved = 0;
        engine::lastInterestDebug(tabsSeen, centersResolved);
        char cb[112];
        _snprintf(cb, sizeof(cb) - 1,
                  "SCENARIO MAGATE CENSUSDBG tabsSeen=%u centersResolved=%u ownRank=%u",
                  tabsSeen, centersResolved, ownRank_);
        cb[sizeof(cb) - 1] = '\0'; coop::logLine(cb);

        // Combat-state parity sample on rank 0's (the host's) leader, resolvable
        // by every client via its own local capture - visible on all 4
        // processes during the combat window regardless of who threw the punch.
        int hostIdx = tabLeaderIdx(sq, n, 0u);
        if (hostIdx >= 0) {
            unsigned int hh[5]; handFromEntity(sq[hostIdx], hh);
            logCombatStateLine(hh, ctx.elapsedMs);
        }
    }

    // One line per distinct tab this client can currently see.
    void logTabMap(const EntityState* sq, unsigned int n) {
        bool seen[MAX_RANKS];
        for (unsigned int i = 0; i < MAX_RANKS; ++i) seen[i] = false;
        for (unsigned int i = 0; i < n; ++i) {
            int r = tabRankOf(sq, n, i);
            if (r < 0 || (unsigned int)r >= MAX_RANKS || seen[(unsigned int)r]) continue;
            seen[(unsigned int)r] = true;
            int idx = tabLeaderIdx(sq, n, (unsigned int)r);
            if (idx < 0) continue;
            unsigned int h[5]; handFromEntity(sq[idx], h);
            char b[112];
            _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE TABMAP rank=%u hand=%u,%u",
                      (unsigned int)r, h[3], h[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Step 5: host spawns MAX_HOSTILE hostile NPCs once and keeps them ordered
    // at its own leader every 2.5 s (throttled the same way combat_order/
    // player_combat keep a fight alive without thrashing the AI).
    void tickCombat(const ScenarioContext& ctx) {
        if (!hostileSpawned_) {
            hostileSpawned_ = true;
            nHostile_ = engine::spawnHostileSquad(ctx.gw, MAX_HOSTILE, HOSTILE_REL,
                                                  hostileHand_);
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE COMBAT spawned n=%u vic=%u,%u",
                      nHostile_, ownHand_[3], ownHand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (nHostile_ > 0 && ctx.elapsedMs - lastCombatOrderMs_ >= 2500) {
            lastCombatOrderMs_ = ctx.elapsedMs;
            for (unsigned int i = 0; i < nHostile_; ++i)
                engine::orderAttackByHand(ctx.gw, hostileHand_[i], ownHand_);
        }
    }

    // Step 6: down our own leader, hold it, then revive it (player_ko's proven
    // scaffold pattern - deterministic within the harness's time budget rather
    // than relying on real combat damage landing a KO in-window).
    void tickKo(const ScenarioContext& ctx) {
        if (ctx.elapsedMs >= KO_AT_MS && ctx.elapsedMs < KO_REVIVE_AT_MS) {
            if (!koDone_ || ctx.elapsedMs - lastKoAssertMs_ >= 1500) {
                lastKoAssertMs_ = ctx.elapsedMs;
                bool ok = engine::orderDownSubject(ctx.gw, ownHand_);
                if (!koDone_) {
                    koDone_ = true;
                    char b[112];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO MAGATE KO-DOWN rank=%u hand=%u,%u ok=%d",
                              ownRank_, ownHand_[3], ownHand_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
        if (ctx.elapsedMs >= KO_REVIVE_AT_MS && !koRevived_) {
            koRevived_ = true;
            bool ok = engine::reviveSubject(ctx.gw, ownHand_);
            char b[112];
            _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE KO-REVIVE rank=%u hand=%u,%u ok=%d",
                      ownRank_, ownHand_[3], ownHand_[4], ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Steps 7-8: diff the connected-peer snapshot tick over tick and log
    // whichever edge is actually observed - no hardcoded "which join
    // disconnects": the diff itself names the departed/returned PlayerId, so
    // this is generic regardless of which harness-killed instance it is.
    void tickPeerWatch(const ScenarioContext& ctx) {
        if (!ctx.connectedPeers) return;
        unsigned int cur[MAX_PLAYERS];
        unsigned int n = ctx.connectedPeers(cur, MAX_PLAYERS);
        bool curSet[MAX_PLAYERS];
        for (unsigned int i = 0; i < MAX_PLAYERS; ++i) curSet[i] = false;
        for (unsigned int i = 0; i < n && i < MAX_PLAYERS; ++i)
            if (cur[i] < MAX_PLAYERS) curSet[cur[i]] = true;

        if (!haveBaseline_) {
            // Give every peer's WELCOME handshake time to land before treating
            // the roster as a baseline - else every already-connected peer
            // would look like a fresh "reconnect" on the very first diff.
            if (ctx.elapsedMs < BASELINE_AT_MS) return;
            haveBaseline_ = true;
            for (unsigned int i = 0; i < MAX_PLAYERS; ++i) lastPeerSet_[i] = curSet[i];
            return;
        }
        for (unsigned int id = 0; id < MAX_PLAYERS; ++id) {
            if (lastPeerSet_[id] && !curSet[id]) {
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO MAGATE DISCONNECT peer=%u ownRank=%u t=%lu",
                          id, ownRank_, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                // SURVIVOR assertion: OUR OWN leader must still resolve after a
                // peer leaves - only the departed peer's replication state is
                // cleared (Phase 3 Plan 04's owner-scoped
                // clearPeerReplicationState; live PEER-02/PEER-03 proof reused
                // here at N=4).
                Character* oc = haveOwn_
                    ? engine::resolveCharByHand(ownHand_[3], ownHand_[4], ownHand_[0],
                                                ownHand_[1], ownHand_[2])
                    : 0;
                _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE SURVIVOR ownRank=%u ok=%d",
                          ownRank_, oc ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else if (!lastPeerSet_[id] && curSet[id]) {
                char b[96];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO MAGATE RECONNECT peer=%u ownRank=%u t=%lu",
                          id, ownRank_, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            lastPeerSet_[id] = curSet[id];
        }
    }

    static const unsigned long HOST_DURATION_MS  = 150000;
    static const unsigned long JOIN_DURATION_MS  = 140000;

    // 04-07 gap-closure (root cause 2, timing): a JOIN launched late relative
    // to the host (run_test4.ps1's reconnectAtSec DoD step) measures its own
    // JOIN_DURATION_MS from ITS OWN onStart-arm moment, not from the host's -
    // so a join that arms ~90s after the host's own onStart still runs the
    // full 140s and self-exits ~80-90s AFTER the host already exited. Its
    // census then goes permanently STALE against a dead host, and
    // analyze_wnpc_diff4.ps1 judged those post-host-exit samples as
    // "divergence" (they are not - the host simply stopped publishing).
    // run_test4.ps1 is the one actor that knows the REAL host-relative
    // offset a deferred join launches at (its lifecycle loop already computes
    // $elapsed = seconds since $hostGameplayEpoch before launching a
    // reconnectAtSec join) - it passes the remaining safe window via
    // KENSHICOOP_SCENARIO_JOIN_DURATION_SEC so a late joiner self-caps
    // instead of blindly running the full default. Unset (normal staggered
    // joins launched near host start, or any other scenario/harness) falls
    // back to JOIN_DURATION_MS unchanged.
    unsigned long joinDurationMs() const {
        const char* e = ::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC");
        if (!e || !*e) return JOIN_DURATION_MS;
        long s = ::atol(e);
        if (s < 10) s = 10; // floor: always leave a meaningful scenario window
        unsigned long ms = (unsigned long)s * 1000ul;
        return (ms < JOIN_DURATION_MS) ? ms : JOIN_DURATION_MS;
    }
    static const unsigned long MOVE_MS           = 16000;
    static const unsigned long LEG_MS            = 4000;
    static const unsigned long COMBAT_AT_MS      = 25000;
    static const unsigned long COMBAT_STOP_AT_MS = 55000;
    static const unsigned long KO_AT_MS          = 45000;
    static const unsigned long KO_REVIVE_AT_MS   = 60000;
    static const unsigned long BASELINE_AT_MS    = 10000;
    // Phase 11 plan 02 (TEST-01): the fixed N=4-only KO_TARGET_RANK=3u
    // constant is replaced by an arm-time-observed adaptive rank - see
    // koTargetRank() and RankRoster (ScenarioSupport.h).
    static const unsigned int  MAX_SQUAD         = 32;
    static const unsigned int  MAX_LOG           = 40;
    static const unsigned int  MAX_RANKS         = 8;
    static const unsigned int  MAX_HOSTILE       = 2;
    static const float         HOSTILE_REL;
    static const float         LEG;

    unsigned int  ownRank_;
    bool          haveOwn_;
    unsigned int  ownHand_[5];
    float         sx_, sy_, sz_;

    bool          hostileSpawned_;
    unsigned int  nHostile_;
    unsigned int  hostileHand_[MAX_HOSTILE][5];
    unsigned long lastCombatOrderMs_;

    bool          koDone_;
    bool          koRevived_;
    unsigned long lastKoAssertMs_;
    // Phase 11 review WR-05: the KO target rank, resolved from
    // roster_.highest() exactly ONCE at the KO leg's arming edge (first tick
    // at/after KO_AT_MS) - see the onTick latch site's comment.
    unsigned int  koTargetRank_;
    bool          koTargetLatched_;

    bool          haveBaseline_;
    bool          lastPeerSet_[MAX_PLAYERS];
    unsigned int  recvCount_;
    RankRoster    roster_; // Phase 11 plan 02 (TEST-01): adaptive rank roster
};
const float MilestoneAScenario::HOSTILE_REL = -90.0f;
const float MilestoneAScenario::LEG         = 12.0f;

} // namespace

Scenario* makeMilestoneAScenario(const std::string& name) {
    if (name == "milestone_a_gate") return new MilestoneAScenario();
    return 0;
}

} // namespace coop
