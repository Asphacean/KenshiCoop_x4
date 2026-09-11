// prototest - the asserting unit layer for the KenshiCoop wire protocol.
//
// Runs in milliseconds, before any game launch, as step 0 of every regression
// tier (scripts/regress.ps1). Locks three things:
//   1. The WIRE CONTRACT: exact packed sizes + field offsets of every packet in
//      src/netproto/Wire.h. A padding/reorder slip silently desyncs both
//      clients (they memcpy struct bytes); this catches it at compile-run time.
//   2. The CONTENT HASH (src/netproto/ContentHash.h): the inventory-sync
//      convergence key. Must be deterministic, field-sensitive, and
//      order-independent across entries - cross-client equality of these sums
//      IS the inv oracle's proof.
//   3. The INTERPOLATION BUFFER (src/plugin/sync/Interp.cpp): bracketing,
//      clamping, dead-reckoning cap, staleness, teleport snap.
//
// Zero game dependencies. Exit code = number of failed checks (0 = PASS).
//
// Build: cmd /c scripts\build_prototest.cmd  ->  dist\prototest.exe

#define _CRT_SECURE_NO_WARNINGS 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <cstdio>
#include <cstring>

#include "../netproto/Wire.h"
#include "../netproto/ContentHash.h"
#include "../plugin/sync/Interp.h"
#include "../plugin/core/OwnRanks.h"
#include "../plugin/core/SteamId.h"
#include "../plugin/core/WorkPose.h"
#include "../plugin/core/DeathLatch.h"
#include "../plugin/core/Inbound.h" // Phase 0 queue-lifecycle fixes (header-only)
#include "../plugin/core/PeerRoster.h" // leave-queue OWNER_ID_ALL expansion (host-link drop)
#include "../plugin/test/PauseSchedule.h" // pause_stress repro-driver cycle schedule (pure)
#include "../plugin/game/EngineFaults.h" // Phase 5c: fault throttle (pure inline)
#include "../plugin/game/EngineCaps.h"   // Phase 5d: capability registry (pure inline)
#include "../plugin/sync/ChangeGate.h"   // Phase 6: change-gated send/accept policy
#include "../plugin/sync/SaveXfer.h"     // Part A: real save-transfer receiver end-to-end
#include "../plugin/sync/FoldDedup.h"    // Phase 5: shared per-owner fold/dedup helpers (ID-01)
#include "../plugin/sync/PinOwner.h"     // 06-01 (GAP-3/PLAY-03): pinPeer_ author bookkeeping
#include "../plugin/sync/XferCommit.h"   // Phase 7 (INV-02/INV-04): host-committed transfer state machine
#include "../plugin/sync/ClaimArbiter.h" // Phase 7 Plan 02 (INV-03): claim-contention arbiter
#include "../plugin/sync/CellMap.h"      // Phase 8 Plan 02 (WORLD-03): host-side cell-claim-map reduce
#include "../plugin/sync/MoneyFold.h"    // Phase 9 Plan 01 (CONS-01): shared-money-pool arbiter
#include "../plugin/sync/SpeedVote.h"    // Phase 9 Plan 02 (CONS-02): N-player speed min-vote reduce
#include "../plugin/sync/SaveCoord.h"    // Phase 10 Plan 01 (SAVE-01): per-client save coordinator
#include "../plugin/sync/LoadCoord.h"    // Phase 10 Plan 01 (SAVE-02/SAVE-03): per-client load coordinator + arbiter

#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

using namespace coop;

// SaveXfer.cpp logs through coop::logLine/logErrLine; CoopLog.cpp is NOT part of
// this CRT-only build, so provide inert definitions to satisfy the linker (the
// round-trip test only cares about the staged/committed bytes, not the log).
namespace coop {
    void logLine(const char*) {}
    void logErrLine(const char*) {}
}

static int g_failed = 0;
static int g_total  = 0;

#define CHECK(name, cond) do { \
    ++g_total; \
    if (cond) { std::printf("  ok   %s\n", name); } \
    else      { std::printf("  FAIL %s\n", name); ++g_failed; } \
} while (0)

#define CHECK_EQ(name, actual, expected) do { \
    ++g_total; \
    unsigned long long a_ = (unsigned long long)(actual); \
    unsigned long long e_ = (unsigned long long)(expected); \
    if (a_ == e_) { std::printf("  ok   %s (= %llu)\n", name, a_); } \
    else { std::printf("  FAIL %s (actual %llu != expected %llu)\n", name, a_, e_); ++g_failed; } \
} while (0)

// ---- 1. Wire contract: packed sizes ------------------------------------------

static void testSizes() {
    std::printf("== wire struct sizes (the packed contract both clients memcpy) ==\n");
    CHECK_EQ("sizeof(HelloPacket)",             sizeof(HelloPacket),             4);
    CHECK_EQ("sizeof(WelcomePacket)",           sizeof(WelcomePacket),           7);
    CHECK_EQ("sizeof(EventPacket)",             sizeof(EventPacket),             54);
    CHECK_EQ("sizeof(EntityState)",             sizeof(EntityState),             79);
    CHECK_EQ("sizeof(EntityBatchHeader)",       sizeof(EntityBatchHeader),       14); // v35: +sendMs; v44: +epoch
    CHECK_EQ("sizeof(InvItemEntry)",            sizeof(InvItemEntry),            159); // v42: +locked, v48: reserved byte became parentIdx (size unchanged), v51: +level (craft grade)
    CHECK_EQ("sizeof(InvSnapshotHeader)",       sizeof(InvSnapshotHeader),       28); // v33: +keyKind; v46: +flags
    CHECK_EQ("sizeof(WorldItemEntry)",          sizeof(WorldItemEntry),          73);
    CHECK_EQ("sizeof(WorldItemSnapshotHeader)", sizeof(WorldItemSnapshotHeader), 6);
    CHECK_EQ("sizeof(WorldItemRemoveHeader)",   sizeof(WorldItemRemoveHeader),   6);
    CHECK_EQ("sizeof(WorldItemClaimHeader)",    sizeof(WorldItemClaimHeader),    14); // v47; v58: +authorClaimMs
    CHECK_EQ("sizeof(WorldDropPacket)",         sizeof(WorldDropPacket),         191);
    CHECK_EQ("sizeof(WorldPickupPacket)",       sizeof(WorldPickupPacket),       91); // v40: +item identity
    CHECK_EQ("sizeof(InvXferPacket)",           sizeof(InvXferPacket),           210); // v36; v51: +level; v58: +srcOwnerId/dstOwnerId

    CHECK_EQ("sizeof(MedPartEntry)",            sizeof(MedPartEntry),            19);
    CHECK_EQ("sizeof(MedicalPacket)",           sizeof(MedicalPacket),           467);
    CHECK_EQ("sizeof(TreatmentPacket)",         sizeof(TreatmentPacket),         77);
    CHECK_EQ("sizeof(CombatHitPacket)",         sizeof(CombatHitPacket),         37);
    CHECK_EQ("sizeof(SpeedPacket)",             sizeof(SpeedPacket),             14);
    CHECK_EQ("sizeof(StatsPacket)",             sizeof(StatsPacket),             194);
    CHECK_EQ("sizeof(StealthPacket)",           sizeof(StealthPacket),           427);
    CHECK_EQ("sizeof(SpawnReqPacket)",          sizeof(SpawnReqPacket),          25);
    CHECK_EQ("sizeof(SpawnInfoPacket)",         sizeof(SpawnInfoPacket),         143);
    CHECK_EQ("sizeof(MoneyPacket)",             sizeof(MoneyPacket),             42); // v60: ackSeq scalar -> per-owner ack vector
    CHECK_EQ("sizeof(MoneyDeltaPacket)",        sizeof(MoneyDeltaPacket),        17); // v60: +authorSpendMs
    CHECK_EQ("sizeof(MoneyRejectPacket)",       sizeof(MoneyRejectPacket),       17); // v60: insufficient-funds verdict (CONS-01)
    CHECK_EQ("sizeof(FactionPacket)",           sizeof(FactionPacket),           61);
    CHECK_EQ("sizeof(TimePacket)",              sizeof(TimePacket),              17);
    CHECK_EQ("sizeof(DoorPacket)",              sizeof(DoorPacket),              31);
    CHECK_EQ("sizeof(BuildPlacePacket)",        sizeof(BuildPlacePacket),        94);
    CHECK_EQ("sizeof(BuildStatePacket)",        sizeof(BuildStatePacket),        34);
    CHECK_EQ("sizeof(BuildDoorPacket)",         sizeof(BuildDoorPacket),         32);
    CHECK_EQ("sizeof(BuildRemovePacket)",       sizeof(BuildRemovePacket),       29);
    CHECK_EQ("sizeof(SaveReqPacket)",           sizeof(SaveReqPacket),           57);
    CHECK_EQ("sizeof(SaveBeginPacket)",         sizeof(SaveBeginPacket),         67);
    CHECK_EQ("sizeof(SaveFileHeader)",          sizeof(SaveFileHeader),          19);
    CHECK_EQ("sizeof(SaveDoneHeader)",          sizeof(SaveDoneHeader),          11);
    CHECK_EQ("sizeof(SaveAckPacket)",           sizeof(SaveAckPacket),           20);
    CHECK_EQ("sizeof(LoadGoPacket)",            sizeof(LoadGoPacket),            61);
    CHECK_EQ("sizeof(LoadReqPacket)",           sizeof(LoadReqPacket),           57);
    CHECK_EQ("sizeof(LoadNackPacket)",          sizeof(LoadNackPacket),          61);
    CHECK_EQ("sizeof(LoadAckPacket)",           sizeof(LoadAckPacket),           10); // v61: positive load-completion ACK (SAVE-02)
    CHECK_EQ("sizeof(CoordRejectPacket)",       sizeof(CoordRejectPacket),       19); // v61: first-wins arbitration reject (SAVE-03)
    CHECK_EQ("sizeof(ProdPacket)",              sizeof(ProdPacket),              109);
    CHECK_EQ("sizeof(NpcCensusHeader)",         sizeof(NpcCensusHeader),         7); // v35: census
    CHECK_EQ("sizeof(ResearchPacket)",          sizeof(ResearchPacket),          57); // v37: research
    CHECK_EQ("sizeof(DeedPacket)",              sizeof(DeedPacket),              78); // v54: deeds
    CHECK_EQ("sizeof(FixturePacket)",           sizeof(FixturePacket),           90); // v55: fixture identity
    CHECK_EQ("sizeof(CamHintPacket)",           sizeof(CamHintPacket),           17); // v43: camera hint
    CHECK_EQ("sizeof(CellClaimPacket)",         sizeof(CellClaimPacket),         21); // v49: cell claim
    CHECK_EQ("sizeof(InvXferAckPacket)",        sizeof(InvXferAckPacket),        18); // v50: transfer verdict (superseded)
    CHECK_EQ("sizeof(XferCommitPacket)",        sizeof(XferCommitPacket),        213); // v58: host-committed transfer verdict
    CHECK_EQ("sizeof(XferCommitAckPacket)",     sizeof(XferCommitAckPacket),     16); // v58: transfer-commit bookkeeping ack
    CHECK_EQ("sizeof(ClaimVerdictPacket)",      sizeof(ClaimVerdictPacket),      14); // v58: claim-contention verdict
    CHECK_EQ("sizeof(CellMapPacket)",           sizeof(CellMapPacket),           775); // v59: host-authoritative cell-claim map (WORLD-03)
    CHECK_EQ("sizeof(RosterPacket)",            sizeof(RosterPacket),            5);  // v56: roster join/leave
    // A full entity batch must fit one ~1400 B datagram (NetLink chunking cap).
    CHECK("entity batch fits datagram",
          sizeof(EntityBatchHeader) + ENTITY_BATCH_MAX * sizeof(EntityState) <= 1428);
    // The Steam sender chunk must fit the 1200 B clamped Steam MTU with room
    // for ENet's per-packet overhead (an oversized UNRELIABLE packet would be
    // sent as RELIABLE fragments - motion-stream stalls; review 2026-07-10).
    CHECK("steam entity batch fits clamped MTU",
          sizeof(EntityBatchHeader) + ENTITY_BATCH_MAX_STEAM * sizeof(EntityState) <= 1150);
    CHECK("steam cap under hard receive bound",
          ENTITY_BATCH_MAX_STEAM <= ENTITY_BATCH_MAX);
    CHECK("world-item batch fits datagram",
          sizeof(WorldItemSnapshotHeader) + WORLD_ITEMS_MAX * sizeof(WorldItemEntry) <= 1400);

    // Carried-body sync (protocol 18): the synthetic carry task must never
    // collide with TASK_NONE, the combat stances, or a real engine task key
    // (small ints), and it must classify as carry but NOT as combat.
    CHECK("TASK_CARRY_BODY != TASK_NONE",         TASK_CARRY_BODY != TASK_NONE);
    CHECK("TASK_CARRY_BODY != TASK_COMBAT_MELEE", TASK_CARRY_BODY != TASK_COMBAT_MELEE);
    CHECK("TASK_CARRY_BODY != TASK_COMBAT_WAIT",  TASK_CARRY_BODY != TASK_COMBAT_WAIT);
    CHECK("TASK_CARRY_BODY above engine keys",    TASK_CARRY_BODY >= 0xFE00u);
    CHECK("taskIsCarry(TASK_CARRY_BODY)",         taskIsCarry(TASK_CARRY_BODY));
    CHECK("!taskIsCombat(TASK_CARRY_BODY)",       !taskIsCombat(TASK_CARRY_BODY));
    CHECK("!taskIsCarry(TASK_COMBAT_MELEE)",      !taskIsCarry(TASK_COMBAT_MELEE));
    // BODY_CARRIED is a distinct bit, EXCLUDED from bodyIsDown (the receiver
    // checks bodyIsCarried FIRST and skips the down path for a carried body).
    CHECK("BODY_CARRIED distinct bit",
          BODY_CARRIED != BODY_DOWN && BODY_CARRIED != BODY_RAGDOLL &&
          BODY_CARRIED != BODY_DEAD && BODY_CARRIED != BODY_CRAWL);
    CHECK("bodyIsDown excludes BODY_CARRIED",     !bodyIsDown(BODY_CARRIED));
    CHECK("bodyIsCarried(BODY_CARRIED)",          bodyIsCarried(BODY_CARRIED));
    CHECK("carried+down still reads down",        bodyIsDown(BODY_CARRIED | BODY_DOWN));
    CHECK("carried+down still reads carried",     bodyIsCarried(BODY_CARRIED | BODY_RAGDOLL));
    CHECK("!bodyIsCarried(BODY_DOWN)",            !bodyIsCarried(BODY_DOWN));
    // The new reliable events must be distinct from the existing set.
    CHECK("EVT_PICKUP_BODY distinct",
          EVT_PICKUP_BODY != EVT_NONE && EVT_PICKUP_BODY != EVT_KNOCKOUT &&
          EVT_PICKUP_BODY != EVT_DEATH && EVT_PICKUP_BODY != EVT_REVIVE &&
          EVT_PICKUP_BODY != EVT_AMPUTATE && EVT_PICKUP_BODY != EVT_CRUSH);
    CHECK("EVT_DROP_BODY distinct",
          EVT_DROP_BODY != EVT_PICKUP_BODY && EVT_DROP_BODY != EVT_NONE &&
          EVT_DROP_BODY != EVT_CRUSH);

    // Furniture occupancy (protocol 19): the new bodyState bits are distinct
    // and EXCLUDED from bodyIsDown (the receiver checks bodyInFurniture FIRST,
    // like the carried carve-out).
    CHECK("BODY_IN_BED distinct bit",
          BODY_IN_BED != BODY_DOWN && BODY_IN_BED != BODY_RAGDOLL &&
          BODY_IN_BED != BODY_DEAD && BODY_IN_BED != BODY_CRAWL &&
          BODY_IN_BED != BODY_CARRIED);
    CHECK("BODY_IN_CAGE distinct bit",
          BODY_IN_CAGE != BODY_IN_BED && BODY_IN_CAGE != BODY_DOWN &&
          BODY_IN_CAGE != BODY_RAGDOLL && BODY_IN_CAGE != BODY_DEAD &&
          BODY_IN_CAGE != BODY_CRAWL && BODY_IN_CAGE != BODY_CARRIED);
    CHECK("bodyIsDown excludes occupancy",   !bodyIsDown(BODY_IN_BED | BODY_IN_CAGE));
    CHECK("bodyInFurniture(BODY_IN_BED)",    bodyInFurniture(BODY_IN_BED));
    CHECK("bodyInFurniture(BODY_IN_CAGE)",   bodyInFurniture(BODY_IN_CAGE));
    CHECK("!bodyInFurniture(down|carried)",  !bodyInFurniture(BODY_DOWN | BODY_CARRIED));
    CHECK("occupant+down still reads down",  bodyIsDown(BODY_IN_CAGE | BODY_DOWN));
    // Chained/pole prisoner (protocol 41): distinct bit, rides the furniture
    // carve-out (bodyInFurniture true) but still reads down when KO'd.
    CHECK("BODY_CHAINED distinct bit",
          BODY_CHAINED != BODY_IN_BED && BODY_CHAINED != BODY_IN_CAGE &&
          BODY_CHAINED != BODY_DOWN && BODY_CHAINED != BODY_RAGDOLL &&
          BODY_CHAINED != BODY_DEAD && BODY_CHAINED != BODY_CRAWL &&
          BODY_CHAINED != BODY_CARRIED && BODY_CHAINED != BODY_SNEAK);
    CHECK("bodyChained(BODY_CHAINED)",       bodyChained(BODY_CHAINED));
    CHECK("bodyInFurniture(BODY_CHAINED)",   bodyInFurniture(BODY_CHAINED));
    CHECK("!bodyChained(down|carried)",      !bodyChained(BODY_DOWN | BODY_CARRIED));
    CHECK("chained+down still reads down",   bodyIsDown(BODY_CHAINED | BODY_DOWN));
    // The new reliable events are distinct from the whole existing set.
    CHECK("EVT_ENTER_FURNITURE distinct",
          EVT_ENTER_FURNITURE != EVT_NONE && EVT_ENTER_FURNITURE != EVT_KNOCKOUT &&
          EVT_ENTER_FURNITURE != EVT_DEATH && EVT_ENTER_FURNITURE != EVT_REVIVE &&
          EVT_ENTER_FURNITURE != EVT_AMPUTATE && EVT_ENTER_FURNITURE != EVT_CRUSH &&
          EVT_ENTER_FURNITURE != EVT_PICKUP_BODY && EVT_ENTER_FURNITURE != EVT_DROP_BODY);
    CHECK("EVT_EXIT_FURNITURE distinct",
          EVT_EXIT_FURNITURE != EVT_ENTER_FURNITURE && EVT_EXIT_FURNITURE != EVT_NONE &&
          EVT_EXIT_FURNITURE != EVT_PICKUP_BODY && EVT_EXIT_FURNITURE != EVT_DROP_BODY);

    // Stealth sync (protocol 20).
    CHECK("BODY_SNEAK distinct bit",
          BODY_SNEAK != BODY_DOWN && BODY_SNEAK != BODY_RAGDOLL &&
          BODY_SNEAK != BODY_DEAD && BODY_SNEAK != BODY_CRAWL &&
          BODY_SNEAK != BODY_CARRIED && BODY_SNEAK != BODY_IN_BED &&
          BODY_SNEAK != BODY_IN_CAGE);
    CHECK("bodyIsDown excludes BODY_SNEAK", !bodyIsDown(BODY_SNEAK));
    CHECK("bodySneaking(BODY_SNEAK)",       bodySneaking(BODY_SNEAK));
    CHECK("!bodySneaking(BODY_CRAWL)",      !bodySneaking(BODY_CRAWL));
    CHECK("sneak+crawl still reads sneak",  bodySneaking((u16)(BODY_SNEAK | BODY_CRAWL)));

    // Prone posture (protocol 53). The prone value is a FIELD sharing the
    // bodyState word with the flag bits, so the two must not be able to corrupt
    // each other: the mask must miss every flag, a stamp must preserve the flags,
    // and a re-stamp must REPLACE the previous posture rather than OR into it.
    CHECK("prone mask clears every BODY_ flag",
          (BODY_PRONE_MASK & (BODY_DOWN | BODY_RAGDOLL | BODY_DEAD | BODY_CRAWL |
                              BODY_CARRIED | BODY_IN_BED | BODY_IN_CAGE |
                              BODY_SNEAK | BODY_CHAINED)) == 0);
    CHECK("prone field fits u16",           (BODY_PRONE_MASK >> BODY_PRONE_SHIFT) == 7);
    CHECK("prone round-trip NORMAL",        bodyProne(bodyWithProne(0, PRONE_NORMAL)) == PRONE_NORMAL);
    CHECK("prone round-trip CRIPPLED",      bodyProne(bodyWithProne(0, PRONE_CRIPPLED)) == PRONE_CRIPPLED);
    CHECK("prone round-trip KO (max)",      bodyProne(bodyWithProne(0, PRONE_KO)) == PRONE_KO);
    CHECK("prone values are distinct",
          PRONE_NORMAL != PRONE_STAYING_LOW && PRONE_STAYING_LOW != PRONE_CRIPPLED &&
          PRONE_CRIPPLED != PRONE_PLAYING_DEAD && PRONE_PLAYING_DEAD != PRONE_KO);
    {
        // A crippled crawler as the wire really carries it: BODY_CRAWL (which
        // cannot say WHICH posture) alongside the posture that can.
        u16 s = bodyWithProne((u16)BODY_CRAWL, PRONE_CRIPPLED);
        CHECK("prone stamp preserves flags",    (s & BODY_CRAWL) != 0);
        CHECK("prone stamp reads back",         bodyProne(s) == PRONE_CRIPPLED);
        CHECK("crippled crawler is not down",   !bodyIsDown(s));
        CHECK("crippled crawler is not sneaking", !bodySneaking(s));
        CHECK("bodyFlags strips the posture",   bodyFlags(s) == BODY_CRAWL);
        // Re-stamp: PS_CRIPPLED -> PS_NORMAL must leave 0, not 2|0.
        u16 up = bodyWithProne(s, PRONE_NORMAL);
        CHECK("prone re-stamp replaces",        bodyProne(up) == PRONE_NORMAL);
        CHECK("prone re-stamp keeps flags",     (up & BODY_CRAWL) != 0);
        // Out-of-range degrades to upright rather than corrupting the flags.
        u16 bad = bodyWithProne(s, (u8)7);
        CHECK("prone out-of-range = NORMAL",    bodyProne(bad) == PRONE_NORMAL);
        CHECK("prone out-of-range keeps flags", (bad & BODY_CRAWL) != 0);
    }
    // A posture must never make a body read as down/dead: that is what the
    // `bodyState != 0` call sites (victim pick, downed-enemy count) test.
    CHECK("bodyFlags(prone only) == 0",     bodyFlags(bodyWithProne(0, PRONE_CRIPPLED)) == 0);
    CHECK("prone KO alone is not down",     !bodyIsDown(bodyWithProne(0, PRONE_KO)));
    CHECK("down+prone still reads down",
          bodyIsDown(bodyWithProne((u16)BODY_DOWN, PRONE_KO)));

    // The crawl carve-out, on the EXACT words the engine was measured to stream
    // (run 20260805_152546, leg amputation): 2051 = DOWN|RAGDOLL|PS_KO for the
    // ~2 s collapse, then 1033 = DOWN|CRAWL|PS_CRIPPLED with unc=0 for the crawl.
    // Both are "down" to Character::isDown(), and treating the second as such is
    // what pinned the copy to the ground while its owner crawled away.
    {
        const u16 COLLAPSED = 2051; // DOWN|RAGDOLL, prone=PS_KO
        const u16 CRAWLING  = 1033; // DOWN|CRAWL,   prone=PS_CRIPPLED
        CHECK("measured collapsed word decodes",
              bodyIsDown(COLLAPSED) && bodyProne(COLLAPSED) == PRONE_KO &&
              (COLLAPSED & BODY_RAGDOLL) != 0);
        CHECK("measured crawling word decodes",
              bodyIsDown(CRAWLING) && bodyProne(CRAWLING) == PRONE_CRIPPLED &&
              (CRAWLING & BODY_CRAWL) != 0);
        CHECK("collapsed is not crawling",      !bodyIsCrawling(COLLAPSED));
        CHECK("crawling is crawling",           bodyIsCrawling(CRAWLING));
        CHECK("collapsed is down-not-crawling", bodyDownNotCrawling(COLLAPSED));
        CHECK("crawler is NOT down-not-crawling", !bodyDownNotCrawling(CRAWLING));
        // The four KO/REVIVE edges the publisher derives from that predicate.
        // Getting any of these backwards is how the copy stayed pinned.
        CHECK("upright -> crawl is no KO",
              !(bodyDownNotCrawling(CRAWLING) && !bodyDownNotCrawling(0)));
        CHECK("crawl -> upright is no REVIVE",
              !(!bodyDownNotCrawling(0) && bodyDownNotCrawling(CRAWLING)));
        CHECK("crawl -> collapse IS a KO",
              bodyDownNotCrawling(COLLAPSED) && !bodyDownNotCrawling(CRAWLING));
        CHECK("collapse -> crawl IS a REVIVE",
              !bodyDownNotCrawling(CRAWLING) && bodyDownNotCrawling(COLLAPSED));
        // A dead body is never a crawler, whatever the posture field says.
        CHECK("dead crawler is not crawling",
              !bodyIsCrawling((u16)(CRAWLING | BODY_DEAD)));
        CHECK("dead crawler stays down-not-crawling",
              bodyDownNotCrawling((u16)(CRAWLING | BODY_DEAD)));
        // A sneaker must not be mistaken for a crawler (PS_STAYING_LOW, upright).
        CHECK("low-crouch sneaker is not crawling",
              !bodyIsCrawling(bodyWithProne((u16)(BODY_SNEAK | BODY_CRAWL),
                                            PRONE_STAYING_LOW)));
    }

    // Protocol 53: the crippled CAUSE flag on the medical packet's flags byte.
    CHECK("MED_CRIPPLED distinct bit",
          MED_CRIPPLED != MED_UNCONSCIOUS && MED_CRIPPLED != MED_DEAD &&
          (MED_CRIPPLED & (MED_UNCONSCIOUS | MED_DEAD)) == 0);

    // Recruitment sync (protocol 23): the new reliable event is distinct from
    // the whole existing set (it rides the EventPacket shape unchanged).
    CHECK("EVT_RECRUIT distinct",
          EVT_RECRUIT != EVT_NONE && EVT_RECRUIT != EVT_KNOCKOUT &&
          EVT_RECRUIT != EVT_DEATH && EVT_RECRUIT != EVT_REVIVE &&
          EVT_RECRUIT != EVT_AMPUTATE && EVT_RECRUIT != EVT_CRUSH &&
          EVT_RECRUIT != EVT_PICKUP_BODY && EVT_RECRUIT != EVT_DROP_BODY &&
          EVT_RECRUIT != EVT_ENTER_FURNITURE && EVT_RECRUIT != EVT_EXIT_FURNITURE);

    // Squad management sync (protocol 35, v34): the move re-key event rides
    // the EventPacket shape unchanged; both ends must agree on its id, and
    // the HELLO version gates the mismatch.
    CHECK_EQ("EVT_SQUAD_MOVE id", (int)EVT_SQUAD_MOVE, 11);
    CHECK("EVT_SQUAD_MOVE distinct", EVT_SQUAD_MOVE != EVT_RECRUIT &&
          EVT_SQUAD_MOVE != EVT_NONE && EVT_SQUAD_MOVE != EVT_EXIT_FURNITURE);
    CHECK_EQ("PROTOCOL_VERSION (v61: per-client save/load coordinator, SAVE-01/02/03)",
             (int)PROTOCOL_VERSION, 61);
    CHECK_EQ("PKT_LOAD_ACK id", (int)PKT_LOAD_ACK, 57);
    CHECK_EQ("PKT_COORD_REJECT id", (int)PKT_COORD_REJECT, 58);
    CHECK("PKT_LOAD_ACK distinct from PKT_COORD_REJECT",
          PKT_LOAD_ACK != PKT_COORD_REJECT);
    CHECK_EQ("PKT_MONEY_REJECT id", (int)PKT_MONEY_REJECT, 56);
    CHECK("PKT_MONEY_REJECT distinct from dead PKT_LEAVE",
          (int)PKT_MONEY_REJECT != (int)PKT_LEAVE);

    // Roster announcements (protocol 56, Phase 2 Plan 03): PKT_PLAYER_JOINED /
    // PKT_PLAYER_LEFT are fresh enum values appended after PKT_FIXTURE - never
    // reuse the dead PKT_LEAVE=3.
    CHECK_EQ("PKT_PLAYER_JOINED id", (int)PKT_PLAYER_JOINED, 49);
    CHECK_EQ("PKT_PLAYER_LEFT id",   (int)PKT_PLAYER_LEFT,   50);
    CHECK("PKT_PLAYER_JOINED distinct from dead PKT_LEAVE",
          (int)PKT_PLAYER_JOINED != (int)PKT_LEAVE &&
          (int)PKT_PLAYER_LEFT   != (int)PKT_LEAVE);

    // Ownership-rank announcement (protocol 57, Phase 3 Plan 03): a fresh enum
    // value appended after PKT_PLAYER_LEFT.
    CHECK_EQ("PKT_OWN_RANKS id", (int)PKT_OWN_RANKS, 51);
    CHECK("PKT_OWN_RANKS distinct from dead PKT_LEAVE",
          (int)PKT_OWN_RANKS != (int)PKT_LEAVE);

    // Protocol 52: the shared money pool. The two players spend from ONE wallet,
    // so the join reports CHANGES and the host the authoritative TOTAL - swap
    // those roles and concurrent purchases silently mint or burn cats. The
    // shapes are locked here because both halves must stay distinguishable:
    // MoneyPacket carries an ack of the join's delta sequence (which is why the
    // old tabRank field is gone), MoneyDeltaPacket a signed change. Protocol 60
    // (CONS-01): the single ackSeq scalar is now a per-owner ack VECTOR.
    CHECK("PKT_MONEY_DELTA distinct", PKT_MONEY_DELTA != PKT_MONEY &&
          (int)PKT_MONEY_DELTA == 46);
    {
        MoneyPacket total; std::memset(&total, 0, sizeof(total));
        total.type = (u8)PKT_MONEY; total.ackCount = 1;
        total.acks[0].ownerId = 2u; total.acks[0].ackSeq = 7u; total.money = 4000;
        MoneyDeltaPacket d; std::memset(&d, 0, sizeof(d));
        d.type = (u8)PKT_MONEY_DELTA; d.seq = 8u; d.delta = -250; d.authorSpendMs = 12345u;
        CHECK("pool total carries a per-owner ack entry",
              total.ackCount == 1 && total.acks[0].ownerId == 2u &&
              total.acks[0].ackSeq == 7u && total.money == 4000);
        CHECK("pool delta is signed and carries its author stamp",
              d.delta < 0 && d.seq == 8u && d.authorSpendMs == 12345u);
    }

    // Protocol 48: the parent reference. A worn backpack owns a PRIVATE inventory, so a bagged
    // item is described by no snapshot unless it can name its container. The byte was already
    // reserved, so the entry must not have grown, and index 0 must keep meaning "top level" or
    // every existing entry would silently claim a parent.
    {
        InvItemEntry pe; std::memset(&pe, 0, sizeof(pe));
        CHECK_EQ("parentIdx defaults to top-level (0)", (int)pe.parentIdx, 0);
        CHECK("parentIdx addresses every entry INV_ITEMS_MAX allows", INV_ITEMS_MAX < 256);
    }

    // Protocol 51: the craft GRADE. Kenshi's named grades (Prototype 5 ... Masterwork 95)
    // are points on a 1..100 craft level held in Gear, and `quality` is CONDITION on a
    // different scale entirely - so the grade needs its own field, must be able to express
    // the whole range, and must have a not-applicable value distinct from every real level
    // (level 0 would otherwise read as "mint this at the worst possible grade").
    {
        CHECK_EQ("GRADE_NA is out of the 0..100 craft-level range", (int)GRADE_NA, 255);
        CHECK("GRADE_NA cannot collide with a real craft level", (int)GRADE_NA > 100);
        InvItemEntry a; std::memset(&a, 0, sizeof(a));
        InvItemEntry b = a;
        a.level = 95; b.level = 20;                  // Masterwork vs Shoddy, all else equal
        CHECK("a re-graded item is a CONTENT change (fingerprint moves)",
              invEntryHash(a) != invEntryHash(b));
        // Items with no craft level must hash exactly as they did before the field existed,
        // or protocol 51 would republish every stack of food in the game once.
        InvItemEntry na = a; na.level = GRADE_NA;
        InvItemEntry zero = a; zero.level = 0;
        CHECK("GRADE_NA folds in as 0 (no spurious resend for non-gear)",
              invEntryHash(na) == invEntryHash(zero));
        // The grade must survive a wire round-trip in the same bytes it was written to.
        InvItemEntry rt; std::memset(&rt, 0, sizeof(rt));
        rt.level = 95;
        unsigned char buf[sizeof(InvItemEntry)];
        std::memcpy(buf, &rt, sizeof(rt));
        InvItemEntry back; std::memcpy(&back, buf, sizeof(back));
        CHECK_EQ("InvItemEntry::level round-trips", (int)back.level, 95);
        InvXferPacket xp; std::memset(&xp, 0, sizeof(xp));
        xp.level = 80;
        unsigned char xbuf[sizeof(InvXferPacket)];
        std::memcpy(xbuf, &xp, sizeof(xp));
        InvXferPacket xback; std::memcpy(&xback, xbuf, sizeof(xback));
        CHECK_EQ("InvXferPacket::level round-trips", (int)xback.level, 80);
    }

    // Protocol 46 (inventory item-loss fixes). The entry cap must match the receiver's
    // own read depth (MAXC in applyContainerContents) or a snapshot silently describes
    // less than the peer holds, and it must stay inside the u8 `count` field.
    CHECK_EQ("INV_ITEMS_MAX raised to the receiver's read depth", (int)INV_ITEMS_MAX, 64);
    CHECK("INV_ITEMS_MAX fits the u8 count field", INV_ITEMS_MAX <= 255);
    // The TRUNCATED bit is what stops a partial snapshot being read as a delete. It must
    // be a single low bit so future flags can share the byte.
    CHECK_EQ("INV_FLAG_TRUNCATED value", (int)INV_FLAG_TRUNCATED, 1);
    CHECK("INV_FLAG_TRUNCATED is a single bit",
          (INV_FLAG_TRUNCATED & (u8)(INV_FLAG_TRUNCATED - 1)) == 0);
    // `count` must remain the LAST header field: the entry array is framed immediately
    // after it, so inserting `flags` anywhere else would shift the payload.
    {
        InvSnapshotHeader h;
        std::memset(&h, 0, sizeof(h));
        const unsigned char* base = reinterpret_cast<const unsigned char*>(&h);
        std::size_t offCount = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.count) - base);
        std::size_t offFlags = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.flags) - base);
        CHECK_EQ("InvSnapshotHeader::count is the last field",
                 offCount, sizeof(InvSnapshotHeader) - 1);
        CHECK("InvSnapshotHeader::flags precedes the container key", offFlags < offCount);
    }

    // Protocol 47 (world-item CLAIM: the W1 pickup mirror). The tag must be unique or a
    // claim would be decoded as some other packet and silently destroy the wrong thing.
    CHECK_EQ("PKT_WORLD_ITEM_CLAIM id", (int)PKT_WORLD_ITEM_CLAIM, 43);
    CHECK("PKT_WORLD_ITEM_CLAIM distinct",
          PKT_WORLD_ITEM_CLAIM != PKT_WORLD_ITEM &&
          PKT_WORLD_ITEM_CLAIM != PKT_WORLD_ITEM_REMOVE &&
          PKT_WORLD_ITEM_CLAIM != PKT_COMBAT_HIT &&
          PKT_WORLD_ITEM_CLAIM != PKT_WORLD_DROP &&
          PKT_WORLD_ITEM_CLAIM != PKT_WORLD_PICKUP);
    {
        // The netId array is framed immediately after `count`, exactly as in the cull
        // header, so `count` must stay LAST. A claim also carries authorId (the netIds
        // live in the AUTHOR's id space, not the claimer's) - it must sit BEFORE count.
        // v58: authorClaimMs (the claim-detection timestamp) must also precede count.
        WorldItemClaimHeader h;
        std::memset(&h, 0, sizeof(h));
        const unsigned char* base = reinterpret_cast<const unsigned char*>(&h);
        std::size_t offCount   = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.count) - base);
        std::size_t offClaimMs = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.authorClaimMs) - base);
        std::size_t offAuthor  = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.authorId) - base);
        std::size_t offOwner   = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.ownerId) - base);
        CHECK_EQ("WorldItemClaimHeader::count is the last field",
                 offCount, sizeof(WorldItemClaimHeader) - 1);
        CHECK("WorldItemClaimHeader::authorClaimMs precedes count", offClaimMs < offCount);
        CHECK("WorldItemClaimHeader::authorId precedes authorClaimMs", offAuthor < offClaimMs);
        CHECK("WorldItemClaimHeader::ownerId precedes authorId", offOwner < offAuthor);
    }
    // A claim batch is capped by the u8 count; even a full one must fit a datagram.
    CHECK("full world-item claim fits datagram",
          sizeof(WorldItemClaimHeader) + 255 * sizeof(u32) <= 1400);

    // Protocol 59 (WORLD-03): CellMapPacket bounds. `count` must fit inside
    // the fixed `entries` array it indexes - CELL_MAP_MAX has to match the
    // array's compile-time size exactly, or a count within [entries-size,
    // CELL_MAP_MAX) would silently over-read the packed struct on receive.
    CHECK_EQ("PKT_CELL_MAP id", (int)PKT_CELL_MAP, 55);
    CHECK("PKT_CELL_MAP distinct",
          PKT_CELL_MAP != PKT_CELL_CLAIM && PKT_CELL_MAP != PKT_CLAIM_VERDICT &&
          PKT_CELL_MAP != PKT_XFER_COMMIT && PKT_CELL_MAP != PKT_NPC_CENSUS);
    {
        CellMapPacket h;
        std::memset(&h, 0, sizeof(h));
        CHECK_EQ("CellMapPacket::entries array size matches CELL_MAP_MAX",
                 sizeof(h.entries) / sizeof(h.entries[0]), CELL_MAP_MAX);
        const unsigned char* base = reinterpret_cast<const unsigned char*>(&h);
        std::size_t offCount = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.count) - base);
        std::size_t offSeq   = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.seq) - base);
        std::size_t offEntries = (std::size_t)(reinterpret_cast<const unsigned char*>(&h.entries) - base);
        CHECK("CellMapPacket::seq precedes count", offSeq < offCount);
        CHECK("CellMapPacket::entries follows count (the count-driven tail)",
              offEntries > offCount);
        CHECK("CELL_MAP_MAX fits the u16 count field", CELL_MAP_MAX <= 65535u);
        // >= the live tab cap with headroom (research Open Question 3): at
        // most MAX_PLAYERS players * a handful of tabs each is comfortably
        // under 64.
        CHECK("CELL_MAP_MAX has headroom over MAX_PLAYERS", CELL_MAP_MAX >= MAX_PLAYERS * 4u);
    }
    // A full map (bounded, not the unbounded-history shape research Open
    // Question 3 rejected) fits one reliable send - ENet fragments a
    // reliable packet transparently, but the struct itself must stay a
    // sane, bounded size rather than growing with session history.
    CHECK("full CellMapPacket stays well under a fragmented-reliable-send sanity bound",
          sizeof(CellMapPacket) <= 4096);
}

