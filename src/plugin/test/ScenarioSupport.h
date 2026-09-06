// ScenarioSupport.h - the PRIVATE shared surface for the Scenario*.cpp domain
// TUs (monolith split of Scenario.cpp, 2026-07-12): the common include
// prelude, the shared SCENARIO-line emitters + squad-tab classification
// helpers (defined in ScenarioSupport.cpp), and the per-domain factory hooks
// that Scenario.cpp's makeScenario chains.
//
// The log emitters are load-bearing API: "SCENARIO MEMBER/RECV/VITALS ..."
// phrasing and field order are what the PowerShell oracles key on (see
// resources/CODE_MAP.md, log-tag index). Never change a format string here.
// Scenario classes themselves stay INSIDE their domain TU (anonymous
// namespace) - only the maker function crosses TUs, so Scenario.h and every
// caller stay unchanged.

#ifndef KENSHICOOP_SCENARIO_SUPPORT_H
#define KENSHICOOP_SCENARIO_SUPPORT_H

#define _CRT_SECURE_NO_WARNINGS 1

#include "Scenario.h"
#include "ScenarioTimed.h" // Phase 7: shared timed-scenario base (duration/cadence/passed)
#include "../CoopLog.h"
#include "../game/Engine.h"
#include "../game/EngineScenario.h" // Phase 5a: deterministic test-scene builders
#include "../game/EngineProbe.h"    // Phase 5a: spike-401/451/402 diagnostic probes
#include "../sync/SaveXfer.h" // save_probe / save_sync (protocol 31)

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <string>

namespace coop {

// ---- Shared helpers (ScenarioSupport.cpp; docs at the definitions) ----------

// One "SCENARIO <kind> hand=i,s,t,c,cs pos=x,y,z" line for character 'c'.
bool logScenarioLine(const char* kind, Character* c);
// Same, straight from a captured EntityState (adds task/pelvis/crouch/idle/bs).
void logScenarioEntity(const char* kind, const EntityState& e);
// Extended "SCENARIO VITALS" line for the body at hand h (readObjectHand layout).
void logVitalsLine(const unsigned int h[5], unsigned long t);
// "SCENARIO COMBATSTATE" combat-cohesion sample (readCombatByHand) for hand h.
// Emit on BOTH sides for the same baked hand so an oracle can pair host<->join
// and flag a copy fighting locally while the authority reports peace.
void logCombatStateLine(const unsigned int h[5], unsigned long t);
// Squad-tab classification (mirrors the Replicator's ownership partition).
bool tabHandLess(const EntityState& a, const EntityState& b);
bool tabCtnrLess(const EntityState& a, const EntityState& b);
bool tabCtnrEq(const EntityState& a, const EntityState& b);
int  tabRankOf(const EntityState* sq, unsigned int n, unsigned int i);
int  tabLeaderIdx(const EntityState* sq, unsigned int n, unsigned int rank);
// Fill h[5] (readObjectHand layout) from a captured EntityState's hand fields.
void handFromEntity(const EntityState& e, unsigned int h[5]);

// ---- Adaptive rank roster (Phase 11 plan 02, TEST-01) -----------------------
// A monotonic high-water-mark of every rank (= PlayerId, OwnRanks.h) this
// client has EVER seen connected - itself (observe()'s localId arg) plus
// whatever ctx.connectedPeers() reports. Ranks are never removed once
// observed: the locked N=4 gate scenarios (milestone_a_gate, player_state_
// gate, save_load_gate) rank-gate a single actor for the whole run (e.g. "the
// client at KO_TARGET_RANK downs its own leader"), and steps 7-8's own
// DISCONNECT/RECONNECT observation must not swap that actor's identity mid-
// run just because the harness drops a peer.
//
// Call observe() every tick (from onStart's first tick onward, before any
// rank-gated leg reads highest()/secondHighestJoin()/lowestTwoJoins()) so the
// roster is maximal by the time a leg's own elapsed-time gate - tens of
// seconds into the run - actually consults it. At N=4, once every join has
// connected at least once this converges to exactly {0,1,2,3}: highest()=3,
// secondHighestJoin(1)=2, lowestTwoJoins()=(1,2) - the frozen constants every
// N=4 gate already exercises, so those gates re-run against byte-unchanged
// behavior.
class RankRoster {
public:
    RankRoster() { for (unsigned int i = 0; i < MAX_PLAYERS; ++i) seen_[i] = false; }

