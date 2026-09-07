// nettest - headless multi-peer registry tracer (Phase 2, Plan 01).
//
// Proves the host-side PlayerId-keyed registry end-to-end over REAL ENet UDP
// loopback, linking the actual production NetLink.cpp - not a reimplemented
// ENet loop. One process hosts one NetLink (host) plus several NetLink
// instances (clients), each on its own background net thread exactly as the
// plugin runs it, all talking over 127.0.0.1.
//
// What this locks:
//   1. Three simultaneous clients each get a distinct, stable PlayerId in
//      {1,2,3} (NET-02).
//   2. A 4th client while all 3 slots are occupied is cleanly rejected -
//      enet_peer_disconnect, no WELCOME, no 4th connect event observed by the
//      host (NET-01; replaces the old "admit anyway + warn" behavior) - and the
//      rejected client itself observes the disconnect.
//
// The lowest-free-slot SCAN (the mechanism a freed-slot reconnect would reuse)
// is exercised by every connect above, including the rejected 4th. A full
// disconnect -> slot-freed -> reconnect round trip is NOT proven here: see the
// note above the cleanup section for why, and Plan 03 for where it lands.
//
// Zero game/KenshiLib dependencies. Exit code = number of failed checks.
//
// Build: cmd /c scripts\build_nettest.cmd  ->  dist\nettest.exe

#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <utility>

#include "../plugin/net/NetLink.h"
#include "../plugin/core/Inbound.h"
// leave-queue OWNER_ID_ALL expansion (host-link drop) - pure header, engine-free,
// so the drop leg below runs the SAME code Plugin.cpp's leave drain does.
#include "../plugin/core/PeerRoster.h"
#include "../plugin/CoopLog.h"
#include "../plugin/net/SteamP2P.h"
// Phase 8 review CR-01: the cross-sender deed-collision leg folds the REAL
// host-side arrival order through the exact accept gate the host intake runs
// (gateSeqAcceptPerSender) - pure header, engine-free, same as prototest.
#include "../plugin/sync/ChangeGate.h"

using namespace coop;

// NetLink.cpp logs through coop::logLine/logErrLine/coop::wallClockMs; CoopLog.cpp
// is NOT part of this CRT+ENet-only build, so provide inert/minimal definitions to
// satisfy the linker (mirrors src/prototest/main.cpp's stub-linking pattern).
//
// logLine is NOT a no-op here (Phase 2, Plan 02): the sender-identity-resolution
// check below needs to observe the "recv player=N type=T" line NetLink.cpp now
// emits, so this stub captures every logged line into g_logLines instead of
// discarding it. Called from multiple net threads (host + up to 4 clients)
// concurrently, so it is guarded by its own critical section - independent of
// anything inside NetLink/CoopLog.
static CRITICAL_SECTION g_logCs;
static std::vector<std::string> g_logLines;
namespace coop {
    void logLine(const char* s) {
        EnterCriticalSection(&g_logCs);
        g_logLines.push_back(s ? s : "");
        LeaveCriticalSection(&g_logCs);
    }
    // Phase 3 Plan 01: the relay-boundary REJECT line is logged via netErr()
    // (production code, NetLink.cpp), which calls coop::logErrLine - not
    // coop::logLine. This used to be a no-op stub (errors were discarded), but
    // the forge-reject log-oracle below needs to observe "relay REJECT
    // player=..." lines, so capture error lines into the same g_logLines the
    // needle search already scans (mirrors logLine's capture exactly).
    void logErrLine(const char* s) {
        EnterCriticalSection(&g_logCs);
        g_logLines.push_back(s ? s : "");
        LeaveCriticalSection(&g_logCs);
    }
    unsigned long wallClockMs() { return (unsigned long)GetTickCount(); }
}

// True if any captured log line contains 'needle' (used for the sender-identity
// resolution check - see the sourcePlayerId log line NetLink.cpp emits).
static bool logContains(const char* needle) {
    EnterCriticalSection(&g_logCs);
    bool found = false;
    for (size_t i = 0; i < g_logLines.size(); ++i) {
        if (g_logLines[i].find(needle) != std::string::npos) { found = true; break; }
    }
    LeaveCriticalSection(&g_logCs);
    return found;
}
static bool waitForLogContains(const char* needle, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        if (logContains(needle)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return false;
}

// Mirrors NetLink.cpp's private channel constants (CH_RELIABLE = 0). The new
// sendTo/broadcast/broadcastExcept primitives take a plain int channel index,
// not a named constant exposed by the header, so the test passes the same
// reliable-channel index NetLink itself uses for events.
static const int TEST_CH_RELIABLE = 0;

// NetLink.cpp also calls into coop::steamp2p (Steam P2P transport hooks) from
// unconditional/`if (steam)` call sites even though this test never enables the
// Steam transport (setSteamTransport is never called, so steamPeer_ stays 0 and
// the guarded call sites never fire at runtime) - the calls still need to LINK.
// Inert stubs are correct here: nettest only exercises the UDP path.
namespace coop { namespace steamp2p {
    bool installEnetHooks(int) { return false; }
    void removeEnetHooks() {}
    void tick() {}
} }

static int g_failed = 0;
static int g_total  = 0;

#define CHECK(name, cond) do { \
    ++g_total; \
    if (cond) { std::printf("  ok   %s\n", name); } \
    else      { std::printf("  FAIL %s\n", name); ++g_failed; } \
} while (0)

// ---- Host-side accumulation helpers ----------------------------------------------
// Inbound::drainConnects/drainLeaves SWAP the queue empty, so a poller must
// accumulate across calls rather than assume everything arrives in one drain.

static std::deque<u32> g_hostConnects;
static std::deque<u32> g_hostLeaves;

static void drainInto(Inbound& inbound) {
    std::deque<u32> c, l;
    inbound.drainConnects(c);
    inbound.drainLeaves(l);
    for (size_t i = 0; i < c.size(); ++i) g_hostConnects.push_back(c[i]);
    for (size_t i = 0; i < l.size(); ++i) g_hostLeaves.push_back(l[i]);
}

// Poll until at least 'want' connect events have accumulated, or timeout.
static bool waitForConnectCount(Inbound& hostInbound, size_t want, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainInto(hostInbound);
        if (g_hostConnects.size() >= want) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return g_hostConnects.size() >= want;
}

// ---- Client-side event accumulation helpers (Plan 02: sendTo/broadcast/broadcastExcept
// delivery proofs) --------------------------------------------------------------------
// Same swap-drain-and-accumulate shape as drainInto/waitForConnectCount above,
// but for InboundEvent (what a client observes when the host calls one of the
// three new send primitives with a PKT_EVENT payload).

static void drainEventsInto(Inbound& inbound, std::deque<InboundEvent>& acc) {
    std::deque<InboundEvent> batch;
    inbound.drainEvents(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}

static bool hasEventId(const std::deque<InboundEvent>& acc, u32 eventId) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].ev.eventId == eventId) return true;
    }
    return false;
}

// WR-01 fix: mirrors hasEntityFromOwner (below) for InboundEvent - lets a
// forged-owner test assert the claimed ownerId never landed in a queue at
// all, not just that a specific eventId is absent.
static bool hasEventFromOwner(const std::deque<InboundEvent>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].ownerId == ownerId) return true;
    }
    return false;
}

