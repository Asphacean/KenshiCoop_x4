// PinOwner - engine-free pin-author bookkeeping (Phase 6, GAP-3/PLAY-03).
//
// RESEARCH (06-RESEARCH.md GAP-3) found `pinPeer_` (Replicator.h) is an
// AUTHORLESS `std::set<Key>`: it records WHICH hands a peer authored (so
// publishOwned never streams them, ReplicatorPublish.cpp:94), but not WHO
// authored each one. At N=2 that is fine - there is only ever one possible
// peer author. At N>=3 it is a genuine gap: `clearPeerReplicationState`
// cannot safely erase a departing peer's pins (a set entry cannot be
// attributed to the departing author without risking a SURVIVING peer's
// pins too), so a departed join's recruits stay orphan-pinned forever on
// every survivor (TWO_PLAYER_ASSUMPTIONS finding 3's own remediation shape:
// a Key -> owner map).
//
// This header captures that map's mutation shape ONCE, mirroring
// FoldDedup.h's precedent (a tiny pure-C++03 helper set, zero engine/Wire
// coupling, unit-testable from prototest with no GameWorld). It is
// ADDITIVE, not a replacement: `pinPeer_` (Replicator.h) remains the
// authoritative membership/veto set exactly as today - every existing
// pinPeer_ read (publish veto ReplicatorPublish.cpp:94, rekey echo guard
// ReplicatorSpawn.cpp:947/972-976/908-911, squad publish
// ReplicatorChannels.cpp:2090-2091) is completely unchanged. `pinnedOwner_`
// (a parallel std::map<Key,u32>) is written alongside every pinPeer_
// mutation and read ONLY by clearPeerReplicationState's owner-scoped erase
// (ReplicatorCore.cpp) - it never influences publish/apply decisions
// itself, so the 2-player veto behavior this phase's own threat register
// (T-06-04) worries about regressing stays byte-for-byte identical.
//
// Templated on the caller's own comparable key type (KeyT) rather than
// naming a concrete `Key` struct: Replicator's `Key` is a PRIVATE nested
// type (Replicator.h ~870), so a free-standing non-friend header cannot
// name it directly. A template sidesteps that with zero coupling - it
// deduces to Replicator::Key at every call site inside Replicator's own
// member functions, where Key is accessible, and to prototest's own local
// test-key struct in the unit section below. Same idiom as the existing
// WorldQ<T> template (Inbound.h) and pushLocked<T> (NetLink.cpp) - this
// codebase already uses C++03 templates, so this is not a new pattern.

#ifndef KENSHICOOP_PIN_OWNER_H
#define KENSHICOOP_PIN_OWNER_H

#include <map>

namespace coop {

// Record (or overwrite) the author of a peer-pinned hand. Called alongside
// every `pinPeer_.insert(key)` with the authenticated author's ownerId (the
// event's own `ownerId` field - already validated by rejectIfForgedOwner on
// every Class A relay, so this map never trusts an unauthenticated claim).
template<typename KeyT>
inline void pinRecord(std::map<KeyT, u32>& owners, const KeyT& key, u32 owner) {
    owners[key] = owner;
}

// Drop one hand's author record. Called alongside every `pinPeer_.erase(key)`
// so the author record never outlives the pin it describes (a stale entry
// here would misattribute the hand to a departed owner it no longer names,
// or worse, to a NEW owner that later reuses the same key).
template<typename KeyT>
inline void pinForget(std::map<KeyT, u32>& owners, const KeyT& key) {
    owners.erase(key);
}

// The disconnect-cleanup primitive: erase every entry authored by `owner`,
// returning the count removed. The caller (clearPeerReplicationState) uses
// the count for its own greppable log line and to drive the matching
// `pinPeer_.erase(key)` for each removed key - this function touches ONLY
// `owners`, never the caller's `pinPeer_` set, keeping the two maps' erase
// as two explicit, auditable steps at the call site rather than a hidden
// side effect here.
template<typename KeyT>
inline unsigned int pinEraseOwner(std::map<KeyT, u32>& owners, u32 owner) {
    unsigned int removed = 0;
    for (typename std::map<KeyT, u32>::iterator it = owners.begin();
         it != owners.end(); ) {
        if (it->second == owner) {
            owners.erase(it++);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

} // namespace coop

#endif // KENSHICOOP_PIN_OWNER_H
