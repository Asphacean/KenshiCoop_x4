// CellMap - engine-free host-side cell-claim-authority reduce (Phase 8 Plan
// 02, WORLD-03).
//
// RESEARCH (08-RESEARCH.md, cell-claim analysis) found the per-instance
// divergent reduce: today every instance runs rebuildClaimedCells
// (ReplicatorAuthority.cpp:1150-1188) independently over its OWN claim
// intake, resolving a contested cell by std::map ITERATION ORDER with a
// host-wins-ties override - which is NOT continuity (a present incumbent
// can lose a cell to a later-arriving host claim it never yielded to), and
// two instances that happen to fold the same claim set in different
// insertion order can disagree about who owns a cell they both see
// identically. The locked fix (CONTEXT.md, PLAN.md): the HOST alone runs
// this reduce and broadcasts the single verdict (PKT_CELL_MAP); every
// instance adopts the SAME host-authored map, so the tie-break only ever
// needs to be computed ONCE, by one party, over one true claim set.
//
// This header mirrors FoldDedup.h / ClaimArbiter.h's precedent exactly:
// header-only, namespace coop, zero engine/Wire coupling (no #include
// "Wire.h" - the includer already supplies coop::u32/i32), unit-testable
// from prototest with no GameWorld. reduceCellMap is a PURE function of its
// four inputs (claim slots, the previous resolved map, the connected-owner
// set, and the host's own id) - the same inputs fed in ANY insertion order
// produce the byte-identical output map, which is what makes a single
// host-side reduce trustworthy: prototest's testCellMap shuffled-order case
// is the regression test for the std::map-iteration-order bug this
// supersedes.
//
// Tie-break, per contested cell (continuity -> host-if-party -> lowest
// playerId, "presence-based hysteresis" - CONTEXT.md's locked decision):
//   1. CONTINUITY - if the cell's PREVIOUS resolved owner still holds a
//      live claim slot in that cell AND is still connected, it keeps the
//      cell. No timers, no dwell counters here (the publish-side dwell in
//      Replicator::syncCellClaims already damps flapping before a claim
//      ever reaches the reduce) - continuity is evaluated purely from
//      (previous verdict, current slot set, connected-owner set).
//   2. FRESH CONTEST (no present incumbent) - the host wins if it holds a
//      slot in the cell; otherwise the LOWEST playerId among the cell's
//      claimants wins (a std::set<u32> iterates ascending, so *begin() is
//      always the answer, deterministically, regardless of insertion
//      order).
//   3. VACANCY - a cell with no current claimant is simply ABSENT from the
//      output map (never inserted), which is the fail-open-to-host
//      contract every consumer (authorityFor/authoritySrc) already
//      implements for a cell absent from claimedCells_.
//
// Disconnect/rejoin (Replicator::clearPeerReplicationState /
// purgeAuthorConservationState, ReplicatorCore.cpp) is the CALLER's
// responsibility, not this header's: erasing a departed owner's claim
// slots (and its cellLastOwner_ entries, so the AUTHSRC_VACATE fallback
// does not resurrect it) before the next reduceCellMap call is what turns
// "departed owner's cell" into "absent from claimantsByCell" here, which
// resolves to VACANCY (case 3) and fails open to the host - the locked
// revert-to-host disposition (RESEARCH.md Open Question 2).

#ifndef KENSHICOOP_CELL_MAP_H
#define KENSHICOOP_CELL_MAP_H

#include <map>
#include <set>
#include <utility>

