// FoldDedup - the composite-key (ownerId, ...) fold/dedup helpers the
// sender-scoped ID audit (Phase 5) standardizes on.
//
// RESEARCH (Phase 5 Pitfall 3) found the proven-dangerous shape: a channel
// tracks "highest seq folded" as ONE bare scalar shared across every sender.
// That is correct for exactly one remote peer (the two-player design target)
// but silently drops a second/third sender's deltas the moment the first
// sender's seq passes them - not a crash, not a log line, just money (or an
// event, or a proxy id) that never lands. The fix is always the same shape:
// key the fold/dedup decision on (ownerId, ...) instead of on the bare value.
//
// This header captures that shape ONCE so every channel this phase touches
// (money pool here, events/proxies in Plan 02) reuses the same, already-
// proven-correct predicate instead of hand-rolling a per-channel composite
// key that can drift. Pure inline C++03, zero game/logger/wire dependency
// (matches ChangeGate.h / EngineFaults.h / EngineCaps.h) so the unit layer
// (prototest) locks the fold/dedup decision directly, without a GameWorld.
// Deliberately does NOT include Wire.h itself (no engine coupling); every
// includer (Replicator.h, prototest/main.cpp) already pulls in Wire.h's
// coop::u32 typedef ahead of this header.
//
// foldOnce()'s composite-key idiom mirrors the existing appliedDrops_ pattern
// at ReplicatorItems.cpp:1053-1060 (std::set<std::pair<u32,u32> > + oldest-
// first eviction past a cap) rather than inventing a new one.

#ifndef KENSHICOOP_FOLD_DEDUP_H
#define KENSHICOOP_FOLD_DEDUP_H

#include <map>
#include <set>
#include <utility>

namespace coop {

// PER-OWNER MONOTONIC FOLD. `acked` tracks, per owner, the highest seq folded
// so far (absent key == never folded). Returns true and records `seq` as the
// new high-water for `owner` when seq is STRICTLY greater than the stored
// value; returns false (no record change) otherwise. Two different owners
// each folding seq=1 both return true - the whole point: the decision is
// keyed on (owner, seq), never on a single cross-owner scalar, so a second or
// third sender's low seq is never mistaken for "already folded" by a first
// sender's higher one.
inline bool foldMonotonic(std::map<u32, u32>& acked, u32 owner, u32 seq) {
    std::map<u32, u32>::iterator it = acked.find(owner);
    if (it == acked.end()) {
        acked.insert(std::make_pair(owner, seq));
        return true;
    }
    if (seq <= it->second) return false; // already folded (or stale/replayed)
    it->second = seq;
    return true;
}

// COMPOSITE-KEY ONE-SHOT DEDUP. `seen` is a set of (owner, id) pairs already
// applied. Returns true and inserts on first sight of (owner, id); returns
// false on a repeat (idempotent - reliable-channel resend/replay). Evicts the
// smallest (owner, id) pair once size exceeds `cap` - ids are per-sender
// monotonic, so the smallest key is always the oldest, far outside any
// plausible reliable-channel replay window (mirrors appliedDrops_'s existing
// 4096-cap eviction at ReplicatorItems.cpp:1060).
inline bool foldOnce(std::set<std::pair<u32, u32> >& seen, u32 owner, u32 id,
                      std::size_t cap) {
    std::pair<u32, u32> key(owner, id);
    if (seen.count(key) != 0) return false;
    seen.insert(key);
    if (seen.size() > cap) seen.erase(seen.begin());
    return true;
}

// CROSS-OWNER COLLISION GUARD (ID-03). True only when `existingOwner` is a
// GENUINE different owner than `incomingOwner` - i.e. some other sender
// already claimed this id and a different sender is now trying to touch it.
// False when the slot is unclaimed (existingOwner == noneSentinel) or already
// owned by the same sender (existingOwner == incomingOwner). Plan 02 consumes
// this to veto a cross-owner id clash before it corrupts shared state.
inline bool crossOwnerCollision(u32 existingOwner, u32 incomingOwner,
                                 u32 noneSentinel) {
    return existingOwner != noneSentinel && existingOwner != incomingOwner;
}

} // namespace coop

#endif // KENSHICOOP_FOLD_DEDUP_H
