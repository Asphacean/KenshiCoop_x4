// ScenarioSaveLoad.cpp - save_load_gate (Phase 10 plan 03, SAVE-01..04): the
// ONE live 4-instance scenario proving the per-client save/load coordinator
// (Plan 01) and the joiner-unicast bootstrap (Plan 02) end to end on real
// UDP, following ScenarioConsensus.cpp's domain-TU shape (a rank-generic
// TimedScenario driven by ctx.localId, host-only phase markers bounding the
// oracle's judged windows, census-verified mutation with a retry deadline so
// a lever false-negative never burns one of the max-4 live runs).
//
// Four legs ride ONE FIXED TIMELINE (host clock), in the order L->S->R->H -
// a DELIBERATE divergence from the research's sketched order (save->reject->
// load->late-join): Leg S's 4-client save and Leg H's host-load-while-3-
// connected both REQUIRE the 4th instance (join3) to have already joined, so
// the late-join leg must run first. Each leg emits its evidence
// independently (the CONSERVE retry-until-observed discipline) so an early
// leg's failure still yields later legs' evidence at whatever N is
// connected:
//
//   Leg L (late-join, host t=0..120s): the THREE ESTABLISHED instances
//     (host + join1 + join2, already connected when this scenario arms) run
//     a continuous 1 Hz census heartbeat - "SCENARIO SAVELOAD census ..." -
//     that samples BOTH this instance's own rank leader position AND
//     rank 3's squad-tab leader position if locally visible (the research
//     Open Question 1 rank-3-driver probe: wanderer4 bakes all four squads,
//     so rank 3's tab exists as save-baked furniture on every instance from
//     t=0 - the probe's job is to let the oracle see whether it sits idle
//     before join3 connects). The establisheds do NOT pause or gate on
//     join3 - the window is passive by design (the bootstrap itself is
//     production code under test, run_test4.ps1's reconnectAtSec deferred-
//     first-launch lever per Plan 04's rig config). join3 (ctx.localId==3)
//     is a genuine LATE launch: Scenario::onStart only fires once THIS
//     instance's own gameplay has started (Scenario.h's onStart contract),
//     so join3 structurally "does nothing until worldLive" with zero extra
//     gating here - the moment its onStart fires (after its own connect-
//     push bootstrap + load complete), it emits a "SCENARIO SAVELOAD
//     joined" marker and starts its OWN census heartbeat (rank=3), which
//     IS the positive "join3 drives its own squad after load" evidence the
//     oracle's joiner-convergence sub-check reads.
//
//   Leg S (coordinated 4-client save, host t=125..175s): the HOST issues
//     ONE engine::saveGameAs (a scenario-fixed name distinct from any
//     fixture name) - production's per-client SaveCoord.h collects three
//     distinct owner ACKs from join1/join2/join3. Census-verified: the
//     scenario polls savexfer::sendXferId() for progress after issuing: no
//     progress within a deadline (an autosave collision drawing a
//     [coord] REJECT, or a genuine engine hiccup) re-issues the save, up to
//     a bounded retry count - the lever false-negative discipline.
//
//   Leg R (concurrent rejection, host t=180..235s): rank 1 and rank 2 (both
//     established joins) each call engine::saveGameAs with a DISTINCT
//     scenario-fixed name at the SAME host-clock tick (sight-anchored on
//     the shared host timeline, not a wall-clock race). Because a JOIN
//     under save-sync is suppressed at the SAME SaveManager::save detour a
//     host save re-enters (EngineInternal.cpp's saveMgrSave_hook - "even
//     the direct g_saveFn call re-enters the detour"), this naturally
//     routes through the REQ->host->arbitrate path: production's
//     CoordArbiter admits whichever REQ it drains first and rejects the
//     other with PKT_COORD_REJECT (both request ids named in the host's
//     log). Neither join has a scenario-visible accessor for "was I
//     rejected" (that state lives entirely in Plugin.cpp/CoordArbiter, not
//     exposed through Engine.h - adding one would be new production surface
//     this plan's constraints forbid), so BOTH ranks unconditionally issue
//     a SECOND attempt ~20s later (a fixed, generous host-clock offset well
//     past the first save's settle) and log every attempt as
//     "SCENARIO SAVELOAD reqsave rank=.. try=.." - the ORACLE (Test-
//     SaveLoad, CoopOraclesN.psm1) is what matches the host's
//     "[coord] REJECT requester=.. reqId=.. reason=busy active=.."  to the
//     loser's own "[coord] REJECTED reqId=.." receipt and confirms the
//     retry lands with no second reject (the requester-may-retry half of
//     the locked SAVE-02/03 decision).
//
//   Leg H (host-load-while-3-connected, host t=240..305s): the HOST calls
//     engine::loadSave on Leg S's OWN save name (the mid-session broadcast-
//     GO load path, proven alongside Plan 02's separate joiner-unicast
//     bootstrap path so both keep working) - production's per-client
//     LoadCoord.h collects a positive PKT_LOAD_ACK from every connected
//     join. Census-verified: the scenario watches its OWN gameplayLive drop
//     then return (the WORLD-RELOAD edge, LoadSyncScenario's precedent) and
//     retries the load once if no swap is observed within a deadline.
//
// Evidence discipline (locked, mirrors ScenarioConsensus.cpp): this file
// emits "SCENARIO MAGATE start ..." verbatim (Get-MagateOwnRank's own-rank
// resolver, the schema every N=4 gate - including clean_exit's
// scheduled-disconnect-exempt health check - keys on) plus five NEW line
// kinds under one "SCENARIO SAVELOAD ..." prefix: "start", "joined",
// "census", "leg=<L|S|R|H> phase=<begin|end>", "savehost", "reqsave", and
// "loadhost". No production "[boot]/[save]/[coord]/[load]" log string is
// touched - Test-SaveLoad reads those verbatim from Plans 01/02.
//
// Must NOT: change any SCENARIO/production log string (oracle API,
// resources/CODE_MAP.md). No wire change - PROTOCOL_VERSION stays 61.

