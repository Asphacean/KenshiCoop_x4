// SaveCoord - engine-free host-side per-client coordinated-save state machine
// (Phase 10 Plan 01, SAVE-01).
//
// RESEARCH (10-RESEARCH.md "Save Flow Trace") found the classic singular
// pattern applied to the save plane: the SaveXfer sender is one set of
// file-statics streaming ONE transfer to "the join" (SaveXfer.cpp:155-168,
// no destination PlayerId), and the ACK tracker is `g_lastAckXferId/
// g_lastAckOk` - the LAST ack overwrites every earlier one, and the ACK
// drain DISCARDS the packet's own ownerId (Plugin.cpp:647-657) even though
// SaveAckPacket already carries it (Wire.h). At N=4, three ACKs (or two
// ok=1 and one ok=0) collapse into one scalar, so one client's ok=1
// structurally "commits" the whole group and a failed client is never
// retried, never dropped, and never stops blocking the others. The locked
// fix (10-CONTEXT.md, "Bounded Retries, Then Drop That Client"): a per-
// playerId state machine with bounded retries and a marshaled drop.
//
// This header mirrors ClaimArbiter.h / MoneyFold.h's contract exactly:
// header-only, namespace coop, zero engine/Wire coupling (no #include
// "Wire.h" - the includer already supplies coop::u32), unit-testable from
// prototest with no GameWorld. HOST-only state; the receiver half (join)
// stays singular in SaveXfer.cpp - a join only ever talks to the host,
// which is correct at any N.
//
// State machine, the whole contract in one place:
//   saveBegin      - seeds every CURRENTLY CONNECTED client at SC_STREAMING
//                 for a fresh xferId, called once per coordinated save (the
//                 host's beginSend edge). Replaces any prior per-client
//                 state outright (a fresh xferId supersedes whatever the
//                 group was doing before).
//   saveNoteAck    - the per-owner ACK drain (Plugin.cpp:647-657's
//                 discarded ownerId, now honored): a stale xferId (not the
//                 owner's CURRENT one) is ignored outright - it can never
//                 undo a later state; ok=1 -> SC_COMMITTED; ok=0 ->
//                 SC_FAILED (a retry candidate, decided by the next
//                 saveTick). Never touches an owner absent from the map (an
//                 ACK from an unseeded owner, e.g. a forged/late one,
//                 is a caller-side concern - rejectIfForgedOwner handles it
//                 before this ever sees the ownerId).
//   saveTick       - the per-tick retry/drop decision: a client whose state
//                 is SC_FAILED, OR whose deadline has expired while it is
//                 still neither COMMITTED nor DROPPED, is a candidate.
//                 retries < maxRetries -> outRetry (the caller re-beginSend
//                 UNICAST to that owner; this function bumps retries, resets
//                 the deadline, and returns the client to SC_STREAMING so a
//                 second expiry before the caller's retry lands doesn't
//                 double-count it). retries == maxRetries -> outDrop (the
//                 caller logs + kickPeer; state becomes SC_DROPPED, terminal
//                 - saveTick never re-considers a DROPPED client). A
//                 COMMITTED client is never touched again either.
//   saveSettled    - true once EVERY tracked client is SC_COMMITTED or
//                 SC_DROPPED (vacuously true for an empty map - a
//                 coordinated save with no connected client to track). This
//                 predicate IS the two SAVE-01 invariants: "one client's ACK
//                 never implies the group committed" (every OTHER client
//                 must independently reach a terminal state too) and "one
//                 client's failure never blocks the others forever" (that
//                 client terminates via DROPPED, at which point it can no
//                 longer prevent settlement).
//   saveEraseOwner - disconnect/reconnect cleanup (the claimEraseClaimant /
//                 moneyEraseOwner shape applied to this per-client map): a
//                 departed client's entry can never again block saveSettled
//                 for the survivors. Returns the erased count (0 or 1) for
//                 the caller's own greppable log line.

