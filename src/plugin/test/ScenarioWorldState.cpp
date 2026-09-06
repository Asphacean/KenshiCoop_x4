// ScenarioWorldState.cpp - world_state_gate (Phase 8 plan 03, WORLD-01/02/03):
// the ONE live scenario exercising every locked world channel (build/door/
// prod on rank0, faction on rank1, deed on rank2, research on rank3) plus the
// 3-step contested-claim leg (D1 fresh contest host-party -> D2 host vacates,
// joins fresh-contest to lowest playerId -> D3 host returns, continuity keeps
// join1 driving) that proves WORLD-03's host-authoritative CellMap.h reduce
// (Plan 02) end to end on a real 4-process run. Test-WorldState (Legs A-D,
// scripts/CoopOraclesN.psm1) judges the archived logs offline.
//
// Modeled on ScenarioItemConservation.cpp's 4-instance gate scaffold
// (ownRank_ = ctx.localId, TimedScenario HOST/JOIN duration split, "SCENARIO
// MAGATE start" so Get-MagateOwnRank resolves this scenario's logs too) and
// the proven per-channel levers from ScenarioBuildings.cpp (door_sync/
// deed_sync/build_sync/prod_sync/research_sync) and ScenarioProbes.cpp
// (faction_sync, cell_probe's cell geometry).
//
// Mutation discipline (Phase 7 locked lesson, reused verbatim): every
// mutation is CENSUS-VERIFIED with a retry deadline - never trust a lever's
// bare bool return alone. Every leg keeps emitting its SCENARIO WORLD
// evidence line regardless of lever success, so a lever false-negative fails
// LOUD in the oracle instead of silently burning a live run (research
// Pitfall 7).
//
// Rank-distinct sentinels: only ONE rank ever WRITES a given channel's
// sentinel (rank0 = build/door/prod, rank1 = faction, rank2 = deed, rank3 =
// research), so legs never contend. Faction/research sentinels are picked
// DETERMINISTICALLY from a sorted census every rank can read independently
// (the faction_sync/research_sync precedent: "gamedata enumeration order is
// shared, so every client picks the same sid" - Engine.h's researchPickSubject
// doc), so every instance (not just the writer) emits that channel's own
// read-back evidence - a genuine 4-way witness. Build/prod/door are
// placer-scoped RUNTIME objects (the protocol-27 identity problem: minted
// hands are not stable across instances), so only rank0 (the placer) emits
// build/prod/door SCENARIO WORLD lines; the other three instances' crossing
// proof comes from the production log's own [build] MINT/STATE-RECV/
// REMOVE-RECV, [prod] RECV and [bdoor] RECV lines, which Test-WorldState's
// Leg A/B read directly. Deed (08-05 gap closure) is WRITER-scoped for a
// different reason: its baked-building hands ARE cross-process-stable, but
// any every-rank pick filtered on CURRENT ownership races the deed write
// itself under arming skew (see the deed leg comment) - so only rank2 picks
// and emits, and the other instances' proof is their own "[deed] RECV ...
// ok=1" apply read-back.
//
// Contested-claim leg (D1/D2/D3, 08-06 redesign - ONLY JOINS MOVE): all four
// "Multiplayer (Wanderer x4)" squads spawn at The Hub (MultiplayerStartGen/
// Program.cs:60,165) - all four tabs begin in ONE 4608 u cell (the HOME
// cell), so D1's fresh contest (host is a party -> owner=0) is the initial
// condition, free. Every instance derives TWO fixed query points it re-reads
// via ScenarioContext::cellOwnerAt (the pickMintedProxy/connectedPeers
// adapter precedent - a scenario must not reach into the Replicator
// directly): HOME = its own leader's spawn, AWAY = a point ~2 k u inside the
// +X-adjacent cell, found by probing cellOwnerAt's out-coords from its own
// spawn (256 u steps to the boundary, then a 2048 u inset - every instance
// lands in the SAME away cell because all four spawn in one cell with <100 u
// spread; the oracle asserts that cross-instance cell agreement).
//
// The HOST NEVER MOVES. 08-04/08-05 measured (runs 135913/144325/145920/
// 152337) that no lever reliably relocates the host's own locally-SELECTED
// world-authority leader (park/ActivePlatoon::teleport: ok=1 zero movement;
// Character::teleport: inconsistent + far-jump zone churn -> cross-author
// runtime-hand collisions on the joins). Instead the JOINS move their OWN
// tab leaders - the exact mover class travel_parity (ScenarioMovement.cpp)
// already teleport-hops with engine::park across 15x4000 u legs:
//   D2: rank2 + rank3 move to AWAY at own-clock D2_MOVE_AT_MS -> fresh
//       contest in a cell where the host is NOT a party -> lowest playerId
//       among {2,3} -> owner=2 (proves host-if-party only applies when the
//       host IS a party, and the ascending-set reduce among joins).
//   D3: rank1 - the LOWEST playerId, the fresh-contest winner-to-be - moves
//       INTO the away cell at own-clock D3_MOVE_AT_MS -> continuity keeps
//       owner=2 (the incumbent still claims and is connected) even though a
//       fresh re-contest would pick 1. That is the locked reduce's FIRST
//       rule beating its THIRD, live - the direct analog of the original
//       "continuity beats the returning host" design, exercised without
//       moving the host. (continuity>host stays headless-proven: nettest's
//       cell-rejoin WORLD-03 cases.)
// Expected owner sequence: HOME 0 in all three windows; AWAY 2 in D2/D3 -
// claimSequence 0->2->2. A failed mover cannot vacuously pass: D2's away
// cell reads 0 (AUTHSRC_OPEN fail-open) if nobody arrives, and the oracle
// additionally requires each mover's own production "[cell] CLAIM
// rank=<r> cell=<away>" line (the claim PIPELINE saw the move, not just the
// body) - join1's is the proof the D3 continuity contest actually armed.
//
// The host emits ONE "SCENARIO WORLD claimphase" marker per transition
// (phase=2/3 at fixed host-clock times, no longer gated on any teleport
// verifying) so the oracle derives its D1/D2/D3 windows from the host's own
// WALL-CLOCK timestamps (Get-LogClockOffsetMs/Convert-StampToMs, the
// Get-CellMap/Test-SplitFar2 precedent) rather than duplicating timing
// constants in two languages. Movers fire 20 s BEFORE the corresponding
// marker (own clock) so worst-case arming skew (~25 s) + teleport verify +
// claim dwell (3 s at 1 Hz) + host reduce/broadcast (immediate on change,
// 5 s re-assert) all settle before the oracle's window opens at marker+30 s
// (the 07/08 arming-skew lesson: generous windows).
//
// Evidence discipline (locked, mirrors ScenarioItemConservation.cpp): the
// only NEW SCENARIO line this file emits is "SCENARIO WORLD ..." - no
// production [fac]/[door]/[bdoor]/[build]/[prod]/[research]/[deed]/[cell]
// log string is touched.
//
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

#include <vector>
#include <algorithm>

