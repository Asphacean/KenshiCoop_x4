// MoneyFold - engine-free host-side shared-money-pool arbitration (Phase 9
// Plan 01, CONS-01).
//
// RESEARCH (09-RESEARCH.md "Money Trace + Gap Analysis") found three genuine
// at-N defects in the shared pool: (1) the host's broadcast ack was ONE
// scalar overwritten by whichever owner folded LAST, so a join's own
// poolPending_ popped against a FOREIGN owner's seq space (the proven wallet
// bounce/double-count); (2) overdraft was not a verdict at all - the host
// folded every delta unconditionally and clamped the pool at 0, silently
// minting value with no reject and nothing deterministic for a second buyer;
// (3) reconnect dropped a rejoined join's restarted seq=1 deltas because the
// fold high-water was never purged at the connect edge. The locked fix
// (09-CONTEXT.md, "Per-Player ACK + Timestamp Overdraft Arbitration"): a
// solvent delta folds immediately (no added latency); a delta that would
// overdraw opens ONE bounded global window (there is exactly one shared
// pool, unlike ClaimArbiter's per-(authorId,netId) map of windows) - every
// competing spend during the window joins the candidate set with its
// peerClock_-mapped stamp, and at expiry the window folds candidates in
// ascending adjusted-stamp order (epsMs tie-band broken by the lowest
// ownerId) while the pool covers them, rejecting the remainder.
//
// This header mirrors ClaimArbiter.h's contract applied to a single pool
// instead of a per-item map: header-only, namespace coop, zero engine/Wire
// coupling (no #include "Wire.h" - the includer already supplies coop::u32,
// mirroring ClaimArbiter.h:14-20), unit-testable from prototest with no
// GameWorld. HOST-only state; the join-side "pop against my own ack only"
// decision is a tiny pure predicate any caller can lock without ENet either
// (see moneyShouldPop below).
//
// Arbitration model, the whole contract in one place:
//   moneyOffer         - the single entry point the host calls for every
//                 incoming join delta. A stale/replayed (owner,seq) already
//                 at or below that owner's high-water in `processed` is
//                 MONEY_DUP (commit-final: never re-folded, never
//                 re-queued - the markClaimCommitted "never overwrite"
//                 property applied to money). A positive delta or a solvent
//                 negative one (poolTotal + delta >= 0) with NO window open
//                 folds immediately: MONEY_FOLD (the caller folds the pool
//                 and advances `processed[owner]` itself - moneyOffer never
//                 mutates `processed` on a FOLD verdict, only reads it for
//                 the dup check). A delta that would overdraw the pool, OR
//                 any delta at all while a window is ALREADY open (the
//                 determinism keystone - once contention has begun, a new
//                 solvent spend must also queue, or fold order would depend
//                 on arrival timing and the verdict would stop being
//                 reproducible across reruns), opens the window if needed
//                 and appends/updates the delta in `windowDeltas`
//                 (idempotent per (owner,seq), a reliable-channel resend
//                 updates its entry in place rather than duplicating a
//                 competing row): MONEY_QUEUE.
//   moneyWindowExpired - true once the open window has been open >= its
//                 configured length (the caller finalizes it that tick).
//                 False (never expired) while no window is open.
//   moneyFinalize      - the deterministic settlement: every queued delta is
//                 ordered by (mappedMs asc; within epsMs of the earliest
//                 MAPPABLE stamp, or with no mappable stamp of its own, is
//                 in the tie band; inside the band the LOWEST ownerId sorts
//                 first; outside the band, ties break the same way -
//                 finalizeClaim's rule verbatim, generalized from "pick one
//                 winner" to "produce a full deterministic order"; a SAME-
//                 owner tie - one owner with several queued seqs - breaks by
//                 ascending seq, that owner's own emission order, so the
//                 order is strict over the full (band, ownerId, seq)
//                 identity under an unstable std::sort). Folds
//                 greedily left-to-right while `poolTotal + delta >= 0`
//                 (mutating poolTotal and appending to outFolded), rejects
//                 the remainder (outRejected); advances `processed[owner]`
//                 for EVERY candidate in BOTH lists (a rejected seq is
//                 "processed" too - that is what lets the join's ack pop the
//                 pending delta and refund). A pure function of its inputs:
//                 the same window replayed with the same candidate set in
//                 ANY insertion order into windowDeltas names the same fold
//                 set and the same final total, because the sort recomputes
//                 the ordering from the candidates' own fields, never from
//                 arrival order.
//   moneyEraseOwner    - disconnect cleanup (the claimEraseClaimant shape
//                 applied to the ONE global window): drops every queued
//                 (unfolded) delta authored by `ownerId` from an open
//                 window, so a departed buyer's queued spend can never win a
//                 fold it no longer participates in. Does NOT touch
//                 `processed` (the connect-edge purge - the
//                 claimEraseAuthor/purgeAuthorConservationState
//                 rationale - owns clearing that, via a direct
//                 `processed.erase(ownerId)` the caller performs itself,
//                 same as claimEraseAuthorFinals is a separate call from
//                 claimEraseClaimant).
//   moneyShouldPop     - the join-side pop-on-own-ack-only predicate
//                 (09-RESEARCH.md Pitfall 1): a join must pop its pending
//                 queue against ONLY the MoneyPacket ack Entry whose
//                 ownerId == its own localId, NEVER a foreign owner's entry
//                 (the at-N bounce/double-count this plan exists to kill).
//                 A pure comparison, extracted so prototest can lock it
//                 without any wire/engine coupling.