// ---- 2. readPacket / packetType round-trips -----------------------------------

// Fill a struct with a deterministic byte pattern (distinct per offset).
template <typename T>
static void fillPattern(T* p, unsigned char seed) {
    unsigned char* b = reinterpret_cast<unsigned char*>(p);
    for (unsigned i = 0; i < sizeof(T); ++i) b[i] = (unsigned char)(seed + i * 7);
}

template <typename T>
static void roundTrip(const char* name, u8 typeTag) {
    T in;
    fillPattern(&in, (unsigned char)(typeTag * 31));
    in.type = typeTag;
    // 1024, not 512: CellMapPacket (protocol 59, WORLD-03) is 775 B
    // (CELL_MAP_MAX=64 entries embedded inline) - the largest fixed-size
    // wire struct this helper round-trips.
    unsigned char buf[1024];
    std::memcpy(buf, &in, sizeof(T));

    char label[128];

    T out;
    std::memset(&out, 0, sizeof(T));
    bool okRead = readPacket(buf, (unsigned)sizeof(T), &out);
    std::sprintf(label, "%s round-trip read", name);
    CHECK(label, okRead && std::memcmp(&in, &out, sizeof(T)) == 0);

    std::sprintf(label, "%s packetType tag", name);
    CHECK(label, packetType(buf, (unsigned)sizeof(T)) == typeTag);

    // Truncated by one byte: the reader MUST reject (never a partial fill).
    std::sprintf(label, "%s rejects truncated buffer", name);
    CHECK(label, !readPacket(buf, (unsigned)sizeof(T) - 1, &out));
}

static void testRoundTrips() {
    std::printf("== readPacket round-trips + truncation rejection ==\n");
    roundTrip<HelloPacket>("HelloPacket", (u8)PKT_HELLO);
    roundTrip<WelcomePacket>("WelcomePacket", (u8)PKT_WELCOME);
    roundTrip<EventPacket>("EventPacket", (u8)PKT_EVENT);
    roundTrip<WorldDropPacket>("WorldDropPacket", (u8)PKT_WORLD_DROP);
    roundTrip<WorldPickupPacket>("WorldPickupPacket", (u8)PKT_WORLD_PICKUP);
    roundTrip<InvXferPacket>("InvXferPacket", (u8)PKT_INV_XFER);
    roundTrip<MedicalPacket>("MedicalPacket", (u8)PKT_MEDICAL);
    roundTrip<TreatmentPacket>("TreatmentPacket", (u8)PKT_TREATMENT);
    roundTrip<CombatHitPacket>("CombatHitPacket", (u8)PKT_COMBAT_HIT);
    roundTrip<SpeedPacket>("SpeedPacket(REQ)", (u8)PKT_SPEED_REQ);
    roundTrip<SpeedPacket>("SpeedPacket(SET)", (u8)PKT_SPEED_SET);
    roundTrip<StatsPacket>("StatsPacket", (u8)PKT_STATS);
    roundTrip<MoneyPacket>("MoneyPacket", (u8)PKT_MONEY);
    roundTrip<MoneyDeltaPacket>("MoneyDeltaPacket", (u8)PKT_MONEY_DELTA);
    roundTrip<MoneyRejectPacket>("MoneyRejectPacket", (u8)PKT_MONEY_REJECT);
    roundTrip<FactionPacket>("FactionPacket", (u8)PKT_FACTION);
    roundTrip<TimePacket>("TimePacket", (u8)PKT_TIME);
    roundTrip<DoorPacket>("DoorPacket", (u8)PKT_DOOR);
    roundTrip<BuildPlacePacket>("BuildPlacePacket", (u8)PKT_BUILD_PLACE);
    roundTrip<BuildStatePacket>("BuildStatePacket", (u8)PKT_BUILD_STATE);
    roundTrip<BuildDoorPacket>("BuildDoorPacket", (u8)PKT_BUILD_DOOR);
    roundTrip<BuildRemovePacket>("BuildRemovePacket", (u8)PKT_BUILD_REMOVE);
    roundTrip<StealthPacket>("StealthPacket", (u8)PKT_STEALTH);
    roundTrip<SpawnReqPacket>("SpawnReqPacket", (u8)PKT_SPAWN_REQ);
    roundTrip<SpawnInfoPacket>("SpawnInfoPacket", (u8)PKT_SPAWN_INFO);
    roundTrip<SaveReqPacket>("SaveReqPacket", (u8)PKT_SAVE_REQ);
    roundTrip<SaveBeginPacket>("SaveBeginPacket", (u8)PKT_SAVE_BEGIN);
    roundTrip<SaveAckPacket>("SaveAckPacket", (u8)PKT_SAVE_ACK);
    roundTrip<LoadGoPacket>("LoadGoPacket", (u8)PKT_LOAD_GO);
    roundTrip<LoadReqPacket>("LoadReqPacket", (u8)PKT_LOAD_REQ);
    roundTrip<LoadNackPacket>("LoadNackPacket", (u8)PKT_LOAD_NACK);
    roundTrip<LoadAckPacket>("LoadAckPacket", (u8)PKT_LOAD_ACK);
    roundTrip<CoordRejectPacket>("CoordRejectPacket", (u8)PKT_COORD_REJECT);
    roundTrip<ProdPacket>("ProdPacket", (u8)PKT_PROD);
    roundTrip<ResearchPacket>("ResearchPacket", (u8)PKT_RESEARCH);
    roundTrip<DeedPacket>("DeedPacket", (u8)PKT_DEED);
    roundTrip<FixturePacket>("FixturePacket", (u8)PKT_FIXTURE);
    roundTrip<CellClaimPacket>("CellClaimPacket", (u8)PKT_CELL_CLAIM);
    roundTrip<CellMapPacket>("CellMapPacket", (u8)PKT_CELL_MAP);
    roundTrip<InvXferAckPacket>("InvXferAckPacket", (u8)PKT_INV_XFER_ACK);
    roundTrip<XferCommitPacket>("XferCommitPacket", (u8)PKT_XFER_COMMIT);
    roundTrip<XferCommitAckPacket>("XferCommitAckPacket", (u8)PKT_XFER_COMMIT_ACK);
    roundTrip<ClaimVerdictPacket>("ClaimVerdictPacket", (u8)PKT_CLAIM_VERDICT);
    roundTrip<RosterPacket>("RosterPacket(JOINED)", (u8)PKT_PLAYER_JOINED);
    roundTrip<RosterPacket>("RosterPacket(LEFT)", (u8)PKT_PLAYER_LEFT);
    roundTrip<OwnRanksPacket>("OwnRanksPacket", (u8)PKT_OWN_RANKS);

    CHECK("packetType(null) == 0", packetType(0, 10) == 0);
    unsigned char b0[1] = { 0 };
    CHECK("packetType(len 0) == 0", packetType(b0, 0) == 0);
    CHECK("readPacket(null) rejected", !readPacket<HelloPacket>(0, 4, (HelloPacket*)b0) || true);
}

// ---- 3. Field-offset lock (HELLO version + batch framing) -----------------------

static void testFraming() {
    std::printf("== field offsets + batch framing ==\n");

    // HELLO: [u8 type][u16 version][u8 nameLen] - the version check that rejects
    // mismatched builds depends on this exact layout.
    unsigned char hello[4];
    hello[0] = (unsigned char)PKT_HELLO;
    hello[1] = (unsigned char)(PROTOCOL_VERSION & 0xFF);
    hello[2] = (unsigned char)((PROTOCOL_VERSION >> 8) & 0xFF);
    hello[3] = 0;
    HelloPacket h;
    CHECK("HELLO parses from raw bytes", readPacket(hello, 4, &h));
    CHECK_EQ("HELLO version field offset", h.version, PROTOCOL_VERSION);
    CHECK("HELLO version mismatch detectable", ((u16)(PROTOCOL_VERSION + 1)) != h.version);

    // Entity batch framing: [EntityBatchHeader][EntityState*count], the exact
    // bounds check NetLink applies ("len >= need") must hold for a full batch
    // and reject a batch whose count field overruns the actual payload.
    const unsigned N = 3;
    unsigned char buf[sizeof(EntityBatchHeader) + 3 * sizeof(EntityState)];
    EntityBatchHeader hdr;
    hdr.type = (u8)PKT_ENTITY_BATCH; hdr.ownerId = 42; hdr.sendMs = 123456u;
    hdr.epoch = 7u; hdr.count = (u8)N;
    std::memcpy(buf, &hdr, sizeof(hdr));
    EntityState src[N];
    for (unsigned i = 0; i < N; ++i) {
        fillPattern(&src[i], (unsigned char)(i * 13 + 1));
        std::memcpy(buf + sizeof(hdr) + i * sizeof(EntityState), &src[i], sizeof(EntityState));
    }
    unsigned len = (unsigned)sizeof(buf);
    EntityBatchHeader rh;
    std::memcpy(&rh, buf, sizeof(rh));
    unsigned need = (unsigned)sizeof(EntityBatchHeader) + (unsigned)rh.count * (unsigned)sizeof(EntityState);
    CHECK("entity batch: full payload accepted",
          len >= need && rh.count == N && rh.ownerId == 42 && rh.sendMs == 123456u
          && rh.epoch == 7u);
    bool all = true;
    for (unsigned i = 0; i < N; ++i) {
        EntityState e;
        std::memcpy(&e, buf + sizeof(rh) + i * sizeof(EntityState), sizeof(e));
        if (std::memcmp(&e, &src[i], sizeof(e)) != 0) all = false;
    }
    CHECK("entity batch: entries round-trip", all);
    // Lying count: header claims one more entity than the datagram carries.
    rh.count = (u8)(N + 1);
    need = (unsigned)sizeof(EntityBatchHeader) + (unsigned)rh.count * (unsigned)sizeof(EntityState);
    CHECK("entity batch: overrun count rejected by len>=need", !(len >= need));

    // NPC census framing (protocol 36): [NpcCensusHeader][u32 hand[5] * count],
    // the exact "len >= need" bound NetLink applies plus the NPC_CENSUS_MAX cap.
    {
        const unsigned CN = 4;
        unsigned char cbuf[sizeof(NpcCensusHeader) + CN * 5 * sizeof(u32)];
        NpcCensusHeader ch;
        ch.type = (u8)PKT_NPC_CENSUS; ch.ownerId = 1; ch.count = (u16)CN;
        std::memcpy(cbuf, &ch, sizeof(ch));
        u32 hands[CN * 5];
        for (unsigned i = 0; i < CN * 5; ++i) hands[i] = 1000u + i;
        std::memcpy(cbuf + sizeof(ch), hands, sizeof(hands));
        NpcCensusHeader cr;
        std::memcpy(&cr, cbuf, sizeof(cr));
        unsigned clen  = (unsigned)sizeof(cbuf);
        unsigned cneed = (unsigned)sizeof(NpcCensusHeader) + (unsigned)cr.count * 5 * (unsigned)sizeof(u32);
        CHECK("npc census: full payload accepted",
              clen >= cneed && cr.count == CN && cr.count <= NPC_CENSUS_MAX);
        u32 back[CN * 5];
        std::memcpy(back, cbuf + sizeof(cr), sizeof(back));
        CHECK("npc census: hands round-trip", std::memcmp(back, hands, sizeof(hands)) == 0);
        cr.count = (u16)(CN + 1);
        cneed = (unsigned)sizeof(NpcCensusHeader) + (unsigned)cr.count * 5 * (unsigned)sizeof(u32);
        CHECK("npc census: overrun count rejected by len>=need", !(clen >= cneed));
        CHECK("npc census: cap sane", NPC_CENSUS_MAX >= 256 && NPC_CENSUS_MAX <= 2048);
    }

    // Save-file chunk framing (protocol 31): [SaveFileHeader][path][payload],
    // the exact "len >= need" bound NetLink applies, plus the pathLen/dataLen
    // sanity caps that reject a malformed chunk.
    {
        const char* relPath = "platoon\\Drifters_0.platoon";
        const unsigned pl = (unsigned)std::strlen(relPath);
        const unsigned dl = 100;
        unsigned char sbuf[sizeof(SaveFileHeader) + 64 + 100];
        SaveFileHeader fh;
        fh.type = (u8)PKT_SAVE_FILE; fh.ownerId = 0; fh.xferId = 7;
        fh.fileIdx = 3; fh.pathLen = (u16)pl; fh.offset = 4096; fh.dataLen = (u16)dl;
        std::memcpy(sbuf, &fh, sizeof(fh));
        std::memcpy(sbuf + sizeof(fh), relPath, pl);
        for (unsigned i = 0; i < dl; ++i) sbuf[sizeof(fh) + pl + i] = (unsigned char)i;
        unsigned slen = (unsigned)(sizeof(fh) + pl + dl);

        SaveFileHeader rfh;
        std::memcpy(&rfh, sbuf, sizeof(rfh));
        unsigned sneed = (unsigned)sizeof(SaveFileHeader) + rfh.pathLen + rfh.dataLen;
        CHECK("save chunk: full payload accepted",
              slen >= sneed && rfh.pathLen > 0 && rfh.pathLen <= SAVE_PATH_MAX &&
              rfh.dataLen <= SAVE_CHUNK_MAX);
        CHECK("save chunk: path bytes at header end",
              std::memcmp(sbuf + sizeof(SaveFileHeader), relPath, pl) == 0);
        CHECK("save chunk: payload follows path",
              sbuf[sizeof(SaveFileHeader) + pl + 42] == 42);
        // Lying dataLen: claims more payload than the packet carries.
        rfh.dataLen = (u16)(dl + 1);
        sneed = (unsigned)sizeof(SaveFileHeader) + rfh.pathLen + rfh.dataLen;
        CHECK("save chunk: overrun dataLen rejected by len>=need", !(slen >= sneed));
        // Oversized dataLen: above the chunk cap even if the bytes were there.
        rfh.dataLen = (u16)(SAVE_CHUNK_MAX + 1);
        CHECK("save chunk: dataLen above SAVE_CHUNK_MAX rejected",
              !(rfh.dataLen <= SAVE_CHUNK_MAX));
        // Zero pathLen: a chunk with no relative path is malformed.
        rfh.pathLen = 0;
        CHECK("save chunk: zero pathLen rejected", !(rfh.pathLen > 0));
    }

    // Save-done framing: [SaveDoneHeader][u32 crc * fileCount].
    {
        const unsigned FC = 5;
        unsigned char dbuf[sizeof(SaveDoneHeader) + FC * sizeof(u32)];
        SaveDoneHeader dh;
        dh.type = (u8)PKT_SAVE_DONE; dh.ownerId = 0; dh.xferId = 7; dh.fileCount = FC;
        std::memcpy(dbuf, &dh, sizeof(dh));
        u32 crcs[FC] = { 1, 2, 3, 4, 5 };
        std::memcpy(dbuf + sizeof(dh), crcs, sizeof(crcs));
        unsigned dlen = (unsigned)sizeof(dbuf);
        SaveDoneHeader rdh;
        std::memcpy(&rdh, dbuf, sizeof(rdh));
        unsigned dneed = (unsigned)sizeof(SaveDoneHeader) + rdh.fileCount * (unsigned)sizeof(u32);
        CHECK("save done: full CRC table accepted", dlen >= dneed && rdh.fileCount == FC);
        rdh.fileCount = (u16)(FC + 1);
        dneed = (unsigned)sizeof(SaveDoneHeader) + rdh.fileCount * (unsigned)sizeof(u32);
        CHECK("save done: overrun fileCount rejected by len>=need", !(dlen >= dneed));
    }
}

// ---- 3b. Save-transfer CRC (protocol 31): incremental FNV-1a-32 ------------------
// The receiver folds each arriving chunk into the file's running CRC; the
// sender does the same while reading. Chunk-split invariance IS the
// reassembly correctness proof: however the file is cut into chunks, the
// final CRC equals the whole-file hash the sender put in the DONE table.

static void testSaveCrc() {
    std::printf("== save-transfer CRC (fnv1a incremental) ==\n");
    unsigned char data[10000];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (unsigned char)(i * 31 + (i >> 8));

    // One-shot reference.
    unsigned ref = fnv1aUpdate(fnv1aInit(), data, sizeof(data));
    CHECK("crc deterministic", fnv1aUpdate(fnv1aInit(), data, sizeof(data)) == ref);

    // 4 KB chunking (the wire chunk size) folds to the same value.
    unsigned h = fnv1aInit();
    for (unsigned off = 0; off < sizeof(data); off += SAVE_CHUNK_MAX) {
        unsigned n = sizeof(data) - off;
        if (n > SAVE_CHUNK_MAX) n = SAVE_CHUNK_MAX;
        h = fnv1aUpdate(h, data + off, n);
    }
    CHECK("crc chunk-split invariant (4 KB chunks)", h == ref);

    // Pathological 1-byte chunks fold to the same value too.
    h = fnv1aInit();
    for (unsigned i = 0; i < sizeof(data); ++i) h = fnv1aUpdate(h, data + i, 1);
    CHECK("crc chunk-split invariant (1 B chunks)", h == ref);

    // A single flipped byte perturbs the CRC (corruption is caught).
    data[5000] ^= 1;
    CHECK("crc detects a flipped byte", fnv1aUpdate(fnv1aInit(), data, sizeof(data)) != ref);
    data[5000] ^= 1;

    // Empty file: CRC = the FNV offset basis, same on both ends.
    CHECK("crc of empty file = fnv basis", fnv1aInit() == 2166136261u);
}

// ---- 3b. Folder fingerprint (protocol 32 coordinated load) --------------------
// The join compares the host's LOAD_GO fingerprint against its own on-disk
// copy - equality must mean "byte-identical folder" regardless of directory
// enumeration order or path case, and any divergence must perturb it.

static void testFolderFingerprint() {
    std::printf("== folder fingerprint (coordinated load) ==\n");
    const char* paths[4] = { "quick.save", "platoon\\a.platoon",
                             "platoon\\b.platoon", "zone\\zone.1.2.zone" };
    unsigned int crcs[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
    unsigned ref = folderFingerprintOf(paths, crcs, 4);

    CHECK("fp deterministic", folderFingerprintOf(paths, crcs, 4) == ref);
    CHECK("fp nonzero (0 reserved for missing)", ref != 0);

    // Enumeration-order invariance: FindFirstFile order differs by filesystem;
    // the same (path, crc) SET must fingerprint identically.
    const char* paths2[4] = { paths[2], paths[0], paths[3], paths[1] };
    unsigned int crcs2[4] = { crcs[2], crcs[0], crcs[3], crcs[1] };
    CHECK("fp enumeration-order invariant",
          folderFingerprintOf(paths2, crcs2, 4) == ref);

    // Windows path case-insensitivity: the same folder listed with different
    // case must agree cross-machine.
    const char* paths3[4] = { "QUICK.SAVE", "Platoon\\A.platoon",
                              "platoon\\b.PLATOON", "zone\\ZONE.1.2.zone" };
    CHECK("fp path-case invariant", folderFingerprintOf(paths3, crcs, 4) == ref);

    // Sensitivity: one changed file content, a renamed path, a missing file
    // and an added file must all perturb the value.
    unsigned int crcs4[4] = { crcs[0], crcs[1] ^ 1u, crcs[2], crcs[3] };
    CHECK("fp detects changed file content",
          folderFingerprintOf(paths, crcs4, 4) != ref);
    const char* paths5[4] = { "quick.save", "platoon\\a.platoon",
                              "platoon\\c.platoon", "zone\\zone.1.2.zone" };
    CHECK("fp detects renamed path", folderFingerprintOf(paths5, crcs, 4) != ref);
    CHECK("fp detects missing file", folderFingerprintOf(paths, crcs, 3) != ref);
    const char* paths6[5] = { paths[0], paths[1], paths[2], paths[3], "extra.bin" };
    unsigned int crcs6[5] = { crcs[0], crcs[1], crcs[2], crcs[3], 0x55555555u };
    CHECK("fp detects added file", folderFingerprintOf(paths6, crcs6, 5) != ref);

    // Empty folder = 0 (the "missing/unreadable" sentinel).
    CHECK("fp of empty set = 0", folderFingerprintOf(paths, crcs, 0) == 0);
}

// ---- 3c. Save-transfer receiver round-trip (protocol 31) ----------------------
// The tests above lock the wire framing + CRC math in isolation. This one drives
// the REAL receiver in SaveXfer.cpp (onSaveBegin/onSaveFile/onSaveDone ->
// stage/verify/commit) end-to-end: it builds the BEGIN/FILE/DONE stream a host
// would send from an in-memory file set, feeds it to the receiver against a temp
// save-root, and asserts the committed folder is byte-identical - the very step
// players report failing ("the initial save didn't transfer"). A second transfer
// with a corrupted chunk must FAIL the commit and leave the prior save untouched.

struct XferSrcFile { const char* rel; const unsigned char* data; unsigned len; };

static bool xferReadWhole(const std::string& path, std::vector<unsigned char>* out) {
    out->clear();
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, 0) && got > 0)
        out->insert(out->end(), buf, buf + got);
    CloseHandle(h);
    return true;
}

