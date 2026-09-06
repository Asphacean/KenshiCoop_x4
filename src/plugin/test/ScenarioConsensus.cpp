// ScenarioConsensus.cpp - consensus_gate (Phase 9 plan 03, CONS-01/02/03): the
// ONE live 4-instance scenario proving all three global-consensus planes
// (shared money pool, speed min-vote, time convergence) end to end on real
// UDP, following ScenarioWorldState.cpp's domain-TU shape (a rank-generic
// TimedScenario driven by ctx.localId, host-only phase markers bounding the
// oracle's judged windows, census-verified mutation with a retry deadline so
// a lever false-negative never burns one of the max-4 live runs).
//
// Three legs ride ONE FIXED TIMELINE with no cross-leg dependency, so an
// early leg's failure still emits later legs' evidence (the honest-gate
// discipline, research "Honest discipline"):
//
//   Leg A (money, t=8..75s): host seeds the pool to a deterministic BASE;
//     all three joins spend fixed, individually-solvent amounts (near t=20s -
//     solvent, no contention: SOLVENT_AMOUNT[rank] is far below BASE minus
//     the other two spends, so order/timing never matters); then rank2 and
//     rank3 each spend a FIXED amount (t=55s) that is individually solvent
//     against the post-solvent pool but whose SUM exceeds it. Money can only
//     ever leave a wallet the engine currently shows as sufficient
//     (writePlayerWallet refuses money<0, and this file's own spend helper
//     mirrors ScenarioProbes.cpp's money_sync guard: never attempt a spend
//     the local wallet cannot currently cover) - a single delta that alone
//     exceeds the WHOLE pool can therefore never be manufactured live (that
//     is exactly what MoneyFold.h's own prototest matrix proves instead, with
//     synthetic deltas no engine constrains). What IS reachable live, and
//     what this leg proves end-to-end, is the exact shape MoneyFold.h exists
//     for: whichever of the two overdraft deltas the host processes FIRST
//     folds immediately (still solvent against the untouched pool);
//     whichever it processes SECOND overdraws the now-reduced pool and is
//     rejected via the bounded contention window - deterministically, in
//     EITHER arrival order, with the reject broadcast (PKT_MONEY_REJECT)
//     observable identically on all four instances and the rejected buyer's
//     wallet visibly refunding. No hardcoded winner: the oracle (Test-
//     Consensus, CoopOraclesN.psm1) derives which one folded from the
//     production [wallet] FOLD/REJECT evidence, never from a number baked
//     into this file or the PowerShell.
//
//   Leg B (speed, t=95..250s): scripted writeGameSpeed clicks across ranks
//     (host raises to 3x while the joins still sit at the default 1x ->
//     denied by the min rule; all three joins then raise to 3x together ->
//     effective 3x; join1 pauses -> effective 0; join1 unpauses -> back to
//     3x; the host issues ONE self-squad attack -> the combat cap pins
//     effective to 1x). Once the combat window has had time to resolve,
//     rank3 clicks 1x and HOLDS it - the constraining vote - across a
//     bracketed window (SCENARIO CONSENSUS legmark leg=B phase=1/2) that
//     Plan 04's live run places run_test4.ps1's PROVEN disconnectAtSec
//     force-kill inside (rank3 is the instance disconncted; see the
//     "TIMING CONTRACT" note below). The survivors' (host/join1/join2)
//     SCENARIO SPEED series raising back to 3x shortly after the host's own
//     "[leave] speed vote=... owner=3" line is the CONS-02 instant-vote-drop
//     proof - no new harness surface, per the plan's locked decision.
//
//   Leg C (time, the whole run): passive 1 Hz readGameClock -> SCENARIO
//     GTIME on all four, reusing time_sync's exact emitter format, so the
//     convergence oracle has one continuous series spanning every Leg-B
//     speed change.
//
// TIMING CONTRACT (Plan 04 must honor this): rank3's held constraining vote
// spans [SPEED_HOLDVOTE_AT_MS, HOST_DURATION_MS) on rank3's OWN clock: the
// scenario emits "SCENARIO CONSENSUS legmark leg=B phase=1" at
// LEGB_HOLDVOTE_MARK_MS (host clock) and "phase=2" at LEGB_HOLDVOTE_END_MS.
// Plan 04's run_test4.ps1 manifest MUST set join3's disconnectAtSec so the
// force-kill lands strictly INSIDE [LEGB_HOLDVOTE_MARK_MS, LEGB_HOLDVOTE_END_MS]
// (host-clock seconds) - a kill before phase=1 never observes the held vote
// (the DENIED/all-raise legs would still be running); a kill after phase=2
// leaves too little run time for the survivors' raise to register before the
// scenario ends. See run_test4.ps1's own disconnectAtSec doc (measured from
// the HOST's clock) and the run_meta.json scheduledDisconnect exemption
// (milestone_a_gate/Phase 5 gate precedent) for clean_exit's carve-out.
//
// Evidence discipline (locked, mirrors ScenarioWorldState.cpp /
// ScenarioItemConservation.cpp): this file emits three NEW line kinds -
// "SCENARIO CONSENSUS legmark ...", "SCENARIO CONSENSUS moneyspend ...", and
// "SCENARIO CONSENSUS speedclick ..." (plus "SCENARIO CONSENSUS moneyseed
// ..." and "SCENARIO CONSENSUS combat ...") - and reuses three EXISTING
// emitter formats verbatim (the plan's own must-have): "SCENARIO POOL
// money=.. who=.. t=.." (ScenarioProbes.cpp money_sync), "SCENARIO SPEED
// t=.. mult=.. paused=.. nbtn=.. buttons=.." (ScenarioCharState.cpp
// speed_sync), and "SCENARIO GTIME hours=.. hourLen=.. fsm=.. paused=..
// ok=.. t=.." (ScenarioProbes.cpp time_sync). No production [wallet]/
// [speed]/[time]/[leave] log string is touched - Test-Consensus reads those
// verbatim from Plans 01/02.
//
// Must NOT: change any SCENARIO/production log string (oracle API,
// resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {

