#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "NetLink.h"
#include "SteamP2P.h"
#include "../CoopLog.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace coop {

namespace {
const int        TICK_MS       = 50; // 20 Hz service/transmit cadence
// Traffic-class channels (protocol 44). ENet guarantees ordering + reliable
// retransmit PER channel, so head-of-line blocking is per channel too. The bulk
// coordinated save/load transfer (multi-MB, dozens of ~4 KB reliable fragments)
// used to share CH_RELIABLE with the small latency-sensitive game events (doors,
// money, faction, ...); during a transfer those events stalled behind megabytes
// of save data. Moving the whole save/load handshake onto its own CH_BULK keeps
// its internal ordering intact while letting events flow on CH_RELIABLE. The
// receive ladder dispatches purely by packet TYPE, so the channel a packet
// arrives on is irrelevant there - only the sender and the host/connect channel
// count change.
const enet_uint8 CH_RELIABLE   = 0;  // handshake + small reliable game events
const enet_uint8 CH_UNRELIABLE = 1;  // entity batches / stealth / cam (newest supersedes)
const enet_uint8 CH_BULK       = 2;  // coordinated save/load transfer (bulk reliable)
const int        CH_COUNT      = 3;  // channels negotiated at host-create / connect

// KENSHICOOP_NET_ROSTER_TRACE: session-boundary roster trace (Phase 14, D-06).
// Same read-once static gate shape as ReplicatorAuthority.cpp's
// KENSHICOOP_DEBUG_CENSUS (commit 2c7ded9): with the variable unset this costs
// one cached int compare and emits ZERO new log lines, so a run that does not
// ask for the trace is byte-identical in output to a pre-Phase-14 run.
//
// Deliberately NOT a Config field. describeConfig builds the 'effective cfg'
// banner from Config, so a field there would change that banner on EVERY run
// and break Phase 14 criterion 4 ("a run that never invokes the lever is
// indistinguishable from a pre-phase run") by construction.
//
// Read only from resetSessionRoster(), i.e. from the two main-thread-exclusive
// windows (before CreateThread / after the worker join), so the non-atomic
// static needs no guard.
bool rosterTraceOn() {
    static int on = -1;
    if (on < 0) {
        const char* e = ::getenv("KENSHICOOP_NET_ROSTER_TRACE");
        // Anything but unset / empty / "0" is ON.
        on = (e && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
    }
    return on == 1;
}

// KENSHICOOP_NET_DIRTY_STOP: HARNESS-ONLY mutation lever. When set, the
// graceful ENet shutdown added for WINDOWS #19 / #22 is skipped, restoring the
// exact pre-fix teardown (enet_host_destroy with no enet_peer_disconnect) so
// the defect can be REPRODUCED on demand and the fix measured rather than
// asserted. Same read-once static gate shape as rosterTraceOn() above, and
// compiled out entirely in Release so no shipped build can take this path.
//
// Not a Config field, for the same reason rosterTraceOn() is not: describeConfig
// builds the 'effective cfg' banner from Config, and a field there would change
// that banner on every run (Phase 14 criterion 4).
#ifdef KENSHICOOP_HARNESS
bool dirtyStopOn() {
    static int on = -1;
    if (on < 0) {
        const char* e = ::getenv("KENSHICOOP_NET_DIRTY_STOP");
        // Anything but unset / empty / "0" is ON.
        on = (e && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
    }
    return on == 1;
}
#else
bool dirtyStopOn() { return false; }
#endif

// Net-thread diagnostics. OutputDebugStringA is thread-safe, and CoopLog guards
// its FILE* with a lock, so both are safe to call off the main thread.
void netLog(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logLine(buf);
}
void netErr(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ERROR: ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logErrLine(buf);
}

// Monotonic ms clock for the batch send stamp (v35). QPC, not GetTickCount:
// the receiver reconstructs snapshot SPACING from consecutive stamps, and
// GetTickCount's ~15 ms granularity would re-introduce the very quantization
// the stamp exists to remove (the Replicator's nowMs rationale).
u32 monoMs() {
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return (u32)GetTickCount();
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (u32)(((unsigned __int64)c.QuadPart * 1000ULL) /
                 (unsigned __int64)freq.QuadPart);
}

// The fixed-POD queueX methods are all the same MAIN-thread enqueue: take the
// publish lock, append one copied packet, release. This collapses that shared
// body so each queueX is a one-liner and the lock discipline lives in one place.
// (The variable-length queues - inv/world-item/census/save-file - keep custom
// bodies because they flatten a payload before the locked append.)
template<class T>
void pushLocked(CRITICAL_SECTION& cs, std::vector<T>& q, const T& v) {
    EnterCriticalSection(&cs);
    q.push_back(v);
    LeaveCriticalSection(&cs);
}
} // namespace

NetLink::NetLink()
    : isHost_(false), port_(0),
      enetHost_(0), serverPeer_(0), inbound_(0),
      outOwner_(0), outStampMs_(0), haveOut_(false),
      thread_(0), running_(0), stopFlag_(0), myId_(0),
      sendEpoch_(0),
      debugResendHello_(0),
      steamPeer_(0),
      simDelayMs_(0), simJitterMs_(0), simLossPct_(0) {
    InitializeCriticalSection(&outCs_);
}

NetLink::~NetLink() {
    stop();
    DeleteCriticalSection(&outCs_);
}

bool NetLink::startHost(int port, Inbound* inbound) {
    isHost_ = true; port_ = port; inbound_ = inbound; myId_ = 0;
    return launchThread();
}

bool NetLink::startClient(const std::string& ip, int port, Inbound* inbound) {
    isHost_ = false; ip_ = ip; port_ = port; inbound_ = inbound; myId_ = 0;
    return launchThread();
}

// Session-boundary reset - see the rationale block on the declaration in
// NetLink.h. Safe without a lock ONLY because both call sites run on the main
// thread with no worker alive (before CreateThread / after the join in stop()).
void NetLink::resetSessionRoster(const char* where) {
    // Session-boundary observation (Phase 14, 14-CONTEXT D-06). Emitted BEFORE
    // the clears below, because the whole point is WHAT was discarded: the ids
    // printed here are the ids a pre-9011527 binary would have carried into the
    // next session, where the lowest-free-slot scan would read them as occupied
    // and eventually refuse a legitimate rejoin with "MAX_PLAYERS=4 slots full".
    // Ids and counts only - PeerState::peer is dangling here by construction.
    if (rosterTraceOn()) {
        // MAX_PLAYERS is 4, so the id list is at most "1,2,3". The buffer is
        // sized well past that and truncates defensively rather than assuming
        // the cap can never move.
        char   ids[64];
        size_t used = 0;
        ids[0] = '\0';
        for (std::map<u32, PeerState>::const_iterator it = registry_.begin();
             it != registry_.end(); ++it) {
            char one[24];
            _snprintf(one, sizeof(one) - 1, "%s%u", (used == 0) ? "" : ",",
                      (unsigned)it->first);
            one[sizeof(one) - 1] = '\0';
            size_t n = strlen(one);
            if (used + n >= sizeof(ids) - 1) break; // truncate, never overrun
            memcpy(ids + used, one, n);
            used += n;
            ids[used] = '\0';
        }
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "roster-reset where=%s role=%s slots=%u ids={%s} epochs=%u",
                  (where && where[0]) ? where : "?",
                  isHost_ ? "host" : "client",
                  (unsigned)registry_.size(), ids,
                  (unsigned)epochSeen_.size());
        b[sizeof(b) - 1] = '\0';
        netLog(b);
    }
    // Drop the stale slot table BEFORE the pointers it holds can be reused.
    // Never touch PeerState::peer here: threadLoop()'s enet_host_destroy()
    // has already freed the peer array, so these pointers are dangling and
    // must be discarded, not reset/disconnected.
    registry_.clear();
    // Accepted-epoch bookkeeping is per-session too. The client path already
    // clears it on its own session boundary (the DISCONNECT branch, with the
    // rationale "all local epoch bookkeeping for this session is moot"); the
    // host path had no equivalent, so a restarted host would judge a
    // reconnecting player's first batches against the PREVIOUS session's
    // accepted epoch.
    epochSeen_.clear();
}

bool NetLink::launchThread() {
    if (enet_initialize() != 0) { netErr("enet_initialize failed"); return false; }
    // Every session starts with an empty roster, regardless of how the previous
    // one ended. This is the load-bearing call: stop() early-returns when
    // thread_ == 0, so a start that follows such a path would otherwise inherit
    // the old slot table.
    resetSessionRoster("launch");
    stopFlag_ = 0;
    thread_ = CreateThread(0, 0, &NetLink::threadEntry, this, 0, 0);
    if (thread_ == 0) { netErr("CreateThread failed"); enet_deinitialize(); return false; }
    return true;
}

void NetLink::stop() {
    if (thread_) {
        InterlockedExchange(&stopFlag_, 1);
        // The worker owns transport teardown (enet_host_destroy + the Steam hook
        // removal at the end of threadLoop). Deinitializing ENet or closing the
        // thread handle while the worker is still inside enet_host_service is a
        // use-after-free / double-free, so we MUST wait for it to fully exit
        // before tearing anything down. The loop services at TICK_MS (50 ms) and
        // checks stopFlag_ each pass, so a clean exit is prompt; if it somehow
        // stalls we keep waiting rather than pulling the rug out from under it.
        DWORD wr = WaitForSingleObject(thread_, 5000);
        if (wr != WAIT_OBJECT_0) {
            netErr("net worker still running after 5s; waiting for clean exit "
                   "before ENet teardown");
            WaitForSingleObject(thread_, INFINITE);
        }
        CloseHandle(thread_);
        thread_ = 0;
        enet_deinitialize();
    }
    // Unconditional and AFTER the join above: once stop() returns, no worker
    // owns these maps, and every ENetPeer* registry_ held was freed by
    // threadLoop()'s enet_host_destroy(). Clearing here means a stopped NetLink
    // never sits around holding dangling peer pointers, so a later
    // sendTo()/broadcast() cannot resurrect them. Idempotent, so a double stop()
    // (or stop() from ~NetLink after an explicit one) is harmless.
    resetSessionRoster("stop");
}

void NetLink::setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count) {
    EnterCriticalSection(&outCs_);
    outOwner_ = ownerId;
    if (arr && count > 0) out_.assign(arr, arr + count);
    else                  out_.clear();
    // Capture-time stamp (v35). The net thread may re-send this same snapshot
    // next tick; the unchanged stamp lets the receiver dedupe the re-send
    // instead of recording a phantom zero-velocity segment.
    outStampMs_ = monoMs();
    haveOut_ = true;
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueEvent(const EventPacket& ev) { pushLocked(outCs_, outEvents_, ev); }

void NetLink::debugResendHelloForTest() { InterlockedExchange(&debugResendHello_, 1); }

void NetLink::queueInvSnapshot(u32 ownerId, u8 keyKind, const u32 cKey[5],
                               const InvItemEntry* items, unsigned int count, u8 flags) {
    OutInv oi;
    oi.ownerId = ownerId;
    oi.keyKind = keyKind;
    oi.flags   = flags;
    for (int k = 0; k < 5; ++k) oi.cKey[k] = cKey[k];
    // Clamping here is itself a truncation: mark it so the receiver stays additive-only
    // (the caller normally sets the flag, but never let a silent clamp delete items).
    if (count > INV_ITEMS_MAX) { count = INV_ITEMS_MAX; oi.flags |= INV_FLAG_TRUNCATED; }
    if (items && count > 0) oi.items.assign(items, items + count);
    pushLocked(outCs_, outInv_, oi);
}

void NetLink::queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count) {
    OutWorldItems ow;
    ow.ownerId = ownerId;
    if (count > WORLD_ITEMS_MAX) count = WORLD_ITEMS_MAX;
    if (items && count > 0) ow.items.assign(items, items + count);
    pushLocked(outCs_, outWorldItems_, ow);
}

void NetLink::queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count) {
    OutWorldRemove ow;
    ow.ownerId = ownerId;
    if (count > 255) count = 255; // u8 count on the wire
    if (netIds && count > 0) ow.netIds.assign(netIds, netIds + count);
    pushLocked(outCs_, outWorldRemove_, ow);
}

void NetLink::queueWorldClaim(u32 ownerId, u32 authorId, const u32* netIds,
                              unsigned int count, u32 authorClaimMs) {
    OutWorldClaim ow;
    ow.ownerId = ownerId; ow.authorId = authorId; ow.authorClaimMs = authorClaimMs;
    if (count > 255) count = 255; // u8 count on the wire
    if (netIds && count > 0) ow.netIds.assign(netIds, netIds + count);
    pushLocked(outCs_, outWorldClaim_, ow);
}

void NetLink::queueClaimVerdict(const ClaimVerdictPacket& pkt) { pushLocked(outCs_, outClaimVerdict_, pkt); }

void NetLink::queueNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                             unsigned int count) {
    OutNpcCensus oc;
    oc.ownerId = ownerId;
    if (count > NPC_CENSUS_MAX) count = NPC_CENSUS_MAX;
    if (hands && count > 0) oc.hands.assign(hands, hands + count * 5);
    if (pos && count > 0) oc.pos.assign(pos, pos + count * 3);
    pushLocked(outCs_, outNpcCensus_, oc);
}

void NetLink::queueWorldDrop(const WorldDropPacket& pkt) { pushLocked(outCs_, outWorldDrops_, pkt); }

void NetLink::queueMedical(const MedicalPacket& pkt) { pushLocked(outCs_, outMedical_, pkt); }

void NetLink::queueTreatment(const TreatmentPacket& pkt) { pushLocked(outCs_, outTreatments_, pkt); }

void NetLink::queueCombatHit(const CombatHitPacket& pkt) { pushLocked(outCs_, outCombatHits_, pkt); }

void NetLink::queueSpeed(const SpeedPacket& pkt) { pushLocked(outCs_, outSpeed_, pkt); }

void NetLink::queueStats(const StatsPacket& pkt) { pushLocked(outCs_, outStats_, pkt); }

void NetLink::queueMoney(const MoneyPacket& pkt) { pushLocked(outCs_, outMoney_, pkt); }
void NetLink::queueMoneyDelta(const MoneyDeltaPacket& pkt) { pushLocked(outCs_, outMoneyDelta_, pkt); }
void NetLink::queueMoneyReject(const MoneyRejectPacket& pkt) { pushLocked(outCs_, outMoneyReject_, pkt); }

void NetLink::queueFaction(const FactionPacket& pkt) { pushLocked(outCs_, outFaction_, pkt); }

void NetLink::queueTime(const TimePacket& pkt) { pushLocked(outCs_, outTime_, pkt); }

void NetLink::queueDoor(const DoorPacket& pkt) { pushLocked(outCs_, outDoor_, pkt); }

void NetLink::queueProd(const ProdPacket& pkt) { pushLocked(outCs_, outProd_, pkt); }

void NetLink::queueResearch(const ResearchPacket& pkt) { pushLocked(outCs_, outResearch_, pkt); }

void NetLink::queueDeed(const DeedPacket& pkt) { pushLocked(outCs_, outDeed_, pkt); }

void NetLink::queueFixture(const FixturePacket& pkt) { pushLocked(outCs_, outFixture_, pkt); }

void NetLink::queueBuildPlace(const BuildPlacePacket& pkt) { pushLocked(outCs_, outBuildPlace_, pkt); }

void NetLink::queueBuildState(const BuildStatePacket& pkt) { pushLocked(outCs_, outBuildState_, pkt); }

void NetLink::queueBuildDoor(const BuildDoorPacket& pkt) { pushLocked(outCs_, outBuildDoor_, pkt); }

void NetLink::queueBuildRemove(const BuildRemovePacket& pkt) { pushLocked(outCs_, outBuildRemove_, pkt); }

void NetLink::queueStealth(const StealthPacket& pkt) { pushLocked(outCs_, outStealth_, pkt); }

void NetLink::queueCamHint(const CamHintPacket& pkt) { pushLocked(outCs_, outCamHint_, pkt); }