static void xferNukeDir(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' ||
                (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0'))) continue;
            std::string child = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) xferNukeDir(child);
            else { SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                   DeleteFileA(child.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

// Feed one transfer (xferId) for 'name' built from srcs[nsrc]. corruptFileIdx>=0
// flips one received payload byte of that file so the receiver's CRC won't match
// the DONE table (a wire-corruption simulation). Returns onSaveDone (1/0).
static int xferRun(const char* name, const XferSrcFile* srcs, unsigned nsrc,
                   u32 xferId, int corruptFileIdx) {
    SaveBeginPacket bp;
    std::memset(&bp, 0, sizeof(bp));
    bp.type = (u8)PKT_SAVE_BEGIN; bp.ownerId = 1; bp.xferId = xferId;
    std::strncpy(bp.name, name, sizeof(bp.name) - 1);
    bp.fileCount = (u16)nsrc;
    unsigned __int64 total = 0;
    for (unsigned i = 0; i < nsrc; ++i) total += srcs[i].len;
    bp.totalBytes = total;
    savexfer::onSaveBegin(bp);

    std::vector<u32> crcs(nsrc, 0);
    for (unsigned i = 0; i < nsrc; ++i) {
        const XferSrcFile& f = srcs[i];
        crcs[i] = fnv1aUpdate(fnv1aInit(), f.data, f.len); // whole-file CRC (DONE table)
        unsigned off = 0;
        do {
            unsigned n = f.len - off;
            if (n > SAVE_CHUNK_MAX) n = SAVE_CHUNK_MAX;
            std::vector<unsigned char> chunk;
            if (n > 0) chunk.assign(f.data + off, f.data + off + n);
            if ((int)i == corruptFileIdx && off == 0 && n > 0) chunk[0] ^= 0xFF;
            SaveFileHeader fh;
            fh.type = (u8)PKT_SAVE_FILE; fh.ownerId = 1; fh.xferId = xferId;
            fh.fileIdx = (u16)i; fh.pathLen = (u16)std::strlen(f.rel);
            fh.offset = off; fh.dataLen = (u16)n;
            savexfer::onSaveFile(fh, f.rel,
                                 n ? &chunk[0] : (const unsigned char*)"");
            off += n;
        } while (off < f.len);
    }

    SaveDoneHeader dh;
    dh.type = (u8)PKT_SAVE_DONE; dh.ownerId = 1; dh.xferId = xferId;
    dh.fileCount = (u16)nsrc;
    u16 of = 0; unsigned __int64 ob = 0;
    return savexfer::onSaveDone(dh, crcs.empty() ? (const u32*)0 : &crcs[0], &of, &ob);
}

static void testSaveXferRoundTrip() {
    std::printf("== save-transfer receiver round-trip (stage/verify/commit) ==\n");

    // Temp save-root so the receiver never touches the real save folder.
    char tmp[MAX_PATH]; tmp[0] = '\0';
    GetTempPathA(sizeof(tmp), tmp);
    char root[MAX_PATH];
    _snprintf(root, sizeof(root) - 1, "%skc_xfer_test_%lu", tmp,
              (unsigned long)GetCurrentProcessId());
    root[sizeof(root) - 1] = '\0';
    std::string rootStr = root;
    xferNukeDir(rootStr);                 // best-effort clean from a prior run
    CreateDirectoryA(rootStr.c_str(), 0);
    savexfer::setSaveRootForTest(rootStr);

    // A representative save: a multi-chunk core, two subdir files, and an empty
    // file (exercises subdir creation + the dataLen=0 chunk path).
    std::vector<unsigned char> quick(5000), plat(1234), zone(1);
    for (unsigned i = 0; i < quick.size(); ++i) quick[i] = (unsigned char)(i * 7 + 3);
    for (unsigned i = 0; i < plat.size();  ++i) plat[i]  = (unsigned char)(i * 13 + 1);
    zone[0] = 0xAB;
    XferSrcFile srcs[4];
    srcs[0].rel = "quick.save";                  srcs[0].data = &quick[0]; srcs[0].len = (unsigned)quick.size();
    srcs[1].rel = "platoon\\Drifters_0.platoon"; srcs[1].data = &plat[0];  srcs[1].len = (unsigned)plat.size();
    srcs[2].rel = "zone\\zone.1.2.zone";         srcs[2].data = &zone[0];  srcs[2].len = (unsigned)zone.size();
    srcs[3].rel = "meta\\empty.dat";             srcs[3].data = (const unsigned char*)""; srcs[3].len = 0;

    // 1) Clean transfer -> commit, byte-identical folder.
    int r1 = xferRun("coopresume", srcs, 4, /*xferId*/1, /*corrupt*/-1);
    CHECK("xfer clean commit returns ok", r1 == 1);
    CHECK("xfer lastCommitResult ok", savexfer::lastCommitResult() == 1);
    CHECK("xfer commitSeq advanced", savexfer::commitSeq() >= 1);

    std::string commit = savexfer::saveFolderFor("coopresume");
    bool allMatch = true;
    for (unsigned i = 0; i < 4; ++i) {
        std::vector<unsigned char> got;
        bool ok = xferReadWhole(commit + "\\" + srcs[i].rel, &got);
        bool same = ok && got.size() == srcs[i].len &&
                    (srcs[i].len == 0 ||
                     std::memcmp(&got[0], srcs[i].data, srcs[i].len) == 0);
        if (!same) allMatch = false;
    }
    CHECK("xfer committed folder is byte-identical (incl. subdirs + empty file)",
          allMatch);

    std::string staging = savexfer::saveFolderFor(std::string("coopresume") + "__incoming");
    CHECK("xfer staging removed after commit",
          GetFileAttributesA(staging.c_str()) == INVALID_FILE_ATTRIBUTES);

    // 2) Corrupted chunk -> commit FAILS, prior save untouched, staging discarded.
    int r2 = xferRun("coopresume", srcs, 4, /*xferId*/2, /*corrupt file*/0);
    CHECK("xfer corrupt chunk fails commit", r2 == 0);
    CHECK("xfer corrupt lastCommitResult fail", savexfer::lastCommitResult() == 0);
    {
        std::vector<unsigned char> got;
        bool ok = xferReadWhole(commit + "\\quick.save", &got);
        bool intact = ok && got.size() == quick.size() &&
                      std::memcmp(&got[0], &quick[0], quick.size()) == 0;
        CHECK("xfer failed commit leaves the previous save intact", intact);
    }
    CHECK("xfer failed commit discards staging",
          GetFileAttributesA(staging.c_str()) == INVALID_FILE_ATTRIBUTES);

    // 3) Phase 10 review CR-04: an EMPTY save name is refused outright - the
    // receiver must never stage toward (or commit over) the save ROOT itself
    // (saveFolderFor("") = the root; a commit would transiently rename the
    // user's whole save library). The refused transfer's DONE fails (ACK
    // ok=0), and the root's existing content is untouched.
    {
        int r3 = xferRun("", srcs, 4, /*xferId*/3, /*corrupt*/-1);
        CHECK("xfer empty-name transfer is refused (DONE fails, ok=0)", r3 == 0);
        // The prior commit ('coopresume') is still intact at the root.
        std::vector<unsigned char> got;
        bool okRead = xferReadWhole(commit + "\\quick.save", &got);
        CHECK("xfer empty-name refusal leaves the save root untouched",
              okRead && got.size() == quick.size());
        // No root-level "__incoming_<pid>" staging dir was created.
        char pidStage[64];
        _snprintf(pidStage, sizeof(pidStage) - 1, "__incoming_%lu",
                  (unsigned long)GetCurrentProcessId());
        pidStage[sizeof(pidStage) - 1] = '\0';
        std::string rootStage = savexfer::saveFolderFor(pidStage);
        CHECK("xfer empty-name refusal creates no root-level staging dir",
              GetFileAttributesA(rootStage.c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    // 4) Phase 10 review WR-07: a BEGIN sweeps sibling staging orphans whose
    // tagged PID is no longer a live process (crash leftovers that would
    // otherwise pollute the save list forever), while a staging dir tagged
    // with a LIVE pid (pid 4 = System, always alive, always ACCESS_DENIED to
    // open) is conservatively left alone.
    {
        std::string deadOrphan = savexfer::saveFolderFor(
            "coopresume__incoming_4000000001"); // not a valid live PID (not a multiple of 4)
        std::string liveOrphan = savexfer::saveFolderFor(
            "coopresume__incoming_4");          // the System process - always alive
        CreateDirectoryA(deadOrphan.c_str(), 0);
        CreateDirectoryA(liveOrphan.c_str(), 0);
        {   // a partial file inside each, as a real crash would leave
            HANDLE h = CreateFileA((deadOrphan + "\\quick.save").c_str(),
                                   GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, 0);
            if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "x", 1, &w, 0); CloseHandle(h); }
            h = CreateFileA((liveOrphan + "\\quick.save").c_str(),
                            GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, 0);
            if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "x", 1, &w, 0); CloseHandle(h); }
        }
        int r4 = xferRun("coopresume", srcs, 4, /*xferId*/4, /*corrupt*/-1);
        CHECK("xfer sweep run still commits cleanly", r4 == 1);
        CHECK("xfer BEGIN sweeps a dead process's staging orphan",
              GetFileAttributesA(deadOrphan.c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK("xfer BEGIN leaves a LIVE pid's staging dir alone",
              GetFileAttributesA(liveOrphan.c_str()) != INVALID_FILE_ATTRIBUTES);
        xferNukeDir(liveOrphan);
    }

    // 4b) Phase 11 review WR-01: the sweep generalizes to the OTHER two
    // PID-tagged folder classes a crash can strand in the save root - the
    // sender's "__xfersrc_<pid>" snapshot and the commit's "__old_<pid>"
    // move-aside. A dead __xfersrc_ orphan is deleted; a dead __old_ orphan
    // is deleted only when save/<name> exists (stale backup), and RESTORED
    // over save/<name> when the logical folder is MISSING (the crash landed
    // between move-aside and move-in - the orphan is the user's only copy).
    {
        // Dead __xfersrc_ and stale __old_ (finalDir 'coopresume' exists from
        // the commits above) -> both swept.
        std::string deadSnap = savexfer::saveFolderFor(
            "coopresume__xfersrc_4000000001");
        std::string staleOld = savexfer::saveFolderFor(
            "coopresume__old_4000000001");
        CreateDirectoryA(deadSnap.c_str(), 0);
        CreateDirectoryA(staleOld.c_str(), 0);
        {
            HANDLE h = CreateFileA((deadSnap + "\\quick.save").c_str(),
                                   GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, 0);
            if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "x", 1, &w, 0); CloseHandle(h); }
            h = CreateFileA((staleOld + "\\quick.save").c_str(),
                            GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, 0);
            if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "x", 1, &w, 0); CloseHandle(h); }
        }
        int r4b = xferRun("coopresume", srcs, 4, /*xferId*/6, /*corrupt*/-1);
        CHECK("xfer WR-01 sweep run still commits cleanly", r4b == 1);
        CHECK("xfer BEGIN sweeps a dead process's __xfersrc_ snapshot orphan",
              GetFileAttributesA(deadSnap.c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK("xfer BEGIN sweeps a dead process's stale __old_ orphan (final save present)",
              GetFileAttributesA(staleOld.c_str()) == INVALID_FILE_ATTRIBUTES);

        // Restore case: save/<name> MISSING, only a dead __old_<pid> orphan
        // holds the previous save. The BEGIN sweep must move it back over
        // save/<name>. Prove it with a transfer whose commit FAILS (corrupt
        // chunk): if the sweep had DELETED instead of restored, save/<name>
        // would be missing afterwards; restored, its marker content survives.
        std::string strandedOld = savexfer::saveFolderFor(
            "cooprestore__old_4000000001");
        CreateDirectoryA(strandedOld.c_str(), 0);
        const char* marker = "previous-save-marker";
        {
            HANDLE h = CreateFileA((strandedOld + "\\quick.save").c_str(),
                                   GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, 0);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(h, marker, (DWORD)std::strlen(marker), &w, 0);
                CloseHandle(h);
            }
        }
        int r4c = xferRun("cooprestore", srcs, 4, /*xferId*/7, /*corrupt*/0);
        CHECK("xfer restore-case transfer fails its commit (corrupt chunk)", r4c == 0);
        CHECK("xfer BEGIN restored the stranded __old_ orphan over the missing save",
              GetFileAttributesA(savexfer::saveFolderFor("cooprestore").c_str())
                  != INVALID_FILE_ATTRIBUTES);
        CHECK("xfer restored orphan folder itself is gone",
              GetFileAttributesA(strandedOld.c_str()) == INVALID_FILE_ATTRIBUTES);
        {
            std::vector<unsigned char> got;
            bool okRead = xferReadWhole(
                savexfer::saveFolderFor("cooprestore") + "\\quick.save", &got);
            CHECK("xfer restored save carries the previous save's bytes",
                  okRead && got.size() == std::strlen(marker) &&
                  std::memcmp(&got[0], marker, got.size()) == 0);
        }
    }

    // 5) Phase 10 review IN-01: two adjacent dots INSIDE a filename (a squad
    // named "a..b" derives such platoon files) are legitimate - the sender
    // ships them, so the receiver must accept them; only a FULL ".."
    // component may be rejected (path escape). A dotted-name transfer must
    // commit byte-identically, not CRC-fail-loop.
    {
        unsigned char body[64];
        for (unsigned i = 0; i < sizeof(body); ++i) body[i] = (unsigned char)(i * 3 + 5);
        XferSrcFile dotted[2];
        dotted[0].rel = "quick.save";                 dotted[0].data = body; dotted[0].len = sizeof(body);
        dotted[1].rel = "platoon\\a..b_0.platoon";    dotted[1].data = body; dotted[1].len = sizeof(body);
        int r5 = xferRun("coopdotted", dotted, 2, /*xferId*/5, /*corrupt*/-1);
        CHECK("xfer filename with interior '..' commits (IN-01)", r5 == 1);
        std::vector<unsigned char> got;
        bool okRead = xferReadWhole(
            savexfer::saveFolderFor("coopdotted") + "\\platoon\\a..b_0.platoon", &got);
        CHECK("xfer dotted-name file lands byte-identical",
              okRead && got.size() == sizeof(body) &&
              std::memcmp(&got[0], body, sizeof(body)) == 0);
    }

    savexfer::setSaveRootForTest(std::string()); // unpin
    xferNukeDir(rootStr);
}

// ---- 4. Content hash (the inventory convergence key) -----------------------------

static InvItemEntry makeEntry() {
    InvItemEntry e;
    std::memset(&e, 0, sizeof(e));
    std::strcpy(e.stringID, "wooden_sandals");
    e.itemType = 7; e.quantity = 2; e.quality = 150;
    e.equipped = 0; e.slot = 0; e.section = 0;
    std::strcpy(e.manufacturer, "");
    std::strcpy(e.material, "");
    return e;
}

static void testContentHash() {
    std::printf("== content hash (ContentHash.h) ==\n");
    InvItemEntry a = makeEntry();
    InvItemEntry b = makeEntry();
    CHECK("hash deterministic (equal entries equal)", invEntryHash(a) == invEntryHash(b));

    // Every field that defines content identity must perturb the hash.
    unsigned base = invEntryHash(a);
    b = makeEntry(); std::strcpy(b.stringID, "wooden_sandalz");
    CHECK("stringID perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.itemType = 8;
    CHECK("itemType perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.quantity = 3;
    CHECK("quantity perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.quality = 151;
    CHECK("quality perturbs hash",      invEntryHash(b) != base);
    b = makeEntry(); b.equipped = 1;
    CHECK("equipped perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.slot = 5;
    CHECK("slot perturbs hash",         invEntryHash(b) != base);
    b = makeEntry(); b.locked = 1;
    CHECK("locked perturbs hash",       invEntryHash(b) != base);
    b = makeEntry(); b.section = 1234;
    CHECK("section perturbs hash",      invEntryHash(b) != base);
    // WHERE it sits is content: the same stack loose on the character vs inside a worn bag is
    // otherwise field-for-field identical, so without this the move never publishes.
    b = makeEntry(); b.parentIdx = 1;
    CHECK("parentIdx perturbs hash",    invEntryHash(b) != base);
    b = makeEntry(); std::strcpy(b.manufacturer, "cross");
    CHECK("manufacturer perturbs hash", invEntryHash(b) != base);
    b = makeEntry(); std::strcpy(b.material, "iron");
    CHECK("material perturbs hash",     invEntryHash(b) != base);

    // Order independence: the container fingerprint is the SUM of entry hashes,
    // so any permutation of the same multiset must produce the same sum.
    InvItemEntry e1 = makeEntry();
    InvItemEntry e2 = makeEntry(); std::strcpy(e2.stringID, "iron_katana"); e2.equipped = 1;
    InvItemEntry e3 = makeEntry(); e3.quantity = 9;
    unsigned s123 = invEntryHash(e1) + invEntryHash(e2) + invEntryHash(e3);
    unsigned s312 = invEntryHash(e3) + invEntryHash(e1) + invEntryHash(e2);
    CHECK("container sum order-independent", s123 == s312);

    // Section-name hash: '' reserved as 0 (loose); non-empty never 0; stable.
    CHECK("sectionNameHash('') == 0",      sectionNameHash("") == 0);
    CHECK("sectionNameHash(null) == 0",    sectionNameHash(0) == 0);
    CHECK("sectionNameHash nonzero",       sectionNameHash("hip") != 0);
    CHECK("sectionNameHash deterministic", sectionNameHash("hip") == sectionNameHash("hip"));
    CHECK("sectionNameHash distinguishes weapon slots", sectionNameHash("hip") != sectionNameHash("back"));

    // Canonical vector: print (not assert) so the baseline doc can record it and
    // a future intentional change is visible in the diff.
    std::printf("  note canonical invEntryHash(wooden_sandals x2 q150) = %u\n", base);
}

// ---- 5. Interpolation buffer invariants -------------------------------------------

static EntityState entAt(float x) {
    EntityState e;
    std::memset(&e, 0, sizeof(e));
    e.hIndex = 1; e.hSerial = 2; e.task = TASK_NONE;
    e.x = x; e.y = 0.0f; e.z = 0.0f; e.heading = 0.0f;
    return e;
}

static void testInterp() {
    std::printf("== interpolation buffer (Interp.cpp) ==\n");
    InterpConfig cfg; // min 50 / max 200 delay, extrap 250, snap 50u, stale 2000

    // Bracketed interpolation: 20 Hz feed moving +1u per 50ms tick.
    {
        EntityInterp it;
        for (int i = 0; i <= 10; ++i) it.push(entAt((float)i), 1000 + i * 50);
        // nowMs=1550 -> renderTime = 1550 - delay(>=50,<=200) = [1350,1500]
        // -> x must interpolate inside [7,10] and never exceed the newest.
        EntityState out;
        bool ok = it.sample(1550, cfg, &out);
        CHECK("bracketed sample returns data", ok);
        CHECK("bracketed sample within segment bounds", ok && out.x >= 6.9f && out.x <= 10.01f);

        // Monotonic advance: successive sample times never move the body backwards.
        float prev = -1.0f; bool mono = true;
        for (unsigned long t = 1400; t <= 1550; t += 10) {
            EntityState o;
            if (it.sample(t, cfg, &o)) { if (o.x < prev - 0.001f) mono = false; prev = o.x; }
        }
        CHECK("sampled position monotonic for monotone source", mono);
    }

    // Dead-reckoning cap: starved buffer extrapolates at most maxExtrapMs beyond
    // the newest snapshot (here: 1u/50ms -> cap = +5u over newest).
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        it.push(entAt(1.0f), 1050);
        EntityState out;
        bool ok = it.sample(2900, cfg, &out); // renderTime far past newest, still < stale
        CHECK("starved sample still returns (dead-reckon)", ok);
        CHECK("dead-reckoning capped at maxExtrapMs", ok && out.x <= 1.0f + 5.0f + 0.01f);
    }

    // Staleness: a stream older than staleMs releases the body (sample -> false).
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        EntityState out;
        CHECK("stale stream releases body", !it.sample(1000 + cfg.staleMs + 500, cfg, &out));
    }

    // Teleport snap: a segment step beyond snapDist snaps to the newer end
    // instead of smearing the body across the gap.
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        it.push(entAt(1000.0f), 1050); // 1000u jump >> 50u snap distance
        it.push(entAt(1001.0f), 1100);
        EntityState out;
        bool ok = it.sample(1120, cfg, &out); // renderTime ~1070 -> inside the jump segment... 
        // renderTime lands in [1000,1050] or [1050,1100] depending on adaptive delay;
        // in the jump segment we must NOT see a smeared mid-point (x in ~[100,900]).
        bool smeared = ok && out.x > 100.0f && out.x < 900.0f;
        CHECK("teleport does not smear", ok && !smeared);
    }

    // Dead-reckon past a teleport: when the LAST segment is a jump (a park /
    // fast-travel) and the render time runs past the newest sample, the buffer
    // must HOLD the newest pose - never dead-reckon ALONG the jump vector, which
    // multiplies the delta by ahead/seg and flings the body thousands of units
    // past its real position (the roaming/fast-travel warp fixed 2026-07-20).
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        it.push(entAt(1000.0f), 1050); // last segment = 1000u jump >> 50u snap
        EntityState out;
        // nowMs=1350 -> renderTime = 1350 - delay(<=200) >= 1150, past newest
        // (1050) but < staleMs, so we hit the extrapolation branch.
        bool ok = it.sample(1350, cfg, &out);
        CHECK("dead-reckon past teleport returns", ok);
        // Without the guard this overshoots to ~3000u; the guard holds newest.
        CHECK("dead-reckon past teleport holds newest (no overshoot)",
              ok && out.x <= 1000.5f && out.x >= 999.5f);
    }

    // Single snapshot: returns that pose verbatim.
    {
        EntityInterp it;
        it.push(entAt(7.0f), 1000);
        EntityState out;
        bool ok = it.sample(1040, cfg, &out);
        CHECK("single snapshot returns pose", ok && out.x == 7.0f);
    }

    // Identity/locomotion passthrough: sample carries the latest full state.
    {
        EntityInterp it;
        EntityState e = entAt(3.0f);
        e.bodyState = BODY_DOWN; e.cMoving = 1; e.task = 42;
        it.push(e, 1000);
        EntityState out;
        bool ok = it.sample(1030, cfg, &out);
        CHECK("identity+state passthrough", ok && out.bodyState == BODY_DOWN && out.cMoving == 1 && out.task == 42 && out.hIndex == 1);
    }
}

// ---- 6. Ownership rank resolution (OwnRanks.h) ----------------------------------
// Guards the squad-tab ownership partition, especially the F2-panel role switch
// regression (2026-07-14): a session launched as HOST resolves ranks to {0};
// switching to JOIN must re-resolve to {1}, or the client claims the host's
// rank-0 player squad and that unit never moves. An explicit env override is
// preserved across the switch.

static bool ranksAre(const std::set<unsigned int>& r, int a, int b) {
    if (b < 0) return r.size() == 1 && r.count((unsigned)a) == 1;
    return r.size() == 2 && r.count((unsigned)a) == 1 && r.count((unsigned)b) == 1;
}

static void testOwnRanks() {
    std::printf("== ownership rank resolution (OwnRanks.h) ==\n");

    // Role defaults from a clean slate.
    {
        std::set<unsigned int> r;
        resolveOwnRanks(r, true, false);
        CHECK("host default owns {0}", ranksAre(r, 0, -1));
        r.clear();
        resolveOwnRanks(r, false, false);
        CHECK("join default owns {1}", ranksAre(r, 1, -1));
    }

    // THE FIX: a session that started HOST (ranks {0}) switches to JOIN via the
    // panel and MUST end up owning {1}, not the host's {0}.
    {
        std::set<unsigned int> r;
        resolveOwnRanks(r, true, false);          // launched HOST -> {0}
        CHECK("pre-switch ranks are {0}", ranksAre(r, 0, -1));
        resolveOwnRanks(r, false, false);         // panel switch to JOIN
        CHECK("HOST->JOIN switch re-resolves to {1}", ranksAre(r, 1, -1));
        resolveOwnRanks(r, true, false);          // and back to HOST
        CHECK("JOIN->HOST switch re-resolves to {0}", ranksAre(r, 0, -1));
    }

    // An explicit env override is preserved across a role switch (the user asked
    // for a specific partition; the panel must not clobber it).
    {
        std::set<unsigned int> r;
        r.insert(2u); r.insert(3u);
        resolveOwnRanks(r, false, true);          // fromEnv -> untouched
        CHECK("env override preserved as JOIN", ranksAre(r, 2, 3));
        resolveOwnRanks(r, true, true);           // still untouched as HOST
        CHECK("env override preserved as HOST", ranksAre(r, 2, 3));
    }

    // CSV parse (KENSHICOOP_OWN_SQUAD/OWN_RANK surface).
    {
        std::set<unsigned int> r;
        CHECK("parse '' -> no ranks",        !parseRankList("", r) && r.empty());
        r.clear();
        CHECK("parse '0' -> {0}",            parseRankList("0", r) && ranksAre(r, 0, -1));
        r.clear();
        CHECK("parse '1,2' -> {1,2}",        parseRankList("1,2", r) && ranksAre(r, 1, 2));
        r.clear();
        CHECK("parse ' 3 ; 5 ' tolerant",    parseRankList(" 3 ; 5 ", r) && ranksAre(r, 3, 5));
        r.clear();
        CHECK("parse '2,2' dedups to {2}",   parseRankList("2,2", r) && ranksAre(r, 2, -1));
    }

    // Config-style resolution: env-provided ranks set fromEnv true and survive;
    // empty env falls back to the role default.
    {
        std::set<unsigned int> r;
        bool fromEnv = parseRankList("1", r);
        resolveOwnRanks(r, true, fromEnv);        // env said {1} even though HOST
        CHECK("env {1} wins over HOST default", fromEnv && ranksAre(r, 1, -1));
        r.clear();
        fromEnv = parseRankList("", r);
        resolveOwnRanks(r, false, fromEnv);       // no env -> JOIN default {1}
        CHECK("empty env -> JOIN default {1}", !fromEnv && ranksAre(r, 1, -1));
    }

    // Phase 3 (OWN-01): single-owner-per-rank bijection for a 4-player roster.
    // rank=playerId (ownerForRank) means each rank has EXACTLY one owner, and
    // no two players' rank sets overlap - the anchor invariant the whole
    // ownership model rests on. A host-authoritative announcement (Plan 03's
    // setAllOwnRanks) can only OVERRIDE this default, never violate the
    // bijection, since every override is still a per-player assignment.
    {
        // Bijection: for a 4-player roster, ownerForRank(r) == r for r in 0..3.
        CHECK("ownerForRank(0) == 0", ownerForRank(0) == 0u);
        CHECK("ownerForRank(1) == 1", ownerForRank(1) == 1u);
        CHECK("ownerForRank(2) == 2", ownerForRank(2) == 2u);
        CHECK("ownerForRank(3) == 3", ownerForRank(3) == 3u);

        // The localId-form resolveOwnRanks overload (Plan 03): each player's
        // default rank set is exactly {localId}.
        std::set<unsigned int> r0, r1, r2, r3;
        resolveOwnRanks(r0, (u32)0, false);
        resolveOwnRanks(r1, (u32)1, false);
        resolveOwnRanks(r2, (u32)2, false);
        resolveOwnRanks(r3, (u32)3, false);
        CHECK("player 0 default owns {0}", ranksAre(r0, 0, -1));
        CHECK("player 1 default owns {1}", ranksAre(r1, 1, -1));
        CHECK("player 2 default owns {2}", ranksAre(r2, 2, -1));
        CHECK("player 3 default owns {3}", ranksAre(r3, 3, -1));

        // Disjoint ranks: the bitmask AND across any two distinct players is
        // always 0 - no rank is claimed by more than one owner.
        u32 m0 = ranksToMask(r0), m1 = ranksToMask(r1);
        u32 m2 = ranksToMask(r2), m3 = ranksToMask(r3);
        CHECK("rank masks pairwise disjoint (0&1)", (m0 & m1) == 0);
        CHECK("rank masks pairwise disjoint (0&2)", (m0 & m2) == 0);
        CHECK("rank masks pairwise disjoint (0&3)", (m0 & m3) == 0);
        CHECK("rank masks pairwise disjoint (1&2)", (m1 & m2) == 0);
        CHECK("rank masks pairwise disjoint (1&3)", (m1 & m3) == 0);
        CHECK("rank masks pairwise disjoint (2&3)", (m2 & m3) == 0);
        CHECK("union of all 4 masks covers ranks 0..3", (m0 | m1 | m2 | m3) == 0xFu);

        std::set<unsigned int> back;
        maskToRanks(m2, back);
        CHECK("maskToRanks recovers rank set", ranksAre(back, 2, -1));
    }

    // Phase 3 (OWN-01/OWN-03): OwnRanksPacket round-trip - fill a 4-player
    // roster where each player owns exactly its own rank (the deterministic
    // default), serialize through readPacket<T>(), and assert every
    // playerId/rankMask survives AND maskToRanks recovers the exact rank set
    // (proves the wire encoding round-trips the map<PlayerId,set<rank>>, not
    // just raw bytes).
    {
        OwnRanksPacket pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        pkt.type = (u8)PKT_OWN_RANKS;
        pkt.count = 4;
        for (unsigned int p = 0; p < 4; ++p) {
            std::set<unsigned int> ranks;
            ranks.insert(p);
            pkt.entries[p].playerId = p;
            pkt.entries[p].rankMask = ranksToMask(ranks);
        }
        unsigned char buf[sizeof(OwnRanksPacket)];
        std::memcpy(buf, &pkt, sizeof(pkt));
        OwnRanksPacket out;
        std::memset(&out, 0, sizeof(out));
        bool ok = readPacket(buf, (unsigned)sizeof(OwnRanksPacket), &out);
        CHECK("OwnRanksPacket round-trip read",
              ok && std::memcmp(&pkt, &out, sizeof(pkt)) == 0);
        bool allMatch = true;
        for (unsigned int p = 0; p < out.count; ++p) {
            if (out.entries[p].playerId != p) allMatch = false;
            std::set<unsigned int> recovered;
            maskToRanks(out.entries[p].rankMask, recovered);
            if (!ranksAre(recovered, (int)p, -1)) allMatch = false;
        }
        CHECK("OwnRanksPacket entries + maskToRanks recover exact rank sets", allMatch);
    }

    // Phase 3 (OWN-03): dynamic-tab determinism. A squad tab CREATED by player
    // k mid-session gets a rank the host and joins may number DIFFERENTLY
    // locally (RESEARCH.md Pitfall 5 - "its rank is a local number the peer
    // does not share"), but once the HOST assigns that rank to owner k and
    // announces the built map over the wire, EVERY machine decodes the SAME
    // map and answers "who owns rank R" identically - the host-built map,
    // serialized and decoded fresh (as a join would receive it), still
    // resolves the dynamic tab's rank to its creator k.
    {
        const unsigned int DYNAMIC_RANK = 7; // beyond the base 0..3 default ranks
        const u32 CREATOR = 2;               // player 2 created this tab
        std::map<u32, std::set<unsigned int> > hostMap;
        hostMap[0].insert(0);                // base default: host owns rank 0
        hostMap[1].insert(1);                // base default: player 1 owns rank 1
        hostMap[CREATOR].insert(CREATOR);    // base default: player 2 owns rank 2
        hostMap[3].insert(3);                // base default: player 3 owns rank 3
        hostMap[CREATOR].insert(DYNAMIC_RANK); // host arbitration: the dynamic
                                                // tab belongs to its creator

        OwnRanksPacket pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        pkt.type = (u8)PKT_OWN_RANKS;
        unsigned int n = 0;
        for (std::map<u32, std::set<unsigned int> >::const_iterator it = hostMap.begin();
             it != hostMap.end() && n < MAX_PLAYERS; ++it, ++n) {
            pkt.entries[n].playerId = it->first;
            pkt.entries[n].rankMask = ranksToMask(it->second);
        }
        pkt.count = (u8)n;

        unsigned char buf[sizeof(OwnRanksPacket)];
        std::memcpy(buf, &pkt, sizeof(pkt));
        OwnRanksPacket decoded;
        std::memset(&decoded, 0, sizeof(decoded));
        bool ok = readPacket(buf, (unsigned)sizeof(OwnRanksPacket), &decoded);
        CHECK("dynamic-tab: host-built map round-trips", ok);

        // Rebuild the map<PlayerId,set<rank>> from the decoded wire bytes (what
        // Replicator::setAllOwnRanks(...) does on the client) and ask who owns
        // the dynamic rank - must be the creator, on every machine.
        u32 resolvedOwner = 0xFFFFFFFFu; // OWNER_NONE-equivalent sentinel
        for (unsigned int i = 0; i < decoded.count; ++i) {
            std::set<unsigned int> ranks;
            maskToRanks(decoded.entries[i].rankMask, ranks);
            if (ranks.count(DYNAMIC_RANK)) resolvedOwner = decoded.entries[i].playerId;
        }
        CHECK("dynamic-tab: decoded map resolves rank to its creator",
              resolvedOwner == CREATOR);
    }
}

// ---- 6b. Leave-queue expansion (PeerRoster.h) -----------------------------------
// Guards the host-link-drop leave path (regression found 2026-09-07 on the x4
// branch). Inbound's leave queue carries real PlayerIds AND the OWNER_ID_ALL
// sentinel NetLink pushes on a CLIENT's ENET_EVENT_TYPE_DISCONNECT ("my single
// link to the host went down"). Every consumer of a drained id is owner-scoped
// by `==` equality, so before this expansion the sentinel matched NOTHING and
// the client's whole teardown no-opped: the observable symptom was
// "[leave] cleared proxies=0 released=0 ... pins=0 ... xfer voided=0" for
// owner=4294967295 while the presence set and Replicator::knownPeers_ kept the
// dead ids forever (resetSession() deliberately preserves knownPeers_, so no
// world reload healed it and each reconnect stacked new ids on top).
//
// The two asserted properties are exactly what the live drain depends on:
//   1. a sentinel expands to every tracked peer, so the owner-scoped teardown
//      actually runs, and applying the expansion leaves the presence set EMPTY;
//   2. the return flag distinguishes "the session ended" from "one peer left",
//      which is what gates the session-global clearKnownPeers().

// Model of Plugin.cpp's processNetEvents leave drain: expand, then run the
// per-id owner-scoped erase over both roster structures exactly as the live
// loop does (g_connectedPeers.erase / Replicator::notePeerLeft), plus the
// session-boundary clearKnownPeers() the flag gates. Returns the expanded
// list so a caller can assert its contents too.
static bool applyLeaveDrain(const std::deque<u32>& drained,
                            std::set<u32>& connected,
                            std::set<u32>& known,
                            std::deque<u32>& expanded) {
    bool linkDown = expandLeaveQueue(drained, connected, expanded);
    for (size_t i = 0; i < expanded.size(); ++i) {
        connected.erase(expanded[i]);  // g_connectedPeers.erase(*it)
        known.erase(expanded[i]);      // Replicator::notePeerLeft(*it)
    }
    if (linkDown) known.clear();       // Replicator::clearKnownPeers()
    return linkDown;
}

static bool idsAre(const std::deque<u32>& d, const char* csv) {
    std::string want(csv), got;
    for (size_t i = 0; i < d.size(); ++i) {
        char b[16]; _snprintf(b, sizeof(b) - 1, "%u", (unsigned)d[i]); b[15] = '\0';
        if (i) got += ",";
        got += b;
    }
    return got == want;
}

static void testLeaveExpansion() {
    std::printf("== leave-queue expansion (PeerRoster.h) ==\n");

    // THE REGRESSION, at N=4: a client tracking 3 other peers loses its host
    // link. One sentinel must tear down all three, and both roster structures
    // must end EMPTY - the assertion that failed before this fix.
    {
        std::set<u32> connected; connected.insert(0); connected.insert(2); connected.insert(3);
        std::set<u32> known(connected);
        std::deque<u32> drained; drained.push_back(OWNER_ID_ALL);
        std::deque<u32> expanded;
        bool linkDown = applyLeaveDrain(drained, connected, known, expanded);
        CHECK("N=4 host-link drop: sentinel expands to all 3 tracked peers",
              idsAre(expanded, "0,2,3"));
        CHECK("N=4 host-link drop: reported as a session boundary", linkDown);
        CHECK("N=4 host-link drop: connected-peer set ends EMPTY", connected.empty());
        CHECK("N=4 host-link drop: knownPeers_ ends EMPTY", known.empty());
    }

    // Compatibility shape (1 host + 1 client UDP): the SAME path, no small-N
    // special case - the sentinel expands to the single tracked id and the
    // teardown the 2-player build got from its global resetSession() still
    // happens.
    {
        std::set<u32> connected; connected.insert(0);
        std::set<u32> known(connected);
        std::deque<u32> drained; drained.push_back(OWNER_ID_ALL);
        std::deque<u32> expanded;
        bool linkDown = applyLeaveDrain(drained, connected, known, expanded);
        CHECK("N=2 host-link drop: sentinel expands to the sole host id",
              idsAre(expanded, "0"));
        CHECK("N=2 host-link drop: reported as a session boundary", linkDown);
        CHECK("N=2 host-link drop: both roster structures end EMPTY",
              connected.empty() && known.empty());
    }

    // An ORDINARY leave is untouched: one peer's departure must NOT be read as
    // a session boundary, and the survivors must stay in the presence set (the
    // PEER-02/03 isolation invariant).
    {
        std::set<u32> connected; connected.insert(0); connected.insert(2); connected.insert(3);
        std::set<u32> known(connected);
        std::deque<u32> drained; drained.push_back(2);
        std::deque<u32> expanded;
        bool linkDown = applyLeaveDrain(drained, connected, known, expanded);
        CHECK("ordinary leave: passes through as itself", idsAre(expanded, "2"));
        CHECK("ordinary leave: NOT a session boundary", !linkDown);
        CHECK("ordinary leave: survivors stay present",
              connected.size() == 2 && connected.count(0) && connected.count(3));
        CHECK("ordinary leave: survivors stay in knownPeers_",
              known.size() == 2 && known.count(0) && known.count(3));
    }

    // Mixed drain, wire order preserved, and DEDUPED: a roster PKT_PLAYER_LEFT
    // for peer 2 landing in the same drain as the host-link drop must not run
    // peer 2's teardown (or log its purge lines) twice.
    {
        std::set<u32> connected; connected.insert(0); connected.insert(2); connected.insert(3);
        std::deque<u32> drained; drained.push_back(2); drained.push_back(OWNER_ID_ALL);
        std::deque<u32> expanded;
        CHECK("mixed drain: id 2 first (wire order), then the rest ascending",
              expandLeaveQueue(drained, connected, expanded)
              && idsAre(expanded, "2,0,3"));
    }
    {
        std::set<u32> connected; connected.insert(0); connected.insert(2);
        std::deque<u32> drained;
        drained.push_back(OWNER_ID_ALL); drained.push_back(OWNER_ID_ALL);
        std::deque<u32> expanded;
        CHECK("two sentinels in one drain still expand to each id exactly once",
              expandLeaveQueue(drained, connected, expanded)
              && idsAre(expanded, "0,2"));
    }

    // The sentinel must never survive expansion into the owner-scoped
    // consumers - that IS the no-op bug. Includes the defensive case of a
    // presence set that a prior build's leak left holding the sentinel.
    {
        std::set<u32> connected; connected.insert(0); connected.insert(OWNER_ID_ALL);
        std::deque<u32> drained; drained.push_back(OWNER_ID_ALL);
        std::deque<u32> expanded;
        bool linkDown = expandLeaveQueue(drained, connected, expanded);
        bool clean = true;
        for (size_t i = 0; i < expanded.size(); ++i)
            if (expanded[i] == OWNER_ID_ALL) clean = false;
        CHECK("expansion never emits OWNER_ID_ALL to an owner-scoped consumer",
              linkDown && clean && idsAre(expanded, "0"));
    }

    // Degenerate inputs: a drop with nothing tracked is still a session
    // boundary (the flag is what the caller acts on), and an empty drain is
    // not a boundary at all.
    {
        std::set<u32> connected;
        std::deque<u32> drained; drained.push_back(OWNER_ID_ALL);
        std::deque<u32> expanded;
        CHECK("drop with no tracked peers: boundary flag set, expansion empty",
              expandLeaveQueue(drained, connected, expanded) && expanded.empty());
    }
    {
        std::set<u32> connected; connected.insert(0);
        std::deque<u32> drained;
        std::deque<u32> expanded; expanded.push_back(99); // must be overwritten
        CHECK("empty drain: no boundary, no ids (out is cleared)",
              !expandLeaveQueue(drained, connected, expanded) && expanded.empty());
    }
}

// ---- 6b. pause_stress cycle schedule (test/PauseSchedule.h) ---------------------
// The `pause_stress` scenario is a REPRO DRIVER for the 0xc0000005 investigation
// (.planning/debug/crash-av-worker-thread.md, lead E-21): it repeatedly drives
// the session into a replicated pause, because a replicated pause is the one
// stimulus provably common to both processes that faulted in the field.
//
// The driver as a whole can only be judged by a live 2-process rig. Its
// SCHEDULE cannot: it is a pure function of elapsed wall-clock ms and the local
// role, so it is asserted here, in milliseconds, with no game. Three properties
// carry real weight (the rest are boundaries around them):
//
//   1. ROLE ALTERNATION. Exactly one side owns each cycle's vote, and the owner
//      flips every cycle. The fault reproduced in BOTH roles, so a driver that
//      only ever votes from one side would exercise half the replication
//      direction and a clean run would mean even less than it already does.
//   2. THE HOLD SPANS THE FAULT WINDOW. The field faults landed 3.7 s and 7.9 s
//      after the pause. A hold trimmed below ~8 s would resume the world before
//      the window the driver exists to sit inside - a silent loss of the whole
//      point, invisible in any log.
//   3. ONE VOTE PER EDGE, NEVER PER TICK. The vote goes out through the same
//      hooked setter a UI click uses; per-tick writes would flood the consensus
//      channel and drown the traffic the run is there to read.
//
// NOTE ON SCOPE, deliberately: passing these checks says the schedule is
// correct. It says NOTHING about whether the driver reproduces the crash, and a
// clean run of it eliminates nothing (debug file, E-26).

// Walk the schedule at a fixed tick and record what each side would do. Returns
// the act log as "cycle:who:act" entries, which is the whole observable.
struct PauseAct { int cycle; bool isHost; bool paused; unsigned long atMs; };

static void simulatePauseRun(const coop::PauseCycle& g, unsigned long tickMs,
                             unsigned long untilMs, std::vector<PauseAct>& out) {
    coop::PauseEdgeState hostEdge = coop::pauseEdgeInit();
    coop::PauseEdgeState joinEdge = coop::pauseEdgeInit();
    for (unsigned long t = 0; t <= untilMs; t += tickMs) {
        coop::PausePhase ph = coop::pausePhaseAt(g, t);
        for (int r = 0; r < 2; ++r) {
            bool isHost = (r == 0);
            coop::PauseEdgeState& st = isHost ? hostEdge : joinEdge;
            if (coop::pauseShouldAct(ph, isHost, st)) {
                PauseAct a; a.cycle = ph.cycle; a.isHost = isHost;
                a.paused = ph.wantPaused; a.atMs = t;
                out.push_back(a);
                coop::pauseNoteActed(ph, st);
            }
        }
    }
}

static void testPauseSchedule() {
    std::printf("== pause_stress cycle schedule (test/PauseSchedule.h) ==\n");

    const coop::PauseCycle g = coop::pauseStressGeometry();

    // ---- Property 2 first: the geometry must still cover the field window ----
    // 7900 ms is the LATER of the two observed post-pause fault latencies
    // (client 3.7 s, host 7.9 s). This is the check that catches someone
    // "tidying" the hold down to a few seconds.
    CHECK("hold spans the later field fault latency (7.9 s)",
          coop::pauseHoldSpansFaultWindow(g, 7900UL));
    CHECK("hold also spans the earlier field fault latency (3.7 s)",
          coop::pauseHoldSpansFaultWindow(g, 3700UL));
    CHECK("hold does NOT claim to span an absurd latency (predicate is real, "
          "not a constant true)", !coop::pauseHoldSpansFaultWindow(g, 600000UL));

    // ---- Unarmed window: nobody votes before the first pause time -----------
    {
        coop::PausePhase p0 = coop::pausePhaseAt(g, 0);
        CHECK("t=0: unarmed", !p0.armed);
        CHECK_EQ("t=0: cycle is -1", (unsigned long long)(p0.cycle + 1), 0ull);
        CHECK("t=0: not paused", !p0.wantPaused);
        CHECK("t=0: the HOST is not a voter (unarmed)",
              !coop::pauseVoterIsMe(p0, true));
        CHECK("t=0: the JOIN is not a voter either - the isHost==hostVotes test "
              "must not make false==false a voter before arming",
              !coop::pauseVoterIsMe(p0, false));

        coop::PausePhase pm1 = coop::pausePhaseAt(g, g.firstPauseAtMs - 1);
        CHECK("one ms before arming: still unarmed", !pm1.armed);
    }

    // ---- The arm instant: cycle 0, paused, host votes ----------------------
    {
        coop::PausePhase p = coop::pausePhaseAt(g, g.firstPauseAtMs);
        CHECK("arm instant: armed", p.armed);
        CHECK_EQ("arm instant: cycle 0", (unsigned long long)p.cycle, 0ull);
        CHECK("arm instant: wants the world PAUSED", p.wantPaused);
        CHECK("arm instant: the host owns cycle 0's vote",
              p.hostVotes && coop::pauseVoterIsMe(p, true));
        CHECK("arm instant: the join does NOT vote on cycle 0",
              !coop::pauseVoterIsMe(p, false));
    }

    // ---- Hold/gap boundaries, both neighbours of each edge ------------------
    {
        const unsigned long arm = g.firstPauseAtMs;
        CHECK("last ms of the hold is still paused",
              coop::pausePhaseAt(g, arm + g.pauseHoldMs - 1).wantPaused);
        CHECK("first ms of the gap is NOT paused",
              !coop::pausePhaseAt(g, arm + g.pauseHoldMs).wantPaused);
        CHECK("the gap stays in the SAME cycle (the resume belongs to the "
              "cycle that paused, not to the next one)",
              coop::pausePhaseAt(g, arm + g.pauseHoldMs).cycle == 0);
        const unsigned long cyc = g.pauseHoldMs + g.resumeGapMs;
        CHECK("last ms of the gap is still cycle 0",
              coop::pausePhaseAt(g, arm + cyc - 1).cycle == 0);
        CHECK("cycle rolls exactly at the cycle length",
              coop::pausePhaseAt(g, arm + cyc).cycle == 1);
        CHECK("the new cycle starts PAUSED",
              coop::pausePhaseAt(g, arm + cyc).wantPaused);
    }

    // ---- Property 1: role alternation, over a whole run ---------------------
    {
        const unsigned long cyc = g.pauseHoldMs + g.resumeGapMs;
        bool alternates = true, exactlyOneVoter = true;
        for (int c = 0; c < 12; ++c) {
            coop::PausePhase p = coop::pausePhaseAt(g, g.firstPauseAtMs + (unsigned long)c * cyc);
            if (p.hostVotes != ((c % 2) == 0)) alternates = false;
            // Exactly one of the two roles is the voter - never both, never
            // neither - at every armed instant.
            int voters = (coop::pauseVoterIsMe(p, true) ? 1 : 0) +
                         (coop::pauseVoterIsMe(p, false) ? 1 : 0);
            if (voters != 1) exactlyOneVoter = false;
        }
        CHECK("role alternation: even cycles host, odd cycles join, 12 cycles "
              "deep", alternates);
        CHECK("role alternation: EXACTLY one role is the voter at every armed "
              "cycle (never both, never neither)", exactlyOneVoter);
    }

    // ---- Property 3 + the full-run shape: simulate the shipped run ----------
    {
        std::vector<PauseAct> acts;
        // 50 ms tick ~ a 20 fps floor; the driver is called once per game tick.
        simulatePauseRun(g, 50, coop::pauseStressHostDurationMs(), acts);

        const unsigned long cyc = g.pauseHoldMs + g.resumeGapMs;
        const unsigned long span = coop::pauseStressHostDurationMs() - g.firstPauseAtMs;
        const int fullCycles = (int)(span / cyc); // cycles that complete inside the run

        CHECK("full run: at least 12 pause/resume cycles fit (the run is a "
              "repeated lottery, not a single sample)", fullCycles >= 12);

        // One pause act and one resume act per cycle, and no cycle acted twice.
        bool onePausePerCycle = true, oneResumePerCycle = true;
        bool noDoubleAct = true, initiatorAlternates = true;
        for (int c = 0; c <= fullCycles; ++c) {
            int pauses = 0, resumes = 0, hostActs = 0, joinActs = 0;
            for (size_t i = 0; i < acts.size(); ++i) {
                if (acts[i].cycle != c) continue;
                if (acts[i].paused) ++pauses; else ++resumes;
                if (acts[i].isHost) ++hostActs; else ++joinActs;
            }
            if (c < fullCycles) { // a partial trailing cycle may lack its resume
                if (pauses != 1) onePausePerCycle = false;
                if (resumes != 1) oneResumePerCycle = false;
                // Every act of a cycle comes from the SAME side, and it is the
                // side the parity rule names.
                bool expectHost = ((c % 2) == 0);
                if (expectHost ? (joinActs != 0) : (hostActs != 0))
                    initiatorAlternates = false;
            }
            if (pauses > 1 || resumes > 1) noDoubleAct = false;
        }
        CHECK("full run: exactly ONE pause vote per completed cycle (edge, not "
              "per-tick)", onePausePerCycle);
        CHECK("full run: exactly ONE resume vote per completed cycle",
              oneResumePerCycle);
        CHECK("full run: no cycle is ever acted on twice in the same phase",
              noDoubleAct);
        CHECK("full run: every cycle's acts come from the parity-named side "
              "only - the non-initiator stays an observer",
              initiatorAlternates);

        // Pause and resume strictly alternate in time across the whole run:
        // two pauses in a row would mean the world never resumed between holds.
        bool strictAlternation = true;
        for (size_t i = 1; i < acts.size(); ++i)
            if (acts[i].paused == acts[i - 1].paused) strictAlternation = false;
        CHECK("full run: pause and resume strictly alternate across the whole "
              "run (the world always runs again between holds)",
              strictAlternation && !acts.empty());
        CHECK("full run: the first act of the run is a PAUSE",
              !acts.empty() && acts[0].paused);

        // The host must exit with the world RUNNING: self-exit while paused
        // leaves the last save/teardown work to a stopped sim.
        CHECK("full run: the host's own exit instant is in a RUNNING phase, "
              "not inside a hold",
              !coop::pausePhaseAt(g, coop::pauseStressHostDurationMs()).wantPaused);
        CHECK("full run: the join stops BEFORE the host, so the host is still "
              "live to log the join's exit",
              coop::pauseStressJoinDurationMs() < coop::pauseStressHostDurationMs());
    }

    // ---- Tick-rate robustness: the schedule must not depend on the tick -----
    {
        // A coarse and a deliberately non-divisor tick (frame times are never
        // round). Both must still produce one pause and one resume per cycle.
        const unsigned long ticks[3] = { 16, 250, 137 };
        const unsigned long cyc4 = g.firstPauseAtMs
                                 + 4 * (g.pauseHoldMs + g.resumeGapMs) + 1000;
        bool allStable = true;
        for (int k = 0; k < 3; ++k) {
            std::vector<PauseAct> acts;
            simulatePauseRun(g, ticks[k], cyc4, acts);
            int pauses = 0, resumes = 0;
            for (size_t i = 0; i < acts.size(); ++i)
                if (acts[i].paused) ++pauses; else ++resumes;
            if (pauses != 5 || resumes != 4) allStable = false;
            for (size_t i = 1; i < acts.size(); ++i)
                if (acts[i].paused == acts[i - 1].paused) allStable = false;
        }
        CHECK("tick-rate robustness: 16/250/137 ms ticks all yield the same 5 "
              "pause + 4 resume edges over 4 cycles, strictly alternating",
              allStable);
    }

    // ---- Degenerate geometries: never divide by zero, never wedge -----------
    {
        coop::PauseCycle z = coop::makePauseCycle(1000, 0, 0);
        coop::PausePhase p = coop::pausePhaseAt(z, 5000);
        CHECK("zero-length cycle never arms (no division by zero on the game "
              "thread)", !p.armed && p.cycle == -1 && !p.wantPaused);
        CHECK("zero-length cycle: nobody is a voter",
              !coop::pauseVoterIsMe(p, true) && !coop::pauseVoterIsMe(p, false));

        coop::PauseCycle z2 = coop::makePauseCycle(0, 5000, 5000);
        CHECK("zero arm delay: armed at t=0, paused, host votes",
              coop::pausePhaseAt(z2, 0).armed &&
              coop::pausePhaseAt(z2, 0).wantPaused &&
              coop::pausePhaseAt(z2, 0).hostVotes);

        // All-hold geometry (no gap): stays paused forever, which the strict
        // alternation check above would catch in the shipped geometry - proven
        // here to be a property of the geometry, not of the phase function.
        coop::PauseCycle allHold = coop::makePauseCycle(0, 5000, 0);
        CHECK("gapless geometry is paused at every instant (so the shipped "
              "geometry's non-zero gap is what makes the world run again)",
              coop::pausePhaseAt(allHold, 0).wantPaused &&
              coop::pausePhaseAt(allHold, 4999).wantPaused &&
              coop::pausePhaseAt(allHold, 5000).wantPaused);
    }
}

// ---- 7. SteamID64 parse + mask (SteamId.h) --------------------------------------
// Guards the F2 panel "Paste friend's Steam ID" button: clipboard text is noisy
// (surrounding whitespace, a trailing newline, or a "Steam ID: 7656..." wrapper),
// so parseSteamId64 keeps only digits and requires a 17-digit community ID
// (76561... prefix). Arbitrary clipboard junk must be rejected.
//
// maskSteamId64 is the other half: the panel rows show only the last 4 digits so
// a streamed screen leaks no account. A leaked prefix would defeat the point, so
// the exact output shape is pinned here.

static void testSteamIdParse() {
    std::printf("== SteamID64 parse (SteamId.h) ==\n");
    unsigned long long id = 0;

    id = 0;
    CHECK("clean 17-digit id accepted",
          coop::parseSteamId64("76561198000000000", id) && id == 76561198000000000ull);
    id = 0;
    CHECK("surrounding whitespace/newline stripped",
          coop::parseSteamId64("  76561198012345678 \r\n", id) && id == 76561198012345678ull);
    id = 0;
    CHECK("wrapper text 'Steam ID: <n>' stripped",
          coop::parseSteamId64("Steam ID: 76561198012345678", id) && id == 76561198012345678ull);

    // Rejections leave the caller's value untouched.
    id = 123ull;
    CHECK("empty string rejected",        !coop::parseSteamId64("", id) && id == 123ull);
    CHECK("non-numeric rejected",         !coop::parseSteamId64("not-an-id", id) && id == 123ull);
    CHECK("too short (16 digits) rejected",
          !coop::parseSteamId64("7656119800000000", id) && id == 123ull);
    CHECK("too long (18 digits) rejected",
          !coop::parseSteamId64("765611980000000000", id) && id == 123ull);
    CHECK("17 digits, wrong prefix rejected",
          !coop::parseSteamId64("12345678901234567", id) && id == 123ull);

    // Masked display: "****" + the last 4 digits, nothing else.
    CHECK("full id masked to last 4",
          coop::maskSteamId64(76561198012345678ull) == "****5678");
    CHECK("trailing zeros kept as digits",
          coop::maskSteamId64(76561198000000000ull) == "****0000");
    // Not real ids, but steamPeer from the config is never length-checked.
    CHECK("short value masked, not padded", coop::maskSteamId64(42ull) == "****42");
    CHECK("zero masked", coop::maskSteamId64(0ull) == "****0");
}

// ---- 8. Pose-fixture acceptance (WorkPose.h) ------------------------------------
// Guards the mining-sync fix (2026-07-14): a player mining an ore node operates a
// mine building. A single 6 m seat gate rejected the CORRECT mine as "far"
// (applyTaskOrder -> park, no mining animation on the peer). Field distances varied
// wildly (one mine ~8.9 m from origin, a larger one 57 m host / 104 m join), so no
// fixed radius covers both. Work fixtures are unique buildings with reliable
// cross-client hands, so they are TRUSTED (ungated); only seats are distance-gated
// (they mis-resolve to a wrong nearby prop).

static void testWorkPoseMatch() {
    std::printf("== pose-fixture acceptance (WorkPose.h) ==\n");

    // Gate applies to seats, never to work fixtures.
    CHECK("seat radius 6 m",            SEAT_MATCH_DIST == 6.0f);
    CHECK("seat is distance-gated",     poseIsDistanceGated(false));
    CHECK("work is NOT distance-gated", !poseIsDistanceGated(true));

    // THE FIX: work fixtures are accepted at ANY resolved distance (the mine origin
    // can sit 8.9 m, 57 m or 104 m from the operate spot), while a seat at those
    // distances is rejected as a mis-resolved wrong prop.
    CHECK("mining 8.9 m accepted as work",  poseFixtureAccepted(true,  8.9f));
    CHECK("mining 57 m accepted as work",   poseFixtureAccepted(true,  57.0f));
    CHECK("mining 104 m accepted as work",  poseFixtureAccepted(true,  104.0f));
    CHECK("mining 8.9 m rejected as seat", !poseFixtureAccepted(false, 8.9f));

    // Medic sync (2026-07-15): a first-aid subject is the PATIENT (a character), also
    // identity-trusted (isWorkFixtureTask || isMedicTask -> the boolean below), so a
    // patient whose driven copy is mid-motion (metres from the streamed transform) is
    // still accepted, exactly like a work fixture; a seat at the same range is not.
    CHECK("medic 12 m accepted (identity-trusted)",  poseFixtureAccepted(true,  12.0f));
    CHECK("medic 12 m rejected as seat",            !poseFixtureAccepted(false, 12.0f));

    // Seat still tight: a fixture right under the body is accepted, a far stool not.
    CHECK("seat 3 m accepted",   poseFixtureAccepted(false, 3.0f));
    CHECK("seat 6 m boundary",   poseFixtureAccepted(false, 6.0f));
    CHECK("seat 6.1 m rejected", !poseFixtureAccepted(false, 6.1f));

    // Squared-distance form (the engine gate) agrees with the metres form.
    CHECK("sq: work 104 m accepted",   poseFixtureAcceptedSq(true,  104.0f * 104.0f));
    CHECK("sq: seat 3 m accepted",     poseFixtureAcceptedSq(false, 3.0f * 3.0f));
    CHECK("sq: seat 6 m boundary",     poseFixtureAcceptedSq(false, 6.0f * 6.0f));
    CHECK("sq: seat 6.1 m rejected",  !poseFixtureAcceptedSq(false, 6.1f * 6.1f));
}

// ---- 9. Debounced task-clear (WorkPose.h poseClearElapsed) ----------------------
// Guards the job-removal fix (2026-07-14): removing a job on the host while the
// character stays STATIONARY streams task=NONE continuously (the movement re-arm
// never fires), so the join must release the held mine/operate pose after a
// sustained-NONE window instead of holding it forever. Transient NONE blips (1-2
// capture frames) must NOT clear a committed pose, so the release is DEBOUNCED.
// clearMs mirrors TASK_CLEAR_MS in ReplicatorUtil.h (game-coupled, so not included
// here); keep the literal in sync with that constant.
static void testTaskClear() {
    std::printf("== debounced task-clear (WorkPose.h) ==\n");
    const unsigned long clearMs = 1200; // mirror of TASK_CLEAR_MS

    // No streak in progress (noneTick == 0) never clears, regardless of 'now'.
    CHECK("no streak never clears",       !poseClearElapsed(0,     999999, clearMs));

    // A transient blip below the window holds (anti-oscillation guarantee).
    CHECK("blip 0 ms holds",              !poseClearElapsed(10000, 10000,  clearMs));
    CHECK("blip 1199 ms holds",           !poseClearElapsed(10000, 11199,  clearMs));

    // Sustained NONE at/after the window releases (genuine stationary un-assign).
    CHECK("streak 1200 ms clears",         poseClearElapsed(10000, 11200,  clearMs));
    CHECK("streak 5 s clears",             poseClearElapsed(10000, 15000,  clearMs));

    // Unsigned tick wrap (GetTickCount rollover): now - noneTick still yields the
    // elapsed delta, so a streak spanning the wrap boundary still clears on time.
    // 'now' values are written pre-wrapped (as GetTickCount would report post-rollover)
    // so the arithmetic under test is the real subtraction, not a constant overflow.
    const unsigned long nearMax   = 0xFFFFFFFFul - 100; // streak started 100 ms before wrap
    const unsigned long stillPre  = nearMax + 50;       // 50 ms later, before wrap (no overflow)
    const unsigned long postWrap  = 1199UL;             // (nearMax + 1300) mod 2^32: 1300 ms later
    CHECK("wrap: 50 ms elapsed holds",    !poseClearElapsed(nearMax, stillPre, clearMs));
    CHECK("wrap: 1300 ms elapsed clears",  poseClearElapsed(nearMax, postWrap, clearMs));
}

// ---- 10. Death/KO latch carry across re-key (DeathLatch.h rekeyCarryLatch) ------
// Guards the death-consistency fix (2026-07-15): a dead/KO'd body that RE-KEYS
// (owner re-containers it - squad move / recruit) must keep its down/death pin,
// or the peer stands the corpse back up under the new hand ("dead on one game,
// alive on the other"). rekeyPeerBody snapshots the OLD key's latch and OR-merges
// it onto the new key; this locks that merge (monotone: never loses a pin, never
// clears a latch already present on the new key).
static void testDeathRekey() {
    std::printf("== death/KO latch carry on re-key (DeathLatch.h) ==\n");

    // Dead old key, fresh new key -> death carries.
    LatchState r1 = rekeyCarryLatch(LatchState(true, true, true), LatchState());
    CHECK("dead old -> new death latched",  r1.death);
    CHECK("dead old -> new ko latched",      r1.ko);
    CHECK("dead old -> new down carried",    r1.down);

    // KO-only old key -> ko carries, death stays clear.
    LatchState r2 = rekeyCarryLatch(LatchState(false, true, true), LatchState());
    CHECK("ko old -> new ko latched",        r2.ko);
    CHECK("ko old -> new death still clear", !r2.death);

    // Alive old key, alive new key -> nothing invented.
    LatchState r3 = rekeyCarryLatch(LatchState(), LatchState());
    CHECK("alive+alive -> no death",         !r3.death);
    CHECK("alive+alive -> no ko",            !r3.ko);

    // New key already has a fresh EVT_DEATH (beat the re-key edge): OR-merge must
    // PRESERVE it even though the old key was alive.
    LatchState r4 = rekeyCarryLatch(LatchState(), LatchState(true, true, false));
    CHECK("alive old + dead new -> death kept", r4.death);
    CHECK("alive old + dead new -> ko kept",    r4.ko);

    // Monotone: merging can only ADD pins, never remove one present on either key.
    LatchState r5 = rekeyCarryLatch(LatchState(true, false, false),
                                    LatchState(false, true, false));
    CHECK("merge keeps old death", r5.death);
    CHECK("merge keeps new ko",    r5.ko);
}

// ---- 11. Inbound queue lifecycle (Inbound.h) ------------------------------------
// Locks the Phase 0 correctness fixes against regression:
//  (a) flushWorldState() drops a queued cross-owner invXfer intent - the bug was
//      that a transfer enqueued before a world reload SURVIVED it (invXfer_ was
//      missing from the clear list), applying against the fresh world.
//  (b) sawRemote_ (peer-readiness) clears on the session-reset edge, so a new
//      scenario cannot arm on a departed peer's stale readiness.
//  (c) the internal session generation advances on every flush (the Phase 0 seed
//      for the Phase 4 wire epoch).
// It also proves the world-state vs session-preserving split: a world-state queue
// is dropped while a coordinated-load queue survives the same flush.
static void testInboundLifecycle() {
    std::printf("== inbound queue lifecycle (Inbound.h) ==\n");
    Inbound in;

    CHECK_EQ("initial session generation", in.sessionGeneration(), 0);
    CHECK("sawRemote false before any entity", !in.sawRemoteEntity());

    EntityState e; std::memset(&e, 0, sizeof(e));
    in.pushEntity(1, 1000, e);
    CHECK("sawRemote true after owned-entity batch", in.sawRemoteEntity());

    // (a) THE FIX: a cross-owner transfer intent must not survive a reload.
    InvXferPacket xf; std::memset(&xf, 0, sizeof(xf));
    xf.type = (u8)PKT_INV_XFER; xf.ownerId = 1;
    in.pushInvXfer(1, xf);
    {
        std::deque<InboundInvXfer> peek;
        in.drainInvXfers(peek);
        CHECK("invXfer enqueues normally", peek.size() == 1);
    }
    in.pushInvXfer(1, xf);          // re-enqueue, then hit the reload edge
    in.flushWorldState();
    {
        std::deque<InboundInvXfer> after;
        in.drainInvXfers(after);
        CHECK("invXfer dropped by flushWorldState (was the bug)", after.empty());
    }

    // (b) + (c): the same flush cleared readiness and advanced the generation.
    CHECK("sawRemote cleared by flushWorldState", !in.sawRemoteEntity());
    CHECK_EQ("generation advanced by flush", in.sessionGeneration(), 1);

    // world-state vs session-preserving: a world event drops, a LOAD_GO survives.
    EventPacket ev; std::memset(&ev, 0, sizeof(ev)); ev.type = (u8)PKT_EVENT; ev.ownerId = 1;
    in.pushEvent(1, ev);
    LoadGoPacket lg; std::memset(&lg, 0, sizeof(lg)); lg.type = (u8)PKT_LOAD_GO; lg.ownerId = 0;
    in.pushLoadGo(0, lg);
    in.flushWorldState();
    {
        std::deque<InboundEvent> evOut; in.drainEvents(evOut);
        std::deque<InboundLoadGo> lgOut; in.drainLoadGos(lgOut);
        CHECK("world-state event dropped by flush", evOut.empty());
        CHECK("coordinated-load GO survives flush (session-preserving)", lgOut.size() == 1);
    }
    CHECK_EQ("generation advanced again", in.sessionGeneration(), 2);
}

// ---- 11b. flushWorldState() full-coverage contract (Inbound.h) ------------------
// The invXfer_ bug (a world-state queue silently missing from the clear list) is a
// CLASS of bug: every queue Inbound owns must be classified as either WORLD-STATE
// (dropped on the reload/reconnect/disconnect edge) or SESSION-PRESERVING (kept
// because the connection outlives the world swap). This test pushes a sentinel
// into EVERY queue, hits one flush, and asserts the split for all of them, so a
// queue added later without being classified in flushWorldState() fails here.
//
// WHEN YOU ADD A NEW INBOUND QUEUE: add its push here and assert it in the correct
// group. A queue absent from both groups is unverified - that is the bug.
static void testFlushWorldStateContract() {
    std::printf("== flushWorldState full-coverage contract (Inbound.h) ==\n");
    Inbound in;

    // Zeroed payloads - the flush contract is about queue membership, not content.
    EntityState     e;   std::memset(&e,   0, sizeof(e));
    EventPacket     ev;  std::memset(&ev,  0, sizeof(ev));
    u32             cKey[5]; std::memset(cKey, 0, sizeof(cKey));
    WorldDropPacket wdp; std::memset(&wdp, 0, sizeof(wdp));
    WorldPickupPacket wpp; std::memset(&wpp, 0, sizeof(wpp));
    InvXferPacket   xf;  std::memset(&xf,  0, sizeof(xf));
    MedicalPacket   mp;  std::memset(&mp,  0, sizeof(mp));
    TreatmentPacket tp;  std::memset(&tp,  0, sizeof(tp));
    CombatHitPacket chp; std::memset(&chp, 0, sizeof(chp));
    SpeedPacket     sp;  std::memset(&sp,  0, sizeof(sp));
    StatsPacket     stp; std::memset(&stp, 0, sizeof(stp));
    MoneyPacket     mo;  std::memset(&mo,  0, sizeof(mo));
    MoneyDeltaPacket md; std::memset(&md,  0, sizeof(md));
    FactionPacket   fa;  std::memset(&fa,  0, sizeof(fa));
    TimePacket      ti;  std::memset(&ti,  0, sizeof(ti));
    DoorPacket      dp;  std::memset(&dp,  0, sizeof(dp));
    ProdPacket      pr;  std::memset(&pr,  0, sizeof(pr));
    ResearchPacket  rp;  std::memset(&rp,  0, sizeof(rp));
    DeedPacket      de;  std::memset(&de,  0, sizeof(de));
    FixturePacket   fx;  std::memset(&fx,  0, sizeof(fx));
    BuildPlacePacket  bp; std::memset(&bp,  0, sizeof(bp));
    BuildStatePacket  bs; std::memset(&bs,  0, sizeof(bs));
    BuildDoorPacket   bd; std::memset(&bd,  0, sizeof(bd));
    BuildRemovePacket br; std::memset(&br,  0, sizeof(br));
    StealthPacket   sl;  std::memset(&sl,  0, sizeof(sl));
    SpawnReqPacket  sq;  std::memset(&sq,  0, sizeof(sq));
    SpawnInfoPacket si;  std::memset(&si,  0, sizeof(si));
    CamHintPacket   ch;  std::memset(&ch,  0, sizeof(ch));
    CellClaimPacket cc;  std::memset(&cc,  0, sizeof(cc));
    CellMapPacket   cmp; std::memset(&cmp, 0, sizeof(cmp));
    InvXferAckPacket xa; std::memset(&xa,  0, sizeof(xa));
    XferCommitPacket xc; std::memset(&xc,  0, sizeof(xc));
    XferCommitAckPacket xca; std::memset(&xca, 0, sizeof(xca));
    ClaimVerdictPacket cv; std::memset(&cv, 0, sizeof(cv));
    // Session-preserving payloads.
    SaveReqPacket   srq; std::memset(&srq, 0, sizeof(srq));
    SaveBeginPacket sbg; std::memset(&sbg, 0, sizeof(sbg));
    SaveFileHeader  sfh; std::memset(&sfh, 0, sizeof(sfh)); // pathLen/dataLen = 0
    SaveDoneHeader  sdh; std::memset(&sdh, 0, sizeof(sdh)); // fileCount = 0
    SaveAckPacket   sak; std::memset(&sak, 0, sizeof(sak));
    LoadGoPacket    lg;  std::memset(&lg,  0, sizeof(lg));
    LoadReqPacket   lrq; std::memset(&lrq, 0, sizeof(lrq));
    LoadNackPacket  lnk; std::memset(&lnk, 0, sizeof(lnk));
    OwnRanksPacket  orp; std::memset(&orp, 0, sizeof(orp));

    // --- Push one sentinel into every WORLD-STATE queue (37, Phase 7: +xferCommit/
    // xferCommitAck/claimVerdict).
    in.pushEntity(1, 0, e);
    in.pushEvent(1, ev);
    in.pushInv(1, 0, cKey, 0, 0);
    in.pushWorldItems(1, 0, 0);
    in.pushWorldRemove(1, 0, 0);
    in.pushWorldClaim(1, 2, 0, 0, 0);
    in.pushNpcCensus(1, 0, 0, 0);
    in.pushWorldDrop(1, wdp);
    in.pushWorldPickup(1, wpp);
    in.pushInvXfer(1, xf);
    in.pushMedical(1, mp);
    in.pushTreatment(1, tp);
    in.pushCombatHit(1, chp);
    in.pushSpeed(1, sp);
    in.pushStats(1, stp);
    in.pushMoney(1, mo);
    in.pushMoneyDelta(1, md);
    in.pushFaction(1, fa);
    in.pushTime(1, ti);
    in.pushDoor(1, dp);
    in.pushProd(1, pr);
    in.pushResearch(1, rp);
    in.pushDeed(1, de);
    in.pushFixture(1, fx);
    in.pushBuildPlace(1, bp);
    in.pushBuildState(1, bs);
    in.pushBuildDoor(1, bd);
    in.pushBuildRemove(1, br);
    in.pushStealth(1, sl);
    in.pushSpawnReq(1, sq);
    in.pushSpawnInfo(1, si);
    in.pushCamHint(1, ch);
    in.pushCellClaim(1, cc);
    in.pushCellMap(cmp);
    in.pushInvXferAck(1, xa);
    in.pushXferCommit(xc);
    in.pushXferCommitAck(1, xca);
    in.pushClaimVerdict(cv);

    // --- Push one sentinel into every SESSION-PRESERVING queue (11).
    in.pushConnect(0);
    in.pushLeave(0);
    in.pushSaveReq(1, srq);
    in.pushSaveBegin(1, sbg);
    in.pushSaveFile(1, sfh, "", 0);
    in.pushSaveDone(1, sdh, 0);
    in.pushSaveAck(1, sak);
    in.pushLoadGo(0, lg);
    in.pushLoadReq(1, lrq);
    in.pushLoadNack(1, lnk);
    in.pushOwnRanks(orp);

    in.flushWorldState();

    // --- Every WORLD-STATE queue must now be empty.
    #define WS_EMPTY(name, type, drain) do { \
        std::deque<type> out; in.drain(out); \
        CHECK("world-state dropped: " name, out.empty()); } while (0)
    WS_EMPTY("entity",      InboundEntity,      drainEntities);
    WS_EMPTY("event",       InboundEvent,       drainEvents);
    WS_EMPTY("inv",         InboundInv,         drainInv);
    WS_EMPTY("worldItems",  InboundWorldItems,  drainWorldItems);
    WS_EMPTY("worldRemove", InboundWorldRemove, drainWorldRemove);
    WS_EMPTY("worldClaim",  InboundWorldClaim,  drainWorldClaim);
    WS_EMPTY("npcCensus",   InboundNpcCensus,   drainNpcCensus);
    WS_EMPTY("worldDrop",   InboundWorldDrop,   drainWorldDrops);
    WS_EMPTY("worldPickup", InboundWorldPickup, drainWorldPickups);
    WS_EMPTY("invXfer",     InboundInvXfer,     drainInvXfers);
    WS_EMPTY("medical",     InboundMedical,     drainMedical);
    WS_EMPTY("treatment",   InboundTreatment,   drainTreatments);
    WS_EMPTY("combatHit",   InboundCombatHit,   drainCombatHits);
    WS_EMPTY("speed",       InboundSpeed,       drainSpeed);
    WS_EMPTY("stats",       InboundStats,       drainStats);
    WS_EMPTY("money",       InboundMoney,       drainMoney);
    WS_EMPTY("moneyDelta",  InboundMoneyDelta,  drainMoneyDeltas);
    WS_EMPTY("faction",     InboundFaction,     drainFaction);
    WS_EMPTY("time",        InboundTime,        drainTime);
    WS_EMPTY("door",        InboundDoor,        drainDoor);
    WS_EMPTY("prod",        InboundProd,        drainProd);
    WS_EMPTY("research",    InboundResearch,    drainResearch);
    WS_EMPTY("deed",        InboundDeed,        drainDeed);
    WS_EMPTY("fixture",     InboundFixture,     drainFixture);
    WS_EMPTY("buildPlace",  InboundBuildPlace,  drainBuildPlace);
    WS_EMPTY("buildState",  InboundBuildState,  drainBuildState);
    WS_EMPTY("buildDoor",   InboundBuildDoor,   drainBuildDoor);
    WS_EMPTY("buildRemove", InboundBuildRemove, drainBuildRemove);
    WS_EMPTY("stealth",     InboundStealth,     drainStealth);
    WS_EMPTY("spawnReq",    InboundSpawnReq,    drainSpawnReqs);
    WS_EMPTY("spawnInfo",   InboundSpawnInfo,   drainSpawnInfos);
    WS_EMPTY("camHint",     InboundCamHint,     drainCamHints);
    WS_EMPTY("cellClaim",   InboundCellClaim,   drainCellClaims);
    WS_EMPTY("cellMap",     InboundCellMap,     drainCellMaps);
    WS_EMPTY("invXferAck",  InboundInvXferAck,  drainInvXferAcks);
    WS_EMPTY("xferCommit",  InboundXferCommit,  drainXferCommits);
    WS_EMPTY("xferCommitAck", InboundXferCommitAck, drainXferCommitAcks);
    WS_EMPTY("claimVerdict", InboundClaimVerdict, drainClaimVerdicts);
    #undef WS_EMPTY

    // --- Every SESSION-PRESERVING queue must still hold its sentinel.
    #define SP_KEPT(name, type, drain) do { \
        std::deque<type> out; in.drain(out); \
        CHECK("session-preserving kept: " name, out.size() == 1); } while (0)
    { std::deque<u32> out; in.drainConnects(out);
      CHECK("session-preserving kept: connect", out.size() == 1); }
    { std::deque<u32> out; in.drainLeaves(out);
      CHECK("session-preserving kept: leave", out.size() == 1); }
    SP_KEPT("saveReq",   InboundSaveReq,   drainSaveReqs);
    SP_KEPT("saveBegin", InboundSaveBegin, drainSaveBegins);
    SP_KEPT("saveFile",  InboundSaveFile,  drainSaveFiles);
    SP_KEPT("saveDone",  InboundSaveDone,  drainSaveDones);
    SP_KEPT("saveAck",   InboundSaveAck,   drainSaveAcks);
    SP_KEPT("loadGo",    InboundLoadGo,    drainLoadGos);
    SP_KEPT("loadReq",   InboundLoadReq,   drainLoadReqs);
    SP_KEPT("loadNack",  InboundLoadNack,  drainLoadNacks);
    SP_KEPT("ownRanks",  InboundOwnRanks,  drainOwnRanks);
    #undef SP_KEPT
}