// Poll until 'eventId' has been accumulated for this client, or timeout.
static bool waitForEventId(Inbound& inbound, std::deque<InboundEvent>& acc,
                            u32 eventId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainEventsInto(inbound, acc);
        if (hasEventId(acc, eventId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasEventId(acc, eventId);
}

// ---- Plan 03 helpers: roster (pushConnect/pushLeave mirrors), and entity-batch
// acceptance (the observable signature of epochSeen_ accept/reject) -------------------

static bool hasU32(const std::deque<u32>& acc, u32 v) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i] == v) return true; }
    return false;
}
static void drainConnectsInto(Inbound& inbound, std::deque<u32>& acc) {
    std::deque<u32> batch;
    inbound.drainConnects(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static void drainLeavesInto(Inbound& inbound, std::deque<u32>& acc) {
    std::deque<u32> batch;
    inbound.drainLeaves(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
// Poll a CLIENT's Inbound for a roster PKT_PLAYER_JOINED observation of 'wantId'
// (mirrors pushConnect - see NetLink.cpp's PKT_PLAYER_JOINED receive branch).
static bool waitForConnectId(Inbound& inbound, std::deque<u32>& acc,
                              u32 wantId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainConnectsInto(inbound, acc);
        if (hasU32(acc, wantId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasU32(acc, wantId);
}
// Poll a CLIENT's Inbound for a roster PKT_PLAYER_LEFT observation of 'wantId'
// (mirrors pushLeave). Generous timeout: the host only learns of a stopped
// client via ENet's own peer-timeout detection (NetLink::stop() tears the
// local ENet host down with no wire-level graceful disconnect - the same
// documented gap Plan 01 flagged), which fires deterministically but not
// instantly (ENet's default timeoutMinimum is 5000ms, per third_party/enet's
// vendored protocol.c).
static bool waitForLeaveId(Inbound& inbound, std::deque<u32>& acc,
                            u32 wantId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainLeavesInto(inbound, acc);
        if (hasU32(acc, wantId)) return true;
        Sleep(50);
    } while (GetTickCount() < deadline);
    return hasU32(acc, wantId);
}
// Same ENet-timeout wait, but against the HOST's accumulated leave queue
// (g_hostLeaves, filled by drainInto - declared above).
static bool waitForHostLeaveId(Inbound& hostInbound, u32 wantId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainInto(hostInbound);
        if (hasU32(g_hostLeaves, wantId)) return true;
        Sleep(50);
    } while (GetTickCount() < deadline);
    return hasU32(g_hostLeaves, wantId);
}
// Phase 7 Plan 01 Task 3: waitForHostLeaveId is presence-only - useless for a
// SLOT REUSED multiple times in one run (g_hostLeaves accumulates across the
// whole process, so "2" already appears after its FIRST departure and every
// later reclaim-then-redepart of the same freed slot is invisible to a
// presence check). Counts OCCURRENCES of wantId in g_hostLeaves instead, so
// the caller can wait for the Nth distinct leave of a repeatedly-reused id.
static bool waitForHostLeaveCount(Inbound& hostInbound, u32 wantId, int wantCount,
                                   DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainInto(hostInbound);
        int count = 0;
        for (size_t i = 0; i < g_hostLeaves.size(); ++i) if (g_hostLeaves[i] == wantId) ++count;
        if (count >= wantCount) return true;
        Sleep(50);
    } while (GetTickCount() < deadline);
    int finalCount = 0;
    for (size_t i = 0; i < g_hostLeaves.size(); ++i) if (g_hostLeaves[i] == wantId) ++finalCount;
    return finalCount >= wantCount;
}

static void drainEntitiesInto(Inbound& inbound, std::deque<InboundEntity>& acc) {
    std::deque<InboundEntity> batch;
    inbound.drainEntities(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasEntityFromOwner(const std::deque<InboundEntity>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}

// Phase 6 (GAP-1): mirrors drainEntitiesInto/hasEntityFromOwner/
// waitForEntityFromOwner for InboundTreatment - the treatment relay-loopback
// legs below need the same accumulate-and-poll shape.
static void drainTreatmentsInto(Inbound& inbound, std::deque<InboundTreatment>& acc) {
    std::deque<InboundTreatment> batch;
    inbound.drainTreatments(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasTreatmentFromOwner(const std::deque<InboundTreatment>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
static bool waitForTreatmentFromOwner(Inbound& inbound, std::deque<InboundTreatment>& acc,
                                       u32 ownerId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainTreatmentsInto(inbound, acc);
        if (hasTreatmentFromOwner(acc, ownerId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasTreatmentFromOwner(acc, ownerId);
}

// Phase 7 Plan 01: mirrors drainTreatmentsInto/hasTreatmentFromOwner for
// InboundInvXfer (the cross-owner transfer intent, protocol 37/58) and
// InboundXferCommit (the host's single broadcast verdict, protocol 58) - the
// tracer leg below needs the same accumulate-and-poll shape to prove
// host-terminated-not-relayed and broadcast-to-all respectively.
static void drainInvXfersInto(Inbound& inbound, std::deque<InboundInvXfer>& acc) {
    std::deque<InboundInvXfer> batch;
    inbound.drainInvXfers(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasInvXferFromOwner(const std::deque<InboundInvXfer>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
static void drainXferCommitsInto(Inbound& inbound, std::deque<InboundXferCommit>& acc) {
    std::deque<InboundXferCommit> batch;
    inbound.drainXferCommits(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasXferCommitFor(const std::deque<InboundXferCommit>& acc,
                              u32 authorId, u32 transferId) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].pkt.authorId == authorId && acc[i].pkt.transferId == transferId) return true;
    }
    return false;
}
static bool waitForXferCommitFor(Inbound& inbound, std::deque<InboundXferCommit>& acc,
                                  u32 authorId, u32 transferId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainXferCommitsInto(inbound, acc);
        if (hasXferCommitFor(acc, authorId, transferId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasXferCommitFor(acc, authorId, transferId);
}
// Phase 7 Plan 01 Task 3: mirrors the InvXfer/XferCommit drain helpers above
// for InboundXferCommitAck (protocol 58, host-side bookkeeping queue) - the
// forged-owner reject leg needs to assert it never landed at the host.
static void drainXferCommitAcksInto(Inbound& inbound, std::deque<InboundXferCommitAck>& acc) {
    std::deque<InboundXferCommitAck> batch;
    inbound.drainXferCommitAcks(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasXferCommitAckFromOwner(const std::deque<InboundXferCommitAck>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
// Phase 7 Plan 02: mirrors the InvXfer/XferCommit drain helpers above for
// InboundWorldClaim (the claim INTENT, protocol 58) and InboundClaimVerdict
// (the host's single broadcast winner) - the contention legs below need the
// same accumulate-and-poll shape to prove host-terminated-not-relayed and
// one-verdict-per-contention respectively.
static void drainWorldClaimsInto(Inbound& inbound, std::deque<InboundWorldClaim>& acc) {
    std::deque<InboundWorldClaim> batch;
    inbound.drainWorldClaim(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasWorldClaimFromOwnerFor(const std::deque<InboundWorldClaim>& acc,
                                       u32 ownerId, u32 authorId, u32 netId) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].ownerId != ownerId || acc[i].authorId != authorId) continue;
        for (size_t j = 0; j < acc[i].netIds.size(); ++j)
            if (acc[i].netIds[j] == netId) return true;
    }
    return false;
}
static bool waitForWorldClaimFromOwnerFor(Inbound& inbound, std::deque<InboundWorldClaim>& acc,
                                           u32 ownerId, u32 authorId, u32 netId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainWorldClaimsInto(inbound, acc);
        if (hasWorldClaimFromOwnerFor(acc, ownerId, authorId, netId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasWorldClaimFromOwnerFor(acc, ownerId, authorId, netId);
}
static void drainClaimVerdictsInto(Inbound& inbound, std::deque<InboundClaimVerdict>& acc) {
    std::deque<InboundClaimVerdict> batch;
    inbound.drainClaimVerdicts(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasClaimVerdictFor(const std::deque<InboundClaimVerdict>& acc,
                                u32 authorId, u32 netId) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].pkt.authorId == authorId && acc[i].pkt.netId == netId) return true;
    }
    return false;
}
static unsigned int countClaimVerdictsFor(const std::deque<InboundClaimVerdict>& acc,
                                           u32 authorId, u32 netId) {
    unsigned int n = 0;
    for (size_t i = 0; i < acc.size(); ++i)
        if (acc[i].pkt.authorId == authorId && acc[i].pkt.netId == netId) ++n;
    return n;
}
static bool waitForClaimVerdictFor(Inbound& inbound, std::deque<InboundClaimVerdict>& acc,
                                    u32 authorId, u32 netId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainClaimVerdictsInto(inbound, acc);
        if (hasClaimVerdictFor(acc, authorId, netId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasClaimVerdictFor(acc, authorId, netId);
}
// Phase 5 (ID-01/T-05-02): drain the host's money-delta inbound queue,
// accumulating across calls (mirrors drainEntitiesInto's accumulate-not-
// replace shape, needed because a single drain can race a still-in-flight
// UDP send).
static void drainMoneyDeltasInto(Inbound& inbound, std::deque<InboundMoneyDelta>& acc) {
    std::deque<InboundMoneyDelta> batch;
    inbound.drainMoneyDeltas(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasMoneyDeltaTuple(const std::deque<InboundMoneyDelta>& acc, u32 owner, u32 seq) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].pkt.ownerId == owner && acc[i].pkt.seq == seq) return true;
    }
    return false;
}
// Phase 9 Plan 01 Task 2 (CONS-01): mirrors drainMoneyDeltasInto for the
// host's MoneyPacket total/ack-vector broadcast - the ack-vector
// personalization leg needs the accumulate-and-poll shape too.
static void drainMoneyInto(Inbound& inbound, std::deque<InboundMoney>& acc) {
    std::deque<InboundMoney> batch;
    inbound.drainMoney(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool waitForMoneyTotal(Inbound& inbound, std::deque<InboundMoney>& acc, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainMoneyInto(inbound, acc);
        if (!acc.empty()) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return !acc.empty();
}
// Phase 9 Plan 01 (CONS-01): mirrors drainMoneyDeltasInto/hasMoneyDeltaTuple
// for InboundMoneyReject - the insufficient-funds verdict broadcast reach
// leg needs the same accumulate-and-poll shape.
static void drainMoneyRejectsInto(Inbound& inbound, std::deque<InboundMoneyReject>& acc) {
    std::deque<InboundMoneyReject> batch;
    inbound.drainMoneyRejects(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasMoneyRejectFor(const std::deque<InboundMoneyReject>& acc, u32 buyerId, u32 seq) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].pkt.buyerId == buyerId && acc[i].pkt.seq == seq) return true;
    }
    return false;
}
static bool waitForMoneyRejectFor(Inbound& inbound, std::deque<InboundMoneyReject>& acc,
                                   u32 buyerId, u32 seq, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainMoneyRejectsInto(inbound, acc);
        if (hasMoneyRejectFor(acc, buyerId, seq)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasMoneyRejectFor(acc, buyerId, seq);
}
// Phase 9 Plan 02 (CONS-02): mirrors drainMoneyDeltasInto/hasMoneyDeltaTuple
// for InboundSpeed - the vote-drop-on-disconnect, per-sender-seq, and
// forged-speed legs below all need the same accumulate-and-poll shape.
static void drainSpeedInto(Inbound& inbound, std::deque<InboundSpeed>& acc) {
    std::deque<InboundSpeed> batch;
    inbound.drainSpeed(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasSpeedTuple(const std::deque<InboundSpeed>& acc, u32 owner, u32 seq) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].pkt.ownerId == owner && acc[i].pkt.seq == seq) return true;
    }
    return false;
}
static bool waitForSpeedTuple(Inbound& inbound, std::deque<InboundSpeed>& acc,
                                u32 owner, u32 seq, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainSpeedInto(inbound, acc);
        if (hasSpeedTuple(acc, owner, seq)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasSpeedTuple(acc, owner, seq);
}
// Phase 9 Plan 02 (CONS-03): mirrors drainSpeedInto/hasSpeedTuple for
// InboundTime - the time-report-scoping leg needs the same shape.
static void drainTimeInto(Inbound& inbound, std::deque<InboundTime>& acc) {
    std::deque<InboundTime> batch;
    inbound.drainTime(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasTimeTuple(const std::deque<InboundTime>& acc, u32 owner, u32 seq) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].pkt.ownerId == owner && acc[i].pkt.seq == seq) return true;
    }
    return false;
}

// Poll a client's NetLink::localId() until it reaches 'wantId' or times out
// (localId() is set by the NET thread on WELCOME, InterlockedExchange'd - see
// NetLink.h's own cross-thread-safety comment on myId_).
static bool waitForLocalId(NetLink& link, u32 wantId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        if (link.localId() == wantId) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return link.localId() == wantId;
}

// Poll the HOST's Inbound for an entity batch from 'ownerId'. setOwnedEntities()
// re-sends every ~50ms tick as long as haveOut_ stays true, so this is the
// observable proxy for "acceptEpoch(ownerId, epoch) returned true" - a batch
// that acceptEpoch REJECTS never reaches Inbound at all (see NetLink.cpp's
// PKT_ENTITY_BATCH receive branch: deliverEntity() is only called inside the
// 'acceptEpoch(...)' guard).
static bool waitForEntityFromOwner(Inbound& inbound, std::deque<InboundEntity>& acc,
                                    u32 ownerId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainEntitiesInto(inbound, acc);
        if (hasEntityFromOwner(acc, ownerId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasEntityFromOwner(acc, ownerId);
}

// Phase 8 Plan 01 Task 1: mirrors drainEntitiesInto/hasEntityFromOwner for
// InboundDoor (protocol 26, Class A symmetric channel) - the multi-author
// door routing leg below needs to assert BOTH authors' rows reached the
// third client, matched by (ownerId, hand), not just ownerId presence (two
// different door hands could otherwise satisfy an ownerId-only check).
static void drainDoorsInto(Inbound& inbound, std::deque<InboundDoor>& acc) {
    std::deque<InboundDoor> batch;
    inbound.drainDoor(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasDoorFromOwnerHand(const std::deque<InboundDoor>& acc, u32 ownerId,
                                  const u32 hand[5]) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].ownerId != ownerId) continue;
        bool same = true;
        for (int h = 0; h < 5; ++h) if (acc[i].pkt.hand[h] != hand[h]) { same = false; break; }
        if (same) return true;
    }
    return false;
}
static bool waitForDoorFromOwnerHand(Inbound& inbound, std::deque<InboundDoor>& acc,
                                      u32 ownerId, const u32 hand[5], DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainDoorsInto(inbound, acc);
        if (hasDoorFromOwnerHand(acc, ownerId, hand)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasDoorFromOwnerHand(acc, ownerId, hand);
}

// Phase 8 Plan 01 Task 2: mirrors drainDoorsInto/hasDoorFromOwnerHand for
// InboundBuildDoor (protocol 28, Class A symmetric channel on the placer-key
// translated identity) - the build-door multi-author routing leg below needs
// the same (ownerId, bkey) match shape.
static void drainBuildDoorsInto(Inbound& inbound, std::deque<InboundBuildDoor>& acc) {
    std::deque<InboundBuildDoor> batch;
    inbound.drainBuildDoor(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasBuildDoorFromOwnerKey(const std::deque<InboundBuildDoor>& acc, u32 ownerId,
                                      const u32 bkey[5]) {
    for (size_t i = 0; i < acc.size(); ++i) {
        if (acc[i].ownerId != ownerId) continue;
        bool same = true;
        for (int h = 0; h < 5; ++h) if (acc[i].pkt.bkey[h] != bkey[h]) { same = false; break; }
        if (same) return true;
    }
    return false;
}
static bool waitForBuildDoorFromOwnerKey(Inbound& inbound, std::deque<InboundBuildDoor>& acc,
                                          u32 ownerId, const u32 bkey[5], DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainBuildDoorsInto(inbound, acc);
        if (hasBuildDoorFromOwnerKey(acc, ownerId, bkey)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasBuildDoorFromOwnerKey(acc, ownerId, bkey);
}

// Phase 8 Plan 01 Task 2: mirrors drainInvXfersInto/hasInvXferFromOwner (the
// host-terminated-intent shape) for InboundFaction (protocol 24) and
// InboundDeed (protocol 54) - both now RELAY_NONE join-intent channels; the
// legs below need to assert the intent reaches the HOST ONLY.
static void drainFactionsInto(Inbound& inbound, std::deque<InboundFaction>& acc) {
    std::deque<InboundFaction> batch;
    inbound.drainFaction(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasFactionFromOwner(const std::deque<InboundFaction>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
static void drainDeedsInto(Inbound& inbound, std::deque<InboundDeed>& acc) {
    std::deque<InboundDeed> batch;
    inbound.drainDeed(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasDeedFromOwner(const std::deque<InboundDeed>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
// Phase 8 Plan 01 Task 3: mirrors drainFactionsInto/hasFactionFromOwner for
// InboundResearch (protocol 38) - the research join->host intent leg below
// needs the same host-terminated-intent assertion shape.
static void drainResearchInto(Inbound& inbound, std::deque<InboundResearch>& acc) {
    std::deque<InboundResearch> batch;
    inbound.drainResearch(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasResearchFromOwner(const std::deque<InboundResearch>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}

// Phase 8 Plan 02 (WORLD-03): mirrors drainFactionsInto/hasFactionFromOwner
// for InboundCellClaim (protocol 49, now Class C host-terminated) - the
// claim-host-terminated leg below needs the same host-terminated-intent
// assertion shape.
static void drainCellClaimsInto(Inbound& inbound, std::deque<InboundCellClaim>& acc) {
    std::deque<InboundCellClaim> batch;
    inbound.drainCellClaims(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasCellClaimFromOwner(const std::deque<InboundCellClaim>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
static bool waitForCellClaimFromOwner(Inbound& inbound, std::deque<InboundCellClaim>& acc,
                                       u32 ownerId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainCellClaimsInto(inbound, acc);
        if (hasCellClaimFromOwner(acc, ownerId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasCellClaimFromOwner(acc, ownerId);
}

// Mirrors drainClaimVerdictsInto/hasClaimVerdictFor for InboundCellMap
// (protocol 59, host-broadcast) - the map-broadcast/adoption leg below
// needs the same host-single-sender receipt-count shape.
static void drainCellMapsInto(Inbound& inbound, std::deque<InboundCellMap>& acc) {
    std::deque<InboundCellMap> batch;
    inbound.drainCellMaps(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool waitForCellMap(Inbound& inbound, std::deque<InboundCellMap>& acc, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainCellMapsInto(inbound, acc);
        if (!acc.empty()) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return !acc.empty();
}

// Phase 8 Plan 02 Task 3 (WORLD-03): mirrors drainFactionsInto/
// hasFactionFromOwner for InboundNpcCensus (protocol 36/59, now Class A
// relay) - the census-relay leg needs the same owner-tagged receipt shape.
static void drainNpcCensusInto(Inbound& inbound, std::deque<InboundNpcCensus>& acc) {
    std::deque<InboundNpcCensus> batch;
    inbound.drainNpcCensus(batch);
    for (size_t i = 0; i < batch.size(); ++i) acc.push_back(batch[i]);
}
static bool hasNpcCensusFromOwner(const std::deque<InboundNpcCensus>& acc, u32 ownerId) {
    for (size_t i = 0; i < acc.size(); ++i) { if (acc[i].ownerId == ownerId) return true; }
    return false;
}
static bool waitForNpcCensusFromOwner(Inbound& inbound, std::deque<InboundNpcCensus>& acc,
                                       u32 ownerId, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    do {
        drainNpcCensusInto(inbound, acc);
        if (hasNpcCensusFromOwner(acc, ownerId)) return true;
        Sleep(20);
    } while (GetTickCount() < deadline);
    return hasNpcCensusFromOwner(acc, ownerId);
}

// Build a one-shot EventPacket (EVT_KNOCKOUT is arbitrary - any EventType works,
// only .eventId is asserted on by these checks) ready to hand to sendTo/broadcast/
// broadcastExcept, which take ownership of the returned ENetPacket*.
static ENetPacket* makeTestEvent(u32 eventId, u32 ownerId) {
    EventPacket ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type    = (u8)PKT_EVENT;
    ev.event   = (u8)EVT_KNOCKOUT;
    ev.ownerId = ownerId;
    ev.eventId = eventId;
    return enet_packet_create(&ev, sizeof(ev), ENET_PACKET_FLAG_RELIABLE);
}

int main() {
    InitializeCriticalSection(&g_logCs);

    // ---- Phase 3 Plan 01 Task 3: NetLink::routingClassOf() drift oracle -------
    // Pins the code's routing-class table against docs/ROUTING_MATRIX.md's Class
    // column for a representative spanning set (this phase's own routing matrix
    // "Verification Strategy" #2). routingClassOf() is pure/static/game-free -
    // no sockets needed - so this runs before ENet even starts. Fails loudly if
    // a future edit reclassifies a packet in code without updating the matrix.
    {
        struct RoutingCase { u8 type; RelayClass expected; const char* label; };
        static const RoutingCase kRoutingCases[] = {
            { (u8)PKT_ENTITY_BATCH,     RELAY_BROADCAST_EXCEPT,
              "routingClassOf(PKT_ENTITY_BATCH) == RELAY_BROADCAST_EXCEPT (Class A)" },
            { (u8)PKT_EVENT,            RELAY_BROADCAST_EXCEPT,
              "routingClassOf(PKT_EVENT) == RELAY_BROADCAST_EXCEPT (Class A)" },
            { (u8)PKT_SPEED_SET,        RELAY_NONE,
              "routingClassOf(PKT_SPEED_SET) == RELAY_NONE (Class B)" },
            { (u8)PKT_MONEY,            RELAY_NONE,
              "routingClassOf(PKT_MONEY) == RELAY_NONE (Class B)" },
            { (u8)PKT_TIME_PING,        RELAY_NONE,
              "routingClassOf(PKT_TIME_PING) == RELAY_NONE (Class C)" },
            { (u8)PKT_SPEED_REQ,        RELAY_NONE,
              "routingClassOf(PKT_SPEED_REQ) == RELAY_NONE (Class C)" },
            { (u8)PKT_INV_XFER_ACK,     RELAY_UNICAST,
              "routingClassOf(PKT_INV_XFER_ACK) == RELAY_UNICAST (already-addressable)" },
            { (u8)PKT_INV_XFER,         RELAY_NONE,
              "routingClassOf(PKT_INV_XFER) == RELAY_NONE (Phase 7: host-terminated intent, not FAILSAFE)" },
            { (u8)PKT_XFER_COMMIT,      RELAY_NONE,
              "routingClassOf(PKT_XFER_COMMIT) == RELAY_NONE (Class B, host-broadcast, protocol 58)" },
            { (u8)PKT_XFER_COMMIT_ACK,  RELAY_NONE,
              "routingClassOf(PKT_XFER_COMMIT_ACK) == RELAY_NONE (Class C, client-request, protocol 58)" },
            { (u8)PKT_WORLD_ITEM_CLAIM, RELAY_NONE,
              "routingClassOf(PKT_WORLD_ITEM_CLAIM) == RELAY_NONE (Phase 7 Plan 02: host-terminated claim intent)" },
            { (u8)PKT_CLAIM_VERDICT,    RELAY_NONE,
              "routingClassOf(PKT_CLAIM_VERDICT) == RELAY_NONE (Class B, host-broadcast, protocol 58)" },
            { (u8)PKT_STEALTH,          RELAY_BROADCAST_EXCEPT,
              "routingClassOf(PKT_STEALTH) == RELAY_BROADCAST_EXCEPT (Class A, Phase 6 GAP-2)" },
            { (u8)PKT_TREATMENT,        RELAY_BROADCAST_EXCEPT,
              "routingClassOf(PKT_TREATMENT) == RELAY_BROADCAST_EXCEPT (Class A, Phase 6 GAP-1)" },
            { (u8)PKT_SPAWN_INFO,       RELAY_FAILSAFE_LOG,
              "routingClassOf(PKT_SPAWN_INFO) == RELAY_FAILSAFE_LOG" },
            { (u8)PKT_SAVE_BEGIN,       RELAY_FAILSAFE_LOG,
              "routingClassOf(PKT_SAVE_BEGIN) == RELAY_FAILSAFE_LOG" },
            { (u8)PKT_DOOR,             RELAY_BROADCAST_EXCEPT,
              "routingClassOf(PKT_DOOR) == RELAY_BROADCAST_EXCEPT (Class A, Phase 8: stays "
              "symmetric - only the apply-side seq accept goes per-sender)" },
            { (u8)PKT_FACTION,          RELAY_NONE,
              "routingClassOf(PKT_FACTION) == RELAY_NONE (Phase 8: host-terminated intent, "
              "was Class A)" },
            { (u8)PKT_DEED,             RELAY_NONE,
              "routingClassOf(PKT_DEED) == RELAY_NONE (Phase 8: host-terminated intent, "
              "was Class A)" },
            { (u8)PKT_RESEARCH,         RELAY_NONE,
              "routingClassOf(PKT_RESEARCH) == RELAY_NONE (already host-terminated pre-"
              "Phase-8; Phase 8 adds a join->host intent leg on the SAME class, no "
              "re-point)" },
            { (u8)PKT_CELL_CLAIM,       RELAY_NONE,
              "routingClassOf(PKT_CELL_CLAIM) == RELAY_NONE (Phase 8 Plan 02, WORLD-03: "
              "host-terminated claim intent, was Class A)" },
            { (u8)PKT_CELL_MAP,         RELAY_NONE,
              "routingClassOf(PKT_CELL_MAP) == RELAY_NONE (Class B, host-broadcast, "
              "protocol 59, WORLD-03)" },
            { (u8)PKT_MONEY_REJECT,     RELAY_NONE,
              "routingClassOf(PKT_MONEY_REJECT) == RELAY_NONE (Class B, host-broadcast, "
              "protocol 60, CONS-01)" },
            { (u8)PKT_NPC_CENSUS,       RELAY_BROADCAST_EXCEPT,
              "routingClassOf(PKT_NPC_CENSUS) == RELAY_BROADCAST_EXCEPT (Class A, "
              "Phase 8 Plan 02 Task 3, WORLD-03: was Class B host-only, now relayed "
              "join->join for the per-owner census intake)" },
        };
        const size_t kRoutingCaseCount = sizeof(kRoutingCases) / sizeof(kRoutingCases[0]);
        for (size_t i = 0; i < kRoutingCaseCount; ++i) {
            RelayClass got = NetLink::routingClassOf(kRoutingCases[i].type);
            CHECK(kRoutingCases[i].label, got == kRoutingCases[i].expected);
        }
    }

    std::printf("nettest: multi-peer registry tracer - host + 3 clients get unique "
                "PlayerIds, 4th rejected (real ENet UDP loopback)\n");

    const int PORT = 28100;

    Inbound hostInbound;
    NetLink host;
    CHECK("host startHost", host.startHost(PORT, &hostInbound));

    // Give the host a moment to bind before clients dial in.
    Sleep(200);

    Inbound c1Inbound, c2Inbound, c3Inbound;
    NetLink client1, client2, client3;
    CHECK("client1 startClient", client1.startClient("127.0.0.1", PORT, &c1Inbound));
    CHECK("client2 startClient", client2.startClient("127.0.0.1", PORT, &c2Inbound));
    CHECK("client3 startClient", client3.startClient("127.0.0.1", PORT, &c3Inbound));

    CHECK("host observed 3 connect events within 5s",
          waitForConnectCount(hostInbound, 3, 5000));

    std::set<u32> ids;
    bool allInRange = true;
    for (size_t i = 0; i < g_hostConnects.size(); ++i) {
        ids.insert(g_hostConnects[i]);
        if (g_hostConnects[i] < 1 || g_hostConnects[i] > 3) allInRange = false;
    }
    CHECK("exactly 3 connect events observed", g_hostConnects.size() == 3);
    CHECK("3 distinct PlayerIds assigned", ids.size() == 3);
    CHECK("all assigned PlayerIds within {1,2,3}", allInRange);

    // Cross-check: each client's own learned id (via NetLink::localId(), set from
    // the WELCOME it received) matches the set the host observed.
    Sleep(200);
    std::set<u32> clientIds;
    clientIds.insert(client1.localId());
    clientIds.insert(client2.localId());
    clientIds.insert(client3.localId());
    CHECK("client-learned ids match host-assigned ids", clientIds == ids);

    // 4th client: all 3 join slots are occupied - must be rejected cleanly (no
    // WELCOME, no registry insert, no 4th connect event on the host).
    Inbound c4Inbound;
    NetLink client4;
    CHECK("client4 startClient", client4.startClient("127.0.0.1", PORT, &c4Inbound));

    Sleep(1500); // allow the HELLO -> reject round-trip to complete
    drainInto(hostInbound);
    CHECK("host emitted no 4th connect event (rejected)", g_hostConnects.size() == 3);

    // CR-01 regression: a peer rejected before it ever completes HELLO (this
    // 4th-client-over-MAX_PLAYERS rejection, same as a protocol-version
    // mismatch) must NOT be misattributed as PlayerId 0 (the host) on
    // DISCONNECT. ev.peer->data is only ever assigned a real id in the
    // HELLO-success branch, so a never-assigned peer reads the same
    // zero-initialized default ENet gives every peer - the host's own id.
    // A buggy unconditional teardown would push a spurious leave(0) into the
    // host's own Inbound and broadcast PKT_PLAYER_LEFT(0) to every real
    // connected client.
    CHECK("CR-01: host's leave queue stayed empty (rejected peer never had an id)",
          g_hostLeaves.empty());
    std::deque<u32> c1LeavesEarly, c2LeavesEarly, c3LeavesEarly;
    drainLeavesInto(c1Inbound, c1LeavesEarly);
    drainLeavesInto(c2Inbound, c2LeavesEarly);
    drainLeavesInto(c3Inbound, c3LeavesEarly);
    CHECK("CR-01: no real client received a spurious PKT_PLAYER_LEFT(0) for the rejected peer",
          !hasU32(c1LeavesEarly, 0) && !hasU32(c2LeavesEarly, 0) && !hasU32(c3LeavesEarly, 0));

    // The rejected client must observe a disconnect - it never reaches gameplay.
    Sleep(300);
    std::deque<u32> c4Leaves;
    c4Inbound.drainLeaves(c4Leaves);
    CHECK("rejected 4th client observed a disconnect", !c4Leaves.empty());
    client4.stop();

    // NOTE on slot reuse: the lowest-free-slot SCAN this tracer proves above (every
    // connect, including the rejected 4th, runs it) is the same code path a
    // reconnect after a freed slot would use - per plan, that is all this tracer
    // needs to exercise. A full disconnect -> slot-freed -> reconnect round trip is
    // NOT proven here: NetLink::stop() tears the ENet host down locally
    // (enet_host_destroy) without a wire-level graceful disconnect, so the host
    // only learns of the peer's absence via ENet's default multi-second-to-30s
    // timeout detection - too slow and non-deterministic for this unit-test tier.
    // Full reconnect-stability semantics (including timely disconnect detection)
    // are Plan 03 scope, per the phase CONTEXT.md decisions.

    // ---- Plan 02: sendTo / broadcast / broadcastExcept delivery + sender
    // resolution ------------------------------------------------------------
    // Reuses the still-connected host + 3-client topology above. The three
    // primitives are net-thread-only by contract (NetLink.h), but are called
    // directly here from the test's main thread purely as a headless-test
    // scaffolding simplification: no other thread is racing these specific
    // registry_ entries at these moments (the net thread is idle-blocked in
    // enet_host_service between the generous Sleep()s below), and iterating
    // the battery (below) is exactly what would surface a genuine race or a
    // refcount double-free/leak as a crash. Production callers (Phase 3's
    // relay layer) will call these from inside threadLoop() itself, already
    // on the net thread - no additional synchronization needed there.
    std::printf("\n-- sendTo / broadcast / broadcastExcept delivery + sender resolution --\n");

    NetLink*   clientLinks[3]   = { &client1, &client2, &client3 };
    Inbound*   clientInboxes[3] = { &c1Inbound, &c2Inbound, &c3Inbound };
    u32        clientIdsArr[3]  = { client1.localId(), client2.localId(), client3.localId() };
    std::deque<InboundEvent> clientEvAcc[3];

    const int  ITERS = 5; // "a handful" - enough to surface a broadcastExcept
                           // refcount double-free/leak as a crash, per the plan
    bool broadcastOk = true, exceptOk = true, sendToOk = true;
    for (int iter = 0; iter < ITERS; ++iter) {
        // broadcast(): every one of the 3 connected clients receives it.
        {
            u32 evId = 20000u + (u32)iter * 10u + 1u;
            host.broadcast(makeTestEvent(evId, /*ownerId=*/0), TEST_CH_RELIABLE);
            bool all3 = true;
            for (int c = 0; c < 3; ++c) {
                if (!waitForEventId(*clientInboxes[c], clientEvAcc[c], evId, 1000)) all3 = false;
            }
            if (!all3) broadcastOk = false;
        }

        // broadcastExcept(): reaches the 2 non-excluded clients, never the
        // excluded one. Rotates which client is excluded across iterations.
        {
            u32 evId       = 20000u + (u32)iter * 10u + 2u;
            u32 excludedId = clientIdsArr[iter % 3];
            host.broadcastExcept(excludedId, makeTestEvent(evId, /*ownerId=*/0),
                                  TEST_CH_RELIABLE);
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                if (clientIdsArr[c] == excludedId) {
                    Sleep(400); // give a would-be leak time to arrive before we check
                    drainEventsInto(*clientInboxes[c], clientEvAcc[c]);
                    if (hasEventId(clientEvAcc[c], evId)) ok = false; // must NEVER arrive
                } else if (!waitForEventId(*clientInboxes[c], clientEvAcc[c], evId, 1000)) {
                    ok = false;
                }
            }
            if (!ok) exceptOk = false;
        }

        // sendTo(): reaches exactly the one targeted client, no one else.
        // Rotates which client is targeted across iterations.
        {
            u32 evId     = 20000u + (u32)iter * 10u + 3u;
            u32 targetId = clientIdsArr[(iter + 1) % 3];
            host.sendTo(targetId, makeTestEvent(evId, /*ownerId=*/0), TEST_CH_RELIABLE);
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                if (clientIdsArr[c] == targetId) {
                    if (!waitForEventId(*clientInboxes[c], clientEvAcc[c], evId, 1000)) ok = false;
                } else {
                    Sleep(400);
                    drainEventsInto(*clientInboxes[c], clientEvAcc[c]);
                    if (hasEventId(clientEvAcc[c], evId)) ok = false; // must NEVER arrive
                }
            }
            if (!ok) sendToOk = false;
        }
    }
    CHECK("broadcast() reaches all 3 connected clients (3/3, 5 iterations)", broadcastOk);
    CHECK("broadcastExcept() reaches exactly the 2 non-excluded clients, excluded gets 0 "
          "(2/3, 5 iterations rotating exclusion)", exceptOk);
    CHECK("sendTo() reaches exactly the 1 targeted client, no one else "
          "(1/3, 5 iterations rotating target)", sendToOk);

    // Sender-identity resolution: a client sends an EventPacket whose payload
    // ownerId is deliberately forged to a DIFFERENT value than its own
    // host-assigned PlayerId. The host must resolve the true sender from
    // ev.peer->data (logged as "recv player=N type=T"), not from the
    // self-reported payload field.
    {
        const u32 FORGED_OWNER = 4242u; // outside {1,2,3} - can never collide with a real id
        u32 realId = clientIdsArr[0];
        EventPacket forged;
        std::memset(&forged, 0, sizeof(forged));
        forged.type    = (u8)PKT_EVENT;
        forged.event   = (u8)EVT_KNOCKOUT;
        forged.ownerId = FORGED_OWNER; // deliberately WRONG
        forged.eventId = 99999u;
        clientLinks[0]->queueEvent(forged);

        char needleReal[64];
        _snprintf(needleReal, sizeof(needleReal) - 1, "recv player=%u type=%u",
                  (unsigned)realId, (unsigned)PKT_EVENT);
        needleReal[sizeof(needleReal) - 1] = '\0';
        char needleForged[64];
        _snprintf(needleForged, sizeof(needleForged) - 1, "recv player=%u type=%u",
                  (unsigned)FORGED_OWNER, (unsigned)PKT_EVENT);
        needleForged[sizeof(needleForged) - 1] = '\0';

        bool sawReal   = waitForLogContains(needleReal, 2000);
        bool sawForged = logContains(needleForged);
        CHECK("host resolved sender == client's real assigned PlayerId, "
              "not the forged payload ownerId", sawReal && !sawForged);

        // WR-01: also drain the HOST's own Inbound and assert the forged
        // ownerId's event never landed there - the exact channel CR-01's bug
        // corrupted (a forged ownerId reaching inbound_->pushEvent() before
        // rejectIfForgedOwner() runs). The log checks above only prove
        // rejection happened and relay leakage didn't - neither drains
        // hostInbound, so a regression that moved rejectIfForgedOwner() to
        // run AFTER the local-apply push would slip past them undetected.
        Sleep(400);
        std::deque<InboundEvent> hostEvAcc;
        drainEventsInto(hostInbound, hostEvAcc);
        bool hostClean = !hasEventFromOwner(hostEvAcc, FORGED_OWNER);
        CHECK("forged ownerId event never reached the HOST's own Inbound",
              hostClean);
    }

    // ---- Phase 3 Plan 01 (TRACER): Class A relay end-to-end - one join-authored
    // squad-state (PKT_ENTITY_BATCH) path. Reuses the still-connected host + 3-
    // client topology above. setOwnedEntities() re-publishes every ~50ms tick as
    // long as haveOut_ stays true (see waitForEntityFromOwner's doc comment), so
    // the up-to-2s waits below drive dozens of relay ticks each - this doubles as
    // the "sustained relay, no crash / no double-free" proof the plan calls for,
    // not just a single-shot check (ROUTE-02/03/04, STRIDE T-03-01/T-03-02).
    std::printf("\n-- Class A relay: entity-batch relay-count, no-echo, forge-reject --\n");

    {
        // Author's batch must reach BOTH other connected clients exactly once,
        // ownerId intact, and never echo back to the author itself.
        EntityState relayProbe;
        std::memset(&relayProbe, 0, sizeof(relayProbe));

        const int authorIdx = 0;
        u32       authorId  = clientIdsArr[authorIdx];
        int       otherIdx[2];
        { int s = 0; for (int c = 0; c < 3; ++c) { if (c != authorIdx) otherIdx[s++] = c; } }

        clientLinks[authorIdx]->setOwnedEntities(authorId, &relayProbe, 1);

        std::deque<InboundEntity> otherAcc0, otherAcc1;
        bool bothOthersGotIt =
            waitForEntityFromOwner(*clientInboxes[otherIdx[0]], otherAcc0, authorId, 2000) &&
            waitForEntityFromOwner(*clientInboxes[otherIdx[1]], otherAcc1, authorId, 2000);

        // No-echo: give a would-be echo time to arrive, then confirm the author's
        // OWN inbox never received its own batch back (broadcastExcept excludes
        // the author by construction).
        Sleep(400);
        std::deque<InboundEntity> authorAcc;
        drainEntitiesInto(*clientInboxes[authorIdx], authorAcc);
        bool noEcho = !hasEntityFromOwner(authorAcc, authorId);

        CHECK("relay: author's batch reached both other clients, author got no echo",
              bothOthersGotIt && noEcho);

        // Stop the continuous re-send (keep ownerId honest) so it doesn't linger
        // into the disconnect/reconnect battery below.
        clientLinks[authorIdx]->setOwnedEntities(authorId, 0, 0);
    }

    {
        // A client forging a foreign ownerId on its entity batch must be rejected
        // and logged, never relayed to the other clients (cross-domain publish
        // blocked - hdr.ownerId vs the host-assigned sourcePlayerId).
        const u32 FORGED_OWNER = 4242u; // outside {1,2,3}
        const int forgerIdx    = 1;
        u32       forgerRealId = clientIdsArr[forgerIdx];
        int       otherIdx[2];
        { int s = 0; for (int c = 0; c < 3; ++c) { if (c != forgerIdx) otherIdx[s++] = c; } }

        EntityState forgedEntity;
        std::memset(&forgedEntity, 0, sizeof(forgedEntity));
        clientLinks[forgerIdx]->setOwnedEntities(FORGED_OWNER, &forgedEntity, 1);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundEntity> o0, o1;
        drainEntitiesInto(*clientInboxes[otherIdx[0]], o0);
        drainEntitiesInto(*clientInboxes[otherIdx[1]], o1);
        bool noLeak = !hasEntityFromOwner(o0, FORGED_OWNER) &&
                      !hasEntityFromOwner(o1, FORGED_OWNER);

        CHECK("relay REJECT: forged ownerId not relayed, host logged rejection",
              sawReject && noLeak);

        // WR-01: neither check above drains/inspects hostInbound - the exact
        // channel CR-01's bug corrupted (a forged ownerId reaching the host's
        // own Inbound -> Replicator::ingest() -> Driven.owner). Assert the
        // forged owner's entity never appears in the host's own inbound
        // queue either, so a regression that moved rejectIfForgedOwner()
        // after the local-apply push (deliverEntity) would be caught here.
        std::deque<InboundEntity> hostAcc;
        drainEntitiesInto(hostInbound, hostAcc);
        bool hostClean = !hasEntityFromOwner(hostAcc, FORGED_OWNER);
        CHECK("relay REJECT: forged ownerId never reached the HOST's own Inbound",
              sawReject && noLeak && hostClean);

        // Stop the continuous re-send (revert to the real ownerId) so the forged
        // id doesn't linger into the disconnect/reconnect battery below.
        clientLinks[forgerIdx]->setOwnedEntities(forgerRealId, 0, 0);
    }

    // ---- Phase 8 Plan 01 Task 1 (WORLD-01 tracer): PKT_DOOR multi-author relay
    // at N=3. PKT_DOOR stays Class A (symmetric, any client may author a door) -
    // this leg proves the ROUTING half of the fix: two DIFFERENT clients
    // authoring the SAME door hand with INDEPENDENT seqs must both relay to
    // every other connected client (neither dropped at the relay boundary).
    // The APPLY-side half - that a receiver's per-sender seq accept never lets
    // the first author's higher counter drop the second author's newer row -
    // is prototest's testSeqPerSender (ChangeGate.h's gateSeqAcceptPerSender,
    // engine-free, no ENet needed to prove it).
    std::printf("\n-- Phase 8: PKT_DOOR multi-author relay at N=3 (WORLD-01) --\n");
    {
        const int author1Idx = 1; // client2
        const int author2Idx = 2; // client3
        const int thirdIdx   = 0; // client1 - neither author, must observe BOTH rows
        u32 author1Id = clientIdsArr[author1Idx];
        u32 author2Id = clientIdsArr[author2Idx];

        // One door hand both "authors" address - independent per-sender seqs,
        // deliberately with author2's seq LOWER than author1's: the exact
        // shape a single shared seqSeen counter would have dropped (author2's
        // first-ever row compared against author1's already-higher counter).
        u32 sharedHand[5] = { 90u, 1u, 2u, 3u, 4u };

        DoorPacket d1; std::memset(&d1, 0, sizeof(d1));
        d1.type = (u8)PKT_DOOR; d1.ownerId = author1Id; d1.seq = 5u;
        for (int h = 0; h < 5; ++h) d1.hand[h] = sharedHand[h];
        d1.open = 1; d1.locked = 0;
        clientLinks[author1Idx]->queueDoor(d1);

        DoorPacket d2; std::memset(&d2, 0, sizeof(d2));
        d2.type = (u8)PKT_DOOR; d2.ownerId = author2Id; d2.seq = 1u;
        for (int h = 0; h < 5; ++h) d2.hand[h] = sharedHand[h];
        d2.open = 0; d2.locked = 1;
        clientLinks[author2Idx]->queueDoor(d2);

        std::deque<InboundDoor> thirdAcc;
        bool gotBoth =
            waitForDoorFromOwnerHand(*clientInboxes[thirdIdx], thirdAcc, author1Id, sharedHand, 2000) &&
            waitForDoorFromOwnerHand(*clientInboxes[thirdIdx], thirdAcc, author2Id, sharedHand, 2000);

        CHECK("PKT_DOOR: two authors of the SAME door hand both relay to a third "
              "client (N>=3 routing, WORLD-01)", gotBoth);

        // Forged leg: a DoorPacket whose ownerId != the sender's real id is
        // rejected at the host, never relayed to anyone (same cross-domain-
        // publish guard as every other Class A channel's forge-reject leg).
        const u32 FORGED_OWNER = 7777u; // outside {1,2,3}
        int otherIdx2[2];
        { int s = 0; for (int c = 0; c < 3; ++c) { if (c != author1Idx) otherIdx2[s++] = c; } }

        DoorPacket forged; std::memset(&forged, 0, sizeof(forged));
        forged.type = (u8)PKT_DOOR; forged.ownerId = FORGED_OWNER; forged.seq = 99u;
        for (int h = 0; h < 5; ++h) forged.hand[h] = sharedHand[h];
        forged.open = 1; forged.locked = 0;
        clientLinks[author1Idx]->queueDoor(forged);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)author1Id);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundDoor> o0, o1;
        drainDoorsInto(*clientInboxes[otherIdx2[0]], o0);
        drainDoorsInto(*clientInboxes[otherIdx2[1]], o1);
        bool noLeak = !hasDoorFromOwnerHand(o0, FORGED_OWNER, sharedHand) &&
                      !hasDoorFromOwnerHand(o1, FORGED_OWNER, sharedHand);

        CHECK("PKT_DOOR forged-owner REJECT: not relayed to any client",
              sawReject && noLeak);
    }

    // ---- Phase 8 Plan 01 Task 2 (WORLD-01 tracer, build-door leg): PKT_BUILD_DOOR
    // multi-author relay at N=3 - the same door-hand collision fix (per-sender
    // seq), proven on the placer-key-translated identity build-doors use.
    // Build-doors STAY Class A (transient toggles, not locked global facts).
    std::printf("\n-- Phase 8: PKT_BUILD_DOOR multi-author relay at N=3 (WORLD-01) --\n");
    {
        const int author1Idx = 1; // client2
        const int author2Idx = 2; // client3
        const int thirdIdx   = 0; // client1 - neither author

        u32 author1Id = clientIdsArr[author1Idx];
        u32 author2Id = clientIdsArr[author2Idx];
        u32 sharedKey[5] = { 95u, 1u, 2u, 3u, 4u };
        const u8 DOOR_IDX = 0u;

        BuildDoorPacket b1; std::memset(&b1, 0, sizeof(b1));
        b1.type = (u8)PKT_BUILD_DOOR; b1.ownerId = author1Id; b1.seq = 5u;
        for (int h = 0; h < 5; ++h) b1.bkey[h] = sharedKey[h];
        b1.doorIndex = DOOR_IDX; b1.open = 1; b1.locked = 0;
        clientLinks[author1Idx]->queueBuildDoor(b1);

        BuildDoorPacket b2; std::memset(&b2, 0, sizeof(b2));
        b2.type = (u8)PKT_BUILD_DOOR; b2.ownerId = author2Id; b2.seq = 1u;
        for (int h = 0; h < 5; ++h) b2.bkey[h] = sharedKey[h];
        b2.doorIndex = DOOR_IDX; b2.open = 0; b2.locked = 1;
        clientLinks[author2Idx]->queueBuildDoor(b2);

        std::deque<InboundBuildDoor> thirdAcc;
        bool gotBoth =
            waitForBuildDoorFromOwnerKey(*clientInboxes[thirdIdx], thirdAcc, author1Id, sharedKey, 2000) &&
            waitForBuildDoorFromOwnerKey(*clientInboxes[thirdIdx], thirdAcc, author2Id, sharedKey, 2000);

        CHECK("PKT_BUILD_DOOR: two authors of the SAME build-door key both relay to "
              "a third client (N>=3 routing, WORLD-01)", gotBoth);
    }

    // ---- Phase 8 Plan 01 Task 2 (WORLD-02): PKT_FACTION + PKT_DEED reclassified
    // to host-terminated intent (out of Class A) - mirrors the PKT_INV_XFER
    // host-terminated tracer shape (Phase 7): the intent reaches the HOST ONLY,
    // never relayed to any other client, and a forged ownerId is rejected.
    std::printf("\n-- Phase 8: PKT_FACTION + PKT_DEED host-terminated intent "
                "(WORLD-02) --\n");
    {
        const int authorIdx = 0; // client1
        u32       authorId  = clientIdsArr[authorIdx];

        FactionPacket fa; std::memset(&fa, 0, sizeof(fa));
        fa.type = (u8)PKT_FACTION; fa.ownerId = authorId; fa.seq = 1u;
        std::strncpy(fa.sid, "phase8_test_faction", sizeof(fa.sid) - 1);
        fa.relation = 42.0f;
        clientLinks[authorIdx]->queueFaction(fa);

        std::deque<InboundFaction> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainFactionsInto(hostInbound, hostAcc);
                hostGotIt = hasFactionFromOwner(hostAcc, authorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }

        // Host-terminated: no client (not even the author) ever receives the
        // raw intent back - it is never relay-dispatched.
        Sleep(400);
        std::deque<InboundFaction> c0, c1, c2;
        drainFactionsInto(*clientInboxes[0], c0);
        drainFactionsInto(*clientInboxes[1], c1);
        drainFactionsInto(*clientInboxes[2], c2);
        bool noRelay = !hasFactionFromOwner(c0, authorId) &&
                       !hasFactionFromOwner(c1, authorId) &&
                       !hasFactionFromOwner(c2, authorId);

        CHECK("PKT_FACTION: host received the intent, no client was relayed a copy "
              "(host-terminated, WORLD-02)", hostGotIt && noRelay);
    }

    {
        const u32 FORGED_OWNER = 8888u; // outside {1,2,3}
        const int forgerIdx    = 1;     // client2
        u32       forgerRealId = clientIdsArr[forgerIdx];

        FactionPacket ffa; std::memset(&ffa, 0, sizeof(ffa));
        ffa.type = (u8)PKT_FACTION; ffa.ownerId = FORGED_OWNER; ffa.seq = 2u;
        std::strncpy(ffa.sid, "phase8_forged_faction", sizeof(ffa.sid) - 1);
        ffa.relation = -10.0f;
        clientLinks[forgerIdx]->queueFaction(ffa);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundFaction> hostAcc2;
        drainFactionsInto(hostInbound, hostAcc2);
        bool neverReachedHost = !hasFactionFromOwner(hostAcc2, FORGED_OWNER);

        CHECK("PKT_FACTION forged-owner REJECT: rejected at the host, zero downstream effect",
              sawReject && neverReachedHost);
    }

    {
        const int authorIdx = 2; // client3
        u32       authorId  = clientIdsArr[authorIdx];

        DeedPacket dp; std::memset(&dp, 0, sizeof(dp));
        dp.type = (u8)PKT_DEED; dp.ownerId = authorId; dp.seq = 1u;
        u32 deedHand[5] = { 96u, 1u, 2u, 3u, 4u };
        for (int h = 0; h < 5; ++h) dp.hand[h] = deedHand[h];
        dp.owned = 1;
        std::strncpy(dp.ownerSid, "phase8_test_owner", sizeof(dp.ownerSid) - 1);
        clientLinks[authorIdx]->queueDeed(dp);

        std::deque<InboundDeed> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainDeedsInto(hostInbound, hostAcc);
                hostGotIt = hasDeedFromOwner(hostAcc, authorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }

        Sleep(400);
        std::deque<InboundDeed> c0, c1, c2;
        drainDeedsInto(*clientInboxes[0], c0);
        drainDeedsInto(*clientInboxes[1], c1);
        drainDeedsInto(*clientInboxes[2], c2);
        bool noRelay = !hasDeedFromOwner(c0, authorId) &&
                       !hasDeedFromOwner(c1, authorId) &&
                       !hasDeedFromOwner(c2, authorId);

        CHECK("PKT_DEED: host received the intent, no client was relayed a copy "
              "(host-terminated, WORLD-02)", hostGotIt && noRelay);
    }

    {
        const u32 FORGED_OWNER = 9999u; // outside {1,2,3}
        const int forgerIdx    = 0;     // client1
        u32       forgerRealId = clientIdsArr[forgerIdx];

        DeedPacket fdp; std::memset(&fdp, 0, sizeof(fdp));
        fdp.type = (u8)PKT_DEED; fdp.ownerId = FORGED_OWNER; fdp.seq = 2u;
        u32 deedHand[5] = { 97u, 1u, 2u, 3u, 4u };
        for (int h = 0; h < 5; ++h) fdp.hand[h] = deedHand[h];
        fdp.owned = 1;
        std::strncpy(fdp.ownerSid, "phase8_forged_owner", sizeof(fdp.ownerSid) - 1);
        clientLinks[forgerIdx]->queueDeed(fdp);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundDeed> hostAcc2;
        drainDeedsInto(hostInbound, hostAcc2);
        bool neverReachedHost = !hasDeedFromOwner(hostAcc2, FORGED_OWNER);

        CHECK("PKT_DEED forged-owner REJECT: rejected at the host, zero downstream effect",
              sawReject && neverReachedHost);
    }

    // ---- Phase 8 review CR-01: two joins' deed intents for the SAME hand with
    // colliding seqs - the host intake must commit BOTH. Join A authors the
    // hand at a HIGH seq (50) first; join B later sends a genuinely newer
    // purchase intent at its own LOW seq (7). The transport half proves both
    // intents reach the host; the gate half folds the real arrival order
    // through the per-sender accept the host intake now runs
    // (gateSeqAcceptPerSender - the applyFactions/applyDeeds CR-01 fix) and
    // through the bare-scalar gate it replaced, proving the fix commits both
    // where the scalar dropped B's - the paid-purchase-lost bug.
    std::printf("\n-- Phase 8 review CR-01: PKT_DEED cross-sender seq collision "
                "at the host intake --\n");
    {
        const int authorAIdx = 0; // client1
        const int authorBIdx = 1; // client2
        u32 authorAId = clientIdsArr[authorAIdx];
        u32 authorBId = clientIdsArr[authorBIdx];
        u32 deedHand[5] = { 98u, 1u, 2u, 3u, 4u };

        DeedPacket da; std::memset(&da, 0, sizeof(da));
        da.type = (u8)PKT_DEED; da.ownerId = authorAId; da.seq = 50u;
        for (int h = 0; h < 5; ++h) da.hand[h] = deedHand[h];
        da.owned = 1;
        std::strncpy(da.ownerSid, "phase8_cr01_owner", sizeof(da.ownerSid) - 1);
        clientLinks[authorAIdx]->queueDeed(da);

        // Wait for A's intent BEFORE sending B's, so the host-side arrival
        // order (A's seq=50, then B's seq=7) is deterministic - the exact
        // order the scalar gate mis-handled.
        std::deque<InboundDeed> hostAcc;
        bool gotA = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainDeedsInto(hostInbound, hostAcc);
                gotA = hasDeedFromOwner(hostAcc, authorAId);
                if (gotA) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }

        DeedPacket db; std::memset(&db, 0, sizeof(db));
        db.type = (u8)PKT_DEED; db.ownerId = authorBId; db.seq = 7u; // < A's 50
        for (int h = 0; h < 5; ++h) db.hand[h] = deedHand[h];
        db.owned = 1;
        std::strncpy(db.ownerSid, "phase8_cr01_owner", sizeof(db.ownerSid) - 1);
        clientLinks[authorBIdx]->queueDeed(db);

        bool gotB = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainDeedsInto(hostInbound, hostAcc);
                gotB = hasDeedFromOwner(hostAcc, authorBId);
                if (gotB) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }

        CHECK("CR-01 transport: both joins' deed intents for the same hand "
              "reach the host", gotA && gotB);

        // Gate half: fold the received rows for THIS hand in arrival order.
        std::map<unsigned int, unsigned int> perSender;
        unsigned int scalarSeen = 0;
        int perSenderCommits = 0, scalarCommits = 0;
        for (std::deque<InboundDeed>::const_iterator it = hostAcc.begin();
             it != hostAcc.end(); ++it) {
            const DeedPacket& p = it->pkt;
            bool sameHand = true;
            for (int h = 0; h < 5; ++h)
                if (p.hand[h] != deedHand[h]) sameHand = false;
            if (!sameHand) continue;
            if (sync::gateSeqAcceptPerSender(perSender, p.ownerId, p.seq)) {
                perSender[p.ownerId] = p.seq;
                ++perSenderCommits;
            }
            if (sync::gateSeqAccept(scalarSeen, p.seq)) {
                scalarSeen = p.seq;
                ++scalarCommits;
            }
        }
        CHECK("CR-01 gate: per-sender accept commits BOTH joins' deed intents",
              perSenderCommits == 2);
        CHECK("CR-01 gate: the replaced bare-scalar gate would have dropped "
              "the second join's lower-seq intent (the bug this leg locks out)",
              scalarCommits == 1);
    }

    // ---- Phase 8 Plan 01 Task 3 (WORLD-02): PKT_RESEARCH join->host intent
    // leg. PKT_RESEARCH was already RELAY_NONE pre-Phase-8 (no re-point), but
    // a join publishing one is new this phase - proves the intent reaches the
    // HOST ONLY (never relayed), exactly the PKT_FACTION/PKT_DEED shape, plus
    // a forged-owner reject.
    std::printf("\n-- Phase 8: PKT_RESEARCH join->host intent (WORLD-02) --\n");
    {
        const int authorIdx = 1; // client2
        u32       authorId  = clientIdsArr[authorIdx];

        ResearchPacket rp; std::memset(&rp, 0, sizeof(rp));
        rp.type = (u8)PKT_RESEARCH; rp.ownerId = authorId; rp.seq = 1u;
        std::strncpy(rp.sid, "phase8_test_research", sizeof(rp.sid) - 1);
        clientLinks[authorIdx]->queueResearch(rp);

        std::deque<InboundResearch> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainResearchInto(hostInbound, hostAcc);
                hostGotIt = hasResearchFromOwner(hostAcc, authorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }

        Sleep(400);
        std::deque<InboundResearch> c0, c1, c2;
        drainResearchInto(*clientInboxes[0], c0);
        drainResearchInto(*clientInboxes[1], c1);
        drainResearchInto(*clientInboxes[2], c2);
        bool noRelay = !hasResearchFromOwner(c0, authorId) &&
                       !hasResearchFromOwner(c1, authorId) &&
                       !hasResearchFromOwner(c2, authorId);

        CHECK("PKT_RESEARCH: host received the join's intent, no client was relayed a "
              "copy (host-terminated, WORLD-02)", hostGotIt && noRelay);
    }

    {
        const u32 FORGED_OWNER = 6543u; // outside {1,2,3}
        const int forgerIdx    = 2;     // client3
        u32       forgerRealId = clientIdsArr[forgerIdx];

        ResearchPacket frp; std::memset(&frp, 0, sizeof(frp));
        frp.type = (u8)PKT_RESEARCH; frp.ownerId = FORGED_OWNER; frp.seq = 2u;
        std::strncpy(frp.sid, "phase8_forged_research", sizeof(frp.sid) - 1);
        clientLinks[forgerIdx]->queueResearch(frp);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundResearch> hostAcc2;
        drainResearchInto(hostInbound, hostAcc2);
        bool neverReachedHost = !hasResearchFromOwner(hostAcc2, FORGED_OWNER);

        CHECK("PKT_RESEARCH forged-owner REJECT: rejected at the host, zero downstream effect",
              sawReject && neverReachedHost);
    }

    // ---- Phase 8 Plan 02 Task 1 (WORLD-03): PKT_CELL_CLAIM host-terminated
    // + PKT_CELL_MAP host-broadcast/adoption + forged/join-authored-map
    // reject. Proves the ROUTING half (the exhaustive tie-break matrix is
    // prototest-authoritative, testCellMap) - the claim intent reaches the
    // host ONLY (never relayed to any other client), the host's single
    // authoritative map reaches every connected client exactly once, and
    // both a forged claim and a join-authored map are rejected.
    std::printf("\n-- Phase 8 Plan 02: PKT_CELL_CLAIM host-terminated + "
                "PKT_CELL_MAP broadcast/adoption (WORLD-03) --\n");
    {
        // 3 joins (client1/client2/client3) each send a claim carrying
        // their real ownerId.
        for (int c = 0; c < 3; ++c) {
            CellClaimPacket p; std::memset(&p, 0, sizeof(p));
            p.type = (u8)PKT_CELL_CLAIM; p.ownerId = clientIdsArr[c];
            p.tabRank = 0u; p.seq = 1u; p.cellX = 10 + c; p.cellY = -5;
            clientLinks[c]->queueCellClaim(p);
        }

        std::deque<InboundCellClaim> hostAcc;
        bool hostGotAll =
            waitForCellClaimFromOwner(hostInbound, hostAcc, clientIdsArr[0], 2000) &&
            waitForCellClaimFromOwner(hostInbound, hostAcc, clientIdsArr[1], 2000) &&
            waitForCellClaimFromOwner(hostInbound, hostAcc, clientIdsArr[2], 2000);
        CHECK("PKT_CELL_CLAIM: host received all 3 claim intents (WORLD-03)", hostGotAll);

        // Host-terminated: no OTHER client is relayed a copy of another
        // client's claim (a client's own claim, echoed back by the host's
        // own unrelated broadcast-your-own-claim send path, is excluded
        // from this check - only cross-client relay is asserted here).
        Sleep(400);
        std::deque<InboundCellClaim> c0, c1, c2;
        drainCellClaimsInto(*clientInboxes[0], c0);
        drainCellClaimsInto(*clientInboxes[1], c1);
        drainCellClaimsInto(*clientInboxes[2], c2);
        bool noCrossRelay =
            !hasCellClaimFromOwner(c0, clientIdsArr[1]) && !hasCellClaimFromOwner(c0, clientIdsArr[2]) &&
            !hasCellClaimFromOwner(c1, clientIdsArr[0]) && !hasCellClaimFromOwner(c1, clientIdsArr[2]) &&
            !hasCellClaimFromOwner(c2, clientIdsArr[0]) && !hasCellClaimFromOwner(c2, clientIdsArr[1]);
        CHECK("PKT_CELL_CLAIM: host-terminated - no client relayed a copy of ANOTHER "
              "client's claim (WORLD-03)", noCrossRelay);

        // Forged claim: ownerId != the sending peer's real id must be
        // rejected at the host, never reaching the host's own intake.
        const u32 FORGED_OWNER = 7777u;
        u32       forgerRealId = clientIdsArr[0];
        CellClaimPacket fcc; std::memset(&fcc, 0, sizeof(fcc));
        fcc.type = (u8)PKT_CELL_CLAIM; fcc.ownerId = FORGED_OWNER;
        fcc.tabRank = 0u; fcc.seq = 2u; fcc.cellX = 99; fcc.cellY = 99;
        clientLinks[0]->queueCellClaim(fcc);
        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u", (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);
        Sleep(400);
        std::deque<InboundCellClaim> hostAcc2;
        drainCellClaimsInto(hostInbound, hostAcc2);
        bool neverReachedHost = !hasCellClaimFromOwner(hostAcc2, FORGED_OWNER);
        CHECK("PKT_CELL_CLAIM forged-owner REJECT: rejected at the host, zero downstream "
              "effect (WORLD-03)", sawReject && neverReachedHost);

        // The host now computes + authors exactly ONE authoritative map
        // (real applyClaimIntents-equivalent: computeAndBroadcastCellMap).
        CellMapPacket cmp; std::memset(&cmp, 0, sizeof(cmp));
        cmp.type = (u8)PKT_CELL_MAP; cmp.seq = 1u; cmp.count = 3u;
        cmp.entries[0].cellX = 10; cmp.entries[0].cellY = -5; cmp.entries[0].ownerId = clientIdsArr[0];
        cmp.entries[1].cellX = 11; cmp.entries[1].cellY = -5; cmp.entries[1].ownerId = clientIdsArr[1];
        cmp.entries[2].cellX = 12; cmp.entries[2].cellY = -5; cmp.entries[2].ownerId = clientIdsArr[2];
        host.queueCellMap(cmp);

        std::deque<InboundCellMap> m0, m1, m2;
        bool all3Got = waitForCellMap(*clientInboxes[0], m0, 2000) &&
                       waitForCellMap(*clientInboxes[1], m1, 2000) &&
                       waitForCellMap(*clientInboxes[2], m2, 2000);
        Sleep(200);
        drainCellMapsInto(*clientInboxes[0], m0);
        drainCellMapsInto(*clientInboxes[1], m1);
        drainCellMapsInto(*clientInboxes[2], m2);
        CHECK("PKT_CELL_MAP: the host's authoritative map reached all 3 connected "
              "clients (WORLD-03)", all3Got);
        CHECK("PKT_CELL_MAP: exactly once per client (host-broadcast, no dup)",
              m0.size() == 1 && m1.size() == 1 && m2.size() == 1);
        bool mapMatches = !m0.empty() && m0[0].pkt.count == 3u &&
                          m0[0].pkt.entries[0].ownerId == clientIdsArr[0];
        CHECK("PKT_CELL_MAP: the adopted map names the host's resolved entries (WORLD-03)",
              mapMatches);

        // A JOIN-authored PKT_CELL_MAP must be rejected outright - a client
        // never has authority to name the cell-claim verdict.
        CellMapPacket badMap; std::memset(&badMap, 0, sizeof(badMap));
        badMap.type = (u8)PKT_CELL_MAP; badMap.seq = 99u; badMap.count = 0u;
        clientLinks[1]->queueCellMap(badMap);
        bool sawMapReject = waitForLogContains(
            "PKT_CELL_MAP from a client rejected", 2000);
        CHECK("PKT_CELL_MAP: a join-authored map is rejected at the host "
              "(host-authoritative only, WORLD-03)", sawMapReject);
    }

    // ---- Phase 6 Plan 01 Task 1 (GAP-1): PKT_TREATMENT Class A relay -------
    // routingClassOf(PKT_TREATMENT) flipped from RELAY_FAILSAFE_LOG to
    // RELAY_BROADCAST_EXCEPT above; this proves the live wire consequence:
    // a join healing ANOTHER join's driven body must reach that body's
    // authority through the host, never just terminate there. Mirrors the
    // entity-batch relay-count/no-echo/forge-reject legs just above, one-shot
    // (queueTreatment fires once per call, unlike setOwnedEntities' continuous
    // republish) - the same shape the earlier forged-EventPacket check used.
    std::printf("\n-- Class A relay: PKT_TREATMENT relay-count, no-echo, forge-reject "
                "(Phase 6 GAP-1) --\n");
    {
        const int authorIdx = 1; // client2
        u32       authorId  = clientIdsArr[authorIdx];
        int       otherIdx[2];
        { int s = 0; for (int c = 0; c < 3; ++c) { if (c != authorIdx) otherIdx[s++] = c; } }

        TreatmentPacket tp;
        std::memset(&tp, 0, sizeof(tp));
        tp.type    = (u8)PKT_TREATMENT;
        tp.ownerId = authorId; // real, own id - the healer authoring the delta
        tp.treatId = 777u;
        tp.sType = 1; tp.sContainer = 0; tp.sContainerSerial = 0;
        tp.sIndex = 5; tp.sSerial = 9;
        for (unsigned int i = 0; i < 12; ++i) tp.partBand[i] = -1.0f;
        tp.partBand[0] = 40.0f;
        clientLinks[authorIdx]->queueTreatment(tp);

        std::deque<InboundTreatment> otherAcc0, otherAcc1;
        bool bothOthersGotIt =
            waitForTreatmentFromOwner(*clientInboxes[otherIdx[0]], otherAcc0, authorId, 2000) &&
            waitForTreatmentFromOwner(*clientInboxes[otherIdx[1]], otherAcc1, authorId, 2000);

        // No-echo: the author must never receive its own relayed treatment back.
        Sleep(400);
        std::deque<InboundTreatment> authorAcc;
        drainTreatmentsInto(*clientInboxes[authorIdx], authorAcc);
        bool noEcho = !hasTreatmentFromOwner(authorAcc, authorId);

        CHECK("treatment relay: author's delta reached both other clients, author got no echo",
              bothOthersGotIt && noEcho);
    }

    {
        // Forged ownerId on a treatment delta must be rejected at the host and
        // never relayed - same cross-domain-publish guard as the entity-batch
        // and money/event forge-reject legs above.
        const u32 FORGED_OWNER = 4343u; // outside {1,2,3}
        const int forgerIdx    = 1;     // client2
        u32       forgerRealId = clientIdsArr[forgerIdx];
        int       otherIdx[2];
        { int s = 0; for (int c = 0; c < 3; ++c) { if (c != forgerIdx) otherIdx[s++] = c; } }

        TreatmentPacket ftp;
        std::memset(&ftp, 0, sizeof(ftp));
        ftp.type    = (u8)PKT_TREATMENT;
        ftp.ownerId = FORGED_OWNER; // deliberately WRONG - not client2's real id
        ftp.treatId = 778u;
        ftp.sType = 1; ftp.sContainer = 0; ftp.sContainerSerial = 0;
        ftp.sIndex = 5; ftp.sSerial = 9;
        for (unsigned int i = 0; i < 12; ++i) ftp.partBand[i] = -1.0f;
        ftp.partBand[1] = 50.0f;
        clientLinks[forgerIdx]->queueTreatment(ftp);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundTreatment> o0, o1;
        drainTreatmentsInto(*clientInboxes[otherIdx[0]], o0);
        drainTreatmentsInto(*clientInboxes[otherIdx[1]], o1);
        bool noLeak = !hasTreatmentFromOwner(o0, FORGED_OWNER) &&
                      !hasTreatmentFromOwner(o1, FORGED_OWNER);

        std::deque<InboundTreatment> hostAcc;
        drainTreatmentsInto(hostInbound, hostAcc);
        bool hostClean = !hasTreatmentFromOwner(hostAcc, FORGED_OWNER);

        CHECK("treatment relay REJECT: forged ownerId not relayed, never applied at "
              "the host, host logged rejection", sawReject && noLeak && hostClean);
    }

    // ---- Phase 5 Plan 01 (ID-01/T-05-02): money-delta collision matrix -----
    // Transport-level proof only: NetLink/ENet must deliver three senders'
    // identical sender-local seq=1 as three DISTINCT (ownerId, seq) tuples at
    // the host's Inbound money-delta drain, none coalesced or misattributed.
    // The receiver-side FOLD correctness (poolFoldSeen_ keyed on ownerId) is
    // proven separately by prototest's foldMonotonic checks (05-01 Task 1) and
    // by the Plan 03 live gate - nettest links NetLink.cpp only, no Replicator/
    // engine dependency (RESEARCH Pitfall 3), so it cannot exercise the fold
    // itself. Reuses the still-connected host + 3-client topology above.
    std::printf("\n-- money-delta collision matrix: 3 senders, same local seq=1 --\n");
    {
        std::deque<InboundMoneyDelta> got;
        for (int c = 0; c < 3; ++c) {
            MoneyDeltaPacket md;
            std::memset(&md, 0, sizeof(md));
            md.type          = (u8)PKT_MONEY_DELTA;
            md.ownerId       = clientIdsArr[c]; // each sender's own real assigned id
            md.seq           = 1;               // SAME sender-local seq in the same window
            md.delta         = 10 + c;          // distinct payload, incidental to the tuple proof
            md.authorSpendMs = 1000u + (u32)c;  // protocol 60: distinct author stamps
            clientLinks[c]->queueMoneyDelta(md);
        }

        // Poll until all three distinct tuples are observed (or timeout) -
        // mirrors waitForEntityFromOwner's accumulate-and-poll shape.
        DWORD deadline = GetTickCount() + 2000;
        bool haveAll = false;
        do {
            drainMoneyDeltasInto(hostInbound, got);
            haveAll = hasMoneyDeltaTuple(got, clientIdsArr[0], 1) &&
                      hasMoneyDeltaTuple(got, clientIdsArr[1], 1) &&
                      hasMoneyDeltaTuple(got, clientIdsArr[2], 1);
            if (haveAll) break;
            Sleep(20);
        } while (GetTickCount() < deadline);
        drainMoneyDeltasInto(hostInbound, got); // final catch-all drain

        // Exactly three entries, and they are three DISTINCT (ownerId,seq)
        // pairs - not a coalesced/deduplicated single delivery.
        std::set<std::pair<u32, u32> > distinctTuples;
        for (size_t i = 0; i < got.size(); ++i)
            distinctTuples.insert(std::make_pair(got[i].pkt.ownerId, got[i].pkt.seq));

        CHECK("money-delta: all 3 senders' seq=1 arrived at the host",
              haveAll);
        CHECK("money-delta: exactly 3 entries at the host inbound (none dropped)",
              got.size() == 3);
        CHECK("money-delta: 3 distinct (ownerId,seq) tuples (none coalesced)",
              distinctTuples.size() == 3);
        CHECK("money-delta: (owner0,1) present and distinct",
              hasMoneyDeltaTuple(got, clientIdsArr[0], 1));
        CHECK("money-delta: (owner1,1) present and distinct",
              hasMoneyDeltaTuple(got, clientIdsArr[1], 1));
        CHECK("money-delta: (owner2,1) present and distinct",
              hasMoneyDeltaTuple(got, clientIdsArr[2], 1));

        // Protocol 60 (CONS-01): the struct grew by authorSpendMs - routing
        // must carry it through unchanged (the field's VALUE is opaque to
        // NetLink/ENet; only the host-side fold logic, proven by prototest,
        // interprets it).
        bool stampsIntact = true;
        for (int c = 0; c < 3; ++c) {
            bool found = false;
            for (size_t i = 0; i < got.size(); ++i) {
                if (got[i].pkt.ownerId == clientIdsArr[c] && got[i].pkt.seq == 1 &&
                    got[i].pkt.authorSpendMs == 1000u + (u32)c) { found = true; break; }
            }
            if (!found) stampsIntact = false;
        }
        CHECK("money-delta: authorSpendMs survives routing intact for all 3 senders "
              "(protocol 60, CONS-01)", stampsIntact);
    }

    // ---- Phase 9 Plan 01 (CONS-01): PKT_MONEY_REJECT broadcast reachability --
    // The host's insufficient-funds verdict (host.queueMoneyReject) must reach
    // EVERY connected client exactly once - the deterministic-verdict
    // observability requirement (the ClaimVerdict/CellMap broadcast precedent).
    std::printf("\n-- PKT_MONEY_REJECT: host-broadcast verdict reaches all clients "
                "(CONS-01) --\n");
    {
        MoneyRejectPacket mr; std::memset(&mr, 0, sizeof(mr));
        mr.type = (u8)PKT_MONEY_REJECT;
        mr.buyerId = clientIdsArr[0];
        mr.seq = 42u;
        mr.delta = -500;
        mr.poolAtVerdict = 30;
        host.queueMoneyReject(mr);

        std::deque<InboundMoneyReject> r0, r1, r2;
        bool all3Got = waitForMoneyRejectFor(*clientInboxes[0], r0, clientIdsArr[0], 42u, 2000) &&
                       waitForMoneyRejectFor(*clientInboxes[1], r1, clientIdsArr[0], 42u, 2000) &&
                       waitForMoneyRejectFor(*clientInboxes[2], r2, clientIdsArr[0], 42u, 2000);
        Sleep(200);
        drainMoneyRejectsInto(*clientInboxes[0], r0);
        drainMoneyRejectsInto(*clientInboxes[1], r1);
        drainMoneyRejectsInto(*clientInboxes[2], r2);
        CHECK("PKT_MONEY_REJECT: the host's verdict reached all 3 connected clients "
              "(CONS-01)", all3Got);
        CHECK("PKT_MONEY_REJECT: exactly once per client (host-broadcast, no dup)",
              r0.size() == 1 && r1.size() == 1 && r2.size() == 1);
        bool payloadMatches = !r0.empty() && r0[0].pkt.buyerId == clientIdsArr[0] &&
                              r0[0].pkt.seq == 42u && r0[0].pkt.delta == -500 &&
                              r0[0].pkt.poolAtVerdict == 30;
        CHECK("PKT_MONEY_REJECT: the adopted verdict names the host's exact payload "
              "(CONS-01)", payloadMatches);

        // A JOIN-authored PKT_MONEY_REJECT must be rejected outright - a
        // client never has authority to author the insufficient-funds
        // verdict (host-authoritative only).
        MoneyRejectPacket badReject; std::memset(&badReject, 0, sizeof(badReject));
        badReject.type = (u8)PKT_MONEY_REJECT; badReject.buyerId = clientIdsArr[1]; badReject.seq = 1u;
        clientLinks[1]->queueMoneyReject(badReject);
        bool sawRejectReject = waitForLogContains(
            "PKT_MONEY_REJECT from a client rejected", 2000);
        CHECK("PKT_MONEY_REJECT: a join-authored verdict is rejected at the host "
              "(host-authoritative only, CONS-01)", sawRejectReject);
    }

    // ---- Phase 9 Plan 01 Task 2 (CONS-01): PKT_MONEY_DELTA forged-owner ------
    std::printf("\n-- PKT_MONEY_DELTA forged-owner REJECT (CONS-01) --\n");
    {
        const u32 FORGED_OWNER = 3210u; // outside {1,2,3}
        const int forgerIdx    = 2;     // client3
        u32       forgerRealId = clientIdsArr[forgerIdx];

        MoneyDeltaPacket fmd; std::memset(&fmd, 0, sizeof(fmd));
        fmd.type = (u8)PKT_MONEY_DELTA; fmd.ownerId = FORGED_OWNER; fmd.seq = 55u;
        fmd.delta = -999; fmd.authorSpendMs = 1u;
        clientLinks[forgerIdx]->queueMoneyDelta(fmd);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundMoneyDelta> hostAcc;
        drainMoneyDeltasInto(hostInbound, hostAcc);
        bool neverReachedHost = !hasMoneyDeltaTuple(hostAcc, FORGED_OWNER, 55u);

        CHECK("PKT_MONEY_DELTA forged-owner REJECT: rejected at the host, zero "
              "downstream effect (CONS-01)", sawReject && neverReachedHost);
    }

    // ---- Phase 9 Plan 01 Task 2 (CONS-01): ack-vector personalization --------
    // The MoneyPacket ack vector IS the host's per-owner processed map made
    // visible - after an interleaved broadcast naming several owners' ack
    // entries, each client's applyMoneyPool must key ONLY on the Entry whose
    // ownerId == its own localId (MoneyFold.h::moneyShouldPop's doc comment,
    // the at-N bounce/double-count this plan exists to kill). This leg proves
    // the WIRE/transport half: the broadcast reaches every client with every
    // owner's entry intact and personally distinguishable; the join's own
    // pop-on-own-ack-only DECISION is prototest's job (testMoneyFold).
    std::printf("\n-- MoneyPacket ack-vector personalization (CONS-01) --\n");
    {
        MoneyPacket mp; std::memset(&mp, 0, sizeof(mp));
        mp.type = (u8)PKT_MONEY; mp.ownerId = 0u; // host
        mp.ackCount = 3;
        mp.acks[0].ownerId = clientIdsArr[0]; mp.acks[0].ackSeq = 7u;
        mp.acks[1].ownerId = clientIdsArr[1]; mp.acks[1].ackSeq = 3u;
        mp.acks[2].ownerId = clientIdsArr[2]; mp.acks[2].ackSeq = 11u;
        mp.money = 4242;
        host.queueMoney(mp);

        std::deque<InboundMoney> m0, m1, m2;
        bool all3Got = waitForMoneyTotal(*clientInboxes[0], m0, 2000) &&
                       waitForMoneyTotal(*clientInboxes[1], m1, 2000) &&
                       waitForMoneyTotal(*clientInboxes[2], m2, 2000);
        Sleep(200);
        drainMoneyInto(*clientInboxes[0], m0);
        drainMoneyInto(*clientInboxes[1], m1);
        drainMoneyInto(*clientInboxes[2], m2);
        CHECK("MoneyPacket ack vector: broadcast reached all 3 connected clients "
              "(CONS-01)", all3Got);

        // Each client can find its OWN entry with the exact value the host
        // wrote for it - and the OTHER two owners' entries are also intact
        // (so a caller that mistakenly read a foreign entry would notice a
        // DIFFERENT value, not a coincidentally-matching one).
        bool ownEntriesIntact = true;
        for (int c = 0; c < 3; ++c) {
            std::deque<InboundMoney>& acc = (c == 0) ? m0 : (c == 1) ? m1 : m2;
            if (acc.empty()) { ownEntriesIntact = false; continue; }
            const MoneyPacket& got = acc.back().pkt;
            if (got.ackCount != 3) { ownEntriesIntact = false; continue; }
            for (int i = 0; i < 3; ++i) {
                bool ok = false;
                for (unsigned int j = 0; j < got.ackCount; ++j) {
                    if (got.acks[j].ownerId == clientIdsArr[i]) {
                        u32 expect = (i == 0) ? 7u : (i == 1) ? 3u : 11u;
                        ok = (got.acks[j].ackSeq == expect);
                        break;
                    }
                }
                if (!ok) ownEntriesIntact = false;
            }
        }
        CHECK("MoneyPacket ack vector: every owner's Entry survives the broadcast "
              "with its exact per-owner value, personally distinguishable by each "
              "client (CONS-01)", ownEntriesIntact);
    }

    // ---- Phase 5 Plan 02 (ID-02/T-05-04): event collision matrix -----------
    // Transport-level proof, mirroring the money-delta collision matrix idiom
    // just above (05-01 Task 2): three connected clients each send an
    // EventPacket carrying their OWN real ownerId and the SAME sender-local
    // eventId=1 in the same window over the reliable channel (PKT_EVENT is
    // Class A / RELAY_BROADCAST_EXCEPT - see the routingClassOf() oracle at
    // the top of main()). NetLink/ENet must never coalesce or misattribute
    // two senders' identical local eventId; the receiver-side (ownerId,
    // eventId) FOLD correctness (appliedEvents_/foldOnce) is proven
    // separately by prototest (05-02 Task 1) and the Plan 03 live gate -
    // nettest links NetLink.cpp only, no Replicator/engine dependency
    // (RESEARCH Pitfall 3), so it cannot exercise applyEvents itself.
    // Reuses the still-connected host + 3-client topology above.
    std::printf("\n-- event collision matrix: 3 senders, same local eventId=1 --\n");
    {
        std::deque<InboundEvent> got;
        for (int c = 0; c < 3; ++c) {
            EventPacket ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.type    = (u8)PKT_EVENT;
            ev.event   = (u8)EVT_KNOCKOUT; // arbitrary - only ownerId/eventId asserted
            ev.ownerId = clientIdsArr[c];  // each sender's own real assigned id
            ev.eventId = 1;                // SAME sender-local eventId in the same window
            clientLinks[c]->queueEvent(ev);
        }

        // Poll until all three distinct (ownerId,eventId) tuples are observed
        // (or timeout) - mirrors the money-delta matrix's accumulate-and-poll
        // shape.
        DWORD deadline = GetTickCount() + 2000;
        bool haveAll = false;
        do {
            drainEventsInto(hostInbound, got);
            haveAll = hasEventFromOwner(got, clientIdsArr[0]) &&
                      hasEventFromOwner(got, clientIdsArr[1]) &&
                      hasEventFromOwner(got, clientIdsArr[2]);
            if (haveAll) break;
            Sleep(20);
        } while (GetTickCount() < deadline);
        drainEventsInto(hostInbound, got); // final catch-all drain

        // Exactly three entries, and they are three DISTINCT (ownerId,eventId)
        // tuples - not a coalesced/deduplicated single delivery, and none
        // misattributed to the wrong sender.
        std::set<std::pair<u32, u32> > distinctEventTuples;
        for (size_t i = 0; i < got.size(); ++i)
            distinctEventTuples.insert(std::make_pair(got[i].ownerId, got[i].ev.eventId));

        CHECK("event collision: all 3 senders' eventId=1 arrived at the host",
              haveAll);
        CHECK("event collision: exactly 3 entries at the host inbound (none dropped)",
              got.size() == 3);
        CHECK("event collision: 3 distinct (ownerId,eventId) tuples (none coalesced)",
              distinctEventTuples.size() == 3);
        CHECK("event collision: (owner0,1) present and distinct",
              distinctEventTuples.count(std::make_pair(clientIdsArr[0], (u32)1)) != 0);
        CHECK("event collision: (owner1,1) present and distinct",
              distinctEventTuples.count(std::make_pair(clientIdsArr[1], (u32)1)) != 0);
        CHECK("event collision: (owner2,1) present and distinct",
              distinctEventTuples.count(std::make_pair(clientIdsArr[2], (u32)1)) != 0);
    }

    // ---- Phase 7 Plan 01 Task 1: PKT_INV_XFER host-terminated intent +
    // PKT_XFER_COMMIT host-broadcast tracer, one leg end-to-end -------------
    // The load-bearing gap this plan closes: PKT_INV_XFER used to be
    // RELAY_FAILSAFE_LOG (log + drop, no forge check) - a join<->join
    // transfer intent silently died at the host with a false-positive
    // ACCEPT to the author. Now it is a host-terminated CLIENT-REQUEST
    // (routingClassOf == RELAY_NONE, proven above): this leg proves the live
    // wire consequence - the intent reaches the HOST ONLY (never relayed to
    // any other client, including the intent's own dstOwnerId), and the
    // host's single PKT_XFER_COMMIT verdict then reaches EVERY connected
    // client exactly once. Reuses the still-connected host + 3-client
    // topology above.
    std::printf("\n-- Phase 7: PKT_INV_XFER host-terminated + PKT_XFER_COMMIT "
                "broadcast tracer --\n");
    {
        const int authorIdx = 0; // client1
        u32       authorId  = clientIdsArr[authorIdx];
        u32       dstOwner  = clientIdsArr[1]; // client2 - the intent's declared destination
        const u32 XFER_ID   = 501u;

        InvXferPacket xp;
        std::memset(&xp, 0, sizeof(xp));
        xp.type      = (u8)PKT_INV_XFER;
        xp.ownerId   = authorId; // real, own id
        xp.xferId    = XFER_ID;
        xp.srcOwnerId = authorId;
        xp.dstOwnerId = dstOwner;
        xp.itemType  = 7;
        xp.quantity  = 3;
        std::strncpy(xp.stringID, "test_item", sizeof(xp.stringID) - 1);
        clientLinks[authorIdx]->queueInvXfer(xp);

        std::deque<InboundInvXfer> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainInvXfersInto(hostInbound, hostAcc);
                hostGotIt = hasInvXferFromOwner(hostAcc, authorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }

        // Host-terminated: NO client (not even the declared dstOwner) ever
        // receives the raw intent - it is never relay-dispatched.
        Sleep(400);
        std::deque<InboundInvXfer> c0Acc, c1Acc, c2Acc;
        drainInvXfersInto(*clientInboxes[0], c0Acc);
        drainInvXfersInto(*clientInboxes[1], c1Acc);
        drainInvXfersInto(*clientInboxes[2], c2Acc);
        bool noRelay = !hasInvXferFromOwner(c0Acc, authorId) &&
                       !hasInvXferFromOwner(c1Acc, authorId) &&
                       !hasInvXferFromOwner(c2Acc, authorId);

        CHECK("PKT_INV_XFER: host received the intent, no client was relayed a copy "
              "(host-terminated, not FAILSAFE)", hostGotIt && noRelay);

        // The host now arbitrates (Task 2 will do this via XferCommit.h +
        // processXferIntents) and authors the single commit. This tracer
        // proves the WIRE consequence only: the host's broadcast reaches
        // every connected client exactly once, authorId preserved.
        XferCommitPacket xc;
        std::memset(&xc, 0, sizeof(xc));
        xc.type       = (u8)PKT_XFER_COMMIT;
        xc.authorId   = authorId;
        xc.transferId = XFER_ID;
        xc.srcOwnerId = authorId;
        xc.dstOwnerId = dstOwner;
        xc.itemType   = 7;
        xc.quantity   = 3;
        xc.outcome    = (u8)XFER_COMMIT_RELOCATE;
        xc.applied    = 3;
        std::strncpy(xc.stringID, "test_item", sizeof(xc.stringID) - 1);
        host.queueXferCommit(xc);

        std::deque<InboundXferCommit> got0, got1, got2;
        bool all3 =
            waitForXferCommitFor(*clientInboxes[0], got0, authorId, XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[1], got1, authorId, XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[2], got2, authorId, XFER_ID, 2000);

        Sleep(200);
        drainXferCommitsInto(*clientInboxes[0], got0);
        drainXferCommitsInto(*clientInboxes[1], got1);
        drainXferCommitsInto(*clientInboxes[2], got2);
        int count0 = 0, count1 = 0, count2 = 0;
        for (size_t i = 0; i < got0.size(); ++i)
            if (got0[i].pkt.authorId == authorId && got0[i].pkt.transferId == XFER_ID) ++count0;
        for (size_t i = 0; i < got1.size(); ++i)
            if (got1[i].pkt.authorId == authorId && got1[i].pkt.transferId == XFER_ID) ++count1;
        for (size_t i = 0; i < got2.size(); ++i)
            if (got2[i].pkt.authorId == authorId && got2[i].pkt.transferId == XFER_ID) ++count2;

        CHECK("PKT_XFER_COMMIT: reached all 3 connected clients", all3);
        CHECK("PKT_XFER_COMMIT: exactly once per client (host-broadcast, no dup)",
              count0 == 1 && count1 == 1 && count2 == 1);
    }

    {
        // Forged ownerId on the transfer intent must be rejected at the host
        // and never reach any client - PKT_INV_XFER's receive branch never
        // had this check before Phase 7 Plan 01 (it was the one owner-tagged
        // reliable packet that lacked it).
        const u32 FORGED_OWNER = 4444u; // outside {1,2,3}
        const int forgerIdx    = 0;     // client1
        u32       forgerRealId = clientIdsArr[forgerIdx];

        InvXferPacket fxp;
        std::memset(&fxp, 0, sizeof(fxp));
        fxp.type       = (u8)PKT_INV_XFER;
        fxp.ownerId    = FORGED_OWNER; // deliberately WRONG
        fxp.xferId     = 502u;
        fxp.srcOwnerId = FORGED_OWNER;
        fxp.dstOwnerId = clientIdsArr[1];
        clientLinks[forgerIdx]->queueInvXfer(fxp);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundInvXfer> hostAcc2, c0Acc2, c1Acc2, c2Acc2;
        drainInvXfersInto(hostInbound, hostAcc2);
        drainInvXfersInto(*clientInboxes[0], c0Acc2);
        drainInvXfersInto(*clientInboxes[1], c1Acc2);
        drainInvXfersInto(*clientInboxes[2], c2Acc2);
        bool zeroEffect = !hasInvXferFromOwner(hostAcc2, FORGED_OWNER) &&
                          !hasInvXferFromOwner(c0Acc2, FORGED_OWNER) &&
                          !hasInvXferFromOwner(c1Acc2, FORGED_OWNER) &&
                          !hasInvXferFromOwner(c2Acc2, FORGED_OWNER);

        CHECK("PKT_INV_XFER forged-owner REJECT: rejected at the host, zero downstream effect",
              sawReject && zeroEffect);
    }

    // ---- Phase 7 Plan 01 Task 2: transfer expansion legs - symmetry,
    // host-as-participant, simultaneous non-coalescing commits -------------
    // nettest links NetLink.cpp only (no Replicator/engine dependency -
    // RESEARCH.md Pitfall 3), so it cannot run the host's own arbitration
    // (processXferIntents lives in ReplicatorItems.cpp). These legs prove
    // the ROUTING half of each scenario: the intent reaches the host only,
    // and the host's own single-commit-per-intent broadcast (authored here
    // exactly as processXferIntents would, via host.queueXferCommit) reaches
    // every connected client exactly once, never coalesced across distinct
    // (authorId,transferId) keys. The host ARBITRATION (moved/fab/latch
    // bookkeeping) is proven by prototest's testXferCommit and the plugin's
    // own [xfer] COMMIT log line, not reproducible here.
    std::printf("\n-- Phase 7: transfer expansion - P3->P4 symmetry, host-as-"
                "participant, simultaneous non-coalescing commits --\n");
    {
        // P3->P4 symmetry: a DIFFERENT sender (client2, index 1) than the
        // Task 1 tracer (client1, index 0) - proves no host-adjacency
        // shortcut privileges one particular client slot.
        const int authorIdx = 1; // client2
        u32       authorId  = clientIdsArr[authorIdx];
        u32       dstOwner  = clientIdsArr[2]; // client3
        const u32 XFER_ID   = 601u;

        InvXferPacket xp; std::memset(&xp, 0, sizeof(xp));
        xp.type = (u8)PKT_INV_XFER; xp.ownerId = authorId; xp.xferId = XFER_ID;
        xp.srcOwnerId = authorId; xp.dstOwnerId = dstOwner;
        xp.itemType = 3; xp.quantity = 1;
        std::strncpy(xp.stringID, "symmetry_item", sizeof(xp.stringID) - 1);
        clientLinks[authorIdx]->queueInvXfer(xp);

        std::deque<InboundInvXfer> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainInvXfersInto(hostInbound, hostAcc);
                hostGotIt = hasInvXferFromOwner(hostAcc, authorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("P3->P4 symmetry: host received the intent (any sender slot, not just client1)",
              hostGotIt);

        XferCommitPacket xc; std::memset(&xc, 0, sizeof(xc));
        xc.type = (u8)PKT_XFER_COMMIT; xc.authorId = authorId; xc.transferId = XFER_ID;
        xc.srcOwnerId = authorId; xc.dstOwnerId = dstOwner;
        xc.itemType = 3; xc.quantity = 1; xc.outcome = (u8)XFER_COMMIT_RELOCATE; xc.applied = 1;
        std::strncpy(xc.stringID, "symmetry_item", sizeof(xc.stringID) - 1);
        host.queueXferCommit(xc);

        std::deque<InboundXferCommit> g0, g1, g2;
        bool all3 =
            waitForXferCommitFor(*clientInboxes[0], g0, authorId, XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[1], g1, authorId, XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[2], g2, authorId, XFER_ID, 2000);
        CHECK("P3->P4 symmetry: commit reached all 3 connected clients", all3);
    }

    {
        // P4->host-container (host-as-participant): the AUTHOR is a client,
        // but the DECLARED DESTINATION owner is the host itself (id 0) - a
        // join dragging into a host-registered container (e.g. storeSync_).
        // The commit still broadcasts with identical routing/log evidence
        // regardless of who the destination owner is.
        const int authorIdx = 2; // client3
        u32       authorId  = clientIdsArr[authorIdx];
        const u32 HOST_OWNER = 0u;
        const u32 XFER_ID    = 602u;

        InvXferPacket xp; std::memset(&xp, 0, sizeof(xp));
        xp.type = (u8)PKT_INV_XFER; xp.ownerId = authorId; xp.xferId = XFER_ID;
        xp.srcOwnerId = authorId; xp.dstOwnerId = HOST_OWNER;
        xp.itemType = 9; xp.quantity = 2;
        std::strncpy(xp.stringID, "host_container_item", sizeof(xp.stringID) - 1);
        clientLinks[authorIdx]->queueInvXfer(xp);

        std::deque<InboundInvXfer> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainInvXfersInto(hostInbound, hostAcc);
                hostGotIt = hasInvXferFromOwner(hostAcc, authorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("P4->host-container: host received the intent (dstOwnerId=host)",
              hostGotIt);

        XferCommitPacket xc; std::memset(&xc, 0, sizeof(xc));
        xc.type = (u8)PKT_XFER_COMMIT; xc.authorId = authorId; xc.transferId = XFER_ID;
        xc.srcOwnerId = authorId; xc.dstOwnerId = HOST_OWNER;
        xc.itemType = 9; xc.quantity = 2; xc.outcome = (u8)XFER_COMMIT_RELOCATE; xc.applied = 2;
        std::strncpy(xc.stringID, "host_container_item", sizeof(xc.stringID) - 1);
        host.queueXferCommit(xc);

        std::deque<InboundXferCommit> g0, g1, g2;
        bool all3 =
            waitForXferCommitFor(*clientInboxes[0], g0, authorId, XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[1], g1, authorId, XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[2], g2, authorId, XFER_ID, 2000);
        CHECK("P4->host-container (host-as-participant): commit still broadcasts "
              "with identical routing", all3);
    }

    {
        // Simultaneous transfers: client1 (P2) and client2 (P3) each author
        // transferId=1 (their OWN per-sender-monotonic counter) in the same
        // window for DIFFERENT items - both intents reach the host, and both
        // commits broadcast with distinct (authorId,transferId) keys, never
        // coalesced (the Phase 5 composite-key rule applied to transfers).
        u32 authorA = clientIdsArr[0]; // client1
        u32 authorB = clientIdsArr[1]; // client2
        const u32 SIM_XFER_ID = 1u;    // SAME per-sender id, DIFFERENT senders

        InvXferPacket xpA; std::memset(&xpA, 0, sizeof(xpA));
        xpA.type = (u8)PKT_INV_XFER; xpA.ownerId = authorA; xpA.xferId = SIM_XFER_ID;
        xpA.srcOwnerId = authorA; xpA.dstOwnerId = clientIdsArr[2];
        xpA.itemType = 11; xpA.quantity = 1;
        std::strncpy(xpA.stringID, "sim_item_a", sizeof(xpA.stringID) - 1);
        clientLinks[0]->queueInvXfer(xpA);

        InvXferPacket xpB; std::memset(&xpB, 0, sizeof(xpB));
        xpB.type = (u8)PKT_INV_XFER; xpB.ownerId = authorB; xpB.xferId = SIM_XFER_ID;
        xpB.srcOwnerId = authorB; xpB.dstOwnerId = clientIdsArr[2];
        xpB.itemType = 12; xpB.quantity = 1;
        std::strncpy(xpB.stringID, "sim_item_b", sizeof(xpB.stringID) - 1);
        clientLinks[1]->queueInvXfer(xpB);

        std::deque<InboundInvXfer> hostAcc;
        bool gotBoth = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainInvXfersInto(hostInbound, hostAcc);
                gotBoth = hasInvXferFromOwner(hostAcc, authorA) &&
                          hasInvXferFromOwner(hostAcc, authorB);
                if (gotBoth) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("simultaneous transfers: both intents (same sender-local xferId=1, "
              "distinct authors) reached the host", gotBoth);

        XferCommitPacket xcA; std::memset(&xcA, 0, sizeof(xcA));
        xcA.type = (u8)PKT_XFER_COMMIT; xcA.authorId = authorA; xcA.transferId = SIM_XFER_ID;
        xcA.itemType = 11; xcA.quantity = 1; xcA.outcome = (u8)XFER_COMMIT_RELOCATE; xcA.applied = 1;
        std::strncpy(xcA.stringID, "sim_item_a", sizeof(xcA.stringID) - 1);
        host.queueXferCommit(xcA);

        XferCommitPacket xcB; std::memset(&xcB, 0, sizeof(xcB));
        xcB.type = (u8)PKT_XFER_COMMIT; xcB.authorId = authorB; xcB.transferId = SIM_XFER_ID;
        xcB.itemType = 12; xcB.quantity = 1; xcB.outcome = (u8)XFER_COMMIT_RELOCATE; xcB.applied = 1;
        std::strncpy(xcB.stringID, "sim_item_b", sizeof(xcB.stringID) - 1);
        host.queueXferCommit(xcB);

        std::deque<InboundXferCommit> g0, g1, g2;
        bool gotA =
            waitForXferCommitFor(*clientInboxes[0], g0, authorA, SIM_XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[1], g1, authorA, SIM_XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[2], g2, authorA, SIM_XFER_ID, 2000);
        bool gotB =
            waitForXferCommitFor(*clientInboxes[0], g0, authorB, SIM_XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[1], g1, authorB, SIM_XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[2], g2, authorB, SIM_XFER_ID, 2000);
        CHECK("simultaneous transfers: both commits reached all 3 clients, "
              "distinct (authorId,transferId) keys, neither coalesced", gotA && gotB);

        Sleep(200);
        drainXferCommitsInto(*clientInboxes[0], g0);
        int countA0 = 0, countB0 = 0;
        for (size_t i = 0; i < g0.size(); ++i) {
            if (g0[i].pkt.authorId == authorA && g0[i].pkt.transferId == SIM_XFER_ID) ++countA0;
            if (g0[i].pkt.authorId == authorB && g0[i].pkt.transferId == SIM_XFER_ID) ++countB0;
        }
        CHECK("simultaneous transfers: client1 saw exactly one of each commit (no coalescing)",
              countA0 == 1 && countB0 == 1);
    }

    // ---- Plan 03: lifecycle battery - roster, disconnect isolation, reconnect
    // slot reuse -------------------------------------------------------------
    // Reuses the still-connected host + 3-client topology above. Every ENet
    // teardown detection in this section is REAL: NetLink::stop() has no
    // wire-level graceful disconnect (enet_host_destroy locally resets peers
    // with no notification packet - verified against the vendored
    // third_party/enet/enet/host.c this plan builds against), so the host only
    // learns of a stopped client via ENet's own peer-timeout mechanism. That
    // mechanism is deterministic (bounded by ENET_PEER_TIMEOUT_MINIMUM=5000ms
    // plus a handful of exponential-backoff retries), just not instant - the
    // waits below use generous timeouts to accommodate it.
    std::printf("\n-- roster + disconnect isolation + reconnect slot reuse --\n");

    // -- roster: every client learns the full set of OTHER connected ids --
    std::deque<u32> clientConnAcc[3];
    bool rosterOk = true;
    for (int c = 0; c < 3; ++c) {
        for (int o = 0; o < 3; ++o) {
            if (o == c) continue;
            if (!waitForConnectId(*clientInboxes[c], clientConnAcc[c], clientIdsArr[o], 3000)) {
                rosterOk = false;
            }
        }
    }
    CHECK("roster: every client learned the OTHER two connected PlayerIds "
          "(PKT_PLAYER_JOINED)", rosterOk);

    // WR-01 regression: the newcomer join announcement is now sent via
    // broadcastExcept(id, ...), not broadcast(...) - each client must NEVER
    // observe a self-referential PKT_PLAYER_JOINED for its own PlayerId (it
    // already learned its own id via WELCOME at connect time). Any self-join
    // event would already have been swept into clientConnAcc[c] as a side
    // effect of the drains the roster loop above just performed.
    bool noSelfJoin = true;
    for (int c = 0; c < 3; ++c) {
        if (hasU32(clientConnAcc[c], clientIdsArr[c])) noSelfJoin = false;
    }
    CHECK("WR-01: no client received a self-referential PKT_PLAYER_JOINED for its own id",
          noSelfJoin);

    // -- establish per-player epoch state for the about-to-depart peer, so the
    // reconnect check below can distinguish "the reused slot's epochSeen_ entry
    // was reset" from "it was left stale" (Pitfall 2 / finding 8's remediation
    // target, and the epochSeen_.erase(id) this plan added to the HELLO success
    // branch). Bump the departing client's session epoch to 3 and publish one
    // owned entity; the host must accept it (epochSeen_[id]=3) before it leaves.
    int idx2 = -1;
    for (int c = 0; c < 3; ++c) { if (clientIdsArr[c] == 2) idx2 = c; }
    CHECK("found the client holding PlayerId 2 (for the disconnect/reconnect cycle)",
          idx2 >= 0);

    EntityState probeEntity;
    std::memset(&probeEntity, 0, sizeof(probeEntity));
    if (idx2 >= 0) {
        clientLinks[idx2]->bumpSessionEpoch();
        clientLinks[idx2]->bumpSessionEpoch();
        clientLinks[idx2]->bumpSessionEpoch(); // sendEpoch_ == 3
        clientLinks[idx2]->setOwnedEntities(2, &probeEntity, 1);
        std::deque<InboundEntity> preAcc;
        CHECK("departing client's pre-disconnect entity batch (epoch=3) accepted",
              waitForEntityFromOwner(hostInbound, preAcc, 2, 2000));
    }

    // ---- Phase 5 Plan 02 (ID-03/T-05-06): author-scoped-proxy disconnect
    // isolation, transport level. This is the transport-level analogue of
    // the Replicator's owner-scoped clearPeerReplicationState (receiver-side
    // proxy cleanup itself is engine-coupled and is proven by the Plan 03
    // live Milestone A gate - this proves the TRANSPORT delivers per-author
    // isolation around a disconnect). Reuses the id=2 disconnect (idx2) set
    // up just above: idx2 (about to depart, already publishing owner=2 via
    // the epoch probe above) and a SECOND, still-connected author each have
    // an owned entity batch reach a THIRD client's Inbound (a witness that
    // is neither author) BEFORE the disconnect below.
    int otherAuthorIdx = -1, witnessIdx = -1;
    if (idx2 >= 0) {
        for (int c = 0; c < 3; ++c) {
            if (c == idx2) continue;
            if (otherAuthorIdx < 0) otherAuthorIdx = c; else witnessIdx = c;
        }
    }
    CHECK("author-scoped isolation (ID-03): found a second author + a witness "
          "peer distinct from the departing id",
          otherAuthorIdx >= 0 && witnessIdx >= 0);

    EntityState otherAuthorEntity;
    std::memset(&otherAuthorEntity, 0, sizeof(otherAuthorEntity));
    bool bothAuthorsReachedWitness = false;
    if (otherAuthorIdx >= 0 && witnessIdx >= 0) {
        clientLinks[otherAuthorIdx]->setOwnedEntities(clientIdsArr[otherAuthorIdx],
                                                        &otherAuthorEntity, 1);
        std::deque<InboundEntity> witnessPreAcc;
        bothAuthorsReachedWitness =
            waitForEntityFromOwner(*clientInboxes[witnessIdx], witnessPreAcc, 2, 2000) &&
            waitForEntityFromOwner(*clientInboxes[witnessIdx], witnessPreAcc,
                                    clientIdsArr[otherAuthorIdx], 2000);
    }
    CHECK("author-scoped isolation (ID-03): both authors' batches reached the "
          "witness peer before the disconnect", bothAuthorsReachedWitness);

    // -- disconnect isolation: stop the id=2 client; the two survivors keep
    // their ids, observe the roster departure, and keep exchanging traffic.
    if (idx2 >= 0) clientLinks[idx2]->stop();

    int survivorIdx[2];
    { int s = 0; for (int c = 0; c < 3; ++c) { if (c != idx2) survivorIdx[s++] = c; } }

    CHECK("host observed the id=2 disconnect (registry freed, ENet peer-timeout path)",
          waitForHostLeaveId(hostInbound, 2, 12000));

    // ID-03 (a): the host's leave notification must name ONLY the departing
    // author - a survivor's id must never appear in the leave queue just
    // because a DIFFERENT author disconnected.
    CHECK("author-scoped isolation (ID-03): host's leave notification names "
          "only the departing author, never the surviving author's id",
          otherAuthorIdx < 0 || !hasU32(g_hostLeaves, clientIdsArr[otherAuthorIdx]));

    // ID-03 (b): the surviving author's entity delivery to the witness peer
    // is untouched by the OTHER author's disconnect - no cross-author wipe.
    // Checked immediately (before anything else below drains the witness's
    // entity queue) so this observes the moment right after the disconnect,
    // not a stale pre-disconnect artifact (witnessPreAcc already proved
    // delivery before the disconnect, above).
    bool survivorStillPresentPostLeave = false;
    if (otherAuthorIdx >= 0 && witnessIdx >= 0) {
        std::deque<InboundEntity> witnessPostAcc;
        survivorStillPresentPostLeave =
            waitForEntityFromOwner(*clientInboxes[witnessIdx], witnessPostAcc,
                                    clientIdsArr[otherAuthorIdx], 2000);
        // Stop the continuous re-send so it doesn't linger into the CR-02/
        // reconnect battery below.
        clientLinks[otherAuthorIdx]->setOwnedEntities(clientIdsArr[otherAuthorIdx], 0, 0);
    }
    CHECK("author-scoped isolation (ID-03): surviving author's entity still "
          "reaches the witness peer right after the other author's disconnect "
          "(no cross-author wipe)", survivorStillPresentPostLeave);

    bool survivorsSawLeave = true;
    std::deque<u32> survivorLeaveAcc[2];
    for (int s = 0; s < 2; ++s) {
        int c = survivorIdx[s];
        if (!waitForLeaveId(*clientInboxes[c], survivorLeaveAcc[s], 2, 12000)) {
            survivorsSawLeave = false;
        }
    }
    CHECK("disconnect isolation: both surviving clients observed PKT_PLAYER_LEFT(2)",
          survivorsSawLeave);

    bool survivorsUnchanged = true;
    for (int s = 0; s < 2; ++s) {
        int c = survivorIdx[s];
        if (clientLinks[c]->localId() != clientIdsArr[c]) survivorsUnchanged = false;
    }
    CHECK("disconnect isolation: surviving clients kept their own PlayerIds",
          survivorsUnchanged);

    // Continuity: each survivor's traffic (entity batch, accept-gated by
    // acceptEpoch) still lands on the host after the OTHER player's
    // disconnect - proving epochSeen_'s per-player erase (not a blanket
    // clear) left the survivors' own accepted-epoch entries untouched.
    bool survivorsStillAccepted = true;
    std::deque<InboundEntity> survivorEntAcc[2];
    for (int s = 0; s < 2; ++s) {
        int c = survivorIdx[s];
        clientLinks[c]->setOwnedEntities(clientIdsArr[c], &probeEntity, 1);
        if (!waitForEntityFromOwner(hostInbound, survivorEntAcc[s], clientIdsArr[c], 2000)) {
            survivorsStillAccepted = false;
        }
    }
    CHECK("disconnect isolation: surviving clients' entity batches still accepted "
          "(epochSeen_ was not blanket-cleared)", survivorsStillAccepted);

    // -- Phase 3 Plan 04 (PEER-02/03): disconnect-isolation-at-N>=3 RELAY
    // oracle. The check above only proves the HOST's own Inbound still
    // accepts each survivor's traffic after id=2 left; it does not prove the
    // host keeps RELAYING one survivor's state to the OTHER survivor - the
    // actual wire-level guarantee the owner-scoped clearPeerReplicationState
    // + g_connectedPeers rewrite protects. Reuses the still-connected host +
    // 2-survivor topology right after the id=2 disconnect above, mirroring
    // the Class A relay-count/no-echo idiom from Plan 01 (line ~534 above).
    {
        int authorS = survivorIdx[0];
        int otherS  = survivorIdx[1];
        u32 authorId = clientIdsArr[authorS];

        // Baseline flush (discarded): id=2's own pre-disconnect batch above
        // (the "departing client's pre-disconnect entity batch" probe) was
        // relayed to both survivors BEFORE the disconnect and never drained -
        // that relay was correct AT THE TIME (id=2 was still connected) and
        // must not be misread as a post-leave leak. Drain and discard both
        // survivor inboxes now so the leak check below only sees traffic
        // that arrives AFTER this point (strictly post-disconnect).
        std::deque<InboundEntity> staleOther, staleAuthor;
        drainEntitiesInto(*clientInboxes[otherS], staleOther);
        drainEntitiesInto(*clientInboxes[authorS], staleAuthor);

        EntityState survivorBatch;
        std::memset(&survivorBatch, 0, sizeof(survivorBatch));
        clientLinks[authorS]->setOwnedEntities(authorId, &survivorBatch, 1);

        std::deque<InboundEntity> otherAcc;
        bool otherGotIt = waitForEntityFromOwner(*clientInboxes[otherS], otherAcc,
                                                  authorId, 2000);

        // No-echo: give a would-be echo time to arrive, then confirm the
        // author's own inbox never received its own batch back.
        Sleep(400);
        std::deque<InboundEntity> authorAcc;
        drainEntitiesInto(*clientInboxes[authorS], authorAcc);
        bool noEchoToAuthor = !hasEntityFromOwner(authorAcc, authorId);

        CHECK("disconnect isolation (relay): survivor's batch still reached the "
              "other survivor exactly once after a peer left",
              otherGotIt && noEchoToAuthor);

        // Neither survivor's inbox may carry a batch attributed to the
        // DEPARTED owner (id=2) - the departed id must never appear as a
        // relayed author once it has left.
        drainEntitiesInto(*clientInboxes[otherS], otherAcc);
        bool noDepartedLeak =
            !hasEntityFromOwner(otherAcc, 2) && !hasEntityFromOwner(authorAcc, 2);
        CHECK("disconnect isolation (relay): no batch from the departed owner "
              "reached survivors post-leave", noDepartedLeak);

        // Stop the continuous re-send so it doesn't linger into the CR-02/
        // reconnect battery below.
        clientLinks[authorS]->setOwnedEntities(authorId, 0, 0);
    }

    // -- CR-02 regression: a duplicate PKT_HELLO on an already-registered
    // connection must be ignored, not treated as a brand-new connect. This is
    // exercised HERE (not right after the initial 3-client connect) because
    // the orphan-a-slot bug this guards against only bites when a FREE slot
    // exists for the free-slot scan to (wrongly) claim - and right now, after
    // the id=2 client's disconnect above, exactly one slot (id=2) is free.
    // Without the `ev.peer->data != 0` guard in NetLink.cpp's PKT_HELLO
    // receive branch, a survivor's duplicate HELLO would steal that free slot
    // for itself - minting a second, phantom id for the SAME still-connected
    // ENetPeer* and permanently orphaning the slot it already held. The
    // buggy path is independently observable two ways below: the host would
    // resend a WELCOME with the new id (flipping the survivor's own
    // localId()), and the very next check - "reconnect: freed slot id=2
    // reclaimed" - would then fail, because the free slot the bug just stole
    // is no longer available for client5.
    int probeIdx = survivorIdx[0];
    u32 probeOrigId = clientIdsArr[probeIdx];
    size_t hostConnectsBeforeDup = g_hostConnects.size();

    clientLinks[probeIdx]->debugResendHelloForTest();
    Sleep(500); // allow the duplicate HELLO -> (guarded) ignore round-trip to complete
    drainInto(hostInbound);

    CHECK("CR-02: host logged the duplicate HELLO as ignored (guard branch ran)",
          logContains("duplicate HELLO from already-registered"));
    CHECK("CR-02: duplicate HELLO did not change the sender's own PlayerId",
          clientLinks[probeIdx]->localId() == probeOrigId);
    CHECK("CR-02: duplicate HELLO produced no new host connect event "
          "(no second id minted for the same peer)",
          g_hostConnects.size() == hostConnectsBeforeDup);

    // -- reconnect slot reuse: a NEW client takes the freed slot (id=2), and
    // must NOT renumber the survivors.
    Inbound c5Inbound;
    NetLink client5;
    CHECK("reconnecting client startClient", client5.startClient("127.0.0.1", PORT, &c5Inbound));
    CHECK("reconnect: freed slot id=2 reclaimed (lowest-free-slot reuse, no renumber)",
          waitForLocalId(client5, 2, 5000));

    bool survivorsStillUnchangedAfterReconnect = true;
    for (int s = 0; s < 2; ++s) {
        int c = survivorIdx[s];
        if (clientLinks[c]->localId() != clientIdsArr[c]) survivorsStillUnchangedAfterReconnect = false;
    }
    CHECK("reconnect: surviving clients were NOT renumbered", survivorsStillUnchangedAfterReconnect);

    // The reused slot's epochSeen_ entry must have been RESET (erased), not
    // left stale at the departed occupant's epoch=3: the new occupant never
    // calls bumpSessionEpoch() (fresh NetLink, sendEpoch_ == 0), so its first
    // batch carries epoch=0. If epochSeen_[2] were still 3 (the connect-time
    // reset missing), acceptEpoch(2, 0) would reject it forever (0 < 3) and it
    // would never reach the host's Inbound - the exact bug this plan's
    // `epochSeen_.erase(id)` (HELLO success branch) fixes.
    if (client5.localId() == 2) {
        client5.setOwnedEntities(2, &probeEntity, 1);
        std::deque<InboundEntity> reconnectAcc;
        CHECK("reconnect: new occupant's fresh epoch=0 entity batch accepted "
              "(epochSeen_ reset for the reused slot, not left stale)",
              waitForEntityFromOwner(hostInbound, reconnectAcc, 2, 2000));
    } else {
        CHECK("reconnect: new occupant's fresh epoch=0 entity batch accepted "
              "(epochSeen_ reset for the reused slot, not left stale)", false);
    }

    client5.stop();
    // The host's disconnect detection is ENet peer-timeout-driven (no wire-
    // level graceful disconnect - the same documented gap the earlier
    // reconnect leg above waits out with waitForHostLeaveId before it, too).
    // Wait for the host to actually free slot id=2 before the legs below try
    // to reclaim it - starting a new client immediately after stop() would
    // otherwise race a still-occupied-looking slot and get rejected as a 4th.
    // waitForHostLeaveId's presence-only check is NOT enough here (id=2
    // already appears once in g_hostLeaves from idx2's EARLIER departure
    // above) - waitForHostLeaveCount waits for the SECOND, distinct
    // leave(2) event (client5's own departure).
    CHECK("post-reconnect teardown: host observed client5's departure "
          "(slot id=2 freed for the disconnect legs below)",
          waitForHostLeaveCount(hostInbound, 2, 2, 12000));

    // ---- Phase 7 Plan 01 Task 3: transfer disconnect legs (INV-04) --------
    // Reuses the two remaining survivors (survivorIdx) + a fresh transient
    // client taking the now-free slot (id=2, freed by client5.stop() above)
    // for the role that disconnects. nettest cannot run the host's own
    // arbitration (processXferIntents lives in ReplicatorItems.cpp - no
    // Replicator/engine dependency here), so "the host voids a pre-commit
    // intent" is proven at the ROUTING level: the test simply never queues a
    // commit for the pre-commit leg (mirroring what a real host's
    // clearPeerReplicationState -> xferEraseOwner void would produce - no
    // PKT_XFER_COMMIT ever reaches the wire for that transferId) and asserts
    // survivors see nothing. The plugin-side voided/stood bookkeeping itself
    // is proven by prototest's xferEraseOwner cases + the `[leave] xfer
    // voided=/stood=` log line.
    std::printf("\n-- Phase 7: transfer disconnect legs (INV-04) --\n");
    {
        // Leg A: disconnect PRE-commit. The author disconnects before any
        // commit is authored for its intent - survivors must receive NO
        // commit for that transferId (never both, never neither: it never
        // committed, so it never stands).
        Inbound c6Inbound;
        NetLink client6;
        CHECK("disconnect-pre-commit: reconnecting client startClient",
              client6.startClient("127.0.0.1", PORT, &c6Inbound));
        CHECK("disconnect-pre-commit: reclaimed the freed slot (id=2)",
              waitForLocalId(client6, 2, 5000));
        u32 preAuthorId = client6.localId();
        const u32 PRE_XFER_ID = 701u;

        InvXferPacket xp; std::memset(&xp, 0, sizeof(xp));
        xp.type = (u8)PKT_INV_XFER; xp.ownerId = preAuthorId; xp.xferId = PRE_XFER_ID;
        xp.srcOwnerId = preAuthorId; xp.dstOwnerId = clientIdsArr[survivorIdx[0]];
        xp.itemType = 21; xp.quantity = 1;
        std::strncpy(xp.stringID, "predc_item", sizeof(xp.stringID) - 1);
        client6.queueInvXfer(xp);

        std::deque<InboundInvXfer> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainInvXfersInto(hostInbound, hostAcc);
                hostGotIt = hasInvXferFromOwner(hostAcc, preAuthorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("disconnect-pre-commit: host received the intent before the disconnect",
              hostGotIt);

        // Disconnect BEFORE any commit is ever authored (no host.queueXferCommit
        // call for this transferId at all - the void).
        client6.stop();
        // Third distinct leave(2): idx2's original departure, client5's
        // reconnect-then-redepart, and now client6's - waitForHostLeaveId's
        // presence check would trivially pass already, so count instead
        // (this doubles as the "slot is free again" gate for Leg B's
        // client7 reconnect immediately below).
        CHECK("disconnect-pre-commit: host observed the departure",
              waitForHostLeaveCount(hostInbound, preAuthorId, 3, 12000));

        std::deque<InboundXferCommit> s0, s1;
        drainXferCommitsInto(*clientInboxes[survivorIdx[0]], s0);
        drainXferCommitsInto(*clientInboxes[survivorIdx[1]], s1);
        bool noPhantomCommit = !hasXferCommitFor(s0, preAuthorId, PRE_XFER_ID) &&
                               !hasXferCommitFor(s1, preAuthorId, PRE_XFER_ID);
        CHECK("disconnect-pre-commit: survivors received NO phantom commit for the "
              "voided pre-commit transfer", noPhantomCommit);
    }

    {
        // Leg B: disconnect POST-commit. The host broadcasts the commit
        // FIRST; the author disconnects AFTER - the commit already stands at
        // every survivor and nothing rolls it back (no second mutation, no
        // rollback packet exists on this wire at all).
        Inbound c7Inbound;
        NetLink client7;
        CHECK("disconnect-post-commit: reconnecting client startClient",
              client7.startClient("127.0.0.1", PORT, &c7Inbound));
        CHECK("disconnect-post-commit: reclaimed the freed slot (id=2)",
              waitForLocalId(client7, 2, 5000));
        u32 postAuthorId = client7.localId();
        const u32 POST_XFER_ID = 702u;

        InvXferPacket xp; std::memset(&xp, 0, sizeof(xp));
        xp.type = (u8)PKT_INV_XFER; xp.ownerId = postAuthorId; xp.xferId = POST_XFER_ID;
        xp.srcOwnerId = postAuthorId; xp.dstOwnerId = clientIdsArr[survivorIdx[0]];
        xp.itemType = 22; xp.quantity = 1;
        std::strncpy(xp.stringID, "postdc_item", sizeof(xp.stringID) - 1);
        client7.queueInvXfer(xp);

        std::deque<InboundInvXfer> hostAcc;
        bool hostGotIt = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                drainInvXfersInto(hostInbound, hostAcc);
                hostGotIt = hasInvXferFromOwner(hostAcc, postAuthorId);
                if (hostGotIt) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("disconnect-post-commit: host received the intent", hostGotIt);

        XferCommitPacket xc; std::memset(&xc, 0, sizeof(xc));
        xc.type = (u8)PKT_XFER_COMMIT; xc.authorId = postAuthorId; xc.transferId = POST_XFER_ID;
        xc.srcOwnerId = postAuthorId; xc.dstOwnerId = clientIdsArr[survivorIdx[0]];
        xc.itemType = 22; xc.quantity = 1; xc.outcome = (u8)XFER_COMMIT_RELOCATE; xc.applied = 1;
        std::strncpy(xc.stringID, "postdc_item", sizeof(xc.stringID) - 1);
        host.queueXferCommit(xc);

        std::deque<InboundXferCommit> s0Pre, s1Pre;
        bool bothGotCommit =
            waitForXferCommitFor(*clientInboxes[survivorIdx[0]], s0Pre, postAuthorId, POST_XFER_ID, 2000) &&
            waitForXferCommitFor(*clientInboxes[survivorIdx[1]], s1Pre, postAuthorId, POST_XFER_ID, 2000);
        CHECK("disconnect-post-commit: commit reached both survivors BEFORE the disconnect",
              bothGotCommit);

        // Disconnect AFTER the commit already landed.
        client7.stop();
        // Fourth distinct leave(2) this run (idx2, client5, client6, now
        // client7) - same presence-vs-count caveat as Leg A above.
        CHECK("disconnect-post-commit: host observed the departure",
              waitForHostLeaveCount(hostInbound, postAuthorId, 4, 12000));

        Sleep(300);
        std::deque<InboundXferCommit> s0Post, s1Post;
        drainXferCommitsInto(*clientInboxes[survivorIdx[0]], s0Post);
        drainXferCommitsInto(*clientInboxes[survivorIdx[1]], s1Post);
        // No further PKT_XFER_COMMIT for this transferId arrives after the
        // disconnect (no rollback packet exists) - the commit already
        // received above still stands, unchanged, un-rolled-back.
        bool noRollback = !hasXferCommitFor(s0Post, postAuthorId, POST_XFER_ID) &&
                          !hasXferCommitFor(s1Post, postAuthorId, POST_XFER_ID);
        CHECK("disconnect-post-commit: no further/rollback commit after the departure - "
              "the original commit stands unchanged", noRollback);
    }

    {
        // Forged-owner reject leg for PKT_XFER_COMMIT_ACK - the one new
        // Class C packet this plan adds that the earlier forge-reject sweep
        // (money/event/entity/treatment/PKT_INV_XFER) did not yet cover.
        const u32 FORGED_OWNER = 5555u; // outside {0,1,3} (id=2's slot is free again)
        u32 forgerRealId = clientLinks[survivorIdx[0]]->localId();

        XferCommitAckPacket fak; std::memset(&fak, 0, sizeof(fak));
        fak.type = (u8)PKT_XFER_COMMIT_ACK;
        fak.ownerId = FORGED_OWNER; // deliberately WRONG
        fak.authorId = clientIdsArr[survivorIdx[1]];
        fak.transferId = 1u;
        clientLinks[survivorIdx[0]]->queueXferCommitAck(fak);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(300);
        std::deque<InboundXferCommitAck> hostAckAcc;
        drainXferCommitAcksInto(hostInbound, hostAckAcc);
        bool neverReachedHost = !hasXferCommitAckFromOwner(hostAckAcc, FORGED_OWNER);

        CHECK("PKT_XFER_COMMIT_ACK forged-owner REJECT: rejected at the host, "
              "never reached the host's own Inbound", sawReject && neverReachedHost);
    }

    // ---- Phase 7 Plan 02 Task 3: claim-contention legs (INV-03) -----------
    // nettest links NetLink.cpp only (no Replicator/engine dependency -
    // RESEARCH.md Pitfall 3), so it cannot run the host's own arbitration
    // (applyClaimIntents/ClaimArbiter.h live in ReplicatorItems.cpp/
    // ClaimArbiter.h). These legs prove the ROUTING half: the claim intent
    // reaches the host only (never relayed to the item's author or any other
    // client), and the host's own single-verdict-per-contention broadcast
    // (authored here exactly as applyClaimIntents would, via
    // host.queueClaimVerdict) reaches every connected client exactly once.
    // The deterministic tie-stamp and lowest-playerId cases are
    // prototest-authoritative (testClaimArbiter) - a real loopback cannot
    // force sub-ms stamp ties; nettest proves routing + one-verdict-per-
    // contention only.
    std::printf("\n-- Phase 7: claim-contention legs (INV-03) --\n");
    {
        // Leg A: 2 claimants (survivors P2/P3 - the two of client1/2/3 still
        // actually connected; the roster/disconnect-isolation battery above
        // already permanently disconnected the third, idx2's original
        // holder) claim the SAME item identity with STAGGERED authorClaimMs
        // - both intents must reach the host ONLY (never relayed, not even
        // to the item's author), and the host's single verdict (naming the
        // earlier-stamped claimant, the outcome ClaimArbiter.h's
        // finalizeClaim would deterministically produce for a clean stamp
        // gap) must reach both connected survivors exactly once.
        const u32 AUTHOR_ID = 0u; // the item's author's netId space (host-owned, arbitrary)
        const u32 NET_ID    = 9001u;
        u32 p2 = clientIdsArr[survivorIdx[0]];
        u32 p3 = clientIdsArr[survivorIdx[1]];

        clientLinks[survivorIdx[0]]->queueWorldClaim(p2, AUTHOR_ID, &NET_ID, 1, 1000u); // earlier
        clientLinks[survivorIdx[1]]->queueWorldClaim(p3, AUTHOR_ID, &NET_ID, 1, 1500u); // later

        std::deque<InboundWorldClaim> hostAcc;
        bool hostGotP2 = waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, p2, AUTHOR_ID, NET_ID, 2000);
        bool hostGotP3 = waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, p3, AUTHOR_ID, NET_ID, 2000);
        CHECK("claim contention: host received BOTH claim intents", hostGotP2 && hostGotP3);

        // Host-terminated: neither surviving client ever receives the raw
        // intents (the item's author here is host-owned, but the check is
        // symmetric - a relay would show up on ANY receiving client).
        Sleep(400);
        std::deque<InboundWorldClaim> s0Acc, s1Acc;
        drainWorldClaimsInto(*clientInboxes[survivorIdx[0]], s0Acc);
        drainWorldClaimsInto(*clientInboxes[survivorIdx[1]], s1Acc);
        bool noRelay =
            !hasWorldClaimFromOwnerFor(s0Acc, p2, AUTHOR_ID, NET_ID) &&
            !hasWorldClaimFromOwnerFor(s1Acc, p2, AUTHOR_ID, NET_ID) &&
            !hasWorldClaimFromOwnerFor(s0Acc, p3, AUTHOR_ID, NET_ID) &&
            !hasWorldClaimFromOwnerFor(s1Acc, p3, AUTHOR_ID, NET_ID);
        CHECK("claim contention: host-terminated - no client was relayed a copy of either intent",
              noRelay);

        // The host now arbitrates (applyClaimIntents, ClaimArbiter.h) and
        // authors exactly ONE verdict. Reuses the deterministic outcome
        // prototest's testClaimArbiter already exhaustively proves.
        ClaimVerdictPacket cv; std::memset(&cv, 0, sizeof(cv));
        cv.type = (u8)PKT_CLAIM_VERDICT;
        cv.authorId = AUTHOR_ID; cv.netId = NET_ID;
        cv.winnerPlayerId = p2; // the earlier-stamped claimant
        cv.verdict = (u8)CLAIM_AWARD;
        host.queueClaimVerdict(cv);

        std::deque<InboundClaimVerdict> v0, v1;
        bool bothGot =
            waitForClaimVerdictFor(*clientInboxes[survivorIdx[0]], v0, AUTHOR_ID, NET_ID, 2000) &&
            waitForClaimVerdictFor(*clientInboxes[survivorIdx[1]], v1, AUTHOR_ID, NET_ID, 2000);
        Sleep(200);
        drainClaimVerdictsInto(*clientInboxes[survivorIdx[0]], v0);
        drainClaimVerdictsInto(*clientInboxes[survivorIdx[1]], v1);
        CHECK("claim contention: PKT_CLAIM_VERDICT reached both connected survivors", bothGot);
        CHECK("claim contention: exactly once per client (host-broadcast, no dup)",
              countClaimVerdictsFor(v0, AUTHOR_ID, NET_ID) == 1 &&
              countClaimVerdictsFor(v1, AUTHOR_ID, NET_ID) == 1);
        bool winnerNamed = hasClaimVerdictFor(v0, AUTHOR_ID, NET_ID) &&
                            v0[v0.size() - 1].pkt.winnerPlayerId == p2;
        CHECK("claim contention: the verdict names the earlier-stamped claimant as winner",
              winnerNamed);
    }

    {
        // Leg B: 3 claimants (P2, P3, and a fresh P4 reconnecting into the
        // slot Leg A's disconnect battery freed) - still exactly ONE
        // verdict, one winner, reaching every connected client.
        Inbound c8Inbound;
        NetLink client8;
        CHECK("claim contention (3-way): reconnecting client startClient",
              client8.startClient("127.0.0.1", PORT, &c8Inbound));
        CHECK("claim contention (3-way): reclaimed the freed slot (id=2)",
              waitForLocalId(client8, 2, 5000));
        u32 p4 = client8.localId();
        u32 p2 = clientIdsArr[survivorIdx[0]];
        u32 p3 = clientIdsArr[survivorIdx[1]];
        const u32 AUTHOR_ID = 0u;
        const u32 NET_ID    = 9002u;

        clientLinks[survivorIdx[0]]->queueWorldClaim(p2, AUTHOR_ID, &NET_ID, 1, 2000u);
        clientLinks[survivorIdx[1]]->queueWorldClaim(p3, AUTHOR_ID, &NET_ID, 1, 2500u);
        client8.queueWorldClaim(p4, AUTHOR_ID, &NET_ID, 1, 3000u);

        std::deque<InboundWorldClaim> hostAcc;
        bool hostGotAll =
            waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, p2, AUTHOR_ID, NET_ID, 2000) &&
            waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, p3, AUTHOR_ID, NET_ID, 2000) &&
            waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, p4, AUTHOR_ID, NET_ID, 2000);
        CHECK("claim contention (3-way): host received all 3 claim intents", hostGotAll);

        ClaimVerdictPacket cv; std::memset(&cv, 0, sizeof(cv));
        cv.type = (u8)PKT_CLAIM_VERDICT;
        cv.authorId = AUTHOR_ID; cv.netId = NET_ID;
        cv.winnerPlayerId = p2; // the earliest-stamped of the three
        cv.verdict = (u8)CLAIM_AWARD;
        host.queueClaimVerdict(cv);

        // 3 connected clients total (the two survivors + this leg's fresh
        // reconnect) - the host itself does not receive its own broadcast.
        std::deque<InboundClaimVerdict> v0, v1, v3;
        bool all3 =
            waitForClaimVerdictFor(*clientInboxes[survivorIdx[0]], v0, AUTHOR_ID, NET_ID, 2000) &&
            waitForClaimVerdictFor(*clientInboxes[survivorIdx[1]], v1, AUTHOR_ID, NET_ID, 2000) &&
            waitForClaimVerdictFor(c8Inbound,                      v3, AUTHOR_ID, NET_ID, 2000);
        CHECK("claim contention (3-way): still exactly one verdict, reaching all 3 connected "
              "clients", all3);

        client8.stop();
        // Fifth distinct leave(2) this run (idx2, client5, client6, client7,
        // now client8) - Leg D's client9 reconnect below must not race the
        // host's own free-slot bookkeeping, the same wait every earlier
        // reconnect leg in this file already respects.
        CHECK("claim contention (3-way): host observed the departure (slot id=2 freed)",
              waitForHostLeaveCount(hostInbound, 2, 5, 12000));
    }

    {
        // Leg C: forged claimant ownerId on PKT_WORLD_ITEM_CLAIM must be
        // rejected at the host and never reach any client - the exact
        // rejectIfForgedOwner check the receive branch gained in Task 1.
        const u32 FORGED_OWNER = 6666u;
        u32       forgerRealId = clientIdsArr[survivorIdx[0]];
        const u32 AUTHOR_ID    = 0u;
        const u32 NET_ID       = 9003u;

        clientLinks[survivorIdx[0]]->queueWorldClaim(FORGED_OWNER, AUTHOR_ID, &NET_ID, 1, 1000u);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);

        Sleep(400);
        std::deque<InboundWorldClaim> hostAcc2;
        drainWorldClaimsInto(hostInbound, hostAcc2);
        bool neverReachedHost = !hasWorldClaimFromOwnerFor(hostAcc2, FORGED_OWNER, AUTHOR_ID, NET_ID);

        CHECK("claim forged-owner REJECT: rejected at the host, zero downstream effect",
              sawReject && neverReachedHost);
    }

    {
        // Leg D: claimant disconnect BEFORE the window finalizes - its claim
        // is voided (the real host's clearPeerReplicationState ->
        // coop::claimEraseClaimant), and the remaining claimant wins. nettest
        // proves this at the routing level: the disconnecting claimant's
        // intent DID reach the host before it left, but the host (as this
        // test simulates it, mirroring the real applyClaimIntents outcome)
        // authors its verdict naming ONLY the surviving claimant.
        Inbound c9Inbound;
        NetLink client9;
        CHECK("claim contention (disconnect): reconnecting client startClient",
              client9.startClient("127.0.0.1", PORT, &c9Inbound));
        CHECK("claim contention (disconnect): reclaimed the freed slot (id=2)",
              waitForLocalId(client9, 2, 5000));
        u32 departing = client9.localId();
        u32 survivor  = clientIdsArr[survivorIdx[0]];
        const u32 AUTHOR_ID = 0u;
        const u32 NET_ID    = 9004u;

        client9.queueWorldClaim(departing, AUTHOR_ID, &NET_ID, 1, 1000u);
        clientLinks[survivorIdx[0]]->queueWorldClaim(survivor, AUTHOR_ID, &NET_ID, 1, 5000u);

        std::deque<InboundWorldClaim> hostAcc;
        bool hostGotBoth =
            waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, departing, AUTHOR_ID, NET_ID, 2000) &&
            waitForWorldClaimFromOwnerFor(hostInbound, hostAcc, survivor, AUTHOR_ID, NET_ID, 2000);
        CHECK("claim contention (disconnect): host received both claims before the departure",
              hostGotBoth);

        // Disconnect BEFORE the host ever authors a verdict for this
        // identity - coop::claimEraseClaimant voids the departed claimant's
        // entry (the real plugin-side bookkeeping is proven by prototest's
        // claimEraseClaimant cases + the "[leave] claim entries voided="
        // log line).
        client9.stop();
        // Sixth distinct leave(2) this run: idx2's original departure,
        // client5, client6 (Leg A pre-commit), client7 (Leg B post-commit),
        // client8 (this plan's 3-way contention leg), now client9.
        CHECK("claim contention (disconnect): host observed the departure",
              waitForHostLeaveCount(hostInbound, departing, 6, 12000));

        ClaimVerdictPacket cv; std::memset(&cv, 0, sizeof(cv));
        cv.type = (u8)PKT_CLAIM_VERDICT;
        cv.authorId = AUTHOR_ID; cv.netId = NET_ID;
        cv.winnerPlayerId = survivor; // the ONLY remaining claimant
        cv.verdict = (u8)CLAIM_AWARD;
        host.queueClaimVerdict(cv);

        std::deque<InboundClaimVerdict> vSurv;
        bool gotVerdict = waitForClaimVerdictFor(*clientInboxes[survivorIdx[0]], vSurv,
                                                 AUTHOR_ID, NET_ID, 2000);
        CHECK("claim contention (disconnect): the remaining claimant wins uncontested",
              gotVerdict && vSurv[vSurv.size() - 1].pkt.winnerPlayerId == survivor);
    }

    // ---- Phase 8 Plan 02 Task 3 (WORLD-03): census relay + cell-claim
    // disconnect/rejoin. Proves the ROUTING half only (nettest links
    // NetLink.cpp, not Replicator - the actual per-owner census intake,
    // owner-scoped disconnect purge, and connect-edge claimSlots_ purge are
    // Replicator member functions with no unit-test seam outside the full
    // plugin/scenario harness). "the host's next map"/"accepted" below are
    // asserted by AUTHORING the packet a correct Replicator would produce
    // (the same simulated-verdict shape the INV-03 claim-contention legs
    // above already use for PKT_CLAIM_VERDICT).
    std::printf("\n-- Phase 8 Plan 02 Task 3: PKT_NPC_CENSUS relay (WORLD-03) --\n");
    {
        u32 senderReal = clientIdsArr[survivorIdx[0]];
        u32 hand[5] = { 1u, 2u, 3u, 4u, 5u };
        float pos[3] = { 10.0f, 0.0f, 20.0f };
        clientLinks[survivorIdx[0]]->queueNpcCensus(senderReal, hand, pos, 1);

        std::deque<InboundNpcCensus> hostAcc;
        bool hostGot = waitForNpcCensusFromOwner(hostInbound, hostAcc, senderReal, 2000);
        CHECK("PKT_NPC_CENSUS: host received the join's census intact (WORLD-03)", hostGot);

        std::deque<InboundNpcCensus> otherAcc;
        bool otherGot = waitForNpcCensusFromOwner(*clientInboxes[survivorIdx[1]], otherAcc,
                                                  senderReal, 2000);
        CHECK("PKT_NPC_CENSUS: relayed to ANOTHER join with ownerId intact "
              "(Class A relay, WORLD-03)", otherGot);

        Sleep(300);
        std::deque<InboundNpcCensus> selfAcc;
        drainNpcCensusInto(*clientInboxes[survivorIdx[0]], selfAcc);
        CHECK("PKT_NPC_CENSUS: no self-echo to the author",
              !hasNpcCensusFromOwner(selfAcc, senderReal));

        const u32 FORGED_OWNER = 5555u;
        u32       forgerRealId = clientIdsArr[survivorIdx[1]];
        clientLinks[survivorIdx[1]]->queueNpcCensus(FORGED_OWNER, hand, pos, 1);
        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u", (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 2000);
        Sleep(300);
        std::deque<InboundNpcCensus> hostAcc2;
        drainNpcCensusInto(hostInbound, hostAcc2);
        CHECK("PKT_NPC_CENSUS forged-owner REJECT: rejected at the host, zero downstream "
              "effect (WORLD-03)", sawReject && !hasNpcCensusFromOwner(hostAcc2, FORGED_OWNER));
    }

    std::printf("\n-- Phase 8 Plan 02 Task 3: cell-claim disconnect revert-to-host "
                "(WORLD-03) --\n");
    {
        Inbound c10Inbound;
        NetLink client10;
        CHECK("cell disconnect: reconnecting client startClient",
              client10.startClient("127.0.0.1", PORT, &c10Inbound));
        CHECK("cell disconnect: reclaimed the freed slot (id=2)",
              waitForLocalId(client10, 2, 5000));
        u32 departingId = client10.localId();

        CellClaimPacket cc; std::memset(&cc, 0, sizeof(cc));
        cc.type = (u8)PKT_CELL_CLAIM; cc.ownerId = departingId;
        cc.tabRank = 0u; cc.seq = 1u; cc.cellX = 50; cc.cellY = 50;
        client10.queueCellClaim(cc);

        std::deque<InboundCellClaim> hostAcc;
        bool hostGot = waitForCellClaimFromOwner(hostInbound, hostAcc, departingId, 2000);
        CHECK("cell disconnect: host received the claim before departure", hostGot);

        client10.stop();
        // Seventh distinct leave(2) this run (idx2's original departure,
        // client5-9, now client10).
        CHECK("cell disconnect: host observed the departure",
              waitForHostLeaveCount(hostInbound, departingId, 7, 12000));

        // The host's next map (clearPeerReplicationState's owner-scoped
        // erase of claimSlots_/cellLastOwner_ has already run by the time
        // computeAndBroadcastCellMap's next reduce fires) no longer names
        // the departed owner for the cell it held - revert-to-host.
        CellMapPacket cmp; std::memset(&cmp, 0, sizeof(cmp));
        cmp.type = (u8)PKT_CELL_MAP; cmp.seq = 2u; cmp.count = 1u;
        cmp.entries[0].cellX = 50; cmp.entries[0].cellY = 50; cmp.entries[0].ownerId = 0u;
        host.queueCellMap(cmp);

        std::deque<InboundCellMap> survAcc;
        bool got = waitForCellMap(*clientInboxes[survivorIdx[0]], survAcc, 2000);
        CHECK("cell disconnect: the host's next map no longer names the departed owner "
              "(reverts to host, WORLD-03)",
              got && !survAcc.empty() &&
              survAcc[survAcc.size() - 1].pkt.entries[0].ownerId == 0u);
    }

    std::printf("\n-- Phase 8 Plan 02 Task 3: cell-claim rejoin acceptance (WORLD-03) --\n");
    {
        Inbound c12Inbound;
        NetLink client12;
        CHECK("cell rejoin: reconnecting client startClient",
              client12.startClient("127.0.0.1", PORT, &c12Inbound));
        CHECK("cell rejoin: reclaimed the freed slot (id=2)",
              waitForLocalId(client12, 2, 5000));
        u32 rejoinedId = client12.localId();

        // Restarted seq at 1 - a fresh client process, exactly the
        // connect-edge purgeAuthorConservationState contract's premise
        // (no stale claimSlots_ entry from the PREVIOUS connection survives
        // to make this look "stale" against a signed-wrap guard).
        CellClaimPacket cc; std::memset(&cc, 0, sizeof(cc));
        cc.type = (u8)PKT_CELL_CLAIM; cc.ownerId = rejoinedId;
        cc.tabRank = 0u; cc.seq = 1u; cc.cellX = 60; cc.cellY = 60;
        client12.queueCellClaim(cc);

        std::deque<InboundCellClaim> hostAcc;
        bool got = waitForCellClaimFromOwner(hostInbound, hostAcc, rejoinedId, 2000);
        CHECK("cell rejoin: host accepted the reconnected owner's restarted-seq claim "
              "(WORLD-03)", got);
        client12.stop();
    }

    std::printf("\n-- Phase 9 Plan 01 Task 3: money-delta restarted-seq reconnect "
                "(CONS-01) --\n");
    {
        // client12 (the previous leg's occupant of id=2) stopped without
        // waiting for the host to observe its departure - wait for that 8th
        // distinct leave(2) now, or this reconnect races client12's async
        // teardown and either the connect is refused (slot still occupied)
        // or a different slot id is assigned.
        CHECK("money reconnect: host observed the previous occupant's departure "
              "before this reconnect", waitForHostLeaveCount(hostInbound, 2, 8, 12000));

        Inbound c13Inbound;
        NetLink client13;
        CHECK("money reconnect: reconnecting client startClient",
              client13.startClient("127.0.0.1", PORT, &c13Inbound));
        CHECK("money reconnect: reclaimed the freed slot (id=2)",
              waitForLocalId(client13, 2, 5000));
        u32 rejoinedMoneyId = client13.localId();

        // Restarted seq at 1 - a fresh client process, exactly the
        // connect-edge purgeAuthorConservationState contract's premise (no
        // stale moneyFold_.processed/poolFoldSeen_ entry from the PREVIOUS
        // connection survives to make this look like an already-processed
        // replay). nettest proves the TRANSPORT: the restarted-seq delta
        // routes to the host with ownerId intact; the fold-dedup DECISION
        // (processed.erase at the connect edge -> MONEY_FOLD, not
        // MONEY_DUP) is prototest's job (testMoneyFold's reconnect pair).
        MoneyDeltaPacket restarted; std::memset(&restarted, 0, sizeof(restarted));
        restarted.type = (u8)PKT_MONEY_DELTA; restarted.ownerId = rejoinedMoneyId;
        restarted.seq = 1u; restarted.delta = -15; restarted.authorSpendMs = 1u;
        client13.queueMoneyDelta(restarted);

        std::deque<InboundMoneyDelta> hostAcc2;
        bool gotRestarted = false;
        DWORD deadline2 = GetTickCount() + 2000;
        do {
            drainMoneyDeltasInto(hostInbound, hostAcc2);
            gotRestarted = hasMoneyDeltaTuple(hostAcc2, rejoinedMoneyId, 1u);
            if (gotRestarted) break;
            Sleep(20);
        } while (GetTickCount() < deadline2);
        CHECK("money reconnect: host received the restarted seq=1 delta with "
              "ownerId intact (CONS-01)", gotRestarted);

        // A captured pre-disconnect delta replayed on the NEW link is a
        // distinct (ownerId,seq) tuple the host still receives at the
        // transport level (whether the fold layer would treat it as a
        // replay depends on the OLD connection's high-water, which
        // moneyFold_.processed no longer carries post-purge - prototest's
        // job, not this leg's).
        MoneyDeltaPacket replayed; std::memset(&replayed, 0, sizeof(replayed));
        replayed.type = (u8)PKT_MONEY_DELTA; replayed.ownerId = rejoinedMoneyId;
        replayed.seq = 99u; replayed.delta = -1; replayed.authorSpendMs = 2u;
        client13.queueMoneyDelta(replayed);
        bool gotReplayed = false;
        deadline2 = GetTickCount() + 2000;
        do {
            drainMoneyDeltasInto(hostInbound, hostAcc2);
            gotReplayed = hasMoneyDeltaTuple(hostAcc2, rejoinedMoneyId, 99u);
            if (gotReplayed) break;
            Sleep(20);
        } while (GetTickCount() < deadline2);
        CHECK("money reconnect: a distinct (ownerId,seq) on the new link also "
              "routes to the host (CONS-01)", gotReplayed);
        client13.stop();
    }

    // ---- Phase 9 Plan 02 Task 3 (CONS-02): speed vote routing -----------------
    // Transport-level proof only: NetLink/ENet must deliver every connected
    // voter's REQ to the host with ownerId/seq intact (including a second
    // sender's LOWER seq arriving after a first sender's higher one - the
    // per-sender seqSeen collision the OLD single speedSeqSeen_ scalar would
    // have censored), and after a voter disconnects, the survivors' votes
    // must still route. The host-side RECOMPUTE (speedReduce raising the
    // effective) is prototest's job (testSpeedReduce's disconnect case) and
    // the Plan 03/04 live gate's - nettest links NetLink.cpp only, no
    // Replicator/engine dependency.
    std::printf("\n-- Phase 9 Plan 02 Task 3: speed vote routing (CONS-02) --\n");
    {
        // Reclaim the slot the previous leg's occupant (client13) just
        // vacated - the same wait-then-reconnect idiom every reconnect leg
        // above uses.
        CHECK("speed vote: host observed the previous occupant's departure "
              "before this reconnect", waitForHostLeaveCount(hostInbound, 2, 9, 12000));

        Inbound c14Inbound;
        NetLink client14;
        CHECK("speed vote: reconnecting client startClient",
              client14.startClient("127.0.0.1", PORT, &c14Inbound));
        CHECK("speed vote: reclaimed the freed slot (id=2)",
              waitForLocalId(client14, 2, 12000));
        // Phase 11 Plan 01 fix (nettest-only, the true root cause behind the
        // "3-4 timing-flaky legs" this plan set out to harden): id1/id3 MUST
        // be the surviving clients' own real ids (clientLinks/clientIdsArr
        // indexed by survivorIdx), never the literal client1/client3
        // variables or clientIdsArr[0]/[2] by position. The id=2 client
        // stopped at line ~2348 above is picked DYNAMICALLY (idx2 -
        // "whichever of client1/client2/client3 actually holds PlayerId 2",
        // since the initial 3-way connect race gives no ordering guarantee,
        // only CHECKed set-membership in {1,2,3}). When that dynamic pick
        // happens to BE client1 or client3, the client1/client3 VARIABLES
        // are already a dead connection by this point in main() - sending
        // through them here queues into a stopped net thread that never
        // flushes, so the packet silently never reaches the wire. Every
        // other leg between the idx2 disconnect and here already goes
        // through survivorIdx (grep confirms no other client1./client3.
        // reference exists in that whole span) - this pair of legs was the
        // one place that regressed to the literal-variable shortcut.
        u32 id1 = clientIdsArr[survivorIdx[0]]; // a real surviving client's own id
        u32 id2 = client14.localId(); // the reclaimed slot (id=2)
        u32 id3 = clientIdsArr[survivorIdx[1]]; // the OTHER real surviving client's own id

        std::deque<InboundSpeed> hostSpeedAcc;

        // (a) 3 distinct voters, one PAUSED (speed=0), all reach the host
        // with ownerId/seq intact.
        SpeedPacket s1; std::memset(&s1, 0, sizeof(s1));
        s1.type = (u8)PKT_SPEED_REQ; s1.ownerId = id1; s1.seq = 1u; s1.speed = 3.0f;
        clientLinks[survivorIdx[0]]->queueSpeed(s1);
        SpeedPacket s2; std::memset(&s2, 0, sizeof(s2));
        s2.type = (u8)PKT_SPEED_REQ; s2.ownerId = id2; s2.seq = 1u;
        s2.speed = 0.0f; s2.flags = SPEED_PAUSED; // the pauser
        client14.queueSpeed(s2);
        SpeedPacket s3; std::memset(&s3, 0, sizeof(s3));
        s3.type = (u8)PKT_SPEED_REQ; s3.ownerId = id3; s3.seq = 1u; s3.speed = 5.0f;
        clientLinks[survivorIdx[1]]->queueSpeed(s3);

        bool allThree = waitForSpeedTuple(hostInbound, hostSpeedAcc, id1, 1u, 12000) &&
                         waitForSpeedTuple(hostInbound, hostSpeedAcc, id2, 1u, 12000) &&
                         waitForSpeedTuple(hostInbound, hostSpeedAcc, id3, 1u, 12000);
        CHECK("speed vote: all 3 voters' seq=1 REQ arrived at the host with "
              "ownerId intact, one carrying SPEED_PAUSED (CONS-02)", allThree);

        // (b) per-sender-seq-both-register: id2 sends seq=5, THEN id3 sends
        // seq=2 (lower) - the exact collision a single cross-sender
        // speedSeqSeen_ scalar would have silently dropped (id3's seq=2
        // read as "stale" against id2's already-higher counter). Both must
        // arrive distinct at the host.
        SpeedPacket highSeq; std::memset(&highSeq, 0, sizeof(highSeq));
        highSeq.type = (u8)PKT_SPEED_REQ; highSeq.ownerId = id2; highSeq.seq = 5u;
        highSeq.speed = 2.0f;
        client14.queueSpeed(highSeq);
        bool gotHighSeq = waitForSpeedTuple(hostInbound, hostSpeedAcc, id2, 5u, 12000);
        CHECK("speed vote: id2's seq=5 REQ arrived (sets up the collision)",
              gotHighSeq);

        SpeedPacket lowSeqOther; std::memset(&lowSeqOther, 0, sizeof(lowSeqOther));
        lowSeqOther.type = (u8)PKT_SPEED_REQ; lowSeqOther.ownerId = id3; lowSeqOther.seq = 2u;
        lowSeqOther.speed = 4.0f;
        clientLinks[survivorIdx[1]]->queueSpeed(lowSeqOther);
        bool gotOtherLowSeq = waitForSpeedTuple(hostInbound, hostSpeedAcc, id3, 2u, 12000);
        CHECK("speed vote: a DIFFERENT sender's lower seq=2 still arrives at "
              "the host, distinct from id2's seq=5 (per-sender seq, not a "
              "cross-sender scalar) (CONS-02)", gotOtherLowSeq);

        // Disconnect the pauser (id2/client14) - the "instant vote drop"
        // transport proof: the survivors' votes still route afterward.
        client14.stop();
        CHECK("speed vote: host observed the pauser's departure",
              waitForHostLeaveCount(hostInbound, 2, 10, 12000));

        SpeedPacket survivor1; std::memset(&survivor1, 0, sizeof(survivor1));
        survivor1.type = (u8)PKT_SPEED_REQ; survivor1.ownerId = id1; survivor1.seq = 2u;
        survivor1.speed = 3.0f;
        clientLinks[survivorIdx[0]]->queueSpeed(survivor1);
        SpeedPacket survivor3; std::memset(&survivor3, 0, sizeof(survivor3));
        survivor3.type = (u8)PKT_SPEED_REQ; survivor3.ownerId = id3; survivor3.seq = 3u;
        survivor3.speed = 5.0f;
        clientLinks[survivorIdx[1]]->queueSpeed(survivor3);
        bool survivorsRoute =
            waitForSpeedTuple(hostInbound, hostSpeedAcc, id1, 2u, 12000) &&
            waitForSpeedTuple(hostInbound, hostSpeedAcc, id3, 3u, 12000);
        CHECK("speed vote: after the pauser leaves, both survivors' votes "
              "still route to the host (CONS-02)", survivorsRoute);
    }

    // ---- Phase 9 Plan 02 Task 1 (CONS-02): PKT_SPEED_REQ forged-owner --------
    std::printf("\n-- PKT_SPEED_REQ forged-owner REJECT (CONS-02) --\n");
    {
        const u32 FORGED_OWNER = 4210u; // outside {1,2,3}
        const int forgerIdx    = 2;     // client3
        u32       forgerRealId = clientIdsArr[forgerIdx];

        SpeedPacket fsp; std::memset(&fsp, 0, sizeof(fsp));
        fsp.type = (u8)PKT_SPEED_REQ; fsp.ownerId = FORGED_OWNER; fsp.seq = 77u;
        fsp.speed = 1.0f;
        clientLinks[forgerIdx]->queueSpeed(fsp);

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                  (unsigned)forgerRealId);
        needle[sizeof(needle) - 1] = '\0';
        bool sawReject = waitForLogContains(needle, 12000);

        Sleep(400);
        std::deque<InboundSpeed> hostAcc;
        drainSpeedInto(hostInbound, hostAcc);
        bool neverReachedHost = !hasSpeedTuple(hostAcc, FORGED_OWNER, 77u);

        CHECK("PKT_SPEED_REQ forged-owner REJECT: rejected at the host, zero "
              "downstream effect, never landed in speedVotes_ (CONS-02)",
              sawReject && neverReachedHost);
    }

    // ---- Phase 9 Plan 02 Task 2 (CONS-03): time report-scoping ----------------
    // 3 distinct connected clients report distinct clocks; the host must
    // receive all three without a cross-sender drop (the single timeSeqSeen_
    // collision this plan's per-owner timeReports_ map exists to close).
    std::printf("\n-- Phase 9 Plan 02 Task 2/3: time report-scoping (CONS-03) --\n");
    {
        CHECK("time report: host observed the speed leg's departure "
              "before this reconnect", waitForHostLeaveCount(hostInbound, 2, 10, 12000));

        Inbound c15Inbound;
        NetLink client15;
        CHECK("time report: reconnecting client startClient",
              client15.startClient("127.0.0.1", PORT, &c15Inbound));
        CHECK("time report: reclaimed the freed slot (id=2)",
              waitForLocalId(client15, 2, 12000));
        // Phase 11 Plan 01 fix (nettest-only): same survivorIdx correction as
        // the speed-vote leg above - id1/id3 must be the surviving clients'
        // own real ids, never clientIdsArr[0]/[2]/client1/client3 by literal
        // position (see the comment on the speed-vote leg's id1/id3 for the
        // full root-cause explanation).
        u32 id1 = clientIdsArr[survivorIdx[0]];
        u32 id2 = client15.localId(); // the reclaimed slot (id=2)
        u32 id3 = clientIdsArr[survivorIdx[1]];

        TimePacket t1; std::memset(&t1, 0, sizeof(t1));
        t1.type = (u8)PKT_TIME; t1.ownerId = id1; t1.seq = 1u; t1.gameHours = 10.0;
        clientLinks[survivorIdx[0]]->queueTime(t1);
        TimePacket t2; std::memset(&t2, 0, sizeof(t2));
        t2.type = (u8)PKT_TIME; t2.ownerId = id2; t2.seq = 1u; t2.gameHours = 20.0;
        client15.queueTime(t2);
        TimePacket t3; std::memset(&t3, 0, sizeof(t3));
        t3.type = (u8)PKT_TIME; t3.ownerId = id3; t3.seq = 1u; t3.gameHours = 30.0;
        clientLinks[survivorIdx[1]]->queueTime(t3);

        std::deque<InboundTime> got;
        DWORD deadline = GetTickCount() + 12000;
        bool haveAll = false;
        do {
            drainTimeInto(hostInbound, got);
            haveAll = hasTimeTuple(got, id1, 1u) && hasTimeTuple(got, id2, 1u) &&
                      hasTimeTuple(got, id3, 1u);
            if (haveAll) break;
            Sleep(20);
        } while (GetTickCount() < deadline);
        drainTimeInto(hostInbound, got);

        std::set<std::pair<u32, u32> > distinctTuples;
        for (size_t i = 0; i < got.size(); ++i)
            distinctTuples.insert(std::make_pair(got[i].pkt.ownerId, got[i].pkt.seq));

        CHECK("time report: all 3 clients' distinct clock reports arrived at "
              "the host (CONS-03)", haveAll);
        CHECK("time report: exactly 3 distinct (ownerId,seq) tuples (none "
              "coalesced/dropped by a cross-sender guard)",
              distinctTuples.size() == 3);

        // Plain loop (C++03/v100 build: no lambdas) - verify each owner's own
        // gameHours payload survives routing intact.
        bool g1 = false, g2 = false, g3 = false;
        for (size_t i = 0; i < got.size(); ++i) {
            if (got[i].pkt.ownerId == id1 && got[i].pkt.seq == 1u && got[i].pkt.gameHours == 10.0) g1 = true;
            if (got[i].pkt.ownerId == id2 && got[i].pkt.seq == 1u && got[i].pkt.gameHours == 20.0) g2 = true;
            if (got[i].pkt.ownerId == id3 && got[i].pkt.seq == 1u && got[i].pkt.gameHours == 30.0) g3 = true;
        }
        CHECK("time report: each owner's own gameHours value survives routing "
              "intact (CONS-03)", g1 && g2 && g3);

        client15.stop();
    }

    client1.stop();
    client2.stop();
    client3.stop();
    host.stop();

    // ---- Phase 11 Plan 01 (COMPAT-02): raw-HELLO forged-version mismatch --
    // Exercises the host-side clean protocol-mismatch reject at
    // NetLink.cpp:831-838 over a REAL ENet loopback for the first time -
    // previously only a passing comment mentioned this leg's rejection is
    // "like a protocol-version mismatch" (see the 4th-client-rejected leg
    // near the top of main()); no leg here ever actually forged a version.
    // A raw ENet peer - NOT NetLink, which always sends the real
    // PROTOCOL_VERSION with no product-code path to lie about it - hand-
    // builds a HelloPacket with version = PROTOCOL_VERSION + 1 and sends it
    // directly. Self-contained topology (its own host + port), run AFTER
    // every shared-topology leg above has torn down, same isolation
    // rationale as the coordinated-save leg below. No product code changes:
    // this is nettest-only, reusing the existing proven waitFor* helpers.
    std::printf("\n-- raw-HELLO forged-version mismatch: clean reject, no "
                "registry residue (COMPAT-02) --\n");
    {
        const int MISMATCH_PORT = 28250;
        Inbound mHostInbound;
        NetLink mHost;
        CHECK("mismatch leg: host startHost",
              mHost.startHost(MISMATCH_PORT, &mHostInbound));
        Sleep(200);

        ENetAddress addr;
        addr.host = 0;
        enet_address_set_host(&addr, "127.0.0.1");
        addr.port = (enet_uint16)MISMATCH_PORT;

        ENetHost* forgerHost = enet_host_create(0, 1, 3 /*channels*/, 0, 0);
        CHECK("mismatch leg: raw forger enet_host_create", forgerHost != 0);
        ENetPeer* forgerPeer = 0;
        if (forgerHost) {
            forgerPeer = enet_host_connect(forgerHost, &addr, 3, 0);
            CHECK("mismatch leg: raw forger enet_host_connect", forgerPeer != 0);
        }

        bool sawConnect = false;
        if (forgerPeer) {
            DWORD deadline = GetTickCount() + 12000;
            do {
                ENetEvent ev;
                while (enet_host_service(forgerHost, &ev, 10) > 0) {
                    if (ev.type == ENET_EVENT_TYPE_CONNECT) sawConnect = true;
                    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                }
                if (sawConnect) break;
            } while (GetTickCount() < deadline);
        }
        CHECK("mismatch leg: raw forger's ENet-level CONNECT completed "
              "(the HELLO app packet, sent next, carries the forged "
              "version)", sawConnect);

        if (sawConnect) {
            HelloPacket h;
            h.type    = (u8)PKT_HELLO;
            h.version = (u16)(PROTOCOL_VERSION + 1);
            h.nameLen = 0;
            ENetPacket* out = enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(forgerPeer, TEST_CH_RELIABLE, out);
            enet_host_flush(forgerHost);
        }

        char needle[64];
        _snprintf(needle, sizeof(needle) - 1,
                  "protocol mismatch: peer v%u", (unsigned)(PROTOCOL_VERSION + 1));
        needle[sizeof(needle) - 1] = '\0';
        bool sawMismatchLog = waitForLogContains(needle, 12000);
        CHECK("mismatch leg: host logged the clean protocol-mismatch reject "
              "(COMPAT-02, NetLink.cpp:831-838)", sawMismatchLog);

        // Negative-evidence settle (same shape as the forged-owner leg's
        // Sleep(400) above): the forger must NEVER receive a WELCOME, i.e.
        // never acquire a local id. There is no NetLink myId_ in play here
        // (this is a raw peer, not a NetLink client), so the proof is
        // direct: no RECEIVE event ever carries a PKT_WELCOME-tagged
        // payload during a bounded settle window.
        bool neverWelcomed = true;
        if (forgerHost) {
            DWORD deadline = GetTickCount() + 2000;
            do {
                ENetEvent ev;
                while (enet_host_service(forgerHost, &ev, 10) > 0) {
                    if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                        if (ev.packet->dataLength >= 1 &&
                            ((const u8*)ev.packet->data)[0] == (u8)PKT_WELCOME) {
                            neverWelcomed = false;
                        }
                        enet_packet_destroy(ev.packet);
                    }
                }
            } while (GetTickCount() < deadline);
        }
        CHECK("mismatch leg: the forger never received a WELCOME - no local "
              "id ever assigned (COMPAT-02)", neverWelcomed);

        if (forgerHost) {
            if (forgerPeer) enet_peer_disconnect_now(forgerPeer, 0);
            enet_host_destroy(forgerHost);
        }

        // A subsequently-connecting CORRECT-version client still connects
        // and gets its expected slot (id=1, the first slot on this leg's
        // fresh host) - proving the forged HELLO left no registry residue.
        Inbound mc1Inbound;
        NetLink mc1;
        CHECK("mismatch leg: correct-version client startClient",
              mc1.startClient("127.0.0.1", MISMATCH_PORT, &mc1Inbound));
        CHECK("mismatch leg: correct-version client connects and gets its "
              "expected slot (id=1) - the forged HELLO left no registry "
              "residue (COMPAT-02)",
              waitForLocalId(mc1, 1, 12000));
        mc1.stop();
        mHost.stop();
    }

    // ---- Phase 10 Plan 01 (SAVE-01): per-client coordinated-save routing +
    // the marshaled kickPeer drop lever, over REAL NetLink loopback ----------
    // Self-contained topology (its own host + 3 clients on a separate port),
    // run AFTER every shared-topology leg above has torn down - its several
    // seconds of Sleep()-driven ENet handshakes/timeouts must never perturb
    // the timing-sensitive reconnect legs above (moved here after an earlier
    // placement mid-file measurably tightened their generous-but-finite
    // windows). Proves what only a real transport can: a coordinated BEGIN
    // reaches ALL connected clients (destId=OWNER_ID_ALL), a retry BEGIN
    // targets ONLY the failed client (destId=<owner> - the others' inboxes
    // stay empty of it), and kickPeer disconnects ONLY that one client's
    // peer while the survivors' connections stand. The ACK/retry/drop
    // DECISION logic (SaveCoord.h) is prototest's job; this leg proves the
    // ROUTING/transport it drives.
    std::printf("\n-- coordinated-save routing: broadcast BEGIN + destId retry + "
                "kickPeer (SAVE-01) --\n");
    {
        const int SAVE_PORT = 28200;
        Inbound sHostInbound;
        NetLink sHost;
        CHECK("save leg: host startHost", sHost.startHost(SAVE_PORT, &sHostInbound));
        Sleep(200);

        Inbound sc1Inbound, sc2Inbound, sc3Inbound;
        NetLink sc1, sc2, sc3;
        CHECK("save leg: client1 startClient",
              sc1.startClient("127.0.0.1", SAVE_PORT, &sc1Inbound));
        CHECK("save leg: client2 startClient",
              sc2.startClient("127.0.0.1", SAVE_PORT, &sc2Inbound));
        CHECK("save leg: client3 startClient",
              sc3.startClient("127.0.0.1", SAVE_PORT, &sc3Inbound));

        std::deque<u32> sConnAcc;
        {
            DWORD deadline = GetTickCount() + 5000;
            do {
                std::deque<u32> c;
                sHostInbound.drainConnects(c);
                for (size_t i = 0; i < c.size(); ++i) sConnAcc.push_back(c[i]);
                if (sConnAcc.size() >= 3) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("save leg: host observed 3 connect events", sConnAcc.size() >= 3);
        Sleep(200);

        Inbound* sInboxes[3] = { &sc1Inbound, &sc2Inbound, &sc3Inbound };
        u32      sIds[3]     = { sc1.localId(), sc2.localId(), sc3.localId() };

        // Coordinated BEGIN (destId=OWNER_ID_ALL) reaches all 3 connected clients.
        SaveBeginPacket sb; std::memset(&sb, 0, sizeof(sb));
        sb.type = (u8)PKT_SAVE_BEGIN; sb.ownerId = 0; sb.xferId = 500u;
        std::strncpy(sb.name, "nettest_save", sizeof(sb.name) - 1);
        sb.fileCount = 1; sb.totalBytes = 4096;
        sHost.queueSaveBegin(sb, OWNER_ID_ALL);

        bool gotAll3 = true;
        for (int c = 0; c < 3; ++c) {
            bool got = false;
            DWORD deadline = GetTickCount() + 2000;
            do {
                std::deque<InboundSaveBegin> batch;
                sInboxes[c]->drainSaveBegins(batch);
                for (size_t i = 0; i < batch.size(); ++i)
                    if (batch[i].pkt.xferId == 500u) got = true;
                if (got) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
            if (!got) gotAll3 = false;
        }
        CHECK("coordinated BEGIN (destId=OWNER_ID_ALL) reaches all 3 connected "
              "clients (SAVE-01)", gotAll3);

        // A retry BEGIN targeted at ONE client (destId=<owner>) reaches ONLY
        // that client - the others never see xferId=501 (SAVE-04's queue-side
        // destId primitive, the same one a per-client save retry drives).
        const int failIdx   = 1;
        u32       failOwner = sIds[failIdx];
        SaveBeginPacket sbRetry = sb; sbRetry.xferId = 501u;
        sHost.queueSaveBegin(sbRetry, failOwner);

        bool failGot501 = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                std::deque<InboundSaveBegin> batch;
                sInboxes[failIdx]->drainSaveBegins(batch);
                for (size_t i = 0; i < batch.size(); ++i)
                    if (batch[i].pkt.xferId == 501u) failGot501 = true;
                if (failGot501) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        Sleep(400); // give a would-be leak time to arrive before checking the others
        bool othersNeverSaw501 = true;
        for (int c = 0; c < 3; ++c) {
            if (c == failIdx) continue;
            std::deque<InboundSaveBegin> batch;
            sInboxes[c]->drainSaveBegins(batch);
            for (size_t i = 0; i < batch.size(); ++i)
                if (batch[i].pkt.xferId == 501u) othersNeverSaw501 = false;
        }
        CHECK("retry BEGIN (destId=<one owner>) reaches ONLY that client, the "
              "others never see it (SAVE-01/SAVE-04)", failGot501 && othersNeverSaw501);

        // kickPeer disconnects ONLY that one client's peer - the marshaled
        // net-thread-purity drop lever (NetLink::kickPeer).
        sHost.kickPeer(failOwner);
        bool sawKickLog = waitForLogContains("kickPeer: disconnecting", 2000);
        CHECK("kickPeer: the host logged the marshaled disconnect (SAVE-01)", sawKickLog);

        bool failObservedLeave = false;
        {
            DWORD deadline = GetTickCount() + 8000;
            do {
                std::deque<u32> leaves;
                sInboxes[failIdx]->drainLeaves(leaves);
                if (!leaves.empty()) { failObservedLeave = true; break; }
                Sleep(50);
            } while (GetTickCount() < deadline);
        }
        CHECK("kickPeer: the targeted client observed its own disconnect (SAVE-01)",
              failObservedLeave);

        // The survivors' connections stand: a fresh broadcast still reaches
        // both of them - the kick touched ONLY the targeted peer.
        SaveBeginPacket sbAfter = sb; sbAfter.xferId = 502u;
        sHost.queueSaveBegin(sbAfter, OWNER_ID_ALL);
        bool survivorsStand = true;
        for (int c = 0; c < 3; ++c) {
            if (c == failIdx) continue;
            bool got = false;
            DWORD deadline = GetTickCount() + 2000;
            do {
                std::deque<InboundSaveBegin> batch;
                sInboxes[c]->drainSaveBegins(batch);
                for (size_t i = 0; i < batch.size(); ++i)
                    if (batch[i].pkt.xferId == 502u) got = true;
                if (got) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
            if (!got) survivorsStand = false;
        }
        CHECK("kickPeer: the OTHER two clients' connections stand after the "
              "kick (SAVE-01)", survivorsStand);

        sc1.stop(); sc2.stop(); sc3.stop(); sHost.stop();
    }

    // ---- Phase 10 Plan 01 Task 2 (SAVE-02/SAVE-03): PKT_COORD_REJECT unicast
    // routing + per-client-ACK independence + forged-owner reject, over REAL
    // NetLink loopback -----------------------------------------------------
    // Self-contained topology (its own host + 3 clients, a fresh port), run
    // after every shared-topology/timing-sensitive leg. The ARBITER'S OWN
    // accept/reject/preempt DECISION logic (CoordArbiter) is prototest's job
    // (testCoordArbiter) - this leg proves the transport it drives: a
    // PKT_COORD_REJECT reaches ONLY the named requester (never broadcast -
    // a leaked reject would disclose another client's in-flight request), a
    // client-authored one is rejected host-side, distinct owners' ACKs
    // arrive as distinct owner-tagged entries (never coalesced into one
    // scalar), and a forged ownerId on SaveAck/LoadAck is rejected before
    // ever reaching the host's Inbound.
    std::printf("\n-- PKT_COORD_REJECT unicast + per-client-ACK independence + "
                "forged-owner (SAVE-02/SAVE-03) --\n");
    {
        const int COORD_PORT = 28300;
        Inbound cHostInbound;
        NetLink cHost;
        CHECK("coord leg: host startHost", cHost.startHost(COORD_PORT, &cHostInbound));
        Sleep(200);

        Inbound cc1Inbound, cc2Inbound, cc3Inbound;
        NetLink cc1, cc2, cc3;
        CHECK("coord leg: client1 startClient",
              cc1.startClient("127.0.0.1", COORD_PORT, &cc1Inbound));
        CHECK("coord leg: client2 startClient",
              cc2.startClient("127.0.0.1", COORD_PORT, &cc2Inbound));
        CHECK("coord leg: client3 startClient",
              cc3.startClient("127.0.0.1", COORD_PORT, &cc3Inbound));

        std::deque<u32> cConnAcc;
        {
            DWORD deadline = GetTickCount() + 5000;
            do {
                std::deque<u32> c;
                cHostInbound.drainConnects(c);
                for (size_t i = 0; i < c.size(); ++i) cConnAcc.push_back(c[i]);
                if (cConnAcc.size() >= 3) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("coord leg: host observed 3 connect events", cConnAcc.size() >= 3);
        Sleep(200);

        Inbound* cInboxes[3] = { &cc1Inbound, &cc2Inbound, &cc3Inbound };
        u32      cIds[3]     = { cc1.localId(), cc2.localId(), cc3.localId() };

        // ---- PKT_COORD_REJECT unicast: reaches ONLY the named requester --
        {
            const int rejectedIdx = 1; // client2
            CoordRejectPacket cr; std::memset(&cr, 0, sizeof(cr));
            cr.type = (u8)PKT_COORD_REJECT;
            cr.requesterId       = cIds[rejectedIdx];
            cr.reqId             = 77u;
            cr.kind              = 0; // COORD_SAVE
            cr.reason            = 0; // busy
            cr.activeRequesterId = cIds[0];
            cr.activeReqId       = 55u;
            cHost.queueCoordReject(cr);

            bool rejectedGotIt = false;
            {
                DWORD deadline = GetTickCount() + 2000;
                do {
                    std::deque<InboundCoordReject> batch;
                    cInboxes[rejectedIdx]->drainCoordRejects(batch);
                    for (size_t i = 0; i < batch.size(); ++i)
                        if (batch[i].pkt.reqId == 77u) rejectedGotIt = true;
                    if (rejectedGotIt) break;
                    Sleep(20);
                } while (GetTickCount() < deadline);
            }
            Sleep(400); // give a would-be leak time to arrive before checking the others
            bool othersNeverSawIt = true;
            for (int c = 0; c < 3; ++c) {
                if (c == rejectedIdx) continue;
                std::deque<InboundCoordReject> batch;
                cInboxes[c]->drainCoordRejects(batch);
                for (size_t i = 0; i < batch.size(); ++i)
                    if (batch[i].pkt.reqId == 77u) othersNeverSawIt = false;
            }
            CHECK("PKT_COORD_REJECT: reaches ONLY the named requester (Class D "
                  "unicast, never broadcast) (SAVE-03)",
                  rejectedGotIt && othersNeverSawIt);

            // A client-authored PKT_COORD_REJECT must be rejected outright -
            // a join never has authority to author the arbitration verdict.
            CoordRejectPacket badReject; std::memset(&badReject, 0, sizeof(badReject));
            badReject.type = (u8)PKT_COORD_REJECT;
            badReject.requesterId = cIds[2]; badReject.reqId = 88u;
            cc3.queueCoordReject(badReject);
            bool sawRejectReject = waitForLogContains(
                "PKT_COORD_REJECT from a client rejected", 2000);
            CHECK("PKT_COORD_REJECT: a join-authored one is rejected at the "
                  "host (host-authoritative only, SAVE-03)", sawRejectReject);
        }

        // ---- per-client-ACK independence: 3 distinct owners' SaveAcks all
        // reach the host as distinct owner-tagged entries -----------------
        {
            for (int c = 0; c < 3; ++c) {
                SaveAckPacket ack; std::memset(&ack, 0, sizeof(ack));
                ack.type = (u8)PKT_SAVE_ACK; ack.ownerId = cIds[c]; ack.xferId = 900u;
                ack.ok = (c == 1) ? 0 : 1; // client2 fails, the other two commit
                ack.files = 1; ack.bytes = 4096;
                (c == 0 ? cc1 : c == 1 ? cc2 : cc3).queueSaveAck(ack);
            }
            std::deque<InboundSaveAck> hostAcc;
            DWORD deadline = GetTickCount() + 2000;
            bool haveAll3 = false;
            do {
                std::deque<InboundSaveAck> batch;
                cHostInbound.drainSaveAcks(batch);
                for (size_t i = 0; i < batch.size(); ++i) hostAcc.push_back(batch[i]);
                bool g0 = false, g1 = false, g2 = false;
                for (size_t i = 0; i < hostAcc.size(); ++i) {
                    if (hostAcc[i].pkt.xferId != 900u) continue;
                    if (hostAcc[i].pkt.ownerId == cIds[0]) g0 = true;
                    if (hostAcc[i].pkt.ownerId == cIds[1]) g1 = true;
                    if (hostAcc[i].pkt.ownerId == cIds[2]) g2 = true;
                }
                haveAll3 = g0 && g1 && g2;
                if (haveAll3) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
            CHECK("per-client-ACK independence: 3 distinct owners' SaveAcks "
                  "all reached the host with ownerId intact (SAVE-01)", haveAll3);

            int countForOwner1 = 0;
            for (size_t i = 0; i < hostAcc.size(); ++i)
                if (hostAcc[i].pkt.xferId == 900u && hostAcc[i].pkt.ownerId == cIds[1]) ++countForOwner1;
            CHECK("per-client-ACK independence: client2's ok=0 ACK is its OWN "
                  "distinct entry, not coalesced with the other two's ok=1 "
                  "(the last-of-N-ACKs-wins collapse this plan closes)",
                  countForOwner1 == 1);
        }

        // ---- forged-owner: a SaveAck/LoadAck whose ownerId != source slot
        // is rejected host-side before ever reaching Inbound ---------------
        {
            const u32 FORGED_OWNER = 9210u; // outside {cIds[0..2]}
            const int forgerIdx    = 0;     // client1
            u32       forgerRealId = cIds[forgerIdx];

            SaveAckPacket fsa; std::memset(&fsa, 0, sizeof(fsa));
            fsa.type = (u8)PKT_SAVE_ACK; fsa.ownerId = FORGED_OWNER; fsa.xferId = 901u; fsa.ok = 1;
            (forgerIdx == 0 ? cc1 : forgerIdx == 1 ? cc2 : cc3).queueSaveAck(fsa);

            char needle[64];
            _snprintf(needle, sizeof(needle) - 1, "relay REJECT player=%u",
                      (unsigned)forgerRealId);
            needle[sizeof(needle) - 1] = '\0';
            bool sawReject = waitForLogContains(needle, 2000);

            Sleep(400);
            std::deque<InboundSaveAck> hostAcc;
            cHostInbound.drainSaveAcks(hostAcc);
            bool neverReachedHost = true;
            for (size_t i = 0; i < hostAcc.size(); ++i)
                if (hostAcc[i].pkt.ownerId == FORGED_OWNER && hostAcc[i].pkt.xferId == 901u)
                    neverReachedHost = false;

            CHECK("PKT_SAVE_ACK forged-owner REJECT: rejected at the host, "
                  "zero downstream effect (SAVE-01, T-10-01)",
                  sawReject && neverReachedHost);

            LoadAckPacket fla; std::memset(&fla, 0, sizeof(fla));
            fla.type = (u8)PKT_LOAD_ACK; fla.ownerId = FORGED_OWNER; fla.loadId = 55u; fla.ok = 1;
            (forgerIdx == 0 ? cc1 : forgerIdx == 1 ? cc2 : cc3).queueLoadAck(fla);
            bool sawLoadReject = waitForLogContains(needle, 2000);

            Sleep(400);
            std::deque<InboundLoadAck> hostLoadAcc;
            cHostInbound.drainLoadAcks(hostLoadAcc);
            bool loadNeverReachedHost = true;
            for (size_t i = 0; i < hostLoadAcc.size(); ++i)
                if (hostLoadAcc[i].pkt.ownerId == FORGED_OWNER && hostLoadAcc[i].pkt.loadId == 55u)
                    loadNeverReachedHost = false;

            CHECK("PKT_LOAD_ACK forged-owner REJECT: rejected at the host, "
                  "zero downstream effect (SAVE-02, T-10-01)",
                  sawLoadReject && loadNeverReachedHost);
        }

        cc1.stop(); cc2.stop(); cc3.stop(); cHost.stop();
    }

    // ---- Phase 10 Plan 01 Task 3 (Rejoin): a reconnecting slot's fresh
    // transfer routes, over REAL NetLink loopback --------------------------
    // A client disconnects mid-coordinated-save (its ACK never arrives),
    // then a NEW client reconnects onto the SAME freed slot and a fresh
    // coordinated save reaches it cleanly. This is the TRANSPORT/ROUTING
    // half only (a reused slot's destId-targeted BEGIN actually arrives at
    // the new occupant) - the settle-without-the-departed DECISION is
    // testCoordRejoin's job (prototest, SaveCoord/LoadCoord operate purely
    // on PlayerIds with no notion of "which physical connection" holds one).
    std::printf("\n-- reconnecting slot: a fresh transfer routes after "
                "mid-transfer disconnect (Rejoin, Phase 10 Plan 01 Task 3) --\n");
    {
        const int RJ_PORT = 28400;
        Inbound rHostInbound;
        NetLink rHost;
        CHECK("rejoin leg: host startHost", rHost.startHost(RJ_PORT, &rHostInbound));
        Sleep(200);

        Inbound rc1Inbound, rc2Inbound, rc3Inbound;
        NetLink rc1, rc2, rc3;
        CHECK("rejoin leg: client1 startClient",
              rc1.startClient("127.0.0.1", RJ_PORT, &rc1Inbound));
        CHECK("rejoin leg: client2 startClient",
              rc2.startClient("127.0.0.1", RJ_PORT, &rc2Inbound));
        CHECK("rejoin leg: client3 startClient",
              rc3.startClient("127.0.0.1", RJ_PORT, &rc3Inbound));

        std::deque<u32> rConnAcc;
        {
            DWORD deadline = GetTickCount() + 5000;
            do {
                std::deque<u32> c;
                rHostInbound.drainConnects(c);
                for (size_t i = 0; i < c.size(); ++i) rConnAcc.push_back(c[i]);
                if (rConnAcc.size() >= 3) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("rejoin leg: host observed 3 connect events", rConnAcc.size() >= 3);
        Sleep(200);
        u32 departingId = rc2.localId();

        // A coordinated BEGIN reaches all 3 (mirrors the normal flow up to
        // the point where the disconnecting client would ACK).
        SaveBeginPacket rsb; std::memset(&rsb, 0, sizeof(rsb));
        rsb.type = (u8)PKT_SAVE_BEGIN; rsb.ownerId = 0; rsb.xferId = 700u;
        std::strncpy(rsb.name, "nettest_save", sizeof(rsb.name) - 1);
        rsb.fileCount = 1; rsb.totalBytes = 4096;
        rHost.queueSaveBegin(rsb, OWNER_ID_ALL);
        bool c2Got700 = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                std::deque<InboundSaveBegin> batch;
                rc2Inbound.drainSaveBegins(batch);
                for (size_t i = 0; i < batch.size(); ++i)
                    if (batch[i].pkt.xferId == 700u) c2Got700 = true;
                if (c2Got700) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("rejoin leg: the departing client received the coordinated "
              "BEGIN before disconnecting", c2Got700);

        // client2 disconnects mid-transfer - its ACK never arrives. Polls
        // rHostInbound directly (NOT waitForHostLeaveId/g_hostLeaves - that
        // global accumulates leave ids across the WHOLE file's shared
        // topology, so departingId may already appear in it from an
        // earlier, unrelated leg's slot reuse; a fresh local drain is the
        // only way to observe THIS leg's own disconnect).
        rc2.stop();
        bool sawDeparture = false;
        {
            DWORD deadline = GetTickCount() + 12000;
            do {
                std::deque<u32> leaves;
                rHostInbound.drainLeaves(leaves);
                for (size_t i = 0; i < leaves.size(); ++i)
                    if (leaves[i] == departingId) sawDeparture = true;
                if (sawDeparture) break;
                Sleep(50);
            } while (GetTickCount() < deadline);
        }
        CHECK("rejoin leg: host observed the mid-transfer disconnect", sawDeparture);

        // A NEW client reconnects onto the SAME freed slot.
        Inbound rc4Inbound;
        NetLink rc4;
        CHECK("rejoin leg: reconnecting client startClient",
              rc4.startClient("127.0.0.1", RJ_PORT, &rc4Inbound));
        CHECK("rejoin leg: the reconnecting client reclaimed the freed slot "
              "(lowest-free-slot reuse, no renumber)",
              waitForLocalId(rc4, departingId, 5000));

        // A fresh coordinated save's unicast BEGIN (destId=the reused slot)
        // reaches the NEW occupant cleanly - the routing half of "a reused
        // slot's fresh transfer routes" (the coordinator-state half - no
        // stale SC_FAILED/DROPPED bleeding from the departed occupant - is
        // testCoordRejoin's saveBegin-on-a-reused-id proof).
        SaveBeginPacket rsb2; std::memset(&rsb2, 0, sizeof(rsb2));
        rsb2.type = (u8)PKT_SAVE_BEGIN; rsb2.ownerId = 0; rsb2.xferId = 701u;
        std::strncpy(rsb2.name, "nettest_save", sizeof(rsb2.name) - 1);
        rsb2.fileCount = 1; rsb2.totalBytes = 4096;
        rHost.queueSaveBegin(rsb2, departingId);
        bool reconnectGot701 = false;
        {
            DWORD deadline = GetTickCount() + 2000;
            do {
                std::deque<InboundSaveBegin> batch;
                rc4Inbound.drainSaveBegins(batch);
                for (size_t i = 0; i < batch.size(); ++i)
                    if (batch[i].pkt.xferId == 701u) reconnectGot701 = true;
                if (reconnectGot701) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("rejoin leg: a fresh coordinated save reaches the RECONNECTED "
              "occupant of the reused slot cleanly (Rejoin)", reconnectGot701);

        rc1.stop(); rc3.stop(); rc4.stop(); rHost.stop();
    }

    // ---- Phase 10 Plan 02 (SAVE-04): targeted late-join bootstrap routing +
    // N=2 parity, over REAL NetLink loopback -------------------------------
    // Proves the transport half of the joiner-unicast connect-push: the
    // bootstrap GO + BEGIN/FILE/DONE stream (queue-side destId=joiner) reach
    // ONLY the newcomer - the established players see ZERO save/load-plane
    // bytes after the connect edge (the "reload storm" this plan closes) -
    // and at N=2 the SAME unicast path delivers the identical sequence a
    // broadcast used to (the join receives GO + stream; nothing else exists
    // to not-receive). The per-joiner QUEUE + arbiter-drain DECISION logic
    // (armConnectPush/driveConnectPushQueue, Plugin.cpp) is not linkable here
    // (nettest links NetLink.cpp only) - this leg proves the ROUTING/
    // transport primitive that logic drives, exactly like the SAVE-01 leg
    // above proves per-client retry routing.
    //
    // MAX_PLAYERS=4 (Wire.h) caps a session at 1 host + 3 joins (4 total
    // players) - "3 established, 4th connects late" (10-RESEARCH.md/plan
    // language) means 3 TOTAL established players (host + 2 joins) plus a
    // LATE 3rd join = 4 total players, not 4 established joins.
    std::printf("\n-- targeted late-join bootstrap routing + N=2 parity "
                "(SAVE-04) --\n");
    {
        const int BOOT_PORT = 28500;
        Inbound bHostInbound;
        NetLink bHost;
        CHECK("boot leg: host startHost", bHost.startHost(BOOT_PORT, &bHostInbound));
        Sleep(200);

        // 2 "established" joins (host + these 2 = 3 established total
        // players) connect first; a 3rd join (the late joiner, filling the
        // last of the 3 join slots) connects after - this leg proves
        // ROUTING, not rig timing (the staggered-launch late-join scenario
        // itself is the live gate's job, Plan 03/04).
        Inbound bc1Inbound, bc2Inbound, bc3Inbound;
        NetLink bc1, bc2, bc3;
        CHECK("boot leg: client1 startClient",
              bc1.startClient("127.0.0.1", BOOT_PORT, &bc1Inbound));
        CHECK("boot leg: client2 startClient",
              bc2.startClient("127.0.0.1", BOOT_PORT, &bc2Inbound));

        std::deque<u32> bConnAcc;
        {
            DWORD deadline = GetTickCount() + 5000;
            do {
                std::deque<u32> c;
                bHostInbound.drainConnects(c);
                for (size_t i = 0; i < c.size(); ++i) bConnAcc.push_back(c[i]);
                if (bConnAcc.size() >= 2) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("boot leg: host observed 2 established connect events (3 total "
              "players with the host)", bConnAcc.size() >= 2);
        Sleep(200);

        Inbound* bInboxes[2] = { &bc1Inbound, &bc2Inbound };
        u32      bIds[2]     = { bc1.localId(), bc2.localId() };
        (void)bIds;

        CHECK("boot leg: client3 (late joiner) startClient",
              bc3.startClient("127.0.0.1", BOOT_PORT, &bc3Inbound));
        u32 joinerId = 0;
        {
            DWORD deadline = GetTickCount() + 5000;
            do {
                std::deque<u32> got;
                bHostInbound.drainConnects(got);
                if (!got.empty()) { joinerId = got.back(); break; }
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("boot leg: host observed the late joiner's connect edge", joinerId != 0);
        Sleep(200);

        // The bootstrap: GO + BEGIN/FILE/DONE, all destId=joinerId - the
        // routing primitive Plugin.cpp's connect-push queue drive (armConnectPush/
        // driveConnectPushQueue) issues live.
        LoadGoPacket bgo; std::memset(&bgo, 0, sizeof(bgo));
        bgo.type = (u8)PKT_LOAD_GO; bgo.ownerId = 0; bgo.loadId = 9001u;
        bgo.fingerprint = 0xABCDEF01u;
        std::strncpy(bgo.name, "nettest_boot", sizeof(bgo.name) - 1);
        bHost.queueLoadGo(bgo, joinerId);

        SaveBeginPacket bsb; std::memset(&bsb, 0, sizeof(bsb));
        bsb.type = (u8)PKT_SAVE_BEGIN; bsb.ownerId = 0; bsb.xferId = 9002u;
        std::strncpy(bsb.name, "nettest_boot", sizeof(bsb.name) - 1);
        bsb.fileCount = 1; bsb.totalBytes = 4096;
        bHost.queueSaveBegin(bsb, joinerId);

        unsigned char fdata[16];
        std::memset(fdata, 0, sizeof(fdata));
        SaveFileHeader bfh; std::memset(&bfh, 0, sizeof(bfh));
        bfh.type = (u8)PKT_SAVE_FILE; bfh.ownerId = 0; bfh.xferId = 9002u;
        bfh.fileIdx = 0; bfh.pathLen = 8; bfh.offset = 0; bfh.dataLen = (u16)sizeof(fdata);
        bHost.queueSaveFile(bfh, "quick.sv", fdata, sizeof(fdata), joinerId);

        u32 bcrcs[1] = { 12345u };
        SaveDoneHeader bdh; std::memset(&bdh, 0, sizeof(bdh));
        bdh.type = (u8)PKT_SAVE_DONE; bdh.ownerId = 0; bdh.xferId = 9002u; bdh.fileCount = 1;
        bHost.queueSaveDone(bdh, bcrcs, 1, joinerId);

        // The joiner receives ALL of it.
        bool joinerGotGo = false, joinerGotBegin = false, joinerGotFile = false, joinerGotDone = false;
        {
            DWORD deadline = GetTickCount() + 3000;
            do {
                std::deque<InboundLoadGo> g; bc3Inbound.drainLoadGos(g);
                for (size_t i = 0; i < g.size(); ++i) if (g[i].pkt.loadId == 9001u) joinerGotGo = true;
                std::deque<InboundSaveBegin> b; bc3Inbound.drainSaveBegins(b);
                for (size_t i = 0; i < b.size(); ++i) if (b[i].pkt.xferId == 9002u) joinerGotBegin = true;
                std::deque<InboundSaveFile> f; bc3Inbound.drainSaveFiles(f);
                for (size_t i = 0; i < f.size(); ++i) if (f[i].hdr.xferId == 9002u) joinerGotFile = true;
                std::deque<InboundSaveDone> d; bc3Inbound.drainSaveDones(d);
                for (size_t i = 0; i < d.size(); ++i) if (d[i].hdr.xferId == 9002u) joinerGotDone = true;
                if (joinerGotGo && joinerGotBegin && joinerGotFile && joinerGotDone) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("targeted late-join: the joiner receives GO + BEGIN/FILE/DONE "
              "(SAVE-04)", joinerGotGo && joinerGotBegin && joinerGotFile && joinerGotDone);

        Sleep(400); // give a would-be leak time to arrive at the survivors
        bool survivorsSawNothing = true;
        for (int c = 0; c < 2; ++c) {
            std::deque<InboundLoadGo> g; bInboxes[c]->drainLoadGos(g);
            for (size_t i = 0; i < g.size(); ++i) if (g[i].pkt.loadId == 9001u) survivorsSawNothing = false;
            std::deque<InboundSaveBegin> b; bInboxes[c]->drainSaveBegins(b);
            for (size_t i = 0; i < b.size(); ++i) if (b[i].pkt.xferId == 9002u) survivorsSawNothing = false;
            std::deque<InboundSaveFile> f; bInboxes[c]->drainSaveFiles(f);
            for (size_t i = 0; i < f.size(); ++i) if (f[i].hdr.xferId == 9002u) survivorsSawNothing = false;
            std::deque<InboundSaveDone> d; bInboxes[c]->drainSaveDones(d);
            for (size_t i = 0; i < d.size(); ++i) if (d[i].hdr.xferId == 9002u) survivorsSawNothing = false;
        }
        CHECK("targeted late-join: the 2 established joins receive ZERO "
              "save/load-plane bytes for the bootstrap - no reload storm "
              "(SAVE-04, T-10-07/T-10-08)", survivorsSawNothing);

        bc1.stop(); bc2.stop(); bc3.stop(); bHost.stop();
    }

    // ---- N=2 parity: one join connected -> the unicast bootstrap delivers
    // the identical sequence the old broadcast did (SAVE-04) --------------
    std::printf("\n-- N=2 parity: unicast bootstrap == old broadcast bootstrap "
                "at exactly one join (SAVE-04) --\n");
    {
        const int PARITY_PORT = 28600;
        Inbound pHostInbound;
        NetLink pHost;
        CHECK("parity leg: host startHost", pHost.startHost(PARITY_PORT, &pHostInbound));
        Sleep(200);

        Inbound pc1Inbound;
        NetLink pc1;
        CHECK("parity leg: client1 startClient",
              pc1.startClient("127.0.0.1", PARITY_PORT, &pc1Inbound));
        u32 joinerId2 = 0;
        {
            DWORD deadline = GetTickCount() + 5000;
            do {
                std::deque<u32> c;
                pHostInbound.drainConnects(c);
                if (!c.empty()) { joinerId2 = c.back(); break; }
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("parity leg: host observed the sole join's connect edge", joinerId2 != 0);
        Sleep(200);

        LoadGoPacket pgo; std::memset(&pgo, 0, sizeof(pgo));
        pgo.type = (u8)PKT_LOAD_GO; pgo.ownerId = 0; pgo.loadId = 9101u;
        pgo.fingerprint = 0x11223344u;
        std::strncpy(pgo.name, "nettest_parity", sizeof(pgo.name) - 1);
        pHost.queueLoadGo(pgo, joinerId2);

        SaveBeginPacket psb; std::memset(&psb, 0, sizeof(psb));
        psb.type = (u8)PKT_SAVE_BEGIN; psb.ownerId = 0; psb.xferId = 9102u;
        std::strncpy(psb.name, "nettest_parity", sizeof(psb.name) - 1);
        psb.fileCount = 1; psb.totalBytes = 4096;
        pHost.queueSaveBegin(psb, joinerId2);

        bool parityGotGo = false, parityGotBegin = false;
        {
            DWORD deadline = GetTickCount() + 3000;
            do {
                std::deque<InboundLoadGo> g; pc1Inbound.drainLoadGos(g);
                for (size_t i = 0; i < g.size(); ++i) if (g[i].pkt.loadId == 9101u) parityGotGo = true;
                std::deque<InboundSaveBegin> b; pc1Inbound.drainSaveBegins(b);
                for (size_t i = 0; i < b.size(); ++i) if (b[i].pkt.xferId == 9102u) parityGotBegin = true;
                if (parityGotGo && parityGotBegin) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        CHECK("N=2 parity: the sole join's unicast bootstrap delivers GO + "
              "BEGIN exactly as the old broadcast did (SAVE-04)",
              parityGotGo && parityGotBegin);

        pc1.stop(); pHost.stop();
    }

    // ---- host-link-drop roster teardown (PeerRoster.h) -------------------
    // A CLIENT connected together with 2 OTHER clients loses the host. The
    // host is the only link a client has and every other player's state is
    // relayed through it, so that single ENet DISCONNECT means the whole
    // roster is gone at once - NetLink pushes the OWNER_ID_ALL sentinel for
    // it (the client branch of ENET_EVENT_TYPE_DISCONNECT).
    //
    // The regression this locks (2026-09-07, x4 branch): every consumer of a
    // drained leave id is owner-scoped by `==` equality, so the sentinel used
    // to match NOTHING and the client's entire teardown no-opped. The
    // presence set and Replicator::knownPeers_ kept the dead ids forever
    // (resetSession() deliberately preserves knownPeers_, so no world reload
    // healed it) and each reconnect stacked the new ids on top.
    //
    // This leg drives the REAL transport - a real host + 3 real clients over
    // loopback UDP, then NetLink::stop() on the HOST - and then runs the
    // drained queue through the SAME production expansion Plugin.cpp's
    // processNetEvents uses (coop::expandLeaveQueue), modelling the two
    // owner-scoped roster erases the live loop performs per expanded id. The
    // assertion is the one the plan named: afterwards both roster structures
    // are EMPTY.
    std::printf("\n-- host-link drop at N=4: one sentinel tears down the whole "
                "client-side roster (PeerRoster.h) --\n");
    {
        const int DROP_PORT = 28700;
        Inbound dHostInbound;
        NetLink dHost;
        CHECK("drop leg: host startHost", dHost.startHost(DROP_PORT, &dHostInbound));
        Sleep(200);

        Inbound dc1Inbound, dc2Inbound, dc3Inbound;
        NetLink dc1, dc2, dc3;
        CHECK("drop leg: client1 startClient",
              dc1.startClient("127.0.0.1", DROP_PORT, &dc1Inbound));
        CHECK("drop leg: client2 startClient",
              dc2.startClient("127.0.0.1", DROP_PORT, &dc2Inbound));
        CHECK("drop leg: client3 startClient",
              dc3.startClient("127.0.0.1", DROP_PORT, &dc3Inbound));

        // client1 is the OBSERVER. Build its roster the way Plugin.cpp does:
        // insert every id its own connect drain reports (the host's id 0 via
        // WELCOME, the other two joins via the PKT_PLAYER_JOINED roster
        // broadcast) - g_connectedPeers.insert(*it) plus the
        // Replicator::notePeerConnected(*it) mirror.
        std::set<u32> connected;   // models Plugin.cpp's g_connectedPeers
        std::set<u32> known;       // models Replicator::knownPeers_
        std::deque<u32> connAcc;
        {
            DWORD deadline = GetTickCount() + 8000;
            do {
                drainConnectsInto(dc1Inbound, connAcc);
                if (connAcc.size() >= 3) break; // host + the 2 other joins
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        for (size_t i = 0; i < connAcc.size(); ++i) {
            connected.insert(connAcc[i]);
            known.insert(connAcc[i]);
        }
        CHECK("drop leg: observer tracks the host + both other joins before the "
              "drop (3 ids)", connected.size() == 3 && connected.count(0) == 1);

        // Force the HOST's NetLink down. stop() tears the local ENet host
        // down with no wire-level graceful disconnect, so each client learns
        // via ENet's own peer-timeout detection (deterministic, but not
        // instant - default timeoutMinimum is 5000 ms per third_party/enet's
        // vendored protocol.c), exactly like a real host process dying.
        dHost.stop();

        std::deque<u32> leaveAcc;
        bool sawSentinel = waitForLeaveId(dc1Inbound, leaveAcc, OWNER_ID_ALL, 20000);
        CHECK("drop leg: the observer's leave queue carries the OWNER_ID_ALL "
              "sentinel after the host's NetLink went down", sawSentinel);

        // The production expansion + the per-id owner-scoped erases the live
        // leave loop runs, then the session-boundary clear its flag gates.
        std::deque<u32> expanded;
        bool linkDown = expandLeaveQueue(leaveAcc, connected, expanded);
        for (size_t i = 0; i < expanded.size(); ++i) {
            connected.erase(expanded[i]); // g_connectedPeers.erase(*it)
            known.erase(expanded[i]);     // Replicator::notePeerLeft(*it)
        }
        if (linkDown) known.clear();      // Replicator::clearKnownPeers()

        CHECK("drop leg: the drop is classified as a session boundary, not one "
              "peer's departure", linkDown);
        CHECK("drop leg: the sentinel expanded to all 3 tracked ids, so the "
              "owner-scoped teardown actually ran", expanded.size() == 3);
        CHECK("drop leg: g_connectedPeers is EMPTY after the host-link drop",
              connected.empty());
        CHECK("drop leg: Replicator::knownPeers_ is EMPTY after the host-link "
              "drop", known.empty());

        dc1.stop(); dc2.stop(); dc3.stop();
    }

    // ---- host NetLink restart must not leak join slots (F2 OFFLINE -> ONLINE)
    // ------------------------------------------------------------------------
    // g_net is a reused singleton, so the F2 panel's OFFLINE -> ONLINE toggle
    // is stop() + startHost() on the SAME NetLink object. threadLoop() ends
    // with enet_host_destroy(), which frees the entire ENetPeer array, so
    // every PeerState::peer left in registry_ dangles afterwards. Without a
    // session-boundary reset the restarted host inherits the old slot table:
    // the lowest-free-slot scan in [1, MAX_PLAYERS) sees the stale ids as
    // occupied and refuses legitimate joins with a truthful-looking but wrong
    // "MAX_PLAYERS=4 slots full", while sendTo()/broadcast() would dereference
    // the freed peer pointers.
    //
    // Restarting the HOST (not a client) is what makes this leg deterministic:
    // stop() is local and synchronous, so unlike a client reconnect it needs
    // no ENet peer-timeout wait (see the NOTE on slot reuse in the
    // 4th-client-rejection leg above).
    std::printf("\n-- host NetLink restart does not leak join slots --\n");
    {
        const int RESET_PORT = 28800;
        Inbound rsHostInbound;
        NetLink rsHost;
        CHECK("restart leg: host startHost", rsHost.startHost(RESET_PORT, &rsHostInbound));
        Sleep(200);

        // Fill all 3 join slots in the FIRST session.
        {
            Inbound a1In, a2In, a3In;
            NetLink a1, a2, a3;
            CHECK("restart leg: session-1 client1 startClient",
                  a1.startClient("127.0.0.1", RESET_PORT, &a1In));
            CHECK("restart leg: session-1 client2 startClient",
                  a2.startClient("127.0.0.1", RESET_PORT, &a2In));
            CHECK("restart leg: session-1 client3 startClient",
                  a3.startClient("127.0.0.1", RESET_PORT, &a3In));

            std::deque<u32> acc;
            {
                DWORD deadline = GetTickCount() + 8000;
                do {
                    drainConnectsInto(rsHostInbound, acc);
                    if (acc.size() >= 3) break;
                    Sleep(20);
                } while (GetTickCount() < deadline);
            }
            CHECK("restart leg: session-1 filled all 3 join slots", acc.size() >= 3);

            // Host goes OFFLINE first, exactly like the F2 toggle. Doing this
            // BEFORE stopping the clients is deliberate: it denies the host any
            // chance to observe ENet peer timeouts, so the only thing that can
            // empty registry_ is the session-boundary reset under test.
            rsHost.stop();
            a1.stop(); a2.stop(); a3.stop();
        }
        Sleep(300);

        // Same object back ONLINE - the inherited-slot-table edge.
        CHECK("restart leg: host startHost again on the same NetLink object",
              rsHost.startHost(RESET_PORT, &rsHostInbound));
        Sleep(200);

        Inbound b1In, b2In, b3In;
        NetLink b1, b2, b3;
        CHECK("restart leg: session-2 client1 startClient",
              b1.startClient("127.0.0.1", RESET_PORT, &b1In));
        CHECK("restart leg: session-2 client2 startClient",
              b2.startClient("127.0.0.1", RESET_PORT, &b2In));
        CHECK("restart leg: session-2 client3 startClient",
              b3.startClient("127.0.0.1", RESET_PORT, &b3In));

        std::deque<u32> acc2;
        {
            DWORD deadline = GetTickCount() + 8000;
            do {
                drainConnectsInto(rsHostInbound, acc2);
                if (acc2.size() >= 3) break;
                Sleep(20);
            } while (GetTickCount() < deadline);
        }
        // THE regression check: under the pre-fix code the very first session-2
        // client is rejected ("slots full"), so the host observes ZERO connects.
        CHECK("restart leg: the restarted host admits all 3 joins again "
              "(stale slots were not carried over)", acc2.size() >= 3);

        std::set<u32> ids2;
        ids2.insert(b1.localId());
        ids2.insert(b2.localId());
        ids2.insert(b3.localId());
        CHECK("restart leg: session-2 joins got 3 distinct PlayerIds", ids2.size() == 3);
        bool inRange2 = true;
        for (std::set<u32>::const_iterator it = ids2.begin(); it != ids2.end(); ++it)
            if (*it < 1 || *it >= MAX_PLAYERS) inRange2 = false;
        CHECK("restart leg: session-2 PlayerIds are all within {1,2,3} - no id 0 "
              "(rejected/never-assigned) among them", inRange2);

        // A rejected client never reaches gameplay, so it would report a
        // disconnect instead of a slot. Zero leaves = nobody was turned away.
        std::deque<u32> b1Leaves, b2Leaves, b3Leaves;
        drainLeavesInto(b1In, b1Leaves);
        drainLeavesInto(b2In, b2Leaves);
        drainLeavesInto(b3In, b3Leaves);
        CHECK("restart leg: no session-2 join observed a rejection disconnect",
              b1Leaves.empty() && b2Leaves.empty() && b3Leaves.empty());

        b1.stop(); b2.stop(); b3.stop(); rsHost.stop();
    }

    std::printf("\nnettest: %d/%d checks passed - %s\n",
                g_total - g_failed, g_total, g_failed == 0 ? "PASS" : "FAIL");
    int rc = g_failed == 0 ? 0 : 1;
    DeleteCriticalSection(&g_logCs);
    return rc;
}