#include "ScenarioSupport.h"

namespace coop {
namespace {

class SaveLoadScenario : public TimedScenario {
public:
    SaveLoadScenario()
        : TimedScenario("save_load_gate", 1000),
          ownRank_(0),
          joinedMarked_(false),
          markedLEnd_(false), markedSBegin_(false), markedSEnd_(false),
          markedRBegin_(false), markedREnd_(false),
          markedHBegin_(false), markedHEnd_(false),
          legSIssued_(false), legSDone_(false), legSTry_(0),
          legSBaselineXfer_(0), legSIssueAtMs_(0),
          legRCommitSeqBase_(0), legRSeenLegS_(false), legRSeenAtMs_(0),
          legRTry1_(false), legRTry2_(false),
          legHIssued_(false), legHRetried_(false),
          legHWasLive_(true), legHDropSeen_(false), legHSwapDone_(false),
          legHIssueAtMs_(0),
          isLateLaunch_(false), legRSkipLogged_(false) {}

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.localId;
        // "SCENARIO MAGATE start ownRank=.. host=.." reused verbatim
        // (ScenarioConsensus.cpp / ScenarioWorldState.cpp precedent) so
        // Get-MagateOwnRank resolves this scenario's own-rank identity with
        // the same helper every N=4 gate uses (including clean_exit's own
        // health check).
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE start ownRank=%u host=%d",
                  ownRank_, ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);

        char sb[64];
        _snprintf(sb, sizeof(sb) - 1, "SCENARIO SAVELOAD start rank=%u", ownRank_);
        sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);

