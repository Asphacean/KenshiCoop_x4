// ChangeGate - the ONE change-gated send/accept policy the periodic state
// channels share (Phase 6).
//
// Almost every sampled channel in ReplicatorChannels.cpp (money, factions,
// doors, build state, build doors, prod, research, ...) reimplements the SAME
// three-part policy by hand, with only its constants and its typed baseline
// differing:
//
//   1. SAMPLE THROTTLE  - only walk the world's rows once per SAMPLE_MS.
//   2. PER-ROW SEND GATE - resend if the value CHANGED vs the last-sent
//      baseline, or if a safety RESEND_MS has elapsed since the last send;
//      an optional MIN_SEND_MS hard-throttles bursts after a send.
//   3. SEQ ACCEPT (apply) - a per-sender monotonic seq drops stale/duplicate
//      rows (resends + echoes) before the receiver touches the engine.
//
// Cloning that policy per channel is where the drift lives: money treats a
// never-sent row as resend-due (it has no silent seed step) while doors/faction
// seed the baseline silently first and only resend rows they have ACTUALLY
// sent. Both fall out of the SAME predicate once `resendUnsent` is a parameter,
// so this header captures the decision in one place and the channels keep only
// their typed baseline + constants.
//
// Pure inline C++03, zero game/logger/wire dependency (matches EngineFaults.h /
// EngineCaps.h) so the unit layer (prototest) locks the policy directly. The
// TYPED baseline compare (money value == , door open/locked, faction |delta| >
// EPS) stays at the call site - only the timing/seq decision lives here.

#ifndef KENSHICOOP_CHANGE_GATE_H
#define KENSHICOOP_CHANGE_GATE_H

// Phase 8 (WORLD-01): gateSeqAcceptPerSender needs a per-owner map for its
// signature. This is the ONLY new include this header ever needed (the
// FoldDedup.h precedent - engine/wire/logger-free stays true, <map> is a
// pure-STL container).
#include <map>

namespace coop {
namespace sync {

// SAMPLE THROTTLE (pass level). True when a fresh world sample is due: the very
// first pass (lastSampleMs == 0) always samples, then at most once per sampleMs.
// The caller stamps lastSampleMs = nowMs when this returns true. Unsigned
// subtraction tolerates the clock's midnight wrap within one sample period.
inline bool gateSampleDue(unsigned long nowMs, unsigned long lastSampleMs,
                          unsigned long sampleMs) {
    return lastSampleMs == 0 || (nowMs - lastSampleMs) >= sampleMs;
}

// PER-ROW SEND GATE. Decide whether a change-gated row should send THIS pass.
//   changed      : sampled value differs from the last-sent baseline (typed
//                  compare owned by the caller).
//   nowMs        : current clock.
//   lastSendMs   : ms of this row's last send; 0 = never sent.
//   minSendMs    : hard throttle after a send (0 = none). A row sent < minSendMs
//                  ago never resends, even on change - burst suppression.
//   resendMs     : periodic safety resend for a row that HAS been sent.
//   resendUnsent : how to treat a never-sent (lastSendMs == 0), unchanged row.
//                  true  = resend-due (money: no silent seed, so stream it once);
//                  false = hold (doors/faction: the baseline was seeded silently,
//                  send only on a real change or a real post-send resend).
// A changed row always sends (subject to the min-send throttle).
inline bool gateShouldSend(bool changed, unsigned long nowMs,
                           unsigned long lastSendMs, unsigned long minSendMs,
                           unsigned long resendMs, bool resendUnsent) {
    if (lastSendMs != 0 && (nowMs - lastSendMs) < minSendMs)
        return false;                       // throttled: sent too recently
    if (changed)
        return true;                        // a real change always crosses
    if (lastSendMs == 0)
        return resendUnsent;                // never-sent, unchanged row
    return (nowMs - lastSendMs) >= resendMs; // periodic safety resend
}

// SEQ ACCEPT (apply level). A per-sender seq is monotonic; accept a row iff it
// is the first ever seen for this key (seqSeen == 0) or strictly newer than the
// last accepted. Drops resends + echoes of already-applied state. The caller
// stamps seqSeen = incomingSeq when this returns true.
inline bool gateSeqAccept(unsigned int seqSeen, unsigned int incomingSeq) {
    return seqSeen == 0 || incomingSeq > seqSeen;
}

// PER-SENDER SEQ ACCEPT (apply level, Phase 8 WORLD-01: the N>=3 fix).
// gateSeqAccept above tracks ONE bare counter across every sender - correct
// only when a receiver ever hears from a single other sender (the two-player
// design target it was built for). It is WRONG for a SYMMETRIC channel where
// more than one client may author the SAME row (a door either P2 or P3 might
// toggle, a build-door either author might touch): at N>=3 a receiver
// hearing the row from two senders would compare the second sender's seq
// against the FIRST sender's counter, silently dropping the second author's
// genuinely-newer fact the moment the first author's counter passes it
// (ChangeGate.h's own pre-Phase-8 comment named this the exact P2/P3/P4-
// collide-on-seq=1 scenario, then mistakenly declared every seq-guarded
// channel structurally immune - true only for hand-partitioned channels,
// false for the symmetric ones; see docs/TWO_PLAYER_ASSUMPTIONS.md finding
// 11's Phase 8 partial overturn). Keying the accept decision on the SENDER
// (ownerId) closes the gap: each sender's monotonic seq is tracked
// independently in the caller's map, so a second author's first-ever row is
// never compared against a first author's counter. Same "first-ever or
// strictly newer" rule as gateSeqAccept, scoped per ownerId. Decision-only,
// matching gateSeqAccept's contract exactly: the caller stamps
// seqSeen[ownerId] = incomingSeq on accept - this function does not mutate
// the map (FoldDedup.h's foldMonotonic is the mutating sibling of this same
// per-owner idiom, for channels that want stamp-on-check-in-one-call
// instead).
inline bool gateSeqAcceptPerSender(const std::map<unsigned int, unsigned int>& seqSeen,
                                    unsigned int ownerId, unsigned int incomingSeq) {
    std::map<unsigned int, unsigned int>::const_iterator it = seqSeen.find(ownerId);
    if (it == seqSeen.end()) return true;         // first sight from this sender
    return incomingSeq > it->second;               // strictly newer than THIS sender's last
}

} // namespace sync
} // namespace coop

#endif // KENSHICOOP_CHANGE_GATE_H