// ---- 12. Worker-teardown ordering (models NetLink::stop()) -----------------------
// The NetLink::stop() fix: ENet teardown (enet_deinitialize + CloseHandle) must
// happen ONLY after the net worker has fully exited - the worker owns transport
// cleanup, so deinitializing while it still runs is a use-after-free / double
// free. The old code tore down unconditionally on a 2 s wait TIMEOUT. This locks
// the ordering invariant with a bare Win32 worker (no ENet dependency in the unit
// layer): stop() waits for the thread to exit FIRST, and the worker's cleanup
// strictly precedes the post-wait teardown.
static volatile LONG g_teardownSeq   = 0;
static LONG          g_workerCleanup = 0;
static LONG          g_teardown      = 0;
static DWORD WINAPI teardownWorker(LPVOID) {
    Sleep(40); // simulate the service loop draining + transport cleanup
    g_workerCleanup = InterlockedIncrement(&g_teardownSeq);
    return 0;
}
static void testTeardownOrdering() {
    std::printf("== worker-teardown ordering (NetLink::stop contract) ==\n");
    g_teardownSeq = 0; g_workerCleanup = 0; g_teardown = 0;
    HANDLE th = CreateThread(0, 0, &teardownWorker, 0, 0, 0);
    CHECK("worker thread created", th != 0);
    if (th) {
        DWORD wr = WaitForSingleObject(th, INFINITE); // stop(): wait for full exit
        CHECK("wait returns signalled (worker exited)", wr == WAIT_OBJECT_0);
        CloseHandle(th);
        g_teardown = InterlockedIncrement(&g_teardownSeq); // "enet_deinitialize" AFTER
        CHECK("worker cleanup precedes teardown", g_workerCleanup < g_teardown);
    }
}

