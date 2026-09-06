// ScenarioItemConservation.cpp - item_conservation_gate (Phase 7 plan 03/04):
// the ONE live conservation-oracle scenario every one of the 4 processes runs
// to exercise the host-committed transfer model (Plan 01, XferCommit.h) and
// the claim arbiter (Plan 02, ClaimArbiter.h) end-to-end against a real Kenshi
// world, emitting the SCENARIO CONSERVE per-instance item-census evidence
// Test-InvConservation (Legs A-D, scripts/CoopOraclesN.psm1) judges offline.
//
// Modeled on ScenarioPlayerState.cpp's 4-instance gate scaffold (ownRank_ =
// ctx.localId, fixed-rank per-leg actions, TimedScenario HOST/JOIN duration
// split, "SCENARIO MAGATE start" so Get-MagateOwnRank resolves this scenario's
// logs too) and the proven engine levers from ScenarioInventory.cpp/
// ScenarioWorldItems.cpp (moveItemBetweenContainers = the UI-drag equivalent a
// cross-owner transfer intent is authored from; dropItemFromInventory +
// pickupWorldItemIntoInventory = the ground-drop/pickup pair a claim
// contention is authored from).
//
// 07-04 LIVE-RUN HARDENING (run 20260902_214911_N4 ground truth - the first
// live run of this scenario). Three real-world behaviors the plan-03 design
// did not survive:
//
//  1. THE CONNECT-PUSH RELOAD STORM. Every time a LATER join connects, the
//     host re-pushes its save and every EARLIER join re-loads the world
//     ([load] WORLD-SWAP). On run 1, join1's world swapped twice after its
//     scenario armed, the last swap ending at its elapsed ~19.6s - a one-shot
//     SEED at 6s fired mid-swap and silently no-opped (resolve/gamedata reads
//     fail during a swap), and any pre-swap local mutation not yet replicated
//     host-ward is wiped by the reload. FIX: every mutation leg is now
//     RETRY-UNTIL-CENSUS-VERIFIED with a deadline, and the whole timeline
//     shifts right so the seed window ([8s,26s)) spans the storm and every
//     transfer leg starts after it.
//
//  2. ENGINE BOOL RETURNS ARE NOT GROUND TRUTH. Run 1's host seed: the items
//     landed (ck=0 census qty=6) but addTestItemsToContainer returned 0 (a
//     tryAddItem false-negative). A naive retry loop would have DOUBLE-seeded.
//     FIX: every leg verifies against its own container CENSUS delta
//     (captureContainerContents count of the probe sid), never the call's
//     return value - retries fire only while the census still shows the
//     mutation missing, and the emitted evidence line reports the census-true
//     count.
//
//  3. PER-INSTANCE SCENARIO CLOCKS ARM UP TO ~23s APART (staggered join
//     launches), so clock-scheduled "simultaneous" claims were 11s apart on
//     run 1 - no contention window (250ms, KENSHICOOP_CLAIM_WINDOW_MS) could
//     ever see both. FIX: the two claimants now poll the GROUND for the
//     dropped item on a fast 100ms cadence and claim on FIRST SIGHT - the
//     drop's world-item CREATE reaches both mirrors in the same host publish,
//     so both claims land within ~poll+jitter (<250ms) of each other and the
//     arbiter sees a real 2-claimant window (coverage gap D4: the loser's
//     applyClaimOutcome rollback fires live). The host's own drop is scheduled
//     late (118s) so both claimants' poll windows (own-clock 96s..130s) are
//     guaranteed open across the worst arming skew.
//
//  Timeline is quiescent at ck=0 (post-seed settle, pre-legs) and ck=5
//  (post-everything settle) - the two checkpoints the oracle's Leg C judges
//  (intermediate cks are not simultaneous across skewed clocks). The HOST
//  duration is 165s (joins stay 140s) so the host - the description hub -
//  outlives the LAST-armed join's final checkpoint (armed ~23s late, ck5 at
//  its own 132s = host ~155s).
//
//  4. INVENTORY CAPACITY IS THE HIDDEN CEILING (run 20260902_221642_N4). The
//     wanderer4 fixture's tab-0 (host) wanderer carries SIX baked units of
//     the common probe item + 2 gear = a FULL 8-entry inventory at gameplay
//     start ([inv] SEND items=8 at gameplay+453ms, 21s before the scenario
//     armed) - the host's seed adds were honestly refused (full), and once
//     every leader held 6 probes + 2 gear, EVERY cross-owner tryAddItem and
//     both claim pickups were refused on capacity (containers frozen at
//     6/6/6/6 through ck4; both claimants saw the drop within 114ms - the
//     sight-trigger works - but got=0). FIX: the seed window now NORMALIZES
//     each rank's container to an ABSOLUTE SEED_QTY=3 probes (drain surplus
//     via removeTestItemsFromContainer - the host's 6 baked units - then top
//     up), and every transfer moves qty=1, so no container ever exceeds ~5
//     probes + 2 gear = 7 entries (8 is the empirically-proven fit). The
//     SEED evidence line's n= reports the absolute normalized count (the
//     oracle never parses SEED lines; Leg A baselines on the ck=0 census).
//
// Evidence discipline (locked, 07-03-PLAN.md): the only NEW SCENARIO line
// this file emits is "SCENARIO CONSERVE ..." - every CONSERVE line is
// scenario-authored and read by the offline oracle only; no production
// [inv]/[xfer]/[wi]/[wd] log string is touched (ReplicatorItems.cpp's own
// header contract forbids that). Line SHAPES are unchanged from plan 03 (the
// oracle API): SEED/XFER/DROP/CLAIM/ck=cont/ck=ground exactly as before.
//
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {

// item_conservation_gate: drives the six live conservation legs and emits the
// SCENARIO CONSERVE census. See the file header for the design rationale.
class ItemConservationScenario : public TimedScenario {
public:
    ItemConservationScenario()
        : TimedScenario("item_conservation_gate", 500),
          ownRank_(0), probeType_(0),
          seedEmitted_(false), seedDone_(false),
          dropBaseKnown_(false), dropBase_(0), dropEmitted_(false),
          claimEmitted_(false), lastClaimPollMs_(0),
          nextCk_(0) {
        probeSid_[0] = '\0';
        for (unsigned int r = 0; r < RANKS; ++r) {
            haveHand_[r] = false;
            for (int k = 0; k < 5; ++k) hand_[r][k] = 0;
        }
        for (unsigned int i = 0; i < NUM_XFER; ++i) {
            xferEmitted_[i] = false; xferBaseKnown_[i] = false; xferBase_[i] = 0;
        }
    }

    // "SCENARIO MAGATE start" reused verbatim (ScenarioPlayerState.cpp
    // precedent) so Get-MagateOwnRank resolves this scenario's own-rank
    // identity with the SAME helper the other N=4 gates use.
    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.localId;
        engine::commonTestItemSid(ctx.gw, probeSid_, sizeof(probeSid_), &probeType_);
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE start ownRank=%u host=%d",
                  ownRank_, ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // One shared 500ms cadence decision per tick (evidenceDue advances its
        // stamp as a side effect, so it must be sampled exactly once).
        bool due = evidenceDue(ctx.elapsedMs);
        if (due) {
            tickResolveHands(ctx);
            // Probe template resolution is retried too: onStart can run
            // before a join's gamedata is fully live (run-1 lesson).
            if (!probeSid_[0])
                engine::commonTestItemSid(ctx.gw, probeSid_, sizeof(probeSid_), &probeType_);
            tickSeed(ctx);
            for (unsigned int i = 0; i < NUM_XFER; ++i) tickTransfer(ctx, i);
            tickDrop(ctx);
        }
        tickClaims(ctx);       // own fast 100ms cadence (contention simultaneity)
        tickCheckpoints(ctx);

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : joinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // Local legs only (script-ran verdict) - Test-InvConservation
            // (Legs A-D) judges the cross-client conservation evidence.
            passed_ = haveHand_[ownRank_] && seedDone_;
            return true;
        }
        return false;
    }

