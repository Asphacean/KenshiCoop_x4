// Scenario - deterministic, log-emitting test scripts driven from the tick hook.
//
// A scenario runs on BOTH clients (it branches on ctx.isHost). The authoritative
// side emits "SCENARIO MEMBER hand=.. pos=.." lines; the observing side emits
// "SCENARIO RECV hand=.. pos=.." lines. The PowerShell runner cross-checks
// MEMBER vs RECV positions per hand within a tolerance, in addition to requiring
// "SCENARIO RESULT PASS" from each client. Keep these schema strings stable -
// run_test.ps1 parses them.

#ifndef KENSHICOOP_SCENARIO_H
#define KENSHICOOP_SCENARIO_H

#include <string>
#include "../../netproto/Wire.h"

class GameWorld;

namespace coop {

struct ScenarioContext {
    GameWorld*    gw;
    bool          isHost;
    u32           localId;
    unsigned long elapsedMs; // since onStart
    unsigned int  tick;      // scenario tick counter
    // True once this client has received an owned-entity batch from a peer. On the
    // HOST this means the JOIN is loaded + streaming, so it is the correct gate for
    // any time-sensitive host action (e.g. a live spawn) that the join must witness.
    // Always false in HostOnly runs (no peer) - gate with a timeout fallback there.
    bool          peerReady;
    // assault_mint: describes the MINTED proxy nearest the body at refHand -
    // outLocal[5] is the hand that resolves on THIS client (what an order needs),
    // outCanon[5] the peer's key it is streamed by, outDist the planar distance.
    // Only the replicator's proxy table can answer this, and a scenario must not
    // reach into the replicator, so Plugin.cpp supplies the adapter (0 when
    // unavailable - always check).
    bool        (*pickMintedProxy)(const unsigned int refHand[5],
                                   unsigned int outLocal[5],
                                   unsigned int outCanon[5], float* outDist);
    // milestone_a_gate (Phase 4 plan 04, PEER-02/03 reuse): snapshot of the
    // OTHER PlayerIds this client currently sees as connected (Plugin.cpp's
    // g_connectedPeers - the roster every client, host or join, maintains via
    // the connect/leave broadcast). Fills outIds (capacity outCap) and returns
    // the count written. A scenario must not reach into Plugin.cpp/NetLink
    // directly, so Plugin.cpp supplies this adapter (0 when unavailable -
    // always check before calling, same discipline as pickMintedProxy).
    unsigned int (*connectedPeers)(unsigned int* outIds, unsigned int outCap);
    // world_state_gate's contested-claim leg (Phase 8 plan 03, WORLD-03):
    // exposes Replicator::authoritySrc's verdict for world position (x,z) -
    // the SAME host-authoritative cell-claim map every real consumer
    // (authorityFor/census/enforceHostAuthority) reads - without the
    // scenario layer reaching into the Replicator directly (the
    // pickMintedProxy/connectedPeers adapter precedent above). Writes the
    // resolved cell (outCx,outCz) and returns the owning ownerId (host id 0
    // when unclaimed/unmapped/cellAuth off - authoritySrc's own fail-open).
    // The FUNCTION POINTER itself is 0 when Plugin.cpp has not supplied it
    // (same discipline as pickMintedProxy/connectedPeers) - always check
    // before calling.
    unsigned int (*cellOwnerAt)(float x, float z, int* outCx, int* outCz);
    // connect_relink (Phase 14 plan 01, UI-06): re-runs the SAME in-game panel
    // Connect handler the F2 button runs - coopUiConnect - so a scenario can
    // exercise the NetLink session boundary (stop() + start again on the reused
    // g_net singleton) with NO keyboard, NO mouse and no panel on screen. The
    // scenario layer must not reach into Plugin.cpp or NetLink directly, so
    // Plugin.cpp supplies the adapter (the pickMintedProxy/connectedPeers/
    // cellOwnerAt precedent above). The FUNCTION POINTER itself is 0 when
    // Plugin.cpp has not supplied it - ALWAYS check before calling, same
    // discipline as the three adapters above. Returns false when the plugin
    // declined to issue the relink.
    //
    // THREADING: called from the scenario tick, i.e. the GAME thread - the only
    // thread the panel handler may run on (it touches live game state and joins
    // the net thread inside NetLink::stop()). Never call it from anywhere else.
    bool         (*relinkSession)(void);
};

class Scenario {
public:
    virtual ~Scenario() {}
    virtual const char* name() const = 0;
    // Called EVERY tick between local gameplay start and peer-ready arming.
    // Scenarios that must capture a subject while the freshly-loaded world is
    // still in its baked pose (the craft worker at the prop, the duelists at
    // their spawn) PIN it on the first call and HOLD it on subsequent calls -
    // the arming wait (a join-load, ~10-20 s) is long enough for faction AI to
    // walk an unpinned subject away from where the save baked it. ctx.elapsedMs
    // here is time since gameplay start (NOT the armed scenario clock).
    virtual void onGameplay(const ScenarioContext&) {}
    // Called ONCE when the scenario ARMS: at peer-ready (the first owned-entity
    // batch from the peer - on the host, "the join is loaded + streaming"), or
    // at the arm-timeout fallback. ctx.elapsedMs is measured from THIS moment,
    // so every scripted action happens with the peer watching.
    virtual void onStart(const ScenarioContext& ctx) = 0;
    // Returns true when the scenario is complete (caller logs RESULT then holds).
    virtual bool onTick(const ScenarioContext& ctx) = 0;
    virtual bool passed() const = 0;
};

// Build the scenario named 'name', or 0 if unknown.
Scenario* makeScenario(const std::string& name);

} // namespace coop

#endif // KENSHICOOP_SCENARIO_H