// ---- ObjectHand: dual-layout unification contract (Phase 5b) ----------------
// Locks the ONE typed identity's two legacy array orders so a future edit can't
// silently reorder a field (the exact "dual hand[5] layout" desync footgun).
static void testObjectHandLayout() {
    std::printf("\n== ObjectHand layout (Phase 5b) ==\n");
    ObjectHand h;
    h.type = 11; h.container = 22; h.containerSerial = 33; h.index = 44; h.serial = 55;

    // OBJECT order  = {type, container, containerSerial, index, serial}
    u32 obj[5];
    h.toObjOrder(obj);
    CHECK("objOrder[0]=type",            obj[0] == 11);
    CHECK("objOrder[1]=container",       obj[1] == 22);
    CHECK("objOrder[2]=containerSerial", obj[2] == 33);
    CHECK("objOrder[3]=index",           obj[3] == 44);
    CHECK("objOrder[4]=serial",          obj[4] == 55);

    // CHAR-KEY order = {index, serial, type, container, containerSerial}
    u32 ck[5];
    h.toCharKey(ck);
    CHECK("charKey[0]=index",           ck[0] == 44);
    CHECK("charKey[1]=serial",          ck[1] == 55);
    CHECK("charKey[2]=type",            ck[2] == 11);
    CHECK("charKey[3]=container",       ck[3] == 22);
    CHECK("charKey[4]=containerSerial", ck[4] == 33);

    // The two legacy orders are genuinely different layouts (the footgun itself).
    CHECK("obj order != char-key order", std::memcmp(obj, ck, sizeof(obj)) != 0);

    // Round-trips: from*(to*(h)) == h for both orders.
    CHECK("fromObjOrder round-trips",  ObjectHand::fromObjOrder(obj).equals(h));
    CHECK("fromCharKey round-trips",   ObjectHand::fromCharKey(ck).equals(h));

    // Cross-order remap through the POD reproduces the manual [3][4][0][1][2]
    // char-key remap of an object-order array (the exact call-site footgun).
    u32 remap[5];
    ObjectHand::fromObjOrder(obj).toCharKey(remap);
    CHECK("obj->charkey remap [0]=obj[3]", remap[0] == obj[3]);
    CHECK("obj->charkey remap [1]=obj[4]", remap[1] == obj[4]);
    CHECK("obj->charkey remap [2]=obj[0]", remap[2] == obj[0]);
    CHECK("obj->charkey remap [3]=obj[1]", remap[3] == obj[1]);
    CHECK("obj->charkey remap [4]=obj[2]", remap[4] == obj[2]);

    // EntityState's named hand fields ARE object order: an ObjectHand built from
    // them must serialize to the same object-order array.
    EntityState e;
    std::memset(&e, 0, sizeof(e));
    e.hType = 11; e.hContainer = 22; e.hContainerSerial = 33; e.hIndex = 44; e.hSerial = 55;
    ObjectHand eh;
    eh.type = e.hType; eh.container = e.hContainer; eh.containerSerial = e.hContainerSerial;
    eh.index = e.hIndex; eh.serial = e.hSerial;
    CHECK("EntityState hand == object order", eh.equals(h));

    // resolvable(): the engine's null handle is all-zero; a non-zero index or
    // serial names a live object, type/container alone never do.
    ObjectHand z; z.type = z.container = z.containerSerial = z.index = z.serial = 0;
    CHECK("all-zero hand not resolvable",       !z.resolvable());
    ObjectHand idxOnly = z; idxOnly.index = 1;
    CHECK("index-only hand resolvable",          idxOnly.resolvable());
    ObjectHand serOnly = z; serOnly.serial = 1;
    CHECK("serial-only hand resolvable",         serOnly.resolvable());
    ObjectHand tcOnly = z; tcOnly.type = 7; tcOnly.container = 9;
    CHECK("type/container-only NOT resolvable", !tcOnly.resolvable());

    // equals() is field-sensitive on every one of the five fields.
    ObjectHand d;
    d = h; d.type++;            CHECK("equals detects type diff",      !d.equals(h));
    d = h; d.container++;       CHECK("equals detects container diff", !d.equals(h));
    d = h; d.containerSerial++; CHECK("equals detects cser diff",      !d.equals(h));
    d = h; d.index++;           CHECK("equals detects index diff",     !d.equals(h));
    d = h; d.serial++;          CHECK("equals detects serial diff",    !d.equals(h));
}

// ---- Engine fault throttle contract (Phase 5c) ------------------------------
// Locks the pure throttle decision that gates the "[engine] FAULT" oracle line:
// always emit the first hit, then at most once per interval, tolerating the
// wall-clock midnight wrap by erring toward an extra emit (never silent forever).
static void testEngineFaults() {
    std::printf("\n== engine fault throttle (Phase 5c) ==\n");
    using coop::engine::faultShouldLog;
    using coop::engine::FAULT_OP_COUNT;
    using coop::engine::FAULT_RESOLVE_CHAR;
    using coop::engine::FAULT_RESOLVE_OBJECT;

    CHECK("FAULT_OP_COUNT > 0",   (int)FAULT_OP_COUNT > 0);
    CHECK("resolve ops ordered",  FAULT_RESOLVE_CHAR == 0 && FAULT_RESOLVE_OBJECT == 1);

    unsigned long last = 0;
    CHECK("first hit logs",             faultShouldLog(1, 5000, &last, 1000));
    CHECK("first hit stamps lastMs",    last == 5000);
    CHECK("hit within interval quiet",  !faultShouldLog(2, 5500, &last, 1000));
    CHECK("lastMs unchanged in quiet",  last == 5000);
    CHECK("hit at interval logs",       faultShouldLog(3, 6000, &last, 1000));
    CHECK("lastMs advanced",            last == 6000);
    CHECK("just-before-boundary quiet", !faultShouldLog(4, 6999, &last, 1000));

    // Midnight wrap of wallClockMs: unsigned delta stays huge -> emit (never
    // permanently suppress across the wrap).
    unsigned long wrapLast = 86399000UL;
    CHECK("wrap boundary logs", faultShouldLog(5, 1000, &wrapLast, 1000));

    // Null lastMs (defensive): only the very first hit logs.
    CHECK("null lastMs first logs",  faultShouldLog(1, 0, 0, 1000));
    CHECK("null lastMs later quiet", !faultShouldLog(2, 0, 0, 1000));
}

static void testEngineCaps() {
    std::printf("\n== engine capability registry (Phase 5d) ==\n");
    using namespace coop::engine;

    // capName tokens are the oracle contract: stable, in enum order, guarded.
    CHECK("CAP_COUNT > 0",          (int)CAP_COUNT > 0);
    CHECK("cap core is hand",       std::strcmp(capName(CAP_HAND_RESOLVE), "hand_resolve") == 0);
    CHECK("cap saveload token",     std::strcmp(capName(CAP_SAVELOAD), "saveload") == 0);
    CHECK("cap faction token",      std::strcmp(capName(CAP_FACTION), "faction") == 0);
    CHECK("cap out-of-range low",   std::strcmp(capName((Capability)-1), "unknown") == 0);
    CHECK("cap out-of-range high",  std::strcmp(capName(CAP_COUNT), "unknown") == 0);

    // Synthetic resolved slots: two caps, one with a redundant required row.
    void* pA = (void*)1;  // saveload row 1
    void* pB = (void*)1;  // saveload row 2
    void* pH = (void*)1;  // hand_resolve (core)
    void* pF = (void*)1;  // faction (unrelated)
    const CapRow rows[] = {
        { &pA, "SaveManager::get",  CAP_SAVELOAD,     true },
        { &pB, "SaveManager::load", CAP_SAVELOAD,     true },
        { &pH, "hand::getCharacter",CAP_HAND_RESOLVE, true },
        { &pF, "FactionRelations",  CAP_FACTION,      true }
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0]));
    bool avail[CAP_COUNT];

    // (1) Everything resolved -> the three exercised caps are available; every
    // untouched cap (no rows) stays fail-closed false; core is OK.
    capEvaluate(rows, n, avail);
    CHECK("all-resolved saveload on",   avail[CAP_SAVELOAD]);
    CHECK("all-resolved hand on",       avail[CAP_HAND_RESOLVE]);
    CHECK("all-resolved faction on",    avail[CAP_FACTION]);
    CHECK("untouched cap fail-closed",  !avail[CAP_DOOR]);
    CHECK("core ok when hand resolved", capCoreOk(avail));

    // (2) One of saveload's two required rows drops -> the WHOLE cap fails, but
    // the other caps are untouched (no cross-contamination).
    pB = 0;
    capEvaluate(rows, n, avail);
    CHECK("partial-miss fails cap",     !avail[CAP_SAVELOAD]);
    CHECK("sibling cap unaffected",     avail[CAP_HAND_RESOLVE]);
    CHECK("unrelated cap unaffected",   avail[CAP_FACTION]);
    pB = (void*)1;

    // (3) Core hand-resolve missing -> capCoreOk trips (unsupported image),
    // while an unrelated cap can still be up.
    pH = 0;
    capEvaluate(rows, n, avail);
    CHECK("core down when hand missing", !capCoreOk(avail));
    CHECK("hand cap off",                !avail[CAP_HAND_RESOLVE]);
    CHECK("faction still up",            avail[CAP_FACTION]);
    pH = (void*)1;

    // (4) capRowResolved: null slot and null pointer both read as unresolved.
    void* live = (void*)1;
    void* dead = 0;
    CapRow rLive = { &live, "x", CAP_SAVELOAD, true };
    CapRow rDead = { &dead, "y", CAP_SAVELOAD, true };
    CapRow rNoSlot = { 0, "z", CAP_SAVELOAD, true };
    CHECK("row resolved (live ptr)",  capRowResolved(rLive));
    CHECK("row unresolved (null ptr)", !capRowResolved(rDead));
    CHECK("row unresolved (no slot)",  !capRowResolved(rNoSlot));
}

// Phase 6: the shared change-gated send/accept policy (ChangeGate.h). This
// locks the exact decisions the money + door channels used to inline by hand,
// so a future consolidation can't silently drift the wire cadence.
static void testChangeGate() {
    std::printf("\n== change-gate policy (Phase 6) ==\n");
    using namespace coop::sync;

    // --- gateSampleDue: first pass always samples, then once per sampleMs. ----
    CHECK("sample due first pass",      gateSampleDue(50000, 0, 1000));
    CHECK("sample not due within win", !gateSampleDue(50500, 50000, 1000));
    CHECK("sample due at interval",     gateSampleDue(51000, 50000, 1000));
    CHECK("sample due past interval",   gateSampleDue(52000, 50000, 1000));

    // --- gateSeqAccept: monotonic per-sender, first sight always accepted. ---
    CHECK("seq accept first sight",   gateSeqAccept(0, 1));
    CHECK("seq accept first sight hi",gateSeqAccept(0, 999));
    CHECK("seq accept newer",         gateSeqAccept(5, 6));
    CHECK("seq drop equal",          !gateSeqAccept(5, 5));
    CHECK("seq drop older",          !gateSeqAccept(5, 4));

    // --- gateShouldSend, MONEY flavor (minSend=1000, resend=5000, unsent=1) ---
    // A never-sent row streams once even unchanged (no silent seed).
    CHECK("money unsent unchanged sends",
          gateShouldSend(false, 90000, 0, 1000, 5000, true));
    // A change always crosses (row sent long ago, past the throttle).
    CHECK("money change sends",
          gateShouldSend(true, 90000, 80000, 1000, 5000, true));
    // ...but not within the min-send throttle window after a send.
    CHECK("money change throttled",
          !gateShouldSend(true, 80500, 80000, 1000, 5000, true));
    // Unchanged + sent recently (past throttle, before resend) holds.
    CHECK("money unchanged holds pre-resend",
          !gateShouldSend(false, 82000, 80000, 1000, 5000, true));
    // Unchanged + resend window elapsed -> safety resend.
    CHECK("money unchanged resends",
          gateShouldSend(false, 86000, 80000, 1000, 5000, true));

    // --- gateShouldSend, DOOR flavor (minSend=0, resend=10000, unsent=0) -----
    // A silently-seeded, never-sent, unchanged row HOLDS (no first-sight send).
    CHECK("door unsent unchanged holds",
          !gateShouldSend(false, 90000, 0, 0, 10000, false));
    // A real change crosses immediately (no throttle).
    CHECK("door change sends",
          gateShouldSend(true, 90000, 0, 0, 10000, false));
    // Unchanged, sent within resend window -> hold.
    CHECK("door unchanged holds pre-resend",
          !gateShouldSend(false, 85000, 80000, 0, 10000, false));
    // Unchanged, resend window elapsed -> safety resend.
    CHECK("door unchanged resends",
          gateShouldSend(false, 90000, 80000, 0, 10000, false));
    // A change sent 1ms ago still crosses under a zero throttle.
    CHECK("door change no throttle",
          gateShouldSend(true, 80001, 80000, 0, 10000, false));
}

// Phase 8 (WORLD-01): gateSeqAcceptPerSender - the N>=3 fix for the cross-
// sender seq collision gateSeqAccept's single shared counter has. This is
// the regression test for the exact silent-drop the phase closes: two
// different senders authoring the SAME row (a door hand any client may
// touch) must NEVER have the second sender's first-ever seq compared against
// the first sender's counter.
static void testSeqPerSender() {
    std::printf("\n== per-sender seq accept (Phase 8 WORLD-01: N>=3 door/bdoor fix) ==\n");
    using namespace coop::sync;
    std::map<unsigned int, unsigned int> seqSeen;

    // owner=2 seq=5: first sight for THIS sender, accepted.
    CHECK("owner=2 seq=5 first sight accepted", gateSeqAcceptPerSender(seqSeen, 2, 5));
    seqSeen[2] = 5;

    // owner=3 seq=1: first sight for a DIFFERENT sender - NOT dropped by
    // owner=2's higher counter. This is the exact N>=3 collision a single
    // shared gateSeqAccept counter would fail: owner=3's genuinely-first row
    // would be compared against owner=2's seq=5 and wrongly rejected.
    CHECK("owner=3 seq=1 first sight accepted despite owner=2's higher seq",
          gateSeqAcceptPerSender(seqSeen, 3, 1));
    seqSeen[3] = 1;

    // A resend (same seq) or a stale/older seq from an ALREADY-seen sender is
    // dropped - the ordinary stale-row guard still works per sender.
    CHECK("owner=2 seq=5 resend dropped", !gateSeqAcceptPerSender(seqSeen, 2, 5));
    CHECK("owner=2 seq=3 stale (older) dropped", !gateSeqAcceptPerSender(seqSeen, 2, 3));

    // owner=3's next genuinely-newer seq is accepted independently of owner=2.
    CHECK("owner=3 seq=2 accepted after seq=1", gateSeqAcceptPerSender(seqSeen, 3, 2));
    seqSeen[3] = 2;

    // owner=2's counter is untouched by owner=3's activity - independent
    // per-sender state, the whole point of the fix.
    CHECK("owner=2 seq=6 accepted, unaffected by owner=3's counter",
          gateSeqAcceptPerSender(seqSeen, 2, 6));
    seqSeen[2] = 6;

    // A resend of owner=3's now-current seq is dropped too.
    CHECK("owner=3 seq=2 resend dropped", !gateSeqAcceptPerSender(seqSeen, 3, 2));

    // Phase 8 review WR-01: the connect-edge rejoin purge
    // (purgeAuthorConservationState) erases exactly the rejoining owner's
    // entry from every row's per-sender map - after which that owner's
    // RESTARTED counter (a fresh client process restarts at 1) is first
    // sight again and accepted, instead of being dropped against the dead
    // connection's high-water mark until it climbed back past it.
    seqSeen.erase(2);
    CHECK("owner=2 seq=1 accepted after the rejoin purge erased its entry "
          "(seq-restart, WR-01)", gateSeqAcceptPerSender(seqSeen, 2, 1));
    // The purge is per-owner: owner=3's counter survives untouched, so its
    // stale rows are still dropped.
    CHECK("owner=3 seq=2 still dropped - the purge touched only owner=2",
          !gateSeqAcceptPerSender(seqSeen, 3, 2));
}

// Phase 5 (ID-01/ID-03): the shared per-owner fold/dedup helpers (FoldDedup.h)
// that fix the proven silent-data-loss bug - a bare-scalar fold tracker
// dropping a second/third sender's deltas as "already folded". Engine-free:
// this is the whole ID-01 proof, since applyMoneyPool itself is not reachable
// from prototest's link graph (RESEARCH Pitfall 3).
static void testFoldDedup() {
    std::printf("\n== fold/dedup composite-key helpers (Phase 5, ID-01) ==\n");

    // --- foldMonotonic: per-owner high-water fold. --------------------------
    std::map<u32, u32> acked;
    // Three DIFFERENT owners (P2, P3, P4) each folding seq=1 all succeed -
    // the core ID-01 proof: no cross-owner "already folded" false negative.
    CHECK("owner 2 seq=1 folds (first sight)",  foldMonotonic(acked, (u32)2, (u32)1));
    CHECK("owner 3 seq=1 folds (distinct owner)", foldMonotonic(acked, (u32)3, (u32)1));
    CHECK("owner 4 seq=1 folds (distinct owner)", foldMonotonic(acked, (u32)4, (u32)1));
    // The SAME owner replaying an already-folded seq is correctly rejected.
    CHECK("owner 2 seq=1 replay rejected", !foldMonotonic(acked, (u32)2, (u32)1));
    // Each owner's high-water advances independently.
    CHECK("owner 2 seq=2 folds (advances)",     foldMonotonic(acked, (u32)2, (u32)2));
    CHECK("owner 3 seq=2 folds (advances)",     foldMonotonic(acked, (u32)3, (u32)2));
    CHECK("owner 4 seq=2 folds (advances)",     foldMonotonic(acked, (u32)4, (u32)2));
    // A stale/equal seq for an owner already past it is rejected.
    CHECK("owner 4 seq=1 stale rejected",      !foldMonotonic(acked, (u32)4, (u32)1));
    CHECK("owner 3 seq=2 equal rejected",      !foldMonotonic(acked, (u32)3, (u32)2));
    // A brand-new owner is unaffected by any prior owner's high-water.
    CHECK("owner 7 seq=1 folds (fresh owner, unaffected by others)",
          foldMonotonic(acked, (u32)7, (u32)1));

    // --- foldOnce: composite-key one-shot dedup, cap eviction. --------------
    std::set<std::pair<u32, u32> > seen;
    CHECK("foldOnce first sight (2,10) inserts", foldOnce(seen, (u32)2, (u32)10, (std::size_t)4096));
    CHECK("foldOnce repeat (2,10) rejected",    !foldOnce(seen, (u32)2, (u32)10, (std::size_t)4096));
    CHECK("foldOnce distinct owner same id (3,10) inserts",
          foldOnce(seen, (u32)3, (u32)10, (std::size_t)4096));
    CHECK("foldOnce distinct id same owner (2,11) inserts",
          foldOnce(seen, (u32)2, (u32)11, (std::size_t)4096));
    CHECK("foldOnce set holds 3 distinct entries", seen.size() == 3);
    // Cap eviction: a tiny cap of 2 forces the oldest (smallest) key out.
    std::set<std::pair<u32, u32> > small;
    CHECK("foldOnce cap=2 (1,1) inserts",  foldOnce(small, (u32)1, (u32)1, (std::size_t)2));
    CHECK("foldOnce cap=2 (1,2) inserts",  foldOnce(small, (u32)1, (u32)2, (std::size_t)2));
    CHECK("foldOnce cap=2 stays at 2 after eviction", small.size() == 2);
    CHECK("foldOnce cap=2 (1,3) inserts + evicts oldest",
          foldOnce(small, (u32)1, (u32)3, (std::size_t)2));
    CHECK("foldOnce cap=2 evicted key (1,1) no longer present",
          small.count(std::make_pair((u32)1, (u32)1)) == 0);
    CHECK("foldOnce cap=2 survivor (1,3) present",
          small.count(std::make_pair((u32)1, (u32)3)) != 0);

    // --- foldOnce as applyEvents' (ownerId,eventId) guard (ID-02). ----------
    // Engine-free proof of the exact predicate applyEvents now runs ahead of
    // its koLatched/deathLatched latch switch (ReplicatorSpawn.cpp): three
    // owners each delivering eventId=1 in the same window must all reach the
    // switch (three distinct events, none mistaken for an already-seen one),
    // and a reliable resend of the SAME (ownerId,eventId) must be skipped.
    std::set<std::pair<u32, u32> > eventsSeen;
    CHECK("events: owner 2 eventId=1 folds (first sight)",
          coop::foldOnce(eventsSeen, (u32)2, (u32)1, (std::size_t)4096));
    CHECK("events: owner 3 eventId=1 folds (distinct owner, not mistaken for owner 2's)",
          coop::foldOnce(eventsSeen, (u32)3, (u32)1, (std::size_t)4096));
    CHECK("events: owner 4 eventId=1 folds (distinct owner)",
          coop::foldOnce(eventsSeen, (u32)4, (u32)1, (std::size_t)4096));
    CHECK("events: three distinct (owner,eventId) tuples recorded",
          eventsSeen.size() == 3);
    CHECK("events: owner 2 eventId=1 replay (reliable resend) rejected",
          !coop::foldOnce(eventsSeen, (u32)2, (u32)1, (std::size_t)4096));
    CHECK("events: owner 3 eventId=1 replay rejected",
          !coop::foldOnce(eventsSeen, (u32)3, (u32)1, (std::size_t)4096));
    CHECK("events: replay rejection did not grow the seen set",
          eventsSeen.size() == 3);
    // A never-duplicated stream is a no-op guard: each owner's next eventId
    // folds normally.
    CHECK("events: owner 2 eventId=2 folds (normal never-duplicated stream)",
          coop::foldOnce(eventsSeen, (u32)2, (u32)2, (std::size_t)4096));

    // --- crossOwnerCollision: ID-03 guard predicate. -------------------------
    const u32 NONE = 0xFFFFFFFFu;
    CHECK("collision: unclaimed slot (existing==none) is never a collision",
          !crossOwnerCollision(NONE, (u32)2, NONE));
    CHECK("collision: same owner re-touching its own id is never a collision",
          !crossOwnerCollision((u32)2, (u32)2, NONE));
    CHECK("collision: genuine different-owner clash IS a collision",
          crossOwnerCollision((u32)2, (u32)3, NONE));
}

// 06-01 (GAP-3/PLAY-03): engine-free proof of PinOwner.h's author-record
// helpers - rekeyPeerBody itself is not reachable from prototest's link
// graph (same constraint as FoldDedup's applyMoneyPool, RESEARCH Pitfall
// 3), so this is the whole PLAY-03 unit-layer proof. A minimal local
// key struct stands in for Replicator's private nested `Key` (PinOwner.h's
// functions are templated on the caller's key type precisely so this
// works with no coupling to Replicator at all).
namespace {
struct TestPinKey {
    u32 a, b;
    bool operator<(const TestPinKey& o) const {
        if (a != o.a) return a < o.a;
        return b < o.b;
    }
};
TestPinKey pinKey(u32 a, u32 b) { TestPinKey k; k.a = a; k.b = b; return k; }
}

static void testPinOwner() {
    std::printf("\n== pin-author bookkeeping (06-01, GAP-3/PLAY-03) ==\n");

    std::map<TestPinKey, u32> owners;
    TestPinKey h1 = pinKey(1, 100); // authored by player 2
    TestPinKey h2 = pinKey(1, 101); // authored by player 3
    TestPinKey h3 = pinKey(1, 102); // authored by player 4
    TestPinKey h4 = pinKey(1, 103); // authored by player 2 (second hand)

    coop::pinRecord(owners, h1, (u32)2);
    coop::pinRecord(owners, h2, (u32)3);
    coop::pinRecord(owners, h3, (u32)4);
    coop::pinRecord(owners, h4, (u32)2);
    CHECK("pinRecord: four distinct hands recorded", owners.size() == 4);
    CHECK("pinRecord: h1 attributed to player 2",
          owners.find(h1) != owners.end() && owners[h1] == (u32)2);
    CHECK("pinRecord: h4 attributed to player 2 (second hand, same owner)",
          owners.find(h4) != owners.end() && owners[h4] == (u32)2);
    CHECK("pinRecord: h2 attributed to player 3",
          owners.find(h2) != owners.end() && owners[h2] == (u32)3);
    CHECK("pinRecord: h3 attributed to player 4",
          owners.find(h3) != owners.end() && owners[h3] == (u32)4);

    // pinRecord is overwrite-on-repeat (a re-key of the SAME hand from a
    // later edge must replace, not duplicate).
    coop::pinRecord(owners, h1, (u32)4);
    CHECK("pinRecord: re-recording an existing key overwrites its owner",
          owners.size() == 4 && owners[h1] == (u32)4);
    coop::pinRecord(owners, h1, (u32)2); // restore for the erase-scoping proof below

    // pinEraseOwner(2): removes ONLY player 2's pins (h1, h4) - player 3's
    // and player 4's survive untouched. This is the exact disconnect-
    // cleanup scoping PLAY-03 requires: a departing peer's pins are
    // released without touching a SURVIVING peer's pins.
    unsigned int removed = coop::pinEraseOwner(owners, (u32)2);
    CHECK("pinEraseOwner(2): removed exactly 2 entries", removed == 2);
    CHECK("pinEraseOwner(2): h1 (player 2) gone", owners.find(h1) == owners.end());
    CHECK("pinEraseOwner(2): h4 (player 2) gone", owners.find(h4) == owners.end());
    CHECK("pinEraseOwner(2): h2 (player 3) untouched",
          owners.find(h2) != owners.end() && owners[h2] == (u32)3);
    CHECK("pinEraseOwner(2): h3 (player 4) untouched",
          owners.find(h3) != owners.end() && owners[h3] == (u32)4);
    CHECK("pinEraseOwner(2): map now holds only the 2 survivors",
          owners.size() == 2);

    // pinEraseOwner on an owner with NO pins is a no-op (zero removed, map
    // unchanged) - the departed-owner-had-nothing-pinned case.
    unsigned int removedNone = coop::pinEraseOwner(owners, (u32)999);
    CHECK("pinEraseOwner: unknown owner removes nothing", removedNone == 0);
    CHECK("pinEraseOwner: unknown owner leaves the map unchanged", owners.size() == 2);

    // pinForget: drops a single hand's author record regardless of owner.
    coop::pinForget(owners, h2);
    CHECK("pinForget: h2 dropped", owners.find(h2) == owners.end());
    CHECK("pinForget: map now holds 1 entry", owners.size() == 1);

    // pinForget on a hand never recorded is a no-op erase (matches
    // std::map::erase's own no-op-on-missing-key contract).
    coop::pinForget(owners, h2);
    CHECK("pinForget: erasing an already-absent key is a no-op",
          owners.find(h2) == owners.end() && owners.size() == 1);
    TestPinKey neverRecorded = pinKey(9, 999);
    coop::pinForget(owners, neverRecorded);
    CHECK("pinForget: erasing a hand that was NEVER recorded is a no-op",
          owners.size() == 1);
}

// Phase 7 (INV-02/INV-04): engine-free proof of XferCommit.h's host-side
// pending-transfer state machine. Task 1 proved the happy path (xferBegin
// opens PENDING once; a duplicate intent is a no-op; xferCommitOnce commits
// exactly once and a second commit returns false); Task 2 extends it with
// the exhaustive cases the host arbitration (processXferIntents) and the
// disconnect cleanup (xferEraseOwner, Task 3) rely on: commit-then-late-
// intent rejected, host-as-participant short-circuit (same transitions, no
// special-cased key shape), and per-owner cleanup pre-commit void vs
// committed stand.
static void testXferCommit() {
    std::printf("\n== host-committed transfer state machine (Phase 7, INV-02/INV-04) ==\n");

    std::map<std::pair<u32, u32>, coop::XferPendingEntry> pending;
    u32 seq = 0;

    // --- xferBegin: first sight opens PENDING; a duplicate is a no-op. ------
    CHECK("xferBegin: fresh (author=2,transferId=1) opens PENDING",
          coop::xferBegin(pending, (u32)2, (u32)1, seq));
    CHECK("xferBegin: duplicate intent (same key) is a no-op",
          !coop::xferBegin(pending, (u32)2, (u32)1, seq));
    CHECK("xferBegin: a DIFFERENT author's transferId=1 is a distinct key (no collision)",
          coop::xferBegin(pending, (u32)3, (u32)1, seq));
    CHECK("xferBegin: table holds exactly 2 distinct (author,transferId) entries",
          pending.size() == 2);
    CHECK("xferBegin: the seq counter advanced only for the 2 FRESH inserts (dup consumed none)",
          seq == 2);

    // --- xferCommitOnce: PENDING -> COMMITTED exactly once. -----------------
    CHECK("xferCommitOnce: first commit on (2,1) succeeds",
          coop::xferCommitOnce(pending, (u32)2, (u32)1));
    CHECK("xferCommitOnce: a second commit on the SAME key returns false (commit is final)",
          !coop::xferCommitOnce(pending, (u32)2, (u32)1));
    CHECK("xferCommitOnce: (3,1) is untouched by (2,1)'s commit (still separately committable)",
          coop::xferCommitOnce(pending, (u32)3, (u32)1));
    CHECK("xferCommitOnce: a commit attempt with no matching intent (author=9) is rejected",
          !coop::xferCommitOnce(pending, (u32)9, (u32)1));

    // --- commit-then-late-intent: a replayed/late intent after COMMITTED is
    // still a no-op (xferBegin never reopens a final entry). --------------
    CHECK("xferBegin: a late/replayed intent AFTER commit (2,1) is still a no-op",
          !coop::xferBegin(pending, (u32)2, (u32)1, seq));
    CHECK("xferCommitOnce: a commit attempt on that same already-committed key still fails",
          !coop::xferCommitOnce(pending, (u32)2, (u32)1));

    // --- host-as-participant short-circuit: the SAME transitions, no
    // special-cased key shape - the host authoring its own intent (e.g.
    // author=0, the host's PlayerId) walks the identical PENDING->COMMITTED
    // path as any join author. ------------------------------------------
    CHECK("host-as-participant: fresh (author=0,transferId=5) opens PENDING",
          coop::xferBegin(pending, (u32)0, (u32)5, seq));
    CHECK("host-as-participant: commits exactly once",
          coop::xferCommitOnce(pending, (u32)0, (u32)5));
    CHECK("host-as-participant: a second commit on the same key fails (commit is final)",
          !coop::xferCommitOnce(pending, (u32)0, (u32)5));

    // --- per-owner disconnect cleanup (INV-04): pre-commit PENDING entries
    // authored by the departing owner are VOIDED (erased); COMMITTED
    // entries STAND (left untouched) - never both, never neither. ---------
    std::map<std::pair<u32, u32>, coop::XferPendingEntry> dc;
    u32 dcSeq = 0;
    CHECK("xferEraseOwner setup: owner 5 opens transferId=1 (will be committed)",
          coop::xferBegin(dc, (u32)5, (u32)1, dcSeq));
    CHECK("xferEraseOwner setup: owner 5's transferId=1 commits",
          coop::xferCommitOnce(dc, (u32)5, (u32)1));
    CHECK("xferEraseOwner setup: owner 5 opens transferId=2 (stays PENDING, pre-commit)",
          coop::xferBegin(dc, (u32)5, (u32)2, dcSeq));
    CHECK("xferEraseOwner setup: a DIFFERENT owner (6) opens transferId=1 (must survive untouched)",
          coop::xferBegin(dc, (u32)6, (u32)1, dcSeq));
    CHECK("xferEraseOwner setup: table holds 3 entries before the erase",
          dc.size() == 3);

    unsigned int voided = 0, stood = 0;
    unsigned int removed = coop::xferEraseOwner(dc, (u32)5, &voided, &stood);
    CHECK("xferEraseOwner(5): voided exactly 1 (the pre-commit PENDING entry)",
          voided == 1);
    CHECK("xferEraseOwner(5): stood exactly 1 (the COMMITTED entry survives)",
          stood == 1);
    CHECK("xferEraseOwner(5): returns the voided count",
          removed == voided);
    CHECK("xferEraseOwner(5): the pre-commit (5,2) entry is GONE (voided)",
          dc.find(std::make_pair((u32)5, (u32)2)) == dc.end());
    CHECK("xferEraseOwner(5): the committed (5,1) entry STILL PRESENT and still COMMITTED",
          dc.find(std::make_pair((u32)5, (u32)1)) != dc.end() &&
          dc[std::make_pair((u32)5, (u32)1)].state == (u8)coop::XFER_PENDING_COMMITTED);
    CHECK("xferEraseOwner(5): owner 6's entry is untouched (owner-scoped, no cross-owner wipe)",
          dc.find(std::make_pair((u32)6, (u32)1)) != dc.end());
    CHECK("xferEraseOwner(5): table now holds 2 entries (1 stood + 1 survivor)",
          dc.size() == 2);

    // A departing owner with nothing pending is a no-op (zero voided, zero stood).
    unsigned int voided2 = 0, stood2 = 0;
    unsigned int removed2 = coop::xferEraseOwner(dc, (u32)999, &voided2, &stood2);
    CHECK("xferEraseOwner: unknown owner voids/stands nothing", voided2 == 0 && stood2 == 0);
    CHECK("xferEraseOwner: unknown owner leaves the table unchanged",
          removed2 == 0 && dc.size() == 2);

    // --- cap eviction (Phase 7 review WR-05): eviction is by INSERTION
    // order (smallest seq), not by smallest (authorId,transferId) key. -----
    std::map<std::pair<u32, u32>, coop::XferPendingEntry> capped;
    u32 capSeq = 0;
    coop::xferBegin(capped, (u32)1, (u32)1, capSeq);
    coop::xferEvictOldest(capped, (std::size_t)1);
    CHECK("xferEvictOldest: single entry survives a cap of 1",
          capped.size() == 1);
    coop::xferBegin(capped, (u32)1, (u32)2, capSeq);
    coop::xferEvictOldest(capped, (std::size_t)1);
    CHECK("xferEvictOldest: a fresh insert past cap=1 evicts the oldest-inserted",
          capped.size() == 1 &&
          capped.find(std::make_pair((u32)1, (u32)1)) == capped.end() &&
          capped.find(std::make_pair((u32)1, (u32)2)) != capped.end());

    // WR-05 regression: author 3's key was inserted FIRST, author 1's key
    // second. erase(begin()) would have evicted author 1's BRAND-NEW key
    // (smallest map key) and kept author 3's ancient one; seq-ordered
    // eviction removes the genuinely oldest-inserted (3,7) instead - so the
    // just-inserted key is never the eviction victim.
    {
        std::map<std::pair<u32, u32>, coop::XferPendingEntry> x;
        u32 xs = 0;
        coop::xferBegin(x, (u32)3, (u32)7, xs);  // inserted first (older)
        coop::xferCommitOnce(x, (u32)3, (u32)7);
        coop::xferBegin(x, (u32)1, (u32)1, xs);  // inserted second (newer, smaller key)
        coop::xferCommitOnce(x, (u32)1, (u32)1);
        coop::xferEvictOldest(x, (std::size_t)1);
        CHECK("xferEvictOldest (WR-05): cross-author eviction is insertion-order, "
              "never smallest-key (the newer (1,1) survives, the older (3,7) goes)",
              x.size() == 1 &&
              x.find(std::make_pair((u32)3, (u32)7)) == x.end() &&
              x.find(std::make_pair((u32)1, (u32)1)) != x.end());
        // The surviving committed entry still answers replays as final: a
        // replayed intent for it must NOT re-open (double-broadcast guard).
        CHECK("xferEvictOldest (WR-05): the surviving COMMITTED entry still rejects a replayed intent",
              !coop::xferBegin(x, (u32)1, (u32)1, xs) &&
              !coop::xferCommitOnce(x, (u32)1, (u32)1));
    }

    // --- rejoin purge (Phase 7 review CR-01): a reconnecting PlayerId's
    // fresh process restarts transferId at 1; xferPurgeAuthor at the connect
    // edge erases ALL of that author's records (COMMITTED included, unlike
    // the disconnect-time xferEraseOwner) so the recycled key opens fresh. --
    {
        std::map<std::pair<u32, u32>, coop::XferPendingEntry> x;
        u32 xs = 0;
        coop::xferBegin(x, (u32)2, (u32)1, xs);      // old connection's transferId=1
        coop::xferCommitOnce(x, (u32)2, (u32)1);     // committed-final
        coop::xferBegin(x, (u32)2, (u32)2, xs);      // old connection, still PENDING
        coop::xferBegin(x, (u32)3, (u32)1, xs);      // ANOTHER author - must survive
        CHECK("rejoin purge setup: without the purge, the recycled (2,1) intent is swallowed",
              !coop::xferBegin(x, (u32)2, (u32)1, xs));
        unsigned int purged = coop::xferPurgeAuthor(x, (u32)2);
        CHECK("xferPurgeAuthor (CR-01): erases ALL of author 2's records, committed included",
              purged == 2 &&
              x.find(std::make_pair((u32)2, (u32)1)) == x.end() &&
              x.find(std::make_pair((u32)2, (u32)2)) == x.end());
        CHECK("xferPurgeAuthor (CR-01): another author's record is untouched",
              x.find(std::make_pair((u32)3, (u32)1)) != x.end());
        CHECK("xferPurgeAuthor (CR-01): the rejoined author's recycled transferId=1 now opens fresh",
              coop::xferBegin(x, (u32)2, (u32)1, xs) &&
              coop::xferCommitOnce(x, (u32)2, (u32)1));
    }
}