private:
    // One cross-owner transfer leg's shape (declared up top: used in method
    // signatures below; VC10 needs the type complete at first use).
    struct XferLeg { unsigned int src, dst; int qty; unsigned long startMs, deadlineMs; };

    // ---- squad-hand resolution (every ~500ms) ---------------------------------
    // Every instance resolves ALL FOUR ranks' own container hands (not just
    // its own) - every client mirrors the whole squad locally, which is what
    // lets ANY rank author a cross-owner drag INTO another rank's container
    // and lets THIS instance's own checkpoint census cover all four fixture
    // containers for Leg B/C. Re-run every cadence tick so a connect-push
    // world swap re-resolves the fresh objects.
    void tickResolveHands(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int r = 0; r < RANKS; ++r) {
            int idx = tabLeaderIdx(sq, n, r);
            if (idx < 0) continue;
            handFromEntity(sq[idx], hand_[r]);
            haveHand_[r] = true;
        }
    }

    // Census-truth count of the probe item inside rank r's container. The
    // ONLY verification primitive every mutation leg trusts (engine bool/int
    // returns proved unreliable on run 1 - see file header point 2). Returns
    // -1 when the container cannot be read this tick (mid-swap), so callers
    // can tell "empty" from "unreadable" and never mutate on a blind read.
    int countProbe(const ScenarioContext& ctx, unsigned int r) {
        if (!haveHand_[r] || !probeSid_[0]) return -1;
        InvItemEntry items[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(ctx.gw, hand_[r], items,
                                                          INV_ITEMS_MAX, 0);
        int qty = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (items[i].itemType == probeType_ && strcmp(items[i].stringID, probeSid_) == 0)
                qty += (int)items[i].quantity;
        return qty;
    }

    // ---- seed: every rank NORMALIZES its OWN container to ABSOLUTE SEED_QTY ---
    // Maintained (re-verified, drained or topped up) across the whole
    // [SEED_START_MS, SEED_EMIT_MS) window so a connect-push reload that wipes
    // an early seed self-heals AND a fixture-baked surplus (the host wanderer's
    // 6 baked probe units - run 20260902_221642_N4) is drained down to the same
    // normalized count every other rank holds; the SEED evidence line is
    // emitted ONCE at SEED_EMIT_MS with the census-true absolute count.
    // Mutations only chase the census shortfall/surplus, so a false-return
    // engine call can never double-seed or over-drain.
    void tickSeed(const ScenarioContext& ctx) {
        if (seedEmitted_ || ctx.elapsedMs < SEED_START_MS) return;
        int cur = countProbe(ctx, ownRank_);
        if (cur >= 0) {
            if (cur > SEED_QTY) {
                engine::removeTestItemsFromContainer(ctx.gw, hand_[ownRank_], cur - SEED_QTY);
                cur = countProbe(ctx, ownRank_);
            } else if (cur < SEED_QTY) {
                char sid[48]; sid[0] = '\0';
                engine::addTestItemsToContainer(ctx.gw, hand_[ownRank_], SEED_QTY - cur,
                                                sid, sizeof(sid));
                cur = countProbe(ctx, ownRank_); // census-truth after the attempt
            }
            if (ctx.elapsedMs >= SEED_EMIT_MS) {
                int got = cur;
                seedDone_ = (got == SEED_QTY);
                seedEmitted_ = true;
                char b[160];
                _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE SEED rank=%u n=%d sid='%s'",
                          ownRank_, got, probeSid_[0] ? probeSid_ : "(none)");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else if (ctx.elapsedMs >= SEED_EMIT_MS) {
            // Never got a readable container by the deadline: emit the honest
            // no-seed evidence (the oracle's Leg A coverage check catches it).
            seedEmitted_ = true;
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE SEED rank=%u n=%d sid='%s'",
                      ownRank_, 0, probeSid_[0] ? probeSid_ : "(none)");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // One cross-owner drag leg: fires ONLY on the authoring rank's own
    // instance (the "dragging client's engine performs the real move locally"
    // model - detectAndPublishTransfers pairs the diff and authors the
    // host-terminated intent from there). Census-verified: the src baseline
    // is captured on the first eligible tick; the engine move is retried only
    // while the census still shows more than (base - qty); the XFER line is
    // emitted ONCE (on verify or at the leg deadline) with the census-true
    // moved count - dup-safe against false-negative engine returns.
    void tickTransfer(const ScenarioContext& ctx, unsigned int leg) {
        const XferLeg& L = LEGS[leg];
        if (xferEmitted_[leg]) return;
        if (ownRank_ != L.src) { xferEmitted_[leg] = true; return; } // not our leg
        if (ctx.elapsedMs < L.startMs) return;
        int cur = countProbe(ctx, L.src);
        bool dstOk = haveHand_[L.dst];
        if (cur >= 0 && dstOk) {
            if (!xferBaseKnown_[leg]) { xferBase_[leg] = cur; xferBaseKnown_[leg] = true; }
            int target = xferBase_[leg] - L.qty;
            if (cur > target) {
                engine::moveItemBetweenContainers(ctx.gw, hand_[L.src], hand_[L.dst],
                                                  probeSid_, probeType_, cur - target);
                cur = countProbe(ctx, L.src);
            }
            if ((cur >= 0 && cur <= target) || ctx.elapsedMs >= L.deadlineMs) {
                int moved = (cur >= 0) ? (xferBase_[leg] - cur) : 0;
                emitXfer(ctx, L, moved);
                xferEmitted_[leg] = true;
            }
        } else if (ctx.elapsedMs >= L.deadlineMs) {
            // Deadline with no readable source count / unresolved dst: nothing
            // was moved, report 0 (07-REVIEW IN-03: this was a both-arms-zero
            // ternary that read like an unfinished intention).
            emitXfer(ctx, L, 0);
            xferEmitted_[leg] = true;
        }
    }

    void emitXfer(const ScenarioContext& ctx, const XferLeg& L, int moved) {
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONSERVE XFER src=%u dst=%u qty=%d moved=%d sid='%s' t=%lu",
                  L.src, L.dst, L.qty, moved, probeSid_[0] ? probeSid_ : "(none)",
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Contention drop: HOST (rank 0) drops one unit to ground, census-verified
    // against its OWN container (dup-safe: a retry only fires while the
    // container still holds more than base - DROP_QTY).
    void tickDrop(const ScenarioContext& ctx) {
        if (dropEmitted_ || ownRank_ != 0u || ctx.elapsedMs < CONTEND_DROP_MS) return;
        int cur = countProbe(ctx, 0);
        if (cur >= 0) {
            if (!dropBaseKnown_) { dropBase_ = cur; dropBaseKnown_ = true; }
            int target = dropBase_ - DROP_QTY;
            if (cur > target) {
                engine::dropItemFromInventory(ctx.gw, hand_[0], probeSid_, probeType_,
                                              cur - target);
                cur = countProbe(ctx, 0);
            }
            if ((cur >= 0 && cur <= target) || ctx.elapsedMs >= CONTEND_DROP_DEADLINE_MS) {
                int dropped = (cur >= 0) ? (dropBase_ - cur) : 0;
                dropEmitted_ = true;
                char b[144];
                _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE DROP rank=0 n=%d sid='%s'",
                          dropped, probeSid_[0] ? probeSid_ : "(none)");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else if (ctx.elapsedMs >= CONTEND_DROP_DEADLINE_MS) {
            dropEmitted_ = true;
            char b[144];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE DROP rank=0 n=0 sid='%s'",
                      probeSid_[0] ? probeSid_ : "(none)");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Contention claims: ranks 1 and 2 poll the ground on a fast 100ms cadence
    // across a WIDE own-clock window (the drop lands mid-window on every
    // instance regardless of arming skew) and claim on FIRST SIGHT - both
    // mirrors receive the drop's world-item CREATE in the same host publish,
    // so the two claims reach the host within the 250ms contention window and
    // the arbiter names exactly one winner; the loser's applyClaimOutcome
    // rollback (coverage gap D4) fires live. The loser's rolled-back copy is
    // removed via removeWorldItemProxy (never re-grounded as a claimable), and
    // claimEmitted_ latches after ONE pickup attempt - no re-claim loops.
    void tickClaims(const ScenarioContext& ctx) {
        if (claimEmitted_) return;
        if (ownRank_ != 1u && ownRank_ != 2u) { claimEmitted_ = true; return; }
        if (ctx.elapsedMs < CLAIM_POLL_START_MS) return;
        if (ctx.elapsedMs >= CLAIM_DEADLINE_MS) {
            claimEmitted_ = true;
            char b[144];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE CLAIM rank=%u got=%d sid='%s'",
                      ownRank_, 0, probeSid_[0] ? probeSid_ : "(none)");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }
        if (lastClaimPollMs_ != 0 && (ctx.elapsedMs - lastClaimPollMs_) < CLAIM_POLL_MS)
            return;
        lastClaimPollMs_ = ctx.elapsedMs;
        if (!haveHand_[ownRank_] || !probeSid_[0]) return;
        int seen = engine::countFreeGroundItemsNear(ctx.gw, hand_[ownRank_], probeSid_,
                                                    probeType_, claimRadius());
        if (seen <= 0) return;
        int got = engine::pickupWorldItemIntoInventory(ctx.gw, hand_[ownRank_], probeSid_,
                                                       probeType_, claimRadius());
        claimEmitted_ = true;
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE CLAIM rank=%u got=%d sid='%s'",
                  ownRank_, got, probeSid_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ---- checkpoints: fixed leg-boundary census (SCENARIO CONSERVE) ----------
    // ck=0 (post-seed baseline, pre-legs) and ck=5 (final, post-everything)
    // are the two QUIESCENT points the oracle's Leg A/B/C verdicts anchor on;
    // ck=1..4 are per-leg diagnostics. Every ck sits >= 10s after its leg's
    // deadline on this instance's own clock (research Pitfall 8 settle floor),
    // and ck=5 (132s) sits 8s before the join retirement floor (140s) and 14s
    // after the contention settles on the earliest clock.
    void tickCheckpoints(const ScenarioContext& ctx) {
        while (nextCk_ < NUM_CK && ctx.elapsedMs >= CK_MS[nextCk_]) {
            emitCheckpoint(ctx, nextCk_);
            ++nextCk_;
        }
    }

    void emitCheckpoint(const ScenarioContext& ctx, unsigned int ck) {
        if (!probeSid_[0]) return;
        for (unsigned int r = 0; r < RANKS; ++r) {
            if (!haveHand_[r]) continue;
            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int n = engine::captureContainerContents(ctx.gw, hand_[r], items, INV_ITEMS_MAX, 0);
            unsigned int qty = 0;
            for (unsigned int i = 0; i < n; ++i)
                if (items[i].itemType == probeType_ && strcmp(items[i].stringID, probeSid_) == 0)
                    qty += (unsigned int)items[i].quantity;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO CONSERVE ck=%u scope=cont rank=%u hand=%u,%u,%u,%u,%u sid='%s' qty=%u",
                ck, r, hand_[r][0], hand_[r][1], hand_[r][2], hand_[r][3], hand_[r][4],
                probeSid_, qty);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (haveHand_[ownRank_]) {
            int grnd = engine::countFreeGroundItemsNear(ctx.gw, hand_[ownRank_], probeSid_,
                                                         probeType_, radius());
            if (grnd < 0) grnd = 0;
            char b[128];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CONSERVE ck=%u scope=ground sid='%s' qty=%d",
                      ck, probeSid_, grnd);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // joinDurationMs(): honors KENSHICOOP_SCENARIO_JOIN_DURATION_SEC for a
    // deferred/late-joining instance, verbatim from ScenarioMilestoneA.cpp /
    // ScenarioPlayerState.cpp.
    unsigned long joinDurationMs() const {
        const char* e = ::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC");
        if (!e || !*e) return JOIN_DURATION_MS;
        long s = ::atol(e);
        if (s < 10) s = 10;
        unsigned long ms = (unsigned long)s * 1000ul;
        return (ms < JOIN_DURATION_MS) ? ms : JOIN_DURATION_MS;
    }

    static float radius()      { return 60.0f; }  // checkpoint ground census
    static float claimRadius() { return 100.0f; } // contention sight/pickup

    static const unsigned int  RANKS         = 4;
    static const unsigned int  MAX_SQUAD     = 32;
    static const unsigned int  NUM_CK        = 6;
    static const unsigned int  NUM_XFER      = 5;
    // SEED_QTY=3 + qty=1 legs keep every container at <= 5 probes + 2 baked
    // gear = 7 entries; 8 entries is the empirically-proven wanderer capacity
    // (run 20260902_221642_N4 - see file header point 4).
    static const int           SEED_QTY      = 3;
    static const int           DROP_QTY      = 1;

    // HOST outlives the last-armed join's final checkpoint (arming skew up to
    // ~25s + join ck5 at 132s); joins retire at 140s as before.
    static const unsigned long HOST_DURATION_MS  = 165000;
    static const unsigned long JOIN_DURATION_MS  = 140000;

    static const unsigned long SEED_START_MS     = 8000;
    static const unsigned long SEED_EMIT_MS      = 26000;

    static const unsigned long CONTEND_DROP_MS          = 118000; // host clock
    static const unsigned long CONTEND_DROP_DEADLINE_MS = 124000;
    static const unsigned long CLAIM_POLL_START_MS      = 96000;  // own clocks
    static const unsigned long CLAIM_DEADLINE_MS        = 130000;
    static const unsigned long CLAIM_POLL_MS            = 100;

    // ck=0 (post-seed baseline) .. ck=5 (final). Each >= 10s after the leg
    // deadline it censuses; ck=5 sits 8s before JOIN_DURATION_MS (140000).
    static const unsigned long CK_MS[NUM_CK];
    static const XferLeg       LEGS[NUM_XFER];

    unsigned int  ownRank_;
    char          probeSid_[48];
    unsigned int  probeType_;

    bool          haveHand_[RANKS];
    unsigned int  hand_[RANKS][5];

    bool          seedEmitted_;
    bool          seedDone_;

    bool          xferEmitted_[NUM_XFER];
    bool          xferBaseKnown_[NUM_XFER];
    int           xferBase_[NUM_XFER];

    bool          dropBaseKnown_;
    int           dropBase_;
    bool          dropEmitted_;

    bool          claimEmitted_;
    unsigned long lastClaimPollMs_;

    unsigned int  nextCk_;
};

// ck=0 at 36s / first transfer at 64s: run 20260902_222940_N4 proved a
// transfer authored on the EARLIEST-armed instance's clock can land on the
// receiving owner's REAL container BEFORE a LATER-armed instance's ck=0
// baseline snapshot (join1's P23 at its own 38.4s committed 8s before join2's
// ck0 at its own 36s - arming skew ~11.5s), silently double-counting that
// transfer in the oracle's expected-delta ledger. The first transfer start
// (64s) minus ck0 (36s) = 28s clears the worst observed arming skew (~23.4s,
// join3 vs host) with margin, so EVERY instance's ck0 wall-time precedes
// EVERY transfer wall-time. ck1-4 are non-quiescent diagnostics only (the
// oracle judges ck0 + final ck).
const unsigned long ItemConservationScenario::CK_MS[ItemConservationScenario::NUM_CK] = {
    36000, 52000, 72000, 96000, 108000, 132000
};

// P2->P3, P3->P4, P4->host-container, then the simultaneous DISJOINT pair
// (rank1->rank0 and rank3->rank2 share one window - two concurrent
// cross-owner intents through the host arbiter). "host-container" reuses
// Plugin.cpp's EXISTING automatic setOwnedContainerHand(pickInventoryContainer)
// registration (fires whenever invSync && isHost): pickInventoryContainer
// resolves the host leader's OWN inventory hand, which IS rank 0's own
// squad-tab container, so P4 (rank 3) moving into it already exercises the
// host-as-participant short-circuit with zero extra registration code.
const ItemConservationScenario::XferLeg
ItemConservationScenario::LEGS[ItemConservationScenario::NUM_XFER] = {
    { 1u, 2u, 1, 64000,  70000 },   // P2 -> P3
    { 2u, 3u, 1, 72000,  78000 },   // P3 -> P4
    { 3u, 0u, 1, 80000,  86000 },   // P4 -> host container
    { 1u, 0u, 1, 88000,  94000 },   // concurrent pair A: P2 -> host
    { 3u, 2u, 1, 88000,  94000 },   // concurrent pair B: P4 -> P3
};

} // namespace

Scenario* makeItemConservationScenario(const std::string& name) {
    if (name == "item_conservation_gate") return new ItemConservationScenario();
    return 0;
}

} // namespace coop