#ifndef KENSHICOOP_SAVE_COORD_H
#define KENSHICOOP_SAVE_COORD_H

#include <map>
#include <set>
#include <vector>

namespace coop {

// Per-client coordinated-save progress. `xferId` is the transfer THIS client
// is currently tracked against: saveBegin's fresh id for a first seed, or -
// after a per-owner retry - the retry stream's own id. SaveXfer::beginSend
// mints a NEW xferId on EVERY call (++g_sendXferId, including a retry
// unicast), so the caller MUST retag this client via saveRetagRetryId after
// each retry beginSend, else the client's ACK for the retry is discarded as
// stale by saveNoteAck's xferId check (phase 10 review CR-01). SaveXfer stays
// one serialized sender machine either way.
enum SaveClientState {
    SC_STREAMING,  // BEGIN queued (broadcast or this client's own retry unicast); no ACK yet
    SC_AWAIT_ACK,  // reserved for a future finer-grained sender-side signal; unused by saveTick today
    SC_COMMITTED,  // this client ACKed ok=1 - terminal
    SC_FAILED,     // this client ACKed ok=0 - a retry candidate on the next saveTick
    SC_DROPPED     // retries exhausted; kickPeer issued - terminal
};

struct SaveClient {
    u32           xferId;
    int           state;
    unsigned int  retries;
    unsigned long deadlineMs;
};

struct SaveCoordState {
    std::map<u32, SaveClient> clients;
    u32                       xferId; // the group's current transfer id (saveBegin's stamp)
    bool                      active; // true once saveBegin has ever run
    SaveCoordState() : xferId(0), active(false) {}
};

// Seed every id in `connected` at SC_STREAMING for a fresh coordinated save.
// Replaces any prior per-client state outright - a fresh xferId supersedes
// whatever the group was doing before (mirrors SaveXfer's own "one transfer
// at a time; a re-begin abandons the previous" contract, applied per-client).
inline void saveBegin(SaveCoordState& st, u32 xferId, const std::set<u32>& connected,
                       unsigned long nowMs, unsigned long ackTimeoutMs) {
    st.clients.clear();
    st.xferId = xferId;
    st.active = true;
    for (std::set<u32>::const_iterator it = connected.begin(); it != connected.end(); ++it) {
        SaveClient c;
        c.xferId     = xferId;
        c.state      = SC_STREAMING;
        c.retries    = 0;
        c.deadlineMs = nowMs + ackTimeoutMs;
        st.clients[*it] = c;
    }
}

// The per-owner ACK drain: a stale xferId (not this owner's CURRENT one) is
// ignored outright - it can never retroactively undo a later state (commit-
// final in spirit, applied per-client rather than per-identity). Returns the
// resulting state (the caller's own log line reads this), or -1 if `owner`
// is not currently tracked (an ACK from an owner saveBegin never seeded, or
// one already erased by saveEraseOwner - never fabricates a new entry here).
inline int saveNoteAck(SaveCoordState& st, u32 owner, u32 xferId, bool ok, unsigned long /*nowMs*/) {
    std::map<u32, SaveClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return -1;
    if (xferId != it->second.xferId) return it->second.state; // stale: ignored, state unchanged
    it->second.state = ok ? SC_COMMITTED : SC_FAILED;
    return it->second.state;
}

// Retag ONE client's tracked xferId after a caller-issued retry beginSend
// (phase 10 review CR-01, the loadRetagRetryId precedent applied to the save
// plane): SaveXfer::beginSend unconditionally mints a fresh xferId, so the
// retried client's next ACK carries THAT id - without the retag, saveNoteAck
// judges the retry's own ACK stale against the original group id and a fully
// successful retry still ends in a bounded-retries kick. Also resets the
// deadline (nowMs + timeoutMs) so the caller can size-scale the retry stream's
// deadline the same way the initial broadcast's saveBegin deadline was.
// No-op if `owner` is not tracked.
inline void saveRetagRetryId(SaveCoordState& st, u32 owner, u32 newXferId,
                             unsigned long nowMs, unsigned long timeoutMs) {
    std::map<u32, SaveClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return;
    it->second.xferId     = newXferId;
    it->second.deadlineMs = nowMs + timeoutMs;
}

// Refund a retry saveTick just charged (phase 10 review WR-02): SaveXfer is
// ONE serialized sender, so when saveTick flags SEVERAL owners in the same
// tick (common-mode expiry) the caller can only actually start ONE retry
// stream - each later beginSend would tear down the earlier one mid-flight.
// The deferred owners must not burn a retry they never got a stream for:
// un-bump the counter and expire the deadline immediately (nowMs), so the
// next saveTick the caller runs with an idle sender re-flags this owner and
// re-charges the retry then. State stays SC_STREAMING (non-terminal).
// No-op if `owner` is not tracked.
inline void saveDeferRetry(SaveCoordState& st, u32 owner, unsigned long nowMs) {
    std::map<u32, SaveClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return;
    if (it->second.retries > 0) --it->second.retries;
    it->second.deadlineMs = nowMs;
}

// Per-tick retry/drop decision. A client is a candidate when it is SC_FAILED,
// OR its deadline has expired while it is neither COMMITTED nor DROPPED
// (silence past the timeout is treated exactly like an explicit failure).
// retries < maxRetries -> outRetry (caller re-beginSend UNICAST to that
// owner); this function itself bumps retries, resets the deadline, and
// returns the client to SC_STREAMING so it is not re-flagged before the
// caller's retry has a chance to land. retries == maxRetries -> outDrop
// (caller logs + kickPeer); state becomes SC_DROPPED, terminal.
inline void saveTick(SaveCoordState& st, unsigned long nowMs, unsigned int maxRetries,
                      unsigned long retryTimeoutMs,
                      std::vector<u32>& outRetry, std::vector<u32>& outDrop) {
    for (std::map<u32, SaveClient>::iterator it = st.clients.begin();
         it != st.clients.end(); ++it) {
        SaveClient& c = it->second;
        if (c.state == SC_COMMITTED || c.state == SC_DROPPED) continue;
        bool expired   = nowMs >= c.deadlineMs; // GetTickCount-scale; wraparound not a concern in practice
        bool candidate = (c.state == SC_FAILED) || expired;
        if (!candidate) continue;
        if (c.retries < maxRetries) {
            outRetry.push_back(it->first);
            ++c.retries;
            c.deadlineMs = nowMs + retryTimeoutMs;
            c.state      = SC_STREAMING;
        } else {
            outDrop.push_back(it->first);
            c.state = SC_DROPPED;
        }
    }
}

// True once EVERY tracked client is SC_COMMITTED or SC_DROPPED (vacuously
// true for an empty map). This IS the SAVE-01 contract: one client's ACK
// never implies the group committed (every other client must independently
// terminate too), and one client's failure never blocks the others forever
// (it terminates via DROPPED and stops counting against settlement).
inline bool saveSettled(const SaveCoordState& st) {
    for (std::map<u32, SaveClient>::const_iterator it = st.clients.begin();
         it != st.clients.end(); ++it) {
        if (it->second.state != SC_COMMITTED && it->second.state != SC_DROPPED) return false;
    }
    return true;
}

// Disconnect/reconnect cleanup (the claimEraseClaimant/moneyEraseOwner
// shape): a departed client's entry can never again block saveSettled for
// the survivors. Returns the erased count (0 or 1) for the caller's own
// greppable log line.
inline unsigned int saveEraseOwner(SaveCoordState& st, u32 owner) {
    return (unsigned int)st.clients.erase(owner);
}

} // namespace coop

#endif // KENSHICOOP_SAVE_COORD_H
