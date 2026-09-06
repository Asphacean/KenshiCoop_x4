// LoadCoord - engine-free host-side per-client coordinated-load state machine
// + the first-wins save/load arbiter (Phase 10 Plan 01, SAVE-02/SAVE-03).
//
// RESEARCH (10-RESEARCH.md "Load Flow Trace") found NO positive load ACK on
// the wire - only LOAD_NACK (Wire.h) - so the host never learned when a join
// finished (or failed) a coordinated load, and a join stuck in the NACK-flow
// whose post-transfer commit failed "re-based and kept waiting" FOREVER
// (Plugin.cpp:866-886). It also found concurrent save/load requests silently
// last-wins at every layer: two REQs each re-issue saveGameAs/loadSave with
// no arbitration, no rejection, and no per-player request-id observable
// verdict (10-RESEARCH.md "one authoritative load transition" section). The
// locked fixes (10-CONTEXT.md): a per-client load state machine mirroring
// SaveCoord.h (SAVE-02), completed by the new PKT_LOAD_ACK; and a shared
// first-wins CoordArbiter serializing BOTH the save and load planes, with an
// observable PKT_COORD_REJECT (SAVE-03).
//
// This header mirrors SaveCoord.h/ClaimArbiter.h/MoneyFold.h's contract:
// header-only, namespace coop, zero engine/Wire coupling (no #include
// "Wire.h"), unit-testable from prototest with no GameWorld. HOST-only
// state; the join side stays singular (a join only ever talks to the host).
//
// LoadCoord state machine (mirrors SaveCoord.h exactly, per-client):
//   loadBegin      - seeds every CURRENTLY CONNECTED client at LC_GO_SENT
//                 for a fresh loadId, called on the host's LOAD_GO edge.
//   loadNoteNack   - a join's LOAD_NACK: stale loadId (not this client's
//                 CURRENT one) ignored; else -> LC_NACKED (the fallback
//                 transfer is expected to start for this client next).
//   loadNoteXferStart - the fallback transfer actually started for this
//                 client -> LC_XFER (mirrors the SaveCoord xferId handoff;
//                 kept as an explicit transition so a caller can observe
//                 "NACKed but transfer not yet started" separately from
//                 "streaming").
//   loadNoteAck    - the per-owner PKT_LOAD_ACK drain (SAVE-02's missing
//                 half): stale loadId ignored; ok=1 -> LC_LOADED (terminal);
//                 ok=0 -> LC_FAILED (a retry candidate on the next loadTick).
//   loadTick       - the same retry/drop decision shape as SaveCoord::
//                 saveTick: LC_FAILED or deadline-expired-while-non-terminal
//                 -> retries<max -> outRetry (caller re-issues LOAD_GO to
//                 that owner), else -> outDrop (caller logs + kickPeer).
//   loadSettled    - true once EVERY tracked client is LC_LOADED or
//                 LC_DROPPED (vacuously true for an empty map) - the same
//                 "one client never blocks the others" / "one ACK never
//                 implies the group loaded" pair SaveCoord::saveSettled
//                 establishes for the save plane.
//   loadEraseOwner - disconnect/reconnect cleanup, the saveEraseOwner shape.
//
// CoordArbiter (SAVE-03, shared by both planes - ONE active transition at a
// time across save AND load, not one arbiter each):
//   coordOffer     - idle -> COORD_ACCEPT (this offer becomes the active
//                 transition; a host-local edge offers with requesterId=0).
//                 busy AND (kind==COORD_LOAD && requesterId==0) -> a HOST
//                 load preempts an in-flight save (the existing abortAll
//                 rule, Plugin.cpp:691) -> COORD_ACCEPT (the record becomes
//                 the new active transition). busy otherwise ->
//                 COORD_REJECT_BUSY (the caller reads the arbiter's CURRENT
//                 fields - captured by the caller BEFORE calling coordOffer,
//                 since a preempting accept overwrites them - as the active
//                 transition's ids for the PKT_COORD_REJECT payload).
//   coordComplete  - fires on saveSettled/loadSettled (or an abandon edge);
//                 frees the arbiter (busy=false) for the next drained offer.
//
// First-wins tie rule: "first" = the first REQ the host DRAINS in one tick
// (deque order = arrival order on the ordered CH_BULK link). Cross-client
// ordering WITHIN one tick is therefore arrival-order-deterministic, not an
// arbitrary race - the same sequence of arrivals always produces the same
// accept/reject outcome.

#ifndef KENSHICOOP_LOAD_COORD_H
#define KENSHICOOP_LOAD_COORD_H

#include <map>
#include <set>
#include <vector>

