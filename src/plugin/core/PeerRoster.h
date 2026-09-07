// PeerRoster.h - leave-queue expansion for the roster teardown (pure, zero
// game/Win32 deps).
//
// Inbound's leave queue carries TWO different kinds of entry:
//   * a real PlayerId - "peer N left" (host: its ENet DISCONNECT for that peer;
//     client: the host's PKT_PLAYER_LEFT roster broadcast for that peer);
//   * OWNER_ID_ALL    - "my single link to the host went down", pushed by the
//     CLIENT branch of NetLink's ENET_EVENT_TYPE_DISCONNECT case. A client only
//     ever holds one connection, and every other player's state is relayed
//     THROUGH the host, so that one drop means the whole roster is gone at once.
//
// Every consumer of a drained leave id is owner-scoped by `==` equality
// (Replicator::clearPeerReplicationState / notePeerLeft, pinEraseOwner,
// xferEraseOwner, saveEraseOwner, loadEraseOwner, the connected-peer set), so
// handing them the OWNER_ID_ALL sentinel matches NOTHING and the entire
// teardown silently no-ops. This header turns the sentinel into the concrete
// set of ids those owner-scoped consumers can actually act on, so the ONE code
// path serves both kinds of entry.
//
// Regression this guards (2026-09-07, x4 branch): the 2-player leave path called
// the session-global resetSession() unconditionally, so the sentinel needed no
// expansion. The N>=3 owner-scoped rewrite never handled it, and the observable
// result on a client whose host link dropped was a cleanup that reported
// "cleared proxies=0 released=0 ... pins=0 ... xfer voided=0" for
// owner=4294967295 while the departed peers' minted proxies, pins, transfer
// latches and presence entries all stayed live. The presence set and
// Replicator::knownPeers_ then only ever GREW - the following reconnect
// inserted the new ids on top of the dead ones, permanently, since
// resetSession() deliberately preserves knownPeers_ across a world reload.
//
// Deliberately shape-agnostic: at exactly one connected peer (1 host + 1 client
// UDP) the sentinel expands to that single id, which is precisely the teardown
// the 2-player build got from its global reset - no separate small-N path.

#ifndef COOP_PEER_ROSTER_H
#define COOP_PEER_ROSTER_H

#include <deque>
#include <set>
// Pulls in only the u32 typedef and the OWNER_ID_ALL constant (+ zero
// game/Win32 deps of its own - see Wire.h's own header comment), so this stays
// the pure, game-free layer prototest links without the Replicator/game.
#include "../../netproto/Wire.h"

namespace coop {

// Expand a drained leave queue into the EFFECTIVE list of departing PlayerIds.
//   drained   - the queue exactly as Inbound::drainLeaves() handed it over,
//               in wire order.
//   connected - the roster this client currently believes is present (the
//               caller's per-PlayerId presence set). Read-only; the caller
//               still erases from it per expanded id, as before.
//   out       - overwritten with the ids to run the per-peer teardown for.
// Returns true if the queue contained at least one OWNER_ID_ALL sentinel, i.e.
// this drain is a session BOUNDARY for us and not one peer's departure - the
// caller additionally drops its session-global roster state (knownPeers_ /
// allOwnRanks_), the same distinction sessionResetForUi() already draws for a
// panel disconnect (Replicator.h's clearKnownPeers WR-02 rationale).
//
// Ordinary ids pass through in wire order; a sentinel expands in place to the
// whole `connected` set in ascending id order (std::set order - deterministic,
// so the teardown log is reproducible run to run). Ids are DEDUPED across the
// whole expansion: a roster PKT_PLAYER_LEFT for peer N arriving in the same
// drain as the host-link drop must not run N's teardown (and log its purge
// lines) twice.
inline bool expandLeaveQueue(const std::deque<u32>& drained,
                             const std::set<u32>& connected,
                             std::deque<u32>& out) {
    out.clear();
    std::set<u32> emitted;
    bool sawAll = false;
    for (std::deque<u32>::const_iterator it = drained.begin();
         it != drained.end(); ++it) {
        if (*it == OWNER_ID_ALL) {
            sawAll = true;
            for (std::set<u32>::const_iterator cit = connected.begin();
                 cit != connected.end(); ++cit) {
                // A presence set can never legitimately hold the sentinel
                // itself; refuse to re-emit it if a prior build's leak left
                // one there, so a stale entry cannot resurrect the no-op.
                if (*cit == OWNER_ID_ALL) continue;
                if (emitted.insert(*cit).second) out.push_back(*cit);
            }
        } else {
            if (emitted.insert(*it).second) out.push_back(*it);
        }
    }
    return sawAll;
}

} // namespace coop

#endif // COOP_PEER_ROSTER_H