namespace coop {
namespace {

class WorldStateScenario : public TimedScenario {
public:
    WorldStateScenario()
        : TimedScenario("world_state_gate", 1000),
          ownRank_(0),
          haveSpawn_(false), spawnX_(0.0f), spawnY_(0.0f), spawnZ_(0.0f),
          doorShackPlaced_(false), doorShackPlaceOk_(false),
          doorRampStep_(0), doorRampDone_(false), nextDoorRampMs_(0),
          doorFoundOk_(false),
          doorWantKnown_(false), doorWant_(0),
          doorWrote_(false), doorWriteOk_(false),
          buildAPlaced_(false), buildAPlaceOk_(false),
          buildARampStep_(0), buildARampDone_(false), nextBuildARampMs_(0),
          buildBPlaced_(false), buildBPlaceOk_(false),
          buildBDestroyed_(false), buildBDestroyOk_(false),
          benchPlaced_(false), benchPlaceOk_(false),
          benchRampStep_(0), benchRampDone_(false), nextBenchRampMs_(0),
          opCount_(0), nextOpMs_(0), benchOutputWriteOk_(false),
          facPickOk_(false), facTargetKnown_(false), facTarget_(0.0f),
          facWrote_(false), facWriteOk_(false),
          deedPickOk_(false), deedWrote_(false), deedWriteOk_(false),
          researchPickOk_(false), researchStarted_(false), researchStartRc_(0),
          haveAway_(false), awayX_(0.0f), awayZ_(0.0f),
          homeCx_(0), homeCz_(0), awayCx_(0), awayCz_(0),
          markedD2_(false), markedD3_(false), moveVerified_(false),
          nextTeleportRetryMs_(0), teleportAttempts_(0) {
        memset(doorShackHand_, 0, sizeof(doorShackHand_)); doorShackSid_[0] = '\0';
        memset(sentinelDoorHand_, 0, sizeof(sentinelDoorHand_));
        memset(buildAHand_, 0, sizeof(buildAHand_)); buildASid_[0] = '\0';
        memset(buildBHand_, 0, sizeof(buildBHand_)); buildBSid_[0] = '\0';
        memset(benchHand_, 0, sizeof(benchHand_));   benchSid_[0] = '\0';
        sentinelFacSid_[0] = '\0';
        memset(sentinelDeedHand_, 0, sizeof(sentinelDeedHand_));
        sentinelResearchSid_[0] = '\0';
    }

    // "SCENARIO MAGATE start" reused verbatim (ScenarioItemConservation.cpp
    // precedent) so Get-MagateOwnRank resolves this scenario's own-rank
    // identity with the SAME helper every N=4 gate uses.
    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.localId;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MAGATE start ownRank=%u host=%d",
                  ownRank_, ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);

        // Own leader's spawn position - the contested-claim leg's fixed
        // reference point for the rest of the run (every rank spawns in the
        // SAME Hub cell per wanderer4's shared start). 08-05: captured through
        // a VALIDITY GUARD, retried each tick until it reads a plausible
        // position - run 20260903_151044_N4 showed the host leader's transform
        // transiently reading inf/near-origin during zone load (~6s in),
        // which the old bare readPos baked as spawn and poisoned every
        // host-authored leg (build 'factory null', claim cell=INT_MIN). The
        // mutation discipline this file already follows for every write leg,
        // applied to the ONE read the whole leg pins to.
        tryCaptureSpawn(ctx);

        // D1 begins the instant the scenario arms - all four tabs already sit
        // in the Hub spawn cell, so the fresh contest is the initial
        // condition. Host-only marker: the oracle's window boundaries are
        // ALWAYS derived from the host's own wall-clock timestamps.
        if (ctx.isHost) {
            char pb[80];
            _snprintf(pb, sizeof(pb) - 1, "SCENARIO WORLD claimphase phase=1 ok=1 t=0");
            pb[sizeof(pb) - 1] = '\0'; coop::logLine(pb);
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool due = evidenceDue(ctx.elapsedMs);

        // Keep retrying the spawn capture until a plausible position reads
        // (the load-transient guard - see onStart). Cheap and idempotent
        // once haveSpawn_ is set. The away-cell derivation follows the same
        // retry-until-verified discipline (see tryDeriveAway).
        if (!haveSpawn_) tryCaptureSpawn(ctx);
        if (haveSpawn_ && !haveAway_) tryDeriveAway(ctx);

        // ---- rank-distinct WRITE legs --------------------------------------
        if (ownRank_ == 0u) {
            tickDoor(ctx);
            tickBuildA(ctx);
            tickBuildB(ctx);
            tickBench(ctx);
        }
        if (ownRank_ == 1u) tickFaction(ctx);
        if (ownRank_ == 2u) tickDeed(ctx);
        if (ownRank_ == 3u) tickResearch(ctx);

        // ---- contested-claim leg (08-06 redesign): the host only marks the
        // phase schedule; rank2/3 move at D2, rank1 moves at D3 -------------
        if (ctx.isHost) tickClaimMarkers(ctx);
        else            tickClaimMove(ctx);

        // ---- 1 Hz evidence: every rank emits what it can independently
        // derive from the shared/baked world; build/prod are placer-scoped
        // (rank0 only) ----------------------------------------------------
        if (due) {
            emitFactionEvidence(ctx);
            // 08-05 gap closure: deed evidence is WRITER-scoped (rank2 only) -
            // see the deed leg comment for the pick-race root cause.
            if (ownRank_ == 2u) emitDeedEvidence(ctx);
            emitResearchEvidence(ctx);
            if (ownRank_ == 0u) {
                emitDoorEvidence(ctx); emitBuildEvidence(ctx); emitProdEvidence(ctx);
            }
            emitClaimEvidence(ctx);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : joinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // Local leg-ran verdict only - Test-WorldState (Legs A-D) judges
            // the cross-client convergence evidence. A mover that never
            // verified its own relocation fails ITS OWN leg locally too
            // (the oracle independently fails the run via the arrival
            // proofs and the away-cell owner assertions).
            bool isMover = (ownRank_ == 1u || ownRank_ == 2u || ownRank_ == 3u);
            passed_ = haveSpawn_ && haveAway_ && (!isMover || moveVerified_);
            return true;
        }
        return false;
    }

private:
    // Capture the leader's spawn once, through a validity guard: the position
    // must READ, be finite (no inf/nan), and sit well away from the world
    // origin (the wanderer4 Hub co-spawn is ~50000 u out; a transient
    // load-time transform reads near 0,0). Retried each tick until it passes,
    // so a garbage read during zone streaming never gets baked as the fixed
    // claim reference. Sets haveSpawn_ only on a clean read.
    void tryCaptureSpawn(const ScenarioContext& ctx) {
        if (haveSpawn_) return;
        Character* ld = engine::leader(ctx.gw);
        if (!ld) return;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!engine::readPos(ld, &x, &y, &z)) return;
        if (!isFinitePos(x) || !isFinitePos(y) || !isFinitePos(z)) return;
        // Reject a near-origin read (the load-transient signature). The real
        // Hub spawn is tens of thousands of units out on X/Z.
        if (x > -MIN_SPAWN_DIST && x < MIN_SPAWN_DIST &&
            z > -MIN_SPAWN_DIST && z < MIN_SPAWN_DIST) return;
        spawnX_ = x; spawnY_ = y; spawnZ_ = z;
        haveSpawn_ = true;
    }
    static bool isFinitePos(float v) {
        // C++03/VC10: nan != itself; inf exceeds any finite bound.
        return (v == v) && (v < 1e18f) && (v > -1e18f);
    }