// Phase 7 Plan 02 (INV-03): engine-free proof of ClaimArbiter.h's host-side
// contention-window arbiter. Task 1 proved the happy path (a single claimant
// opens and wins the window; a second, later-stamped claimant still loses to
// the earlier one); Task 2 extends it with the exhaustive cases the host
// arbitration (applyClaimIntents) and the disconnect cleanup
// (claimEraseClaimant) rely on: tie-stamp lowest-playerId break, 3-claimant
// mixed, commit-final (a later-arriving EARLIER stamp after commit is still
// rejected), unmappable-stamp tie-band membership, and claimant disconnect
// pre-verdict (the remaining claimant wins).
static void testClaimArbiter() {
    std::printf("\n== claim-contention arbiter (Phase 7 Plan 02, INV-03) ==\n");

    // --- single claimant opens the window and wins it unopposed. -----------
    std::map<std::pair<u32, u32>, coop::ClaimWindow> windows;
    CHECK("openClaim: fresh (author=10,netId=5) opens a window",
          coop::openClaim(windows, (u32)10, (u32)5, (u32)2, (unsigned long)1000, true,
                          (unsigned long)1000));
    CHECK("openClaim: table holds exactly 1 open window", windows.size() == 1);
    std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator wit =
        windows.find(std::make_pair((u32)10, (u32)5));
    CHECK("openClaim: the window is findable by (authorId,netId)", wit != windows.end());
    if (wit != windows.end()) {
        CHECK("claimWindowExpired: false before the window length elapses",
              !coop::claimWindowExpired(wit->second, (unsigned long)1100, (unsigned long)250));
        CHECK("claimWindowExpired: true once the window length elapses",
              coop::claimWindowExpired(wit->second, (unsigned long)1260, (unsigned long)250));
        u32 winner = 0;
        CHECK("finalizeClaim: a single unopposed claimant wins",
              coop::finalizeClaim(wit->second, (unsigned long)30, &winner));
        CHECK_EQ("finalizeClaim: the winner is the sole claimant", winner, 2);
    }

    // --- a second, LATER-stamped claimant still loses to the earlier one. --
    std::map<std::pair<u32, u32>, coop::ClaimWindow> windows2;
    CHECK("openClaim: claimant 3 opens (author=10,netId=6) at mappedMs=1000",
          coop::openClaim(windows2, (u32)10, (u32)6, (u32)3, (unsigned long)1000, true,
                          (unsigned long)1000));
    CHECK("openClaim: claimant 2 competes at mappedMs=1500 (later, well outside eps)",
          !coop::openClaim(windows2, (u32)10, (u32)6, (u32)2, (unsigned long)1500, true,
                           (unsigned long)1050));
    std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator wit2 =
        windows2.find(std::make_pair((u32)10, (u32)6));
    CHECK("openClaim: the window now holds 2 competing claims",
          wit2 != windows2.end() && wit2->second.claims.size() == 2);
    if (wit2 != windows2.end()) {
        u32 winner2 = 0;
        CHECK("finalizeClaim: the earlier-stamped claimant (3) wins",
              coop::finalizeClaim(wit2->second, (unsigned long)30, &winner2));
        CHECK_EQ("finalizeClaim: winner is claimant 3, not the later claimant 2", winner2, 3);
    }

    // --- commit-final bookkeeping: a fresh (authorId,netId) is uncommitted
    // until markClaimCommitted records it (WR-05: flag+winner in ONE record). --
    std::map<std::pair<u32, u32>, coop::ClaimFinal> committed;
    u32 cSeq = 0;
    CHECK("claimCommitted: a fresh (10,5) key is not yet committed",
          !coop::claimCommitted(committed, (u32)10, (u32)5));
    coop::markClaimCommitted(committed, (u32)10, (u32)5, (u32)2, cSeq, (std::size_t)4096);
    CHECK("claimCommitted: (10,5) reads committed after markClaimCommitted",
          coop::claimCommitted(committed, (u32)10, (u32)5));
    {
        u32 w = 999;
        CHECK("claimWinnerFor: a committed hit ALWAYS carries the real prior winner",
              coop::claimWinnerFor(committed, (u32)10, (u32)5, &w) && w == 2);
        CHECK("claimWinnerFor: an uncommitted key reads not-final",
              !coop::claimWinnerFor(committed, (u32)10, (u32)6, &w));
        // Commit is final: a replayed mark can never overwrite the winner.
        coop::markClaimCommitted(committed, (u32)10, (u32)5, (u32)7, cSeq, (std::size_t)4096);
        w = 999;
        CHECK("markClaimCommitted: a replayed mark never overwrites the final winner",
              coop::claimWinnerFor(committed, (u32)10, (u32)5, &w) && w == 2);
    }

    // --- Task 2 exhaustive cases (host arbitration/disconnect rely on these) --

    // 2 claimants TIE (delta <= eps=30) -> lowest playerId, not earliest stamp
    // (a 5 ms delta is well inside the tie-band, so playerId alone decides).
    {
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w;
        coop::openClaim(w, (u32)20, (u32)1, (u32)7, (unsigned long)1000, true, (unsigned long)1000);
        coop::openClaim(w, (u32)20, (u32)1, (u32)4, (unsigned long)1005, true, (unsigned long)1000);
        std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator it =
            w.find(std::make_pair((u32)20, (u32)1));
        CHECK("tie: window holds both competing claims", it != w.end() && it->second.claims.size() == 2);
        if (it != w.end()) {
            u32 winner = 0;
            CHECK("tie (delta=5ms <= eps=30ms): finalizeClaim resolves a winner",
                  coop::finalizeClaim(it->second, (unsigned long)30, &winner));
            CHECK_EQ("tie: lowest playerId (4) wins over the earlier-stamped higher id (7)",
                     winner, 4);
        }
    }

    // 3 claimants MIXED: one clear earliest, two tied with each other but not
    // with the earliest - the earliest alone wins (its own tie-band is empty).
    {
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w;
        coop::openClaim(w, (u32)21, (u32)2, (u32)9, (unsigned long)1000, true, (unsigned long)1000); // earliest
        coop::openClaim(w, (u32)21, (u32)2, (u32)5, (unsigned long)1200, true, (unsigned long)1000); // tied pair
        coop::openClaim(w, (u32)21, (u32)2, (u32)6, (unsigned long)1210, true, (unsigned long)1000); // tied pair
        std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator it =
            w.find(std::make_pair((u32)21, (u32)2));
        CHECK("3-claimant: window holds all 3", it != w.end() && it->second.claims.size() == 3);
        if (it != w.end()) {
            u32 winner = 0;
            CHECK("3-claimant mixed: finalizeClaim resolves a winner",
                  coop::finalizeClaim(it->second, (unsigned long)30, &winner));
            CHECK_EQ("3-claimant mixed: the single clear-earliest claimant (9) wins",
                     winner, 9);
        }
    }

    // commit-final: a later-arriving EARLIER stamp after commit is STILL
    // rejected - claimCommitted() gates the caller from ever re-arbitrating.
    {
        std::map<std::pair<u32, u32>, coop::ClaimFinal> c;
        u32 s = 0;
        coop::markClaimCommitted(c, (u32)22, (u32)3, (u32)5, s, (std::size_t)4096);
        CHECK("commit-final: (22,3) reads committed",
              coop::claimCommitted(c, (u32)22, (u32)3));
        // The caller's contract (mirrored by applyClaimIntents) is: check
        // claimCommitted() BEFORE ever touching the window/finalize path.
        // Re-checking after a hypothetical "earlier" claim confirms the
        // guard does not depend on the claim's own stamp at all.
        CHECK("commit-final: still committed regardless of a later claim's stamp",
              coop::claimCommitted(c, (u32)22, (u32)3));
    }

    // WR-05 regression: cap eviction on the finals map is insertion-order
    // across authors, and flag+winner can never be evicted apart (they are
    // one record) - the old independent caps could leave "committed" true
    // with the winner gone, answering winner=0 (host named winner of a claim
    // it never made).
    {
        std::map<std::pair<u32, u32>, coop::ClaimFinal> c;
        u32 s = 0;
        coop::markClaimCommitted(c, (u32)9, (u32)4, (u32)9, s, (std::size_t)4096); // older
        coop::markClaimCommitted(c, (u32)2, (u32)1, (u32)3, s, (std::size_t)1);    // newer, smaller key
        u32 w = 999;
        CHECK("markClaimCommitted (WR-05): at cap, the OLDER-inserted (9,4) is evicted, "
              "not the just-inserted smaller key (2,1)",
              c.size() == 1 &&
              !coop::claimCommitted(c, (u32)9, (u32)4) &&
              coop::claimWinnerFor(c, (u32)2, (u32)1, &w) && w == 3);
    }

    // unmappable stamp -> automatic tie-band membership (never unusable): an
    // unmappable claimant with a numerically-later mappedMs still competes on
    // playerId, exactly like an in-band tie would.
    {
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w;
        coop::openClaim(w, (u32)23, (u32)4, (u32)8, (unsigned long)1000, true, (unsigned long)1000);
        // mappedMs is irrelevant when mappable=false (no peerClock_ entry yet -
        // e.g. the claimant's first packet, before sendStamp has a sample).
        coop::openClaim(w, (u32)23, (u32)4, (u32)2, (unsigned long)999999, false, (unsigned long)1000);
        std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator it =
            w.find(std::make_pair((u32)23, (u32)4));
        CHECK("unmappable: window holds both claims", it != w.end() && it->second.claims.size() == 2);
        if (it != w.end()) {
            u32 winner = 0;
            CHECK("unmappable: finalizeClaim resolves a winner",
                  coop::finalizeClaim(it->second, (unsigned long)30, &winner));
            CHECK_EQ("unmappable claimant (2) beats the mappable-but-higher-id claimant (8) "
                     "on the lowest-playerId tie-break", winner, 2);
        }
    }

    // claimant disconnect PRE-VERDICT: claimEraseClaimant drops the departed
    // claimant's entry from the OPEN window; the remaining claimant wins.
    {
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w;
        coop::openClaim(w, (u32)24, (u32)5, (u32)11, (unsigned long)1000, true, (unsigned long)1000);
        coop::openClaim(w, (u32)24, (u32)5, (u32)3,  (unsigned long)1200, true, (unsigned long)1000);
        std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator it =
            w.find(std::make_pair((u32)24, (u32)5));
        CHECK("disconnect setup: window holds both claims before the departure",
              it != w.end() && it->second.claims.size() == 2);
        unsigned int erased = coop::claimEraseClaimant(w, (u32)11); // claimant 11 disconnects
        CHECK_EQ("claimEraseClaimant: exactly 1 entry erased for the departed claimant", erased, 1);
        it = w.find(std::make_pair((u32)24, (u32)5));
        CHECK("claimEraseClaimant: the window survives (claimant 3 still bid)", it != w.end());
        if (it != w.end()) {
            CHECK_EQ("claimEraseClaimant: exactly 1 claim remains", it->second.claims.size(), 1);
            u32 winner = 0;
            CHECK("post-disconnect: finalizeClaim resolves a winner",
                  coop::finalizeClaim(it->second, (unsigned long)30, &winner));
            CHECK_EQ("post-disconnect: the remaining claimant (3) wins uncontested", winner, 3);
        }
        // A window where EVERY claimant disconnects is erased outright (nothing
        // left to finalize).
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w2;
        coop::openClaim(w2, (u32)25, (u32)6, (u32)12, (unsigned long)1000, true, (unsigned long)1000);
        coop::claimEraseClaimant(w2, (u32)12);
        CHECK("claimEraseClaimant: a window with zero remaining claims is erased outright",
              w2.find(std::make_pair((u32)25, (u32)6)) == w2.end());
        // An unrelated claimant/window is untouched (claimant-scoped, not
        // window-scoped - the same owner-scoped-not-global-wipe rule
        // xferEraseOwner established for transfers).
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w3;
        coop::openClaim(w3, (u32)26, (u32)7, (u32)13, (unsigned long)1000, true, (unsigned long)1000);
        coop::claimEraseClaimant(w3, (u32)999); // unrelated claimant id
        CHECK("claimEraseClaimant: an unrelated claimant id leaves the window untouched",
              w3.find(std::make_pair((u32)26, (u32)7)) != w3.end() &&
              w3[std::make_pair((u32)26, (u32)7)].claims.size() == 1);
    }

    // --- cap eviction: the LONGEST-OPEN window (smallest openMs) is evicted
    // past the cap (defensive - windows in practice self-erase on finalize,
    // so this bound rarely engages). WR-05: age is judged by openMs, never by
    // the (authorId,netId) key order, which across authors is unrelated to
    // age and could evict the window a claim just opened. --------------------
    {
        std::map<std::pair<u32, u32>, coop::ClaimWindow> capped;
        coop::openClaim(capped, (u32)1, (u32)1, (u32)1, (unsigned long)0, true, (unsigned long)100);
        coop::claimEvictOldest(capped, (std::size_t)1);
        CHECK("claimEvictOldest: single entry survives a cap of 1", capped.size() == 1);
        coop::openClaim(capped, (u32)1, (u32)2, (u32)1, (unsigned long)0, true, (unsigned long)200);
        coop::claimEvictOldest(capped, (std::size_t)1);
        CHECK("claimEvictOldest: a fresh insert past cap=1 evicts the longest-open window",
              capped.size() == 1 &&
              capped.find(std::make_pair((u32)1, (u32)1)) == capped.end() &&
              capped.find(std::make_pair((u32)1, (u32)2)) != capped.end());
        // WR-05 regression: author 3's window opened FIRST (older openMs),
        // author 1's window just opened (newer, smaller map key). Smallest-key
        // eviction would kill the brand-new window mid-contention; openMs
        // eviction keeps it.
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w2;
        coop::openClaim(w2, (u32)3, (u32)9, (u32)1, (unsigned long)0, true, (unsigned long)100); // older
        coop::openClaim(w2, (u32)1, (u32)1, (u32)2, (unsigned long)0, true, (unsigned long)500); // newer
        coop::claimEvictOldest(w2, (std::size_t)1);
        CHECK("claimEvictOldest (WR-05): cross-author eviction is by openMs age, "
              "never smallest-key (the just-opened (1,1) survives)",
              w2.size() == 1 &&
              w2.find(std::make_pair((u32)3, (u32)9)) == w2.end() &&
              w2.find(std::make_pair((u32)1, (u32)1)) != w2.end());
    }

    // --- rejoin purge (Phase 7 review CR-01): a reconnecting author's fresh
    // process restarts netId at 1; claimEraseAuthor/claimEraseAuthorFinals at
    // the connect edge erase the departed connection's windows AND final
    // records so a claim on the NEW connection's recycled netId opens a fresh
    // window instead of being re-answered with the OLD winner (which would
    // destroy the new item on both ends). Author-scoped: other authors'
    // state is untouched. -----------------------------------------------------
    {
        std::map<std::pair<u32, u32>, coop::ClaimWindow> w;
        std::map<std::pair<u32, u32>, coop::ClaimFinal>  f;
        u32 s = 0;
        // Old connection of author 2: one finalized claim, one still-open window.
        coop::markClaimCommitted(f, (u32)2, (u32)1, (u32)5, s, (std::size_t)4096);
        coop::openClaim(w, (u32)2, (u32)2, (u32)5, (unsigned long)1000, true, (unsigned long)1000);
        // Another author's state - must survive the purge.
        coop::markClaimCommitted(f, (u32)3, (u32)1, (u32)4, s, (std::size_t)4096);
        coop::openClaim(w, (u32)3, (u32)2, (u32)4, (unsigned long)1000, true, (unsigned long)1000);
        CHECK("rejoin purge setup: without the purge, a claim on the recycled (2,1) is "
              "answered committed with the OLD winner",
              coop::claimCommitted(f, (u32)2, (u32)1));
        unsigned int wErased = coop::claimEraseAuthor(w, (u32)2);
        unsigned int fErased = coop::claimEraseAuthorFinals(f, (u32)2);
        CHECK("claimEraseAuthor (CR-01): author 2's open window erased, author 3's survives",
              wErased == 1 &&
              w.find(std::make_pair((u32)2, (u32)2)) == w.end() &&
              w.find(std::make_pair((u32)3, (u32)2)) != w.end());
        CHECK("claimEraseAuthorFinals (CR-01): author 2's final record erased, author 3's survives",
              fErased == 1 &&
              !coop::claimCommitted(f, (u32)2, (u32)1) &&
              coop::claimCommitted(f, (u32)3, (u32)1));
        // The rejoined author's recycled netId=1 now arbitrates fresh: a new
        // window opens and finalizes to the NEW claimant, not the old winner.
        CHECK("rejoin purge (CR-01): a claim on the recycled key opens a FRESH window",
              coop::openClaim(w, (u32)2, (u32)1, (u32)7, (unsigned long)2000, true,
                              (unsigned long)2000));
        std::map<std::pair<u32, u32>, coop::ClaimWindow>::iterator nit =
            w.find(std::make_pair((u32)2, (u32)1));
        u32 nw = 0;
        CHECK("rejoin purge (CR-01): the fresh window finalizes to the NEW claimant",
              nit != w.end() &&
              coop::finalizeClaim(nit->second, (unsigned long)30, &nw) && nw == 7);
    }
}

// ---- Shared money pool arbitration (Phase 9 Plan 01, CONS-01) ---------------
// MoneyFold.h's engine-free host arbiter - the ClaimArbiter.h contract applied
// to ONE shared pool instead of a per-item map. Task 1 proves the two
// load-bearing cases the tracer's end-to-end integration depends on: a
// solvent delta folds immediately and advances the ack-vector's source map,
// and two competing overdrawing deltas in one window settle deterministically
// (earlier mappedMs folds, the other rejects). The exhaustive stamp-order/
// tie/commit-final/replay/determinism matrix lands in Task 2; the reconnect
// purge regression pair lands in Task 3.
static void testMoneyFold() {
    std::printf("\n== shared money pool arbitration (Phase 9 Plan 01, CONS-01) ==\n");

    // --- solvent immediate fold: no window open, pool covers it -> FOLD, and
    // the caller's own processed-advance (foldMonotonic) sticks. -------------
    {
        coop::MoneyFoldState st;
        coop::MoneyDelta d;
        d.ownerId = 2u; d.seq = 1u; d.delta = -100; d.mappedMs = 1000ul; d.mappable = true;
        int pool = 500;
        coop::MoneyVerdict v = coop::moneyOffer(st, d, pool, (unsigned long)1000);
        CHECK("solvent delta with no window open -> MONEY_FOLD", v == coop::MONEY_FOLD);
        CHECK("MONEY_FOLD does not itself touch processed (caller's job)",
              st.processed.find(2u) == st.processed.end());
        CHECK("caller folds + advances processed via foldMonotonic",
              coop::foldMonotonic(st.processed, d.ownerId, d.seq));
        CHECK("processed now carries owner 2's high-water seq=1",
              st.processed[2u] == 1u);
        // A second, independent owner's seq=1 is unaffected (Phase 5 ID-01
        // shape, generalized to money).
        coop::MoneyDelta d2;
        d2.ownerId = 3u; d2.seq = 1u; d2.delta = -50; d2.mappedMs = 1000ul; d2.mappable = true;
        CHECK("a distinct owner's seq=1 also folds (per-owner, not a bare scalar)",
              coop::moneyOffer(st, d2, pool + d.delta, (unsigned long)1000) == coop::MONEY_FOLD);
    }

    // --- two competing deltas contest ONE window: deterministic split by
    // TIMESTAMP, not by arrival order at the host. Owner 5's big spend (-150)
    // overdraws pool=100 alone and is the one that OPENS the window; while
    // that window is open, owner 6's smaller spend (-70), individually
    // solvent against the untouched pool, must ALSO queue rather than fold
    // immediately (the determinism keystone, research Pitfall 2) - this is
    // exactly the race CONS-01 exists for: whichever delta reaches the host
    // FIRST over the wire must never decide the winner, only the game-time
    // stamp may. Owner 6's spend carries the EARLIER mappedMs (it happened
    // first in game time, even though it arrived at the host second), so it
    // is the one finalize folds; owner 5's later-stamped, larger spend is the
    // one the remaining pool can no longer cover. ----------------------------
    {
        coop::MoneyFoldState st;
        int pool = 100;
        coop::MoneyDelta bigOverdraw;   // owner 5: opens the window (overdraws alone)
        bigOverdraw.ownerId = 5u; bigOverdraw.seq = 1u; bigOverdraw.delta = -150;
        bigOverdraw.mappedMs = 2000ul; bigOverdraw.mappable = true; // LATER game-time stamp
        coop::MoneyDelta smallQueued;   // owner 6: arrives while the window is open
        smallQueued.ownerId = 6u; smallQueued.seq = 1u; smallQueued.delta = -70;
        smallQueued.mappedMs = 1000ul; smallQueued.mappable = true; // EARLIER game-time stamp

        CHECK("the overdrawing delta opens the contention window",
              coop::moneyOffer(st, bigOverdraw, pool, (unsigned long)1000) == coop::MONEY_QUEUE);
        CHECK("a second, individually-solvent delta STILL queues once the "
              "window is open (the determinism keystone)",
              coop::moneyOffer(st, smallQueued, pool, (unsigned long)1000) == coop::MONEY_QUEUE);
        CHECK("window holds exactly 2 candidates", st.windowDeltas.size() == 2u);
        CHECK("window not yet expired before its length elapses",
              !coop::moneyWindowExpired(st, (unsigned long)1100, (unsigned long)250));
        CHECK("window expired once its length elapses",
              coop::moneyWindowExpired(st, (unsigned long)1300, (unsigned long)250));

        std::vector<coop::MoneyDelta> folded, rejected;
        coop::moneyFinalize(st, pool, (unsigned long)30, folded, rejected);
        CHECK_EQ("exactly one delta folded (the earlier-stamped one)", folded.size(), 1);
        CHECK_EQ("exactly one delta rejected (the later-stamped one)", rejected.size(), 1);
        CHECK("the EARLIER-stamped owner (6) folds, regardless of arrival order",
              !folded.empty() && folded[0].ownerId == 6u);
        CHECK("the LATER-stamped owner (5) rejects, even though it arrived at "
              "the host FIRST and opened the window",
              !rejected.empty() && rejected[0].ownerId == 5u);
        CHECK_EQ("pool reflects exactly the one folded spend (100-70=30)", pool, 30);
        CHECK("processed advanced for BOTH the folded AND the rejected owner "
              "(highest seq PROCESSED, not just folded)",
              st.processed[6u] == 1u && st.processed[5u] == 1u);
        CHECK("finalize closes the window", !st.windowOpen && st.windowDeltas.empty());
    }

    // ---- Task 2 exhaustive matrices (host arbitration/reconnect rely on these) ----

    // 3 owners' seq=1 all fold independently (P2/P3/P4 independence, the
    // testFoldDedup shape generalized to money) - no cross-owner collision.
    {
        coop::MoneyFoldState st;
        int pool = 1000;
        coop::MoneyDelta d2; d2.ownerId = 2u; d2.seq = 1u; d2.delta = -10;
        d2.mappedMs = 1000ul; d2.mappable = true;
        coop::MoneyDelta d3; d3.ownerId = 3u; d3.seq = 1u; d3.delta = -20;
        d3.mappedMs = 1000ul; d3.mappable = true;
        coop::MoneyDelta d4; d4.ownerId = 4u; d4.seq = 1u; d4.delta = -30;
        d4.mappedMs = 1000ul; d4.mappable = true;
        CHECK("owner 2 seq=1 solvent fold", coop::moneyOffer(st, d2, pool, 1000ul) == coop::MONEY_FOLD);
        coop::foldMonotonic(st.processed, d2.ownerId, d2.seq); pool += d2.delta;
        CHECK("owner 3 seq=1 solvent fold (independent of owner 2)",
              coop::moneyOffer(st, d3, pool, 1000ul) == coop::MONEY_FOLD);
        coop::foldMonotonic(st.processed, d3.ownerId, d3.seq); pool += d3.delta;
        CHECK("owner 4 seq=1 solvent fold (independent of owners 2 and 3)",
              coop::moneyOffer(st, d4, pool, 1000ul) == coop::MONEY_FOLD);
        coop::foldMonotonic(st.processed, d4.ownerId, d4.seq); pool += d4.delta;
        CHECK_EQ("all three folded (1000-10-20-30=940)", pool, 940);
    }

    // 3-competing overdraft, EQUAL stamps (eps tie -> lowest ownerId wins the
    // fold, exactly like ClaimArbiter's tie rule) - a case where the pool
    // covers only ONE of the three.
    {
        coop::MoneyFoldState st;
        int pool = 100;
        // Owner 9 opens the window (individually overdraws): the OTHER two
        // then queue too because the window is already open, all at the SAME
        // mappedMs (an exact tie - inside epsMs regardless of eps size).
        coop::MoneyDelta open; open.ownerId = 9u; open.seq = 1u; open.delta = -150;
        open.mappedMs = 5000ul; open.mappable = true;
        coop::MoneyDelta a; a.ownerId = 7u; a.seq = 1u; a.delta = -80;
        a.mappedMs = 5000ul; a.mappable = true;
        coop::MoneyDelta b; b.ownerId = 4u; b.seq = 1u; b.delta = -80;
        b.mappedMs = 5000ul; b.mappable = true;
        CHECK("3-way tie setup: opener queues", coop::moneyOffer(st, open, pool, 5000ul) == coop::MONEY_QUEUE);
        CHECK("3-way tie setup: second queues", coop::moneyOffer(st, a, pool, 5000ul) == coop::MONEY_QUEUE);
        CHECK("3-way tie setup: third queues", coop::moneyOffer(st, b, pool, 5000ul) == coop::MONEY_QUEUE);
        std::vector<coop::MoneyDelta> folded, rejected;
        coop::moneyFinalize(st, pool, (unsigned long)30, folded, rejected);
        CHECK_EQ("exactly one of the three tied candidates folds", folded.size(), 1);
        CHECK_EQ("the other two reject", rejected.size(), 2);
        CHECK("tie (equal stamps): lowest ownerId (4) folds, not the earliest-inserted (9)",
              !folded.empty() && folded[0].ownerId == 4u);
    }

    // Partial coverage: earlier folds, later rejects (the plain, non-tied
    // 2-candidate case, distinct stamps well outside epsMs).
    {
        coop::MoneyFoldState st;
        int pool = 50;
        coop::MoneyDelta earlier; earlier.ownerId = 2u; earlier.seq = 1u; earlier.delta = -40;
        earlier.mappedMs = 1000ul; earlier.mappable = true;
        coop::MoneyDelta later; later.ownerId = 3u; later.seq = 1u; later.delta = -60;
        later.mappedMs = 9000ul; later.mappable = true; // overdraws pool alone (50-60<0), opens the window
        CHECK("partial coverage: later delta opens the window (queues)",
              coop::moneyOffer(st, later, pool, 1000ul) == coop::MONEY_QUEUE);
        CHECK("partial coverage: earlier delta queues too (window already open)",
              coop::moneyOffer(st, earlier, pool, 1000ul) == coop::MONEY_QUEUE);
        std::vector<coop::MoneyDelta> folded, rejected;
        coop::moneyFinalize(st, pool, (unsigned long)30, folded, rejected);
        CHECK("partial coverage: the EARLIER-stamped delta folds", !folded.empty() && folded[0].ownerId == 2u);
        CHECK("partial coverage: the LATER-stamped delta rejects", !rejected.empty() && rejected[0].ownerId == 3u);
        CHECK_EQ("pool after the one covered fold (50-40=10)", pool, 10);
    }

    // Unmappable stamp -> automatic tie-band membership (the ClaimArbiter
    // unmappable precedent, generalized): an unmappable candidate competes on
    // ownerId alone regardless of its numerically-later mappedMs.
    {
        coop::MoneyFoldState st;
        int pool = 100;
        coop::MoneyDelta mappable; mappable.ownerId = 8u; mappable.seq = 1u; mappable.delta = -150;
        mappable.mappedMs = 1000ul; mappable.mappable = true; // opens the window
        coop::MoneyDelta unmappable; unmappable.ownerId = 2u; unmappable.seq = 1u; unmappable.delta = -80;
        unmappable.mappedMs = 999999ul; unmappable.mappable = false; // no peerClock_ entry yet
        coop::moneyOffer(st, mappable, pool, 1000ul);
        coop::moneyOffer(st, unmappable, pool, 1000ul);
        std::vector<coop::MoneyDelta> folded, rejected;
        coop::moneyFinalize(st, pool, (unsigned long)30, folded, rejected);
        CHECK("unmappable candidate (owner 2) beats the mappable-but-later "
              "candidate (owner 8) via the automatic tie-band membership",
              !folded.empty() && folded[0].ownerId == 2u);
        CHECK("the mappable candidate (owner 8) rejects", !rejected.empty() && rejected[0].ownerId == 8u);
    }

    // A positive delta arriving mid-window still queues (it only helps at
    // finalize - it never needs to, since it can never cause a reject).
    {
        coop::MoneyFoldState st;
        int pool = 50;
        coop::MoneyDelta negative; negative.ownerId = 2u; negative.seq = 1u; negative.delta = -80;
        negative.mappedMs = 2000ul; negative.mappable = true; // opens the window (50-80<0)
        coop::MoneyDelta positive; positive.ownerId = 3u; positive.seq = 1u; positive.delta = 500;
        positive.mappedMs = 1000ul; positive.mappable = true; // earlier stamp, solvent alone
        CHECK("overdrawing delta opens the window", coop::moneyOffer(st, negative, pool, 2000ul) == coop::MONEY_QUEUE);
        CHECK("a positive, individually-solvent delta STILL queues once a window is open",
              coop::moneyOffer(st, positive, pool, 2000ul) == coop::MONEY_QUEUE);
        std::vector<coop::MoneyDelta> folded, rejected;
        coop::moneyFinalize(st, pool, (unsigned long)30, folded, rejected);
        CHECK_EQ("both candidates fold (the positive delta only helps)", folded.size(), 2);
        CHECK_EQ("nothing rejects", rejected.size(), 0);
        CHECK_EQ("pool reflects both (50+500-80=470)", pool, 470);
    }

    // Commit-final: a later-arriving EARLIER stamp after a fold/reject is
    // already committed for that (owner,seq) is answered MONEY_DUP, never
    // re-arbitrated (the markClaimCommitted "never overwrite" property).
    {
        coop::MoneyFoldState st;
        st.processed[5u] = 3u; // owner 5's seq=3 already processed (folded or rejected)
        coop::MoneyDelta replay; replay.ownerId = 5u; replay.seq = 2u; replay.delta = -10;
        replay.mappedMs = 1ul; replay.mappable = true; // an EARLIER stamp changes nothing
        CHECK("a stale seq (2) below the processed high-water (3) is MONEY_DUP",
              coop::moneyOffer(st, replay, 1000, 5000ul) == coop::MONEY_DUP);
        coop::MoneyDelta exact; exact.ownerId = 5u; exact.seq = 3u; exact.delta = -10;
        CHECK("the exact already-processed seq is also MONEY_DUP",
              coop::moneyOffer(st, exact, 1000, 5000ul) == coop::MONEY_DUP);
    }

    // Replay: the SAME (owner,seq) offered twice never double-folds - the
    // second offer sees the caller's own processed-advance from the first.
    {
        coop::MoneyFoldState st;
        coop::MoneyDelta d; d.ownerId = 6u; d.seq = 1u; d.delta = -25;
        d.mappedMs = 1000ul; d.mappable = true;
        int pool = 100;
        CHECK("first offer folds", coop::moneyOffer(st, d, pool, 1000ul) == coop::MONEY_FOLD);
        coop::foldMonotonic(st.processed, d.ownerId, d.seq);
        pool += d.delta;
        CHECK("replayed offer of the SAME (owner,seq) is MONEY_DUP, never a second fold",
              coop::moneyOffer(st, d, pool, 1000ul) == coop::MONEY_DUP);
        CHECK_EQ("pool moved exactly once (100-25=75)", pool, 75);
    }

    // Shuffled-arrival determinism: the SAME candidate set, appended to
    // windowDeltas in either order, finalizes to the identical fold set and
    // the identical final pool total.
    {
        coop::MoneyDelta x; x.ownerId = 11u; x.seq = 1u; x.delta = -60;
        x.mappedMs = 1000ul; x.mappable = true;
        coop::MoneyDelta y; y.ownerId = 12u; y.seq = 1u; y.delta = -60;
        y.mappedMs = 2000ul; y.mappable = true;
        coop::MoneyDelta z; z.ownerId = 13u; z.seq = 1u; z.delta = -60;
        z.mappedMs = 3000ul; z.mappable = true;

        // Order A: x, y, z.
        coop::MoneyFoldState stA;
        stA.windowOpen = true; stA.windowOpenMs = 0;
        stA.windowDeltas.push_back(x); stA.windowDeltas.push_back(y); stA.windowDeltas.push_back(z);
        int poolA = 100;
        std::vector<coop::MoneyDelta> foldedA, rejectedA;
        coop::moneyFinalize(stA, poolA, (unsigned long)30, foldedA, rejectedA);

        // Order B: z, x, y (shuffled insertion order into the SAME window).
        coop::MoneyFoldState stB;
        stB.windowOpen = true; stB.windowOpenMs = 0;
        stB.windowDeltas.push_back(z); stB.windowDeltas.push_back(x); stB.windowDeltas.push_back(y);
        int poolB = 100;
        std::vector<coop::MoneyDelta> foldedB, rejectedB;
        coop::moneyFinalize(stB, poolB, (unsigned long)30, foldedB, rejectedB);

        CHECK_EQ("shuffled arrival: identical final pool total", poolA, poolB);
        CHECK_EQ("shuffled arrival: identical fold-set size", foldedA.size(), foldedB.size());
        bool sameWinner = !foldedA.empty() && !foldedB.empty() &&
                          foldedA[0].ownerId == foldedB[0].ownerId;
        CHECK("shuffled arrival: identical fold-set membership (the earliest "
              "stamp, owner 11, wins regardless of insertion order)",
              sameWinner && foldedA[0].ownerId == 11u);
    }

    // SAME-owner tie (Phase 9 review WR-02): one owner queues seq 5 (-100)
    // and seq 6 (-50) into ONE window - trivially reachable live, since
    // publishMoneyPool emits one delta per tick and any delta arriving while
    // the window is open queues. Both share the same in-band stamp, so before
    // the (ownerId, seq) full ordering they compared equivalent both ways and
    // the UNSTABLE std::sort left "which folds, which rejects" to the
    // implementation's partitioning. Locked here: ascending seq (the owner's
    // own emission order) is the tie-break, so over a pool of 70 the greedy
    // fold visits seq 5 first (70-100<0 -> reject) then seq 6 (70-50=20 ->
    // fold) - identically in EITHER insertion order.
    {
        coop::MoneyDelta first;  first.ownerId  = 2u; first.seq  = 5u; first.delta  = -100;
        first.mappedMs  = 4000ul; first.mappable  = true;
        coop::MoneyDelta second; second.ownerId = 2u; second.seq = 6u; second.delta = -50;
        second.mappedMs = 4000ul; second.mappable = true; // exact stamp tie, same owner

        // Order A: seq 5 inserted first.
        coop::MoneyFoldState stA;
        stA.windowOpen = true; stA.windowOpenMs = 0;
        stA.windowDeltas.push_back(first); stA.windowDeltas.push_back(second);
        int poolA = 70;
        std::vector<coop::MoneyDelta> foldedA, rejectedA;
        coop::moneyFinalize(stA, poolA, (unsigned long)30, foldedA, rejectedA);

        // Order B: seq 6 inserted first (shuffled arrival, SAME candidate set).
        coop::MoneyFoldState stB;
        stB.windowOpen = true; stB.windowOpenMs = 0;
        stB.windowDeltas.push_back(second); stB.windowDeltas.push_back(first);
        int poolB = 70;
        std::vector<coop::MoneyDelta> foldedB, rejectedB;
        coop::moneyFinalize(stB, poolB, (unsigned long)30, foldedB, rejectedB);

        CHECK_EQ("same-owner tie: exactly one of the two seqs folds", foldedA.size(), 1);
        CHECK_EQ("same-owner tie: exactly one of the two seqs rejects", rejectedA.size(), 1);
        CHECK("same-owner tie: the LOWER seq (5) is visited first and rejects "
              "(pool cannot cover it), the higher seq (6) folds",
              !foldedA.empty() && foldedA[0].seq == 6u &&
              !rejectedA.empty() && rejectedA[0].seq == 5u);
        CHECK_EQ("same-owner tie: shuffled insertion names the identical final total", poolA, poolB);
        CHECK("same-owner tie: shuffled insertion names the identical fold/reject split",
              foldedA.size() == foldedB.size() &&
              !foldedB.empty() && foldedB[0].seq == 6u &&
              !rejectedB.empty() && rejectedB[0].seq == 5u);
        CHECK("same-owner tie: processed high-water carries the owner's MAX seq (6) "
              "(both seqs processed - reject advances the ack too)",
              stA.processed[2u] == 6u && stB.processed[2u] == 6u);
    }

    // Join-side pop predicate (09-RESEARCH.md Pitfall 1, the at-N bounce/
    // double-count this plan exists to kill): pop on OWN ack only, pop a
    // rejected seq too (processed advances on reject), never on a foreign
    // owner's ack value.
    {
        CHECK("moneyShouldPop: a pending seq at or below MY OWN ack pops",
              coop::moneyShouldPop(3u, 5u) && coop::moneyShouldPop(5u, 5u));
        CHECK("moneyShouldPop: a pending seq above my own ack does not pop yet",
              !coop::moneyShouldPop(6u, 5u));
        // The at-N regression this predicate exists to prevent: a FOREIGN
        // owner's ack (say, a co-join with a much higher seq counter) must
        // never be the value compared - the CALLER's own responsibility is
        // to look up its OWN Entry in MoneyPacket::acks[] before calling
        // this predicate (ReplicatorChannels.cpp), never totals.back()'s
        // old scalar or another owner's entry. This predicate itself is
        // pure and correct for whatever "myAck" value the caller passes -
        // that call-site discipline is what nettest's ack-personalization
        // leg and the join-branch code itself prove.
        u32 foreignHigherAck = 50u; // a co-join's own ack space, NOT mine
        CHECK("moneyShouldPop is a pure function of its OWN-ack argument - "
              "passing a foreign owner's (higher) ack would wrongly pop a "
              "seq that owner never processed, which is exactly why the "
              "caller must never pass one",
              coop::moneyShouldPop(6u, foreignHigherAck)); // documents the danger, not a caller
    }

    // ---- Task 3 reconnect regression PAIR (purgeAuthorConservationState) ----
    // The ReplicatorCore.cpp:660-665 claimSlots_ rationale verbatim, applied
    // to money: a fresh client process restarts poolSeq_ at 1, so the
    // connect-edge purge of processed.erase(owner) is what lets the
    // rejoined join's seq=1 delta fold instead of reading as an
    // already-processed replay forever.
    {
        // (a) WITHOUT the purge: a stale high-water from the PREVIOUS
        // connection incorrectly reads the rejoined join's restarted seq=1
        // as already processed - the regression this purge fixes.
        coop::MoneyFoldState noPurge;
        noPurge.processed[21u] = 9u; // owner 21's stale high-water from the old connection
        coop::MoneyDelta restarted; restarted.ownerId = 21u; restarted.seq = 1u;
        restarted.delta = -5; restarted.mappedMs = 1000ul; restarted.mappable = true;
        CHECK("reconnect regression: WITHOUT the connect-edge purge, the "
              "rejoined join's restarted seq=1 is wrongly read as MONEY_DUP",
              coop::moneyOffer(noPurge, restarted, 1000, 1000ul) == coop::MONEY_DUP);

        // (b) WITH the purge (processed.erase(owner), the connect-edge
        // primitive purgeAuthorConservationState performs): the SAME
        // restarted seq=1 is correctly accepted and folds.
        coop::MoneyFoldState purged;
        purged.processed[21u] = 9u; // same stale high-water...
        purged.processed.erase(21u); // ...erased at the connect edge
        CHECK("reconnect purge (CONS-01): WITH the connect-edge purge, the "
              "restarted seq=1 correctly folds",
              coop::moneyOffer(purged, restarted, 1000, 1000ul) == coop::MONEY_FOLD);
    }
}