        // Leg L begins the instant the HOST arms - the host-only marker
        // gives the oracle a t=0 anchor in its own wall-clock frame (the
        // ScenarioConsensus.cpp/ScenarioWorldState.cpp claimphase idiom).
        if (ctx.isHost) emitLegMark('L', "begin", 0);

        // join3's onStart structurally cannot fire before ITS OWN gameplay
        // is live (Scenario.h's onStart contract - arm at peer-ready or the
        // arm-timeout fallback, both gated on g_gameStarted for THIS
        // instance) - so by the time this branch runs, join3 has already
        // ridden the connect-push bootstrap to a live world. No additional
        // worldLive gate is needed here.
        //
        // Phase 11 plan 02 (TEST-01) identity fix: "am I the deliberately
        // deferred/late-launching instance" is NOT resolved via a rank
        // comparison here, deliberately - a roster-based "highest rank I've
        // seen connected so far" READ AT ONSTART would misfire for whichever
        // ESTABLISHED join's onStart happens to fire first (its own rank is
        // trivially "the highest seen so far" for as long as the true late
        // rank has not yet connected - which, by this leg's whole premise,
        // is exactly when established instances arm; see the file header's
        // Leg L note). The ROBUST, timing-independent signal is the SAME one
        // joinDurationMs() already reads: run_test4.ps1 sets
        // KENSHICOOP_SCENARIO_JOIN_DURATION_SEC ONLY on the rig entry that
        // carries reconnectAtSec (the deferred-first-launch lever) -
        // regardless of which rank number that entry ends up being at
        // N=2/N=3/N=4, so this is exact and race-free where a rank-arithmetic
        // read at onStart is not. At N=4 this is byte-identical to the old
        // hardcoded `ownRank_ == 3u` (only join3's rig entry ever sets the
        // env var).
        isLateLaunch_ = (::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC") != 0);
        if (isLateLaunch_) {
            char jb[80];
            _snprintf(jb, sizeof(jb) - 1, "SCENARIO SAVELOAD joined rank=%u t=%lu",
                      ownRank_, ctx.elapsedMs);
            jb[sizeof(jb) - 1] = '\0'; coop::logLine(jb);
            joinedMarked_ = true;
        }

        // Leg R clock-skew fix (10-04 live gate run 3): a baseline taken
        // BEFORE Leg S ever runs, so tickLegR can later detect "a NEW
        // commit landed" via coop::savexfer::commitSeq() rather than
        // ctx.elapsedMs (see tickLegR's own comment for why elapsedMs is
        // unusable here). Captured for EVERY join, not just the two that
        // turn out to be Leg R's contestants (roster.lowestTwoJoins() is not
        // yet resolvable at onStart time for the same reason the identity
        // fix above exists) - a value nobody reads is harmless, and at N=4
        // this is byte-identical: rank 3 (the late-launching join) never
        // matches roster_.lowestTwoJoins()'s (1,2), so it never calls
        // tickLegR and its now-also-captured baseline stays unused, exactly
        // as before.
        if (ownRank_ != 0u) {
            legRCommitSeqBase_ = coop::savexfer::commitSeq();
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Phase 11 plan 02 (TEST-01): keep the adaptive rank roster maximally
        // fresh every tick, before any rank-gated leg below reads it - see
        // ScenarioSupport.h's RankRoster doc comment. Leg R's contestants
        // (roster_.lowestTwoJoins()) are only consulted starting at
        // LEGR_BEGIN_MS (~130s host-clock in) - ample margin for the roster
        // to have converged even under a staggered join launch.
        if (ctx.connectedPeers) roster_.observe(ownRank_, ctx);

        if (evidenceDue(ctx.elapsedMs)) emitCensus(ctx);

        if (ctx.isHost) {
            tickLegMarkers(ctx);
            tickLegS(ctx);
            tickLegH(ctx);
        } else if (ownRank_ != 0u) {
            unsigned int lo = 0, hi = 0;
            if (roster_.lowestTwoJoins(&lo, &hi)) {
                if (ownRank_ == lo || ownRank_ == hi) tickLegR(ctx);
            } else if (!legRSkipLogged_ && ctx.elapsedMs >= LEGR_BEGIN_MS) {
                // N-inapplicable (N<3: fewer than two joins have ever
                // connected, so Leg R's rank-vs-rank contest has only one
                // possible contestant) - an honest, named SKIP rather than a
                // silent/vacuous local pass (must_haves: TEST-01).
                legRSkipLogged_ = true;
                char b[80];
                _snprintf(b, sizeof(b) - 1, "SCENARIO SAVELOAD legskip leg=R n=%u",
                          roster_.highest() + 1u);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : joinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // Local leg-ran verdict only - Test-SaveLoad (CoopOraclesN.psm1)
            // judges the cross-instance census-true evidence. The live-run
            // oracle's evidentiary-gap discipline (NoSignalFails) is the
            // authoritative judgment, exactly like ScenarioConsensus.cpp's
            // own local-verdict caveat.
            unsigned int lo = 0, hi = 0;
            bool legRApplicable = !ctx.isHost && roster_.lowestTwoJoins(&lo, &hi) &&
                                   (ownRank_ == lo || ownRank_ == hi);
            if (ctx.isHost) {
                passed_ = legSDone_ && legHIssued_;
            } else if (legRApplicable) {
                passed_ = legRTry1_ && legRTry2_;
            } else if (isLateLaunch_) {
                passed_ = joinedMarked_;
            } else {
                passed_ = true;
            }
            return true;
        }
        return false;
    }

private:
    // ==== continuous 1 Hz census heartbeat (every rank, every tick) ==========

    // Own-rank leader position + the rank-3-driver probe (Open Question 1):
    // whether rank 3's squad-tab leader is locally visible at all (wanderer4
    // bakes it as world furniture from t=0 on every instance) and where it
    // sits - the oracle's job is comparing this series across the join
    // window to judge "nobody drove it before join3 connected".
    void emitCensus(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);

        int haveOwn = 0; float px = 0.0f, py = 0.0f, pz = 0.0f;
        int idx = tabLeaderIdx(sq, n, ownRank_);
        if (idx >= 0) { haveOwn = 1; px = sq[idx].x; py = sq[idx].y; pz = sq[idx].z; }

        int r3seen = 0; float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        // PROBE_RANK (fixed at 3, MAX_PLAYERS-1) - NOT adaptive, deliberately:
        // this probes wanderer4's always-baked 4th squad tab (world/save
        // furniture present from t=0 REGARDLESS of how many real PlayerIds
        // are connected - tabLeaderIdx classifies by OBSERVED container
        // ordinal, not by live roster membership), so an adaptive "highest
        // connected rank" would misdirect this probe at an ALREADY-driven
        // established rank's tab instead of the genuinely unowned one - the
        // opposite of research Open Question 1's intent (whether an unowned
        // baked squad sits idle before its real owner connects, at ANY N).
        // Byte-identical to the old hardcoded LATE_RANK read at N=4.
        int r3idx = tabLeaderIdx(sq, n, PROBE_RANK);
        if (r3idx >= 0) { r3seen = 1; rx = sq[r3idx].x; ry = sq[r3idx].y; rz = sq[r3idx].z; }

        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SAVELOAD census rank=%u haveOwn=%d pos=%.1f,%.1f,%.1f "
                  "r3seen=%d r3pos=%.1f,%.1f,%.1f t=%lu",
                  ownRank_, haveOwn, px, py, pz, r3seen, rx, ry, rz, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== host-only phase markers (bound the oracle's judged windows) ========

    void tickLegMarkers(const ScenarioContext& ctx) {
        if (!markedLEnd_   && ctx.elapsedMs >= LEGL_END_MS)   { markedLEnd_   = true; emitLegMark('L', "end",   ctx.elapsedMs); }
        if (!markedSBegin_ && ctx.elapsedMs >= LEGS_BEGIN_MS) { markedSBegin_ = true; emitLegMark('S', "begin", ctx.elapsedMs); }
        if (!markedSEnd_   && ctx.elapsedMs >= LEGS_END_MS)   { markedSEnd_   = true; emitLegMark('S', "end",   ctx.elapsedMs); }
        if (!markedRBegin_ && ctx.elapsedMs >= LEGR_BEGIN_MS) { markedRBegin_ = true; emitLegMark('R', "begin", ctx.elapsedMs); }
        if (!markedREnd_   && ctx.elapsedMs >= LEGR_END_MS)   { markedREnd_   = true; emitLegMark('R', "end",   ctx.elapsedMs); }
        if (!markedHBegin_ && ctx.elapsedMs >= LEGH_BEGIN_MS) { markedHBegin_ = true; emitLegMark('H', "begin", ctx.elapsedMs); }
        if (!markedHEnd_   && ctx.elapsedMs >= LEGH_END_MS)   { markedHEnd_   = true; emitLegMark('H', "end",   ctx.elapsedMs); }
    }

    static void emitLegMark(char leg, const char* phase, unsigned long t) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SAVELOAD leg=%c phase=%s t=%lu",
                  leg, phase, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== Leg S: host coordinated 4-client save ===============================

    // Census-verified with a retry deadline: after issuing, poll
    // savexfer::sendXferId() for progress (a genuine transfer arming) rather
    // than trusting saveGameAs's bool return alone (it only reports "the
    // call didn't fault", not "the arbiter admitted it" - a rejected/failed
    // edge never advances sendXferId()). No progress within the deadline
    // re-issues, up to LEGS_MAX_TRIES - the lever false-negative discipline
    // so a single missed arm never burns this leg's evidence.
    void tickLegS(const ScenarioContext& ctx) {
        if (legSDone_) return;
        if (ctx.elapsedMs < LEGS_SAVE_AT_MS) return;
        if (!legSIssued_) {
            issueLegSSave(ctx);
            return;
        }
        if (coop::savexfer::sendXferId() != legSBaselineXfer_) {
            legSDone_ = true; // transfer armed - production's per-client
                               // SaveCoord.h + Test-SaveLoad take it from here
            return;
        }
        if (legSTry_ < LEGS_MAX_TRIES &&
            ctx.elapsedMs >= legSIssueAtMs_ + LEGS_RETRY_DEADLINE_MS) {
            issueLegSSave(ctx);
        }
    }

    void issueLegSSave(const ScenarioContext& ctx) {
        legSBaselineXfer_ = coop::savexfer::sendXferId();
        bool ok = coop::engine::saveGameAs(LEGS_SAVE_NAME);
        legSIssued_ = true;
        legSIssueAtMs_ = ctx.elapsedMs;
        ++legSTry_;
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SAVELOAD savehost name='%s' ok=%d try=%u t=%lu",
                  LEGS_SAVE_NAME, ok ? 1 : 0, legSTry_, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== Leg R: concurrent rejection (the two lowest join ranks only) ========

    // Neither join has a scenario-visible accessor for "was my request
    // rejected" (CoordArbiter state lives entirely in Plugin.cpp; exposing
    // it would be new production surface this plan's constraints forbid),
    // so both ranks issue their SECOND attempt unconditionally at a fixed,
    // generous offset (~20s after the first) rather than event-driven on a
    // reject receipt. The ORACLE matches REJECT/REJECTED from the
    // production log and judges whether the retry landed clean.
    //
    // 10-04 live gate finding (run 3): the offset used to be measured from
    // ctx.elapsedMs, i.e. THIS RANK's OWN onStart (Scenario.h's contract -
    // arm at peer-ready). onStart fires whenever THIS instance individually
    // reaches peer-ready, which is NOT synchronized across instances -
    // run_test4.ps1 launches joins staggered (JoinDelaySec) and each one's
    // own load time varies independently, so rank 1 and rank 2's onStart
    // moments can differ by tens of seconds of real wall-clock time (39s
    // observed live). Scheduling both ranks' "concurrent" attempt from
    // their OWN onStart therefore does not make them concurrent at all -
    // Leg R silently never collided in run 3 (0 rejects observed) even
    // though every other leg passed clean.
    //
    // Fix: anchor the offset to a SHARED real-time event instead - each
    // rank's OWN observation of Leg S's coordinated-save commit
    // (coop::savexfer::commitSeq()/lastCommitName()/lastCommitResult(),
    // already-existing join-side accessors, SaveXfer.h - no new production
    // surface). Leg S's broadcast lands at essentially the SAME wall-clock
    // instant for every connected client regardless of when their onStart
    // fired (it is governed by network delivery of a HOST-issued transfer,
    // not by any instance's own local scenario clock), so scheduling from
    // "N ms after I observed Leg S commit" is genuinely synchronized
    // between rank 1 and rank 2.
    // Called only when ownRank_ is one of roster_.lowestTwoJoins()'s two
    // contestants (onTick's dispatch already verified this) - lo/hi are
    // re-resolved here rather than threaded through as parameters purely for
    // a stable, self-contained signature; by LEGR_BEGIN_MS the roster has
    // long converged, so this reads the SAME (lo,hi) the caller just saw.
    void tickLegR(const ScenarioContext& ctx) {
        unsigned int lo = 0, hi = 0;
        if (!roster_.lowestTwoJoins(&lo, &hi)) return; // defensive; caller already checked
        if (!legRSeenLegS_) {
            if (coop::savexfer::commitSeq() != legRCommitSeqBase_ &&
                coop::savexfer::lastCommitResult() == 1 &&
                coop::savexfer::lastCommitName() == LEGS_SAVE_NAME) {
                legRSeenLegS_ = true;
                legRSeenAtMs_ = ctx.elapsedMs;
            }
            return; // Leg R cannot safely start until Leg S is observed done
        }
        unsigned long since = ctx.elapsedMs - legRSeenAtMs_;
        const char* name = (ownRank_ == lo) ? LEGR_NAME_R1 : LEGR_NAME_R2;
        if (!legRTry1_ && since >= LEGR_TRY1_DELAY_MS) {
            legRTry1_ = true;
            bool ok = coop::engine::saveGameAs(name);
            emitReqSave(name, ok, 1, ctx.elapsedMs);
        }
        // 10-04 live gate finding (run 4): try=2's delay MUST differ between
        // rank 1 and rank 2. Both ranks now observe Leg S's commit at the
        // SAME real-world instant (the fix above), so giving them the SAME
        // try=2 delay too made their retries collide a SECOND time in
        // lockstep (run 4: join2's try=2 was itself rejected - by join1's
        // OWN try=2, not by anything still-active from try=1 - defeating
        // the "requester may retry after completion" guarantee the leg
        // exists to prove). Whichever rank LOST try=1 needs its retry to
        // land well after the WINNER's try=1 has settled (a few seconds,
        // observed live); staggering try=2 by rank guarantees that in
        // EITHER win/lose ordering, since the two ranks' retries can no
        // longer coincide.
        unsigned long try2DelayMs = (ownRank_ == lo) ? LEGR_TRY2_DELAY_MS_R1
                                                       : LEGR_TRY2_DELAY_MS_R2;
        if (legRTry1_ && !legRTry2_ && since >= try2DelayMs) {
            legRTry2_ = true;
            bool ok = coop::engine::saveGameAs(name);
            emitReqSave(name, ok, 2, ctx.elapsedMs);
        }
    }

    void emitReqSave(const char* name, bool ok, unsigned int tryNum, unsigned long t) {
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SAVELOAD reqsave rank=%u name='%s' ok=%d try=%u t=%lu",
                  ownRank_, name, ok ? 1 : 0, tryNum, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== Leg H: host mid-session load while 3 joins connected =================

    // Reloads Leg S's OWN save name (ties H directly to S's baked output,
    // and proves the mid-session BROADCAST-GO load path keeps working
    // alongside Plan 02's separate joiner-unicast bootstrap path). Census-
    // verified: watches this instance's OWN gameplayLive drop-then-return
    // (the WORLD-RELOAD edge, LoadSyncScenario.cpp's precedent) and retries
    // once if no swap is observed within a deadline.
    void tickLegH(const ScenarioContext& ctx) {
        if (ctx.elapsedMs < LEGH_LOAD_AT_MS) return;
        bool live = coop::engine::gameplayLive(ctx.gw);
        if (!legHIssued_) {
            bool ok = coop::engine::loadSave(LEGS_SAVE_NAME);
            legHIssued_ = true;
            legHIssueAtMs_ = ctx.elapsedMs;
            legHWasLive_ = live;
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SAVELOAD loadhost name='%s' ok=%d t=%lu",
                      LEGS_SAVE_NAME, ok ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }
        if (legHSwapDone_) return;
        if (legHWasLive_ && !live) legHDropSeen_ = true;
        if (legHDropSeen_ && live) legHSwapDone_ = true;
        legHWasLive_ = live;
        if (!legHSwapDone_ && !legHRetried_ &&
            ctx.elapsedMs >= legHIssueAtMs_ + LEGH_RETRY_DEADLINE_MS) {
            legHRetried_ = true;
            bool ok = coop::engine::loadSave(LEGS_SAVE_NAME);
            legHIssueAtMs_ = ctx.elapsedMs;
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SAVELOAD loadhost name='%s' ok=%d t=%lu (retry)",
                      LEGS_SAVE_NAME, ok ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // joinDurationMs(): honors KENSHICOOP_SCENARIO_JOIN_DURATION_SEC for a
    // deferred/late-joining instance, verbatim from ScenarioConsensus.cpp /
    // ScenarioWorldState.cpp / ScenarioItemConservation.cpp. Only join3's
    // rig entry (Plan 04's local.rig.json) sets the underlying env var via
    // run_test4.ps1's reconnectAtSec lifecycle monitor (-JoinDurationSec) -
    // the two established joins fall through to the compiled default.
    unsigned long joinDurationMs() const {
        const char* e = ::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC");
        if (!e || !*e) return JOIN_DURATION_MS;
        long s = ::atol(e);
        if (s < 10) s = 10;
        unsigned long ms = (unsigned long)s * 1000ul;
        return (ms < JOIN_DURATION_MS) ? ms : JOIN_DURATION_MS;
    }

    static const unsigned int MAX_SQUAD = 32;
    // PROBE_RANK: the always-baked 4th squad tab (wanderer4 world/save
    // furniture, N-independent) - see emitCensus's comment. NOT the same
    // concept as "the late-launching instance" (isLateLaunch_, an env-var
    // signal) or "Leg R's contestants" (roster_.lowestTwoJoins()) - Phase 11
    // plan 02 (TEST-01) replaces the old single LATE_RANK=3u constant (which
    // conflated all three) with these three independently-correct mechanisms.
    static const unsigned int PROBE_RANK = 3;

    // ---- Leg schedule (host clock, ms) -----------------------------------------
    static const unsigned long LEGL_END_MS         = 120000; // Leg L window closes
    static const unsigned long LEGS_BEGIN_MS       = 125000;
    static const unsigned long LEGS_SAVE_AT_MS     = 130000;
    static const unsigned long LEGS_RETRY_DEADLINE_MS = 20000;
    static const unsigned int  LEGS_MAX_TRIES      = 3;
    static const unsigned long LEGS_END_MS         = 175000;
    // LEGR_BEGIN_MS matches LEGS_SAVE_AT_MS (not a later, disjoint offset
    // like the other legs) - Leg R's rank-1/2 activity is now gated on
    // OBSERVING Leg S's commit (tickLegR), so it structurally cannot start
    // before Leg S does; this is just a generous lower search bound for the
    // oracle's host-clock window, not something that forces early activity.
    static const unsigned long LEGR_BEGIN_MS       = LEGS_SAVE_AT_MS;
    // Delays are relative to EACH RANK'S OWN observation of Leg S's commit
    // (legRSeenAtMs_, tickLegR) - see tickLegR's comment for why this
    // replaces a fixed ctx.elapsedMs offset (onStart-relative clocks are
    // not synchronized across instances; Leg S's broadcast commit is).
    static const unsigned long LEGR_TRY1_DELAY_MS  = 8000;  // let Leg S fully settle first
    // Staggered per rank (10-04 run 4 finding, tickLegR's own comment) so
    // the two ranks' SECOND attempts cannot collide with EACH OTHER the
    // same way their synchronized first attempts are designed to.
    static const unsigned long LEGR_TRY2_DELAY_MS_R1 = 28000; // ~20s after try=1
    static const unsigned long LEGR_TRY2_DELAY_MS_R2 = 48000; // ~40s after try=1
    static const unsigned long LEGR_END_MS         = 235000;
    static const unsigned long LEGH_BEGIN_MS       = 240000;
    static const unsigned long LEGH_LOAD_AT_MS     = 245000;
    static const unsigned long LEGH_RETRY_DEADLINE_MS = 30000;
    static const unsigned long LEGH_END_MS         = 305000;

    // HOST outlives Leg H's judged window with margin; established JOINs
    // trail by the same margin class ScenarioConsensus.cpp/
    // ScenarioWorldState.cpp use (~15s: staggered exit + shutdown overhead).
    // The manifest's HostSelfExitSec (scenarios.psd1) MUST equal
    // HOST_DURATION_MS/1000 (320) - the reconnectAtSec deferred-launch cap
    // run_test4.ps1 applies to join3 is keyed off this value (research
    // Pitfall 9).
    static const unsigned long HOST_DURATION_MS = 320000;
    static const unsigned long JOIN_DURATION_MS = 305000;

    unsigned int ownRank_;
    bool joinedMarked_;

    bool markedLEnd_, markedSBegin_, markedSEnd_;
    bool markedRBegin_, markedREnd_;
    bool markedHBegin_, markedHEnd_;

    bool          legSIssued_, legSDone_;
    unsigned int  legSTry_;
    u32           legSBaselineXfer_;
    unsigned long legSIssueAtMs_;

    u32           legRCommitSeqBase_;
    bool          legRSeenLegS_;
    unsigned long legRSeenAtMs_;
    bool legRTry1_, legRTry2_;

    bool          legHIssued_, legHRetried_;
    bool          legHWasLive_, legHDropSeen_, legHSwapDone_;
    unsigned long legHIssueAtMs_;

    RankRoster    roster_;         // Phase 11 plan 02 (TEST-01): adaptive rank roster
    bool          isLateLaunch_;   // set in onStart from the env-var identity signal
    bool          legRSkipLogged_; // N<3 legskip marker latch (onTick)

    static const char* const LEGS_SAVE_NAME;
    static const char* const LEGR_NAME_R1;
    static const char* const LEGR_NAME_R2;
};

const char* const SaveLoadScenario::LEGS_SAVE_NAME = "svloadS";
const char* const SaveLoadScenario::LEGR_NAME_R1   = "svloadR1";
const char* const SaveLoadScenario::LEGR_NAME_R2   = "svloadR2";

} // namespace

Scenario* makeSaveLoadScenario(const std::string& name) {
    if (name == "save_load_gate") return new SaveLoadScenario();
    return 0;
}

} // namespace coop
