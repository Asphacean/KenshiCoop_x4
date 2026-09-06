// ScenarioPlayerState.cpp - player_state_gate (Phase 6 plan 02): the ONE
// master player-state scenario every one of the 4 processes runs to exercise
// PLAY-01/02/03 at N=4 (medical/KO/revive + stats/limbs, carry+furniture,
// stealth/prone, recruitment). Modeled directly on ScenarioMilestoneA.cpp's
// pattern: a single TimedScenario subclass, branching only on ctx.isHost and
// this client's own rank (ownRank_ = ctx.localId, the host-authoritative
// rank=playerId identity a baked 4-squad start keeps exact at N=4 - see
// ScenarioMilestoneA.cpp's own A4 note), never a hardcoded host/join1/join2/
// join3 enum. Individual steps address ONE specific rank via a fixed rank
// constant (KO_RANK/STEALTH_RANK/RECRUIT_RANK), exactly like
// ScenarioMilestoneA's KO_TARGET_RANK.
//
// Evidence discipline (locked, 06-02-PLAN.md): emit ONLY through the existing
// ScenarioSupport emitters - logScenarioEntity("MEMBER"/"RECV", ...),
// logVitalsLine(...), the SCENARIO RECRUIT line (ScenarioProbes.cpp's own
// format, reused verbatim), and the SCENARIO MAGATE start/TABMAP lines
// (ScenarioMilestoneA.cpp's own format, reused verbatim so
// CoopOraclesN.psm1's Get-MagateOwnRank/Get-MagateTabMap helpers work
// unmodified against this scenario's logs too). No new SCENARIO token is
// invented: EntityState.bodyState (the "bs=" field logScenarioEntity already
// emits) carries BODY_DOWN/RAGDOLL/DEAD (KO), BODY_CARRIED (carry),
// BODY_IN_BED/BODY_IN_CAGE/BODY_CHAINED (furniture), BODY_SNEAK (stealth) and
// the protocol-53 PRONE field (Wire.h:357-417) for EVERY squad member on
// EVERY tick already, and logVitalsLine's blood/limbState/unc/dead fields
// carry the medical/limb-loss ground truth - so the passive MEMBER/RECV +
// VITALS streams this scenario already emits are sufficient evidence for the
// carry/furniture/stealth/prone/KO legs without any bespoke "ACT" line.
//
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md) -
// this scenario reuses the MEMBER/RECV/VITALS/RECRUIT/TABMAP schemas verbatim.

#include "ScenarioSupport.h"

namespace coop {
namespace {

// player_state_gate: drives + self-observes the four PLAY-01/02/03 steps on
// every one of the 4 processes. See the file header for the evidence-emission
// rationale.
class PlayerStateScenario : public TimedScenario {
public:
    PlayerStateScenario()
        : TimedScenario("player_state_gate", 1000),
          ownRank_(0), haveOwn_(false), haveKo_(false),
          statDone_(false), woundDone_(false), healDone_(false), lastHealMs_(0),
          koDown_(false), koRevived_(false), lastKoHoldMs_(0),
          carryPicked_(false), carryDropped_(false),
          stealthOn_(false), stealthOff_(false),
          recruitDone_(false), recruitRes_(-9),
          stealthRank_(0), stealthLatched_(false),
          recruitRank_(0), recruitLatched_(false),
          haveBaseline_(false) {
        memset(ownHand_, 0, sizeof(ownHand_));
        memset(koHand_, 0, sizeof(koHand_));
        for (unsigned int i = 0; i < 3; ++i) {
            furnPutDone_[i] = false; furnPutOk_[i] = false; furnOutDone_[i] = false;
        }
        lastFurnActMs_ = 0;
        for (unsigned int i = 0; i < MAX_PLAYERS; ++i) lastPeerSet_[i] = false;
    }

    // Step 1 (connection) is already proven by arming itself (peerReady/timeout,
    // Scenario.h's own contract) - nothing extra beyond announcing our own rank
    // via the SAME "SCENARIO MAGATE start ownRank=..." line
    // ScenarioMilestoneA.cpp emits, so CoopOraclesN.psm1's Get-MagateOwnRank
    // helper resolves this scenario's logs identically.
    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.localId;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE start ownRank=%u host=%d",
                  ownRank_, ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Phase 11 plan 02 (TEST-01): keep the adaptive rank roster maximally
        // fresh every tick, before any rank-gated leg below reads it - see
        // ScenarioSupport.h's RankRoster doc comment.
        if (ctx.connectedPeers) roster_.observe(ownRank_, ctx);