namespace coop {

// One claim slot's cell, as the reduce needs it - a smaller view than
// Replicator::CellClaim (no seq/recvMs; staleness/dwell is the publish
// side's problem, upstream of this pure function). The includer fills one
// CellSlotView per (ownerId, tabRank) slot it still considers live.
struct CellSlotView {
    int cx, cz;
};

// Input: (ownerId, tabRank) -> the cell that slot currently claims. Same
// key shape as Replicator::claimSlots_ so the includer can build this with
// a trivial per-entry copy (no engine/Wire type crosses this header).
typedef std::map<std::pair<u32, u32>, CellSlotView> CellSlotMap;

// Output/previous-map shape: cell -> owning player. Matches
// Replicator::claimedCells_'s type exactly (a bare
// std::map<std::pair<int,int>, u32> rather than a named typedef, so the
// includer's own member variable is a drop-in match on both sides).
typedef std::map<std::pair<int, int>, u32> CellOwnerMap;

// The pure reduce (see file header for the full tie-break contract).
// `slots` - every (ownerId, tabRank) slot's current cell, from every
//           connected owner (a departed owner's slots must already be
//           erased by the caller - see clearPeerReplicationState).
// `previousMap` - the PRIOR resolved map (a caller-supplied copy, NOT the
//           same object as `outMap` - aliasing the two is the caller's bug
//           to avoid, since this function clears `outMap` first).
// `connectedOwners` - every owner id currently connected (host included -
//           the host is always "connected" to itself).
// `hostId` - CELL_OWNER_HOST (0), passed explicitly rather than assumed so
//           this header stays engine/Replicator-constant-free.
// `outMap` - cleared and filled with the resolved verdict.
inline void reduceCellMap(const CellSlotMap& slots,
                           const CellOwnerMap& previousMap,
                           const std::set<u32>& connectedOwners,
                           u32 hostId,
                           CellOwnerMap& outMap) {
    outMap.clear();

    // Group live claimants per cell. std::map<cell, std::set<owner>> keeps
    // both the cell ordering and the per-cell claimant ordering canonical
    // (sorted), which is what makes the whole function insertion-order-
    // invariant regardless of the order `slots` was built in.
    std::map<std::pair<int, int>, std::set<u32> > claimantsByCell;
    for (CellSlotMap::const_iterator it = slots.begin(); it != slots.end(); ++it) {
        u32 owner = it->first.first;
        std::pair<int, int> cell(it->second.cx, it->second.cz);
        claimantsByCell[cell].insert(owner);
    }

    for (std::map<std::pair<int, int>, std::set<u32> >::const_iterator ci =
             claimantsByCell.begin();
         ci != claimantsByCell.end(); ++ci) {
        const std::pair<int, int>& cell      = ci->first;
        const std::set<u32>&       claimants = ci->second;

        // (1) CONTINUITY: the previous incumbent keeps the cell if it still
        // holds a live claim slot there AND is still connected.
        CellOwnerMap::const_iterator pv = previousMap.find(cell);
        if (pv != previousMap.end()) {
            u32 incumbent = pv->second;
            if (claimants.find(incumbent) != claimants.end() &&
                connectedOwners.find(incumbent) != connectedOwners.end()) {
                outMap[cell] = incumbent;
                continue;
            }
        }

        // (2) FRESH CONTEST: host wins if it is a party; else the lowest
        // playerId among the claimants (std::set<u32> is sorted ascending).
        if (claimants.find(hostId) != claimants.end()) {
            outMap[cell] = hostId;
        } else {
            outMap[cell] = *claimants.begin();
        }
    }

    // (3) VACANCY: a cell with zero current claimants never enters
    // claimantsByCell at all, so it is simply absent from outMap here -
    // every consumer already fails that open to the host.
}

// Phase 8 review WR-04: deterministic overflow truncation, shared by the
// host's OWN adoption and its broadcast serialization so the one-verdict
// invariant holds even past the wire cap. The broadcast can carry at most
// maxN entries (CellMapPacket::entries / CELL_MAP_MAX); if the host adopted
// the FULL reduced map while every client adopted the truncated broadcast,
// cells maxN+1.. would fail open to host on clients while the host honored
// the real owner - a silent authority split, the exact divergence protocol
// 59 exists to make impossible by construction. Truncating the map BEFORE
// the host adopts it means host and clients read one identical verdict; the
// dropped cells fail open to host on EVERYONE (the safe direction, and the
// ordinary semantics of a cell absent from the map). Deterministic by
// construction: std::map iterates its keys sorted ascending, so the kept
// prefix is always the LOWEST (cellX, cellY) keys regardless of insertion
// order. Returns the number of entries erased (0 = no overflow).
inline unsigned int truncateCellMap(CellOwnerMap& m, unsigned int maxN) {
    if (m.size() <= (size_t)maxN) return 0;
    unsigned int erased = 0;
    CellOwnerMap::iterator it = m.begin();
    for (unsigned int i = 0; i < maxN; ++i) ++it;
    while (it != m.end()) {
        CellOwnerMap::iterator victim = it++;
        m.erase(victim);
        ++erased;
    }
    return erased;
}

} // namespace coop

#endif // KENSHICOOP_CELL_MAP_H