    // ==== door leg (rank0 only: placer-scoped runtime hand) ==================
    // 08-04 fix: no longer an existing-door search (see the doorShack_ field
    // comment) - rank0 mints its own door-bearing shack (bdoor_probe/
    // bdoor_sync precedent: wantDoor=true selects a walk-in template whose
    // GameData mints DoorStuff children), ramps it to completion, then
    // locates its door via enumDoorsNear (guaranteed to succeed - the shack
    // sits right next to the leader, same anchor discipline as buildA/B).
    void tickDoorPlace(const ScenarioContext& ctx) {
        if (!doorShackPlaced_ && ctx.elapsedMs >= DOOR_A_PLACE_AT_MS) {
            doorShackPlaced_ = true;
            float x = 0, y = 0, z = 0, yaw = 0;
            int rc = engine::probePlaceBuilding(ctx.gw, -8.0f, 0.0f, /*wantDoor*/true,
                                                doorShackHand_, doorShackSid_,
                                                sizeof(doorShackSid_), &x, &y, &z, &yaw);
            doorShackPlaceOk_ = (rc == 1) && (doorShackHand_[4] != 0 || doorShackHand_[3] != 0);
            if (doorShackPlaceOk_) nextDoorRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        }
        if (doorShackPlaceOk_ && !doorRampDone_ && doorRampStep_ < MAX_RAMP_STEPS &&
            ctx.elapsedMs >= nextDoorRampMs_) {
            ++doorRampStep_;
            nextDoorRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
            float want = 0.25f * (float)doorRampStep_;
            if (want > 1.0f) want = 1.0f;
            engine::BuildRead post;
            if (engine::writeBuildProgressByHand(doorShackHand_, want, want >= 1.0f, &post) &&
                post.complete) doorRampDone_ = true;
        }
        if (doorRampDone_ && !doorFoundOk_) {
            engine::DoorRead rows[MAX_DOORS];
            unsigned int n = engine::enumDoorsNear(ctx.gw, 100.0f, rows, MAX_DOORS);
            for (unsigned int i = 0; i < n; ++i) {
                if (rows[i].parentHand[3] == doorShackHand_[3] &&
                    rows[i].parentHand[4] == doorShackHand_[4]) {
                    memcpy(sentinelDoorHand_, rows[i].hand, sizeof(unsigned int) * 5);
                    doorFoundOk_ = true;
                    break;
                }
            }
        }
    }

    void tickDoor(const ScenarioContext& ctx) {
        tickDoorPlace(ctx);
        if (doorWrote_ || ctx.elapsedMs < DOOR_WRITE_AT_MS) return;
        if (!doorFoundOk_) {
            if (ctx.elapsedMs >= DOOR_WRITE_DEADLINE_MS) doorWrote_ = true;
            return;
        }
        engine::DoorRead cur;
        if (engine::readDoorByHand(sentinelDoorHand_, &cur)) {
            if (!doorWantKnown_) { doorWant_ = cur.open ? 0 : 1; doorWantKnown_ = true; }
            if (cur.open == doorWant_) {
                doorWriteOk_ = true;
            } else {
                engine::DoorRead post;
                engine::writeDoorByHand(sentinelDoorHand_, doorWant_, /*lock untouched*/ -1, &post);
            }
        }
        if (doorWriteOk_ || ctx.elapsedMs >= DOOR_WRITE_DEADLINE_MS) doorWrote_ = true;
    }

