// PauseSchedule.h - the PURE cycle schedule behind the `pause_stress` repro
// driver (PauseStressScenario, ScenarioCharState.cpp).
//
// WHY THIS IS A SEPARATE HEADER: the driver's schedule is a pure function of
// elapsed wall-clock milliseconds and the local role - no GameWorld, no engine
// call, no network. Splitting it out lets src/prototest assert the schedule
// deterministically in milliseconds, WITHOUT launching Kenshi. That matters
// because the driver itself can only be validated by a live 2-process rig, and
// that rig is currently unavailable; the schedule is the half that does not
// need one. The scenario calls THESE functions - it does not keep a private
// copy - so the unit checks guard the shipped logic rather than a paraphrase
// of it.
//
// THE GEOMETRY AND WHY IT IS WHAT IT IS:
//   * `firstPauseAtMs` lets both sides finish connecting/settling before the
//     first vote, so the first pause is a steady-state event and not part of
//     the handshake.
//   * `pauseHoldMs` MUST span the field fault window. The two observed faults
//     landed 3.7 s and 7.9 s after the replicated pause, so a hold shorter
//     than ~8 s would resume the world before the window it is meant to sit
//     inside. See pauseHoldSpansFaultWindow() - the guard for that.
//   * `resumeGapMs` is short: it only has to let the world run again between
//     holds, and every second spent running is a second not spent in the
//     window under test.
//
// ROLE ALTERNATION IS THE POINT, not a detail: the fault site reproduced in
// BOTH roles, so a driver that always votes from one side would only ever
// exercise one replication direction (initiator -> observer). Even cycles are
// voted by the host, odd cycles by the join, and the non-voting side only
// observes - so each pause genuinely crosses the wire, in both directions over
// the course of a run.
//
// Header-only, C++03, zero dependencies (not even <windows.h>), so both the
// Harness plugin build and the CRT-only prototest build compile it unchanged.

#ifndef KENSHICOOP_TEST_PAUSESCHEDULE_H
#define KENSHICOOP_TEST_PAUSESCHEDULE_H

namespace coop {

// Cycle geometry, in wall-clock milliseconds since scenario start.
// One cycle = pauseHoldMs paused, then resumeGapMs running.
struct PauseCycle {
    unsigned long firstPauseAtMs;
    unsigned long pauseHoldMs;
    unsigned long resumeGapMs;
};

inline PauseCycle makePauseCycle(unsigned long firstPauseAtMs,
                                 unsigned long pauseHoldMs,
                                 unsigned long resumeGapMs) {
    PauseCycle g;
    g.firstPauseAtMs = firstPauseAtMs;
    g.pauseHoldMs    = pauseHoldMs;
    g.resumeGapMs    = resumeGapMs;
    return g;
}

// The schedule's verdict at one instant.
//   armed      - the first pause time has been reached (before it, nobody votes)
//   cycle      - 0-based cycle index, or -1 while unarmed
//   wantPaused - true inside the hold, false inside the resume gap
//   hostVotes  - which ROLE owns this cycle's vote (false => the join owns it)
struct PausePhase {
    bool armed;
    int  cycle;
    bool wantPaused;
    bool hostVotes;
};

inline PausePhase pausePhaseAt(const PauseCycle& g, unsigned long elapsedMs) {
    PausePhase ph;
    const unsigned long cycleMs = g.pauseHoldMs + g.resumeGapMs;
    // cycleMs == 0 would divide by zero; a degenerate geometry simply never
    // arms rather than faulting the game thread.
    const bool armed = (cycleMs != 0) && (elapsedMs >= g.firstPauseAtMs);
    const unsigned long sinceArm = armed ? (elapsedMs - g.firstPauseAtMs) : 0;
    ph.armed      = armed;
    ph.cycle      = armed ? (int)(sinceArm / cycleMs) : -1;
    ph.wantPaused = armed && ((sinceArm % cycleMs) < g.pauseHoldMs);
    ph.hostVotes  = armed && ((ph.cycle % 2) == 0);
    return ph;
}

// Does THIS process own the vote for this cycle? Exactly one role does, at
// every armed instant - that is the role-alternation invariant.
inline bool pauseVoterIsMe(const PausePhase& ph, bool isHost) {
    return ph.armed && (isHost == ph.hostVotes);
}

// Per-process edge memory: the schedule is a level, the vote is an edge. One
// write per phase transition, never per tick (a per-tick write would spam the
// consensus channel and drown the very traffic the run is trying to read).
struct PauseEdgeState {
    int  lastCycleActed;
    bool lastPhasePaused;
};

inline PauseEdgeState pauseEdgeInit() {
    PauseEdgeState s;
    s.lastCycleActed  = -1;
    s.lastPhasePaused = false;
    return s;
}

inline bool pauseShouldAct(const PausePhase& ph, bool isHost,
                           const PauseEdgeState& s) {
    return pauseVoterIsMe(ph, isHost) &&
           (ph.cycle != s.lastCycleActed || ph.wantPaused != s.lastPhasePaused);
}

inline void pauseNoteActed(const PausePhase& ph, PauseEdgeState& s) {
    s.lastCycleActed  = ph.cycle;
    s.lastPhasePaused = ph.wantPaused;
}

// The hold must still cover the latency between the replicated pause and the
// LATER of the two observed faults, or the driver stops driving the window it
// exists for. Expressed as a predicate so the unit layer can assert it against
// the shipped geometry instead of restating the constant.
inline bool pauseHoldSpansFaultWindow(const PauseCycle& g,
                                      unsigned long faultLatencyMs) {
    return g.pauseHoldMs > faultLatencyMs;
}

// ---- THE SHIPPED GEOMETRY ---------------------------------------------------
// These are the numbers PauseStressScenario actually runs with. They live here
// rather than as class statics in the (TU-private, anonymous-namespace)
// scenario so prototest can assert against the REAL constants instead of a
// restated copy that could silently drift away from them.
//
//   20 s  arm   - both sides connected and settled before the first vote.
//   14 s  hold  - strictly greater than the 7.9 s later-of-the-two field fault
//                 latency, so the hold contains the whole observed window.
//    8 s  gap   - long enough for the world to visibly run between holds.
// 300/292 s     - run length. The join stops first so the host is still live
//                 to log whatever the join's exit does to the session.
inline PauseCycle pauseStressGeometry() {
    return makePauseCycle(20000UL, 14000UL, 8000UL);
}
inline unsigned long pauseStressHostDurationMs() { return 300000UL; }
inline unsigned long pauseStressJoinDurationMs() { return 292000UL; }

} // namespace coop

#endif // KENSHICOOP_TEST_PAUSESCHEDULE_H