        if (evidenceDue(ctx.elapsedMs)) tickEvidence(ctx);

        // Step 1 (PLAY-01): every rank raises its OWN stat, then wounds + heals
        // its OWN limb - "each owner authors real medical/.../stat/limb
        // edges". A fixed rank (KO_RANK) additionally downs + holds + revives
        // its OWN leader, so a real KO/revive edge exists for Gate A's KO leg.
        tickOwnMedical(ctx);
        if (ownRank_ == KO_RANK && haveOwn_) tickKo(ctx);

        // Step 2 (PLAY-02): the HOST carries + furniture-cycles the KO_RANK's
        // (cross-owner) downed body - carrySubject/dropSubject then
        // putSubjectInFurniture kind 1 (bed) / 2 (cage) / 3 (chain).
        if (ctx.isHost && haveKo_) tickCarryFurniture(ctx);

        // Step 3 (PLAY-02): an adaptive rank (stealthRank(), Phase 11 plan 02
        // TEST-01) toggles sneak mode on its OWN leader - the driven copy's
        // BODY_SNEAK/PRONE bits (bs= in the MEMBER/RECV stream) are the
        // evidence, no separate read-back line. stealthRank() is the
        // second-highest JOIN rank ever observed connected, falling back to
        // KO_RANK (1) when fewer than two joins have ever connected (N<3) -
        // at N=4 this resolves to 2, today's frozen STEALTH_RANK value, once
        // every join has connected (RankRoster::secondHighestJoin's doc
        // comment). At N<3 the stealth leg piggybacks onto whichever rank
        // KO_RANK already drives rather than going silently unexercised - an
        // intentional degeneration (must_haves), not a legskip: unlike Leg
        // R's rank-vs-rank CONTEST, a single-actor leg has nothing structural
        // that becomes impossible at low N.
        //
        // Phase 11 review WR-05 (same latch as ScenarioMilestoneA.cpp's
        // koTargetRank_): resolve the stealth actor ONCE, at the leg's own
        // arming edge (first tick at/after STEALTH_ON_AT_MS), so a peer whose
        // first connect lands mid-run can neither flip the dispatch off a
        // client that already sneaked ON (stranding it sneaked with no OFF
        // edge) nor mint a second stealth actor for the oracles.
        if (!stealthLatched_ && ctx.elapsedMs >= STEALTH_ON_AT_MS) {
            stealthLatched_ = true;
            stealthRank_ = stealthRank();
            char sb[96];
            _snprintf(sb, sizeof(sb) - 1,
                      "SCENARIO MAGATE STEALTH-TARGET rank=%u ownRank=%u",
                      stealthRank_, ownRank_);
            sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
        }
        if (stealthLatched_ && ownRank_ == stealthRank_ && haveOwn_) tickStealth(ctx);

        // Step 4 (PLAY-03): an adaptive rank (recruitRank() - the highest rank
        // ever observed connected, Phase 11 plan 02 TEST-01) recruits a fresh
        // RUNTIME subject EARLY in the run (save-independent - the wanderer4
        // baked-NPC reachability question stays open, 06-RESEARCH.md open
        // question 2) so a Plan-03 scheduled disconnect can still land after a
        // confirmed recruit. At N=4 this resolves to 3, today's frozen
        // RECRUIT_RANK value.
        //
        // Phase 11 review WR-05: same one-resolution-per-run latch, at this
        // leg's own arming edge (first tick at/after RECRUIT_AT_MS) - a late
        // first-connect can otherwise re-target the recruit actor mid-run.
        if (!recruitLatched_ && ctx.elapsedMs >= RECRUIT_AT_MS) {
            recruitLatched_ = true;
            recruitRank_ = recruitRank();
            char rb[96];
            _snprintf(rb, sizeof(rb) - 1,
                      "SCENARIO MAGATE RECRUIT-TARGET rank=%u ownRank=%u",
                      recruitRank_, ownRank_);
            rb[sizeof(rb) - 1] = '\0'; coop::logLine(rb);
        }
        if (recruitLatched_ && ownRank_ == recruitRank_) tickRecruit(ctx);

        // Peer connect/disconnect watch (reused verbatim from
        // ScenarioMilestoneA.cpp): lets Gate A's disconnect-skip guard find
        // ground truth for a scheduled mid-run disconnect via the SAME
        // "SCENARIO MAGATE DISCONNECT/RECONNECT/SURVIVOR" lines
        // Test-MagateDesyncConvergence already parses.
        tickPeerWatch(ctx);

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : joinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // Local legs only (script-ran verdict) - the N=4 oracle set (Gate
            // A/Gate B) judges the cross-client log evidence for the real gate.
            passed_ = haveOwn_ && statDone_ && woundDone_ && healDone_;
            return true;
        }
        return false;
    }

private:
    // ---- Continuous MEMBER/RECV/VITALS/TABMAP evidence (1 Hz) -----------------
    void tickEvidence(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, /*leaderOnly*/ false, sq, MAX_SQUAD);