    void observe(unsigned int localId, const ScenarioContext& ctx) {
        mark(localId);
        if (!ctx.connectedPeers) return;
        unsigned int ids[MAX_PLAYERS];
        unsigned int n = ctx.connectedPeers(ids, MAX_PLAYERS);
        for (unsigned int i = 0; i < n && i < MAX_PLAYERS; ++i) mark(ids[i]);
    }

    // Highest rank observed so far (0 if observe() has never been called -
    // callers always observe() their own localId first, so this is never
    // truly empty once a scenario has ticked at least once).
    unsigned int highest() const {
        for (unsigned int r = MAX_PLAYERS; r-- > 0; ) if (seen_[r]) return r;
        return 0;
    }

    // The second-highest JOIN rank observed (ranks 1..MAX_PLAYERS-1, host
    // rank 0 excluded), or 'fallbackRank' when fewer than two joins have ever
    // been observed (N<3 - only one join exists, so there is no "second").
    unsigned int secondHighestJoin(unsigned int fallbackRank) const {
        bool haveTop = false;
        for (unsigned int r = MAX_PLAYERS; r-- > 1; ) {
            if (!seen_[r]) continue;
            if (!haveTop) { haveTop = true; continue; } // skip the highest join
            return r;
        }
        return fallbackRank;
    }

    // The two LOWEST join ranks observed (*a < *b), for Leg R's rank-vs-rank
    // contest - the two ESTABLISHED joins, excluding whichever join (if any)
    // is the highest/late one. Returns false (the leg is N-inapplicable, N<3
    // - fewer than two joins have ever connected) rather than a degenerate
    // single contestant.
    bool lowestTwoJoins(unsigned int* a, unsigned int* b) const {
        unsigned int found = 0;
        for (unsigned int r = 1; r < MAX_PLAYERS; ++r) {
            if (!seen_[r]) continue;
            if (found == 0) { *a = r; ++found; }
            else if (found == 1) { *b = r; return true; }
        }
        return false;
    }

private:
    void mark(unsigned int id) { if (id < MAX_PLAYERS) seen_[id] = true; }
    bool seen_[MAX_PLAYERS];
};

// ---- Zone-cell geometry (shared by cell_probe and escape_cohesion) ----------
// engine::cellAt with the two coords swapped by axis, so one bisection body
// serves both axes.
bool cellAtAxis(GameWorld* gw, bool axisX, float fixed, float v, int* cx, int* cz);
// The nearest zone-cell BOUNDARY along one axis, searching in the sign of 'dir'
// (+1 ascending, -1 descending) from 'start'. Bisected rather than computed
// from a cell-size constant, so it needs no assumption about the grid origin
// and keeps working if the mapping changes. Returns false when no boundary is
// found within 60000 u or the mapping is unreadable.
bool findCellEdge(GameWorld* gw, bool axisX, float fixed, float start, float dir,
                  float* outEdge);

// ---- Per-domain factory hooks (each returns 0 when the name is not its own;
// ---- Scenario.cpp's makeScenario chains them) --------------------------------

Scenario* makeMovementScenario(const std::string& name);  // ScenarioMovement.cpp
Scenario* makeNpcScenario(const std::string& name);       // ScenarioNpc.cpp
Scenario* makeCombatScenario(const std::string& name);    // ScenarioCombat.cpp
Scenario* makeMedicalScenario(const std::string& name);   // ScenarioMedical.cpp
Scenario* makeInventoryScenario(const std::string& name); // ScenarioInventory.cpp
Scenario* makeWorldItemScenario(const std::string& name); // ScenarioWorldItems.cpp
Scenario* makeCharStateScenario(const std::string& name); // ScenarioCharState.cpp
Scenario* makeProbeScenario(const std::string& name);     // ScenarioProbes.cpp
Scenario* makeBuildingScenario(const std::string& name);  // ScenarioBuildings.cpp
Scenario* makeSessionScenario(const std::string& name);   // ScenarioSession.cpp
Scenario* makeMilestoneAScenario(const std::string& name); // ScenarioMilestoneA.cpp
Scenario* makePlayerStateScenario(const std::string& name); // ScenarioPlayerState.cpp (Phase 6 plan 02)
Scenario* makeItemConservationScenario(const std::string& name); // ScenarioItemConservation.cpp (Phase 7 plan 03)
Scenario* makeWorldStateScenario(const std::string& name); // ScenarioWorldState.cpp (Phase 8 plan 03)
Scenario* makeConsensusScenario(const std::string& name); // ScenarioConsensus.cpp (Phase 9 plan 03)
Scenario* makeSaveLoadScenario(const std::string& name); // ScenarioSaveLoad.cpp (Phase 10 plan 03)

} // namespace coop

#endif // KENSHICOOP_SCENARIO_SUPPORT_H