    void emitDoorEvidence(const ScenarioContext& ctx) {
        if (!doorFoundOk_) return;
        engine::DoorRead cur;
        if (!engine::readDoorByHand(sentinelDoorHand_, &cur)) return;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WORLD door hand=%u.%u.%u.%u.%u open=%d locked=%d t=%lu",
                  sentinelDoorHand_[0], sentinelDoorHand_[1], sentinelDoorHand_[2],
                  sentinelDoorHand_[3], sentinelDoorHand_[4],
                  cur.open, cur.locked, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== build leg (rank0 only: placer-scoped runtime hands) ===============
    // Sentinel A: place -> ramp to complete. Sentinel B: place -> destroy
    // (the removal leg). Distinct leader-relative spots so neither collides.
    void tickBuildA(const ScenarioContext& ctx) {
        if (!buildAPlaced_ && ctx.elapsedMs >= BUILD_A_PLACE_AT_MS) {
            buildAPlaced_ = true;
            float x = 0, y = 0, z = 0, yaw = 0;
            int rc = engine::probePlaceBuilding(ctx.gw, 8.0f, -8.0f, /*wantDoor*/false,
                                                buildAHand_, buildASid_, sizeof(buildASid_),
                                                &x, &y, &z, &yaw);
            buildAPlaceOk_ = (rc == 1) && (buildAHand_[4] != 0 || buildAHand_[3] != 0);
            if (buildAPlaceOk_) nextBuildARampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        }
        if (buildAPlaceOk_ && !buildARampDone_ && buildARampStep_ < MAX_RAMP_STEPS &&
            ctx.elapsedMs >= nextBuildARampMs_) {
            ++buildARampStep_;
            nextBuildARampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
            float want = 0.25f * (float)buildARampStep_;
            if (want > 1.0f) want = 1.0f;
            engine::BuildRead post;
            if (engine::writeBuildProgressByHand(buildAHand_, want, want >= 1.0f, &post) &&
                post.complete) buildARampDone_ = true;
        }
    }

    void tickBuildB(const ScenarioContext& ctx) {
        if (!buildBPlaced_ && ctx.elapsedMs >= BUILD_B_PLACE_AT_MS) {
            buildBPlaced_ = true;
            float x = 0, y = 0, z = 0, yaw = 0;
            int rc = engine::probePlaceBuilding(ctx.gw, 8.0f, 8.0f, /*wantDoor*/false,
                                                buildBHand_, buildBSid_, sizeof(buildBSid_),
                                                &x, &y, &z, &yaw);
            buildBPlaceOk_ = (rc == 1) && (buildBHand_[4] != 0 || buildBHand_[3] != 0);
        }
        if (buildBPlaceOk_ && !buildBDestroyed_ && ctx.elapsedMs >= BUILD_B_DESTROY_AT_MS) {
            buildBDestroyed_ = true;
            buildBDestroyOk_ = engine::destroyBuildingByHand(ctx.gw, buildBHand_);
            // The programmatic destroy never passes the dismantle notification
            // (bdoor_probe precedent); queue the removal edge manually so the
            // production [build] REMOVE-RECV leg actually streams.
            if (buildBDestroyOk_) engine::queueRemoveEdge(buildBHand_);
        }
    }

    void emitBuildEvidence(const ScenarioContext& ctx) {
        if (buildAPlaceOk_) {
            engine::BuildRead cur;
            if (engine::readBuildingByHand(buildAHand_, &cur)) {
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO WORLD build key=%u.%u.%u.%u.%u prog=%.3f removed=0 t=%lu",
                          buildAHand_[0], buildAHand_[1], buildAHand_[2],
                          buildAHand_[3], buildAHand_[4], cur.progress, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (buildBPlaceOk_) {
            engine::BuildRead cur;
            bool resolves = engine::readBuildingByHand(buildBHand_, &cur);
            float prog = resolves ? cur.progress : -1.0f;
            int removed = (buildBDestroyed_ && buildBDestroyOk_) ? 1 : 0;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO WORLD build key=%u.%u.%u.%u.%u prog=%.3f removed=%d t=%lu",
                      buildBHand_[0], buildBHand_[1], buildBHand_[2],
                      buildBHand_[3], buildBHand_[4], prog, removed, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // ==== prod leg (rank0 only: host-authored, placer-scoped hand) ==========
    void tickBench(const ScenarioContext& ctx) {
        if (!benchPlaced_ && ctx.elapsedMs >= BENCH_PLACE_AT_MS) {
            benchPlaced_ = true;
            int rc = engine::probePlaceMachine(ctx.gw, 10.0f, 0.0f, /*kind*/1 /*bench*/,
                                               benchHand_, benchSid_, sizeof(benchSid_));
            benchPlaceOk_ = (rc == 1) && (benchHand_[4] != 0 || benchHand_[3] != 0);
            if (benchPlaceOk_) nextBenchRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        }
        if (benchPlaceOk_ && !benchRampDone_ && benchRampStep_ < MAX_RAMP_STEPS &&
            ctx.elapsedMs >= nextBenchRampMs_) {
            ++benchRampStep_;
            nextBenchRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
            float want = 0.25f * (float)benchRampStep_;
            if (want > 1.0f) want = 1.0f;
            engine::BuildRead post;
            if (engine::writeBuildProgressByHand(benchHand_, want, want >= 1.0f, &post) &&
                post.complete) benchRampDone_ = true;
        }
        if (benchRampDone_ && ctx.elapsedMs >= BENCH_OP_START_MS &&
            ctx.elapsedMs < BENCH_OP_END_MS && ctx.elapsedMs >= nextOpMs_) {
            nextOpMs_ = ctx.elapsedMs + BENCH_OP_PERIOD_MS;
            engine::operateMachineByHand(ctx.gw, benchHand_, 1.0f);
            ++opCount_;
        }
        // 08-04 live-gate fix: a live run showed operateMachineByHand alone
        // never materializes outAmount for a BCTYPE_CRAFTING bench within
        // the test window - fillProdRead's own documented sentinel
        // ("the buffer only materializes on the first production tick")
        // needs a real worked crafting session (skill roll + input
        // materials this scenario never provisions), not a bare operate()
        // nudge. Mirrors ScenarioBuildings.cpp's prod_probe doOutputWrite
        // precedent: a useSetItem=true call materializes the output
        // buffer/template (a follow-up live run confirmed this alone only
        // reaches amt=0.000, not the requested amount - the native
        // setProductionItem lever's stack/progress split evidently isn't
        // an instant absolute set), so a SECOND useSetItem=false DIRECT
        // write (the "clamp/fight probe" path: outBuf->amount = outAmount
        // verbatim) pins the final value onto the now-existing buffer -
        // the same two-step recipe prod_probe's own doOutputWrite exercises
        // via its separate setitem/direct test legs, just sequenced here
        // instead of split across two scenario steps.
        if (benchRampDone_ && !benchOutputWriteOk_ &&
            ctx.elapsedMs >= BENCH_OP_START_MS) {
            engine::ProdRead post;
            engine::writeMachineByHand(benchHand_, /*power*/-1, /*outAmount*/5.0f,
                                       /*useSetItem*/true, /*in*/0, /*farm*/0, &post);
            bool ok2 = engine::writeMachineByHand(benchHand_, /*power*/-1, /*outAmount*/5.0f,
                                                  /*useSetItem*/false, /*in*/0, /*farm*/0, &post);
            if (ok2 && post.outAmount >= 4.0f) benchOutputWriteOk_ = true;
        }
    }

    void emitProdEvidence(const ScenarioContext& ctx) {
        if (!benchPlaceOk_) return;
        engine::ProdRead cur;
        if (!engine::readMachineByHand(benchHand_, &cur)) return;
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WORLD prod key=%u.%u.%u.%u.%u amt=%.3f t=%lu",
                  benchHand_[0], benchHand_[1], benchHand_[2],
                  benchHand_[3], benchHand_[4], cur.outAmount, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== faction leg (rank1 writes; every rank reads) =======================
    // Deterministic pick: faction sids sorted ascending, index 0 - the
    // faction_probe precedent, independently derivable on every instance
    // (shared game data).
    bool ensureFacPicked(const ScenarioContext& ctx) {
        if (facPickOk_) return true;
        if (ctx.elapsedMs < PICK_AT_MS) return false;
        engine::FactionRead rows[MAX_FACTIONS];
        unsigned int n = engine::listPlayerRelations(ctx.gw, rows, MAX_FACTIONS);
        if (n == 0) return false;
        std::vector<std::string> sids;
        for (unsigned int i = 0; i < n; ++i) sids.push_back(std::string(rows[i].sid));
        std::sort(sids.begin(), sids.end());
        strncpy(sentinelFacSid_, sids[0].c_str(), sizeof(sentinelFacSid_) - 1);
        sentinelFacSid_[sizeof(sentinelFacSid_) - 1] = '\0';
        facPickOk_ = true;
        return true;
    }

    void tickFaction(const ScenarioContext& ctx) {
        if (facWrote_ || ctx.elapsedMs < FAC_WRITE_AT_MS) return;
        if (!ensureFacPicked(ctx)) {
            if (ctx.elapsedMs >= FAC_WRITE_DEADLINE_MS) facWrote_ = true;
            return;
        }
        float us = -999.0f, them = -999.0f;
        engine::readRelationBySid(ctx.gw, sentinelFacSid_, &us, &them);
        // 08-04 live-gate fix: a live run showed the picked sentinel's BAKED
        // relation in wanderer4 happened to already equal the old fixed
        // FAC_TARGET (-75.0) - the write was a permanent no-op (rel already
        // "converged" before FAC_WRITE_AT_MS ever fired), so no [fac] SEND/
        // RECV ever crossed. Choose the target relative to the CURRENT
        // baseline (the door leg's own cur.open?0:1 toggle-relative-to-
        // current pattern), guaranteed to differ from whatever baseline
        // this fixture bakes.
        if (!facTargetKnown_ && us > -900.0f) {
            facTarget_ = (us > -25.0f) ? FAC_TARGET_LOW : FAC_TARGET_HIGH;
            facTargetKnown_ = true;
        }
        if (!facTargetKnown_) return; // no baseline read yet - retry next tick
        if (us > -900.0f && us > facTarget_ - 0.5f && us < facTarget_ + 0.5f) {
            facWriteOk_ = true;
        } else {
            float before = 0.0f, after = 0.0f;
            engine::writeRelationBySid(ctx.gw, sentinelFacSid_, facTarget_,
                                       /*reciprocal*/ true, &before, &after);
        }
        if (facWriteOk_ || ctx.elapsedMs >= FAC_WRITE_DEADLINE_MS) facWrote_ = true;
    }

    void emitFactionEvidence(const ScenarioContext& ctx) {
        if (!ensureFacPicked(ctx)) return;
        float us = -999.0f, them = -999.0f;
        if (!engine::readRelationBySid(ctx.gw, sentinelFacSid_, &us, &them)) return;
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO WORLD fac sid='%s' rel=%.1f t=%lu",
                  sentinelFacSid_, us, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== deed leg (rank2 ONLY: writer-scoped pick + evidence) ===============
    // 08-05 gap closure: the previous every-rank pick (buildings-near sorted
    // by (serial,index), first NOT-owned) RACED the deed write itself. The
    // owned==0 filter is TIME-VARYING - the deed write flips the sentinel
    // owned=0->1 on every instance the moment it crosses - and arming skew
    // puts the last-launched join's own-clock DEED_PICK_AT_MS after rank2's
    // own-clock DEED_WRITE_AT_MS in wall-clock terms. Runs 3+4 (20260903_
    // 134400/135913): join3 armed ~13-15s after join2(rank2); the write
    // crossed 7-8s BEFORE join3's pick fired ([deed] RECV ok=1 landed at
    // 14:00:38.9, join3 picked at 14:00:45.6), so join3 deterministically
    // skipped the now-owned sentinel and picked the next sorted hand -
    // explaining the "same divergent hand every run" signature. Building
    // hands themselves are CROSS-PROCESS-STABLE (both runs: all 4 instances
    // resolved and applied the SAME wire hand, [deed] RECV ... ok=1), so
    // this was never a channel/product identity problem.
    //
    // Fix (the door leg's own 08-04 redesign shape): only rank2 (the writer)
    // picks a sentinel and emits SCENARIO WORLD deed evidence - its own pick
    // cannot race its own write (pick at own 30s, write at own 35s, and no
    // other instance writes deeds). The other three instances' convergence/
    // crossing proof is the production log's own "[deed] RECV hand=
    // <writerHand> owned=*->1 ok=1" line - a post-write readDeedByHand
    // read-back (Replicator::applyDeeds), i.e. a genuine per-instance
    // applied-state witness Test-WorldState's Leg C reads directly.
    bool ensureDeedPicked(const ScenarioContext& ctx) {
        if (deedPickOk_) return true;
        if (ctx.elapsedMs < DEED_PICK_AT_MS) return false;
        // Radius 1000u (08-04): enumBuildingsNear is a POSITION-based query
        // from the leader's own position; the wide radius keeps the writer's
        // candidate set stable against spawn-point spread and proved to pick
        // a baked, cross-resolvable building (Market Stall) in runs 2-4.
        engine::DeedRead rows[MAX_DEED_ROWS];
        unsigned int n = engine::enumBuildingsNear(ctx.gw, 1000.0f, rows, MAX_DEED_ROWS);
        if (n == 0) return false;
        unsigned int order[MAX_DEED_ROWS];
        for (unsigned int i = 0; i < n; ++i) order[i] = i;
        for (unsigned int i = 0; i + 1 < n; ++i)
            for (unsigned int j = i + 1; j < n; ++j) {
                const engine::DeedRead& a = rows[order[i]];
                const engine::DeedRead& b = rows[order[j]];
                if (b.hand[4] < a.hand[4] ||
                    (b.hand[4] == a.hand[4] && b.hand[3] < a.hand[3])) {
                    unsigned int t = order[i]; order[i] = order[j]; order[j] = t;
                }
            }
        for (unsigned int i = 0; i < n; ++i) {
            if (rows[order[i]].owned == 0) {
                memcpy(sentinelDeedHand_, rows[order[i]].hand, sizeof(unsigned int) * 5);
                deedPickOk_ = true;
                return true;
            }
        }
        return false; // nothing unowned yet - retry next call
    }

    void tickDeed(const ScenarioContext& ctx) {
        if (deedWrote_ || ctx.elapsedMs < DEED_WRITE_AT_MS) return;
        if (!ensureDeedPicked(ctx)) {
            if (ctx.elapsedMs >= DEED_WRITE_DEADLINE_MS) deedWrote_ = true;
            return;
        }
        engine::DeedRead cur;
        if (engine::readDeedByHand(ctx.gw, sentinelDeedHand_, &cur)) {
            if (cur.owned == 1) {
                deedWriteOk_ = true;
            } else {
                engine::DeedRead post;
                engine::writeDeedByHand(ctx.gw, sentinelDeedHand_, /*wantOwned*/1, "", &post);
            }
        }
        if (deedWriteOk_ || ctx.elapsedMs >= DEED_WRITE_DEADLINE_MS) deedWrote_ = true;
    }

    void emitDeedEvidence(const ScenarioContext& ctx) {
        if (!ensureDeedPicked(ctx)) return;
        engine::DeedRead cur;
        if (!engine::readDeedByHand(ctx.gw, sentinelDeedHand_, &cur)) return;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WORLD deed hand=%u.%u.%u.%u.%u owned=%d t=%lu",
                  sentinelDeedHand_[0], sentinelDeedHand_[1], sentinelDeedHand_[2],
                  sentinelDeedHand_[3], sentinelDeedHand_[4], cur.owned, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== research leg (rank3 starts; every rank reads) ======================
    // Deterministic pick: researchPickSubject's own doc guarantees "gamedata
    // enumeration order is shared, so every client picks the same sid" - run
    // independently on every instance, snapshotted early (PICK_AT_MS) before
    // any research can have crossed (an already-known sid would fall out of
    // its own not-known-and-canResearch filter).
    bool ensureResearchPicked(const ScenarioContext& ctx) {
        if (researchPickOk_) return true;
        if (ctx.elapsedMs < PICK_AT_MS) return false;
        char sid[48]; sid[0] = '\0';
        int rc = engine::researchPickSubject(ctx.gw, sid, sizeof(sid));
        if (rc == 1 && sid[0]) {
            strncpy(sentinelResearchSid_, sid, sizeof(sentinelResearchSid_) - 1);
            sentinelResearchSid_[sizeof(sentinelResearchSid_) - 1] = '\0';
            researchPickOk_ = true;
        }
        return researchPickOk_;
    }

    void tickResearch(const ScenarioContext& ctx) {
        if (researchStarted_ || ctx.elapsedMs < RESEARCH_START_AT_MS) return;
        if (!ensureResearchPicked(ctx)) {
            if (ctx.elapsedMs >= RESEARCH_START_DEADLINE_MS) researchStarted_ = true;
            return;
        }
        int known = -1, can = -1;
        engine::researchQueryBySid(ctx.gw, sentinelResearchSid_, &known, &can);
        if (known == 1) {
            researchStarted_ = true;
        } else {
            researchStartRc_ = engine::researchStartBySid(ctx.gw, sentinelResearchSid_);
            if (ctx.elapsedMs >= RESEARCH_START_DEADLINE_MS) researchStarted_ = true;
        }
    }

    void emitResearchEvidence(const ScenarioContext& ctx) {
        if (!ensureResearchPicked(ctx)) return;
        int known = -1, can = -1;
        if (engine::researchQueryBySid(ctx.gw, sentinelResearchSid_, &known, &can) != 1) return;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO WORLD research sid='%s' known=%d t=%lu",
                  sentinelResearchSid_, known, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // ==== contested-claim leg (08-06 redesign: joins move, host marks) =======
    //
    // Away-point derivation (retry-until-verified, like tryCaptureSpawn):
    // probe cellOwnerAt's OUT-COORDS (pure engine::cellAt math - the owner
    // value is ignored here) eastward from the own spawn in 256 u steps
    // until the reported cell flips to (homeCx+1, homeCz), then land the
    // away point AWAY_INSET u past that first-found probe - between ~2.0 k
    // and ~2.6 k u inside the adjacent cell (cell width 4608 u), so the
    // spawn cluster's <100 u spread plus the probe grid's 256 u quantum can
    // never put two instances in different away cells. Verified by reading
    // the derived point's own cell back before latching (mutation
    // discipline, applied to a derived coordinate). Any unexpected mapping
    // (fault fills 0,0; a diagonal flip) aborts and retries next tick.
    void tryDeriveAway(const ScenarioContext& ctx) {
        if (haveAway_ || !haveSpawn_ || !ctx.cellOwnerAt) return;
        int hcx = 0, hcz = 0;
        ctx.cellOwnerAt(spawnX_, spawnZ_, &hcx, &hcz);
        // engine::cellAt faults/mid-load leave the out-coords at 0,0; the
        // wanderer4 Hub cell is well away from cell 0,0 (measured 20,32).
        if (hcx == 0 && hcz == 0) return;
        for (unsigned int k = 1; k <= AWAY_PROBE_MAX; ++k) {
            float x = spawnX_ + AWAY_PROBE_STEP * (float)k;
            int pcx = 0, pcz = 0;
            ctx.cellOwnerAt(x, spawnZ_, &pcx, &pcz);
            if (pcx == hcx && pcz == hcz) continue; // still home - keep probing
            if (pcx != hcx + 1 || pcz != hcz) return; // unexpected mapping - retry next tick
            float ax = x + AWAY_INSET;
            int acx = 0, acz = 0;
            ctx.cellOwnerAt(ax, spawnZ_, &acx, &acz);
            if (acx != hcx + 1 || acz != hcz) return;  // inset overshot?? - retry next tick
            homeCx_ = hcx; homeCz_ = hcz;
            awayCx_ = acx; awayCz_ = acz;
            awayX_ = ax; awayZ_ = spawnZ_;
            haveAway_ = true;
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO WORLD claimcells home=%d,%d away=%d,%d ax=%.0f az=%.0f t=%lu",
                      homeCx_, homeCz_, awayCx_, awayCz_, awayX_, awayZ_, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }
    }

    // HOST: pure timekeeping. One "SCENARIO WORLD claimphase" marker per
    // transition, at FIXED host-clock times (no longer gated on any teleport
    // verifying - the host does not move in this design, so the 08-05
    // "2/3 markers" evidence-gap failure mode is gone structurally). The
    // oracle derives its D1/D2/D3 judged windows from these wall-clock
    // stamps.
    void tickClaimMarkers(const ScenarioContext& ctx) {
        if (!markedD2_ && ctx.elapsedMs >= D2_MARK_AT_MS) {
            markedD2_ = true;
            char b[80];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WORLD claimphase phase=2 ok=1 t=%lu",
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (!markedD3_ && ctx.elapsedMs >= D3_MARK_AT_MS) {
            markedD3_ = true;
            char b[80];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WORLD claimphase phase=3 ok=1 t=%lu",
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // JOIN MOVERS: rank2/3 relocate to the away point at own-clock
    // D2_MOVE_AT_MS; rank1 follows at D3_MOVE_AT_MS (the continuity
    // contestant). Each mover fires 20 s before the host's corresponding
    // phase marker so skew + verify + claim dwell + broadcast settle before
    // the oracle's window opens (see file header). Retry-until-verified at
    // 1 Hz inside TELEPORT_VERIFY_WINDOW_MS; every attempt logs a
    // "SCENARIO WORLD claimverify" line (phase=/mode=/zone= forensics).
    void tickClaimMove(const ScenarioContext& ctx) {
        if (ownRank_ != 1u && ownRank_ != 2u && ownRank_ != 3u) return;
        if (!haveSpawn_ || !haveAway_ || moveVerified_) return;
        unsigned long at = (ownRank_ == 1u) ? D3_MOVE_AT_MS : D2_MOVE_AT_MS;
        if (ctx.elapsedMs < at) return;
        if (ctx.elapsedMs >= at + TELEPORT_VERIFY_WINDOW_MS) return; // budget spent - claimverify trail tells why
        if (ctx.elapsedMs < nextTeleportRetryMs_) return;
        nextTeleportRetryMs_ = ctx.elapsedMs + TELEPORT_RETRY_PERIOD_MS;
        doMoveAttempt(ctx, (ownRank_ == 1u) ? 3 : 2);
    }

    // One relocation attempt on the mover's OWN tab leader, with read-back
    // verification (mutation discipline). Body resolution follows
    // travel_parity's proven mover recipe verbatim (ScenarioMovement.cpp):
    // captureSquad + tabLeaderIdx(ownRank) + engine::resolve - NOT
    // engine::leader(), which is playerCharacters[0] (the locally SELECTED
    // char - the one body class 08-04/08-05 measured every teleport lever
    // failing on; travel_parity's join hops its own UNSELECTED tab leader
    // with park() across 15x4000 u legs, so park is the primary rung here).
    // Ladder (per-attempt mode=/zone= fields attribute everything from the
    // archived log alone):
    //   mode=2 engine::park - primary (the travel_parity join-mover
    //     precedent; falls through to mode=3 in the same attempt when the
    //     lever reports failure).
    //   mode=3 engine::teleportCharTo (Character::teleport, ABSOLUTE dest) -
    //     alternate rung from attempt 2 on (the lever that moved a player
    //     body in run 145920).
    //   mode=5 engine::cameraTeleport zone pre-stream - only when the away
    //     zone reads unloaded (unlikely at ~2.6 k u; kept as the last rung).
    // A generous +-50 u tolerance confirms real cell-scale movement without
    // being fooled by physics settle jitter; on verify a short walk order
    // re-grounds the body (the travel_parity dwell-leg precedent).
    void doMoveAttempt(const ScenarioContext& ctx, int phase) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int mi = tabLeaderIdx(sq, n, ownRank_);
        Character* c = (mi >= 0) ? engine::resolve(sq[mi]) : 0;
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        bool haveP = c && engine::readPos(c, &px, &py, &pz);
        float dx = haveP ? (px - awayX_) : 999999.0f;
        float dz = haveP ? (pz - awayZ_) : 999999.0f;
        bool verified = haveP && dx > -50.0f && dx < 50.0f && dz > -50.0f && dz < 50.0f;
        float destY = haveP ? py : spawnY_;
        engine::terrainHeightAt(awayX_, awayZ_, &destY);
        int zone = engine::isZoneLoadedAt(ctx.gw, awayX_, destY, awayZ_) ? 1 : 0;
        int mode = 0;
        bool ok = (c != 0);
        if (c && !verified) {
            // Attempts 0-1: park. From attempt 2: alternate charTeleport
            // (even) / park (odd), with the camera pre-stream rung stealing
            // an odd slot from attempt 5 on while the zone reads unloaded.
            if (teleportAttempts_ >= 4u && (teleportAttempts_ % 2u) == 1u && !zone) {
                mode = 5;
                ok = engine::cameraTeleport(ctx.gw, awayX_, destY, awayZ_);
            } else if (teleportAttempts_ >= 2u && (teleportAttempts_ % 2u) == 0u) {
                mode = 3;
                ok = engine::teleportCharTo(c, awayX_, destY, awayZ_);
                if (!ok) {
                    mode = 2;
                    ok = engine::park(c, awayX_, destY, awayZ_, 0.0f);
                }
            } else {
                mode = 2;
                ok = engine::park(c, awayX_, destY, awayZ_, 0.0f);
                if (!ok) {
                    mode = 3;
                    ok = engine::teleportCharTo(c, awayX_, destY, awayZ_);
                }
            }
            ++teleportAttempts_;
        }
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WORLD claimverify phase=%d ok=%d have=%d px=%.1f pz=%.1f "
                  "dx=%.1f dz=%.1f verified=%d mode=%d zone=%d t=%lu",
                  phase, ok ? 1 : 0, haveP ? 1 : 0, px, pz, dx, dz,
                  verified ? 1 : 0, mode, zone, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (verified) {
            moveVerified_ = true;
            // Re-ground the parked body with a short walk leg (travel_parity
            // precedent: keeps it a live, settled subject rather than a
            // statue mid-air) - the claim publisher reads its position at
            // 1 Hz and needs 3 stable same-cell samples (CELL_DWELL_N).
            engine::orderMoveTo(c, awayX_ + 12.0f, destY, awayZ_);
        }
    }

    // 1 Hz, every instance: TWO fixed query points - the HOME cell (own
    // spawn) and the AWAY cell (derived, see tryDeriveAway). Both read the
    // SAME host-authored adopted map via the cellOwnerAt adapter, so four
    // instances reporting the same owner for the same cell is a genuine
    // convergence witness of the PKT_CELL_MAP broadcast/adoption pipeline.
    void emitClaimEvidence(const ScenarioContext& ctx) {
        if (!haveSpawn_ || !ctx.cellOwnerAt) return;
        int cx = 0, cz = 0;
        unsigned int owner = ctx.cellOwnerAt(spawnX_, spawnZ_, &cx, &cz);
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WORLD claim site=home cell=%d,%d owner=%u t=%lu",
                  cx, cz, owner, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAway_) return;
        int ax = 0, az = 0;
        unsigned int aOwner = ctx.cellOwnerAt(awayX_, awayZ_, &ax, &az);
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WORLD claim site=away cell=%d,%d owner=%u t=%lu",
                  ax, az, aOwner, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // joinDurationMs(): honors KENSHICOOP_SCENARIO_JOIN_DURATION_SEC for a
    // deferred/late-joining instance, verbatim from ScenarioItemConservation.cpp.
    unsigned long joinDurationMs() const {
        const char* e = ::getenv("KENSHICOOP_SCENARIO_JOIN_DURATION_SEC");
        if (!e || !*e) return JOIN_DURATION_MS;
        long s = ::atol(e);
        if (s < 10) s = 10;
        unsigned long ms = (unsigned long)s * 1000ul;
        return (ms < JOIN_DURATION_MS) ? ms : JOIN_DURATION_MS;
    }

    static const unsigned int MAX_DOORS      = 64;
    static const unsigned int MAX_FACTIONS   = 96;
    static const unsigned int MAX_DEED_ROWS  = 64;
    static const unsigned int MAX_RAMP_STEPS = 8;
    // Spawn-validity guard (see tryCaptureSpawn): a plausible wanderer4 Hub
    // spawn is tens of thousands of units from origin; a load-transient
    // transform reads within a few units of 0,0. 1000u cleanly separates them.
    static const float MIN_SPAWN_DIST;

    // Picks happen as early as the 1 Hz evidence loop can reach them (every
    // instance's own clock) - before ANY write leg's earliest WRITE_AT_MS, so
    // a late-arming instance's own-owned-filtered picks (deed/research) still
    // see the pre-mutation census (the deed_sync/research_sync "snapshot
    // before anything can have crossed" precedent).
    static const unsigned long PICK_AT_MS = 8000;

    // Door: no longer a PICK_AT_MS-gated search (see the doorShack_ field
    // comment) - rank0 places+ramps its own shack on the same schedule as
    // buildA, at a distinct anchor spot so the two placements never collide.
    static const unsigned long DOOR_A_PLACE_AT_MS     = 10000;
    static const unsigned long DOOR_WRITE_AT_MS       = 35000;
    static const unsigned long DOOR_WRITE_DEADLINE_MS = 60000;

    static const unsigned long BUILD_A_PLACE_AT_MS    = 10000;
    static const unsigned long BUILD_B_PLACE_AT_MS    = 16000;
    static const unsigned long BUILD_B_DESTROY_AT_MS  = 40000;
    static const unsigned long RAMP_STEP_MS           = 5000;

    static const unsigned long BENCH_PLACE_AT_MS      = 20000;
    static const unsigned long BENCH_OP_START_MS      = 46000;
    static const unsigned long BENCH_OP_END_MS        = 66000;
    static const unsigned long BENCH_OP_PERIOD_MS     = 1000;

    static const unsigned long FAC_WRITE_AT_MS       = 12000;
    static const unsigned long FAC_WRITE_DEADLINE_MS = 27000;

    // Deed (08-05: WRITER-scoped, rank2 only - see the deed leg comment):
    // DEED_PICK_AT_MS=30000 gives the writer's own local building census
    // time to settle (enumBuildingsNear is a load-dependent local query;
    // an 8s pick raced zone streaming in run 2), and the writer's pick at
    // its OWN 30s always precedes its OWN write at 35s - no cross-instance
    // pick exists anymore, so arming skew cannot race the owned filter.
    static const unsigned long DEED_PICK_AT_MS        = 30000;
    static const unsigned long DEED_WRITE_AT_MS       = 35000;
    static const unsigned long DEED_WRITE_DEADLINE_MS = 60000;

    static const unsigned long RESEARCH_START_AT_MS       = 12000;
    static const unsigned long RESEARCH_START_DEADLINE_MS = 27000;

    // Contested-claim leg schedule (own clocks; the oracle re-derives its
    // judged windows from the HOST's WALL-CLOCK claimphase markers, not
    // these constants - see file header). Host marks phase=2/3 at the
    // MARK times; movers fire 20 s EARLIER on their own clocks so worst-case
    // arming skew (~25 s) + verify + claim dwell (3 s at 1 Hz) + host
    // reduce/broadcast (immediate on change; 5 s re-assert backstop) settle
    // before the oracle's window opens at marker+30 s. D1 is judged
    // [p1+30, p2-5], D2 [p2+30, p3-5], D3 [p3+30, p3+55].
    static const unsigned long D2_MOVE_AT_MS = 100000; // rank2 + rank3 (own clock)
    static const unsigned long D3_MOVE_AT_MS = 160000; // rank1 (own clock)
    static const unsigned long D2_MARK_AT_MS = 120000; // host phase=2 marker
    static const unsigned long D3_MARK_AT_MS = 180000; // host phase=3 marker
    // Retry-until-verified budget per mover (see doMoveAttempt); 1 s cadence
    // (one attempt per scenario tick) so the lever ladder completes well
    // before the oracle's settle-tolerance sub-window.
    static const unsigned long TELEPORT_VERIFY_WINDOW_MS = 20000;
    static const unsigned long TELEPORT_RETRY_PERIOD_MS  = 1000;

    // Away-point probe (tryDeriveAway): 256 u steps, up to 24 (6144 u > one
    // 4608 u cell width, so the +X boundary is always inside the sweep);
    // 2048 u inset past the first probe that reads the adjacent cell puts
    // the point 2.0-2.6 k u inside it - a small hop in the proven
    // splitfar/travel_parity class, nowhere near the 08-05 far-jump churn.
    static const unsigned int  AWAY_PROBE_MAX = 24;
    static const float         AWAY_PROBE_STEP;
    static const float         AWAY_INSET;
    static const unsigned int  MAX_SQUAD = 32;

    // HOST outlives D3's judged window ([p3+30, p3+55] -> 235 s) with 15 s
    // margin; JOIN duration covers the same wall window even for a zero-skew
    // join (240 >= 235; later-armed joins only extend the coverage - the
    // arming-skew margin ItemConservationScenario's file header documents).
    static const unsigned long HOST_DURATION_MS = 250000;
    static const unsigned long JOIN_DURATION_MS = 240000;

    // 08-04 live-gate fix: two well-separated candidate targets instead of
    // one fixed value - tickFaction picks whichever is farther from the
    // read-back baseline, so the write is never a baseline-matching no-op.
    static const float FAC_TARGET_LOW;
    static const float FAC_TARGET_HIGH;

    unsigned int  ownRank_;

    bool          haveSpawn_;
    float         spawnX_, spawnY_, spawnZ_;

    // Door leg (08-04 live-gate fix): wanderer4's actual co-op spawn has NO
    // pre-existing door within enumDoorsNear's 100u search radius (a
    // "Wanderer" start begins in the wilderness, not literally inside Hub's
    // building cluster, even though all four squads share Hub's CELL) - a
    // live run confirmed zero door evidence on all 4 instances the whole
    // run. Fixed by mirroring the build/prod legs' already-proven
    // self-sufficient pattern (bdoor_probe/bdoor_sync precedent,
    // ScenarioBuildings.cpp): rank0 MINTS its own door-bearing shack near
    // the leader (wantDoor=true), ramps it to completion, then locates its
    // door via enumDoorsNear (now guaranteed to succeed - the shack sits
    // right next to the leader). This makes the door hand a placer-scoped
    // RUNTIME hand (the protocol-27 identity problem), so - like build/prod
    // - only rank0 emits SCENARIO WORLD door evidence; the other 3
    // instances' crossing proof is the production log's own [door] SEND/
    // RECV lines (Test-WorldState's Leg A reads them directly).
    bool          doorShackPlaced_, doorShackPlaceOk_;
    unsigned int  doorShackHand_[5];
    char          doorShackSid_[48];
    unsigned int  doorRampStep_;
    bool          doorRampDone_;
    unsigned long nextDoorRampMs_;
    bool          doorFoundOk_;
    unsigned int  sentinelDoorHand_[5];
    bool          doorWantKnown_;
    int           doorWant_;
    bool          doorWrote_;
    bool          doorWriteOk_;

    bool          buildAPlaced_, buildAPlaceOk_;
    unsigned int  buildAHand_[5];
    char          buildASid_[48];
    unsigned int  buildARampStep_;
    bool          buildARampDone_;
    unsigned long nextBuildARampMs_;

    bool          buildBPlaced_, buildBPlaceOk_;
    unsigned int  buildBHand_[5];
    char          buildBSid_[48];
    bool          buildBDestroyed_, buildBDestroyOk_;

    bool          benchPlaced_, benchPlaceOk_;
    unsigned int  benchHand_[5];
    char          benchSid_[48];
    unsigned int  benchRampStep_;
    bool          benchRampDone_;
    unsigned long nextBenchRampMs_;
    unsigned int  opCount_;
    unsigned long nextOpMs_;
    bool          benchOutputWriteOk_;

    bool          facPickOk_;
    char          sentinelFacSid_[48];
    // 08-04 live-gate fix: the picked sentinel faction's BAKED relation in
    // wanderer4 happened to already equal the old fixed FAC_TARGET (-75.0),
    // making the write a permanent no-op (rel==target before the write leg
    // ever ran) - no [fac] SEND/RECV ever fired live. facTarget_ is now
    // chosen relative to the CURRENT read value (the door leg's own
    // cur.open?0:1 toggle-relative-to-current pattern), guaranteed to
    // differ from baseline.
    bool          facTargetKnown_;
    float         facTarget_;
    bool          facWrote_;
    bool          facWriteOk_;

    bool          deedPickOk_;
    unsigned int  sentinelDeedHand_[5];
    bool          deedWrote_;
    bool          deedWriteOk_;

    bool          researchPickOk_;
    char          sentinelResearchSid_[48];
    bool          researchStarted_;
    int           researchStartRc_;

    // Contested-claim leg state (08-06 redesign). Every instance derives the
    // away query point; only ranks 1-3 ever move (each exactly once).
    bool          haveAway_;
    float         awayX_, awayZ_;
    int           homeCx_, homeCz_;
    int           awayCx_, awayCz_;
    bool          markedD2_, markedD3_;   // host phase markers (one-shot)
    bool          moveVerified_;          // read-back confirmed the relocation
    unsigned long nextTeleportRetryMs_;
    unsigned int  teleportAttempts_;      // ladder rung selector
};

const float WorldStateScenario::FAC_TARGET_LOW  = -75.0f;
const float WorldStateScenario::FAC_TARGET_HIGH = 25.0f;
const float WorldStateScenario::AWAY_PROBE_STEP = 256.0f;
const float WorldStateScenario::AWAY_INSET      = 2048.0f;
const float WorldStateScenario::MIN_SPAWN_DIST  = 1000.0f;

} // namespace

Scenario* makeWorldStateScenario(const std::string& name) {
    if (name == "world_state_gate") return new WorldStateScenario();
    return 0;
}

} // namespace coop