#ifndef KENSHICOOP_MONEY_FOLD_H
#define KENSHICOOP_MONEY_FOLD_H

#include <algorithm>
#include <cstddef>
#include <map>
#include <vector>
#include "FoldDedup.h" // foldMonotonic: the exact processed[owner] high-water advance

namespace coop {

// One competing money delta: either a fresh arrival being offered to
// moneyOffer, or an entry already sitting in an open window's candidate set.
struct MoneyDelta {
    u32           ownerId;
    u32           seq;
    int           delta;    // signed change to the pool (negative = spent)
    unsigned long mappedMs; // hostLocalEst = authorSpendMs + peerClock_[owner].offsetMs,
                             // clamped <= now; meaningless when !mappable
    bool          mappable; // false when the owner has no peerClock_ entry yet
};

// The host's per-pool arbitration state. Unlike ClaimArbiter's
// map<(authorId,netId), ClaimWindow> (many independent contested items),
// there is exactly ONE shared money pool, so exactly one optional open
// window at a time - a plain bool + a single candidate vector, not a map.
struct MoneyFoldState {
    std::map<u32, u32>      processed;    // highestAck[owner]: seq PROCESSED (folded OR rejected)
    bool                    windowOpen;
    unsigned long           windowOpenMs; // host nowMs() when the window opened
    std::vector<MoneyDelta> windowDeltas; // candidates queued during the open window
    MoneyFoldState() : windowOpen(false), windowOpenMs(0) {}
};

enum MoneyVerdict {
    MONEY_FOLD,  // solvent, no window open - the caller folds it immediately
    MONEY_QUEUE, // would overdraw, or a window is already open - joins the candidate set
    MONEY_DUP    // stale/replayed vs processed[owner] - commit-final, never re-answered
};

// Single entry point the host calls for every incoming join delta. See the
// header comment above for the full verdict contract. Never mutates
// `processed` itself on MONEY_FOLD (the caller folds the pool and advances
// processed[owner] - moneyOffer only READS processed for the dup check);
// moneyFinalize is the ONLY function that advances processed for a queued
// candidate, on both the folded and rejected side.
inline MoneyVerdict moneyOffer(MoneyFoldState& st, const MoneyDelta& d,
                                int poolTotal, unsigned long nowMs) {
    std::map<u32, u32>::const_iterator pit = st.processed.find(d.ownerId);
    if (pit != st.processed.end() && d.seq <= pit->second) return MONEY_DUP;

    bool solvent = (poolTotal + d.delta) >= 0;
    if (solvent && !st.windowOpen) return MONEY_FOLD;

    // Would overdraw, OR a window is already open (the determinism keystone:
    // once contention has begun, a NEW solvent spend queues too, so fold
    // order never depends on arrival timing).
    if (!st.windowOpen) { st.windowOpen = true; st.windowOpenMs = nowMs; }
    for (std::size_t i = 0; i < st.windowDeltas.size(); ++i) {
        if (st.windowDeltas[i].ownerId == d.ownerId && st.windowDeltas[i].seq == d.seq) {
            st.windowDeltas[i] = d; // idempotent per (owner,seq): resend updates in place
            return MONEY_QUEUE;
        }
    }
    st.windowDeltas.push_back(d);
    return MONEY_QUEUE;
}

// True once the open window has been open >= windowLenMs (the caller
// finalizes it this tick). Always false while no window is open.
inline bool moneyWindowExpired(const MoneyFoldState& st, unsigned long nowMs,
                                unsigned long windowLenMs) {
    return st.windowOpen && (nowMs - st.windowOpenMs) >= windowLenMs;
}

namespace detail {
// The finalizeClaim tie-band rule (ClaimArbiter.h:132-162) generalized from
// "pick the single winner" to "produce a full deterministic order": the
// earliest-mapped-stamp cluster (within epsMs of the earliest MAPPABLE
// stamp, or unmappable at all) sorts FIRST as a group, ordered by ascending
// ownerId within that group; everything else sorts after, by ascending
// mappedMs (ties broken by ownerId too, for a fully deterministic order
// regardless of how many candidates share the exact same stamp).
struct MoneyOrder {
    bool          haveMappable;
    unsigned long bestMs;
    unsigned long epsMs;
    MoneyOrder(bool hm, unsigned long bm, unsigned long eps)
        : haveMappable(hm), bestMs(bm), epsMs(eps) {}
    bool inBand(const MoneyDelta& d) const {
        return !haveMappable || !d.mappable || (d.mappedMs - bestMs <= epsMs);
    }
    bool operator()(const MoneyDelta& a, const MoneyDelta& b) const {
        bool ai = inBand(a), bi = inBand(b);
        if (ai != bi) return ai;                     // in-band candidates sort first
        // Phase 9 review WR-02: SAME-owner candidates (one owner queues seq N
        // and seq N+1 into one window - trivially reachable, publishMoneyPool
        // emits one delta per tick and any delta arriving while the window is
        // open queues) previously compared equivalent both ways, and
        // std::sort is UNSTABLE - their relative order (and therefore which
        // folds and which rejects when the pool covers only one) depended on
        // the implementation's partitioning of the copied windowDeltas
        // vector, contradicting the header's "same candidate SET in ANY
        // insertion order names the same fold set" contract. Break the tie by
        // ascending seq (the owner's own emission order) so the ordering is a
        // strict weak order over the full candidate identity (band, ownerId,
        // seq) and every rerun names the same verdict.
        if (ai) {
            if (a.ownerId != b.ownerId) return a.ownerId < b.ownerId; // band: lowest owner first
            return a.seq < b.seq;      // same owner: emission order, deterministic
        }
        if (a.mappedMs != b.mappedMs) return a.mappedMs < b.mappedMs;
        if (a.ownerId != b.ownerId) return a.ownerId < b.ownerId;     // out-of-band tie
        return a.seq < b.seq;          // same owner out-of-band: still deterministic
    }
};
} // namespace detail

// The deterministic settlement of an open (or just-expired) window: orders
// every candidate (see detail::MoneyOrder above), folds greedily while
// `poolTotal + delta >= 0` (mutating poolTotal, appending to outFolded),
// rejects the remainder (outRejected), and advances processed[owner] for
// EVERY candidate in both lists (foldMonotonic - a rejected seq is
// "processed" too, which is what lets the join's ack pop the pending delta
// and refund). A pure function of the window's contents: the same
// candidate SET replayed in any insertion order names the same fold set and
// the same final total. Clears the window (windowDeltas + windowOpen) on
// return, including the degenerate case of zero candidates (a window whose
// only participant disconnected mid-window - see moneyEraseOwner).
inline void moneyFinalize(MoneyFoldState& st, int& poolTotal, unsigned long epsMs,
                           std::vector<MoneyDelta>& outFolded,
                           std::vector<MoneyDelta>& outRejected) {
    outFolded.clear();
    outRejected.clear();

    bool          haveMappable = false;
    unsigned long bestMs       = 0;
    for (std::size_t i = 0; i < st.windowDeltas.size(); ++i) {
        if (!st.windowDeltas[i].mappable) continue;
        if (!haveMappable || st.windowDeltas[i].mappedMs < bestMs) {
            bestMs       = st.windowDeltas[i].mappedMs;
            haveMappable = true;
        }
    }

    std::vector<MoneyDelta> ordered = st.windowDeltas;
    std::sort(ordered.begin(), ordered.end(),
              detail::MoneyOrder(haveMappable, bestMs, epsMs));

    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const MoneyDelta& d = ordered[i];
        int want = poolTotal + d.delta;
        if (want >= 0) {
            poolTotal = want;
            outFolded.push_back(d);
        } else {
            outRejected.push_back(d);
        }
        foldMonotonic(st.processed, d.ownerId, d.seq);
    }

