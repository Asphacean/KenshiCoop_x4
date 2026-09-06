// XferCommit - engine-free host-side pending-transfer state machine
// (Phase 7, INV-02/INV-04).
//
// RESEARCH (07-RESEARCH.md "Two-Phase Hook Design") found the load-bearing
// gap: PKT_INV_XFER never reaches a join<->join destination owner at N>=3
// (NetLink.cpp's PKT_INV_XFER receive branch only pushed to the host's own
// Inbound - no relay, no forge check, not even the FAILSAFE log), and the
// transfer verdict was settled by whichever receiver's ACK answered FIRST
// (applyXferAcks, ReplicatorItems.cpp) rather than by the host. The locked
// fix (07-CONTEXT.md, "Host-Committed Two-Phase") re-points the DECISION:
// the intent becomes a host-terminated request, the host arbitrates via the
// state machine this header captures, and every client applies ONLY the
// host's single broadcast commit.
//
// This header mirrors FoldDedup.h / PinOwner.h's precedent exactly: header-
// only, namespace coop, zero engine/Wire coupling (no <windows.h>, no
// GameWorld/Character*, no #include "Wire.h" - the includer already supplies
// coop::u32), unit-testable from prototest with no GameWorld. The pending-
// transfer table is host-only state, keyed (authorId, transferId) - the same
// composite-key idiom Phase 5's FoldDedup.h standardized on, chosen so N
// different authors' transferId=1 (each per-sender-monotonic) never collide.
//
// State machine (per (authorId, transferId) key):
//   (absent)  -- xferBegin -->      PENDING
//   PENDING   -- xferCommitOnce --> COMMITTED
//   COMMITTED -- xferCommitOnce --> (no-op, returns false: "commit is final")
// A duplicate/replayed intent (xferBegin on an already-PENDING or already-
// COMMITTED key) is a no-op - idempotent against a reliable-channel resend.
// A commit-then-late-intent (xferBegin after COMMITTED) is also a no-op: the
// entry already reflects the final state, so replaying the intent must never
// reopen it.
//
// xferEraseOwner is the disconnect-cleanup primitive (INV-04, "host-final-
// else-rollback, never both, never neither"): a departing owner's PENDING
// entries are VOIDED (erased - the transfer never happened, so nobody needs
// to roll anything back, because nobody ever committed it), while COMMITTED
// entries STAND (left untouched - the destination already has the item on
// every survivor). Mirrors PinOwner.h's pinEraseOwner shape: erase-by-owner,
// return the count, let the caller emit its own greppable log line.

#ifndef KENSHICOOP_XFER_COMMIT_H
#define KENSHICOOP_XFER_COMMIT_H

#include <map>
#include <utility>