class ConsensusScenario : public TimedScenario {
public:
    ConsensusScenario()
        : TimedScenario("consensus_gate", 1000),
          ownRank_(0),
          seeded_(false), seedOk_(false),
          solventSpent_(false), solventOk_(false), solventSpentAtMs_(0),
          overdraftSpent_(false), overdraftOk_(false), overdraftArmedMs_(0),
          overdraftHoldMs_(0),
          haveOwn_(false),
          hostClicked3x_(false), allClicked3x_(false),
          joinPaused_(false), joinUnpaused_(false),
          combatIssued_(false), haveStriker_(false), combatOrderMs_(0),
          holdVoteSet_(false),
          markedA2_(false), markedB1_(false), markedB2_(false) {
        memset(ownHand_, 0, sizeof(ownHand_));
        memset(striker_, 0, sizeof(striker_));
    }

    // "SCENARIO MAGATE start" reused verbatim (ScenarioItemConservation.cpp /
    // ScenarioWorldState.cpp precedent) so Get-MagateOwnRank resolves this
    // scenario's own-rank identity with the same helper every N=4 gate uses.
    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.localId;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE start ownRank=%u host=%d",
                  ownRank_, ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);

        // Leg A begins the instant the scenario arms - the host-only marker
        // gives the oracle a t=0 anchor in its own wall-clock frame (the
        // ScenarioWorldState.cpp claimphase idiom).
        if (ctx.isHost) emitLegMark('A', 1, 0);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool due = evidenceDue(ctx.elapsedMs);

        // ---- Leg A: money (rank-generic: host seeds, joins 1-3 spend) -----
        tickMoneySeed(ctx);
        tickMoneySolvent(ctx);
        tickMoneyOverdraft(ctx);

        // ---- Leg B: speed (rank-generic script) ----------------------------
        if (!haveOwn_) tryLatchLeader(ctx);
        tickSpeedScript(ctx);

        // ---- host-only phase markers (bound the oracle's judged windows) --
        if (ctx.isHost) tickLegMarkers(ctx);

        // ---- Leg C + reused series: 1 Hz on every instance -----------------
        if (due) {
            emitPool(ctx);
            emitSpeed(ctx);
            emitGTime(ctx);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : joinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // Local leg-ran verdict only - Test-Consensus (CoopOraclesN.psm1)
            // judges the cross-instance convergence/verdict evidence. A rank
            // whose own lever never verified still fails its own leg locally
            // too, but the live-run oracle's evidentiary-gap discipline
            // (NoSignalFails) is the authoritative judgment.
            bool isSolventRank   = (ownRank_ >= 1u && ownRank_ <= 3u);
            bool isOverdraftRank = (ownRank_ == 2u || ownRank_ == 3u);
            passed_ = (!ctx.isHost || seeded_) &&
                      (!isSolventRank   || solventSpent_) &&
                      (!isOverdraftRank || overdraftSpent_);
            return true;
        }
        return false;
    }