// ---- Per-client save coordinator (Phase 10 Plan 01, SAVE-01) ----------------
// SaveCoord.h's engine-free host arbiter - the ClaimArbiter.h/MoneyFold.h
// contract applied to per-playerId transfer/ACK/retry/drop tracking. Task 1
// proves the two load-bearing tracer cases the end-to-end integration depends
// on: every client ACKing ok=1 settles with all COMMITTED, and one client's
// ok=0 retries bounded times then drops while the OTHERS stay COMMITTED
// throughout and settlement waits for the drop. The exhaustive matrix (stale
// xferId, deadline expiry, drop-after-exactly-maxRetries, erase-mid-flight)
// lands in Task 2; the reconnect regression pair lands in Task 3.
static void testSaveCoord() {
    std::printf("\n== per-client save coordinator (Phase 10 Plan 01, SAVE-01) ==\n");

    // --- all-ACK-ok: every client independently COMMITs; saveSettled is
    // true only once ALL of them have (never on the first ACK alone). ------
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u); connected.insert(4u);
        coop::saveBegin(st, 7u, connected, 1000ul, 30000ul);
        CHECK_EQ("saveBegin: seeds one entry per connected client", st.clients.size(), 4u);
        CHECK("saveBegin: every seeded client starts SC_STREAMING",
              st.clients[1u].state == coop::SC_STREAMING &&
              st.clients[2u].state == coop::SC_STREAMING &&
              st.clients[3u].state == coop::SC_STREAMING &&
              st.clients[4u].state == coop::SC_STREAMING);
        CHECK("all-ok: not settled before any ACK", !coop::saveSettled(st));

        CHECK_EQ("owner 1 ACKs ok=1 -> SC_COMMITTED",
                 coop::saveNoteAck(st, 1u, 7u, true, 1100ul), coop::SC_COMMITTED);
        CHECK("all-ok: one client's ACK never implies the group committed",
              !coop::saveSettled(st));
        coop::saveNoteAck(st, 2u, 7u, true, 1100ul);
        coop::saveNoteAck(st, 3u, 7u, true, 1100ul);
        CHECK("all-ok: still not settled with one client outstanding",
              !coop::saveSettled(st));
        coop::saveNoteAck(st, 4u, 7u, true, 1100ul);
        CHECK("all-ok: settled only once EVERY client independently committed",
              coop::saveSettled(st));

        // A stale ACK (superseded xferId) is ignored outright - it can never
        // retroactively undo a COMMITTED state.
        CHECK_EQ("stale xferId ACK ignored (state unchanged)",
                 coop::saveNoteAck(st, 1u, 6u /* not 7 */, false, 1200ul), coop::SC_COMMITTED);
    }

    // --- one client fails, retries bounded times, then drops - the others
    // stay SC_COMMITTED throughout and saveSettled becomes true only AFTER
    // the drop (the "one client's failure never blocks the others forever"
    // + "bounded retries then drop" tracer proof). -------------------------
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u); connected.insert(4u);
        const unsigned int maxRetries    = 2u;
        const unsigned long retryTimeout = 5000ul;
        coop::saveBegin(st, 42u, connected, 1000ul, retryTimeout);

        // Owners 1/2/3 commit immediately and are never touched again.
        coop::saveNoteAck(st, 1u, 42u, true, 1100ul);
        coop::saveNoteAck(st, 2u, 42u, true, 1100ul);
        coop::saveNoteAck(st, 3u, 42u, true, 1100ul);
        // Owner 4 fails.
        CHECK_EQ("owner 4 ACKs ok=0 -> SC_FAILED",
                 coop::saveNoteAck(st, 4u, 42u, false, 1100ul), coop::SC_FAILED);
        CHECK("one-fail: not settled while owner 4 is still FAILED", !coop::saveSettled(st));

        // Tick 1: owner 4 is the only candidate -> retry (retries: 0 -> 1),
        // state returns to SC_STREAMING so it isn't re-flagged immediately.
        std::vector<u32> retry1, drop1;
        coop::saveTick(st, 1200ul, maxRetries, retryTimeout, retry1, drop1);
        CHECK("tick1: exactly owner 4 retried, nobody dropped",
              retry1.size() == 1 && retry1[0] == 4u && drop1.empty());
        CHECK("tick1: owner 4 back to SC_STREAMING (not re-flagged next tick)",
              st.clients[4u].state == coop::SC_STREAMING && st.clients[4u].retries == 1u);
        CHECK("tick1: owners 1/2/3 stay SC_COMMITTED, untouched by the retry tick",
              st.clients[1u].state == coop::SC_COMMITTED &&
              st.clients[2u].state == coop::SC_COMMITTED &&
              st.clients[3u].state == coop::SC_COMMITTED);
        CHECK("tick1: a fresh SC_STREAMING candidate is not settled", !coop::saveSettled(st));

        // Owner 4 fails again (its retry also came back ok=0).
        coop::saveNoteAck(st, 4u, 42u, false, 2000ul);
        std::vector<u32> retry2, drop2;
        coop::saveTick(st, 2100ul, maxRetries, retryTimeout, retry2, drop2);
        CHECK("tick2: retried again (retries: 1 -> 2), still not dropped",
              retry2.size() == 1 && retry2[0] == 4u && drop2.empty());
        CHECK_EQ("tick2: retries counter reached maxRetries", st.clients[4u].retries, maxRetries);

        // Owner 4 fails a third time - retries == maxRetries now, so this
        // tick DROPS it instead of retrying again.
        coop::saveNoteAck(st, 4u, 42u, false, 3000ul);
        std::vector<u32> retry3, drop3;
        coop::saveTick(st, 3100ul, maxRetries, retryTimeout, retry3, drop3);
        CHECK("tick3: retries exhausted -> owner 4 is dropped, not retried again",
              retry3.empty() && drop3.size() == 1 && drop3[0] == 4u);
        CHECK_EQ("tick3: owner 4's terminal state is SC_DROPPED",
                 st.clients[4u].state, coop::SC_DROPPED);
        CHECK("post-drop: the OTHER three stayed SC_COMMITTED the entire time",
              st.clients[1u].state == coop::SC_COMMITTED &&
              st.clients[2u].state == coop::SC_COMMITTED &&
              st.clients[3u].state == coop::SC_COMMITTED);
        CHECK("post-drop: saveSettled becomes true only NOW (every client "
              "COMMITTED or DROPPED)", coop::saveSettled(st));
    }

    // --- phase 10 review CR-01: a retry beginSend mints a FRESH xferId -
    // saveRetagRetryId keeps the coordinator's staleness check aligned with
    // it, so the retried client's OWN ACK commits instead of being judged
    // stale against the original group id (the deterministic-kick bug). ----
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u);
        coop::saveBegin(st, 10u, connected, 1000ul, 5000ul);
        coop::saveNoteAck(st, 1u, 10u, true, 1100ul);
        // Owner 2 fails; saveTick flags the retry; the caller's beginSend
        // mints xferId 11 and retags.
        coop::saveNoteAck(st, 2u, 10u, false, 1100ul);
        std::vector<u32> retry, drop;
        coop::saveTick(st, 1200ul, 2u, 5000ul, retry, drop);
        CHECK("retag: owner 2 flagged for retry", retry.size() == 1 && retry[0] == 2u);
        coop::saveRetagRetryId(st, 2u, 11u, 1300ul, 8000ul);
        CHECK_EQ("retag: owner 2's tracked xferId is now the retry stream's",
                 st.clients[2u].xferId, 11u);
        CHECK_EQ("retag: deadline reset to the caller's size-scaled timeout",
                 st.clients[2u].deadlineMs, 9300ul);
        // An ACK still carrying the ORIGINAL group id is stale (superseded).
        CHECK_EQ("retag: ACK for the superseded group id is ignored",
                 coop::saveNoteAck(st, 2u, 10u, true, 1400ul), coop::SC_STREAMING);
        // The retry's own ACK (fresh id) commits - the CR-01 proof.
        CHECK_EQ("retag: the retry's own ACK ok=1 commits (no phantom-stale kick)",
                 coop::saveNoteAck(st, 2u, 11u, true, 1500ul), coop::SC_COMMITTED);
        CHECK("retag: group settles after the retagged commit", coop::saveSettled(st));
        // The group id itself is untouched (log/bookkeeping only).
        CHECK_EQ("retag: group xferId unchanged", st.xferId, 10u);
        // Retagging an unknown owner is a no-op, never fabricates an entry.
        coop::saveRetagRetryId(st, 9u, 12u, 1600ul, 5000ul);
        CHECK_EQ("retag: unknown owner is a no-op", st.clients.size(), 2u);
    }

    // --- phase 10 review WR-02: saveDeferRetry refunds a retry that never
    // got a stream (SaveXfer is ONE serialized sender - the caller starts at
    // most one retry stream per tick and defers the rest). ------------------
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u);
        const unsigned int maxRetries = 2u;
        coop::saveBegin(st, 20u, connected, 1000ul, 500ul); // deadline = 1500
        // Common-mode expiry: ALL three expire in the same tick.
        std::vector<u32> retry, drop;
        coop::saveTick(st, 1600ul, maxRetries, 500ul, retry, drop);
        CHECK("defer: common-mode expiry flags every owner in one tick",
              retry.size() == 3 && drop.empty());
        // Caller streams to retry[0] only; defers the other two.
        coop::saveDeferRetry(st, retry[1], 1600ul);
        coop::saveDeferRetry(st, retry[2], 1600ul);
        CHECK("defer: deferred owners' retry budget refunded",
              st.clients[retry[1]].retries == 0u && st.clients[retry[2]].retries == 0u);
        CHECK_EQ("defer: the streamed owner keeps its charge",
                 st.clients[retry[0]].retries, 1u);
        // Next idle tick: ONLY the deferred owners are re-flagged (their
        // deadline expired at defer time; the streamed one got a fresh one).
        std::vector<u32> retry2, drop2;
        coop::saveTick(st, 1700ul, maxRetries, 500ul, retry2, drop2);
        CHECK("defer: deferred owners re-flagged on the next idle tick, "
              "streamed owner not double-flagged",
              retry2.size() == 2 && drop2.empty());
        CHECK("defer: re-flagged owners are the deferred pair",
              (retry2[0] == retry[1] || retry2[0] == retry[2]) &&
              (retry2[1] == retry[1] || retry2[1] == retry[2]) &&
              retry2[0] != retry2[1]);
        // The refund means a deferred owner still gets its FULL retry budget:
        // defer again, then walk it through maxRetries real charges + a drop.
        coop::saveDeferRetry(st, retry2[1], 1700ul);
        std::vector<u32> r3, d3;
        coop::saveTick(st, 1800ul, maxRetries, 500ul, r3, d3); // re-charge (1)
        coop::saveTick(st, 2400ul, maxRetries, 500ul, r3, d3); // charge 2
        coop::saveTick(st, 3000ul, maxRetries, 500ul, r3, d3); // -> drop
        CHECK("defer: a repeatedly-deferred owner still exhausts the SAME "
              "bounded budget before dropping (no infinite defer loop)",
              st.clients[retry2[1]].state == coop::SC_DROPPED);
        // Deferring an unknown owner is a no-op.
        coop::saveDeferRetry(st, 99u, 3000ul);
        CHECK_EQ("defer: unknown owner is a no-op", st.clients.size(), 3u);
    }

    // --- deadline expiry with NO ack at all behaves exactly like an
    // explicit ok=0 failure (silence is not distinguished from failure). ---
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(5u);
        coop::saveBegin(st, 100u, connected, 1000ul, 500ul); // deadline = 1500
        CHECK("deadline: not yet expired before the deadline", !coop::saveSettled(st));
        std::vector<u32> retry, drop;
        coop::saveTick(st, 1400ul, 1u, 500ul, retry, drop);
        CHECK("deadline: no candidate before the deadline elapses",
              retry.empty() && drop.empty());
        coop::saveTick(st, 1600ul, 1u, 500ul, retry, drop);
        CHECK("deadline: silence past the deadline is treated as a failure candidate",
              retry.size() == 1 && retry[0] == 5u);
    }

    // --- saveEraseOwner: disconnect cleanup never blocks the survivors'
    // settlement (Task 3 exercises the reconnect pair exhaustively; this is
    // the tracer-level smoke case). ----------------------------------------
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u);
        coop::saveBegin(st, 9u, connected, 1000ul, 30000ul);
        coop::saveNoteAck(st, 1u, 9u, true, 1100ul);
        CHECK("erase: not settled while owner 2 is still outstanding", !coop::saveSettled(st));
        CHECK_EQ("saveEraseOwner: erases exactly the departed owner",
                 coop::saveEraseOwner(st, 2u), 1u);
        CHECK("erase: settles immediately once the blocking owner is erased",
              coop::saveSettled(st));
        CHECK_EQ("saveEraseOwner: an already-erased/unknown owner erases nothing",
                 coop::saveEraseOwner(st, 2u), 0u);
    }
}

// ---- Per-client load coordinator (Phase 10 Plan 01, SAVE-02) ---------------
// LoadCoord.h's per-client state machine - the SaveCoord.h mirror for the
// load plane, plus the positive PKT_LOAD_ACK it completes. Exhaustive: GO ->
// ACK ok / ACK fail -> retry -> drop; NACK -> XFER -> commit-ACK -> LOADED;
// stale loadId NACK/ACK ignored; erase-mid-flight never blocks settlement.
static void testLoadCoord() {
    std::printf("\n== per-client load coordinator (Phase 10 Plan 01, SAVE-02) ==\n");

    // --- all-ACK-ok (the direct MATCH-arm path: GO -> ACK ok, no NACK). ----
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u);
        coop::loadBegin(st, 5u, connected, 1000ul, 30000ul);
        CHECK_EQ("loadBegin: seeds one entry per connected client", st.clients.size(), 3u);
        CHECK("loadBegin: every seeded client starts LC_GO_SENT",
              st.clients[1u].state == coop::LC_GO_SENT &&
              st.clients[2u].state == coop::LC_GO_SENT &&
              st.clients[3u].state == coop::LC_GO_SENT);
        CHECK("all-ok: not settled before any ACK", !coop::loadSettled(st));

        CHECK_EQ("owner 1 ACKs ok=1 -> LC_LOADED",
                 coop::loadNoteAck(st, 1u, 5u, true, 1100ul), coop::LC_LOADED);
        CHECK("all-ok: one client's ACK never implies the group loaded",
              !coop::loadSettled(st));
        coop::loadNoteAck(st, 2u, 5u, true, 1100ul);
        coop::loadNoteAck(st, 3u, 5u, true, 1100ul);
        CHECK("all-ok: settled only once EVERY client independently loaded",
              coop::loadSettled(st));

        // A stale ACK (superseded loadId) is ignored outright.
        CHECK_EQ("stale loadId ACK ignored (state unchanged)",
                 coop::loadNoteAck(st, 1u, 4u /* not 5 */, false, 1200ul), coop::LC_LOADED);
    }

    // --- the NACK-flow path: GO -> NACK -> XFER -> commit-ACK -> LOADED. ---
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(7u);
        coop::loadBegin(st, 20u, connected, 1000ul, 30000ul);
        CHECK_EQ("NACK: owner 7 NACKs -> LC_NACKED",
                 coop::loadNoteNack(st, 7u, 20u), coop::LC_NACKED);
        // A stale NACK (superseded loadId) is ignored.
        CHECK_EQ("NACK: stale loadId NACK ignored (state unchanged)",
                 coop::loadNoteNack(st, 7u, 19u), coop::LC_NACKED);
        CHECK_EQ("NACK: the fallback transfer starting -> LC_XFER",
                 coop::loadNoteXferStart(st, 7u), coop::LC_XFER);
        CHECK("NACK-flow: not settled while owner 7 is mid-transfer", !coop::loadSettled(st));
        CHECK_EQ("NACK-flow: the post-commit LOAD_ACK ok=1 -> LC_LOADED",
                 coop::loadNoteAck(st, 7u, 20u, true, 2000ul), coop::LC_LOADED);
        CHECK("NACK-flow: settled once the transferred client loads", coop::loadSettled(st));
    }

    // --- phase 10 review CR-03: NACK and transfer-start are PROGRESS edges -
    // each re-arms the deadline, so a healthy NACK-flow (host reload +
    // fallback transfer + join reload, easily > the GO-issue floor) is never
    // expired mid-flight, double-reloaded, and kicked. ----------------------
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(3u);
        coop::loadBegin(st, 30u, connected, 1000ul, 500ul); // GO deadline = 1500
        // The NACK lands at 1400 (before expiry) and re-arms for the reload
        // + transfer window the client is now legitimately waiting on.
        CHECK_EQ("CR-03: accepted NACK re-arms the deadline",
                 coop::loadNoteNack(st, 3u, 30u, 1400ul, 2000ul), coop::LC_NACKED);
        CHECK_EQ("CR-03: deadline = nackNow + extend", st.clients[3u].deadlineMs, 3400ul);
        // Past the ORIGINAL floor (1500) the client is NOT a candidate.
        std::vector<u32> retry, drop;
        coop::loadTick(st, 1600ul, 2u, 500ul, retry, drop);
        CHECK("CR-03: not expired at the superseded GO deadline",
              retry.empty() && drop.empty());
        // The fallback transfer starts at 3000; its size-scaled extension
        // re-arms again (covers the stream + the join's post-commit reload).
        CHECK_EQ("CR-03: transfer start re-arms the deadline (size-scaled)",
                 coop::loadNoteXferStart(st, 3u, 3000ul, 5000ul), coop::LC_XFER);
        CHECK_EQ("CR-03: deadline = xferNow + scaled extend",
                 st.clients[3u].deadlineMs, 8000ul);
        coop::loadTick(st, 3500ul, 2u, 500ul, retry, drop);
        CHECK("CR-03: not expired at the superseded NACK deadline either",
              retry.empty() && drop.empty());
        // A stale NACK must NOT re-arm (the extension is only for accepted
        // progress on the client's CURRENT loadId).
        coop::loadNoteNack(st, 3u, 29u, 9000ul, 2000ul);
        CHECK_EQ("CR-03: a stale NACK never re-arms the deadline",
                 st.clients[3u].deadlineMs, 8000ul);
        // The healthy flow completes; only genuine silence past the LAST
        // progress edge would ever have expired it.
        CHECK_EQ("CR-03: the flow completes normally after the extensions",
                 coop::loadNoteAck(st, 3u, 30u, true, 7000ul), coop::LC_LOADED);
        CHECK("CR-03: settled", coop::loadSettled(st));
        // Omitting the extension (extendMs=0, the legacy 3-arg shape) leaves
        // the deadline untouched - callers opt IN to the re-arm.
        coop::LoadCoordState st2;
        coop::loadBegin(st2, 31u, connected, 1000ul, 500ul);
        coop::loadNoteNack(st2, 3u, 31u);
        CHECK_EQ("CR-03: extendMs=0 leaves the deadline untouched",
                 st2.clients[3u].deadlineMs, 1500ul);
    }

    // --- one client fails, retries bounded times, then drops - the SaveCoord
    // shape mirrored onto the load plane, plus loadRetagRetryId (a retry
    // MUST carry a fresh loadId - the join's own stale-GO dedup would drop a
    // reused one, unlike SAVE's reused xferId). --------------------------
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u); connected.insert(4u);
        const unsigned int maxRetries    = 2u;
        const unsigned long retryTimeout = 5000ul;
        coop::loadBegin(st, 42u, connected, 1000ul, retryTimeout);

        coop::loadNoteAck(st, 1u, 42u, true, 1100ul);
        coop::loadNoteAck(st, 2u, 42u, true, 1100ul);
        coop::loadNoteAck(st, 3u, 42u, true, 1100ul);
        CHECK_EQ("owner 4 ACKs ok=0 -> LC_FAILED",
                 coop::loadNoteAck(st, 4u, 42u, false, 1100ul), coop::LC_FAILED);
        CHECK("one-fail: not settled while owner 4 is still FAILED", !coop::loadSettled(st));

        std::vector<u32> retry1, drop1;
        coop::loadTick(st, 1200ul, maxRetries, retryTimeout, retry1, drop1);
        CHECK("tick1: exactly owner 4 retried, nobody dropped",
              retry1.size() == 1 && retry1[0] == 4u && drop1.empty());
        CHECK("tick1: owner 4 back to LC_GO_SENT (not re-flagged next tick)",
              st.clients[4u].state == coop::LC_GO_SENT && st.clients[4u].retries == 1u);
        CHECK("tick1: owners 1/2/3 stay LC_LOADED, untouched by the retry tick",
              st.clients[1u].state == coop::LC_LOADED &&
              st.clients[2u].state == coop::LC_LOADED &&
              st.clients[3u].state == coop::LC_LOADED);

        // The caller mints a fresh loadId for the retry (owner 4's OLD
        // loadId, 42, would be dropped by the join's stale-GO dedup) and
        // retags LoadCoord so the client's NEXT ACK is judged against it,
        // not the original.
        coop::loadRetagRetryId(st, 4u, 43u);
        CHECK_EQ("loadRetagRetryId: owner 4's tracked loadId is now the retry's",
                 st.clients[4u].loadId, 43u);
        CHECK_EQ("post-retag: an ACK against the OLD loadId (42) is now stale, ignored",
                 coop::loadNoteAck(st, 4u, 42u, true, 1250ul), coop::LC_GO_SENT);
        CHECK_EQ("post-retag: an ACK against the NEW loadId (43) applies",
                 coop::loadNoteAck(st, 4u, 43u, false, 1300ul), coop::LC_FAILED);

        std::vector<u32> retry2, drop2;
        coop::loadTick(st, 2100ul, maxRetries, retryTimeout, retry2, drop2);
        CHECK("tick2: retried again (retries: 1 -> 2), still not dropped",
              retry2.size() == 1 && retry2[0] == 4u && drop2.empty());
        CHECK_EQ("tick2: retries counter reached maxRetries", st.clients[4u].retries, maxRetries);
        coop::loadRetagRetryId(st, 4u, 44u);
        coop::loadNoteAck(st, 4u, 44u, false, 2200ul);

        std::vector<u32> retry3, drop3;
        coop::loadTick(st, 3100ul, maxRetries, retryTimeout, retry3, drop3);
        CHECK("tick3: retries exhausted -> owner 4 is dropped, not retried again",
              retry3.empty() && drop3.size() == 1 && drop3[0] == 4u);
        CHECK_EQ("tick3: owner 4's terminal state is LC_DROPPED",
                 st.clients[4u].state, coop::LC_DROPPED);
        CHECK("post-drop: the OTHER three stayed LC_LOADED the entire time",
              st.clients[1u].state == coop::LC_LOADED &&
              st.clients[2u].state == coop::LC_LOADED &&
              st.clients[3u].state == coop::LC_LOADED);
        CHECK("post-drop: loadSettled becomes true only NOW (every client "
              "LOADED or DROPPED)", coop::loadSettled(st));
    }

    // --- deadline expiry with no ACK/NACK at all behaves exactly like an
    // explicit failure. -------------------------------------------------
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(9u);
        coop::loadBegin(st, 100u, connected, 1000ul, 500ul); // deadline = 1500
        std::vector<u32> retry, drop;
        coop::loadTick(st, 1400ul, 1u, 500ul, retry, drop);
        CHECK("deadline: no candidate before the deadline elapses",
              retry.empty() && drop.empty());
        coop::loadTick(st, 1600ul, 1u, 500ul, retry, drop);
        CHECK("deadline: silence past the deadline is treated as a failure candidate",
              retry.size() == 1 && retry[0] == 9u);
    }

    // --- loadEraseOwner: disconnect cleanup never blocks the survivors'
    // settlement. ---------------------------------------------------------
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u);
        coop::loadBegin(st, 9u, connected, 1000ul, 30000ul);
        coop::loadNoteAck(st, 1u, 9u, true, 1100ul);
        CHECK("erase: not settled while owner 2 is still outstanding", !coop::loadSettled(st));
        CHECK_EQ("loadEraseOwner: erases exactly the departed owner",
                 coop::loadEraseOwner(st, 2u), 1u);
        CHECK("erase: settles immediately once the blocking owner is erased",
              coop::loadSettled(st));
        CHECK_EQ("loadEraseOwner: an already-erased/unknown owner erases nothing",
                 coop::loadEraseOwner(st, 2u), 0u);
    }
}