        int ownIdx = tabLeaderIdx(sq, n, ownRank_);
        if (ownIdx >= 0) {
            handFromEntity(sq[ownIdx], ownHand_);
            haveOwn_ = true;
        }
        int koIdx = tabLeaderIdx(sq, n, KO_RANK);
        if (koIdx >= 0) {
            handFromEntity(sq[koIdx], koHand_);
            haveKo_ = true;
        }

        // The FULL observed rank->player map, same format as
        // ScenarioMilestoneA.cpp's logTabMap - CoopOraclesN.psm1's
        // Get-MagateTabMap keys on this line unmodified.
        logTabMap(sq, n);

        // MEMBER/RECV + VITALS for every squad member across every rank we can
        // see (bs= already carries KO/carry/furniture/stealth/prone bits;
        // VITALS already carries blood/limbState/unc/dead).
        for (unsigned int i = 0; i < n; ++i) {
            int r = tabRankOf(sq, n, i);
            if (r < 0) continue;
            logScenarioEntity(((unsigned int)r == ownRank_) ? "MEMBER" : "RECV", sq[i]);
            unsigned int h[5]; handFromEntity(sq[i], h);
            logVitalsLine(h, ctx.elapsedMs);
        }
    }

    // One "SCENARIO MAGATE TABMAP" line per distinct tab this client can
    // currently see - verbatim copy of ScenarioMilestoneA.cpp's logTabMap.
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

    // Step 1a (PLAY-01, every rank): raise our own stat, then wound + heal our
    // own limb - the raise-only scaffolds from stats_sync/medic_order, run on
    // OUR OWN leader (we are its owner - authoritative write/damage).
    void tickOwnMedical(const ScenarioContext& ctx) {
        if (!haveOwn_) return;
        if (!statDone_ && ctx.elapsedMs >= STAT_AT_MS) {
            engine::raiseSubjectStat(ctx.gw, ownHand_, /*STAT_STRENGTH*/ 1, 60.0f);
            statDone_ = true;
        }
        if (!woundDone_ && ctx.elapsedMs >= WOUND_AT_MS) {
            engine::woundSubjectLimbs(ctx.gw, ownHand_, 40.0f, 70.0f);
            woundDone_ = true;
        }
        if (woundDone_ && ctx.elapsedMs >= HEAL_AT_MS && ctx.elapsedMs < HEAL_UNTIL_MS) {
            if (!healDone_ || ctx.elapsedMs - lastHealMs_ >= 1000) {
                lastHealMs_ = ctx.elapsedMs;
                engine::healSubjectBandage(ctx.gw, ownHand_);
                healDone_ = true;
            }
        }
    }

    // Step 1b (PLAY-01, KO_RANK only): down our own leader, hold it, revive it
    // - player_ko's proven scaffold pattern, generalized off a fixed rank
    // rather than a hardcoded host/join branch, exactly like
    // ScenarioMilestoneA.cpp's tickKo.
    void tickKo(const ScenarioContext& ctx) {
        if (!koDown_ && ctx.elapsedMs >= KO_AT_MS) {
            engine::orderDownSubject(ctx.gw, ownHand_);
            koDown_ = true;
        }
        if (koDown_ && !koRevived_ && ctx.elapsedMs - lastKoHoldMs_ >= 2000 &&
            ctx.elapsedMs < KO_REVIVE_AT_MS) {
            lastKoHoldMs_ = ctx.elapsedMs;
            Character* oc = engine::resolveCharByHand(ownHand_[3], ownHand_[4],
                                                       ownHand_[0], ownHand_[1],
                                                       ownHand_[2]);
            if (oc) engine::holdDown(oc);
        }
        if (koDown_ && !koRevived_ && ctx.elapsedMs >= KO_REVIVE_AT_MS) {
            koRevived_ = true;
            engine::reviveSubject(ctx.gw, ownHand_);
        }
    }

    // Step 2 (PLAY-02, host only, cross-owner): carry KO_RANK's downed body,
    // drop it, then cycle it through bed/cage/chain (kind 1/2/3) -
    // FurnPutScenario/CarryOrderScenario's own proven cross-tab levers.
    void tickCarryFurniture(const ScenarioContext& ctx) {
        if (!carryPicked_ && ctx.elapsedMs >= CARRY_PICK_AT_MS) {
            carryPicked_ = true;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int hostIdx = tabLeaderIdx(sq, n, 0u);
            if (hostIdx >= 0) {
                unsigned int hostHand[5]; handFromEntity(sq[hostIdx], hostHand);
                engine::carrySubject(ctx.gw, hostHand, koHand_);
            }
        }
        if (carryPicked_ && !carryDropped_ && ctx.elapsedMs >= CARRY_DROP_AT_MS) {
            carryDropped_ = true;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int hostIdx = tabLeaderIdx(sq, n, 0u);
            if (hostIdx >= 0) {
                unsigned int hostHand[5]; handFromEntity(sq[hostIdx], hostHand);
                engine::dropSubject(ctx.gw, hostHand, /*ragdoll*/true);
            }
        }
        // Furniture cycle: bed (kind 1, index 0), cage (kind 2, index 1),
        // chain (kind 3, index 2) - throttled re-issue until the engine
        // accepts (the bed_pose lesson: never give up on a transient failure).
        for (unsigned int i = 0; i < 3; ++i) {
            int kind = (int)i + 1;
            unsigned long putAt = FURN_PUT_AT_MS[i];
            unsigned long outAt = FURN_OUT_AT_MS[i];
            if (carryDropped_ && ctx.elapsedMs >= putAt && ctx.elapsedMs < outAt &&
                (!furnPutDone_[i] || (!furnPutOk_[i] && ctx.elapsedMs - lastFurnActMs_ >= 3000))) {
                lastFurnActMs_ = ctx.elapsedMs;
                furnPutOk_[i] = engine::putSubjectInFurniture(ctx.gw, koHand_, kind, true);
                furnPutDone_[i] = true;
            }
            if (furnPutDone_[i] && !furnOutDone_[i] && ctx.elapsedMs >= outAt) {
                engine::putSubjectInFurniture(ctx.gw, koHand_, kind, false);
                furnOutDone_[i] = true;
            }
        }
    }

    // Step 3 (PLAY-02, STEALTH_RANK only): sneak mode on our own leader, both
    // edges - the driven copy's BODY_SNEAK bit (bs=) is the crossing evidence.
    void tickStealth(const ScenarioContext& ctx) {
        if (!stealthOn_ && ctx.elapsedMs >= STEALTH_ON_AT_MS) {
            engine::sneakSubject(ctx.gw, ownHand_, true);
            stealthOn_ = true;
        }
        if (stealthOn_ && !stealthOff_ && ctx.elapsedMs >= STEALTH_OFF_AT_MS) {
            engine::sneakSubject(ctx.gw, ownHand_, false);
            stealthOff_ = true;
        }
    }

    // Step 4 (PLAY-03, RECRUIT_RANK only): one RUNTIME recruit, EARLY in the
    // run. Reuses ScenarioProbes.cpp's own "SCENARIO RECRUIT ..." format
    // verbatim (the oracle API a Gate B implementation keys on).
    void tickRecruit(const ScenarioContext& ctx) {
        if (recruitDone_ || ctx.elapsedMs < RECRUIT_AT_MS) return;
        recruitDone_ = true;
        unsigned int hb[5], ha[5];
        recruitRes_ = engine::probeRecruit(ctx.gw, /*runtimeSubject*/true, hb, ha);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO RECRUIT who=%s leg=%s res=%d "
                  "before=%u,%u,%u,%u,%u after=%u,%u,%u,%u,%u t=%lu",
                  ctx.isHost ? "host" : "join", "runtime", recruitRes_,
                  hb[0], hb[1], hb[2], hb[3], hb[4],
                  ha[0], ha[1], ha[2], ha[3], ha[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Peer connect/disconnect diff watch - verbatim copy of
    // ScenarioMilestoneA.cpp's tickPeerWatch (same log lines, same rationale:
    // a scheduled mid-run disconnect is the harness's job, not this
    // scenario's; this only detects + asserts on whichever edge happens).
    void tickPeerWatch(const ScenarioContext& ctx) {
        if (!ctx.connectedPeers) return;
        unsigned int cur[MAX_PLAYERS];
        unsigned int n = ctx.connectedPeers(cur, MAX_PLAYERS);
        bool curSet[MAX_PLAYERS];
        for (unsigned int i = 0; i < MAX_PLAYERS; ++i) curSet[i] = false;
        for (unsigned int i = 0; i < n && i < MAX_PLAYERS; ++i)
            if (cur[i] < MAX_PLAYERS) curSet[cur[i]] = true;

        if (!haveBaseline_) {
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

    // joinDurationMs(): honors KENSHICOOP_SCENARIO_JOIN_DURATION_SEC for a
    // deferred/late-joining instance, verbatim from ScenarioMilestoneA.cpp.
    unsigned long joinDurationMs() const {
        const char* e = ::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC");
        if (!e || !*e) return JOIN_DURATION_MS;
        long s = ::atol(e);
        if (s < 10) s = 10;
        unsigned long ms = (unsigned long)s * 1000ul;
        return (ms < JOIN_DURATION_MS) ? ms : JOIN_DURATION_MS;
    }

    static const unsigned long HOST_DURATION_MS  = 150000;
    static const unsigned long JOIN_DURATION_MS  = 140000;
    static const unsigned long BASELINE_AT_MS    = 10000;

    // Step timeline (>= ~20s margin before joinDurationMs()'s 140000 floor;
    // the last scripted step, KO_REVIVE_AT_MS, still leaves >= 50s of margin).
    static const unsigned long RECRUIT_AT_MS     = 8000;  // EARLY (disconnect can land after)
    static const unsigned long STAT_AT_MS        = 14000;
    static const unsigned long WOUND_AT_MS       = 18000;
    static const unsigned long HEAL_AT_MS        = 24000;
    static const unsigned long HEAL_UNTIL_MS     = 31000;
    static const unsigned long STEALTH_ON_AT_MS  = 30000;
    static const unsigned long STEALTH_OFF_AT_MS = 88000;
    static const unsigned long KO_AT_MS          = 36000;
    static const unsigned long CARRY_PICK_AT_MS  = 42000;
    static const unsigned long CARRY_DROP_AT_MS  = 48000;
    static const unsigned long KO_REVIVE_AT_MS   = 84000;

    static const unsigned int  KO_RANK           = 1u;
    // Phase 11 plan 02 (TEST-01): the fixed N=4-only STEALTH_RANK=2u/
    // RECRUIT_RANK=3u constants are replaced by arm-time-observed adaptive
    // ranks - see stealthRank()/recruitRank() and RankRoster
    // (ScenarioSupport.h).
    static const unsigned int  MAX_SQUAD         = 32;
    static const unsigned int  MAX_RANKS         = 8;

    unsigned int  ownRank_;
    bool          haveOwn_;
    bool          haveKo_;
    unsigned int  ownHand_[5];
    unsigned int  koHand_[5];

    bool          statDone_;
    bool          woundDone_;
    bool          healDone_;
    unsigned long lastHealMs_;

    bool          koDown_;
    bool          koRevived_;
    unsigned long lastKoHoldMs_;

    bool          carryPicked_;
    bool          carryDropped_;
    bool          furnPutDone_[3];
    bool          furnPutOk_[3];
    bool          furnOutDone_[3];
    unsigned long lastFurnActMs_;
    static const unsigned long FURN_PUT_AT_MS[3];
    static const unsigned long FURN_OUT_AT_MS[3];

    bool          stealthOn_;
    bool          stealthOff_;

    bool          recruitDone_;
    int           recruitRes_;

    // Phase 11 review WR-05: the stealth/recruit actor ranks, each resolved
    // from the roster exactly ONCE at its leg's arming edge - see the onTick
    // latch sites (and ScenarioMilestoneA.cpp's koTargetRank_ for the full
    // mid-run re-resolution rationale).
    unsigned int  stealthRank_;
    bool          stealthLatched_;
    unsigned int  recruitRank_;
    bool          recruitLatched_;

    bool          haveBaseline_;
    bool          lastPeerSet_[MAX_PLAYERS];
    RankRoster    roster_; // Phase 11 plan 02 (TEST-01): adaptive rank roster

    // Second-highest JOIN rank ever observed connected, else KO_RANK (see the
    // onTick call site's comment for the N<3 degeneration rationale).
    unsigned int stealthRank() const { return roster_.secondHighestJoin(KO_RANK); }
    // Highest rank ever observed connected (itself included).
    unsigned int recruitRank() const { return roster_.highest(); }
};

const unsigned long PlayerStateScenario::FURN_PUT_AT_MS[3] = { 52000, 62000, 72000 };
const unsigned long PlayerStateScenario::FURN_OUT_AT_MS[3] = { 58000, 68000, 78000 };

} // namespace

Scenario* makePlayerStateScenario(const std::string& name) {
    if (name == "player_state_gate") return new PlayerStateScenario();
    return 0;
}

} // namespace coop