private:
    // ==== Leg A: money ========================================================

    // Host-only: seed the pool to a deterministic BASE, retried each tick
    // (never trust a bare write's bool return) until the read-back confirms
    // it or the deadline passes.
    void tickMoneySeed(const ScenarioContext& ctx) {
        if (!ctx.isHost || seeded_) return;
        if (ctx.elapsedMs < SEED_AT_MS) return;
        int before = -1;
        engine::readPlayerWallet(ctx.gw, &before);
        if (before == BASE_MONEY) { // already there (a retry after a prior success raced logging)
            seeded_ = true; seedOk_ = true;
            emitMoneySeed(before, before, ctx.elapsedMs);
            return;
        }
        bool wroteOk = engine::writePlayerWallet(ctx.gw, BASE_MONEY) ? true : false;
        int after = -1;
        engine::readPlayerWallet(ctx.gw, &after);
        bool verified = wroteOk && (after == BASE_MONEY);
        if (verified || ctx.elapsedMs >= SEED_DEADLINE_MS) {
            seeded_ = true; seedOk_ = verified;
            emitMoneySeed(before, after, ctx.elapsedMs);
        }
    }

    // Joins 1-3: each spends a FIXED, individually-solvent amount once the
    // seed has had time to settle. Never attempts a spend the local wallet
    // cannot currently cover (the money_sync doSpend guard, ScenarioProbes.cpp) -
    // retried each tick until verified or the deadline passes.
    void tickMoneySolvent(const ScenarioContext& ctx) {
        if (ctx.isHost || ownRank_ < 1u || ownRank_ > 3u) return;
        if (solventSpent_) return;
        if (ctx.elapsedMs < SOLVENT_AT_MS) return;
        int amount = solventAmountForRank(ownRank_);
        int before = -1, after = -1;
        bool wroteOk = false;
        if (engine::readPlayerWallet(ctx.gw, &before) && before >= amount) {
            wroteOk = engine::writePlayerWallet(ctx.gw, before - amount) ? true : false;
            engine::readPlayerWallet(ctx.gw, &after);
        }
        bool verified = wroteOk && (after == before - amount);
        if (verified || ctx.elapsedMs >= SOLVENT_DEADLINE_MS) {
            solventSpent_ = true; solventOk_ = verified;
            solventSpentAtMs_ = ctx.elapsedMs;
            emitMoneySpend("solvent", amount, verified, before, after, ctx.elapsedMs);
        }
    }

    // Ranks 2 and 3 only: the deliberate overdraft contest. Each amount is
    // individually solvent against the post-Leg-A-solvent pool, but their SUM
    // exceeds it - see the file header for why this (not a single delta
    // exceeding the whole pool) is the live-reachable shape of the contest.
    //
    // Plan 04 live-run fix (run 20260904_031458_N4): the ORIGINAL gate fired
    // at a fixed OWN-elapsed-clock offset (ctx.elapsedMs >= OVERDRAFT_AT_MS),
    // which is skew-vulnerable - N=4 own-clock arming skew (~14-25s, Phase 8
    // research) meant rank2 (armed earlier in wall time) reached its own
    // OVERDRAFT_AT_MS and folded its overdraft LONG BEFORE rank3's own clock
    // (armed ~14s later) ever reached the same own-elapsed threshold. By the
    // time rank3's window finally opened, its own wallet view already
    // reflected rank2's already-folded, already-replicated pool (4200, not
    // 9400) - rank3's OWN local solvency guard (mirroring the production
    // money_sync spend guard) then correctly refused to send a request for
    // 5100 against a locally-visible 4200 for the ENTIRE
    // [OVERDRAFT_AT_MS, OVERDRAFT_DEADLINE_MS) window, never reaching the
    // host's contention window at all (moneyspend rank=3 ok=0 before=4200
    // amount=5100 the whole time). This is not a false contest - it is
    // exactly correct client-side behavior once the pool has genuinely
    // shrunk - but it meant the two overdraft deltas never raced.
    //
    // Fix: trigger on the REPLICATED WALLET VALUE reaching the known
    // post-solvent total (POST_SOLVENT_POOL = 9400, computed from the three
    // fixed solvent amounts), not on an own-elapsed-clock offset. The pool
    // becomes 9400 for every connected instance within one wire round-trip
    // of the LAST solvent fold landing (a shared, host-broadcast value -
    // "every instance's own view of the total", the GTIME wall-clock-anchor
    // idiom applied to money), regardless of how differently each rank's own
    // scenario clock happened to arm. Both overdraft ranks now poll every
    // tick from the moment their OWN solvent action is done
    // (solventSpent_ true - always well before the pool can reach 9400,
    // since rank3's own solvent spend is what completes the transition to
    // 9400 in the observed run) until they OBSERVE 9400, then fire
    // immediately - landing both sends within a tick-plus-round-trip of each
    // other, safely inside MoneyFold.h's real ~250ms contention window
    // (SyncTuning::moneyWindowMs) instead of tens of seconds apart.
    // OVERDRAFT_DEADLINE_MS (unchanged) remains the give-up bound: if the
    // pool never reaches exactly 9400 (a genuine conservation defect
    // upstream), this rank still reports ok=0 rather than hanging forever -
    // the oracle's NoSignalFails discipline surfaces that as a named FAIL.
    //
    // Second Plan 04 live-run fix (run 20260904_033708_N4): solventSpent_
    // alone is not a strong enough gate - tickMoneySolvent() and
    // tickMoneyOverdraft() are called back-to-back within the SAME onTick(),
    // so on the exact tick solventSpent_ FIRST becomes true, the pool has
    // already been written down to POST_SOLVENT_POOL and this function would
    // immediately see poolReady=true and fire the overdraft write ON THAT
    // SAME TICK. publishMoneyPool (ReplicatorChannels.cpp) samples the
    // engine wallet ONCE per tick and reports the NET delta since its last
    // sample - two writes landing before that single sample coalesces them
    // into ONE wire delta with ONE seq (observed live: rank3's solvent -250
    // and overdraft -5100 merged into a single "[wallet] POOL FOLD owner=3
    // seq=1 delta=-5350" - exactly 250+5100 - which the host evaluated as
    // one lump sum, individually solvent against the pre-solvent pool, so
    // it folded immediately with no contention window at all). Requiring a
    // STRICTLY LATER tick (ctx.elapsedMs > solventSpentAtMs_) guarantees at
    // least one publish sample lands in between, so the solvent delta is
    // captured and sent on its own before the overdraft write ever happens.
    // Third live-run fix (Phase 11 11-03, run 20260905_122658_N4): the
    // strictly-later-tick guard above separates the two WRITES by one tick,
    // but one tick is still inside a single HOST DRAIN window - the two wire
    // deltas (separate seqs this time) arrived at the host together and were
    // folded in the SAME ms ("[wallet] POOL FOLD owner=3 seq=1 -250" +
    // "seq=2 -5100" both at 12:28:20.203), so the intermediate 9400 pool
    // state was NEVER BROADCAST and rank2's ==POST_SOLVENT_POOL trigger
    // could never arm: it fell to the deadline give-up (local wallet already
    // 4300, insolvent, no wire delta, no contest, no observable
    // "[wallet] REJECT" - the exact no-signal FAIL the oracle reported).
    // Fix: LATCH the poolReady observation (overdraftArmedMs_) and HOLD the
    // overdraft write for OVERDRAFT_HOLD_MS afterwards - long enough
    // (400ms >> the ~50ms publish tick + host drain + change-driven
    // POOL SEND rebroadcast) that the trigger state each contestant armed
    // on has reliably crossed the wire to the OTHER contestant before
    // either overdraft delta exists. Both ranks hold symmetrically, so the
    // two fires still land within a broadcast-latency of each other (the
    // archived-PASS run measured them 47ms apart) - safely inside the
    // production contention machinery, which needs no simultaneity at the
    // host anyway (the reject fires whenever the SECOND delta arrives
    // insolvent). The write at hold-expiry re-reads the CURRENT wallet: if
    // the other contestant's fold somehow landed inside the hold (wallet <
    // amount - the contest window truly closed), this rank honestly reports
    // ok=0 instead of authoring a locally-insolvent write.
    void tickMoneyOverdraft(const ScenarioContext& ctx) {
        if (ctx.isHost || (ownRank_ != 2u && ownRank_ != 3u)) return;
        if (overdraftSpent_) return;
        if (!solventSpent_ || ctx.elapsedMs <= solventSpentAtMs_) return;
        int amount = overdraftAmountForRank(ownRank_);
        int before = -1, after = -1;
        if (overdraftArmedMs_ == 0) {
            bool poolReady = engine::readPlayerWallet(ctx.gw, &before) &&
                             before == POST_SOLVENT_POOL;
            if (poolReady) {
                overdraftArmedMs_ = (ctx.elapsedMs != 0) ? ctx.elapsedMs : 1;
                // Fourth live-run fix (run 20260905_174257_N4): the hold must
                // be ASYMMETRIC by observation path. A SELF-ARMED rank (its
                // own solvent write completed the 9400 transition, detectable
                // as arming within one beat of its own solvent write) holds
                // OVERDRAFT_HOLD_MS so its two deltas cross the host's drain
                // in separate batches and the intermediate 9400 state
                // actually broadcasts (the fold-merge fix, kept). A
                // BROADCAST-ARMED rank observed 9400 via the wire - it is
                // already a round-trip BEHIND the self-armed contestant, and
                // 174257's measured failure was exactly the symmetric 400ms
                // hold making it fire AFTER the self-armed rank's overdraft
                // fold landed (wallet already 4300 at hold expiry -> honest
                // ok=0, no contest, no [wallet] REJECT). It must fire ON
                // SIGHT: its delta is then in flight before the self-armed
                // rank's fold (fired at arm+hold) can round-trip back, so
                // BOTH deltas race and the host's deterministic reject fires
                // on whichever folds second. onTick runs at frame rate (the
                // ctor's 1000ms is only the EVIDENCE cadence), so on-sight
                // is ~one frame after the broadcast lands.
                overdraftHoldMs_ =
                    (ctx.elapsedMs - solventSpentAtMs_ <= SELF_ARM_WINDOW_MS)
                        ? OVERDRAFT_HOLD_MS : 0ul;
                if (overdraftHoldMs_ > 0) return; // separate the two writes
            }
        }
        if (overdraftArmedMs_ != 0 &&
            ctx.elapsedMs >= overdraftArmedMs_ + overdraftHoldMs_) {
            bool wroteOk = false;
            bool haveCur = engine::readPlayerWallet(ctx.gw, &before);
            if (haveCur && before >= amount) {
                wroteOk = engine::writePlayerWallet(ctx.gw, before - amount) ? true : false;
                engine::readPlayerWallet(ctx.gw, &after);
            }
            bool verified = haveCur && before >= amount && wroteOk &&
                            (after == before - amount);
            overdraftSpent_ = true; overdraftOk_ = verified;
            emitMoneySpend("overdraft", amount, verified, before, after, ctx.elapsedMs);
            return;
        }
        if (ctx.elapsedMs >= OVERDRAFT_DEADLINE_MS) {
            overdraftSpent_ = true; overdraftOk_ = false;
            emitMoneySpend("overdraft", amount, false, before, after, ctx.elapsedMs);
        }
    }

    static int solventAmountForRank(unsigned int rank) {
        switch (rank) {
            case 1u: return SOLVENT_AMOUNT_R1;
            case 2u: return SOLVENT_AMOUNT_R2;
            case 3u: return SOLVENT_AMOUNT_R3;
            default: return 0;
        }
    }
    static int overdraftAmountForRank(unsigned int rank) {
        if (rank == 2u) return OVERDRAFT_AMOUNT_R2;
        if (rank == 3u) return OVERDRAFT_AMOUNT_R3;
        return 0;
    }

    void emitMoneySeed(int before, int after, unsigned long t) {
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONSENSUS moneyseed base=%d ok=%d before=%d after=%d t=%lu",
                  BASE_MONEY, seedOk_ ? 1 : 0, before, after, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    void emitMoneySpend(const char* phase, int amount, bool ok, int before, int after,
                        unsigned long t) {
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONSENSUS moneyspend rank=%u phase=%s amount=%d ok=%d "
                  "before=%d after=%d t=%lu",
                  ownRank_, phase, amount, ok ? 1 : 0, before, after, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== Leg B: speed =========================================================

    void tryLatchLeader(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank_);
        if (idx < 0) return;
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
    }

    void tickSpeedScript(const ScenarioContext& ctx) {
        // Host clicks 3x while every join still sits at the default 1x ->
        // the min rule denies the raise (both/all must raise together).
        if (ctx.isHost && !hostClicked3x_ && ctx.elapsedMs >= SPEED_HOST3X_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            emitSpeedClick(3.0f, ok, "host_raise_denied", ctx.elapsedMs);
            hostClicked3x_ = true;
        }
        // All three joins raise to 3x TOGETHER -> now every voter agrees,
        // effective becomes 3x.
        if (!ctx.isHost && ownRank_ >= 1u && ownRank_ <= 3u &&
            !allClicked3x_ && ctx.elapsedMs >= SPEED_ALL3X_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            emitSpeedClick(3.0f, ok, "all_raise", ctx.elapsedMs);
            allClicked3x_ = true;
        }
        // join1 pauses (either can pause -> effective 0), then unpauses back
        // to 3x (proving the pause-then-recover round trip at N).
        if (!ctx.isHost && ownRank_ == 1u && !joinPaused_ &&
            ctx.elapsedMs >= SPEED_PAUSE_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 0.0f, true);
            emitSpeedClick(0.0f, ok, "pause", ctx.elapsedMs);
            joinPaused_ = true;
        }
        if (!ctx.isHost && ownRank_ == 1u && joinPaused_ && !joinUnpaused_ &&
            ctx.elapsedMs >= SPEED_UNPAUSE_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            emitSpeedClick(3.0f, ok, "unpause", ctx.elapsedMs);
            joinUnpaused_ = true;
        }
        // Host issues ONE self-squad attack order (own leader as the victim's
        // target-of-attention is irrelevant - the ATTACKER's own-squad combat
        // flag is what trips the cap): the combat cap pins effective to 1x
        // while every voter still requests 3x (speed_sync's own combat-phase
        // precedent, ScenarioCharState.cpp).
        //
        // Plan 04 live-run fix (run 20260904_031458_N4): the ORIGINAL pick
        // was a single-shot attempt at the default 30u radius and gave up
        // permanently on the first miss ("combat pick FAILED (no upright
        // NPC)", combatIssued_ latched true regardless) - unlike every other
        // mutation in this file, it never retried. wanderer4's co-op spawn is
        // documented wilderness (Phase 8 gate: "the door leg's 100u
        // existing-door search found ZERO doors near wanderer4's wilderness
        // co-op spawn") - 30u is far too tight to expect any wandering
        // wildlife/bandit there. Now retries every tick at
        // COMBAT_PICK_RADIUS_U (600u, the codebase's own established
        // "nearby" scale - KENSHICOOP_SPAWN_MINT_RADIUS's default) until a
        // candidate is found.
        //
        // Second Plan 04 live-run fix (run 20260904_033708_N4): finding a
        // striker is not enough - the ATTACK must be issued only once the
        // min-vote is CONFIRMED already at eff=3x (all three joins' raises
        // landed), or the cap's 3x->1x transition can coincide with rank3's
        // own still-in-flight raise and never produce a distinct '[speed]
        // SET' line (both states read as eff=1x, so the change-gate never
        // fires - the observed run 2 signature: combat issued 2s before
        // rank3's raise even reached the host, so cap and "not yet raised"
        // were indistinguishable in the SET stream the whole window).
        // readGameSpeed() reads the HOST's own already-applied effective
        // multiplier - the authoritative post-reduce value, immune to
        // arming skew - so gating the ATTACK (not just the pick) on it
        // confirms the cap's transition will be a genuine, visible change.
        // Third Plan-04-class live-run fix (Phase 11 11-03, runs
        // 20260905_123937/124602_N4): a SINGLE attack order is not enough -
        // both runs issued ok=1 to a valid 600u pick that then simply never
        // engaged (order decayed / candidate unreachable), so the leader
        // never entered combat mode and '[speed] SET cap=1' never fired (the
        // archived PASS's pick happened to engage in 0.77s). Convert the
        // one-shot into the codebase's established keep-the-fight-alive
        // idiom (ScenarioMilestoneA.cpp re-orders its hostiles every 2.5s):
        // re-pick + re-order every COMBAT_REORDER_MS until the VICTIM's own
        // combat read confirms engagement (the authoritative signal the
        // speed cap itself rides), giving up only at the unchanged
        // SPEED_COMBAT_DEADLINE_MS.
        if (ctx.isHost && haveOwn_ && !combatIssued_ && ctx.elapsedMs >= SPEED_COMBAT_AT_MS) {
            engine::CombatRead vcr;
            bool engaged = engine::readCombatByHand(ownHand_, &vcr) && vcr.valid &&
                           (vcr.inCombat || vcr.modeActive);
            float curMult = 0.0f; bool curPaused = false;
            bool allRaised = engine::readGameSpeed(ctx.gw, &curMult, &curPaused) &&
                             !curPaused && curMult >= 2.99f;
            if (engaged) {
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO CONSENSUS combat ENGAGED vic=%u,%u t=%lu",
                          ownHand_[3], ownHand_[4], ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                combatIssued_ = true;
            } else if (ctx.elapsedMs >= SPEED_COMBAT_DEADLINE_MS) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO CONSENSUS combat pick FAILED (striker=%d allRaised=%d mult=%.2f)",
                          haveStriker_ ? 1 : 0, allRaised ? 1 : 0, curMult);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                combatIssued_ = true;
            } else if (allRaised &&
                       (combatOrderMs_ == 0 ||
                        ctx.elapsedMs - combatOrderMs_ >= COMBAT_REORDER_MS)) {
                haveStriker_ = engine::pickCombatVictim(ctx.gw, ownHand_, 0, striker_,
                                                        0, COMBAT_PICK_RADIUS_U);
                if (haveStriker_) {
                    bool ok = engine::orderAttackByHand(ctx.gw, striker_, ownHand_);
                    char b[144];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO CONSENSUS combat issued atk=%u,%u vic=%u,%u ok=%d t=%lu",
                              striker_[3], striker_[4], ownHand_[3], ownHand_[4],
                              ok ? 1 : 0, ctx.elapsedMs);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    combatOrderMs_ = ctx.elapsedMs;
                }
            }
        }
        // rank3 clicks 1x and HOLDS it - the constraining vote across the
        // disconnect bracket (see the file header's TIMING CONTRACT). By
        // SPEED_HOLDVOTE_AT_MS the combat window (SPEED_COMBAT_AT_MS, well
        // earlier) has had time to resolve, so the effective staying at 1x
        // from here on is attributable to rank3's vote, not the cap.
        if (!ctx.isHost && ownRank_ == 3u && !holdVoteSet_ &&
            ctx.elapsedMs >= SPEED_HOLDVOTE_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 1.0f, false);
            emitSpeedClick(1.0f, ok, "hold_constraining_vote", ctx.elapsedMs);
            holdVoteSet_ = true;
        }
    }

    void emitSpeedClick(float mult, bool ok, const char* tag, unsigned long t) {
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONSENSUS speedclick rank=%u mult=%.2f ok=%d tag=%s t=%lu",
                  ownRank_, mult, ok ? 1 : 0, tag, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== host-only phase markers (bound the oracle's judged windows) ========

    void tickLegMarkers(const ScenarioContext& ctx) {
        if (!markedA2_ && ctx.elapsedMs >= LEGA_END_MARK_MS) {
            markedA2_ = true;
            emitLegMark('A', 2, ctx.elapsedMs);
        }
        if (!markedB1_ && ctx.elapsedMs >= LEGB_HOLDVOTE_MARK_MS) {
            markedB1_ = true;
            emitLegMark('B', 1, ctx.elapsedMs);
        }
        if (!markedB2_ && ctx.elapsedMs >= LEGB_HOLDVOTE_END_MS) {
            markedB2_ = true;
            emitLegMark('B', 2, ctx.elapsedMs);
        }
    }

    static void emitLegMark(char leg, int phase, unsigned long t) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CONSENSUS legmark leg=%c phase=%d t=%lu",
                  leg, phase, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== Leg C + reused 1 Hz series (every instance) =========================

    // "SCENARIO POOL money=.. who=.. t=.." reused verbatim (money_sync,
    // ScenarioProbes.cpp) - Test-Consensus's money ledger is built from this
    // series plus the production [wallet] FOLD/REJECT lines.
    void emitPool(const ScenarioContext& ctx) {
        int pool = -1;
        engine::readPlayerWallet(ctx.gw, &pool);
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO POOL money=%d who=%s t=%lu",
                  pool, ctx.isHost ? "host" : "join", ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // "SCENARIO SPEED t=.. mult=.. paused=.. nbtn=.. buttons=.." reused
    // verbatim (speed_sync, ScenarioCharState.cpp) - the per-instance
    // effective-speed series the min-vote/cap/instant-drop oracle reads.
    void emitSpeed(const ScenarioContext& ctx) {
        float mult = 0.0f; bool paused = false;
        if (!engine::readGameSpeed(ctx.gw, &mult, &paused)) return;
        char btn[16]; btn[0] = '\0';
        int nBtn = engine::readSpeedButtons(btn, sizeof(btn));
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SPEED t=%lu mult=%.2f paused=%d nbtn=%d buttons=%s",
                  ctx.elapsedMs, mult, paused ? 1 : 0, nBtn, btn);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // "SCENARIO GTIME hours=.. hourLen=.. fsm=.. paused=.. ok=.. t=.." reused
    // verbatim (time_sync, ScenarioProbes.cpp) - Leg C's convergence series.
    void emitGTime(const ScenarioContext& ctx) {
        double hours = -1.0; float hourLen = -1.0f;
        bool ok = engine::readGameClock(ctx.gw, &hours, &hourLen);
        float mult = -1.0f; bool paused = false;
        engine::readGameSpeed(ctx.gw, &mult, &paused);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO GTIME hours=%.5f hourLen=%.1f fsm=%.2f paused=%d ok=%d t=%lu",
                  hours, hourLen, mult, paused ? 1 : 0, ok ? 1 : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // joinDurationMs(): honors KENSHICOOP_SCENARIO_JOIN_DURATION_SEC for a
    // deferred/late-joining instance, verbatim from ScenarioItemConservation.cpp
    // / ScenarioWorldState.cpp.
    unsigned long joinDurationMs() const {
        const char* e = ::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC");
        if (!e || !*e) return JOIN_DURATION_MS;
        long s = ::atol(e);
        if (s < 10) s = 10;
        unsigned long ms = (unsigned long)s * 1000ul;
        return (ms < JOIN_DURATION_MS) ? ms : JOIN_DURATION_MS;
    }

    static const unsigned int MAX_SQUAD = 32;

    // ---- Leg A schedule (own clock) -------------------------------------------
    static const int BASE_MONEY            = 10000;
    static const int SOLVENT_AMOUNT_R1     = 150;
    static const int SOLVENT_AMOUNT_R2     = 200;
    static const int SOLVENT_AMOUNT_R3     = 250;
    // Sum = 600; expected post-solvent pool = BASE_MONEY - 600 = 9400 (the
    // oracle derives this from evidence, never from these constants).
    static const int POST_SOLVENT_POOL     = 9400;
    static const int OVERDRAFT_AMOUNT_R2   = 5200;
    static const int OVERDRAFT_AMOUNT_R3   = 5100;
    // 5200 + 5100 = 10300 > 9400 (the post-solvent pool), while each alone
    // (5200 or 5100) is well under it - the live-reachable overdraft shape
    // the file header documents: whichever the host processes first folds,
    // the other overdraws the reduced pool and is rejected.

    static const unsigned long SEED_AT_MS            = 8000;
    static const unsigned long SEED_DEADLINE_MS      = 20000;
    static const unsigned long SOLVENT_AT_MS         = 22000;
    static const unsigned long SOLVENT_DEADLINE_MS   = 42000;
    // OVERDRAFT_DEADLINE_MS is now purely a give-up bound for
    // tickMoneyOverdraft's POST_SOLVENT_POOL value-gate (see that function's
    // header comment, Plan 04 live-run fix) - the trigger itself no longer
    // waits for a fixed own-elapsed-clock offset.
    static const unsigned long OVERDRAFT_DEADLINE_MS = 75000;
    // Phase 11 (11-03): the trigger-observation -> overdraft-write hold (see
    // tickMoneyOverdraft's header comment) - long enough for the armed
    // trigger state to have crossed the wire to the other contestant before
    // either overdraft delta exists.
    static const unsigned long OVERDRAFT_HOLD_MS     = 150;
    // Arm-time window for classifying an arm as SELF-caused (the rank's own
    // solvent write completed the 9400 transition on the same/next frames)
    // vs BROADCAST-caused (another rank's fold delivered it). See the
    // asymmetric-hold comment in tickMoneyOverdraft.
    static const unsigned long SELF_ARM_WINDOW_MS    = 500;
    static const unsigned long LEGA_END_MARK_MS      = 85000; // host marker: leg A settled

    // ---- Leg B schedule (own clock) --------------------------------------------
    static const unsigned long SPEED_HOST3X_AT_MS    = 95000;  // denied (min=1x)
    static const unsigned long SPEED_ALL3X_AT_MS     = 105000; // all raise -> eff=3x
    static const unsigned long SPEED_PAUSE_AT_MS     = 117000; // join1 pauses -> eff=0
    static const unsigned long SPEED_UNPAUSE_AT_MS   = 122000; // join1 unpauses -> eff=3x
    static const unsigned long SPEED_COMBAT_AT_MS    = 130000; // host self-attack -> cap=1x
    // Retry-until-observed bound for the combat pick (Plan 04 live-run fix):
    // gives up 2s before LEGB_HOLDVOTE_MARK_MS (150000) if no candidate is
    // ever found within COMBAT_PICK_RADIUS_U - an 18s retry window from
    // SPEED_COMBAT_AT_MS (130000). Phase 9 review IN-01: this comment used to
    // claim a 20s post-deadline buffer that never existed (148000 vs 150000
    // is 2s); the CONSTANT is the half that is right - moving the deadline to
    // 130000 (the review's literal suggestion) would make it equal
    // SPEED_COMBAT_AT_MS and fire the give-up branch on the very first
    // unsatisfied attempt, destroying the retry-until-observed design. The
    // real engage/register margin is the retry window itself plus the fact
    // that rank3's held vote (155000) trails the bracket-open marker by 5s;
    // the archived passing run (20260904_040725_N4) issued the attack at
    // ~t=130s, 18s inside the bound.
    static const unsigned long SPEED_COMBAT_DEADLINE_MS = 148000;
    // Phase 11 (11-03): re-pick/re-order cadence for the combat leg's
    // keep-the-fight-alive loop (ScenarioMilestoneA.cpp's 2.5s idiom).
    static const unsigned long COMBAT_REORDER_MS        = 2500;
    // 600u = KENSHICOOP_SPAWN_MINT_RADIUS's own default "nearby" scale
    // (Config.h/Config.cpp), reused here rather than inventing a new radius -
    // the default 30u pick (calibrated for the "sync" bar-crowd fixture) has
    // no chance against wanderer4's documented wilderness spawn. Defined
    // out-of-class below (C++03: only integral statics may be initialized
    // in-class, the ScenarioCharState.cpp/ScenarioCombat.cpp float-const
    // precedent).
    static const float         COMBAT_PICK_RADIUS_U;
    // Held constraining vote bracket (see the file header's TIMING CONTRACT -
    // Plan 04's disconnectAtSec for join3 MUST land inside
    // [LEGB_HOLDVOTE_MARK_MS, LEGB_HOLDVOTE_END_MS], host-clock seconds).
    static const unsigned long SPEED_HOLDVOTE_AT_MS     = 155000;
    static const unsigned long LEGB_HOLDVOTE_MARK_MS    = 150000; // host marker: bracket opens
    static const unsigned long LEGB_HOLDVOTE_END_MS     = 220000; // host marker: bracket closes

    // HOST outlives the held-vote bracket's close with 30 s margin for the
    // survivors' post-disconnect raise to register; JOIN duration trails by
    // 15 s (the world_state_gate/item_conservation_gate margin class).
    static const unsigned long HOST_DURATION_MS = 250000;
    static const unsigned long JOIN_DURATION_MS = 235000;

    unsigned int ownRank_;

    bool seeded_, seedOk_;
    bool solventSpent_, solventOk_;
    unsigned long solventSpentAtMs_;
    bool overdraftSpent_, overdraftOk_;
    unsigned long overdraftArmedMs_; // poolReady latched at this own-clock ms (0 = not yet)
    unsigned long overdraftHoldMs_;  // this rank's arm->fire hold (OVERDRAFT_HOLD_MS if
                                     //   self-armed, 0 if broadcast-armed; set at arm time)

    bool         haveOwn_;
    unsigned int ownHand_[5];
    bool         hostClicked3x_, allClicked3x_;
    bool         joinPaused_, joinUnpaused_;
    bool         combatIssued_, haveStriker_;
    unsigned long combatOrderMs_; // last attack (re-)order own-clock ms (0 = none yet)
    unsigned int striker_[5];
    bool         holdVoteSet_;

    bool markedA2_, markedB1_, markedB2_;
};

const float ConsensusScenario::COMBAT_PICK_RADIUS_U = 600.0f;

} // namespace

Scenario* makeConsensusScenario(const std::string& name) {
    if (name == "consensus_gate") return new ConsensusScenario();
    return 0;
}

} // namespace coop