namespace coop {

enum LoadClientState {
    LC_GO_SENT, // LOAD_GO queued (broadcast or this client's own retry); no NACK/ACK yet
    LC_NACKED,  // this client NACKed (copy missing/diverged); fallback transfer expected
    LC_XFER,    // the fallback transfer has started streaming to this client
    LC_LOADED,  // this client ACKed ok=1 (loaded + live) - terminal
    LC_FAILED,  // this client ACKed ok=0 - a retry candidate on the next loadTick
    LC_DROPPED  // retries exhausted; kickPeer issued - terminal
};

struct LoadClient {
    u32           loadId;
    int           state;
    unsigned int  retries;
    unsigned long deadlineMs;
};

struct LoadCoordState {
    std::map<u32, LoadClient> clients;
    u32                       loadId;
    bool                      active;
    LoadCoordState() : loadId(0), active(false) {}
};

// Seed every id in `connected` at LC_GO_SENT for a fresh coordinated load.
// Replaces any prior per-client state outright, mirroring SaveCoord::saveBegin.
inline void loadBegin(LoadCoordState& st, u32 loadId, const std::set<u32>& connected,
                       unsigned long nowMs, unsigned long ackTimeoutMs) {
    st.clients.clear();
    st.loadId = loadId;
    st.active = true;
    for (std::set<u32>::const_iterator it = connected.begin(); it != connected.end(); ++it) {
        LoadClient c;
        c.loadId     = loadId;
        c.state      = LC_GO_SENT;
        c.retries    = 0;
        c.deadlineMs = nowMs + ackTimeoutMs;
        st.clients[*it] = c;
    }
}

// A join's LOAD_NACK: stale loadId (not this client's CURRENT one) ignored;
// else -> LC_NACKED. Returns the resulting state, or -1 if `owner` is not
// currently tracked.
// Phase 10 review CR-03: a NACK is PROGRESS, not silence - the client is
// alive and now waiting on the host's own reload + the fallback transfer +
// its own reload, which together can far exceed the GO-issue deadline. When
// extendMs > 0 the accepted NACK re-arms the deadline (nowMs + extendMs) so
// a healthy NACK-flow is never expired mid-flight, double-reloaded, and
// kicked by the fixed floor alone.
inline int loadNoteNack(LoadCoordState& st, u32 owner, u32 loadId,
                        unsigned long nowMs = 0, unsigned long extendMs = 0) {
    std::map<u32, LoadClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return -1;
    if (loadId != it->second.loadId) return it->second.state; // stale: ignored
    it->second.state = LC_NACKED;
    if (extendMs > 0) it->second.deadlineMs = nowMs + extendMs; // CR-03: progress re-arms
    return it->second.state;
}

// The fallback transfer actually started streaming to `owner` -> LC_XFER.
// No-op (returns -1) if `owner` is not currently tracked.
// Phase 10 review CR-03: the transfer start is the second progress edge -
// when extendMs > 0 the deadline is re-armed (nowMs + extendMs); the caller
// size-scales extendMs from the transfer's own byte count (the save plane's
// totalBytes / 2.5MBps * 3 shape) floored at the load-plane timeout so a
// large-but-healthy stream plus the join's reload is never mistaken for a
// dead client.
inline int loadNoteXferStart(LoadCoordState& st, u32 owner,
                             unsigned long nowMs = 0, unsigned long extendMs = 0) {
    std::map<u32, LoadClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return -1;
    it->second.state = LC_XFER;
    if (extendMs > 0) it->second.deadlineMs = nowMs + extendMs; // CR-03: progress re-arms
    return it->second.state;
}

// The per-owner PKT_LOAD_ACK drain (SAVE-02's missing half): stale loadId
// ignored; ok=1 -> LC_LOADED (terminal); ok=0 -> LC_FAILED (a retry
// candidate). Returns the resulting state, or -1 if `owner` is not tracked.
inline int loadNoteAck(LoadCoordState& st, u32 owner, u32 loadId, bool ok, unsigned long /*nowMs*/) {
    std::map<u32, LoadClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return -1;
    if (loadId != it->second.loadId) return it->second.state; // stale: ignored
    it->second.state = ok ? LC_LOADED : LC_FAILED;
    return it->second.state;
}

// Retag ONE client's tracked loadId after a caller-minted retry GO. Unlike
// SAVE's xferId (SaveXfer's receiver re-stages unconditionally on any BEGIN,
// so a retry may safely reuse the same xferId), the join's own LOAD_GO
// handling drops any loadId <= the newest one it has already seen
// (Plugin.cpp's `it->pkt.loadId <= g_loadIdSeen` stale-GO guard) - a retry
// MUST carry a fresh, higher loadId to ever reach the join at all. The
// caller mints that fresh id (its own monotonic per-host counter) and calls
// this to keep loadNoteNack/loadNoteAck's staleness check aligned with it.
// No-op if `owner` is not tracked.
inline void loadRetagRetryId(LoadCoordState& st, u32 owner, u32 newLoadId) {
    std::map<u32, LoadClient>::iterator it = st.clients.find(owner);
    if (it == st.clients.end()) return;
    it->second.loadId = newLoadId;
}

// Per-tick retry/drop decision, the SaveCoord::saveTick shape applied to the
// load plane. A candidate (LC_FAILED, or deadline-expired while non-
// terminal) with retries<maxRetries -> outRetry (caller re-issues LOAD_GO to
// that owner; this function bumps retries/resets the deadline/returns the
// client to LC_GO_SENT); retries==maxRetries -> outDrop (caller logs +
// kickPeer; state -> LC_DROPPED, terminal). The caller MUST also call
// loadRetagRetryId with the fresh loadId it mints for each outRetry entry
// (see that function's doc comment for why).
inline void loadTick(LoadCoordState& st, unsigned long nowMs, unsigned int maxRetries,
                      unsigned long retryTimeoutMs,
                      std::vector<u32>& outRetry, std::vector<u32>& outDrop) {
    for (std::map<u32, LoadClient>::iterator it = st.clients.begin();
         it != st.clients.end(); ++it) {
        LoadClient& c = it->second;
        if (c.state == LC_LOADED || c.state == LC_DROPPED) continue;
        bool expired   = nowMs >= c.deadlineMs;
        bool candidate = (c.state == LC_FAILED) || expired;
        if (!candidate) continue;
        if (c.retries < maxRetries) {
            outRetry.push_back(it->first);
            ++c.retries;
            c.deadlineMs = nowMs + retryTimeoutMs;
            c.state      = LC_GO_SENT;
        } else {
            outDrop.push_back(it->first);
            c.state = LC_DROPPED;
        }
    }
}

// True once EVERY tracked client is LC_LOADED or LC_DROPPED (vacuously true
// for an empty map) - the SAVE-02 mirror of SaveCoord::saveSettled.
inline bool loadSettled(const LoadCoordState& st) {
    for (std::map<u32, LoadClient>::const_iterator it = st.clients.begin();
         it != st.clients.end(); ++it) {
        if (it->second.state != LC_LOADED && it->second.state != LC_DROPPED) return false;
    }
    return true;
}

// Disconnect/reconnect cleanup (the SaveCoord::saveEraseOwner shape).
inline unsigned int loadEraseOwner(LoadCoordState& st, u32 owner) {
    return (unsigned int)st.clients.erase(owner);
}

// ---- CoordArbiter (SAVE-03): first-wins serialization shared by BOTH the
// save and load planes - exactly ONE active transition at a time, never one
// arbiter per plane. -----------------------------------------------------

enum CoordKind {
    COORD_SAVE = 0,
    COORD_LOAD = 1
};

enum CoordResult {
    COORD_ACCEPT      = 0,
    COORD_REJECT_BUSY = 1
};

struct CoordArbiter {
    bool busy;
    int  kind;
    u32  requesterId; // 0 = a host-local edge (menu/quicksave/autosave/connect-push)
    u32  reqId;
    CoordArbiter() : busy(false), kind(COORD_SAVE), requesterId(0), reqId(0) {}
};

// Idle -> COORD_ACCEPT (this offer becomes the active transition). Busy AND
// this is the SAME transition re-offering itself (kind/requesterId/reqId all
// match the active record) -> COORD_ACCEPT, a no-op (Phase 10 Plan 01: a
// REQ-admitted save/load fires the SAME (requester,reqId) again at its own
// detour-driven local edge a tick later - re-checking must never self-reject
// the transition it just admitted). Busy AND (kind==COORD_LOAD &&
// requesterId==0) -> a HOST-issued load preempts an in-flight save/load (the
// existing abortAll rule) -> COORD_ACCEPT, and the record becomes the NEW
// active transition (overwriting the preempted one - callers needing the
// preempted transition's ids for a log line must read them BEFORE calling
// coordOffer). Busy otherwise -> COORD_REJECT_BUSY, leaving the arbiter's
// fields untouched at the CURRENTLY active transition (the caller reads
// arb.requesterId/arb.reqId AFTER the call for the PKT_COORD_REJECT
// payload's active* fields).
inline int coordOffer(CoordArbiter& arb, int kind, u32 requesterId, u32 reqId) {
    if (!arb.busy) {
        arb.busy = true; arb.kind = kind; arb.requesterId = requesterId; arb.reqId = reqId;
        return COORD_ACCEPT;
    }
    if (arb.kind == kind && arb.requesterId == requesterId && arb.reqId == reqId) {
        return COORD_ACCEPT; // idempotent re-offer of the already-active transition
    }
    if (kind == COORD_LOAD && requesterId == 0) {
        // Host-issued load preempts an active save/load transition.
        arb.kind = kind; arb.requesterId = requesterId; arb.reqId = reqId;
        return COORD_ACCEPT;
    }
    return COORD_REJECT_BUSY;
}

// Frees the arbiter for the next drained offer. Called on saveSettled/
// loadSettled, or an abandon edge (the active transition's holder
// disconnected mid-transfer).
inline void coordComplete(CoordArbiter& arb) {
    arb.busy = false;
}

} // namespace coop

#endif // KENSHICOOP_LOAD_COORD_H