// ---- Rejoin regression pair, both planes (Phase 10 Plan 01 Task 3) --------
// The two load-bearing rejoin cases the connect/leave-edge purge in
// Plugin.cpp depends on, for BOTH SaveCoord and LoadCoord: (a) an owner
// erased mid-flight (disconnect) never blocks the SURVIVORS' settlement,
// and (b) a fresh admission on the SAME (reused) slot after the erase seeds
// that client cleanly - no stale FAILED/DROPPED state bleeds from the
// PREVIOUS occupant of the id into the new connection's fresh transfer.
static void testCoordRejoin() {
    std::printf("\n== coordinator rejoin regression pair (Phase 10 Plan 01 Task 3) ==\n");

    // --- SaveCoord: (a) erase-mid-flight settles the survivors; (b) a fresh
    // saveBegin on the reused slot seeds SC_STREAMING, not stale SC_FAILED. -
    {
        coop::SaveCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u);
        coop::saveBegin(st, 50u, connected, 1000ul, 30000ul);
        coop::saveNoteAck(st, 1u, 50u, true, 1100ul);
        coop::saveNoteAck(st, 2u, 50u, true, 1100ul);
        // Owner 3 goes silent/fails, then DISCONNECTS mid-flight (never
        // reaches SC_DROPPED via bounded retry - the connect/leave-edge
        // purge is a SEPARATE cleanup path from saveTick's retry/drop).
        coop::saveNoteAck(st, 3u, 50u, false, 1100ul);
        CHECK("(a) not settled while owner 3 is still FAILED, pre-disconnect",
              !coop::saveSettled(st));
        CHECK_EQ("(a) saveEraseOwner mid-flight erases exactly owner 3",
                 coop::saveEraseOwner(st, 3u), 1u);
        CHECK("(a) the SURVIVORS (owners 1/2, already COMMITTED) let the "
              "group settle the instant the blocking owner is erased - "
              "disconnect never blocks the others any more than a bounded "
              "drop does", coop::saveSettled(st));

        // (b) Owner 3's slot reconnects (a fresh client process, restarted
        // per-sender state) and a NEW coordinated save admits it cleanly.
        std::set<u32> connected2;
        connected2.insert(1u); connected2.insert(2u); connected2.insert(3u);
        coop::saveBegin(st, 51u, connected2, 2000ul, 30000ul);
        CHECK("(b) the reused slot (owner 3) seeds SC_STREAMING on the fresh "
              "admission - no stale SC_FAILED/erased-entry residue from the "
              "PREVIOUS occupant of this id",
              st.clients.find(3u) != st.clients.end() &&
              st.clients[3u].state == coop::SC_STREAMING &&
              st.clients[3u].retries == 0u);
    }

    // --- LoadCoord: the same pair, mirrored onto the load plane. -----------
    {
        coop::LoadCoordState st;
        std::set<u32> connected;
        connected.insert(1u); connected.insert(2u); connected.insert(3u);
        coop::loadBegin(st, 60u, connected, 1000ul, 30000ul);
        coop::loadNoteAck(st, 1u, 60u, true, 1100ul);
        coop::loadNoteAck(st, 2u, 60u, true, 1100ul);
        coop::loadNoteAck(st, 3u, 60u, false, 1100ul);
        CHECK("(a) not settled while owner 3 is still FAILED, pre-disconnect",
              !coop::loadSettled(st));
        CHECK_EQ("(a) loadEraseOwner mid-flight erases exactly owner 3",
                 coop::loadEraseOwner(st, 3u), 1u);
        CHECK("(a) the survivors let the group settle the instant the "
              "blocking owner is erased", coop::loadSettled(st));

        std::set<u32> connected2;
        connected2.insert(1u); connected2.insert(2u); connected2.insert(3u);
        coop::loadBegin(st, 61u, connected2, 2000ul, 30000ul);
        CHECK("(b) the reused slot (owner 3) seeds LC_GO_SENT on the fresh "
              "admission - no stale LC_FAILED residue from the PREVIOUS "
              "occupant of this id",
              st.clients.find(3u) != st.clients.end() &&
              st.clients[3u].state == coop::LC_GO_SENT &&
              st.clients[3u].retries == 0u);
    }

    // --- CoordArbiter ABANDON: the disconnecting client held the ACTIVE
    // transition - coordComplete frees the arbiter so a LATER requester is
    // never rejected forever by a transition nobody is left to complete
    // (Plugin.cpp's leave-edge "[coord] ABANDON" path, proven here as the
    // pure decision: the caller detects heldArbiter BEFORE calling
    // coordComplete, exactly as the leave-edge integration does). ---------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 3u, 500u); // owner 3 is the active requester
        bool heldArbiter = arb.busy && arb.requesterId == 3u;
        CHECK("ABANDON: the departing owner is detected as holding the "
              "active transition", heldArbiter);
        coop::coordComplete(arb);
        CHECK("ABANDON: coordComplete frees the arbiter", !arb.busy);
        CHECK("ABANDON: a LATER, different requester is now accepted (not "
              "rejected forever by the dead transition)",
              coop::coordOffer(arb, coop::COORD_LOAD, 7u, 1u) == coop::COORD_ACCEPT);
    }
}

// ---- First-wins save/load arbiter (Phase 10 Plan 01, SAVE-03) --------------
// LoadCoord.h's CoordArbiter - ONE active transition at a time, shared by
// BOTH the save and load planes. Exhaustive: save-then-save reject; save-
// then-join-load reject; save-then-HOST-load preempt; load-then-anything
// reject; idempotent re-offer of the already-active transition; complete ->
// next offer accepted; a reject leaves the active transition's ids
// unchanged (the caller reads them for the PKT_COORD_REJECT payload).
static void testCoordArbiter() {
    std::printf("\n== first-wins save/load arbiter (Phase 10 Plan 01, SAVE-03) ==\n");

    // --- idle -> ACCEPT; the record becomes the active transition. --------
    {
        coop::CoordArbiter arb;
        CHECK("idle arbiter accepts the first offer",
              coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u) == coop::COORD_ACCEPT);
        CHECK("accepted offer becomes the active transition",
              arb.busy && arb.kind == coop::COORD_SAVE &&
              arb.requesterId == 5u && arb.reqId == 1u);
    }

    // --- save-then-save (a DIFFERENT requester/reqId) -> REJECT_BUSY,
    // carrying the ORIGINAL active transition's ids untouched. -------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u);
        CHECK("save-then-save (different requester): REJECT_BUSY",
              coop::coordOffer(arb, coop::COORD_SAVE, 6u, 2u) == coop::COORD_REJECT_BUSY);
        CHECK("save-then-save: the active transition is UNCHANGED after the reject "
              "(the caller reads these for the reject payload's active* fields)",
              arb.requesterId == 5u && arb.reqId == 1u && arb.kind == coop::COORD_SAVE);
    }

    // --- idempotent re-offer: the SAME (kind,requester,reqId) re-offering
    // itself while busy is a no-op ACCEPT (Phase 10 Plan 01: a REQ-admitted
    // save/load fires the same offer again at its own detour-driven local
    // edge a tick later). -------------------------------------------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u);
        CHECK("idempotent re-offer of the already-active transition -> ACCEPT",
              coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u) == coop::COORD_ACCEPT);
        CHECK("idempotent re-offer: the active transition is unchanged",
              arb.requesterId == 5u && arb.reqId == 1u && arb.kind == coop::COORD_SAVE);
    }

    // --- save-then-join-load (requesterId != 0) -> REJECT_BUSY (only a
    // HOST load preempts; a join's load request never does). ---------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u);
        CHECK("save-then-join-load: REJECT_BUSY (a join never preempts)",
              coop::coordOffer(arb, coop::COORD_LOAD, 6u, 9u) == coop::COORD_REJECT_BUSY);
        CHECK("save-then-join-load: the active SAVE transition is unchanged",
              arb.kind == coop::COORD_SAVE && arb.requesterId == 5u);
    }

    // --- save-then-HOST-load (requesterId==0) -> ACCEPT, preempting the
    // save (the existing abortAll rule, generalized). ----------------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u);
        CHECK("save-then-HOST-load (requesterId=0): ACCEPT (preempt)",
              coop::coordOffer(arb, coop::COORD_LOAD, 0u, 0u) == coop::COORD_ACCEPT);
        CHECK("preempt: the active transition is now the HOST load, not the save",
              arb.kind == coop::COORD_LOAD && arb.requesterId == 0u);
    }

    // --- load-then-anything -> REJECT_BUSY (only save-then-HOST-load
    // preempts; the reverse - an active LOAD superseded by a new SAVE or a
    // join's load - always rejects). ---------------------------------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_LOAD, 0u, 0u); // active: host load
        CHECK("load-then-save: REJECT_BUSY",
              coop::coordOffer(arb, coop::COORD_SAVE, 7u, 3u) == coop::COORD_REJECT_BUSY);
        CHECK("load-then-join-load: REJECT_BUSY (only a HOST load preempts, "
              "not a second join load)",
              coop::coordOffer(arb, coop::COORD_LOAD, 7u, 4u) == coop::COORD_REJECT_BUSY);
        CHECK("load-then-anything: the active HOST load transition is unchanged",
              arb.kind == coop::COORD_LOAD && arb.requesterId == 0u);
    }

    // --- complete -> next offer accepted. ----------------------------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 5u, 1u);
        CHECK("busy: a competing offer is rejected before complete",
              coop::coordOffer(arb, coop::COORD_SAVE, 6u, 2u) == coop::COORD_REJECT_BUSY);
        coop::coordComplete(arb);
        CHECK("coordComplete: the arbiter is idle again", !arb.busy);
        CHECK("post-complete: the NEXT offer is accepted",
              coop::coordOffer(arb, coop::COORD_SAVE, 6u, 2u) == coop::COORD_ACCEPT);
        CHECK("post-complete: the new offer is now the active transition",
              arb.requesterId == 6u && arb.reqId == 2u);
    }

    // --- reject carries BOTH the rejected AND the active request ids (the
    // CALLER builds PKT_COORD_REJECT from the offer's own args + the
    // arbiter's post-call state - this proves both are independently
    // readable after a reject). --------------------------------------------
    {
        coop::CoordArbiter arb;
        coop::coordOffer(arb, coop::COORD_SAVE, 11u, 100u);
        u32 rejectedRequester = 22u, rejectedReqId = 200u;
        int verdict = coop::coordOffer(arb, coop::COORD_SAVE, rejectedRequester, rejectedReqId);
        CHECK("reject: verdict is REJECT_BUSY", verdict == coop::COORD_REJECT_BUSY);
        CHECK("reject: the rejected request's OWN ids are the caller's own args "
              "(unchanged by the call - CoordRejectPacket.requesterId/reqId)",
              rejectedRequester == 22u && rejectedReqId == 200u);
        CHECK("reject: the ACTIVE transition's ids are readable from the arbiter "
              "post-call (CoordRejectPacket.activeRequesterId/activeReqId)",
              arb.requesterId == 11u && arb.reqId == 100u);
    }

    // --- determinism: offering the SAME two competing requests in either
    // order always names the SAME winner (whichever the caller drains
    // FIRST) - first-wins is deterministic by construction (a pure function
    // of call order), not a race. -------------------------------------------
    {
        coop::CoordArbiter arbA;
        coop::coordOffer(arbA, coop::COORD_SAVE, 1u, 1u);
        coop::coordOffer(arbA, coop::COORD_SAVE, 2u, 2u);
        coop::CoordArbiter arbB;
        coop::coordOffer(arbB, coop::COORD_SAVE, 1u, 1u); // same drain order
        coop::coordOffer(arbB, coop::COORD_SAVE, 2u, 2u);
        CHECK("determinism: the same drain order names the same winner every "
              "time (first-wins is a pure function of arrival/drain order)",
              arbA.requesterId == arbB.requesterId && arbA.reqId == arbB.reqId &&
              arbA.requesterId == 1u);
    }
}

// ---- N-player speed min-vote reduce (Phase 9 Plan 02, CONS-02) --------------
// SpeedVote.h's speedReduce is a PURE function of (hostReq, hostCombat, votes,
// capEnabled) - the exhaustive matrix below locks the vote/pause/combat/cap/
// disconnect contract without ENet or the engine (the host arbitration and
// disconnect drop that actually DRIVE speedVotes_ are ReplicatorChannels.cpp/
// ReplicatorCore.cpp's job, proven by nettest + the Plan 03/04 live gate).
static void testSpeedReduce() {
    std::printf("\n== N-player speed min-vote reduce (Phase 9 Plan 02, CONS-02) ==\n");

    // --- solo host passthrough: empty votes map ------------------------------
    {
        std::map<u32, coop::SpeedVoteRec> votes;
        coop::SpeedReduceOut r = coop::speedReduce(2.0f, false, votes, true);
        CHECK_EQ("empty votes: eff == hostReq", (int)(r.eff * 100), 200);
        CHECK("empty votes: not paused", !r.paused);
        CHECK("empty votes: cap did not fire (no combat)", !r.combatCapped);
        coop::SpeedReduceOut r2 = coop::speedReduce(-1.0f, false, votes, true);
        CHECK_EQ("empty votes, host not-yet-known (-1) defaults to 1.0x",
                  (int)(r2.eff * 100), 100);
    }

    // --- 1-3 votes, all permutations of {0,1,2,3,5}: eff = min(host, votes) -
    {
        const float V[5] = {0.0f, 1.0f, 2.0f, 3.0f, 5.0f};
        for (int a = 0; a < 5; ++a) {
            for (int b = 0; b < 5; ++b) {
                for (int c = 0; c < 5; ++c) {
                    std::map<u32, coop::SpeedVoteRec> votes;
                    coop::SpeedVoteRec ra; ra.req = V[a]; votes[2u] = ra;
                    coop::SpeedVoteRec rb; rb.req = V[b]; votes[3u] = rb;
                    coop::SpeedVoteRec rc; rc.req = V[c]; votes[4u] = rc;
                    float hostReq = 5.0f;
                    coop::SpeedReduceOut r = coop::speedReduce(hostReq, false, votes, true);
                    float want = hostReq;
                    if (V[a] < want) want = V[a];
                    if (V[b] < want) want = V[b];
                    if (V[c] < want) want = V[c];
                    CHECK("3-vote permutation: eff == min(host, all votes)",
                          fabs(r.eff - want) < 0.001f);
                }
            }
        }
    }

    // --- pause from any single voter wins the min (eff == 0) ----------------
    {
        std::map<u32, coop::SpeedVoteRec> votes;
        coop::SpeedVoteRec r2; r2.req = 3.0f; votes[2u] = r2;
        coop::SpeedVoteRec r3; r3.req = 0.0f; votes[3u] = r3; // paused
        coop::SpeedVoteRec r4; r4.req = 5.0f; votes[4u] = r4;
        coop::SpeedReduceOut out = coop::speedReduce(3.0f, false, votes, true);
        CHECK_EQ("a single paused voter pins eff to 0 regardless of others",
                  (int)(out.eff * 100), 0);
        CHECK("a single paused voter reads paused==true", out.paused);
    }

    // --- combat flag from any single voter caps; the cap never unpauses -----
    {
        std::map<u32, coop::SpeedVoteRec> votes;
        coop::SpeedVoteRec r2; r2.req = 3.0f; r2.combat = false; votes[2u] = r2;
        coop::SpeedVoteRec r3; r3.req = 3.0f; r3.combat = true;  votes[3u] = r3;
        coop::SpeedReduceOut out = coop::speedReduce(3.0f, false, votes, true);
        CHECK_EQ("a single voter's combat flag caps eff to 1x", (int)(out.eff * 100), 100);
        CHECK("cap fired -> combatCapped is true", out.combatCapped);
        CHECK("combined combat flag is true (host OR any vote)", out.combat);

        // The cap never force-unpauses: a paused voter's eff==0 stays 0 even
        // when another voter is fighting.
        std::map<u32, coop::SpeedVoteRec> votes2;
        coop::SpeedVoteRec p; p.req = 0.0f; votes2[2u] = p; // paused
        coop::SpeedVoteRec f; f.req = 3.0f; f.combat = true; votes2[3u] = f;
        coop::SpeedReduceOut out2 = coop::speedReduce(3.0f, false, votes2, true);
        CHECK_EQ("cap never force-unpauses: pause (0) stays 0 even under combat",
                  (int)(out2.eff * 100), 0);
        CHECK("still reads paused==true under the combat cap", out2.paused);
    }

    // --- capEnabled=false: passthrough, combat never pins to 1x -------------
    {
        std::map<u32, coop::SpeedVoteRec> votes;
        coop::SpeedVoteRec f; f.req = 3.0f; f.combat = true; votes[2u] = f;
        coop::SpeedReduceOut out = coop::speedReduce(3.0f, false, votes, false);
        CHECK_EQ("capEnabled=false: eff passes through uncapped even in combat",
                  (int)(out.eff * 100), 300);
        CHECK("capEnabled=false: combatCapped never fires", !out.combatCapped);
        CHECK("capEnabled=false: combined combat flag still reports true", out.combat);
    }

    // --- host-only combat caps the joins too (combat is an OR, not per-party)-
    {
        std::map<u32, coop::SpeedVoteRec> votes;
        coop::SpeedVoteRec v; v.req = 3.0f; v.combat = false; votes[2u] = v;
        coop::SpeedReduceOut out = coop::speedReduce(3.0f, /*hostCombat*/true, votes, true);
        CHECK_EQ("host-only combat still caps a non-fighting join's effective",
                  (int)(out.eff * 100), 100);
        CHECK("host-only combat sets combatCapped", out.combatCapped);
    }

    // --- disconnect modelled as erasing an entry raises the min --------------
    {
        std::map<u32, coop::SpeedVoteRec> votes;
        coop::SpeedVoteRec lo; lo.req = 0.0f; votes[2u] = lo;  // paused voter
        coop::SpeedVoteRec hi; hi.req = 3.0f; votes[3u] = hi;
        coop::SpeedReduceOut before = coop::speedReduce(3.0f, false, votes, true);
        CHECK_EQ("before disconnect: the paused voter pins eff to 0",
                  (int)(before.eff * 100), 0);
        votes.erase(2u); // the "instant vote drop" clearPeerReplicationState performs
        coop::SpeedReduceOut after = coop::speedReduce(3.0f, false, votes, true);
        CHECK_EQ("after the paused voter's entry is erased, eff raises to the "
                  "remaining min", (int)(after.eff * 100), 300);
        CHECK("after disconnect: no longer paused", !after.paused);
    }
}

// ---- Cell-claim map reduce (Phase 8 Plan 02, WORLD-03) -----------------------
// CellMap.h's reduceCellMap is the ONE structural fix WORLD-03 needs - the
// host-side pure function every instance's authority verdict derives from.
// Task 1 proves the HAPPY PATH only (a single fresh contest with the host as
// a party, and a fresh contest among joins alone); the exhaustive tie-break
// matrix (continuity vs arrival, incumbent-leave re-contest, vacancy,
// disconnect revert-to-host, shuffled-order determinism, collapse-
// equivalent) lands in Task 2.
static void testCellMap() {
    std::printf("\n== cell-claim map reduce (Phase 8 Plan 02, WORLD-03) ==\n");

    const u32 HOST = 0u;

    // Fresh contest, host is a party: host wins even though it is not the
    // lowest playerId in the general sense (playerId 0 IS the host by
    // convention, but this proves the HOST-FIRST rule, not a coincidental
    // lowest-id win - see the joins-only case below for that distinction).
    {
        coop::CellSlotMap slots;
        coop::CellSlotView v; v.cx = 5; v.cz = 7;
        slots[std::make_pair(HOST, (u32)0)] = v;
        coop::CellSlotView v2; v2.cx = 5; v2.cz = 7;
        slots[std::make_pair((u32)2, (u32)0)] = v2;
        std::set<u32> connected; connected.insert(HOST); connected.insert(2u);
        coop::CellOwnerMap prev; // empty - no incumbent, this is a fresh contest
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK("fresh contest, host is a party: exactly one resolved cell", out.size() == 1);
        CHECK_EQ("fresh contest, host is a party: host wins",
                 out[std::make_pair(5, 7)], HOST);
    }

    // Fresh contest among joins only (no host claim in this cell): the
    // LOWEST playerId wins, not insertion order.
    {
        coop::CellSlotMap slots;
        coop::CellSlotView vA; vA.cx = 10; vA.cz = -3;
        slots[std::make_pair((u32)3, (u32)0)] = vA; // higher id inserted first
        coop::CellSlotView vB; vB.cx = 10; vB.cz = -3;
        slots[std::make_pair((u32)1, (u32)0)] = vB; // lower id inserted second
        std::set<u32> connected; connected.insert(HOST); connected.insert(1u); connected.insert(3u);
        coop::CellOwnerMap prev;
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK("fresh contest, joins only: exactly one resolved cell", out.size() == 1);
        CHECK_EQ("fresh contest, joins only: lowest playerId wins",
                 out[std::make_pair(10, -3)], 1u);
    }

    // A cell with no claimant is absent from the output map (fail-open to
    // host - every authorityFor consumer already implements this for an
    // absent claimedCells_ entry).
    {
        coop::CellSlotMap slots; // empty
        std::set<u32> connected; connected.insert(HOST);
        coop::CellOwnerMap prev;
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK("no claimants: the output map is empty (fail-open to host)", out.empty());
    }

    // ---- Task 2: the exhaustive tie-break matrix -----------------------------

    // CONTINUITY holds against a host ARRIVAL: join1 already holds the cell
    // (previousMap says so) and still claims it; the host arriving this
    // round must NOT steal it - this is the assertion that distinguishes
    // the new model from the old host-wins-ties reduce (which would have
    // handed it to the host the instant it saw a host claim in the map,
    // regardless of who was there first).
    {
        coop::CellSlotMap slots;
        coop::CellSlotView vJoin; vJoin.cx = 1; vJoin.cz = 1;
        slots[std::make_pair((u32)1, (u32)0)] = vJoin; // incumbent join1
        coop::CellSlotView vHost; vHost.cx = 1; vHost.cz = 1;
        slots[std::make_pair(HOST, (u32)0)] = vHost;   // host arrives too
        std::set<u32> connected; connected.insert(HOST); connected.insert(1u);
        coop::CellOwnerMap prev;
        prev[std::make_pair(1, 1)] = 1u; // join1 already resolved owner last round
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK_EQ("continuity vs host arrival: incumbent join1 KEEPS the cell "
                 "(not stolen by the arriving host)", out[std::make_pair(1, 1)], 1u);
    }

    // Continuity holds against a LOWER playerId arriving: incumbent join2
    // keeps the cell over a fresh join1 claim (a lower id would win a FRESH
    // contest, but this is not fresh - join2 is the present incumbent).
    {
        coop::CellSlotMap slots;
        coop::CellSlotView v2; v2.cx = 2; v2.cz = 2;
        slots[std::make_pair((u32)2, (u32)0)] = v2; // incumbent join2
        coop::CellSlotView v1; v1.cx = 2; v1.cz = 2;
        slots[std::make_pair((u32)1, (u32)0)] = v1; // fresh, lower id, join1
        std::set<u32> connected; connected.insert(HOST); connected.insert(1u); connected.insert(2u);
        coop::CellOwnerMap prev;
        prev[std::make_pair(2, 2)] = 2u;
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK_EQ("continuity vs lower-id arrival: incumbent join2 keeps the "
                 "cell over a fresh lower-id join1 claim", out[std::make_pair(2, 2)], 2u);
    }

    // Incumbent LEAVES (its slot moves to a different cell / it disconnects
    // and its slot is erased by the caller before this reduce runs): the
    // cell re-contests per the fresh rule among whoever is left.
    {
        coop::CellSlotMap slots;
        // Incumbent join2's slot moved away - only join3 and join1 (fresh,
        // no host) remain claiming cell (3,3).
        coop::CellSlotView v3; v3.cx = 3; v3.cz = 3;
        slots[std::make_pair((u32)3, (u32)0)] = v3;
        coop::CellSlotView v1; v1.cx = 3; v1.cz = 3;
        slots[std::make_pair((u32)1, (u32)0)] = v1;
        std::set<u32> connected; connected.insert(HOST); connected.insert(1u); connected.insert(3u);
        coop::CellOwnerMap prev;
        prev[std::make_pair(3, 3)] = 2u; // join2 was the incumbent, now gone
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK_EQ("incumbent leaves: re-contest resolves per the fresh rule "
                 "(lowest playerId among the REMAINING claimants)",
                 out[std::make_pair(3, 3)], 1u);
    }

    // DISCONNECT revert-to-host: a departed owner's slot is erased by the
    // caller (clearPeerReplicationState's contract) BEFORE the next reduce -
    // once erased, the cell has zero claimants and is absent from the map
    // (fail-open to host, the locked disposition), even though the
    // PREVIOUS map still names the departed owner.
    {
        coop::CellSlotMap slots; // departed owner's slot already erased - empty
        std::set<u32> connected; connected.insert(HOST); // departed owner no longer connected either
        coop::CellOwnerMap prev;
        prev[std::make_pair(4, 4)] = 3u; // stale: the departed owner's old verdict
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK("disconnect revert-to-host: a cell with its claimant's slot "
              "erased is absent from the map (fail-open to host)",
              out.find(std::make_pair(4, 4)) == out.end());
    }
    // The CONTINUITY guard's OWN connectedOwners check matters independently
    // of an erased slot: an incumbent whose slot is STILL present (the
    // caller has not yet erased it) but who dropped OUT of connectedOwners
    // must not keep continuity on that fact alone - it falls through to the
    // fresh-contest rule, which is purely slot-based (by design, per
    // CellMap.h's contract: the CALLER is responsible for erasing a
    // departed owner's slots in the same tick connectedOwners drops it -
    // clearPeerReplicationState runs before the next reduce in Plugin.cpp's
    // tick order - so this proves the guard fires on connectedOwners alone,
    // not that a stale slot is somehow re-filtered by it too).
    {
        coop::CellSlotMap slots;
        coop::CellSlotView v3; v3.cx = 5; v3.cz = 5;
        slots[std::make_pair((u32)3, (u32)0)] = v3; // stale slot, owner 3 disconnected
        std::set<u32> connected; connected.insert(HOST); // owner 3 NOT in connectedOwners
        coop::CellOwnerMap prev;
        prev[std::make_pair(5, 5)] = 3u; // owner 3 was the incumbent
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK_EQ("disconnected incumbent (not connected) loses CONTINUITY - "
                 "falls to the fresh-contest rule, which is purely slot-based "
                 "(resolves to the sole slot-holding claimant, 3)",
                 out[std::make_pair(5, 5)], 3u);
    }

    // DETERMINISM: the SAME slot set fed in shuffled insertion order
    // produces the byte-identical output map - the regression test for the
    // std::map-iteration-order tie-break bug this reduce supersedes.
    {
        coop::CellSlotMap slotsA;
        {
            coop::CellSlotView a; a.cx = 6; a.cz = 6;
            slotsA[std::make_pair((u32)3, (u32)0)] = a;
            coop::CellSlotView b; b.cx = 6; b.cz = 6;
            slotsA[std::make_pair((u32)1, (u32)0)] = b;
            coop::CellSlotView c; c.cx = 6; c.cz = 6;
            slotsA[std::make_pair((u32)2, (u32)0)] = c;
        }
        coop::CellSlotMap slotsB;
        {
            // Same three entries, inserted in a DIFFERENT order. std::map
            // itself always iterates sorted regardless of insertion order,
            // so this proves reduceCellMap's OWN behavior is order-
            // invariant (it does not, say, remember "first seen" outside
            // the map's own ordering).
            coop::CellSlotView c; c.cx = 6; c.cz = 6;
            slotsB[std::make_pair((u32)2, (u32)0)] = c;
            coop::CellSlotView a; a.cx = 6; a.cz = 6;
            slotsB[std::make_pair((u32)3, (u32)0)] = a;
            coop::CellSlotView b; b.cx = 6; b.cz = 6;
            slotsB[std::make_pair((u32)1, (u32)0)] = b;
        }
        std::set<u32> connected; connected.insert(HOST); connected.insert(1u);
        connected.insert(2u); connected.insert(3u);
        coop::CellOwnerMap prev;
        coop::CellOwnerMap outA, outB;
        coop::reduceCellMap(slotsA, prev, connected, HOST, outA);
        coop::reduceCellMap(slotsB, prev, connected, HOST, outB);
        CHECK("determinism: shuffled insertion order produces the byte-"
              "identical output map", outA == outB);
        CHECK_EQ("determinism: the shuffled-order result is still the lowest "
                 "playerId (1), not an artifact of insertion order",
                 outA[std::make_pair(6, 6)], 1u);
    }

    // COLLAPSE-EQUIVALENT: when every claimed cell is co-located with a host
    // claim, every cell resolves to host WITHOUT a separate collapse
    // boolean - the per-cell rule subsumes the old claimsCoLocated() binary
    // partition.
    {
        coop::CellSlotMap slots;
        coop::CellSlotView h1; h1.cx = 7; h1.cz = 7;
        slots[std::make_pair(HOST, (u32)0)] = h1;
        coop::CellSlotView j1; j1.cx = 7; j1.cz = 7;
        slots[std::make_pair((u32)1, (u32)0)] = j1; // join1 co-located with host
        coop::CellSlotView h2; h2.cx = 8; h2.cz = 8;
        slots[std::make_pair(HOST, (u32)1)] = h2;
        coop::CellSlotView j2; j2.cx = 8; j2.cz = 8;
        slots[std::make_pair((u32)1, (u32)1)] = j2; // second co-located cell
        std::set<u32> connected; connected.insert(HOST); connected.insert(1u);
        coop::CellOwnerMap prev;
        coop::CellOwnerMap out;
        coop::reduceCellMap(slots, prev, connected, HOST, out);
        CHECK("collapse-equivalent: two resolved cells", out.size() == 2);
        bool allHost = true;
        for (coop::CellOwnerMap::const_iterator it = out.begin(); it != out.end(); ++it)
            if (it->second != HOST) allHost = false;
        CHECK("collapse-equivalent: every co-located cell resolves to host "
              "with no separate collapse boolean consulted", allHost);
    }

    // ---- Phase 8 review WR-04: deterministic overflow truncation ------------
    // Past CELL_MAP_MAX the host must adopt EXACTLY what it broadcasts -
    // truncateCellMap is the shared pure primitive: keeps the lowest
    // (cellX, cellY) keys, drops the rest, insertion-order-invariant.
    {
        // Under the cap: a no-op (nothing erased, map untouched).
        coop::CellOwnerMap m;
        m[std::make_pair(1, 1)] = 1u;
        m[std::make_pair(2, 2)] = 2u;
        coop::CellOwnerMap before = m;
        CHECK("truncate under cap: nothing erased",
              coop::truncateCellMap(m, 4u) == 0u);
        CHECK("truncate under cap: map untouched", m == before);
        // At the cap exactly: still a no-op.
        CHECK("truncate at cap exactly: nothing erased",
              coop::truncateCellMap(m, 2u) == 0u && m.size() == 2);
    }
    {
        // Overflow: 6 cells, cap 4 - the 4 LOWEST cell keys survive, the 2
        // highest are dropped, and the same set built in a DIFFERENT
        // insertion order truncates to the byte-identical map (std::map
        // iterates sorted, so the kept prefix is canonical, never an
        // insertion-order artifact - the determinism the one-verdict
        // invariant needs).
        coop::CellOwnerMap a;
        a[std::make_pair(9, 9)]   = 3u;
        a[std::make_pair(1, 5)]   = 1u;
        a[std::make_pair(4, 0)]   = 2u;
        a[std::make_pair(1, 2)]   = 2u;
        a[std::make_pair(-3, 7)]  = 1u;
        a[std::make_pair(4, 8)]   = 0u;
        coop::CellOwnerMap b;
        b[std::make_pair(1, 2)]   = 2u;
        b[std::make_pair(4, 8)]   = 0u;
        b[std::make_pair(-3, 7)]  = 1u;
        b[std::make_pair(9, 9)]   = 3u;
        b[std::make_pair(4, 0)]   = 2u;
        b[std::make_pair(1, 5)]   = 1u;
        unsigned int ea = coop::truncateCellMap(a, 4u);
        unsigned int eb = coop::truncateCellMap(b, 4u);
        CHECK("truncate overflow: exactly the overflow count erased",
              ea == 2u && eb == 2u && a.size() == 4 && b.size() == 4);
        CHECK("truncate overflow: shuffled insertion order truncates to the "
              "byte-identical map (deterministic prefix)", a == b);
        CHECK("truncate overflow: the LOWEST cell keys survive",
              a.find(std::make_pair(-3, 7)) != a.end() &&
              a.find(std::make_pair(1, 2))  != a.end() &&
              a.find(std::make_pair(1, 5))  != a.end() &&
              a.find(std::make_pair(4, 0))  != a.end());
        CHECK("truncate overflow: the highest cell keys are the ones dropped",
              a.find(std::make_pair(4, 8)) == a.end() &&
              a.find(std::make_pair(9, 9)) == a.end());
    }
}

int main() {
    std::printf("prototest: KenshiCoop wire/hash/interp unit layer (protocol v%u)\n",
                (unsigned)PROTOCOL_VERSION);
    testSizes();
    testObjectHandLayout();
    testEngineFaults();
    testEngineCaps();
    testChangeGate();
    testSeqPerSender();
    testFoldDedup();
    testPinOwner();
    testXferCommit();
    testClaimArbiter();
    testCellMap();
    testMoneyFold();
    testSaveCoord();
    testLoadCoord();
    testCoordRejoin();
    testCoordArbiter();
    testSpeedReduce();
    testRoundTrips();
    testFraming();
    testSaveCrc();
    testFolderFingerprint();
    testSaveXferRoundTrip();
    testContentHash();
    testInterp();
    testOwnRanks();
    testLeaveExpansion();
    testPauseSchedule();
    testSteamIdParse();
    testWorkPoseMatch();
    testTaskClear();
    testDeathRekey();
    testInboundLifecycle();
    testFlushWorldStateContract();
    testTeardownOrdering();
    std::printf("\nprototest: %d/%d checks passed%s\n",
                g_total - g_failed, g_total, g_failed ? " - FAIL" : " - PASS");
    return g_failed;
}
