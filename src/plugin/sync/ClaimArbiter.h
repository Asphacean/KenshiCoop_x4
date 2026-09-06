// ClaimArbiter - engine-free host-side world-item claim-contention arbiter
// (Phase 7 Plan 02, INV-03).
//
// RESEARCH (07-RESEARCH.md "World-Item Pickup Race Analysis") found the
// duplicate: two players picking up the same world item at (nearly) the same
// moment each keep a copy today, even at 2 players - the author's
// applyWorldClaims finds "no track; already gone" and silently no-ops
// (ReplicatorItems.cpp, pre-fix). The locked fix (07-CONTEXT.md, "Timestamp-
// Based Ordering") re-points PKT_WORLD_ITEM_CLAIM from a notice the author
// acts on unconditionally into a host-terminated INTENT: the host collects
// every claim for one item identity within a bounded contention window and
// arbitrates the single winner this header captures.
//
// This header mirrors FoldDedup.h / XferCommit.h's precedent exactly: header-
// only, namespace coop, zero engine/Wire coupling (no #include "Wire.h" - the
// includer already supplies coop::u32), unit-testable from prototest with no
// GameWorld. The contention-window table is host-only state, keyed
// (authorId, netId) - the item's shared identity (the SAME key W1 claims
// already use), so N different authors' netId=1 (each per-sender-scoped)
// never collide (the Phase 5 composite-key rule).
//
// Arbitration model (per (authorId, netId) key), the whole contract in one
// place:
//   openClaim   - opens a fresh window on the FIRST claim seen for a key
//                 (openMs = the host's nowMs()), or appends/updates the
//                 claimant's entry into an ALREADY-open window. Idempotent
//                 per claimant: a replayed/resent claim from the SAME
//                 claimantId updates its entry in place rather than
//                 appending a second competing row.
//   addClaim    - the lower-level append-only primitive openClaim calls
//                 once the window is known to exist; also directly usable
//                 by a caller that already holds the window (host loop).
//   claimWindowExpired - true once the window has been open >= its
//                 configured length (the caller finalizes it that tick).
//   finalizeClaim - the deterministic winner: earliest mapped stamp among
//                 MAPPABLE claims; every claim within `epsMs` of that
//                 earliest stamp, or with no mappable stamp at all, is in
//                 the tie band, and the winner within the tie band is the
//                 LOWEST claimantId. A pure function of (stamps, epsMs,
//                 claimantIds) - the same window replayed with the same
//                 inputs always names the same winner, which is what makes
//                 this unit-testable and reproducible across reruns.
//   claimCommitted / markClaimCommitted - "commit is final" (INV-03): once
//                 a (authorId, netId) key is marked committed, a caller must
//                 never re-open or re-finalize a window for it - a later-
//                 arriving earlier-stamped claim for an already-committed
//                 identity is answered with a targeted reject, never a
//                 retroactive re-award.
//   claimEraseClaimant - disconnect cleanup (INV-04 shape applied to
//                 claims): erases every entry authored by one claimantId
//                 from every OPEN window, so a departed player's bid can
//                 never win a contention it no longer participates in.
//                 Windows left with zero claims are erased outright (mirrors
//                 xferEraseOwner's erase-by-owner shape).
//   claimEvictOldest - defensive cap-eviction, mirroring FoldDedup.h's
//                 foldOnce/XferCommit.h's xferEvictOldest idiom. In practice
//                 claimWindows_ stays small (the host erases a window the
//                 instant it finalizes), so this is a bound, not a hot path.

