// SpeedVote - engine-free host-side game-speed min-vote reduce (Phase 9
// Plan 02, CONS-02).
//
// RESEARCH (09-RESEARCH.md "Speed Trace + Vote Design Evidence") found the
// N>=3 defect: the host's speedPeerReq_/speedPeerCombat_ (Replicator.h) were
// bare scalars, last-writer-wins across every join - the documented Phase 4
// deferral, and exactly the FoldDedup.h "one cross-sender scalar" shape, but
// in the VALUE itself this time, not just a seq-guard. A second join's vote
// was silently overwritten by whichever join's REQ the host processed last,
// and the vote never dropped when a join disconnected
// (clearPeerReplicationState left it untouched pre-Phase-9) - a join that
// left while paused left the host paused forever.
//
// This header mirrors FoldDedup.h / ClaimArbiter.h / MoneyFold.h's contract
// exactly: header-only, namespace coop, zero engine/Wire coupling (no
// #include "Wire.h" - the includer already supplies coop::u32, mirroring
// ClaimArbiter.h:14-20), unit-testable from prototest with no GameWorld.
// speedReduce is a PURE function of (hostReq, hostCombat, votes,
// capEnabled) - the SAME inputs always name the SAME effective, which is
// what makes the vote/pause/combat/cap/disconnect matrix exhaustible
// without ENet or the engine. The map itself (speedVotes_, declared by the
// includer) is the host's per-owner state; a disconnect's erase() plus the
// next reduce() call is the WHOLE "instant vote drop" mechanism - no
// dedicated disconnect-branch logic lives in this header at all.
//
// Model, the whole contract in one place:
//   SpeedVoteRec - one connected player's latest vote: the requested
//                 multiplier (0 = pause, so min() gives "either can pause,
//                 both must raise" with no separate flag needed - the
//                 wire's SPEED_PAUSED flag stays only for log clarity), that
//                 player's own-squad combat flag, and the per-SENDER
//                 monotonic seq the caller uses to reject a stale/replayed
//                 REQ before overwriting this record (the foldMonotonic
//                 idiom generalized to "one map entry per sender" instead of
//                 "one high-water scalar per sender in a side map").
//   speedReduce  - eff = min(hostReq, min over every vote's req); combat =
//                 hostCombat OR any vote's combat; if combat && capEnabled
//                 && eff > 1, pin eff to 1 (the cap never force-unpauses -
//                 pause (0) is already below 1, so min semantics preserve
//                 it); paused = eff <= EPS. A pure function - the
//                 ReplicatorChannels.cpp min-reduce block generalized from
//                 ONE bare peer scalar to N independent votes. An EMPTY
//                 votes map is the solo-host passthrough (eff == hostReq, or
//                 1.0 if hostReq is not yet known).

#ifndef KENSHICOOP_SPEED_VOTE_H
#define KENSHICOOP_SPEED_VOTE_H

#include <map>

namespace coop {

// One connected player's latest speed vote (host-only map entry, keyed by
// ownerId in the includer's std::map<u32, SpeedVoteRec>).
struct SpeedVoteRec {
    float req;     // requested multiplier; 0 = pause; -1 = not yet voted
    bool  combat;  // that player's own-squad in-combat flag
    u32   seqSeen; // newest per-SENDER seq accepted into this record (stale guard)
    SpeedVoteRec() : req(-1.0f), combat(false), seqSeen(0) {}
};

// The min-reduce's verdict.
struct SpeedReduceOut {
    float eff;          // arbitrated effective multiplier
    bool  combat;        // combined combat flag (hostCombat OR any vote's combat) -
                          // the caller needs this for the outgoing SET packet's
                          // SPEED_IN_COMBAT flag, not just the cap decision
    bool  paused;         // eff <= EPS
    bool  combatCapped;   // true if the combat cap actually pinned eff to 1x
    SpeedReduceOut() : eff(1.0f), combat(false), paused(false), combatCapped(false) {}
};

// Pure min-vote reduce: eff = min(hostReq, min over votes[i].req) - pause
// (req == 0) is already the strongest possible vote under min(), so no
// separate pause flag is needed in the reduce itself. combat = hostCombat OR
// any vote's combat; if combat && capEnabled && eff > 1, pin eff to 1 (never
// force-unpauses - pause is already below 1). hostReq < 0 (not yet known) is
// treated as "no request yet" -> defaults to 1.0, matching the pre-existing
// ReplicatorChannels.cpp semantics (`(speedMyReq_ >= 0.0f) ? speedMyReq_ :
// 1.0f`). An empty votes map is the solo-host passthrough.
inline SpeedReduceOut speedReduce(float hostReq, bool hostCombat,
                                   const std::map<u32, SpeedVoteRec>& votes,
                                   bool capEnabled) {
    const float EPS = 0.01f;
    SpeedReduceOut out;
    out.eff = (hostReq >= 0.0f) ? hostReq : 1.0f;
    out.combat = hostCombat;
    for (std::map<u32, SpeedVoteRec>::const_iterator it = votes.begin();
         it != votes.end(); ++it) {
        if (it->second.req >= 0.0f && it->second.req < out.eff) out.eff = it->second.req;
        if (it->second.combat) out.combat = true;
    }
    if (out.combat && capEnabled && out.eff > 1.0f) {
        out.eff = 1.0f;
        out.combatCapped = true;
    }
    out.paused = (out.eff <= EPS);
    return out;
}

} // namespace coop

#endif // KENSHICOOP_SPEED_VOTE_H