namespace coop {

enum XferPendingState {
    XFER_PENDING_OPEN      = 0, // intent seen, no commit yet
    XFER_PENDING_COMMITTED = 1  // host has committed (or rejected-and-closed) this transfer
};

// One pending-transfer table entry. `seq` is the caller-supplied monotonic
// INSERTION order (Phase 7 review WR-05): map iteration order is by
// (authorId, transferId), which is NOT insertion order across authors -
// author 1's brand-new keys sort before author 3's ancient ones, so the old
// erase-begin() cap eviction could evict a COMMITTED "commit is final"
// marker (letting a replayed intent re-open and double-broadcast a transfer)
// or even the key inserted THIS very call. Eviction now removes the entry
// with the smallest seq - the genuinely oldest-inserted one - which by
// construction is never the just-inserted key (its seq is the maximum).
struct XferPendingEntry {
    u8  state; // XferPendingState
    u32 seq;   // monotonic insertion order (caller-owned counter)
};

// Record a fresh intent as PENDING. Returns true only the FIRST time this
// (authorId, transferId) key is seen (a fresh open); returns false on any
// replay - whether the key is already PENDING (duplicate/resent intent,
// idempotent no-op) or already COMMITTED (a late intent arriving after the
// commit already closed the transfer - the entry's final state must never
// reopen). The caller (host's processXferIntents) only proceeds to arbitrate
// when this returns true. `seqCounter` is the caller-owned monotonic
// insertion counter (see XferPendingEntry); it is consumed (post-incremented)
// only on a fresh insert.
inline bool xferBegin(std::map<std::pair<u32, u32>, XferPendingEntry>& pending,
                       u32 authorId, u32 transferId, u32& seqCounter) {
    std::pair<u32, u32> key(authorId, transferId);
    std::map<std::pair<u32, u32>, XferPendingEntry>::iterator it = pending.find(key);
    if (it != pending.end()) return false; // already PENDING or already COMMITTED
    XferPendingEntry e;
    e.state = (u8)XFER_PENDING_OPEN;
    e.seq   = seqCounter++;
    pending.insert(std::make_pair(key, e));
    return true;
}

// Transition PENDING -> COMMITTED exactly once. Returns true only on the
// transfer's FIRST commit (the host should author and broadcast the
// XferCommitPacket in that case); returns false when the key is missing
// entirely (xferBegin was never called - a commit attempt with no matching
// intent) or already COMMITTED (a second commit attempt on an already-final
// transfer - "commit is final", never re-arbitrated, never re-broadcast).
inline bool xferCommitOnce(std::map<std::pair<u32, u32>, XferPendingEntry>& pending,
                            u32 authorId, u32 transferId) {
    std::pair<u32, u32> key(authorId, transferId);
    std::map<std::pair<u32, u32>, XferPendingEntry>::iterator it = pending.find(key);
    if (it == pending.end()) return false;               // no matching intent
    if (it->second.state == (u8)XFER_PENDING_COMMITTED) return false; // already final
    it->second.state = (u8)XFER_PENDING_COMMITTED;
    return true;
}

// Disconnect-cleanup primitive (INV-04): erase every PENDING entry authored
// by `owner` (voided - the transfer never happened, item stays with source),
// leaving every COMMITTED entry authored by `owner` untouched (stands - the
// destination already has the item on every survivor). *outVoided and
// *outStood (if non-null) receive the respective counts for the caller's own
// greppable log line (mirrors clearPeerReplicationState's `[leave] cleared
// pins=...` shape). Returns the total PENDING entries erased (== *outVoided).
inline unsigned int xferEraseOwner(std::map<std::pair<u32, u32>, XferPendingEntry>& pending,
                                    u32 owner,
                                    unsigned int* outVoided = 0,
                                    unsigned int* outStood = 0) {
    unsigned int voided = 0;
    unsigned int stood  = 0;
    for (std::map<std::pair<u32, u32>, XferPendingEntry>::iterator it = pending.begin();
         it != pending.end(); ) {
        if (it->first.first != owner) { ++it; continue; }
        if (it->second.state == (u8)XFER_PENDING_COMMITTED) {
            ++stood;
            ++it;
        } else {
            pending.erase(it++);
            ++voided;
        }
    }
    if (outVoided) *outVoided = voided;
    if (outStood)  *outStood  = stood;
    return voided;
}

// Rejoin-cleanup primitive (Phase 7 review CR-01): erase EVERY entry authored
// by `owner`, regardless of state - called on the CONNECT edge when a
// PlayerId slot is (re)assigned. A fresh client process restarts its
// per-sender transferId counter at 1, so any record kept from that slot's
// PREVIOUS connection collides with the new connection's ids: a stale
// COMMITTED entry makes xferBegin return false for the rejoined author's new
// transferId - the intent is silently dropped, no commit and no reject is
// ever broadcast, and transfers stay broken until the counter passes its old
// high-water mark. Distinct from xferEraseOwner (the DISCONNECT primitive,
// which keeps COMMITTED entries standing so late duplicates of the original
// connection's intents still answer "commit is final"): purging only here,
// at reassignment time, preserves host-final-else-rollback for the original
// transfer while never poisoning a rejoin. Erasing a COMMITTED record never
// mutates items - the commit was already applied on every survivor; only the
// replay-dedup memory is forgotten, and the connection that could have
// replayed it is dead. Returns the number of entries erased.
inline unsigned int xferPurgeAuthor(std::map<std::pair<u32, u32>, XferPendingEntry>& pending,
                                     u32 owner) {
    unsigned int erased = 0;
    for (std::map<std::pair<u32, u32>, XferPendingEntry>::iterator it = pending.begin();
         it != pending.end(); ) {
        if (it->first.first == owner) { pending.erase(it++); ++erased; }
        else ++it;
    }
    return erased;
}

// Cap-evict the OLDEST-INSERTED entry (smallest seq) once the table exceeds
// `cap` (Phase 7 review WR-05: erase-begin() evicted the lowest authorId's
// smallest transferId - NOT the oldest entry across authors - which could
// discard a COMMITTED "commit is final" marker while a younger author's
// re-openable state survived, or evict the key inserted this very call).
// The linear min-seq scan only runs once the table is already over `cap`
// (4096 in practice - a long-session bound, not a hot path). The caller
// invokes this after the commit transition, so an in-flight OPEN key is
// never the eviction candidate anyway (and its max seq protects it besides).
inline void xferEvictOldest(std::map<std::pair<u32, u32>, XferPendingEntry>& pending,
                             std::size_t cap) {
    while (pending.size() > cap) {
        std::map<std::pair<u32, u32>, XferPendingEntry>::iterator oldest = pending.begin();
        for (std::map<std::pair<u32, u32>, XferPendingEntry>::iterator it = pending.begin();
             it != pending.end(); ++it) {
            if (it->second.seq < oldest->second.seq) oldest = it;
        }
        pending.erase(oldest);
    }
}

} // namespace coop

#endif // KENSHICOOP_XFER_COMMIT_H