void NetLink::queueCellClaim(const CellClaimPacket& pkt) { pushLocked(outCs_, outCellClaim_, pkt); }
void NetLink::queueCellMap(const CellMapPacket& pkt) { pushLocked(outCs_, outCellMap_, pkt); }

void NetLink::queueSpawnReq(const SpawnReqPacket& pkt) { pushLocked(outCs_, outSpawnReq_, pkt); }

void NetLink::queueSpawnInfo(const SpawnInfoPacket& pkt) { pushLocked(outCs_, outSpawnInfo_, pkt); }

void NetLink::queueWorldPickup(const WorldPickupPacket& pkt) { pushLocked(outCs_, outWorldPickups_, pkt); }

void NetLink::queueInvXfer(const InvXferPacket& pkt) { pushLocked(outCs_, outInvXfers_, pkt); }

void NetLink::queueInvXferAck(const InvXferAckPacket& pkt) { pushLocked(outCs_, outInvXferAcks_, pkt); }

void NetLink::queueXferCommit(const XferCommitPacket& pkt) { pushLocked(outCs_, outXferCommit_, pkt); }

void NetLink::queueXferCommitAck(const XferCommitAckPacket& pkt) { pushLocked(outCs_, outXferCommitAck_, pkt); }

void NetLink::queueSaveReq(const SaveReqPacket& pkt) { pushLocked(outCs_, outSaveReq_, pkt); }

void NetLink::queueSaveBegin(const SaveBeginPacket& pkt, u32 destId) {
    OutSaveBegin ob; ob.pkt = pkt; ob.destId = destId;
    pushLocked(outCs_, outSaveBegin_, ob);
}

void NetLink::queueSaveFile(const SaveFileHeader& hdr, const char* relPath,
                            const unsigned char* data, unsigned int dataLen,
                            u32 destId) {
    OutSaveFile of;
    of.hdr = hdr;
    of.tail.reserve(hdr.pathLen + dataLen);
    of.tail.assign(relPath, relPath + hdr.pathLen);
    if (data && dataLen > 0) of.tail.insert(of.tail.end(), data, data + dataLen);
    of.destId = destId;
    pushLocked(outCs_, outSaveFile_, of);
}

void NetLink::queueSaveDone(const SaveDoneHeader& hdr, const u32* crcs,
                            unsigned int count, u32 destId) {
    OutSaveDone od;
    od.hdr = hdr;
    if (crcs && count > 0) od.crcs.assign(crcs, crcs + count);
    od.destId = destId;
    pushLocked(outCs_, outSaveDone_, od);
}

void NetLink::queueSaveAck(const SaveAckPacket& pkt) { pushLocked(outCs_, outSaveAck_, pkt); }

void NetLink::queueLoadGo(const LoadGoPacket& pkt, u32 destId) {
    OutLoadGo og; og.pkt = pkt; og.destId = destId;
    pushLocked(outCs_, outLoadGo_, og);
}

void NetLink::queueLoadReq(const LoadReqPacket& pkt) { pushLocked(outCs_, outLoadReq_, pkt); }

void NetLink::queueLoadNack(const LoadNackPacket& pkt) { pushLocked(outCs_, outLoadNack_, pkt); }

void NetLink::queueLoadAck(const LoadAckPacket& pkt) { pushLocked(outCs_, outLoadAck_, pkt); }

void NetLink::queueCoordReject(const CoordRejectPacket& pkt) { pushLocked(outCs_, outCoordReject_, pkt); }

void NetLink::kickPeer(u32 playerId) { pushLocked(outCs_, outKick_, playerId); }

void NetLink::broadcastOwnRanks(const OwnRanksPacket& pkt) { pushLocked(outCs_, outOwnRanks_, pkt); }

// Host-side send primitives (Phase 2). NET-thread-only, built directly on the
// registry above and the same enet_peer_send/enet_host_broadcast calls every
// outbound drain already uses - see the two-branch shape repeated ~40 times
// below in threadLoop(). Not migrated into any existing drain this plan
// (Phase 3 scope); this is the primitive layer only.
void NetLink::sendTo(u32 playerId, ENetPacket* pkt, int channel) {
    std::map<u32, PeerState>::iterator it = registry_.find(playerId);
    if (it != registry_.end() && it->second.peer &&
        it->second.peer->state == ENET_PEER_STATE_CONNECTED) {
        enet_peer_send(it->second.peer, (enet_uint8)channel, pkt);
    } else {
        enet_packet_destroy(pkt);
    }
}

void NetLink::broadcast(ENetPacket* pkt, int channel) {
    if (isHost_) {
        enet_host_broadcast(enetHost_, (enet_uint8)channel, pkt);
    } else {
        // Host-only concept; a client calling this would misroute the packet
        // to the single server peer under a name that promises "everyone".
        enet_packet_destroy(pkt);
    }
}

void NetLink::broadcastExcept(u32 playerId, ENetPacket* pkt, int channel) {
    unsigned sentCount = 0;
    for (std::map<u32, PeerState>::iterator it = registry_.begin();
         it != registry_.end(); ++it) {
        if (it->first == playerId) continue;
        if (it->second.peer && it->second.peer->state == ENET_PEER_STATE_CONNECTED) {
            // Same ENetPacket* reused across sends - ENet refcounts it
            // internally, so this is the documented-safe fan-out pattern
            // (never enet_host_broadcast-then-"unsend"; ENet has no such
            // retraction).
            enet_peer_send(it->second.peer, (enet_uint8)channel, pkt);
            ++sentCount;
        }
    }
    if (sentCount == 0) enet_packet_destroy(pkt);
}

// NET thread (Phase 3): routing-class lookup, keyed by docs/ROUTING_MATRIX.md's
// Class column. Pure and game-free - a switch over PacketType, no I/O, safe to
// call from any thread (nettest's Task 3 drift oracle calls it directly with no
// NetLink instance). Every case below cites the matrix row(s) it implements by
// packet NAME - the matrix's row NUMBERS are its own thematic ordering, not the
// PacketType enum's numeric value, so names (not numbers) are the source of truth.
RelayClass NetLink::routingClassOf(u8 packetType) {
    switch (packetType) {
    // Class A (broadcast-authoritative): join-authored state must reach every
    // OTHER connected client, ownerId unchanged, never echoed to the author.
    case PKT_ENTITY_BATCH:
    case PKT_EVENT:
    case PKT_INV_SNAPSHOT:
    case PKT_WORLD_ITEM:
    case PKT_WORLD_ITEM_REMOVE:
    case PKT_WORLD_DROP:
    case PKT_WORLD_PICKUP:
    case PKT_MEDICAL:
    case PKT_STATS:
    case PKT_DOOR:
    case PKT_BUILD_PLACE:
    case PKT_BUILD_STATE:
    case PKT_BUILD_DOOR:
    case PKT_BUILD_REMOVE:
    case PKT_CAM_HINT:
    case PKT_FIXTURE:
    // Phase 8 Plan 02 Task 3 (WORLD-03): PKT_NPC_CENSUS moved HERE (Class A,
    // RELAY_BROADCAST_EXCEPT) out of Class B (host-only) - the per-owner
    // census intake (map<ownerId,CensusSet>) means a join's own census is
    // now safe to relay to OTHER joins: no wire change (NpcCensusHeader
    // already carries ownerId), and rejectIfForgedOwner on the receive
    // branch guards a forged census owner exactly like every other Class A
    // row. The host's own census still reaches everyone the same way it
    // always did (broadcastExcept with the host as sourcePlayerId reaches
    // every OTHER connected client, which for the host IS everyone).
    case PKT_NPC_CENSUS:
    // Phase 6 (GAP-1/GAP-2): PKT_TREATMENT and PKT_STEALTH were FAILSAFE
    // because neither wire struct carries a destination PlayerId (Phase 3
    // rationale below). Both are now proven Class A instead: broadcast-except
    // fan-out reaches every non-author receiver, and correctness holds
    // because every receiver applies ONLY to bodies it authors - the
    // treatment authority guard (ReplicatorChannels.cpp applyTreatments,
    // "own hand" skip) and the stealth ownHands_ apply guard
    // (ReplicatorChannels.cpp applyStealthFeedback) both already ignore a
    // relayed packet whose target hand isn't theirs. A non-authority
    // receiver's ignore is a no-op, not a correctness risk - the same
    // pattern PKT_MEDICAL/PKT_STATS already rely on above. No wire change;
    // rejectIfForgedOwner() already authenticates ownerId == sourcePlayerId
    // for every Class A relay.
    case PKT_TREATMENT:
    case PKT_STEALTH:
        return RELAY_BROADCAST_EXCEPT;

    // Class D already carrying a routable destination PlayerId field on the
    // wire (InvXferAckPacket::xferOwnerId) - no game-thread hand-ownership
    // lookup needed; a same-day sendTo() swap. (PKT_WORLD_ITEM_CLAIM moved OUT
    // of this group in Phase 7 Plan 02 - see below.)
    case PKT_INV_XFER_ACK:
        return RELAY_UNICAST;

    // Class D/E whose destination cannot be resolved on the net thread this
    // phase - either it needs a game-thread hand->owner lookup, or the wire
    // struct carries no destination-player field at all (PKT_SPAWN_INFO, the
    // PKT_SAVE_BEGIN/FILE/DONE save-transfer group - RESEARCH.md Pitfall 4).
    // Never blind-broadcast a targeted/host-only packet to route around the
    // missing unicast (Security note: a leaked Class D/E is information
    // disclosure) - log and drop instead. (PKT_STEALTH/PKT_TREATMENT moved to
    // Class A above, Phase 6 GAP-1/GAP-2; PKT_INV_XFER moved OUT of this
    // group in Phase 7 Plan 01 - see below.)
    case PKT_SPAWN_INFO:
    case PKT_SAVE_BEGIN:
    case PKT_SAVE_FILE:
    case PKT_SAVE_DONE:
        return RELAY_FAILSAFE_LOG;

    // Everything else never relays through the host's dispatch: Class B
    // (host-broadcast, e.g. PKT_MONEY/PKT_PROD/PKT_LOAD_GO/PKT_PLAYER_JOINED/
    // PKT_PLAYER_LEFT/PKT_OWN_RANKS - already correct via enet_host_broadcast
    // or the dedicated sendTo/broadcastExcept roster call sites), Class C
    // (client-request, e.g. PKT_TIME_PING/PKT_SPEED_REQ/PKT_SAVE_REQ/
    // PKT_COMBAT_HIT - terminates at the host by definition), PKT_TIME (the
    // B/C role-split channel - neither branch relays), Class F handshake
    // (PKT_HELLO/PKT_WELCOME - already targeted inline against ev.peer at
    // connect time), PKT_TIME_PONG (Class D, but ALREADY correctly unicast
    // inline via enet_peer_send(ev.peer,...) - ROUTING_MATRIX.md row 12's
    // stale "needs sendTo()" claim is corrected by this phase's doc update,
    // not by a code change here - RESEARCH.md Pitfall 5), and PKT_LEAVE (the
    // N/A sentinel, never actually sent on the wire).
    //
    // Phase 7 Plan 01 (protocol 58): PKT_INV_XFER moved HERE (RELAY_NONE) out
    // of the RELAY_FAILSAFE_LOG group above - it now carries srcOwnerId/
    // dstOwnerId (resolved on the AUTHOR's game thread), which closes the
    // exact gap that used to force FAILSAFE: the intent is a host-terminated
    // CLIENT-REQUEST (Class C shape, like PKT_SPEED_REQ), so it falls through
    // to this default branch - the host consumes it (after
    // rejectIfForgedOwner on the receive branch) and never relays the raw
    // intent to any other client. PKT_XFER_COMMIT (host's single broadcast
    // verdict, Class B - authored via queueXferCommit's isHost_ branch, same
    // shape as broadcastOwnRanks, never received-and-relayed through this
    // switch) and PKT_XFER_COMMIT_ACK (participant -> host bookkeeping,
    // Class C shape) also land here by omission, matching every other
    // Class B/C packet's "no explicit case needed" convention above.
    //
    // Phase 7 Plan 02 (protocol 58): PKT_WORLD_ITEM_CLAIM moved HERE
    // (RELAY_NONE) out of the RELAY_UNICAST group above - it is now the claim
    // INTENT (claimant -> host, Class C shape, same host-terminated
    // reasoning as PKT_INV_XFER just above), carrying authorClaimMs so the
    // host's ClaimArbiter.h contention window can map it. The receive branch
    // calls rejectIfForgedOwner then pushes to the host's own arbiter -
    // never relayed to the item's author or any other client anymore.
    // PKT_CLAIM_VERDICT (host's single broadcast winner, Class B - authored
    // via queueClaimVerdict's isHost_ branch, same shape as
    // broadcastOwnRanks/queueXferCommit) also lands here by omission.
    //
    // Phase 8 (WORLD-01/WORLD-02, protocol 58 - NO wire change): PKT_FACTION
    // and PKT_DEED moved HERE (RELAY_NONE) out of the Class A block above.
    // They are LOCKED global world facts (faction diplomacy, property
    // ownership), not per-hand symmetric state like doors - the "joins never
    // mutate global facts directly" decision means a join's row is now an
    // INTENT (host-terminated CLIENT-REQUEST, the exact PKT_INV_XFER/
    // PKT_WORLD_ITEM_CLAIM precedent from Phase 7), never relayed to any
    // other client. The wire struct is unchanged (no new field, no PROTOCOL_
    // VERSION bump) - only the routing class and the host's apply-side echo
    // guard change (ReplicatorChannels.cpp applyFactions/applyDeeds): the
    // HOST applies the write but does NOT update its own publish baseline
    // for a received row, so its own publishFactions/publishDeeds detects the
    // change and RE-EMITS the committed fact under the host's own ownerId/
    // seq - every join converges to the host's value. This also kills the
    // cross-sender seq collision on these two channels as a side effect
    // (after conversion only the host ever authors the rows a join applies).
    // The receive branches gain rejectIfForgedOwner (the check relayDispatch
    // used to perform for a Class A relay, now bypassed for RELAY_NONE).
    //
    // Phase 8 Plan 02 (protocol 59, WORLD-03): PKT_CELL_CLAIM moved HERE
    // (RELAY_NONE) out of the Class A block above - it is now a
    // host-terminated INTENT (every instance still PUBLISHES its own claims,
    // but only the host folds them into claimSlots_ and runs the reduce),
    // the exact PKT_INV_XFER/PKT_WORLD_ITEM_CLAIM precedent. The receive
    // branch calls rejectIfForgedOwner then pushes to the host's own intake
    // - never relayed to any other client anymore (a join no longer needs
    // to see another join's raw claim; it only ever adopts the host's
    // reduced map). PKT_CELL_MAP (the host's single broadcast verdict,
    // Class B - authored via queueCellMap's isHost_ branch, same shape as
    // queueClaimVerdict/queueXferCommit) also lands here by omission.
    //
    // Phase 9 Plan 01 (protocol 60, CONS-01): PKT_MONEY_REJECT (the host's
    // single insufficient-funds verdict broadcast, Class B - authored via
    // queueMoneyReject's isHost_ branch, same shape as
    // queueClaimVerdict/queueCellMap/queueXferCommit) also lands here by
    // omission - never received-and-relayed through this switch.
    //
    // Phase 10 Plan 01 (protocol 61, SAVE-02/SAVE-03): PKT_LOAD_ACK (join ->
    // host, Class C client-request shape like PKT_SAVE_ACK - terminates at
    // the host) and PKT_COORD_REJECT (host -> the rejected requester ONLY,
    // Class D unicast authored via queueCoordReject's isHost_ branch -
    // sendTo, never enet_host_broadcast) both land here by omission - neither
    // is ever received-and-relayed through this switch.
    default:
        return RELAY_NONE;
    }
}