    st.windowDeltas.clear();
    st.windowOpen = false;
}

// Disconnect cleanup (the claimEraseClaimant shape applied to the ONE global
// window): drop every queued (unfolded) delta authored by `ownerId`, so a
// departed buyer's queued spend can never win a fold it no longer
// participates in. Does NOT touch `processed` or close the window itself -
// an emptied-out window still expires normally next tick and
// moneyFinalize on zero candidates is a harmless no-op. Returns the count
// erased, for the caller's own greppable log line.
inline unsigned int moneyEraseOwner(MoneyFoldState& st, u32 ownerId) {
    unsigned int erased = 0;
    for (std::size_t i = 0; i < st.windowDeltas.size(); ) {
        if (st.windowDeltas[i].ownerId == ownerId) {
            st.windowDeltas.erase(
                st.windowDeltas.begin() + (std::vector<MoneyDelta>::difference_type)i);
            ++erased;
        } else {
            ++i;
        }
    }
    return erased;
}

// Join-side pop predicate (09-RESEARCH.md Pitfall 1, the at-N bounce/
// double-count this plan exists to kill): true when a pending delta whose
// own seq is `pendingSeq` should be popped given THIS join's own ack value
// `myAck` (the MoneyPacket Entry whose ownerId == the join's own localId -
// never a foreign owner's entry, never MoneyPacket::acks[last] or any other
// cross-owner compare). Pure predicate so prototest locks the exact
// property without any wire/engine coupling.
inline bool moneyShouldPop(u32 pendingSeq, u32 myAck) {
    return pendingSeq <= myAck;
}

} // namespace coop

#endif // KENSHICOOP_MONEY_FOLD_H
