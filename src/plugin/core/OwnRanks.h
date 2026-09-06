// OwnRanks.h - squad-tab ownership resolution (pure, zero game/Win32 deps).
//
// The ownership partition decides which squad tabs a peer controls locally and
// streams (its own) versus drives from the peer's stream. Host owns tab {0},
// join owns {1} by default; an explicit KENSHICOOP_OWN_SQUAD/OWN_RANK env
// override wins. This logic is shared by:
//   * Config.cpp   - initial resolution at load
//   * Plugin.cpp   - re-resolution when the F2 panel switches role mid-session
//   * prototest    - the no-game unit layer that guards the role-switch fix
//
// Bug this guards (2026-07-14): a session launched as HOST resolves ranks to
// {0}; switching to JOIN via the panel MUST re-resolve to {1}. Skipping that
// left the client claiming the host's rank-0 player squad, so that unit was
// treated as locally owned and never driven by the host's motion stream (it
// stood frozen while unowned NPCs replicated normally).

#ifndef COOP_OWN_RANKS_H
#define COOP_OWN_RANKS_H

#include <set>
#include <string>
// Phase 3 Plan 03: pulls in only the u32 typedef (+ zero game/Win32 deps of
// its own - see Wire.h's own header comment), so this stays the pure,
// game-free layer prototest links without the Replicator/game.
#include "../../netproto/Wire.h"

namespace coop {

// Parse a CSV of unsigned ints ("0", "1", "1,2") into out. Tolerant of spaces
// or any non-digit separator. Returns true if at least one rank was parsed.
inline bool parseRankList(const std::string& csv, std::set<unsigned int>& out) {
    unsigned int v = 0; bool have = false; bool any = false;
    for (size_t i = 0; i < csv.size(); ++i) {
        char ch = csv[i];
        if (ch >= '0' && ch <= '9') { v = v * 10u + (unsigned int)(ch - '0'); have = true; }
        else if (have) { out.insert(v); any = true; v = 0; have = false; }
    }
    if (have) { out.insert(v); any = true; }
    return any;
}

// Resolve the ownership ranks a session should hold for a given role.
//   fromEnv == true : ranks came from an explicit env override - preserve them.
//   fromEnv == false: use the role default (host owns {0}, join owns {1}).
// Safe to call repeatedly; on a role switch the default is recomputed so the
// client can never keep the host's rank (see the header note above).
inline void resolveOwnRanks(std::set<unsigned int>& ranks, bool isHost, bool fromEnv) {
    if (fromEnv) return;
    ranks.clear();
    ranks.insert(isHost ? 0u : 1u);
}

// Phase 3 Plan 03 (OWN-01/OWN-03): re-resolve to the deterministic
// rank=playerId default once a REAL network id is known (WELCOME assigns it
// on the join; the host's id is always 0). Overload, not a replacement - the
// (isHost,fromEnv) form above stays the pre-WELCOME default (Config.cpp's
// load-time call, when localId is not yet known). fromEnv preserves an
// explicit override exactly like the other overload.
inline void resolveOwnRanks(std::set<unsigned int>& ranks, u32 localId, bool fromEnv) {
    if (fromEnv) return;
    ranks.clear();
    ranks.insert(localId);
}

// Phase 3 (OWN-01/OWN-02, POC-01): the deterministic default rank->PlayerId
// ownership mapping - until a host-authoritative announcement overrides it
// (Plan 03's Replicator::setAllOwnRanks/allOwnRanks_), the owner of squad-tab
// rank R is simply PlayerId R (MAIN_GOAL.MD section 4). Pure and game-free
// (no GameWorld/engine dependency) so the no-game unit layer (prototest) can
// assert the bijection - each rank maps to exactly one PlayerId, and the
// {0,1,2,3} roster yields disjoint single-owner ranks - without linking the
// Replicator/game. Consumed by Replicator::publishOwned()
// (src/plugin/sync/ReplicatorPublish.cpp) to populate the per-tick
// handOwner_ map behind Replicator::ownerOfHand().
inline u32 ownerForRank(unsigned int rank) { return rank; }

// Phase 3 Plan 03: encode/decode a set of squad-tab ranks as a bitmask (bit R
// set means rank R is present) - the wire shape OwnRanksPacket's per-player
// rankMask field carries (Wire.h). Ranks fit comfortably in [0,31); MAX_PLAYERS
// (4) is far under that ceiling, and any out-of-range rank is silently
// dropped by ranksToMask rather than corrupting adjacent bits.
inline u32 ranksToMask(const std::set<unsigned int>& ranks) {
    u32 mask = 0;
    for (std::set<unsigned int>::const_iterator it = ranks.begin(); it != ranks.end(); ++it) {
        if (*it < 32u) mask |= (1u << *it);
    }
    return mask;
}

inline void maskToRanks(u32 mask, std::set<unsigned int>& out) {
    out.clear();
    for (unsigned int r = 0; r < 32u; ++r) {
        if (mask & (1u << r)) out.insert(r);
    }
}

} // namespace coop

#endif // COOP_OWN_RANKS_H