// NET thread, host-only (Phase 3 CR-01 fix): see the declaration doc comment
// in NetLink.h for the full contract. Factored out of relayDispatch()'s
// RELAY_BROADCAST_EXCEPT/RELAY_UNICAST reject branches (below) so every
// Class A/E receive branch can run the SAME ownerId-vs-domain check BEFORE
// its own local-apply push, not just before the relay. relayDispatch() keeps
// its own copy of this check as defense in depth - a forged packet that
// somehow slipped past this pre-check would still be rejected there rather
// than relayed to a third client.
bool NetLink::rejectIfForgedOwner(u32 sourcePlayerId, u32 claimedOwnerId) {
    if (!isHost_) return false; // a client only ever applies host-authored payloads
    if (claimedOwnerId == sourcePlayerId) return false; // legitimate: author owns its own domain
    char b[96];
    _snprintf(b, sizeof(b) - 1, "relay REJECT player=%u claimed owner=%u",
              (unsigned)sourcePlayerId, (unsigned)claimedOwnerId);
    b[sizeof(b) - 1] = '\0';
    netErr(b);
    return true;
}

// NET thread, host-only (Phase 3): see the declaration doc comment in
// NetLink.h for the full contract. Extracted from this plan's Task 1 tracer
// (the RELAY_BROADCAST_EXCEPT branch below is byte-for-byte what Task 1 proved
// end-to-end for PKT_ENTITY_BATCH) and generalized to every routing class.
void NetLink::relayDispatch(u8 packetType, u32 sourcePlayerId, u32 claimedOwnerId,
                             u32 destId, const ENetEvent& ev) {
    if (!isHost_) return;
    const unsigned len = (unsigned)ev.packet->dataLength;
    switch (routingClassOf(packetType)) {
    case RELAY_BROADCAST_EXCEPT:
        if (claimedOwnerId == sourcePlayerId) {
            // Fresh copy, never ev.packet itself: the single unconditional
            // enet_packet_destroy(ev.packet) at the end of the receive case
            // runs for every branch, and broadcastExcept relinquishes
            // ownership of whatever it's handed - passing ev.packet itself
            // would double-free it (STRIDE T-03-02).
            ENetPacket* copy = enet_packet_create(ev.packet->data, len, ev.packet->flags);
            broadcastExcept(sourcePlayerId, copy, (int)ev.channelID);
        } else {
            char b[96];
            _snprintf(b, sizeof(b) - 1, "relay REJECT player=%u claimed owner=%u",
                      (unsigned)sourcePlayerId, (unsigned)claimedOwnerId);
            b[sizeof(b) - 1] = '\0';
            netErr(b);
        }
        break;
    case RELAY_UNICAST:
        // Same ownerId-vs-sourcePlayerId validation as Class A (Security
        // note): a client must not be able to publish another player's
        // ownership domain through a targeted channel either.
        if (claimedOwnerId == sourcePlayerId) {
            ENetPacket* copy = enet_packet_create(ev.packet->data, len, ev.packet->flags);
            sendTo(destId, copy, (int)ev.channelID);
        } else {
            char b[96];
            _snprintf(b, sizeof(b) - 1, "relay REJECT player=%u claimed owner=%u",
                      (unsigned)sourcePlayerId, (unsigned)claimedOwnerId);
            b[sizeof(b) - 1] = '\0';
            netErr(b);
        }
        break;
    case RELAY_FAILSAFE_LOG: {
        char b[80];
        _snprintf(b, sizeof(b) - 1, "relay FAILSAFE no-relay type=%u player=%u",
                  (unsigned)packetType, (unsigned)sourcePlayerId);
        b[sizeof(b) - 1] = '\0';
        netErr(b);
        break;
    }
    case RELAY_NONE:
    default:
        break; // no-op: this class never relays through the host's dispatch.
    }
}

void NetLink::setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct) {
    simDelayMs_  = delayMs;
    simJitterMs_ = jitterMs;
    simLossPct_  = (lossPct > 100) ? 100 : lossPct;
}

void NetLink::setSteamTransport(unsigned long long peerSteamId) {
    steamPeer_ = peerSteamId;
}

// Deliver one received entity to the game thread, applying the WAN sim if enabled.
// Loopback has ~0 latency, so without this a received "down"/move lands the same
// frame it was sent - exactly the regime we want to stop relying on. With it, the
// entity is parked until base +/- jitter has elapsed (and dropped lossPct% of the
// time), so the join must coast on interpolation/local enforcement between arrivals.
// MAIN thread: advance the session epoch so post-reset batches supersede any
// still-in-flight batch from the prior session, and drop the pending owned-
// entity snapshot so the net thread does not re-broadcast a stale (old-world)
// snapshot stamped with the NEW epoch (which the peer would then accept as
// fresh). The main thread republishes a real snapshot within a tick or two.
void NetLink::bumpSessionEpoch() {
    InterlockedIncrement(&sendEpoch_);
    EnterCriticalSection(&outCs_);
    out_.clear();
    haveOut_ = false;
    LeaveCriticalSection(&outCs_);
}

// NET thread: monotonic per-owner epoch gate. Latest-wins on the unreliable
// motion stream, so "accept if >= newest seen" both admits a fresh session
// (higher epoch) and rejects a delayed prior-session batch (lower epoch).
bool NetLink::acceptEpoch(u32 ownerId, u32 epoch) {
    std::map<u32, u32>::iterator it = epochSeen_.find(ownerId);
    if (it == epochSeen_.end()) { epochSeen_[ownerId] = epoch; return true; }
    if (epoch < it->second) return false;
    it->second = epoch;
    return true;
}

void NetLink::deliverEntity(u32 ownerId, u32 sendMs, const EntityState& e) {
    if (simDelayMs_ == 0 && simJitterMs_ == 0 && simLossPct_ == 0) {
        if (inbound_) inbound_->pushEntity(ownerId, sendMs, e);
        return;
    }
    if (simLossPct_ > 0 && (unsigned)(rand() % 100) < simLossPct_) return; // dropped
    int jitter = 0;
    if (simJitterMs_ > 0) jitter = (rand() % (int)(2 * simJitterMs_ + 1)) - (int)simJitterMs_;
    int delay = (int)simDelayMs_ + jitter;
    if (delay < 0) delay = 0;
    Delayed d;
    d.releaseTick = GetTickCount() + (DWORD)delay;
    d.ownerId     = ownerId;
    d.sendMs      = sendMs;
    d.e           = e;
    delayed_.push_back(d);
}

// Release every held entity whose simulated arrival time has passed. Jitter can put
// release times out of order; we scan the whole queue (small N) and keep the rest.
// "Newest supersedes" on the receiver makes any reordering harmless (and realistic).
void NetLink::flushDelayed() {
    if (delayed_.empty()) return;
    DWORD now = GetTickCount();
    std::deque<Delayed> keep;
    for (std::deque<Delayed>::iterator it = delayed_.begin(); it != delayed_.end(); ++it) {
        if ((long)(now - it->releaseTick) >= 0) { if (inbound_) inbound_->pushEntity(it->ownerId, it->sendMs, it->e); }
        else                                     { keep.push_back(*it); }
    }
    delayed_.swap(keep);
}

DWORD WINAPI NetLink::threadEntry(LPVOID self) {
    reinterpret_cast<NetLink*>(self)->threadLoop();
    return 0;
}

// Graceful transport shutdown (WINDOWS #19 / #22). NET THREAD ONLY, and only
// from the tail of threadLoop(): every ENetPeer* touched here is still owned by
// enetHost_, which has not been destroyed yet, so nothing in this function ever
// dereferences the dangling pointers resetSessionRoster() exists to discard.
// The ordering contract is the whole safety argument:
//
//   threadLoop loop exits
//     -> shutdownPeersGracefully()   <- peers ALIVE, wire still open (here)
//     -> enet_host_destroy()         <- peer array freed, every pointer dangles
//     -> stop() joins the worker
//     -> resetSessionRoster("stop")  <- discards the now-dangling registry_
//
// Commit 9011527's invariant is untouched: this function never stores a peer
// pointer, never clears registry_, and never runs after the destroy.
void NetLink::shutdownPeersGracefully() {
    if (!enetHost_) return;
    if (dirtyStopOn()) {
        netLog("graceful-stop SKIPPED (KENSHICOOP_NET_DIRTY_STOP)");
        return;
    }

    // Collect the peers worth telling. A host has N clients in registry_; a
    // client has exactly one server peer. Peers that are not CONNECTED (still
    // handshaking, already gone, rejected pre-HELLO) have nothing to acknowledge
    // and are left to enet_host_destroy().
    ENetPeer* pending[MAX_PLAYERS];
    unsigned  nPending = 0;
    if (isHost_) {
        for (std::map<u32, PeerState>::iterator it = registry_.begin();
             it != registry_.end() && nPending < MAX_PLAYERS; ++it) {
            ENetPeer* p = it->second.peer;
            if (p && p->state == ENET_PEER_STATE_CONNECTED) pending[nPending++] = p;
        }
    } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
        pending[nPending++] = serverPeer_;
    }

    if (nPending == 0) {
        if (rosterTraceOn()) {
            char b[96];
            _snprintf(b, sizeof(b) - 1, "graceful-stop role=%s peers=0 acked=0 waitedMs=0",
                      isHost_ ? "host" : "client");
            b[sizeof(b) - 1] = '\0';
            netLog(b);
        }
        return;
    }

    for (unsigned i = 0; i < nPending; ++i) enet_peer_disconnect(pending[i], 0);

    // Bounded drain so the queued disconnect command is actually SENT and, where
    // the remote end is reachable, acknowledged. Unbounded would hang the F2
    // panel; stop() already waits up to 5000 ms for this thread, so the budget
    // has to stay far below that. 300 ms covers a LAN round trip many times over
    // and is invisible next to the ~4 ms reconnect it protects.
    const unsigned SHUTDOWN_BUDGET_MS = 300;
    const unsigned SHUTDOWN_SLICE_MS  = 20;
    unsigned waited = 0;
    unsigned acked  = 0;
    while (waited < SHUTDOWN_BUDGET_MS && acked < nPending) {
        ENetEvent ev;
        int rc = enet_host_service(enetHost_, &ev, SHUTDOWN_SLICE_MS);
        waited += SHUTDOWN_SLICE_MS;
        if (rc < 0) break;
        if (rc == 0) continue;
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            // The session is over; nothing here may reach the game thread.
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            // Deliberately NOT the full per-player teardown: registry_ and
            // epochSeen_ are about to be discarded wholesale by
            // resetSessionRoster("stop"), and pushLeave()ing a departure the
            // game thread will never act on would be noise. Clearing ->data
            // keeps the id off a peer slot that is about to be freed.
            ev.peer->data = 0;
            ++acked;
        }
    }

    // Anything still unacknowledged inside the budget gets one last-ditch
    // immediate disconnect: enet_peer_disconnect_now() puts the command on the
    // wire without waiting for a reply, which is strictly better than the
    // pre-fix silence even when the remote end never answers.
    if (acked < nPending) {
        for (unsigned i = 0; i < nPending; ++i) {
            if (pending[i]->state != ENET_PEER_STATE_DISCONNECTED)
                enet_peer_disconnect_now(pending[i], 0);
        }
        enet_host_flush(enetHost_);
    }

    if (rosterTraceOn()) {
        char b[112];
        _snprintf(b, sizeof(b) - 1,
                  "graceful-stop role=%s peers=%u acked=%u waitedMs=%u",
                  isHost_ ? "host" : "client",
                  (unsigned)nPending, (unsigned)acked, (unsigned)waited);
        b[sizeof(b) - 1] = '\0';
        netLog(b);
    }
}