#ifndef KENSHICOOP_CLAIM_ARBITER_H
#define KENSHICOOP_CLAIM_ARBITER_H

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace coop {

// One competing claim inside an open contention window.
struct ClaimEntry {
    u32           claimantId;
    unsigned long mappedMs; // hostLocalEst = authorClaimMs + peerClock_[claimant].offsetMs,
                            // clamped <= now; meaningless when !mappable
    bool          mappable; // false when the claimant has no peerClock_ entry yet
};

// One item's open contention window, keyed (authorId, netId) by the caller's map.
struct ClaimWindow {
    unsigned long            openMs; // host nowMs() when the FIRST claim opened it
    std::vector<ClaimEntry>  claims;
};

// Append (claimantId, mappedMs, mappable) into an EXISTING window. Idempotent
// per claimant: a second call with the same claimantId (reliable-channel
// resend) updates that claimant's entry in place rather than appending a
// duplicate competing row. Returns true if this claimant is NEW to the
// window, false if it replaced an existing entry.
inline bool addClaim(ClaimWindow& w, u32 claimantId, unsigned long mappedMs, bool mappable) {
    for (std::size_t i = 0; i < w.claims.size(); ++i) {
        if (w.claims[i].claimantId == claimantId) {
            w.claims[i].mappedMs = mappedMs;
            w.claims[i].mappable = mappable;
            return false;
        }
    }
    ClaimEntry e;
    e.claimantId = claimantId; e.mappedMs = mappedMs; e.mappable = mappable;
    w.claims.push_back(e);
    return true;
}

// Single entry point the host calls for every incoming claim: opens a fresh
// window on the FIRST claim seen for (authorId, netId) (openMs = nowMs), or
// appends/updates the claimant's entry into an ALREADY-open window via
// addClaim. Returns true only when this call OPENED a fresh window (the
// caller uses this to decide whether to log "window opened").
inline bool openClaim(std::map<std::pair<u32, u32>, ClaimWindow>& windows,
                       u32 authorId, u32 netId, u32 claimantId,
                       unsigned long mappedMs, bool mappable, unsigned long nowMs) {
    std::pair<u32, u32> key(authorId, netId);
    std::map<std::pair<u32, u32>, ClaimWindow>::iterator it = windows.find(key);
    if (it == windows.end()) {
        ClaimWindow w;
        w.openMs = nowMs;
        ClaimEntry e;
        e.claimantId = claimantId; e.mappedMs = mappedMs; e.mappable = mappable;
        w.claims.push_back(e);
        windows.insert(std::make_pair(key, w));
        return true;
    }
    addClaim(it->second, claimantId, mappedMs, mappable);
    return false;
}

// True once `w` has been open >= windowLenMs (the caller finalizes it this tick).
inline bool claimWindowExpired(const ClaimWindow& w, unsigned long nowMs,
                                unsigned long windowLenMs) {
    return (nowMs - w.openMs) >= windowLenMs;
}

// The deterministic winner of an open (or just-expired) window: the smallest
// mappedMs among MAPPABLE claims; every claim within epsMs of that earliest
// stamp, or with no mappable stamp of its own, is in the tie band; the
// winner is the LOWEST claimantId within the tie band. Pure function of
// (stamps, epsMs, claimantIds) - the same window replayed with the same
// inputs always names the same winner. Returns false (no winner) only when
// `w` has zero claims (should not happen - a window is only ever opened by
// its first claim).
inline bool finalizeClaim(const ClaimWindow& w, unsigned long epsMs, u32* outWinnerId) {
    if (outWinnerId) *outWinnerId = 0;
    if (w.claims.empty()) return false;
    bool          haveMappable = false;
    unsigned long bestMs = 0;
    for (std::size_t i = 0; i < w.claims.size(); ++i) {
        if (!w.claims[i].mappable) continue;
        if (!haveMappable || w.claims[i].mappedMs < bestMs) {
            bestMs = w.claims[i].mappedMs;
            haveMappable = true;
        }
    }
    bool have = false;
    u32  winner = 0;
    for (std::size_t i = 0; i < w.claims.size(); ++i) {
        bool inTieBand = !haveMappable || !w.claims[i].mappable ||
                          (w.claims[i].mappedMs - bestMs <= epsMs);
        if (!inTieBand) continue;
        if (!have || w.claims[i].claimantId < winner) { winner = w.claims[i].claimantId; have = true; }
    }
    if (outWinnerId) *outWinnerId = winner;
    return have;
}

// One FINALIZED claim record (Phase 7 review WR-05): the committed flag and
// the winner live in a SINGLE map entry, so they can never be cap-evicted
// apart (the old separate claimCommitted_ set + claimWinners_ map were capped
// independently - a CLAIM-LATE hit whose winner record had been evicted but
// whose committed flag survived answered "winner=0", naming the host winner
// of a claim it never made, and the true claimant rolled back and destroyed
// its copy: loss). `seq` is the caller-supplied monotonic INSERTION order;
// cap eviction removes the smallest seq - the genuinely oldest-inserted
// record across authors - never the just-inserted one (its seq is the max).
struct ClaimFinal {
    u32 winnerId;
    u32 seq; // monotonic insertion order (caller-owned counter)
};

// True once (authorId, netId) has already been finalized ("commit is
// final" - INV-03): a later claim for this identity must never re-open or
// re-finalize a window, only be answered with the SAME prior verdict.
inline bool claimCommitted(const std::map<std::pair<u32, u32>, ClaimFinal>& finals,
                            u32 authorId, u32 netId) {
    return finals.count(std::make_pair(authorId, netId)) != 0;
}

// Committed-check + winner lookup in ONE step (the CLAIM-LATE re-answer
// path): returns true and fills *outWinnerId when (authorId, netId) is
// final. Because the flag and the winner are one record, a true return
// ALWAYS carries the real prior winner - the "committed but winner missing"
// divergence is structurally impossible.
inline bool claimWinnerFor(const std::map<std::pair<u32, u32>, ClaimFinal>& finals,
                            u32 authorId, u32 netId, u32* outWinnerId) {
    std::map<std::pair<u32, u32>, ClaimFinal>::const_iterator it =
        finals.find(std::make_pair(authorId, netId));
    if (it == finals.end()) return false;
    if (outWinnerId) *outWinnerId = it->second.winnerId;
    return true;
}

// Record (authorId, netId) as committed with its winner. `seqCounter` is the
// caller-owned monotonic insertion counter (consumed/post-incremented only on
// a fresh insert; a replayed mark keeps the original record untouched -
// commit is final). Cap-evicts the OLDEST-INSERTED record (smallest seq)
// once `finals` exceeds `cap` - see ClaimFinal for why not erase(begin()).
inline void markClaimCommitted(std::map<std::pair<u32, u32>, ClaimFinal>& finals,
                                u32 authorId, u32 netId, u32 winnerId,
                                u32& seqCounter, std::size_t cap) {
    std::pair<u32, u32> key(authorId, netId);
    if (finals.find(key) != finals.end()) return; // commit is final - never overwrite
    ClaimFinal f;
    f.winnerId = winnerId;
    f.seq      = seqCounter++;
    finals.insert(std::make_pair(key, f));
    while (finals.size() > cap) {
        std::map<std::pair<u32, u32>, ClaimFinal>::iterator oldest = finals.begin();
        for (std::map<std::pair<u32, u32>, ClaimFinal>::iterator it = finals.begin();
             it != finals.end(); ++it) {
            if (it->second.seq < oldest->second.seq) oldest = it;
        }
        finals.erase(oldest);
    }
}

// Disconnect-cleanup primitive (INV-04 shape applied to claims): erase every
// claim entry authored by `claimantId` from every OPEN window, so a departed
// claimant's bid can never win a contention it no longer participates in.
// A window left with zero claims is erased outright (nothing left to
// finalize). Returns the total number of claim entries erased, for the
// caller's own greppable log line (mirrors xferEraseOwner's shape).
inline unsigned int claimEraseClaimant(std::map<std::pair<u32, u32>, ClaimWindow>& windows,
                                        u32 claimantId) {
    unsigned int erased = 0;
    for (std::map<std::pair<u32, u32>, ClaimWindow>::iterator it = windows.begin();
         it != windows.end(); ) {
        std::vector<ClaimEntry>& claims = it->second.claims;
        for (std::size_t i = 0; i < claims.size(); ) {
            if (claims[i].claimantId == claimantId) {
                claims.erase(claims.begin() + (std::vector<ClaimEntry>::difference_type)i);
                ++erased;
            } else {
                ++i;
            }
        }
        if (claims.empty()) windows.erase(it++);
        else ++it;
    }
    return erased;
}

// Rejoin-cleanup primitives (Phase 7 review CR-01): erase every window /
// final record whose AUTHOR (key.first - the netId space the item lives in)
// is `authorId`. Called on the CONNECT edge when a PlayerId slot is
// (re)assigned: a fresh client process restarts nextWorldNetId_ at 1, so any
// record kept from that slot's previous connection collides with the new
// connection's netIds - a stale final record re-answers a claim on a NEW
// item with the OLD winner (the item is then destroyed on both ends:
// permanent loss). Distinct from claimEraseClaimant (the DISCONNECT
// primitive, claimant-scoped): purging author-scoped state only at
// reassignment time lets windows the departed author's items still had open
// finalize normally among the surviving claimants (exactly one of the
// optimistic pickups survives), and sweeps the final records those late
// finalizations minted too - a disconnect-time purge would miss them.
// Return the number of entries erased.
inline unsigned int claimEraseAuthor(std::map<std::pair<u32, u32>, ClaimWindow>& windows,
                                      u32 authorId) {
    unsigned int erased = 0;
    for (std::map<std::pair<u32, u32>, ClaimWindow>::iterator it = windows.begin();
         it != windows.end(); ) {
        if (it->first.first == authorId) { windows.erase(it++); ++erased; }
        else ++it;
    }
    return erased;
}

inline unsigned int claimEraseAuthorFinals(std::map<std::pair<u32, u32>, ClaimFinal>& finals,
                                            u32 authorId) {
    unsigned int erased = 0;
    for (std::map<std::pair<u32, u32>, ClaimFinal>::iterator it = finals.begin();
         it != finals.end(); ) {
        if (it->first.first == authorId) { finals.erase(it++); ++erased; }
        else ++it;
    }
    return erased;
}

// Defensive cap-eviction. claimWindows_ stays small in practice (a window is
// erased the instant it finalizes), so this is a bound, not a hot path.
// Phase 7 review WR-05: evicts the window with the SMALLEST openMs - the one
// genuinely open longest - not erase(begin())'s lowest-(authorId,netId) key,
// which across authors is unrelated to age and could evict the window a
// claim just opened this very call (openMs == now is by definition never the
// minimum while any older window exists).
inline void claimEvictOldest(std::map<std::pair<u32, u32>, ClaimWindow>& windows,
                              std::size_t cap) {
    while (windows.size() > cap) {
        std::map<std::pair<u32, u32>, ClaimWindow>::iterator oldest = windows.begin();
        for (std::map<std::pair<u32, u32>, ClaimWindow>::iterator it = windows.begin();
             it != windows.end(); ++it) {
            if (it->second.openMs < oldest->second.openMs) oldest = it;
        }
        windows.erase(oldest);
    }
}

} // namespace coop

#endif // KENSHICOOP_CLAIM_ARBITER_H