void NetLink::threadLoop() {
    InterlockedExchange(&running_, 1);

    // Steam P2P transport: redirect ENet's socket layer through the Steam tunnel
    // BEFORE the host is created (the fake socket is handed out at create time).
    // The tunnel is addressless; ENet still needs an ENetAddress for its peer
    // routing, so both sides use the fabricated "1.0.0.1:port".
    const bool steam = (steamPeer_ != 0);
    if (steam) {
        if (steamp2p::installEnetHooks(port_)) {
            netLog("transport=steam (ENet tunnelled over Steam P2P)");
        } else {
            netErr("steam transport requested but hooks failed; falling back to UDP");
        }
    }

    if (isHost_) {
        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = (enet_uint16)port_;
        enetHost_ = enet_host_create(&addr, 8 /*peers*/, CH_COUNT /*channels*/, 0, 0);
        if (!enetHost_) { netErr("host create failed"); InterlockedExchange(&running_, 0); return; }
        netLog("hosting");
    } else {
        enetHost_ = enet_host_create(0, 1, CH_COUNT, 0, 0);
        if (!enetHost_) { netErr("client create failed"); InterlockedExchange(&running_, 0); return; }
        ENetAddress addr;
        if (steam) enet_address_set_host_ip(&addr, "1.0.0.1");
        else       enet_address_set_host(&addr, ip_.c_str());
        addr.port = (enet_uint16)port_;
        serverPeer_ = enet_host_connect(enetHost_, &addr, CH_COUNT, 0);
        if (!serverPeer_) netErr("connect failed");
        else              netLog("connecting");
    }

    // Steam's unreliable P2P packets cap at 1200 bytes; clamp the MTU so every
    // ENet datagram (including fragments of large reliable packets) fits one
    // P2P packet. Set before any peer negotiates its own MTU from ours.
    if (steam && enetHost_ && enetHost_->mtu > 1200) {
        enetHost_->mtu = 1200;
        if (serverPeer_) serverPeer_->mtu = 1200;
    }

    DWORD lastConnectAttempt = GetTickCount();

    // Wall-clock time-sync state (client only). The join pings every ~2 s; each
    // pong yields an (rtt, offset) sample; the minimum-RTT sample wins (NTP
    // filter). CLOCKSYNC is logged every ~5 s so the oracles can align this
    // log's timestamps into the host clock frame.
    DWORD         lastTimePing  = 0;
    DWORD         lastClockLog  = 0;
    u32           pingNonce     = 1;
    unsigned long bestRttMs     = 0xFFFFFFFFul;
    long          bestOffsetMs  = 0;
    unsigned int  syncSamples   = 0;
    const long    HALF_DAY_MS   = 12l * 3600l * 1000l;

    while (!stopFlag_) {
        // Steam heartbeat: spike ping/echo + session-state change logging.
        // Cheap no-op when Steam isn't initialised (pure-UDP sessions).
        steamp2p::tick();

        // Client reconnect: if we have no live connection, retry every 2 s so
        // dropping/relaunching the host re-establishes.
        if (!isHost_) {
            bool disconnected =
                (serverPeer_ == 0) ||
                (serverPeer_->state == ENET_PEER_STATE_DISCONNECTED) ||
                (serverPeer_->state == ENET_PEER_STATE_ZOMBIE);
            DWORD now = GetTickCount();
            if (disconnected && (now - lastConnectAttempt) >= 2000) {
                lastConnectAttempt = now;
                if (serverPeer_) { enet_peer_reset(serverPeer_); serverPeer_ = 0; }
                ENetAddress addr;
                if (steam) enet_address_set_host_ip(&addr, "1.0.0.1");
                else       enet_address_set_host(&addr, ip_.c_str());
                addr.port = (enet_uint16)port_;
                serverPeer_ = enet_host_connect(enetHost_, &addr, CH_COUNT, 0);
                if (steam && serverPeer_) serverPeer_->mtu = 1200;
                netLog(serverPeer_ ? "reconnecting" : "reconnect failed");
            }
        }

        ENetEvent ev;
        while (enet_host_service(enetHost_, &ev, TICK_MS) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    if (isHost_) {
                        // Do NOT blanket-clear epochSeen_ here (Phase 2 Plan 03):
                        // this event fires for EVERY connecting peer, including a
                        // 3rd/4th client joining an already-populated host, and a
                        // blanket .clear() here used to wipe players 1/2's
                        // already-accepted epoch state the instant a 3rd peer's
                        // ENet handshake completed - before that peer's own id is
                        // even known (TWO_PLAYER_ASSUMPTIONS finding 8's global-
                        // wipe bug class, RESEARCH.md Pitfall 2). The per-player
                        // reset for THIS connecting peer happens once its id is
                        // assigned below (epochSeen_.erase(id) in the HELLO
                        // success branch), touching only its own stale epoch
                        // entry - never another connected player's.
                        //
                        // Wait for the client's HELLO before assigning an id, so
                        // a version mismatch is rejected before we admit it.
                        netLog("peer connecting (awaiting HELLO)");
                    } else {
                        // Client: a fresh connection to the host restarts our own
                        // epoch bookkeeping. A client's registry is exactly the
                        // one link to the host, so a blanket reset here is
                        // correct - not the finding-8 global-wipe bug class,
                        // which is specific to the HOST fanning one connect event
                        // out across many already-connected players.
                        epochSeen_.clear();
                        // Introduce ourselves with our protocol version.
                        HelloPacket h;
                        h.type = (u8)PKT_HELLO; h.version = PROTOCOL_VERSION; h.nameLen = 0;
                        ENetPacket* out = enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE);
                        enet_peer_send(ev.peer, CH_RELIABLE, out);
                        netLog("connected to host; sent HELLO");
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const u8 type = packetType(ev.packet->data, (unsigned)ev.packet->dataLength);
                    // Sender-identity resolution (Phase 2, NET-04): the host's only
                    // AUTHORITATIVE knowledge of who sent this packet is the connection
                    // itself - ev.peer->data, assigned by the host at HELLO/WELCOME time
                    // (see the connect-handler registry insert below) - never the
                    // packet's own self-reported ownerId field, which every receive
                    // branch below still reads from the payload unchanged this phase.
                    // Resolved once here, before the per-type dispatch, so it is
                    // available to every branch; not yet used to reject/rewrite a
                    // mismatched payload ownerId (that is Phase 3's relay-boundary
                    // work per docs/ROUTING_MATRIX.md's Security note) - this phase's
                    // job is only to make the true sender EXIST and be OBSERVABLE on
                    // every receive, closing the "only known at disconnect" gap.
                    // Excluded from the HELLO branch: ev.peer->data is not assigned
                    // yet for a still-connecting peer, so it would only ever read 0.
                    u32 sourcePlayerId = (u32)(size_t)ev.peer->data;
                    if (isHost_ && type != PKT_HELLO) {
                        char sb[64];
                        _snprintf(sb, sizeof(sb) - 1, "recv player=%u type=%u",
                                  (unsigned)sourcePlayerId, (unsigned)type);
                        sb[sizeof(sb) - 1] = '\0';
                        netLog(sb);
                    }
                    if (isHost_ && type == PKT_HELLO) {
                        HelloPacket h;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &h)) {
                            if (ev.peer->data != 0) {
                                // Duplicate HELLO on an already-registered connection
                                // (CR-02): ev.peer->data is only ever non-zero once the
                                // free-slot scan below has assigned this peer a real id
                                // (ids start at 1; a still-connecting peer reads its
                                // zero-initialized default). Re-running the scan here
                                // would mint a SECOND id for the same ENetPeer* and
                                // orphan the slot it already holds - permanently, since
                                // the disconnect handler only ever erases
                                // registry_.find(ev.peer->data), which by then reads the
                                // new id, not the old one. Treat the resend as a no-op:
                                // do not re-scan, re-insert, or re-broadcast anything.
                                char b[96];
                                _snprintf(b, sizeof(b) - 1,
                                          "duplicate HELLO from already-registered id=%u; ignored",
                                          (unsigned)(size_t)ev.peer->data);
                                b[sizeof(b) - 1] = '\0';
                                netLog(b);
                            } else if (h.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: peer v%u, ours v%u; rejecting",
                                          (unsigned)h.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                                enet_peer_disconnect(ev.peer, 0);
                            } else {
                                // Registry-backed lowest-free-slot assignment (Phase 2):
                                // PlayerId 0 is reserved for the host; scan [1, MAX_PLAYERS)
                                // for the first id not currently held in registry_. Replaces
                                // the old nextId++ counter + "admit anyway, warn" guard
                                // (TWO_PLAYER_ASSUMPTIONS finding 6) with a real
                                // access-control boundary: a connect beyond the cap is
                                // cleanly rejected instead of silently desyncing.
                                u32  id    = 0;
                                bool found = false;
                                for (u32 cand = 1; cand < MAX_PLAYERS; ++cand) {
                                    if (registry_.find(cand) == registry_.end()) {
                                        id = cand;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    char b[128];
                                    _snprintf(b, sizeof(b) - 1,
                                              "peer rejected: MAX_PLAYERS=%u slots full",
                                              (unsigned)MAX_PLAYERS);
                                    b[sizeof(b) - 1] = '\0';
                                    netErr(b);
                                    enet_peer_disconnect(ev.peer, 0);
                                } else {
                                    // Roster (Phase 2 Plan 03, NET-05): snapshot
                                    // every ALREADY-connected peer's id BEFORE the
                                    // newcomer is inserted below, so this list
                                    // never includes the newcomer itself.
                                    std::vector<u32> alreadyConnected;
                                    for (std::map<u32, PeerState>::const_iterator it =
                                             registry_.begin();
                                         it != registry_.end(); ++it) {
                                        alreadyConnected.push_back(it->first);
                                    }

                                    PeerState ps;
                                    ps.peer = ev.peer;
                                    registry_[id] = ps;
                                    ev.peer->data = (void*)(size_t)id;
                                    // Per-player epoch reset for the (possibly
                                    // reused) slot only - never a blanket clear,
                                    // so other connected players' accepted-epoch
                                    // state is untouched at N>=3 (finding 8 /
                                    // RESEARCH.md Pitfall 2).
                                    epochSeen_.erase(id);

                                    // Tell the NEWCOMER about every peer that was
                                    // ALREADY connected. sendTo() looks the target
                                    // up in registry_, so this MUST run after the
                                    // newcomer's own insert above - calling it
                                    // before the insert would find no destination
                                    // and silently drop every catch-up packet.
                                    for (size_t ai = 0; ai < alreadyConnected.size(); ++ai) {
                                        RosterPacket existing;
                                        existing.type = (u8)PKT_PLAYER_JOINED;
                                        existing.playerId = alreadyConnected[ai];
                                        sendTo(id,
                                               enet_packet_create(
                                                   &existing, sizeof(existing),
                                                   ENET_PACKET_FLAG_RELIABLE),
                                               CH_RELIABLE);
                                    }
                                    WelcomePacket w;
                                    w.type = (u8)PKT_WELCOME; w.version = PROTOCOL_VERSION; w.playerId = id;
                                    ENetPacket* out =
                                        enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                                    enet_peer_send(ev.peer, CH_RELIABLE, out);
                                    char b[112];
                                    _snprintf(b, sizeof(b) - 1,
                                              "peer connected id=%u player=%u (proto v%u)",
                                              (unsigned)id, (unsigned)id, (unsigned)PROTOCOL_VERSION);
                                    b[sizeof(b) - 1] = '\0';
                                    netLog(b);
                                    if (inbound_) inbound_->pushConnect(id);

                                    // Announce the newcomer to every ALREADY-
                                    // connected client, excluding the newcomer
                                    // itself (WR-01 fix): it already learned its
                                    // own id via WELCOME above, and a self-
                                    // targeted PKT_PLAYER_JOINED would be a new,
                                    // untested pushConnect(id == localId())
                                    // event with no documented downstream
                                    // meaning. broadcastExcept matches the
                                    // routing matrix's no-echo-to-author rule.
                                    RosterPacket joined;
                                    joined.type = (u8)PKT_PLAYER_JOINED;
                                    joined.playerId = id;
                                    broadcastExcept(id,
                                                     enet_packet_create(&joined, sizeof(joined),
                                                                         ENET_PACKET_FLAG_RELIABLE),
                                                     CH_RELIABLE);
                                }
                            }
                        }
                    } else if (!isHost_ && type == PKT_WELCOME) {
                        WelcomePacket w;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &w)) {
                            if (w.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: host v%u, ours v%u",
                                          (unsigned)w.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                            } else {
                                InterlockedExchange(&myId_, (LONG)w.playerId);
                                char b[96];
                                _snprintf(b, sizeof(b) - 1,
                                          "peer connected id=%u (proto v%u) - received WELCOME",
                                          (unsigned)w.playerId, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netLog(b);
                                if (inbound_) inbound_->pushConnect(0); // host id = 0
                            }
                        }
                    } else if (!isHost_ && type == PKT_PLAYER_JOINED) {
                        // Roster (protocol 56): learn of a connected peer - either
                        // the newcomer just announced, or one of the already-
                        // connected peers this client is catching up on. Mirrors
                        // pushConnect exactly (Inbound::pushConnect).
                        RosterPacket rp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &rp)
                            && inbound_) {
                            inbound_->pushConnect(rp.playerId);
                        }
                    } else if (!isHost_ && type == PKT_PLAYER_LEFT) {
                        // Roster (protocol 56): a peer left. Mirrors pushLeave
                        // exactly (Inbound::pushLeave). OWNER_ID_ALL ("the host
                        // itself disconnected") is handled separately in the
                        // DISCONNECT case below, never sent as a roster id here.
                        RosterPacket rp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &rp)
                            && inbound_) {
                            inbound_->pushLeave(rp.playerId);
                        }
                    } else if (!isHost_ && type == PKT_OWN_RANKS) {
                        // Ownership-rank announcement (protocol 57): the host's
                        // authoritative map<PlayerId,set<rank>>. Applied by the
                        // game thread via Replicator::setAllOwnRanks() (Plugin.cpp
                        // drains this queue in processNetEvents) - T-03-06: a
                        // client only ever APPLIES this host-authored map, never
                        // authors its own.
                        OwnRanksPacket rp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &rp)
                            && inbound_) {
                            inbound_->pushOwnRanks(rp);
                        }
                    } else if (isHost_ && type == PKT_OWN_RANKS) {
                        // IN-01: a client must never author PKT_OWN_RANKS (T-03-06,
                        // host-authoritative only) - correctly discarded below via
                        // the fallthrough enet_packet_destroy(ev.packet), but unlike
                        // every other rejected/forged packet in this phase there was
                        // no log line, leaving an attempted peer-authored ownership
                        // claim with no audit trail. Matches the "relay REJECT ..."/
                        // "relay FAILSAFE ..." logging discipline used elsewhere.
                        netErr("PKT_OWN_RANKS from a client rejected (host-authoritative only)");
                    } else if (type == PKT_ENTITY_BATCH) {
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(EntityBatchHeader) && inbound_) {
                            EntityBatchHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need =
                                sizeof(EntityBatchHeader) + (unsigned)hdr.count * sizeof(EntityState);
                            // WR-03: reject a forged ownerId BEFORE acceptEpoch() ever
                            // touches epochSeen_ - acceptEpoch() unconditionally inserts
                            // epochSeen_[hdr.ownerId] on a first sighting, and hdr.ownerId
                            // is exactly the client-controlled, not-yet-validated field
                            // CR-01 exists to guard. Validating first also avoids
                            // polluting a real owner's epoch tracking in the case a
                            // forged id happens to collide with a currently-connected
                            // real player's id.
                            if (len >= need && !rejectIfForgedOwner(sourcePlayerId, hdr.ownerId) &&
                                acceptEpoch(hdr.ownerId, hdr.epoch)) {
                                const enet_uint8* p = ev.packet->data + sizeof(EntityBatchHeader);
                                for (unsigned i = 0; i < hdr.count; ++i) {
                                    EntityState e;
                                    std::memcpy(&e, p + i * sizeof(EntityState), sizeof(e));
                                    deliverEntity(hdr.ownerId, hdr.sendMs, e);
                                }
                                // Class A relay (host only, Phase 3 ROUTE-02/03/04): the
                                // author's PKT_ENTITY_BATCH must reach every OTHER
                                // connected client exactly once, ownerId unchanged, never
                                // echoed back to the author (docs/ROUTING_MATRIX.md
                                // Security note; STRIDE T-03-01/T-03-02).
                                relayDispatch(type, sourcePlayerId, hdr.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_EVENT) {
                        // Reliable transition. Delivered immediately (NOT through the
                        // WAN-sim delay buffer): the sim models unreliable-batch loss,
                        // while the whole point of the reliable channel is that these
                        // survive that loss - so we honour their guaranteed delivery.
                        EventPacket evp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &evp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply, not
                            // just before the relay.
                            if (!rejectIfForgedOwner(sourcePlayerId, evp.ownerId)) {
                                inbound_->pushEvent(evp.ownerId, evp);
                                relayDispatch(type, sourcePlayerId, evp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_INV_SNAPSHOT) {
                        // Reliable container-contents snapshot (Phase 4a). Like
                        // PKT_EVENT, delivered immediately (not through the WAN-sim
                        // delay buffer): it rides the reliable channel precisely so a
                        // content change survives unreliable-batch loss.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(InvSnapshotHeader) && inbound_) {
                            InvSnapshotHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(InvSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(InvItemEntry);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(InvSnapshotHeader);
                                u32 cKey[5] = { hdr.cType, hdr.cContainer,
                                                hdr.cContainerSerial, hdr.cIndex, hdr.cSerial };
                                const InvItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const InvItemEntry*>(p) : 0;
                                // CR-01: reject a forged ownerId before local-apply.
                                if (!rejectIfForgedOwner(sourcePlayerId, hdr.ownerId)) {
                                    inbound_->pushInv(hdr.ownerId, hdr.keyKind, cKey,
                                                      items, hdr.count, hdr.flags);
                                    relayDispatch(type, sourcePlayerId, hdr.ownerId, 0, ev);
                                }
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM) {
                        // Reliable world-item snapshot (Phase W1). Delivered immediately
                        // (not through the WAN-sim delay buffer), like the inventory
                        // snapshot - it rides the reliable channel so a ground-item
                        // change survives unreliable-batch loss.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemSnapshotHeader) && inbound_) {
                            WorldItemSnapshotHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(WorldItemEntry);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemSnapshotHeader);
                                const WorldItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const WorldItemEntry*>(p) : 0;
                                // CR-01: reject a forged ownerId before local-apply.
                                if (!rejectIfForgedOwner(sourcePlayerId, hdr.ownerId)) {
                                    inbound_->pushWorldItems(hdr.ownerId, items, hdr.count);
                                    relayDispatch(type, sourcePlayerId, hdr.ownerId, 0, ev);
                                }
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_REMOVE) {
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemRemoveHeader) && inbound_) {
                            WorldItemRemoveHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemRemoveHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemRemoveHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                // CR-01: reject a forged ownerId before local-apply.
                                if (!rejectIfForgedOwner(sourcePlayerId, hdr.ownerId)) {
                                    inbound_->pushWorldRemove(hdr.ownerId, netIds, hdr.count);
                                    relayDispatch(type, sourcePlayerId, hdr.ownerId, 0, ev);
                                }
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_CLAIM) {
                        // A peer consumed the proxies it held for these netIds -
                        // protocol 58: this is now the claim INTENT (claimant ->
                        // host, host-terminated CLIENT-REQUEST), not a notice the
                        // author acts on unconditionally. The host arbitrates via
                        // ClaimArbiter.h's bounded contention window and
                        // broadcasts the single PKT_CLAIM_VERDICT; the raw intent
                        // is never relayed to the item's author or any other
                        // client anymore.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemClaimHeader) && inbound_) {
                            WorldItemClaimHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemClaimHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemClaimHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                // CR-01: reject a forged ownerId before local-apply
                                // (this branch lacked the check before Phase 7 -
                                // the WorldItemClaimHeader::ownerId field IS the
                                // claimant's own domain, so the same guard every
                                // other owner-tagged reliable packet uses applies).
                                if (!rejectIfForgedOwner(sourcePlayerId, hdr.ownerId)) {
                                    inbound_->pushWorldClaim(hdr.ownerId, hdr.authorId,
                                                             hdr.authorClaimMs,
                                                             netIds, hdr.count);
                                }
                            }
                        }
                    } else if (!isHost_ && type == PKT_CLAIM_VERDICT) {
                        // Reliable host-committed claim-contention verdict
                        // (protocol 58). Host-authored broadcast (Class B, like
                        // PKT_XFER_COMMIT/PKT_OWN_RANKS) - never received-and-
                        // relayed here; a client just pushes it to Inbound. The
                        // wire topology itself is the host-source guard (the
                        // PKT_OWN_RANKS T-03-06 precedent): a client's ENet
                        // connection has exactly one peer (the host), so anything
                        // a client receives here genuinely came from the host.
                        ClaimVerdictPacket cvp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &cvp)
                            && inbound_) {
                            inbound_->pushClaimVerdict(cvp);
                        }
                    } else if (isHost_ && type == PKT_CLAIM_VERDICT) {
                        // A client must never author PKT_CLAIM_VERDICT (host-
                        // authoritative only, protocol 58) - reject with an audit
                        // line rather than silently accepting a peer-authored
                        // verdict into the host's own Inbound (mirrors the
                        // PKT_XFER_COMMIT/PKT_OWN_RANKS reject branches).
                        netErr("PKT_CLAIM_VERDICT from a client rejected (host-authoritative only)");
                    } else if (type == PKT_NPC_CENSUS) {
                        // Reliable wide-radius NPC existence census (protocol
                        // 36; v38 rows carry positions too; protocol 59 Task 3
                        // WORLD-03: Class A relay, was Class B host-only - a
                        // join's own census now reaches every OTHER connected
                        // client too, so the per-owner intake, not just the
                        // host's, is usable at N). Latest-wins per owner on
                        // the game thread; delivered whole.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(NpcCensusHeader) && inbound_) {
                            NpcCensusHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(NpcCensusHeader)
                                          + (unsigned)hdr.count * 5 * sizeof(u32)
                                          + (unsigned)hdr.count * 3 * sizeof(float);
                            if (len >= need && hdr.count <= NPC_CENSUS_MAX) {
                                // CR-01: reject a forged ownerId before
                                // local-apply/relay (this branch lacked the
                                // check pre-Task-3 - harmless while it was
                                // host-only Class B, load-bearing now that a
                                // join's census relays to other joins).
                                if (!rejectIfForgedOwner(sourcePlayerId, hdr.ownerId)) {
                                    const enet_uint8* p = ev.packet->data + sizeof(NpcCensusHeader);
                                    const u32* hands =
                                        (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                    const float* pos = (hdr.count > 0)
                                        ? reinterpret_cast<const float*>(
                                              p + (unsigned)hdr.count * 5 * sizeof(u32))
                                        : 0;
                                    inbound_->pushNpcCensus(hdr.ownerId, hands, pos, hdr.count);
                                    relayDispatch(type, sourcePlayerId, hdr.ownerId, 0, ev);
                                }
                            }
                        }
                    } else if (type == PKT_WORLD_DROP) {
                        // Reliable conservation drop intent (Phase W2). Delivered immediately
                        // (not via the WAN-sim buffer) like the other reliable packets.
                        WorldDropPacket wdp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &wdp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, wdp.ownerId)) {
                                inbound_->pushWorldDrop(wdp.ownerId, wdp);
                                relayDispatch(type, sourcePlayerId, wdp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_WORLD_PICKUP) {
                        // Reliable conservation pickup intent (Phase W3), mirror of the drop.
                        WorldPickupPacket wpp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &wpp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, wpp.ownerId)) {
                                inbound_->pushWorldPickup(wpp.ownerId, wpp);
                                relayDispatch(type, sourcePlayerId, wpp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_INV_XFER) {
                        // Reliable cross-owner transfer intent (protocol 37;
                        // protocol 58: host-terminated CLIENT-REQUEST carrying
                        // srcOwnerId/dstOwnerId - the host arbitrates via
                        // XferCommit.h and broadcasts PKT_XFER_COMMIT; the raw
                        // intent is never relayed to any other client).
                        InvXferPacket ixp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ixp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply
                            // (this branch lacked the check before Phase 7 -
                            // the one owner-tagged reliable packet that did).
                            if (!rejectIfForgedOwner(sourcePlayerId, ixp.ownerId)) {
                                inbound_->pushInvXfer(ixp.ownerId, ixp);
                            }
                        }
                    } else if (!isHost_ && type == PKT_XFER_COMMIT) {
                        // Reliable host-committed transfer verdict (protocol
                        // 58). Host-authored broadcast (Class B, like
                        // PKT_OWN_RANKS) - never received-and-relayed here; a
                        // client just pushes it to Inbound. The wire topology
                        // itself is the host-source guard: a client's ENet
                        // connection has exactly one peer (the host), so
                        // anything a client receives here genuinely came from
                        // the host - the T-03-06/PKT_OWN_RANKS precedent
                        // (host-authoritative packet, !isHost_ receive branch
                        // + a matching isHost_ REJECT branch below for a
                        // client that tries to author one).
                        XferCommitPacket xcp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &xcp)
                            && inbound_) {
                            inbound_->pushXferCommit(xcp);
                        }
                    } else if (isHost_ && type == PKT_XFER_COMMIT) {
                        // A client must never author PKT_XFER_COMMIT (host-
                        // authoritative only, protocol 58) - reject with an
                        // audit line rather than silently accepting a peer-
                        // authored verdict into the host's own Inbound
                        // (mirrors the PKT_OWN_RANKS T-03-06 reject branch
                        // immediately above it).
                        netErr("PKT_XFER_COMMIT from a client rejected (host-authoritative only)");
                    } else if (type == PKT_XFER_COMMIT_ACK) {
                        // Reliable transfer-commit bookkeeping ack (protocol
                        // 58, host side only). Audit/correlation - never
                        // settles anything.
                        XferCommitAckPacket xap;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &xap)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, xap.ownerId)) {
                                inbound_->pushXferCommitAck(xap.ownerId, xap);
                            }
                        }
                    } else if (type == PKT_INV_XFER_ACK) {
                        // Reliable transfer verdict (protocol 50). Must be as
                        // reliable as the intent it answers: a lost ack leaves
                        // the author back on the wall-clock guess it replaces.
                        InvXferAckPacket iap;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &iap)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, iap.ownerId)) {
                                inbound_->pushInvXferAck(iap.ownerId, iap);
                                // Class D, already-addressable (matrix row 46): xferOwnerId
                                // is the routable destination - unicast the verdict back to
                                // the client that authored the original intent.
                                relayDispatch(type, sourcePlayerId, iap.ownerId,
                                              iap.xferOwnerId, ev);
                            }
                        }
                    } else if (type == PKT_MEDICAL) {
                        // Reliable owner-authoritative vitals snapshot (phase 2).
                        // Delivered immediately like the other reliable packets.
                        MedicalPacket mp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &mp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, mp.ownerId)) {
                                inbound_->pushMedical(mp.ownerId, mp);
                                relayDispatch(type, sourcePlayerId, mp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_TREATMENT) {
                        // Reliable treatment delta (first aid on a driven copy,
                        // forwarded to the owner). Phase 6 (GAP-1): now Class A
                        // broadcast-except - a join healing ANOTHER join's driven
                        // body must reach that body's authority, not just the
                        // host. relayDispatch() only fans out on the host
                        // (isHost_ guard); a client still pushes to its own
                        // Inbound so it observes host-forwarded treatments from
                        // other joins.
                        TreatmentPacket tp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &tp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, tp.ownerId)) {
                                inbound_->pushTreatment(tp.ownerId, tp);
                                relayDispatch(type, sourcePlayerId, tp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_COMBAT_HIT) {
                        // Reliable join-dealt damage report (join -> host): the
                        // host wounds the authoritative world NPC.
                        CombatHitPacket chp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &chp)
                            && inbound_) {
                            inbound_->pushCombatHit(chp.ownerId, chp);
                        }
                    } else if (type == PKT_SPEED_REQ || type == PKT_SPEED_SET) {
                        // Reliable game-speed request/set (consensus speed sync).
                        // Delivered immediately like the other reliable packets.
                        // Protocol 60 (Phase 9 Plan 02, CONS-02): PKT_SPEED_REQ
                        // is a join-authored intent (host-terminated, like
                        // PKT_MONEY_DELTA) and gains rejectIfForgedOwner before
                        // it can land in speedVotes_ - it lacked the check
                        // pre-Phase-9 (research Wire Impact row 7). PKT_SPEED_SET
                        // is host-authored only; the host's own syncSpeed drain
                        // ignores any PKT_SPEED_SET it receives (the isHost
                        // guard on apply), so a client-forged SET already has
                        // nowhere to land - no check needed for it here.
                        SpeedPacket sp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sp)
                            && inbound_) {
                            if (type == PKT_SPEED_REQ) {
                                if (!rejectIfForgedOwner(sourcePlayerId, sp.ownerId))
                                    inbound_->pushSpeed(sp.ownerId, sp);
                            } else {
                                inbound_->pushSpeed(sp.ownerId, sp);
                            }
                        }
                    } else if (type == PKT_STATS) {
                        // Reliable owner-authoritative character-stats snapshot
                        // (protocol 17). Delivered immediately like the others.
                        StatsPacket stp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &stp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, stp.ownerId)) {
                                inbound_->pushStats(stp.ownerId, stp);
                                relayDispatch(type, sourcePlayerId, stp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_MONEY) {
                        // Reliable host-authoritative money-pool total
                        // (protocol 52). Delivered immediately like the others.
                        MoneyPacket mo;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &mo)
                            && inbound_) {
                            inbound_->pushMoney(mo.ownerId, mo);
                        }
                    } else if (type == PKT_MONEY_DELTA) {
                        // Reliable money-pool delta (protocol 52, join -> host).
                        // Ordered delivery is what makes the fold exactly-once.
                        // Protocol 60 (CONS-01): reject a forged ownerId before
                        // local-apply - this branch lacked the check pre-Phase-9
                        // (research Wire Impact row 7 / Pitfall 7), the one
                        // owner-tagged reliable packet that did.
                        MoneyDeltaPacket md;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &md)
                            && inbound_) {
                            if (!rejectIfForgedOwner(sourcePlayerId, md.ownerId)) {
                                inbound_->pushMoneyDelta(md.ownerId, md);
                            }
                        }
                    } else if (!isHost_ && type == PKT_MONEY_REJECT) {
                        // Reliable host-committed insufficient-funds verdict
                        // (protocol 60, CONS-01). Host-authored broadcast
                        // (Class B, like PKT_CLAIM_VERDICT/PKT_CELL_MAP) -
                        // never received-and-relayed here; a client just
                        // pushes it to Inbound. The wire topology itself is
                        // the host-source guard: a client's ENet connection
                        // has exactly one peer (the host), so anything a
                        // client receives here genuinely came from the host.
                        MoneyRejectPacket mr;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &mr)
                            && inbound_) {
                            inbound_->pushMoneyReject(mr);
                        }
                    } else if (isHost_ && type == PKT_MONEY_REJECT) {
                        // A client must never author PKT_MONEY_REJECT (host-
                        // authoritative only, protocol 60) - reject with an
                        // audit line rather than silently accepting a peer-
                        // authored verdict into the host's own Inbound
                        // (mirrors the PKT_CLAIM_VERDICT/PKT_CELL_MAP reject
                        // branches).
                        netErr("PKT_MONEY_REJECT from a client rejected (host-authoritative only)");
                    } else if (type == PKT_FACTION) {
                        // Reliable player-faction relation row (protocol 24):
                        // host stream or join intent, disambiguated on apply.
                        FactionPacket fa;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &fa)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, fa.ownerId)) {
                                inbound_->pushFaction(fa.ownerId, fa);
                                relayDispatch(type, sourcePlayerId, fa.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_TIME) {
                        // Bidirectional game-clock channel (protocol 25): host
                        // broadcasts its sample, a join reports its own back.
                        // Delivered immediately like the others. Protocol 60
                        // (Phase 9 Plan 02, CONS-03): a join's report is a
                        // host-terminated client request just like
                        // PKT_SPEED_REQ/PKT_MONEY_DELTA, and the host's
                        // per-owner timeReports_ map must never be poisoned by
                        // a misattributed owner slot - rejectIfForgedOwner
                        // returns false immediately on a client (isHost_
                        // guard), so the host's own broadcast to joins is
                        // unaffected.
                        TimePacket ti;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ti)
                            && inbound_) {
                            if (!rejectIfForgedOwner(sourcePlayerId, ti.ownerId))
                                inbound_->pushTime(ti.ownerId, ti);
                        }
                    } else if (type == PKT_DOOR) {
                        // Reliable baked-door state row (protocol 26):
                        // symmetric change-gated, disambiguated on apply.
                        DoorPacket dp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &dp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, dp.ownerId)) {
                                inbound_->pushDoor(dp.ownerId, dp);
                                relayDispatch(type, sourcePlayerId, dp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_PROD) {
                        // Reliable host-authoritative machine state row
                        // (protocol 33), applied via the engine levers.
                        ProdPacket pp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &pp)
                            && inbound_) {
                            inbound_->pushProd(pp.ownerId, pp);
                        }
                    } else if (type == PKT_RESEARCH) {
                        // Reliable known-research row (protocol 38): a host
                        // stream (unchanged) OR, since Phase 8 (08-01,
                        // WORLD-02), a join's post-baseline-unlock INTENT -
                        // disambiguated on apply exactly like PKT_FACTION/
                        // PKT_DOOR above. Already RELAY_NONE pre-Phase-8 (no
                        // routing change needed), but a join publishing one
                        // now means the host-side receive genuinely needs the
                        // forge check it never needed before (a client could
                        // not previously author this packet type at all).
                        ResearchPacket rp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &rp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, rp.ownerId)) {
                                inbound_->pushResearch(rp.ownerId, rp);
                            }
                        }
                    } else if (type == PKT_DEED) {
                        // Reliable property-deed ownership row (protocol 54):
                        // symmetric, applied as a pure state write.
                        DeedPacket dep;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &dep)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, dep.ownerId)) {
                                inbound_->pushDeed(dep.ownerId, dep);
                                relayDispatch(type, sourcePlayerId, dep.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_FIXTURE) {
                        // Reliable runtime-fixture identity row (protocol 55):
                        // symmetric, pairs the sender's hand with our own copy.
                        FixturePacket fxp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &fxp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, fxp.ownerId)) {
                                inbound_->pushFixture(fxp.ownerId, fxp);
                                relayDispatch(type, sourcePlayerId, fxp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_BUILD_PLACE) {
                        // Reliable placed-building announcement (protocol 27):
                        // describe/mint edge, keyed by the placer's hand.
                        BuildPlacePacket bp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &bp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, bp.ownerId)) {
                                inbound_->pushBuildPlace(bp.ownerId, bp);
                                relayDispatch(type, sourcePlayerId, bp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_BUILD_STATE) {
                        // Reliable construction-progress row (protocol 27),
                        // placer-authoritative, translation-map applied.
                        BuildStatePacket bs;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &bs)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, bs.ownerId)) {
                                inbound_->pushBuildState(bs.ownerId, bs);
                                relayDispatch(type, sourcePlayerId, bs.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_BUILD_DOOR) {
                        // Reliable placed-building door row (protocol 28),
                        // symmetric on the translated (bkey, index) identity.
                        BuildDoorPacket bd;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &bd)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, bd.ownerId)) {
                                inbound_->pushBuildDoor(bd.ownerId, bd);
                                relayDispatch(type, sourcePlayerId, bd.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_BUILD_REMOVE) {
                        // Reliable placer-authoritative building removal
                        // (protocol 28).
                        BuildRemovePacket br;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &br)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, br.ownerId)) {
                                inbound_->pushBuildRemove(br.ownerId, br);
                                relayDispatch(type, sourcePlayerId, br.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_STEALTH) {
                        // Unreliable stealth detection-map snapshot (protocol 20).
                        // Delivered immediately; the game thread keeps latest-wins
                        // per subject. Phase 6 (GAP-2): now Class A broadcast-
                        // except - one join's detection feedback about ANOTHER
                        // join's sneaker must reach that join, not just the
                        // host. relayDispatch() only fans out on the host
                        // (isHost_ guard); a client still pushes to its own
                        // Inbound so it observes host-forwarded feedback from
                        // other joins.
                        StealthPacket slp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &slp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, slp.ownerId)) {
                                inbound_->pushStealth(slp.ownerId, slp);
                                relayDispatch(type, sourcePlayerId, slp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_SPAWN_REQ) {
                        // Reliable runtime-spawn query (protocol 21, join -> host).
                        SpawnReqPacket srp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &srp)
                            && inbound_) {
                            inbound_->pushSpawnReq(srp.ownerId, srp);
                        }
                    } else if (type == PKT_SPAWN_INFO) {
                        // Reliable runtime-spawn description (protocol 21, host -> join).
                        SpawnInfoPacket sip;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sip)
                            && inbound_) {
                            inbound_->pushSpawnInfo(sip.ownerId, sip);
                        }
                    } else if (type == PKT_SAVE_REQ) {
                        // Reliable join save request (protocol 31, join -> host).
                        SaveReqPacket sq;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sq)
                            && inbound_) {
                            // Phase 10 Plan 01 (SAVE-01, Spoofing): reject a
                            // forged ownerId before the coordinator ever
                            // keys arbiter/SaveCoord state on it - a forged
                            // requester could otherwise name another
                            // client's reqId in the arbitration record.
                            if (!rejectIfForgedOwner(sourcePlayerId, sq.ownerId))
                                inbound_->pushSaveReq(sq.ownerId, sq);
                        }
                    } else if (type == PKT_SAVE_BEGIN) {
                        // Reliable save-transfer announce (protocol 31, host -> join).
                        SaveBeginPacket sb;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sb)
                            && inbound_) {
                            inbound_->pushSaveBegin(sb.ownerId, sb);
                        }
                    } else if (type == PKT_SAVE_FILE) {
                        // Reliable save-file chunk (protocol 31, host -> join):
                        // [SaveFileHeader][path[pathLen]][payload[dataLen]].
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(SaveFileHeader) && inbound_) {
                            SaveFileHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(SaveFileHeader)
                                          + (unsigned)hdr.pathLen + (unsigned)hdr.dataLen;
                            if (len >= need && hdr.pathLen > 0 &&
                                hdr.pathLen <= SAVE_PATH_MAX &&
                                hdr.dataLen <= SAVE_CHUNK_MAX) {
                                const enet_uint8* p = ev.packet->data + sizeof(SaveFileHeader);
                                inbound_->pushSaveFile(hdr.ownerId, hdr,
                                                       (const char*)p,
                                                       (const u8*)p + hdr.pathLen);
                            }
                        }
                    } else if (type == PKT_SAVE_DONE) {
                        // Reliable save-transfer CRC table (protocol 31, host -> join):
                        // [SaveDoneHeader][u32 crc * fileCount].
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(SaveDoneHeader) && inbound_) {
                            SaveDoneHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(SaveDoneHeader)
                                          + (unsigned)hdr.fileCount * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(SaveDoneHeader);
                                inbound_->pushSaveDone(hdr.ownerId, hdr,
                                                       (hdr.fileCount > 0)
                                                           ? (const u32*)p : 0);
                            }
                        }
                    } else if (type == PKT_SAVE_ACK) {
                        // Reliable commit acknowledgement (protocol 31, join -> host).
                        SaveAckPacket sa;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sa)
                            && inbound_) {
                            // Phase 10 Plan 01 (SAVE-01, Spoofing, T-10-01):
                            // the exact gap the last-of-N-ACKs-wins collapse
                            // fix depends on closing - a forged ownerId here
                            // could mark ANOTHER client's SaveClient
                            // COMMITTED (or fail it out) once saveNoteAck
                            // keys per-owner state on it.
                            if (!rejectIfForgedOwner(sourcePlayerId, sa.ownerId))
                                inbound_->pushSaveAck(sa.ownerId, sa);
                        }
                    } else if (type == PKT_LOAD_GO) {
                        // Reliable coordinated-load order (protocol 32, host -> join).
                        LoadGoPacket lg;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &lg)
                            && inbound_) {
                            inbound_->pushLoadGo(lg.ownerId, lg);
                        }
                    } else if (type == PKT_LOAD_REQ) {
                        // Reliable join load request (protocol 32, join -> host).
                        LoadReqPacket lr;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &lr)
                            && inbound_) {
                            // Phase 10 Plan 01 (SAVE-03, Spoofing): reject a
                            // forged ownerId before it can key the arbiter's
                            // record (a forged requester naming another
                            // client's reqId).
                            if (!rejectIfForgedOwner(sourcePlayerId, lr.ownerId))
                                inbound_->pushLoadReq(lr.ownerId, lr);
                        }
                    } else if (type == PKT_LOAD_NACK) {
                        // Reliable copy-missing/diverged answer (protocol 32, join -> host).
                        LoadNackPacket ln;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ln)
                            && inbound_) {
                            // Phase 10 Plan 01 (SAVE-02, Spoofing): a forged
                            // ownerId here could mark ANOTHER client's
                            // LoadClient NACKED once loadNoteNack keys
                            // per-owner state on it.
                            if (!rejectIfForgedOwner(sourcePlayerId, ln.ownerId))
                                inbound_->pushLoadNack(ln.ownerId, ln);
                        }
                    } else if (type == PKT_LOAD_ACK) {
                        // Reliable positive coordinated-load completion (protocol
                        // 61, join -> host, SAVE-02). Host-terminated CLIENT
                        // input, the PKT_SAVE_ACK shape.
                        LoadAckPacket la;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &la)
                            && inbound_) {
                            // Phase 10 Plan 01 (SAVE-02, Spoofing, T-10-01):
                            // a forged ownerId here could mark ANOTHER
                            // client's LoadClient LOADED (or FAILED) once
                            // loadNoteAck keys per-owner state on it - the
                            // exact SaveAck-forgery gap, mirrored onto the
                            // new positive load ACK.
                            if (!rejectIfForgedOwner(sourcePlayerId, la.ownerId))
                                inbound_->pushLoadAck(la.ownerId, la);
                        }
                    } else if (!isHost_ && type == PKT_COORD_REJECT) {
                        // Reliable first-wins arbitration reject (protocol 61,
                        // SAVE-03). Host-authored Class D unicast to the
                        // rejected requester - never received-and-relayed here;
                        // a client just pushes it to Inbound. The wire topology
                        // itself is the host-source guard (the PKT_CLAIM_VERDICT/
                        // PKT_OWN_RANKS T-03-06 precedent): a client's ENet
                        // connection has exactly one peer (the host).
                        CoordRejectPacket cr;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &cr)
                            && inbound_) {
                            inbound_->pushCoordReject(cr);
                        }
                    } else if (isHost_ && type == PKT_COORD_REJECT) {
                        // A client must never author PKT_COORD_REJECT (host-
                        // authoritative only, protocol 61, SAVE-03) - reject
                        // with an audit line rather than silently accepting a
                        // peer-authored rejection into the host's own Inbound
                        // (mirrors the PKT_CLAIM_VERDICT/PKT_MONEY_REJECT
                        // reject branches).
                        netErr("PKT_COORD_REJECT from a client rejected (host-authoritative only)");
                    } else if (type == PKT_CAM_HINT) {
                        // Camera hint (protocol 43): latest-wins interest
                        // anchor. Accepted in BOTH directions - the attention
                        // gate needs each side to know where the peer is
                        // looking, not just the host.
                        CamHintPacket chp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &chp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, chp.ownerId)) {
                                inbound_->pushCamHint(chp.ownerId, chp);
                                relayDispatch(type, sourcePlayerId, chp.ownerId, 0, ev);
                            }
                        }
                    } else if (type == PKT_CELL_CLAIM) {
                        // Cell claim (protocol 49; protocol 59 WORLD-03:
                        // host-terminated INTENT, not a Class A relay -
                        // every instance still PUBLISHES its own claims, but
                        // only the host folds them into claimSlots_ and runs
                        // the reduce). The raw claim is never relayed to any
                        // other client anymore.
                        CellClaimPacket ccp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ccp)
                            && inbound_) {
                            // CR-01: reject a forged ownerId before local-apply.
                            if (!rejectIfForgedOwner(sourcePlayerId, ccp.ownerId)) {
                                inbound_->pushCellClaim(ccp.ownerId, ccp);
                            }
                        }
                    } else if (!isHost_ && type == PKT_CELL_MAP) {
                        // Reliable host-committed cell-claim map (protocol
                        // 59, WORLD-03). Host-authored broadcast (Class B,
                        // like PKT_CLAIM_VERDICT/PKT_XFER_COMMIT/
                        // PKT_OWN_RANKS) - never received-and-relayed here; a
                        // client just pushes it to Inbound. The wire topology
                        // itself is the host-source guard (the
                        // PKT_OWN_RANKS T-03-06 precedent): a client's ENet
                        // connection has exactly one peer (the host), so
                        // anything a client receives here genuinely came
                        // from the host.
                        CellMapPacket cmp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &cmp)
                            && inbound_) {
                            inbound_->pushCellMap(cmp);
                        }
                    } else if (isHost_ && type == PKT_CELL_MAP) {
                        // A client must never author PKT_CELL_MAP (host-
                        // authoritative only, protocol 59) - reject with an
                        // audit line rather than silently accepting a
                        // peer-authored map into the host's own Inbound
                        // (mirrors the PKT_CLAIM_VERDICT/PKT_XFER_COMMIT/
                        // PKT_OWN_RANKS reject branches).
                        netErr("PKT_CELL_MAP from a client rejected (host-authoritative only)");
                    } else if (type == PKT_TIME_PING) {
                        // Wall-clock sync probe: echo immediately (host side). Answered
                        // inside the service loop so the response delay stays minimal
                        // (the join's rtt/2 assumption depends on it).
                        TimePingPacket tp;
                        if (isHost_ && readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &tp)) {
                            TimePongPacket po;
                            po.type = (u8)PKT_TIME_PONG;
                            po.nonce = tp.nonce;
                            po.echoWallMs = tp.senderWallMs;
                            po.responderWallMs = (u32)coop::wallClockMs();
                            ENetPacket* out = enet_packet_create(&po, sizeof(po), 0 /*unreliable*/);
                            enet_peer_send(ev.peer, CH_UNRELIABLE, out);
                        }
                    } else if (type == PKT_TIME_PONG) {
                        // Wall-clock sync echo (join side): derive one (rtt, offset)
                        // sample; keep the minimum-RTT one (least queueing noise).
                        TimePongPacket po;
                        if (!isHost_ && readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &po)) {
                            unsigned long t1 = coop::wallClockMs();
                            unsigned long t0 = (unsigned long)po.echoWallMs;
                            if (t1 >= t0) { // skip the (rare) sample spanning local midnight
                                unsigned long rtt = t1 - t0;
                                long offset = (long)po.responderWallMs + (long)(rtt / 2ul) - (long)t1;
                                // Normalize a cross-midnight host/join wrap into [-12h, +12h).
                                while (offset >= HALF_DAY_MS)  offset -= 2l * HALF_DAY_MS;
                                while (offset < -HALF_DAY_MS)  offset += 2l * HALF_DAY_MS;
                                ++syncSamples;
                                if (rtt <= bestRttMs) { bestRttMs = rtt; bestOffsetMs = offset; }
                            }
                        }
                    }
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    if (isHost_) {
                        u32 id = (u32)(size_t)ev.peer->data;
                        ev.peer->data = 0;
                        // Per-player teardown (Phase 2 Plan 03, CR-01 fix): clear
                        // THIS departing player's registry + epoch bookkeeping
                        // WITHOUT touching any other connected player's state -
                        // the blanket epochSeen_.clear() this replaced used to
                        // wipe every OTHER connected player's accepted-epoch
                        // state too at N>=3 (TWO_PLAYER_ASSUMPTIONS finding 8 /
                        // RESEARCH.md Pitfall 2). Registry erase happens BEFORE
                        // the roster-leave broadcast and before this slot can be
                        // reassigned to a new connection (threat T-02-04:
                        // teardown-before-reuse). Never dereference peer after
                        // this edge (never retain ENetPeer* across disconnect).
                        //
                        // CR-01: ev.peer->data is ONLY ever set to a real
                        // PlayerId in the HELLO-success branch above. A peer
                        // rejected before that point (protocol-version
                        // mismatch, or MAX_PLAYERS slots full - both call
                        // enet_peer_disconnect() before ev.peer->data is
                        // assigned) still raises this DISCONNECT event, and
                        // ev.peer->data reads its zero-initialized default
                        // (enet_host_create() memsets the whole peer array) -
                        // the SAME value as PlayerId 0, the host itself. The
                        // entire teardown (epoch erase, leave-queue push, log,
                        // and PKT_PLAYER_LEFT broadcast) must therefore run
                        // ONLY when this peer was actually found in registry_ -
                        // never unconditionally on bare `id`.
                        std::map<u32, PeerState>::iterator regIt = registry_.find(id);
                        if (regIt != registry_.end()) {
                            registry_.erase(regIt);
                            epochSeen_.erase(id);
                            if (inbound_) inbound_->pushLeave(id);
                            char b[80];
                            _snprintf(b, sizeof(b) - 1,
                                      "peer disconnected id=%u player=%u",
                                      (unsigned)id, (unsigned)id);
                            b[sizeof(b) - 1] = '\0';
                            netLog(b);
                            // Roster (NET-05): tell every REMAINING connected
                            // client the slot is free. broadcast() only
                            // reaches currently-connected peers, so the
                            // departing id (already erased from registry_
                            // above) is never itself a destination.
                            RosterPacket left;
                            left.type = (u8)PKT_PLAYER_LEFT;
                            left.playerId = id;
                            broadcast(enet_packet_create(&left, sizeof(left),
                                                          ENET_PACKET_FLAG_RELIABLE),
                                      CH_RELIABLE);
                        } else {
                            // Never completed HELLO (protocol-version
                            // mismatch, MAX_PLAYERS full, or dropped
                            // mid-handshake) - no registry entry, no roster
                            // fact to announce, and bare id=0 must never be
                            // mistaken for the host's own PlayerId.
                            netLog("peer disconnected before completing "
                                   "handshake (no id assigned)");
                        }
                    } else {
                        // Client: the sole connection (to the host) just ended -
                        // all local epoch bookkeeping for this session is moot
                        // (unlike the host, a client only ever tracks ownerIds
                        // relative to this one link, so a blanket clear here is
                        // correct - not the finding-8 global-wipe bug class).
                        epochSeen_.clear();
                        serverPeer_ = 0;
                        if (inbound_) inbound_->pushLeave(OWNER_ID_ALL);
                        netLog("disconnected from host");
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Release any WAN-sim-delayed inbound entities whose arrival time has come.
        // No-op (and cheap) when the sim is disabled / nothing is pending.
        flushDelayed();

        // Wall-clock time sync (join side): ping every ~2 s on the UNRELIABLE
        // channel (a retransmitted probe would carry a stale t0), and log the
        // best CLOCKSYNC estimate every ~5 s for the oracles' clock alignment.
        if (!isHost_) {
            DWORD nowTick = GetTickCount();
            if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED
                && (lastTimePing == 0 || nowTick - lastTimePing >= 2000)) {
                lastTimePing = nowTick;
                TimePingPacket tp;
                tp.type = (u8)PKT_TIME_PING;
                tp.nonce = pingNonce++;
                tp.senderWallMs = (u32)coop::wallClockMs();
                ENetPacket* out = enet_packet_create(&tp, sizeof(tp), 0 /*unreliable*/);
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            }
            if (syncSamples > 0 && (lastClockLog == 0 || nowTick - lastClockLog >= 5000)) {
                lastClockLog = nowTick;
                char b[96];
                _snprintf(b, sizeof(b) - 1, "CLOCKSYNC offset=%ld rtt=%lu n=%u",
                          bestOffsetMs, bestRttMs, syncSamples);
                b[sizeof(b) - 1] = '\0';
                netLog(b);
            }
        }

        // Drain + send any queued reliable events on CH_RELIABLE. ENet guarantees
        // delivery + ordering on that channel, so these survive the unreliable-batch
        // loss the WAN sim injects (the reliability proof the death oracle checks).
        std::vector<EventPacket> events;
        EnterCriticalSection(&outCs_);
        events.swap(outEvents_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < events.size(); ++i) {
            ENetPacket* out = enet_packet_create(&events[i], sizeof(EventPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // TEST HOOK (Phase 2, CR-02 regression): resend a fresh HELLO on this
        // already-established connection when debugResendHelloForTest() was
        // called. CLIENT only - mirrors the exact HelloPacket construction in
        // the CONNECT handler above, byte for byte, so the host receives an
        // indistinguishable "second successful HELLO" on the same ENetPeer.
        if (!isHost_ && InterlockedExchange(&debugResendHello_, 0)) {
            if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                HelloPacket h;
                h.type = (u8)PKT_HELLO; h.version = PROTOCOL_VERSION; h.nameLen = 0;
                ENetPacket* out = enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
                netLog("debug: resent duplicate HELLO on established connection");
            }
        }

        // Drain + send any queued conservation DROP intents on CH_RELIABLE (Phase W2). A
        // fixed-size POD per drop, like an event; reliable so a drop is never lost.
        //
        // Ordered BEFORE the inventory snapshots below: both ride CH_RELIABLE, which ENet
        // delivers in SEND order, so draining drops first means a drop intent can never be
        // overtaken on the wire by the bag snapshot that reflects it. The peer's apply phase
        // already runs drops before the reconcile within a tick, so this is belt-and-braces
        // - but it removes an inversion that made the same-tick case confusing to reason
        // about (and to read in a packet capture).
        std::vector<WorldDropPacket> drops;
        EnterCriticalSection(&outCs_);
        drops.swap(outWorldDrops_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < drops.size(); ++i) {
            ENetPacket* out = enet_packet_create(&drops[i], sizeof(WorldDropPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued conservation PICKUP intents on CH_RELIABLE (Phase W3).
        std::vector<WorldPickupPacket> pickups;
        EnterCriticalSection(&outCs_);
        pickups.swap(outWorldPickups_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < pickups.size(); ++i) {
            ENetPacket* out = enet_packet_create(&pickups[i], sizeof(WorldPickupPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued container-contents snapshots on CH_RELIABLE. Each
        // is [InvSnapshotHeader][InvItemEntry*count]; only enqueued on content-change,
        // so this channel stays quiet. Reliable so the change survives WAN loss.
        std::vector<OutInv> invs;
        EnterCriticalSection(&outCs_);
        invs.swap(outInv_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < invs.size(); ++i) {
            unsigned count = (unsigned)invs[i].items.size();
            u8 iflags = invs[i].flags;
            if (count > INV_ITEMS_MAX) { count = INV_ITEMS_MAX; iflags |= INV_FLAG_TRUNCATED; }
            unsigned bytes = sizeof(InvSnapshotHeader) + count * sizeof(InvItemEntry);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            InvSnapshotHeader hdr;
            hdr.type             = (u8)PKT_INV_SNAPSHOT;
            hdr.ownerId          = invs[i].ownerId;
            hdr.keyKind          = invs[i].keyKind;
            hdr.flags            = iflags;
            hdr.cType            = invs[i].cKey[0];
            hdr.cContainer       = invs[i].cKey[1];
            hdr.cContainerSerial = invs[i].cKey[2];
            hdr.cIndex           = invs[i].cKey[3];
            hdr.cSerial          = invs[i].cKey[4];
            hdr.count            = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &invs[i].items[0],
                            count * sizeof(InvItemEntry));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued world-item snapshots on CH_RELIABLE (Phase W1). Each
        // is [WorldItemSnapshotHeader][WorldItemEntry*count]; only enqueued for new/
        // changed ground items, so a settled world produces no traffic. Host-authored.
        std::vector<OutWorldItems> wis;
        std::vector<OutWorldRemove> wrs;
        std::vector<OutWorldClaim> wcs;
        EnterCriticalSection(&outCs_);
        wis.swap(outWorldItems_);
        wrs.swap(outWorldRemove_);
        wcs.swap(outWorldClaim_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < wis.size(); ++i) {
            unsigned count = (unsigned)wis[i].items.size();
            if (count > WORLD_ITEMS_MAX) count = WORLD_ITEMS_MAX;
            unsigned bytes = sizeof(WorldItemSnapshotHeader) + count * sizeof(WorldItemEntry);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemSnapshotHeader hdr;
            hdr.type    = (u8)PKT_WORLD_ITEM;
            hdr.ownerId = wis[i].ownerId;
            hdr.count   = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wis[i].items[0],
                            count * sizeof(WorldItemEntry));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        // Drain + send any queued world-item culls on CH_RELIABLE (Phase W1).
        for (size_t i = 0; i < wrs.size(); ++i) {
            unsigned count = (unsigned)wrs[i].netIds.size();
            if (count > 255) count = 255;
            unsigned bytes = sizeof(WorldItemRemoveHeader) + count * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemRemoveHeader hdr;
            hdr.type    = (u8)PKT_WORLD_ITEM_REMOVE;
            hdr.ownerId = wrs[i].ownerId;
            hdr.count   = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wrs[i].netIds[0], count * sizeof(u32));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        // Drain + send any queued world-item claim INTENTS on CH_RELIABLE (protocol
        // 58). Now a host-terminated CLIENT-REQUEST (Class C shape, like
        // PKT_INV_XFER) - only the HOST's own applyClaimIntents ever drains
        // InboundWorldClaim. When the HOST itself is the claimant (a host player
        // can pick up an item exactly like any other claimant - INV-01 2-player
        // parity requires this), the generic isHost_ ? broadcast branch below
        // would otherwise fan the raw intent out to every connected client, none
        // of which drain it as an intent anymore (the PKT_INV_XFER self-loop fix
        // precedent, Phase 7 Plan 01 Task 2). Loop it directly into the host's OWN
        // Inbound so the SAME applyClaimIntents drain path picks it up next tick,
        // exactly as if it had arrived over the network from itself.
        for (size_t i = 0; i < wcs.size(); ++i) {
            unsigned count = (unsigned)wcs[i].netIds.size();
            if (count > 255) count = 255;
            if (isHost_) {
                if (inbound_)
                    inbound_->pushWorldClaim(wcs[i].ownerId, wcs[i].authorId,
                                             wcs[i].authorClaimMs,
                                             count > 0 ? &wcs[i].netIds[0] : 0, count);
                continue;
            }
            unsigned bytes = sizeof(WorldItemClaimHeader) + count * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemClaimHeader hdr;
            hdr.type          = (u8)PKT_WORLD_ITEM_CLAIM;
            hdr.ownerId       = wcs[i].ownerId;
            hdr.authorId      = wcs[i].authorId;
            hdr.authorClaimMs = wcs[i].authorClaimMs;
            hdr.count         = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wcs[i].netIds[0], count * sizeof(u32));
            if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued host-committed claim-contention verdicts on
        // CH_RELIABLE (protocol 58). HOST ONLY in practice (queueClaimVerdict's
        // doc comment) - isHost_ true broadcasts to every connected client (the
        // single authoritative winner); a mis-called join falls into the
        // else-branch and sends toward the host, which has no PKT_CLAIM_VERDICT
        // receive branch (same inert-mis-call shape as PKT_XFER_COMMIT/
        // PKT_OWN_RANKS).
        std::vector<ClaimVerdictPacket> claimVerdicts;
        EnterCriticalSection(&outCs_);
        claimVerdicts.swap(outClaimVerdict_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < claimVerdicts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&claimVerdicts[i], sizeof(ClaimVerdictPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued host-committed cell-claim maps on
        // CH_RELIABLE (protocol 59, WORLD-03). HOST ONLY in practice
        // (queueCellMap's doc comment) - isHost_ true broadcasts to every
        // connected client (the single authoritative verdict); a mis-called
        // join falls into the else-branch and sends toward the host, which
        // rejects it (isHost_ && type == PKT_CELL_MAP receive branch above).
        std::vector<CellMapPacket> cellMaps;
        EnterCriticalSection(&outCs_);
        cellMaps.swap(outCellMap_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < cellMaps.size(); ++i) {
            ENetPacket* out = enet_packet_create(&cellMaps[i], sizeof(CellMapPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued NPC existence censuses on CH_RELIABLE
        // (protocol 36, host -> join, 1 Hz). ENet fragments the large list.
        std::vector<OutNpcCensus> censuses;
        EnterCriticalSection(&outCs_);
        censuses.swap(outNpcCensus_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < censuses.size(); ++i) {
            unsigned count = (unsigned)(censuses[i].hands.size() / 5);
            if (count > NPC_CENSUS_MAX) count = NPC_CENSUS_MAX;
            // v38 layout: hands block then positions block. A queue call that
            // somehow lacked positions still sends a well-formed packet
            // (zeroed positions), never a short one.
            unsigned bytes = sizeof(NpcCensusHeader) + count * 5 * sizeof(u32)
                           + count * 3 * sizeof(float);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            NpcCensusHeader hdr;
            hdr.type    = (u8)PKT_NPC_CENSUS;
            hdr.ownerId = censuses[i].ownerId;
            hdr.count   = (u16)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0) {
                std::memcpy(out->data + sizeof(hdr), &censuses[i].hands[0],
                            count * 5 * sizeof(u32));
                enet_uint8* pp = out->data + sizeof(hdr) + count * 5 * sizeof(u32);
                std::memset(pp, 0, count * 3 * sizeof(float));
                if (censuses[i].pos.size() >= count * 3)
                    std::memcpy(pp, &censuses[i].pos[0], count * 3 * sizeof(float));
            }
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued cross-owner TRANSFER intents on CH_RELIABLE
        // (protocol 37/58). Fixed-size PODs like the drop/pickup intents.
        //
        // Phase 7 Plan 01 Task 2: PKT_INV_XFER is now a host-terminated
        // CLIENT-REQUEST (routingClassOf == RELAY_NONE) - only the HOST's own
        // processXferIntents ever drains InboundInvXfer. When the HOST itself
        // is the author (a host player can drag an item into/out of a join's
        // container exactly like any other cross-owner trade - INV-01
        // 2-player parity requires this), the generic isHost_ ? broadcast
        // branch below would otherwise fan the raw intent out to every
        // connected client, none of which ever drain it anymore (dead
        // traffic, and an unbounded InboundInvXfer growth on every join's
        // Inbound - Class E's own "never blind-broadcast a targeted packet"
        // rule). A host-authored intent never needs the wire at all: loop it
        // directly into the host's OWN Inbound so the SAME
        // processXferIntents drain path picks it up next tick, exactly as if
        // it had arrived over the network from itself.
        std::vector<InvXferPacket> xfers;
        EnterCriticalSection(&outCs_);
        xfers.swap(outInvXfers_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < xfers.size(); ++i) {
            if (isHost_) {
                if (inbound_) inbound_->pushInvXfer(xfers[i].ownerId, xfers[i]);
                continue;
            }
            ENetPacket* out = enet_packet_create(&xfers[i], sizeof(InvXferPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Transfer VERDICTS (protocol 50, superseded), same channel as the intents.
        std::vector<InvXferAckPacket> xferAcks;
        EnterCriticalSection(&outCs_);
        xferAcks.swap(outInvXferAcks_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < xferAcks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&xferAcks[i], sizeof(InvXferAckPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued host-committed transfer verdicts on
        // CH_RELIABLE (protocol 58). HOST ONLY in practice (queueXferCommit's
        // doc comment) - isHost_ true broadcasts to every connected client
        // (the single authoritative verdict); a mis-called join falls into
        // the else-branch and sends toward the host, which has no
        // PKT_XFER_COMMIT receive branch (same inert-mis-call shape as
        // PKT_OWN_RANKS).
        std::vector<XferCommitPacket> xferCommits;
        EnterCriticalSection(&outCs_);
        xferCommits.swap(outXferCommit_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < xferCommits.size(); ++i) {
            ENetPacket* out = enet_packet_create(&xferCommits[i], sizeof(XferCommitPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued transfer-commit bookkeeping acks on
        // CH_RELIABLE (protocol 58). Participant -> host; audit only.
        std::vector<XferCommitAckPacket> xferCommitAcks;
        EnterCriticalSection(&outCs_);
        xferCommitAcks.swap(outXferCommitAck_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < xferCommitAcks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&xferCommitAcks[i], sizeof(XferCommitAckPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued medical snapshots + treatment deltas on
        // CH_RELIABLE (phase 2). Fixed-size PODs like events; change-gated by the
        // Replicator so steady state is silent.
        std::vector<MedicalPacket>   meds;
        std::vector<TreatmentPacket> treats;
        std::vector<CombatHitPacket> combatHits;
        EnterCriticalSection(&outCs_);
        meds.swap(outMedical_);
        treats.swap(outTreatments_);
        combatHits.swap(outCombatHits_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < meds.size(); ++i) {
            ENetPacket* out = enet_packet_create(&meds[i], sizeof(MedicalPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < treats.size(); ++i) {
            ENetPacket* out = enet_packet_create(&treats[i], sizeof(TreatmentPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < combatHits.size(); ++i) {
            ENetPacket* out = enet_packet_create(&combatHits[i], sizeof(CombatHitPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued game-speed packets on CH_RELIABLE (consensus
        // speed sync). Change-gated by the Replicator; a lost SET would leave
        // the engines running at different rates, so reliable is mandatory.
        std::vector<SpeedPacket> speeds;
        EnterCriticalSection(&outCs_);
        speeds.swap(outSpeed_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < speeds.size(); ++i) {
            ENetPacket* out = enet_packet_create(&speeds[i], sizeof(SpeedPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued character-stats snapshots on CH_RELIABLE
        // (protocol 17). Fixed-size PODs; change-gated by the Replicator so
        // steady state is silent (stats creep slowly).
        std::vector<StatsPacket> statPkts;
        EnterCriticalSection(&outCs_);
        statPkts.swap(outStats_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < statPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&statPkts[i], sizeof(StatsPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued money-pool totals on CH_RELIABLE (protocol
        // 52). Fixed-size PODs; change-gated by the Replicator so a settled
        // economy is silent. A lost total would diverge cats until the safety
        // resend, so reliable is the right channel.
        std::vector<MoneyPacket> moneyPkts;
        EnterCriticalSection(&outCs_);
        moneyPkts.swap(outMoney_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < moneyPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&moneyPkts[i], sizeof(MoneyPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued money-pool deltas on CH_RELIABLE (protocol
        // 52, join -> host). These are CONSERVATION intents, not snapshots: a
        // dropped or reordered delta would silently mint or burn cats, which is
        // exactly what the reliable ordered channel prevents.
        std::vector<MoneyDeltaPacket> moneyDeltaPkts;
        EnterCriticalSection(&outCs_);
        moneyDeltaPkts.swap(outMoneyDelta_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < moneyDeltaPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&moneyDeltaPkts[i], sizeof(MoneyDeltaPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued insufficient-funds verdicts on CH_RELIABLE
        // (protocol 60, CONS-01). HOST ONLY in practice (queueMoneyReject's
        // doc comment) - isHost_ true broadcasts to every connected client
        // (the single authoritative reject); a mis-called join falls into
        // the else-branch and sends toward the host, which rejects it
        // (isHost_ && type == PKT_MONEY_REJECT receive branch below), the
        // same inert-mis-call shape as PKT_CLAIM_VERDICT/PKT_CELL_MAP.
        std::vector<MoneyRejectPacket> moneyRejectPkts;
        EnterCriticalSection(&outCs_);
        moneyRejectPkts.swap(outMoneyReject_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < moneyRejectPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&moneyRejectPkts[i], sizeof(MoneyRejectPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued faction-relation rows on CH_RELIABLE
        // (protocol 24). Change-gated by the Replicator; a settled diplomacy
        // is silent. A lost row would diverge hostility until the safety
        // resend, so reliable is the right channel.
        std::vector<FactionPacket> facPkts;
        EnterCriticalSection(&outCs_);
        facPkts.swap(outFaction_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < facPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&facPkts[i], sizeof(FactionPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued game-clock samples on CH_RELIABLE (protocol
        // 25). ~1 Hz host broadcast; ordered-reliable keeps the join's offset
        // estimator from ever seeing samples out of order.
        std::vector<TimePacket> timePkts;
        EnterCriticalSection(&outCs_);
        timePkts.swap(outTime_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < timePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&timePkts[i], sizeof(TimePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued baked-door state rows on CH_RELIABLE
        // (protocol 26). Change-gated by the Replicator; a settled town is
        // silent. A lost row would leave a door diverged until the safety
        // resend, so reliable is the right channel.
        std::vector<DoorPacket> doorPkts;
        EnterCriticalSection(&outCs_);
        doorPkts.swap(outDoor_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < doorPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&doorPkts[i], sizeof(DoorPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued machine state rows on CH_RELIABLE
        // (protocol 33). Host -> joins only in practice (the Replicator only
        // publishes on the host); change-gated + safety-resent by the caller,
        // so a settled base is near-silent.
        std::vector<ProdPacket> prodPkts;
        EnterCriticalSection(&outCs_);
        prodPkts.swap(outProd_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < prodPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&prodPkts[i], sizeof(ProdPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued known-research rows on CH_RELIABLE
        // (protocol 38). Host -> joins only (the Replicator only publishes on
        // the host); first-sight + safety-resent by the caller, so a settled
        // tech tree is near-silent.
        std::vector<ResearchPacket> researchPkts;
        EnterCriticalSection(&outCs_);
        researchPkts.swap(outResearch_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < researchPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&researchPkts[i], sizeof(ResearchPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued ownership-rank announcements on CH_RELIABLE
        // (protocol 57). HOST ONLY (Replicator::announceOwnRanks no-ops on a
        // join) - a join calling this by mistake falls into the else-branch
        // below and sends toward the HOST, which has no PKT_OWN_RANKS receive
        // branch (T-03-06: only the client-apply direction exists), so a
        // mis-called queue here is inert, never a peer-authored ownership claim
        // reaching another client.
        std::vector<OwnRanksPacket> ownRanksPkts;
        EnterCriticalSection(&outCs_);
        ownRanksPkts.swap(outOwnRanks_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < ownRanksPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&ownRanksPkts[i], sizeof(OwnRanksPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued property-deed rows on CH_RELIABLE (protocol
        // 54). SYMMETRIC - either client may buy - so this drains on both
        // sides. First-sight + safety-resent by the caller; a party that owns a
        // stable set of buildings is silent between resends.
        std::vector<DeedPacket> deedPkts;
        EnterCriticalSection(&outCs_);
        deedPkts.swap(outDeed_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < deedPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&deedPkts[i], sizeof(DeedPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued runtime-fixture identity rows on CH_RELIABLE
        // (protocol 55). SYMMETRIC: the pose path is bidirectional, so each side
        // must be able to translate the other's fixture hands.
        std::vector<FixturePacket> fixPkts;
        EnterCriticalSection(&outCs_);
        fixPkts.swap(outFixture_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < fixPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&fixPkts[i], sizeof(FixturePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued placed-building announcements + progress
        // rows on CH_RELIABLE (protocol 27). PLACE is a one-shot describe/mint
        // edge (a lost one strands an invisible building on the peer - the
        // protocol-21 lesson); STATE rows are change-gated by the Replicator
        // (~1 Hz sample, 10 s safety resend while incomplete), so the channel
        // is silent once every site completes. Same-channel ordered-reliable
        // guarantees a STATE row never arrives before its PLACE.
        std::vector<BuildPlacePacket> buildPlacePkts;
        std::vector<BuildStatePacket> buildStatePkts;
        EnterCriticalSection(&outCs_);
        buildPlacePkts.swap(outBuildPlace_);
        buildStatePkts.swap(outBuildState_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < buildPlacePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildPlacePkts[i], sizeof(BuildPlacePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < buildStatePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildStatePkts[i], sizeof(BuildStatePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued placed-building door rows + removals on
        // CH_RELIABLE (protocol 28). Door rows are change-gated by the
        // Replicator (the protocol-26 door cadence on the translated key);
        // a REMOVE is a one-shot edge - losing it would strand a ghost proxy
        // on the peer, so reliable is mandatory.
        std::vector<BuildDoorPacket>   buildDoorPkts;
        std::vector<BuildRemovePacket> buildRemovePkts;
        EnterCriticalSection(&outCs_);
        buildDoorPkts.swap(outBuildDoor_);
        buildRemovePkts.swap(outBuildRemove_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < buildDoorPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildDoorPkts[i], sizeof(BuildDoorPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < buildRemovePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildRemovePkts[i], sizeof(BuildRemovePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued stealth detection-map snapshots on
        // CH_UNRELIABLE (protocol 20). Latest-wins continuous state: loss just
        // delays an arrow refresh until the next throttled snapshot.
        std::vector<StealthPacket> stealthPkts;
        EnterCriticalSection(&outCs_);
        stealthPkts.swap(outStealth_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < stealthPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&stealthPkts[i], sizeof(StealthPacket),
                                                 0 /*unreliable*/);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued camera hints on CH_UNRELIABLE (protocol
        // 43, both directions, ~1 Hz). Latest wins; a lost hint is replaced
        // by the next one a second later.
        std::vector<CamHintPacket> camHints;
        EnterCriticalSection(&outCs_);
        camHints.swap(outCamHint_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < camHints.size(); ++i) {
            ENetPacket* out = enet_packet_create(&camHints[i], sizeof(CamHintPacket),
                                                 0 /*unreliable*/);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Cell claims ride CH_RELIABLE (protocol 49, ~1 Hz): losing one leaves
        // the cell reading as host-authored until the next re-assert, and both
        // sides authoring the same bodies is the failure this channel exists to
        // prevent.
        std::vector<CellClaimPacket> cellClaims;
        EnterCriticalSection(&outCs_);
        cellClaims.swap(outCellClaim_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < cellClaims.size(); ++i) {
            ENetPacket* out = enet_packet_create(&cellClaims[i], sizeof(CellClaimPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued runtime-spawn packets on CH_RELIABLE
        // (protocol 21). Requests are debounced per hand and replies are
        // cache-throttled by the Replicator, so the reliable channel stays
        // quiet; a lost request would strand an invisible enemy on the join,
        // so reliable is mandatory.
        std::vector<SpawnReqPacket>  spawnReqs;
        std::vector<SpawnInfoPacket> spawnInfos;
        EnterCriticalSection(&outCs_);
        spawnReqs.swap(outSpawnReq_);
        spawnInfos.swap(outSpawnInfo_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < spawnReqs.size(); ++i) {
            ENetPacket* out = enet_packet_create(&spawnReqs[i], sizeof(SpawnReqPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < spawnInfos.size(); ++i) {
            ENetPacket* out = enet_packet_create(&spawnInfos[i], sizeof(SpawnInfoPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued coordinated-save packets on CH_BULK (protocol
        // 31; moved off CH_RELIABLE in v44). REQ/ACK are fixed PODs; FILE/DONE
        // carry their variable tails (ENet fragments + reassembles the ~4 KB
        // chunks transparently on the reliable channel). Pacing lives in the
        // SaveXfer sender (~32 chunks queued per 50 ms), so one drain never
        // floods the channel - and CH_BULK keeps the megabytes off CH_RELIABLE,
        // so a live transfer no longer stalls door/money/faction events.
        std::vector<SaveReqPacket>   saveReqs;
        std::vector<OutSaveBegin>    saveBegins;
        std::vector<OutSaveFile>     saveFiles;
        std::vector<OutSaveDone>     saveDones;
        std::vector<SaveAckPacket>   saveAcks;
        EnterCriticalSection(&outCs_);
        saveReqs.swap(outSaveReq_);
        saveBegins.swap(outSaveBegin_);
        saveFiles.swap(outSaveFile_);
        saveDones.swap(outSaveDone_);
        saveAcks.swap(outSaveAck_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < saveReqs.size(); ++i) {
            ENetPacket* out = enet_packet_create(&saveReqs[i], sizeof(SaveReqPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_BULK, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        // Phase 10 Plan 01 (SAVE-01/SAVE-04): BEGIN/FILE/DONE each carry a
        // queue-side destId - OWNER_ID_ALL keeps the historical coordinated-
        // save broadcast (byte-for-byte unchanged at N=2); a specific
        // PlayerId routes via sendTo (a per-client retry unicast, or Plan
        // 02's targeted late-join push). Host-only concept, like every other
        // destId-aware drain - a client (join) has no registry to sendTo, so
        // it always falls through to its one serverPeer_ send regardless of
        // the queued destId.
        for (size_t i = 0; i < saveBegins.size(); ++i) {
            ENetPacket* out = enet_packet_create(&saveBegins[i].pkt, sizeof(SaveBeginPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                if (saveBegins[i].destId == OWNER_ID_ALL) enet_host_broadcast(enetHost_, CH_BULK, out);
                else sendTo(saveBegins[i].destId, out, CH_BULK);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < saveFiles.size(); ++i) {
            unsigned bytes = sizeof(SaveFileHeader) + (unsigned)saveFiles[i].tail.size();
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            std::memcpy(out->data, &saveFiles[i].hdr, sizeof(SaveFileHeader));
            if (!saveFiles[i].tail.empty())
                std::memcpy(out->data + sizeof(SaveFileHeader), &saveFiles[i].tail[0],
                            saveFiles[i].tail.size());
            if (isHost_) {
                if (saveFiles[i].destId == OWNER_ID_ALL) enet_host_broadcast(enetHost_, CH_BULK, out);
                else sendTo(saveFiles[i].destId, out, CH_BULK);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < saveDones.size(); ++i) {
            unsigned bytes = sizeof(SaveDoneHeader)
                           + (unsigned)saveDones[i].crcs.size() * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            std::memcpy(out->data, &saveDones[i].hdr, sizeof(SaveDoneHeader));
            if (!saveDones[i].crcs.empty())
                std::memcpy(out->data + sizeof(SaveDoneHeader), &saveDones[i].crcs[0],
                            saveDones[i].crcs.size() * sizeof(u32));
            if (isHost_) {
                if (saveDones[i].destId == OWNER_ID_ALL) enet_host_broadcast(enetHost_, CH_BULK, out);
                else sendTo(saveDones[i].destId, out, CH_BULK);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < saveAcks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&saveAcks[i], sizeof(SaveAckPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_BULK, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send queued coordinated-load packets (protocol 32) on CH_BULK
        // (v44). All fixed PODs: GO host -> join, REQ/NACK join -> host. They
        // share CH_BULK with the save transfer they gate (a NACK's fallback
        // stream must stay ordered behind its GO), and off CH_RELIABLE so they
        // do not queue behind - or ahead of - live game events. GO carries a
        // queue-side destId (Phase 10 Plan 01), same OWNER_ID_ALL-broadcast-
        // or-sendTo convention as the save packets above.
        std::vector<OutLoadGo>      loadGos;
        std::vector<LoadReqPacket>  loadReqs;
        std::vector<LoadNackPacket> loadNacks;
        EnterCriticalSection(&outCs_);
        loadGos.swap(outLoadGo_);
        loadReqs.swap(outLoadReq_);
        loadNacks.swap(outLoadNack_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < loadGos.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadGos[i].pkt, sizeof(LoadGoPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                if (loadGos[i].destId == OWNER_ID_ALL) enet_host_broadcast(enetHost_, CH_BULK, out);
                else sendTo(loadGos[i].destId, out, CH_BULK);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < loadReqs.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadReqs[i], sizeof(LoadReqPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_BULK, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < loadNacks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadNacks[i], sizeof(LoadNackPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_BULK, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send queued positive coordinated-load ACKs on CH_BULK
        // (protocol 61, join -> host, SAVE-02's missing half of the NACK-
        // only pair).
        std::vector<LoadAckPacket> loadAcks;
        EnterCriticalSection(&outCs_);
        loadAcks.swap(outLoadAck_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < loadAcks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadAcks[i], sizeof(LoadAckPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_BULK, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send queued first-wins arbitration rejects on CH_BULK
        // (protocol 61, HOST ONLY in practice - queueCoordReject's doc
        // comment). Class D unicast to the rejected requester ONLY - never
        // broadcast (a leaked reject to every client would be information
        // disclosure about another client's in-flight request). A mis-called
        // join falls into the else-branch and sends toward the host, which
        // rejects it (the isHost_ && type==PKT_COORD_REJECT receive branch),
        // the same inert-mis-call shape as PKT_CLAIM_VERDICT/PKT_MONEY_REJECT.
        std::vector<CoordRejectPacket> coordRejects;
        EnterCriticalSection(&outCs_);
        coordRejects.swap(outCoordReject_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < coordRejects.size(); ++i) {
            ENetPacket* out = enet_packet_create(&coordRejects[i], sizeof(CoordRejectPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                sendTo(coordRejects[i].requesterId, out, CH_BULK);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_BULK, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + process any marshaled kick requests (Phase 10 Plan 01,
        // SAVE-01): the net-thread-purity drop lever for a client whose
        // save/load transfer exhausted its bounded retries. HOST ONLY - a
        // join has no registry to kick from. The resulting DISCONNECT event
        // drives the EXISTING leave edge (pushLeave -> clearPeerReplication
        // State + roster LEFT) - no new cleanup path here.
        std::vector<u32> kicks;
        EnterCriticalSection(&outCs_);
        kicks.swap(outKick_);
        LeaveCriticalSection(&outCs_);
        if (isHost_) {
            for (size_t i = 0; i < kicks.size(); ++i) {
                std::map<u32, PeerState>::iterator it = registry_.find(kicks[i]);
                if (it != registry_.end() && it->second.peer) {
                    char kb[64];
                    _snprintf(kb, sizeof(kb) - 1, "kickPeer: disconnecting player=%u",
                              (unsigned)kicks[i]);
                    kb[sizeof(kb) - 1] = '\0';
                    netLog(kb);
                    enet_peer_disconnect(it->second.peer, 0);
                }
            }
        }

        // Transmit this peer's owned entities (latest snapshot), chunked so each
        // batch fits one datagram - which on Steam means the 1200 B clamped MTU
        // (ENet would otherwise send the oversized unreliable packet as RELIABLE
        // fragments: retransmits + ordering stalls on the motion stream). Raw UDP
        // keeps the full 17-entity chunk. Unreliable: the newest batch supersedes
        // loss.
        const unsigned batchCap = steam ? ENTITY_BATCH_MAX_STEAM : ENTITY_BATCH_MAX;
        std::vector<EntityState> ents;
        u32  owner = 0;
        u32  stamp = 0;
        bool have  = false;
        EnterCriticalSection(&outCs_);
        ents  = out_;
        owner = outOwner_;
        stamp = outStampMs_;
        have  = haveOut_;
        LeaveCriticalSection(&outCs_);

        if (have && !ents.empty()) {
            for (size_t off = 0; off < ents.size(); off += batchCap) {
                unsigned count = (unsigned)(ents.size() - off);
                if (count > batchCap) count = batchCap;

                unsigned bytes = sizeof(EntityBatchHeader) + count * sizeof(EntityState);
                ENetPacket* out = enet_packet_create(0, bytes, 0 /*unreliable*/);
                EntityBatchHeader hdr;
                hdr.type = (u8)PKT_ENTITY_BATCH; hdr.ownerId = owner; hdr.count = (u8)count;
                hdr.sendMs = stamp;
                hdr.epoch  = (u32)sendEpoch_; // v44: current session epoch
                std::memcpy(out->data, &hdr, sizeof(hdr));
                std::memcpy(out->data + sizeof(hdr), &ents[off], count * sizeof(EntityState));
                if (isHost_) {
                    enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
                } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                    enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
                } else {
                    enet_packet_destroy(out); // no one to send to yet
                }
            }
        }
    }

    // WINDOWS #19 / #22 - tell the other side we are leaving BEFORE the
    // transport goes away. enet_host_destroy() below frees the whole ENetPeer
    // array without putting a single byte on the wire, so pre-fix the remote
    // end kept this connection alive until ENet's own timeout expired
    // (measured ~5.4 s in tools/test-runs/20260912_110440_N3_relink, and 3.49 s
    // / 3.83 s in the live F2 session of 2026-09-12) while this side's
    // reconnect returned in ~4 ms and was handed the NEXT free id. At 2 players
    // that only looks like a cosmetic id change; at 3-4 players the free slot
    // runs out and the next reconnect is refused with "MAX_PLAYERS=4 slots
    // full". Runs on the net thread, still inside threadLoop, while every peer
    // pointer is still valid - it must never move above the loop or below the
    // destroy.
    shutdownPeersGracefully();
    if (enetHost_) { enet_host_destroy(enetHost_); enetHost_ = 0; }
    if (steam) steamp2p::removeEnetHooks();
    InterlockedExchange(&running_, 0);
}

} // namespace coop
