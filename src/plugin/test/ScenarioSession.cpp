// ScenarioSession.cpp - session lifecycle scenarios (monolith split from
// Scenario.cpp, 2026-07-12): latejoin_probe/latejoin_sync, save_probe,
// save_sync/save_stage1, resume_check, load_probe, load_sync, money_persist,
// connect_relink. Classes are
// TU-private (anonymous namespace); only makeSessionScenario
// (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// latejoin_probe (protocol 30 phase 0, probe tier; latejoinSync forced OFF) /
// latejoin_sync (probe=false, full tier; ON). A client that connects late
// trusts the shared save + live streams; anything that diverged BEFORE the
// connect stays diverged until a safety resend happens to cover it - and the
// one-shot describe/mint edges (PKT_BUILD_PLACE) are lost FOREVER. The HOST
// mutates state in the PRE-ARM window (onGameplay - gameplay has started but
// the join has not connected yet; the harness launches the join 8 s after
// host gameplay + its own load time): toggles a baked door, writes a
// sentinel faction relation, bumps the shared money pool, and places + completes a
// small building. Post-arm both sides census door/faction/money at 1 Hz;
// build evidence is the join's [build] MINT line (or its absence). PROBE
// QUESTIONS: which pre-connect mutations reach the join and how fast
// (expected: door/faction/money heal via their 10 s/5 s safety resends -
// the rows were sent once into the void, arming the resend; the building
// NEVER mints); the sync arm gates that the connect-edge resync closes all
// four immediately.
class LatejoinProbeScenario : public TimedScenario {
public:
    explicit LatejoinProbeScenario(bool probe)
        : TimedScenario(probe ? "latejoin_probe" : "latejoin_sync", 1000),
          probe_(probe), censusLogged_(0),
          mutDoor_(false), mutFac_(false), mutMoney_(false), mutBuild_(false),
          doorOk_(false), facOk_(false), moneyOk_(false), buildOk_(false),
          buildDone_(false), rampStep_(0), nextRampTick_(0), nextDoorFixMs_(0),
          doorLockMode_(false) {
        memset(doorHand_, 0, sizeof(doorHand_));
        memset(buildHand_, 0, sizeof(buildHand_));
        facSid_[0] = '\0'; buildSid_[0] = '\0';
        doorWant_ = -1;
    }

    // PRE-ARM (host only): all four mutations land while the join is still
    // loading/connecting. elapsedMs here is time since GAMEPLAY START.
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        if (!mutDoor_ && ctx.elapsedMs >= MUT_AT_MS) {
            mutDoor_ = true;
            doDoorMutation(ctx);
        }
        // The engine can flip a freshly-written baked door right back (town AI
        // settling / an NPC in the doorway - observed on run 224454: reverted
        // within 1 s). Re-assert the sentinel state during a window that is
        // safely PRE-connect (the join needs ~20 s to launch + load), so the
        // mutated state is the host truth the late joiner must converge to.
        // Never re-asserted after the window: post-connect writes would stream
        // as live changes and erase the A/B evidence.
        if (mutDoor_ && doorWant_ >= 0 &&
            ctx.elapsedMs <= DOOR_HOLD_UNTIL_MS && ctx.elapsedMs >= nextDoorFixMs_) {
            nextDoorFixMs_ = ctx.elapsedMs + 2000;
            engine::DoorRead cur;
            if (engine::readDoorByHand(doorHand_, &cur)) {
                int have = doorLockMode_ ? cur.locked : cur.open;
                if (have == doorWant_) {
                    // The mutation is holding (a deferred lock apply also
                    // lands here) - this is the local leg's real verdict.
                    doorOk_ = true;
                } else {
                    engine::DoorRead post;
                    int ok = doorLockMode_
                        ? (engine::writeDoorByHand(doorHand_, 0, doorWant_, &post) ? 1 : 0)
                        : (engine::writeDoorByHand(doorHand_, doorWant_, -1, &post) ? 1 : 0);
                    char b[176];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO LJDOORFIX mode=%s reverted=%d want=%d ok=%d after=%d t=%lu",
                              doorLockMode_ ? "lock" : "open", have, doorWant_, ok,
                              doorLockMode_ ? post.locked : post.open, ctx.elapsedMs);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
        if (!mutFac_ && ctx.elapsedMs >= MUT_AT_MS) {
            mutFac_ = true;
            doFacMutation(ctx);
        }
        if (!mutMoney_ && ctx.elapsedMs >= MUT_AT_MS) {
            mutMoney_ = true;
            doMoneyMutation(ctx);
        }
        if (!mutBuild_ && ctx.elapsedMs >= BUILD_AT_MS) {
            mutBuild_ = true;
            doBuildMutation(ctx);
        }
        if (buildOk_ && !buildDone_ && rampStep_ < MAX_RAMP_STEPS &&
            GetTickCount() >= nextRampTick_)
            doRampStep(ctx);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO LATEJOIN start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (ctx.isHost) {
            // The pre-arm mutation verdict AT the connect boundary (a
            // re-asserted door counts as held) - what the oracle gates on.
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LJMUT door=%d fac=%d money=%d build=%d done=%d",
                      doorOk_ ? 1 : 0, facOk_ ? 1 : 0, moneyOk_ ? 1 : 0,
                      buildOk_ ? 1 : 0, buildDone_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // The build ramp may still be finishing if arming came fast (the ramp
        // schedule is wall-clock, so it survives the arm-time clock reset).
        if (ctx.isHost && buildOk_ && !buildDone_ && rampStep_ < MAX_RAMP_STEPS &&
            GetTickCount() >= nextRampTick_)
            doRampStep(ctx);
        if (evidenceDue(ctx.elapsedMs)) {
            logCensus(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = (censusLogged_ > 0);
            // The host's pre-arm mutation leg must have worked in BOTH tiers
            // or the A/B proves nothing. The DOOR leg is findings-only: town
            // AI owns the sync save's baked door and fights sentinel state
            // (runs 225300/230601), so it never gates.
            if (ctx.isHost)
                passed_ = passed_ && facOk_ && moneyOk_ &&
                          buildOk_ && buildDone_;
            return true;
        }
        return false;
    }

private:
    // -- pre-arm mutations (host) --------------------------------------------

    void doDoorMutation(const ScenarioContext& ctx) {
        int ok = 0, before = -1, after = -1;
        if (pickDoor(ctx, doorHand_)) {
            engine::DoorRead cur;
            if (engine::readDoorByHand(doorHand_, &cur)) {
                engine::DoorRead post;
                if (cur.hasLock) {
                    // Prefer the LOCK bit: town AI flips a baked door's open
                    // state right back (run 225300: reverted within ~1 s of
                    // every write), but nothing in the settle loop touches
                    // locks - a lock sentinel actually HOLDS pre-connect.
                    // CLOSE + lock in one write (run 225815: a lock-only
                    // write on an OPEN door did not take).
                    doorLockMode_ = true;
                    before = cur.locked;
                    doorWant_ = cur.locked ? 0 : 1;
                    ok = engine::writeDoorByHand(doorHand_, 0, doorWant_,
                                                 &post) ? 1 : 0;
                    after = post.locked;
                } else {
                    doorLockMode_ = false;
                    before = cur.open;
                    doorWant_ = cur.open ? 0 : 1;
                    ok = engine::writeDoorByHand(doorHand_, doorWant_, -1,
                                                 &post) ? 1 : 0;
                    after = post.open;
                }
                doorOk_ = (ok == 1) && (after == doorWant_);
            }
        }
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO LJDOOR hand=%u.%u.%u.%u.%u mode=%s before=%d want=%d "
                  "ok=%d after=%d t=%lu",
                  doorHand_[0], doorHand_[1], doorHand_[2], doorHand_[3], doorHand_[4],
                  doorLockMode_ ? "lock" : "open", before, doorWant_,
                  doorOk_ ? 1 : 0, after, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doFacMutation(const ScenarioContext& ctx) {
        float before = -999.0f, after = -999.0f;
        int ok = 0;
        if (pickFacSid(ctx, facSid_, sizeof(facSid_))) {
            float target = (float)FAC_SENTINEL;
            ok = engine::writeRelationBySid(ctx.gw, facSid_, target,
                                            /*reciprocal*/true, &before, &after) ? 1 : 0;
            facOk_ = (ok == 1) && (after > target - 0.5f) && (after < target + 0.5f);
        }
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO LJFAC sid='%s' target=%.1f ok=%d before=%.1f after=%.1f t=%lu",
                  facSid_, (double)FAC_SENTINEL, facOk_ ? 1 : 0, before, after, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doMoneyMutation(const ScenarioContext& ctx) {
        int before = -1, after = -1, ok = 0;
        if (engine::readPlayerWallet(ctx.gw, &before)) {
            int want = before + MONEY_BUMP;
            if (engine::writePlayerWallet(ctx.gw, want)) {
                engine::readPlayerWallet(ctx.gw, &after);
                ok = (after == want) ? 1 : 0;
            }
        }
        moneyOk_ = (ok == 1);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO LJMONEY before=%d bump=%d after=%d ok=%d t=%lu",
                  before, MONEY_BUMP, after, ok, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doBuildMutation(const ScenarioContext& ctx) {
        float x = 0, y = 0, z = 0, yaw = 0;
        int rc = engine::probePlaceBuilding(ctx.gw, 9.0f, -5.0f, /*wantDoor*/false,
                                            buildHand_, buildSid_, sizeof(buildSid_),
                                            &x, &y, &z, &yaw);
        buildOk_ = (rc == 1) && (buildHand_[4] != 0 || buildHand_[3] != 0);
        if (buildOk_) nextRampTick_ = GetTickCount() + RAMP_STEP_MS;
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO LJBUILD rc=%d ok=%d sid='%s' hand=%u.%u.%u.%u.%u "
                  "pos=(%.1f,%.1f,%.1f) t=%lu",
                  rc, buildOk_ ? 1 : 0, buildSid_,
                  buildHand_[0], buildHand_[1], buildHand_[2], buildHand_[3],
                  buildHand_[4], x, y, z, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doRampStep(const ScenarioContext& ctx) {
        ++rampStep_;
        nextRampTick_ = GetTickCount() + RAMP_STEP_MS;
        float want = 0.5f * (float)rampStep_;
        if (want > 1.0f) want = 1.0f;
        engine::BuildRead post;
        int ok = engine::writeBuildProgressByHand(buildHand_, want,
                                                   want >= 1.0f, &post) ? 1 : 0;
        if (ok && post.complete) buildDone_ = true;
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO LJBUILDPROG step=%u write=%.2f ok=%d prog=%.3f complete=%d t=%lu",
                  rampStep_, want, ok, post.progress, post.complete, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // -- post-arm census (both sides) ----------------------------------------

    void logCensus(const ScenarioContext& ctx) {
        // Doors (the sync save has one baked door in range).
        engine::DoorRead rows[MAX_DOORS];
        unsigned int nd = engine::enumDoorsNear(ctx.gw, 100.0f, rows, MAX_DOORS);
        for (unsigned int i = 0; i < nd && i < 4; ++i) {
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LJDOORROW hand=%u.%u.%u.%u.%u open=%d locked=%d t=%lu",
                      rows[i].hand[0], rows[i].hand[1], rows[i].hand[2],
                      rows[i].hand[3], rows[i].hand[4], rows[i].open, rows[i].locked,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // Faction rows: the first two sorted sids (the host sentinels the first).
        engine::FactionRead fr[MAX_FACTIONS];
        unsigned int nf = engine::listPlayerRelations(ctx.gw, fr, MAX_FACTIONS);
        if (nf > 0) {
            std::vector<std::string> sids;
            for (unsigned int i = 0; i < nf; ++i) sids.push_back(std::string(fr[i].sid));
            std::sort(sids.begin(), sids.end());
            for (unsigned int k = 0; k < 2 && k < sids.size(); ++k) {
                float us = -999.0f, them = -999.0f;
                if (!engine::readRelationBySid(ctx.gw, sids[k].c_str(), &us, &them))
                    continue;
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LJFACROW sid='%s' us=%.1f t=%lu",
                          sids[k].c_str(), us, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // The shared money pool (protocol 52): one value per client, so the
        // host's pre-arm bump has to show up in the join's series for the money
        // leg to have healed. rank= is kept in the line shape (always 0 now) so
        // the oracle's row parser is unchanged.
        int money = -1;
        if (engine::readPlayerWallet(ctx.gw, &money)) {
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LJMONEYROW rank=0 money=%d t=%lu",
                      money, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        ++censusLogged_;
    }

    // -- deterministic picks --------------------------------------------------

    bool pickDoor(const ScenarioContext& ctx, unsigned int outHand[5]) {
        engine::DoorRead rows[MAX_DOORS];
        unsigned int n = engine::enumDoorsNear(ctx.gw, 100.0f, rows, MAX_DOORS);
        if (n == 0) return false;
        unsigned int best = 0; // lowest (serial, index) = the door_sync pick
        for (unsigned int i = 1; i < n; ++i) {
            if (rows[i].hand[4] < rows[best].hand[4] ||
                (rows[i].hand[4] == rows[best].hand[4] &&
                 rows[i].hand[3] < rows[best].hand[3]))
                best = i;
        }
        memcpy(outHand, rows[best].hand, sizeof(unsigned int) * 5);
        return true;
    }

    bool pickFacSid(const ScenarioContext& ctx, char* outSid, unsigned int outLen) {
        engine::FactionRead rows[MAX_FACTIONS];
        unsigned int n = engine::listPlayerRelations(ctx.gw, rows, MAX_FACTIONS);
        if (n == 0) return false;
        std::vector<std::string> sids;
        for (unsigned int i = 0; i < n; ++i) sids.push_back(std::string(rows[i].sid));
        std::sort(sids.begin(), sids.end());
        strncpy(outSid, sids[0].c_str(), outLen - 1);
        outSid[outLen - 1] = '\0';
        return true;
    }

    static const unsigned long MUT_AT_MS      = 3000;
    static const unsigned long BUILD_AT_MS    = 5000;
    // Ends BEFORE the earliest observed connect (~13.6 s after host gameplay
    // start): a re-assert after the connect would stream as a live change and
    // erase the pre-connect A/B evidence.
    static const unsigned long DOOR_HOLD_UNTIL_MS = 10000;
    static const unsigned long RAMP_STEP_MS   = 2000;
    static const unsigned long DURATION_MS    = 45000;
    static const unsigned int  MAX_DOORS      = 32;
    static const unsigned int  MAX_FACTIONS   = 48;
    static const unsigned int  MAX_SQUAD      = 16;
    static const unsigned int  MAX_RAMP_STEPS = 6;
    static const int           MONEY_BUMP     = 777;
    static const int           FAC_SENTINEL   = -85; // distinct from faction_sync's -75

    bool          probe_;
    unsigned int  censusLogged_;
    bool          mutDoor_, mutFac_, mutMoney_, mutBuild_;
    bool          doorOk_, facOk_, moneyOk_, buildOk_, buildDone_;
    unsigned int  rampStep_;
    unsigned long nextRampTick_; // absolute GetTickCount deadline (survives arm reset)
    unsigned long nextDoorFixMs_; // pre-arm door re-assert throttle (gameplay clock)
    bool          doorLockMode_;  // true = sentinel is the LOCK bit (door has one)
    int           doorWant_;
    unsigned int  doorHand_[5];
    unsigned int  buildHand_[5];
    char          facSid_[48];
    char          buildSid_[48];
};

// save_probe (protocol 31 phase 12a, probe tier; saveSync forced OFF - the
// detour is installed for edge logging only, no coordination). Retires the two
// runtime unknowns that gate the host-authoritative save transfer:
//   1. do SaveManager::getCurrentGame()/getSavePath() (spike 39 RVAs, never
//      called before) return the real save identity/root at runtime?
//   2. how long does a MID-SESSION save take from the save(name) call to the
//      folder going quiet (the completion edge the transfer must wait for),
//      and does gameplay hitch while the engine writes?
// Script: after arming (both clients in-game), the HOST logs saveInfo, issues
// engine::saveGameAs("coopresume") and watches the folder via the SaveXfer
// quiescence watcher, logging SAVEWATCH ~1 Hz and SAVEDONE once. The JOIN
// idles (its half of protocol 31 arrives in phase 12b). The [save] LOCAL-SAVE
// detour line is the edge evidence the oracle cross-checks.
class SaveProbeScenario : public TimedScenario {
public:
    SaveProbeScenario()
        : TimedScenario("save_probe", 0),
          issued_(false), issueOk_(false), done_(false),
          doneKind_(0), lastWatchLogMs_(0), lastTickMs_(0), maxTickGapMs_(0),
          doneFiles_(0), doneBytes_(0), doneWaitMs_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SAVEPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Hitch oracle: the widest wall-clock gap between consecutive ticks
        // while the save is in flight (the engine writes on this thread).
        unsigned long now = GetTickCount();
        if (issued_ && !done_ && lastTickMs_ != 0 &&
            now - lastTickMs_ > maxTickGapMs_)
            maxTickGapMs_ = now - lastTickMs_;
        lastTickMs_ = now;

        if (ctx.isHost && !issued_ && ctx.elapsedMs >= SAVE_AT_MS) {
            issued_ = true;
            logSaveInfo("before", ctx.elapsedMs);
            issueOk_ = engine::saveGameAs(SAVE_NAME);
            savexfer::armWatch(SAVE_NAME);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SAVEISSUE name='%s' ok=%d t=%lu",
                      SAVE_NAME, issueOk_ ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (ctx.isHost && issued_ && !done_) {
            unsigned int files = 0;
            unsigned __int64 bytes = 0;
            unsigned long waited = 0;
            int rc = savexfer::tickWatch(&files, &bytes, &waited);
            if (rc == 1 || rc == 2) {
                done_     = true;
                doneKind_ = rc;
                doneFiles_ = files; doneBytes_ = bytes; doneWaitMs_ = waited;
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SAVEDONE kind=%s files=%u bytes=%I64u waitMs=%lu t=%lu",
                          rc == 1 ? "quiesced" : "timeout", files, bytes, waited,
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                logSaveInfo("after", ctx.elapsedMs);
            } else if (ctx.elapsedMs - lastWatchLogMs_ >= 1000) {
                lastWatchLogMs_ = ctx.elapsedMs;
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SAVEWATCH files=%u bytes=%I64u waitedMs=%lu t=%lu",
                          files, bytes, waited, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            if (ctx.isHost) {
                char b[96];
                _snprintf(b, sizeof(b) - 1, "SCENARIO SAVEHITCH maxTickGapMs=%lu",
                          maxTickGapMs_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                // The probe's local legs: the save was issued and the folder
                // completion edge was actually OBSERVED (kind=quiesced).
                passed_ = issueOk_ && done_ && doneKind_ == 1 && doneFiles_ > 0;
            } else {
                passed_ = true; // the join has no protocol-31 half yet (12b)
            }
            return true;
        }
        return false;
    }

private:
    void logSaveInfo(const char* when, unsigned long t) {
        char curGame[96], savePath[512];
        bool ok = engine::saveInfo(curGame, sizeof(curGame),
                                   savePath, sizeof(savePath));
        char b[704];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SAVEINFO when=%s ok=%d curGame='%s' savePath='%s' t=%lu",
                  when, ok ? 1 : 0, curGame, savePath, t);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long SAVE_AT_MS  = 3000;
    static const unsigned long DURATION_MS = 45000;

    bool          issued_, issueOk_, done_;
    int           doneKind_;
    unsigned long lastWatchLogMs_;
    unsigned long lastTickMs_, maxTickGapMs_;
    unsigned int  doneFiles_;
    unsigned __int64 doneBytes_;
    unsigned long doneWaitMs_;

    static const char* const SAVE_NAME;
};
const char* const SaveProbeScenario::SAVE_NAME = "coopresume";

// load_probe (protocol 32 phase 13a, probe tier; loadSync forced OFF - the
// load detour is installed for edge logging only, no coordination on top;
// saveSync stays ON so the coordinated save still delivers the join's copy
// first). Retires the runtime unknowns gating the coordinated load:
//   1. is a MID-SESSION engine::loadSave() safe from the main-loop tick?
//      (the deferred LOADGAME mechanism suggests yes - never validated)
//   2. does mainLoop_hook keep ticking across the load screen? (the Plugin's
//      WORLD-SWAP / WORLD-RELOAD pair + hookTicksDuringSwap answer)
//   3. does the HOST survive its own world swap with the sync layer still
//      running? (the stale-pointer smoke test the 13b session reset needs)
//   4. do save-stable hands RESOLVE again after the swap? (post-load census
//      of a hand captured before the load)
// Script: the HOST censuses a squad hand, issues a coordinated
// saveGameAs('coopresume') (the Plugin's saveSync half streams it to the
// join), waits for the transfer DONE edge, then issues
// engine::loadSave('coopresume') MID-SESSION and measures the swap. The JOIN
// deliberately does NOT load - it logs its leader position ~2 s as the
// unsynced divergence baseline (its coordinated half arrives in 13b).
class LoadProbeScenario : public TimedScenario {
public:
    LoadProbeScenario()
        : TimedScenario("load_probe", 0),
          censused_(false), censusOk_(false), saveIssued_(false),
          saveOk_(false), loadIssued_(false), loadOk_(false), wasLive_(false),
          execTried_(false), swapSeen_(false), swapDone_(false), resolved_(false),
          resolveOk_(false), loadIssueMs_(0), resolveAtMs_(0), lastSigLogMs_(0),
          lastTickMs_(0), maxTickGapMs_(0), lastJoinLogMs_(0),
          preCount_(0), preLeader_(0) {
        memset(hand_, 0, sizeof(hand_));
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO LOADPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!ctx.isHost) {
            // Divergence baseline: where the join's leader stands while the
            // host reloads (today's behaviour = the join never follows).
            if (ctx.elapsedMs - lastJoinLogMs_ >= 2000) {
                lastJoinLogMs_ = ctx.elapsedMs;
                float x = 0, y = 0, z = 0;
                Character* c = engine::leader(ctx.gw);
                if (c && engine::readPos(c, &x, &y, &z)) {
                    char b[144];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO LOADWATCH-JOIN pos=%.1f,%.1f,%.1f live=%d t=%lu",
                              x, y, z, engine::gameplayLive(ctx.gw) ? 1 : 0,
                              ctx.elapsedMs);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
            if (ctx.elapsedMs >= DURATION_MS) { passed_ = true; return true; }
            return false;
        }

        // Hitch oracle: the widest wall-clock gap between consecutive ticks
        // from the load issue until the swap completes (does the hook stall?).
        unsigned long now = GetTickCount();
        if (loadIssued_ && !swapDone_ && lastTickMs_ != 0 &&
            now - lastTickMs_ > maxTickGapMs_)
            maxTickGapMs_ = now - lastTickMs_;
        lastTickMs_ = now;

        // 1. Pre-load census: a save-stable squad hand to re-resolve after.
        if (!censused_ && ctx.elapsedMs >= CENSUS_AT_MS) {
            censused_ = true;
            EntityState st[8];
            preCount_ = engine::captureSquad(ctx.gw, false, st, 8);
            if (preCount_ > 0) {
                hand_[0] = st[0].hIndex; hand_[1] = st[0].hSerial;
                hand_[2] = st[0].hType;  hand_[3] = st[0].hContainer;
                hand_[4] = st[0].hContainerSerial;
                censusOk_ = true;
            }
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOADCENSUS when=before n=%u hand=%u.%u.%u.%u.%u t=%lu",
                      preCount_, hand_[0], hand_[1], hand_[2], hand_[3], hand_[4],
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // 2. The coordinated save (the Plugin's saveSync half does the rest:
        // detour edge -> quiescence -> paced transfer -> join commit).
        if (censused_ && !saveIssued_ && ctx.elapsedMs >= SAVE_AT_MS) {
            saveIssued_ = true;
            saveOk_ = engine::saveGameAs(SAVE_NAME);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOADSAVEISSUE name='%s' ok=%d t=%lu",
                      SAVE_NAME, saveOk_ ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // 3. The mid-session load, once the transfer's DONE went out (the
        // join holds an identical copy) - or after the fallback window if
        // the transfer never completes (the load measurement still counts).
        if (saveIssued_ && !loadIssued_ &&
            (savexfer::lastSentXferId() != 0 || ctx.elapsedMs >= LOAD_FALLBACK_MS)) {
            loadIssued_ = true;
            loadIssueMs_ = ctx.elapsedMs;
            wasLive_ = engine::gameplayLive(ctx.gw);
            preLeader_ = engine::leader(ctx.gw); // identity evidence: a world
                                                 // rebuild reallocates Characters
            loadOk_  = engine::loadSave(SAVE_NAME);
            int delay = -1, sig = engine::saveMgrSignal(&delay);
            char b[192];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOADISSUE name='%s' ok=%d xferDone=%d signal=%d delay=%d t=%lu",
                      SAVE_NAME, loadOk_ ? 1 : 0,
                      savexfer::lastSentXferId() != 0 ? 1 : 0, sig, delay,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // 3b. First-run finding: SaveManager::load only SETS the deferred
        // LOADGAME signal - mid-session nothing consumes it (the title-screen
        // loop is the execute() pump), so the world never swapped. Pump
        // execute() manually once, a beat after the issue, from this
        // end-of-tick context (onTick runs AFTER the engine tick) - the probe
        // measures whether the engine's own deferred-load path is safe here.
        if (loadIssued_ && !execTried_ && !swapSeen_ &&
            ctx.elapsedMs >= loadIssueMs_ + EXEC_AFTER_MS) {
            execTried_ = true;
            int delay = -1, sig = engine::saveMgrSignal(&delay);
            bool ok = false;
            if (sig == 2 /*LOADGAME*/) ok = engine::saveMgrExecute();
            int sigAfter = engine::saveMgrSignal(&delay);
            bool liveAfter = engine::gameplayLive(ctx.gw);
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOADEXEC ok=%d sigBefore=%d sigAfter=%d liveAfter=%d t=%lu",
                      ok ? 1 : 0, sig, sigAfter, liveAfter ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            // A fully SYNCHRONOUS swap (torn down + rebuilt inside the call)
            // never shows a live-drop edge - latch completion off the
            // consumed signal instead.
            if (ok && sig == 2 && sigAfter != 2 && liveAfter && !swapSeen_) {
                swapSeen_ = swapDone_ = true;
                resolveAtMs_ = ctx.elapsedMs + RESOLVE_SETTLE_MS;
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LOADSWAPDONE t=%lu (synchronous inside execute)",
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // Signal telemetry while the load is pending (~1 Hz): does the
        // deferred signal sit, count down, or get consumed?
        if (loadIssued_ && !swapDone_ && ctx.elapsedMs - lastSigLogMs_ >= 1000) {
            lastSigLogMs_ = ctx.elapsedMs;
            int delay = -1, sig = engine::saveMgrSignal(&delay);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOADSIG signal=%d delay=%d t=%lu",
                      sig, delay, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // 4. Swap tracking: gameplay-live drop, then return (the engine's
        // deferred load performs the swap a few frames after the issue).
        if (loadIssued_) {
            bool live = engine::gameplayLive(ctx.gw);
            if (!swapSeen_ && wasLive_ && !live) {
                swapSeen_ = true;
                char b[96];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LOADSWAP begin t=%lu",
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (swapSeen_ && !swapDone_ && live) {
                swapDone_ = true;
                resolveAtMs_ = ctx.elapsedMs + RESOLVE_SETTLE_MS;
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LOADSWAPDONE t=%lu",
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // 5. Post-load census: does the pre-load hand resolve in the fresh
        // world (the same-lineage guarantee the whole sync rests on)?
        if (swapDone_ && !resolved_ && ctx.elapsedMs >= resolveAtMs_) {
            resolved_ = true;
            Character* c = censusOk_
                ? engine::resolveCharByHand(hand_[0], hand_[1], hand_[2],
                                            hand_[3], hand_[4])
                : 0;
            float x = 0, y = 0, z = 0;
            bool posOk = c && engine::readPos(c, &x, &y, &z);
            EntityState st[8];
            unsigned int postCount = engine::captureSquad(ctx.gw, false, st, 8);
            resolveOk_ = (c != 0);
            // Identity evidence: a real world rebuild reallocates every
            // Character - the leader pointer changing proves the stale-pointer
            // hazard the 13b session reset must cover.
            Character* postLeader = engine::leader(ctx.gw);
            char b[224];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOADCENSUS when=after resolved=%d pos=%.1f,%.1f,%.1f n=%u leaderChanged=%d t=%lu",
                      resolveOk_ ? 1 : 0, x, y, z, postCount,
                      (postLeader != preLeader_) ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            (void)posOk;
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            char b[112];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOADHITCH maxTickGapMs=%lu",
                      maxTickGapMs_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            // The probe's local legs: the mid-session load was issued, the
            // world actually swapped (live drop + return observed), and the
            // pre-load hand resolved again in the fresh world.
            passed_ = loadOk_ && swapDone_ && resolveOk_;
            return true;
        }
        return false;
    }

private:
    static const unsigned long CENSUS_AT_MS     = 3000;
    static const unsigned long SAVE_AT_MS       = 4000;
    static const unsigned long LOAD_FALLBACK_MS = 45000;
    static const unsigned long EXEC_AFTER_MS    = 3000;
    static const unsigned long RESOLVE_SETTLE_MS = 4000;
    static const unsigned long DURATION_MS      = 100000;

    bool          censused_, censusOk_, saveIssued_, saveOk_;
    bool          loadIssued_, loadOk_, wasLive_, execTried_;
    bool          swapSeen_, swapDone_, resolved_, resolveOk_;
    unsigned long loadIssueMs_, resolveAtMs_, lastSigLogMs_;
    unsigned long lastTickMs_, maxTickGapMs_, lastJoinLogMs_;
    unsigned int  preCount_;
    Character*    preLeader_;
    unsigned int  hand_[5];

    static const char* const SAVE_NAME;
};
const char* const LoadProbeScenario::SAVE_NAME = "coopresume";

// load_sync (protocol 32 phase 13c, full tier; loadSync + saveSync both ON).
// The user's exact manual scenario, automated: the HOST places a construction
// site (session-runtime state that exists in NO baked save), issues a
// coordinated saveGameAs('coopresume') (the saveSync half streams the join a
// byte-identical copy; its PKT_SAVE_ACK is the "join holds my copy" gate),
// then loads that save MID-SESSION. The Plugin's protocol-32 coordination
// does everything else: the load detour edge broadcasts PKT_LOAD_GO with the
// folder fingerprint, the join verifies its copy and issues its own
// bypass-once load, both sides swap worlds, and each runs the session reset
// on its own reload edge. Post-swap each side censuses nearby construction
// sites and logs LSSITE rows - the oracle gates host-hand == join-hand (the
// shared-save-lineage identity claim, POST-load), GO receipt + the join's
// load issue, and both sides' WORLD-RELOAD/session-reset log evidence.
class LoadSyncScenario : public TimedScenario {
public:
    LoadSyncScenario()
        : TimedScenario("load_sync", 0),
          placed_(false), placeOk_(false), saveIssued_(false),
          saveOk_(false), ackSeen_(false), ackOk_(false), loadIssued_(false),
          loadOk_(false), sigWas2_(false), swapSeen_(false), swapDone_(false),
          censused_(false), siteSeen_(false), dropStartMs_(0), sigClearedMs_(0),
          censusAtMs_(0), lastStatusMs_(0) {
        memset(ownHand_, 0, sizeof(ownHand_));
        ownSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO LOADSYNC start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool live = engine::gameplayLive(ctx.gw);

        // ---- Host script: build -> coordinated save -> wait ACK -> load ----
        if (ctx.isHost) {
            if (!placed_ && ctx.elapsedMs >= PLACE_AT_MS) {
                placed_ = true;
                float x = 0, y = 0, z = 0, yaw = 0;
                int rc = engine::probePlaceBuilding(ctx.gw, 8.0f, -4.0f,
                                                    /*wantDoor*/false,
                                                    ownHand_, ownSid_, sizeof(ownSid_),
                                                    &x, &y, &z, &yaw);
                placeOk_ = (rc == 1) && (ownHand_[4] != 0 || ownHand_[3] != 0);
                char b[224];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LSBUILD rc=%d ok=%d sid='%s' hand=%u.%u.%u.%u.%u t=%lu",
                          rc, placeOk_ ? 1 : 0, ownSid_,
                          ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3],
                          ownHand_[4], ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (placed_ && !saveIssued_ && ctx.elapsedMs >= SAVE_AT_MS) {
                saveIssued_ = true;
                saveOk_ = engine::saveGameAs(SAVE_NAME);
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LSSAVE name='%s' ok=%d t=%lu",
                          SAVE_NAME, saveOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // The join's commit acknowledgement = it holds a byte-identical
            // copy, so the coordinated load's fingerprint check will MATCH
            // (the transfer-fallback leg is load-tested by divergence runs).
            if (saveIssued_ && !ackSeen_ && savexfer::lastAckXferId() != 0) {
                ackSeen_ = true;
                ackOk_ = (savexfer::lastAckOk() == 1);
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LSACK ok=%d t=%lu",
                          ackOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (ackSeen_ && ackOk_ && !loadIssued_ && live) {
                loadIssued_ = true;
                loadOk_ = engine::loadSave(SAVE_NAME);
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LSLOAD name='%s' ok=%d t=%lu",
                          SAVE_NAME, loadOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // The JOIN is purely reactive: the Plugin's GO handler issues its
        // bypass-once load; the scenario just watches for its own swap.

        // ---- Both sides: world-swap tracking --------------------------------
        // Asynchronous path: gameplay-live drop >= the reload threshold, then
        // return (mirrors the Plugin's WORLD-SWAP/WORLD-RELOAD detection).
        if (!live && dropStartMs_ == 0) {
            dropStartMs_ = ctx.elapsedMs;
            if (!swapSeen_) {
                swapSeen_ = true;
                char b[96];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LSSWAP begin t=%lu",
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else if (live && dropStartMs_ != 0) {
            unsigned long ms = ctx.elapsedMs - dropStartMs_;
            dropStartMs_ = 0;
            if (ms >= SWAP_MIN_MS && !swapDone_) {
                swapDone_ = true;
                censusAtMs_ = ctx.elapsedMs + CENSUS_SETTLE_MS;
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LSSWAPDONE swapMs=%lu t=%lu", ms, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // Synchronous path: execute() tears down + rebuilds inside one call
        // (load_probe evidence), so live never visibly drops - latch off the
        // deferred LOADGAME signal being consumed, confirmed by a window with
        // no live-drop after the clear (covers the async race).
        {
            int sig = engine::saveMgrSignal(0);
            if (sig == 2) { sigWas2_ = true; sigClearedMs_ = 0; }
            else if (sigWas2_ && sigClearedMs_ == 0) sigClearedMs_ = ctx.elapsedMs;
            if (!swapDone_ && sigWas2_ && sigClearedMs_ != 0 && live &&
                dropStartMs_ == 0 &&
                ctx.elapsedMs >= sigClearedMs_ + SYNC_CONFIRM_MS) {
                swapDone_ = true;
                censusAtMs_ = ctx.elapsedMs + CENSUS_SETTLE_MS;
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LSSWAPDONE t=%lu (synchronous inside execute)",
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // ---- Both sides: post-load site census ------------------------------
        // The pre-load building must enumerate in the FRESH world - and under
        // the SAME save-stable hand on both clients (the oracle cross-check).
        if (swapDone_ && !censused_ && live && ctx.elapsedMs >= censusAtMs_) {
            censused_ = true;
            engine::BuildRead rows[16];
            unsigned int n = engine::enumSitesNear(ctx.gw, 150.0f, rows, 16);
            char c[96];
            _snprintf(c, sizeof(c) - 1, "SCENARIO LSCOUNT n=%u t=%lu",
                      n, ctx.elapsedMs);
            c[sizeof(c) - 1] = '\0'; coop::logLine(c);
            for (unsigned int i = 0; i < n && i < 8; ++i) {
                const engine::BuildRead& r = rows[i];
                char b[256];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO LSSITE hand=%u.%u.%u.%u.%u sid='%s' "
                          "prog=%.3f complete=%d t=%lu",
                          r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                          r.sid, r.progress, r.complete, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            siteSeen_ = (n > 0);
        }

        if (ctx.elapsedMs - lastStatusMs_ >= 5000) {
            lastStatusMs_ = ctx.elapsedMs;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LSSTATE placed=%d save=%d ack=%d load=%d "
                      "swapDone=%d site=%d live=%d t=%lu",
                      placeOk_ ? 1 : 0, saveOk_ ? 1 : 0, ackOk_ ? 1 : 0,
                      loadOk_ ? 1 : 0, swapDone_ ? 1 : 0, siteSeen_ ? 1 : 0,
                      live ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Finish early once the local legs are all in (plus a hold for the
        // peer's tail evidence), else at the hard duration.
        bool localDone = ctx.isHost
            ? (placeOk_ && saveOk_ && ackOk_ && loadOk_ && swapDone_ && censused_)
            : (swapDone_ && censused_);
        if ((localDone && ctx.elapsedMs >= censusAtMs_ + TAIL_HOLD_MS) ||
            ctx.elapsedMs >= DURATION_MS) {
            passed_ = ctx.isHost
                ? (placeOk_ && saveOk_ && ackOk_ && loadOk_ && swapDone_ && siteSeen_)
                : (swapDone_ && siteSeen_);
            return true;
        }
        return false;
    }

private:
    static const unsigned long PLACE_AT_MS      = 8000;
    static const unsigned long SAVE_AT_MS       = 12000;
    static const unsigned long SWAP_MIN_MS      = 400;   // Plugin's flicker floor
    static const unsigned long SYNC_CONFIRM_MS  = 3000;  // no-drop window after sig clear
    static const unsigned long CENSUS_SETTLE_MS = 5000;
    static const unsigned long TAIL_HOLD_MS     = 8000;
    static const unsigned long DURATION_MS      = 110000;

    bool          placed_, placeOk_, saveIssued_, saveOk_;
    bool          ackSeen_, ackOk_, loadIssued_, loadOk_;
    bool          sigWas2_, swapSeen_, swapDone_, censused_, siteSeen_;
    unsigned long dropStartMs_, sigClearedMs_, censusAtMs_, lastStatusMs_;
    unsigned int  ownHand_[5];
    char          ownSid_[48];

    static const char* const SAVE_NAME;
};
const char* const LoadSyncScenario::SAVE_NAME = "coopresume";

// save_sync (protocol 31 phase 12c, full tier; saveSync ON) / save_stage1
// (the resume_test.ps1 stage-1 variant: a building is placed FIRST so the
// coordinated save bakes session-runtime state). The HOST issues one
// mid-session save; the Plugin's coordination does everything else (detour
// edge -> quiescence watch -> paced folder transfer -> join stage/verify/
// commit -> ACK). The scenario itself only issues the save and gates the
// terminal SaveXfer states:
//   host: the save issued AND the transfer's DONE went out (lastSentXferId);
//   join: the staged save VERIFIED + COMMITTED (lastCommitResult == 1).
// The oracle cross-checks the log evidence (LOCAL-SAVE edge, QUIESCED,
// XFER-SENT/COMMIT file+byte equality, ACK ok=1).
class SaveSyncScenario : public TimedScenario {
public:
    explicit SaveSyncScenario(bool stage1)
        : TimedScenario(stage1 ? "save_stage1" : "save_sync", 0),
          stage1_(stage1), placed_(false), placeOk_(false),
          rampStep_(0), nextRampMs_(0), issued_(false), issueOk_(false),
          sentLogged_(false), commitLogged_(false), lastStatusMs_(0) {
        memset(ownHand_, 0, sizeof(ownHand_));
        ownSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SAVESYNC start host=%d stage1=%d",
                  ctx.isHost ? 1 : 0, stage1_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Stage 1 only: bake session-runtime state into the save - place a
        // construction site and ramp it PART-way (0.5: still enumerable by
        // the incomplete-site census after resume, the same-hand proof).
        if (stage1_ && ctx.isHost) {
            if (!placed_ && ctx.elapsedMs >= PLACE_AT_MS) {
                placed_ = true;
                float x = 0, y = 0, z = 0, yaw = 0;
                int rc = engine::probePlaceBuilding(ctx.gw, 8.0f, -4.0f,
                                                    /*wantDoor*/false,
                                                    ownHand_, ownSid_, sizeof(ownSid_),
                                                    &x, &y, &z, &yaw);
                placeOk_ = (rc == 1) && (ownHand_[4] != 0 || ownHand_[3] != 0);
                if (placeOk_) nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
                char b[224];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SAVEBUILD rc=%d ok=%d sid='%s' hand=%u.%u.%u.%u.%u t=%lu",
                          rc, placeOk_ ? 1 : 0, ownSid_,
                          ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3],
                          ownHand_[4], ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (placeOk_ && rampStep_ < 2 && ctx.elapsedMs >= nextRampMs_) {
                ++rampStep_;
                nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
                engine::BuildRead post;
                int ok = engine::writeBuildProgressByHand(
                             ownHand_, 0.25f * (float)rampStep_,
                             0.25f * (float)rampStep_ >= 1.0f, &post) ? 1 : 0;
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SAVEPROG step=%u ok=%d prog=%.3f t=%lu",
                          rampStep_, ok, post.progress, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // The one coordinated save (host only; a join-initiated save is the
        // REQ path, exercised manually - the transfer legs are identical).
        unsigned long saveAt = stage1_ ? STAGE1_SAVE_AT_MS : SAVE_AT_MS;
        if (ctx.isHost && !issued_ && ctx.elapsedMs >= saveAt) {
            issued_ = true;
            issueOk_ = engine::saveGameAs(SAVE_NAME);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SAVEISSUE name='%s' ok=%d t=%lu",
                      SAVE_NAME, issueOk_ ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Terminal-state edges, logged once each (the oracle's gate lines).
        if (ctx.isHost && !sentLogged_ && savexfer::lastSentXferId() != 0) {
            sentLogged_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SAVESENT id=%u t=%lu",
                      savexfer::lastSentXferId(), ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (!ctx.isHost && !commitLogged_ && savexfer::lastCommitResult() != -1) {
            commitLogged_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SAVECOMMIT ok=%d t=%lu",
                      savexfer::lastCommitResult(), ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (ctx.elapsedMs - lastStatusMs_ >= 5000) {
            lastStatusMs_ = ctx.elapsedMs;
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SAVESTATE issued=%d sent=%u commit=%d t=%lu",
                      issued_ ? 1 : 0, savexfer::lastSentXferId(),
                      savexfer::lastCommitResult(), ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            if (ctx.isHost) {
                passed_ = issueOk_ && savexfer::lastSentXferId() != 0;
                if (stage1_) passed_ = passed_ && placeOk_ && rampStep_ >= 2;
            } else {
                passed_ = (savexfer::lastCommitResult() == 1);
            }
            return true;
        }
        return false;
    }

private:
    static const unsigned long PLACE_AT_MS       = 8000;
    static const unsigned long RAMP_STEP_MS      = 3000;
    static const unsigned long SAVE_AT_MS        = 8000;  // save_sync
    static const unsigned long STAGE1_SAVE_AT_MS = 20000; // after the build bakes
    static const unsigned long DURATION_MS       = 45000;

    bool          stage1_;
    bool          placed_, placeOk_;
    unsigned int  rampStep_;
    unsigned long nextRampMs_;
    bool          issued_, issueOk_;
    bool          sentLogged_, commitLogged_;
    unsigned long lastStatusMs_;
    unsigned int  ownHand_[5];
    char          ownSid_[48];

    static const char* const SAVE_NAME;
};
const char* const SaveSyncScenario::SAVE_NAME = "coopresume";

// resume_check (protocol 31 phase 12c, stage 2 of resume_test.ps1). Both
// clients relaunched with KENSHICOOP_SAVE=coopresume - the save the stage-1
// coordinated transfer delivered to the join. The stage-1 building is BAKED
// in that save (progress 0.5, still enumerable by the incomplete-site
// census), so if the resume flow really re-ran the shared-save lineage, BOTH
// clients enumerate it under the SAME save-stable hand - the identity-reset
// claim, proven. Each side logs RESUMESITE rows ~1 Hz; the Test-SaveResume
// oracle gates host-hand == join-hand.
class ResumeCheckScenario : public TimedScenario {
public:
    ResumeCheckScenario()
        : TimedScenario("resume_check", 0),
          lastCensusMs_(0), siteSeen_(false) {}

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO RESUMECHECK start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastCensusMs_ >= 1000 || lastCensusMs_ == 0) {
            lastCensusMs_ = ctx.elapsedMs;
            engine::BuildRead rows[16];
            unsigned int n = engine::enumSitesNear(ctx.gw, 100.0f, rows, 16);
            char c[96];
            _snprintf(c, sizeof(c) - 1, "SCENARIO RESUMECOUNT n=%u t=%lu",
                      n, ctx.elapsedMs);
            c[sizeof(c) - 1] = '\0'; coop::logLine(c);
            for (unsigned int i = 0; i < n && i < 8; ++i) {
                const engine::BuildRead& r = rows[i];
                char b[256];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO RESUMESITE hand=%u.%u.%u.%u.%u sid='%s' "
                          "prog=%.3f complete=%d t=%lu",
                          r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                          r.sid, r.progress, r.complete, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (n > 0) siteSeen_ = true;
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            // The local leg: the baked site enumerated here. The cross-client
            // same-hand equality is the oracle's gate (it has both logs).
            passed_ = siteSeen_;
            return true;
        }
        return false;
    }

private:
    static const unsigned long DURATION_MS = 30000;

    unsigned long lastCensusMs_;
    bool          siteSeen_;
};

// money_persist (protocol 52, full tier; loadSync + saveSync ON): the bug this
// whole channel exists for. Before the shared pool, cats the JOIN earned or
// spent lived only in the join's own engine wallet, so the host's save - the only
// save that survives - wrote the host's untouched number and the join's economy
// was silently erased on the next load.
//
// The gate replays exactly that: the JOIN spends, the HOST saves + loads
// (protocol-32 coordination drags the join through the same load), and the pool
// must come back on BOTH sides at the post-spend total.
//   * join t=8s:  spend a QUARTER of the pool (a purchase, as far as the engine
//                 is concerned). A fraction rather than a fixed amount so the
//                 scenario does not depend on how many cats the fixture ships
//                 with - and, unlike a host-seeded base, it is still a real
//                 spend with the channel switched OFF, which is what makes the
//                 KENSHICOOP_MONEY_SYNC=0 A/B reproduce the reported bug
//                 instead of just failing to set itself up.
//   * host: waits until the fold has actually moved its own wallet (the join's
//           delta landed), latches that total as the expectation, then issues the
//           coordinated save and, once the join has ACKed its copy, loads it.
//   * both: post-swap, the pool must read the latched total. Reading the
//     pre-spend number instead IS the original bug.
class MoneyPersistScenario : public TimedScenario {
public:
    MoneyPersistScenario()
        : TimedScenario("money_persist", /*evidenceMs=*/1000),
          spent_(false), spendOk_(false), startPool_(-1), expect_(-1),
          latched_(false), poolMoved_(false),
          saveIssued_(false), saveOk_(false), ackSeen_(false),
          ackOk_(false), loadIssued_(false), loadOk_(false), sigWas2_(false),
          swapDone_(false), checked_(false), postOk_(false), postPool_(-1),
          dropStartMs_(0), sigClearedMs_(0), checkAtMs_(0), lastStatusMs_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        engine::readPlayerWallet(ctx.gw, &startPool_);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO MPSTART host=%d pool=%d",
                  ctx.isHost ? 1 : 0, startPool_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool live = engine::gameplayLive(ctx.gw);
        if (evidenceDue(ctx.elapsedMs)) {
            int pool = -1;
            engine::readPlayerWallet(ctx.gw, &pool);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "SCENARIO MPPOOL money=%d who=%s t=%lu",
                      pool, ctx.isHost ? "host" : "join", ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            // The join's expectation is the LAST total it saw before the load
            // began, not the number its own spend produced: the host is the
            // authority and what it saves is what it converged us to.
            if (!ctx.isHost && spendOk_ && pool >= 0 &&
                !swapDone_ && !sigWas2_ && dropStartMs_ == 0) {
                expect_ = pool;
            }
        }

        // ---- Join: the spend that used to be thrown away -------------------
        if (!ctx.isHost && !spent_ && ctx.elapsedMs >= SPEND_AT_MS) {
            int before = -1, after = -1, ok = 0, amount = 0;
            bool readable = engine::readPlayerWallet(ctx.gw, &before) &&
                            before >= MIN_POOL;
            // An unreadable/empty pool this early is more likely "not settled
            // yet" than "broken", so retry until the deadline rather than
            // reporting a failure we would have to explain away.
            if (readable || ctx.elapsedMs >= SPEND_DEADLINE_MS) {
                spent_ = true;
                if (readable) {
                    amount = before / 4;
                    ok = engine::writePlayerWallet(ctx.gw, before - amount) ? 1 : 0;
                    engine::readPlayerWallet(ctx.gw, &after);
                }
                startPool_ = before; // the pre-spend total the reload must beat
                spendOk_ = (ok == 1) && (amount > 0) && (after == before - amount);
                expect_ = after;     // what the reload must give us back
                latched_ = spendOk_;
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO MPSPEND amount=%d ok=%d before=%d after=%d t=%lu",
                          amount, ok, before, after, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // ---- Host: latch the folded total, then save + load -----------------
        if (ctx.isHost) {
            // The fold is what proves the join's spend reached the authority, so
            // the expectation is read from OUR wallet after it moves - never
            // computed from a number we assumed the join had. Latch whether it
            // moved or not, and record which: waiting for a fold that will never
            // come (the channel switched off) would stall the run before the
            // save, and then the A/B would only show a scenario that failed to
            // set itself up rather than the erase it is meant to catch.
            if (!latched_ && ctx.elapsedMs >= LATCH_AFTER_MS) {
                int pool = -1;
                if (engine::readPlayerWallet(ctx.gw, &pool) && pool >= 0) {
                    latched_ = true;
                    expect_ = pool;
                    poolMoved_ = (pool != startPool_);
                    char b[176];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO MPLATCH expect=%d start=%d moved=%d t=%lu",
                              expect_, startPool_, poolMoved_ ? 1 : 0,
                              ctx.elapsedMs);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
            if (latched_ && !saveIssued_ && live) {
                saveIssued_ = true;
                saveOk_ = engine::saveGameAs(SAVE_NAME);
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MPSAVE name='%s' ok=%d t=%lu",
                          SAVE_NAME, saveOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (saveIssued_ && !ackSeen_ && savexfer::lastAckXferId() != 0) {
                ackSeen_ = true;
                ackOk_ = (savexfer::lastAckOk() == 1);
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MPACK ok=%d t=%lu",
                          ackOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (ackSeen_ && ackOk_ && !loadIssued_ && live) {
                loadIssued_ = true;
                loadOk_ = engine::loadSave(SAVE_NAME);
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MPLOAD name='%s' ok=%d t=%lu",
                          SAVE_NAME, loadOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // ---- Both sides: world-swap tracking (the load_sync detection) -------
        if (!live && dropStartMs_ == 0) {
            dropStartMs_ = ctx.elapsedMs;
        } else if (live && dropStartMs_ != 0) {
            unsigned long ms = ctx.elapsedMs - dropStartMs_;
            dropStartMs_ = 0;
            if (ms >= SWAP_MIN_MS && !swapDone_) {
                swapDone_ = true;
                checkAtMs_ = ctx.elapsedMs + SETTLE_MS;
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MPSWAPDONE swapMs=%lu t=%lu",
                          ms, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        {
            // Synchronous load: execute() rebuilds the world inside one call, so
            // live never visibly drops - latch off the deferred LOADGAME signal.
            int sig = engine::saveMgrSignal(0);
            if (sig == 2) { sigWas2_ = true; sigClearedMs_ = 0; }
            else if (sigWas2_ && sigClearedMs_ == 0) sigClearedMs_ = ctx.elapsedMs;
            if (!swapDone_ && sigWas2_ && sigClearedMs_ != 0 && live &&
                dropStartMs_ == 0 &&
                ctx.elapsedMs >= sigClearedMs_ + SYNC_CONFIRM_MS) {
                swapDone_ = true;
                checkAtMs_ = ctx.elapsedMs + SETTLE_MS;
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO MPSWAPDONE t=%lu (synchronous inside execute)",
                          ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // ---- Both sides: did the cats survive the reload? -------------------
        if (swapDone_ && !checked_ && live && ctx.elapsedMs >= checkAtMs_) {
            checked_ = true;
            engine::readPlayerWallet(ctx.gw, &postPool_);
            postOk_ = (expect_ >= 0) && (postPool_ == expect_);
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO MPPOST money=%d expect=%d start=%d ok=%d who=%s t=%lu",
                      postPool_, expect_, startPool_, postOk_ ? 1 : 0,
                      ctx.isHost ? "host" : "join", ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.elapsedMs - lastStatusMs_ >= 5000) {
            lastStatusMs_ = ctx.elapsedMs;
            char b[192];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO MPSTATE spend=%d latch=%d save=%d ack=%d load=%d "
                      "swap=%d post=%d live=%d t=%lu",
                      spendOk_ ? 1 : 0, latched_ ? 1 : 0, saveOk_ ? 1 : 0,
                      ackOk_ ? 1 : 0, loadOk_ ? 1 : 0, swapDone_ ? 1 : 0,
                      postOk_ ? 1 : 0, live ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        bool localDone = ctx.isHost
            ? (saveOk_ && ackOk_ && loadOk_ && checked_)
            : (spendOk_ && checked_);
        if ((localDone && ctx.elapsedMs >= checkAtMs_ + TAIL_HOLD_MS) ||
            ctx.elapsedMs >= DURATION_MS) {
            passed_ = ctx.isHost
                ? (poolMoved_ && saveOk_ && ackOk_ && loadOk_ && swapDone_ && postOk_)
                : (spendOk_ && swapDone_ && postOk_);
            return true;
        }
        return false;
    }

private:
    static const unsigned long SPEND_AT_MS      = 8000;
    static const unsigned long SPEND_DEADLINE_MS = 14000;
    static const unsigned long LATCH_AFTER_MS   = 18000;
    static const unsigned long SWAP_MIN_MS      = 400;
    static const unsigned long SYNC_CONFIRM_MS  = 3000;
    static const unsigned long SETTLE_MS        = 5000;
    static const unsigned long TAIL_HOLD_MS     = 8000;
    static const unsigned long DURATION_MS      = 110000;
    static const int           MIN_POOL         = 4; // a quarter has to be >= 1

    bool          spent_, spendOk_;
    int           startPool_, expect_;
    bool          latched_, poolMoved_;
    bool          saveIssued_, saveOk_, ackSeen_, ackOk_;
    bool          loadIssued_, loadOk_, sigWas2_, swapDone_, checked_, postOk_;
    int           postPool_;
    unsigned long dropStartMs_, sigClearedMs_, checkAtMs_, lastStatusMs_;

    static const char* const SAVE_NAME;
};
const char* const MoneyPersistScenario::SAVE_NAME = "coopresume";


// connect_relink (Phase 14 plan 01, UI-06): the first automated trigger for the
// F2 panel's Connect action. The panel button has been its ONLY live trigger, so
// commit 9011527 - which clears the host join-slot roster at every transport
// session boundary - has never been confirmed in a running game - two attempts to drive
// the panel with synthetic input (keybd_event VK_F2, synthetic mouse_event
// clicks) produced zero [coop-ui] lines because RE_Kenshi's own menu intercepts
// F2. This scenario reaches the SAME handler programmatically, through
// ScenarioContext::relinkSession, which Plugin.cpp wires straight to
// coopUiConnect. No keyboard, no mouse, no panel on screen.
//
// One side RELINKS (the role knob picks it) and the others OBSERVE:
//   relinking side - snapshots its connected roster, calls the adapter, then
//     polls until the pre-relink id set is fully back or the recovery budget
//     expires. PASS = issued AND every id returned inside budget.
//   observing side - passive; logs every roster-count transition, so a second
//     log independently shows the peer go and come back. PASS = it saw a DROP
//     and a RETURN; never saw a drop is a FAIL, not a vacuous pass.
//
// Knobs are read with ::getenv IN THIS TU and are deliberately NOT Config
// fields (the ScenarioCombat KENSHICOOP_BATTLE_N precedent): a Config field
// would change the `effective cfg` banner describeConfig prints, and Phase 14's
// criterion 4 is that a run which never invokes this lever stays byte-identical
// to a pre-phase run. Read-once statics, main-thread only, the shape the
// KENSHICOOP_DEBUG_CENSUS gate uses.
//   KENSHICOOP_RELINK_AT_MS      first relink offset from ARM  (default 40000)
//   KENSHICOOP_RELINK_RECOVER_MS per-relink recovery budget     (default 60000)
//   KENSHICOOP_RELINK_COUNT      relinks to issue               (default 1)
//   KENSHICOOP_RELINK_ROLE       host | join | both             (default host)
//   KENSHICOOP_RELINK_JOIN_ID    WHICH join relinks at N>=3     (default 1)
//
// The role knob is not optional scope: ROADMAP criterion 1 says a CLIENT must
// reconnect, while the roster evidence for 9011527 is host-side (registry_ only
// exists on the host), so one side alone cannot close both criteria. The adapter
// is role-agnostic by construction - it hands the panel handler the process's
// OWN current role - so supporting both sides is a branch here, not a second
// mechanism.
//
// A JOIN relinking mid-session is walking into a known interaction: a join whose
// networking is already running never auto-loads its own save (titleUpdate_hook
// early-returns) and reaches gameplay only via the host's push, retrying connect
// every 2 s. Mid-session that early-return does not apply - the join is already
// in gameplay and only the link dropped - so the 2 s retry alone should carry it
// back. If it does not, that is the finding; do NOT widen the budget to hide it.
class ConnectRelinkScenario : public TimedScenario {
public:
    ConnectRelinkScenario()
        : TimedScenario("connect_relink", 0), // no periodic-evidence gate: this scenario logs on transitions
          issued_(0), waiting_(false), issuedAtMs_(0), budgetEndMs_(0),
          preN_(0), anyIssued_(false), allRecovered_(true), done_(false),
          obsKnown_(false), obsN_(0), obsPeakN_(0),
          dropOpen_(false), sawDrop_(false), cycles_(0), side_(-1) {
        memset(preIds_, 0, sizeof(preIds_));
    }

    // PRE-ARM roster watch, observing side only. The relinking side's schedule
    // runs off the ARMED clock, and the two sides do not arm together (the host
    // can arm on its 45 s peer-ready timeout while the join is still loading),
    // so an observer that only started looking at its own arm could miss the
    // drop entirely. Watching from gameplay start removes that race; the tag is
    // distinct because ctx.elapsedMs here is the GAMEPLAY clock, not the armed
    // one, and the two must never be read as the same series.
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (iRelink(ctx)) return; // never relink before the armed clock exists
        observeRoster(ctx, "prearm");
    }

    virtual void onStart(const ScenarioContext& ctx) {
        // Latch the side ONCE, at arm. computeSide reads ctx.localId, which is 0
        // on a join until WELCOME lands and could in principle move across a
        // relink; a side that flipped mid-run would silently switch this process
        // from producing relink evidence to producing observer evidence.
        side_ = computeSide(ctx) ? 1 : 0;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO relink start host=%d localId=%u side=%s role=%s "
                  "atMs=%lu recoverMs=%lu count=%u adapter=%d",
                  ctx.isHost ? 1 : 0, (unsigned)ctx.localId,
                  (side_ == 1) ? "relink" : "observe", roleName(),
                  atMs(), recoverMs(), count(),
                  ctx.relinkSession ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (done_) return true;
        return iRelink(ctx) ? tickRelink(ctx) : tickObserve(ctx);
    }

private:
    // ---- knobs (read-once statics; main thread only) ------------------------
    static unsigned long atMs() {
        static long v = -1;
        if (v < 0) {
            const char* e = ::getenv("KENSHICOOP_RELINK_AT_MS");
            v = e ? (long)::atoi(e) : 40000;
            if (v < 0) v = 0;
        }
        return (unsigned long)v;
    }
    static unsigned long recoverMs() {
        static long v = -1;
        if (v < 0) {
            const char* e = ::getenv("KENSHICOOP_RELINK_RECOVER_MS");
            v = e ? (long)::atoi(e) : 60000;
            if (v < 1000) v = 1000; // a budget below one connect-retry is unjudgeable
        }
        return (unsigned long)v;
    }
    static unsigned int count() {
        static int v = -1;
        if (v < 0) {
            const char* e = ::getenv("KENSHICOOP_RELINK_COUNT");
            v = e ? ::atoi(e) : 1;
            if (v < 1) v = 1;
            if (v > (int)MAX_RELINKS) v = (int)MAX_RELINKS;
        }
        return (unsigned int)v;
    }
    // 0 = host relinks, 1 = join relinks, 2 = both. First letter only, so
    // 'host'/'join'/'both' and their initials all resolve; anything else is host.
    static int role() {
        static int v = -1;
        if (v < 0) {
            const char* e = ::getenv("KENSHICOOP_RELINK_ROLE");
            char c = (e && e[0]) ? e[0] : 'h';
            v = (c == 'j' || c == 'J') ? 1 : ((c == 'b' || c == 'B') ? 2 : 0);
        }
        return v;
    }
    static const char* roleName() {
        int r = role();
        return (r == 1) ? "join" : ((r == 2) ? "both" : "host");
    }
    // Which JOIN relinks in 'join'/'both' mode (default 1, the lowest join slot).
    static unsigned int relinkJoinId() {
        static int v = -1;
        if (v < 0) {
            const char* e = ::getenv("KENSHICOOP_RELINK_JOIN_ID");
            v = e ? ::atoi(e) : 1;
            if (v < 1) v = 1;
        }
        return (unsigned int)v;
    }
    // EXACTLY ONE process relinks per scheduled slot; everybody else observes.
    // At N=2 "!isHost" said that and nothing more was needed. At N>=3 a bare
    // "!isHost" makes EVERY join relink at the same offset - which is a
    // simultaneous-rejoin shape (a different mechanism from a single in-process
    // relink, and explicitly out of Phase 14's scope), and which additionally
    // leaves NOBODY on the observing side, so the independent second-log
    // evidence this scenario's own header promises would not exist. Pick the
    // designated join by localId instead.
    bool computeSide(const ScenarioContext& ctx) const {
        const int r = role();
        if (ctx.isHost) return (r == 0 || r == 2);
        return (r == 1 || r == 2) && ctx.localId == relinkJoinId();
    }
    bool iRelink(const ScenarioContext& ctx) const {
        return (side_ >= 0) ? (side_ == 1) : computeSide(ctx);
    }

    // Scheduled offset (from ARM) of this process's i-th relink. Single-role:
    // at + i*recover. In `both` mode the sides INTERLEAVE one recovery budget
    // apart - host on the even slots, join on the odd ones - so no two session
    // boundaries ever overlap and each one is separately attributable.
    unsigned long slotMs(unsigned int i, bool isHost) const {
        const unsigned long at = atMs(), rec = recoverMs();
        if (role() == 2) return at + (unsigned long)(2u * i + (isHost ? 0u : 1u)) * rec;
        return at + (unsigned long)i * rec;
    }
    // When the whole schedule (either side) has certainly finished - the observer's
    // own end-of-run, so it logs a RESULT instead of ticking forever.
    unsigned long windowEndMs() const {
        const unsigned long rec = recoverMs();
        const unsigned int  n   = count();
        unsigned long last = (role() == 2)
            ? atMs() + (unsigned long)(2u * (n - 1u) + 1u) * rec
            : atMs() + (unsigned long)(n - 1u) * rec;
        return last + rec;
    }

    // ---- roster helpers -----------------------------------------------------
    static unsigned int snapshot(const ScenarioContext& ctx, unsigned int* out) {
        if (!ctx.connectedPeers) return 0;
        return ctx.connectedPeers(out, MAX_PLAYERS);
    }
    static void idsCsv(const unsigned int* ids, unsigned int n, char* out, unsigned int cap) {
        out[0] = '\0';
        unsigned int used = 0;
        for (unsigned int i = 0; i < n && i < MAX_PLAYERS; ++i) {
            char one[16];
            _snprintf(one, sizeof(one) - 1, (i == 0) ? "%u" : ",%u", (unsigned)ids[i]);
            one[sizeof(one) - 1] = '\0';
            unsigned int len = (unsigned int)strlen(one);
            if (used + len + 1 >= cap) break;
            memcpy(out + used, one, len);
            used += len;
            out[used] = '\0';
        }
    }
    static bool containsAll(const unsigned int* have, unsigned int nHave,
                            const unsigned int* want, unsigned int nWant) {
        for (unsigned int i = 0; i < nWant; ++i) {
            bool found = false;
            for (unsigned int j = 0; j < nHave && !found; ++j)
                if (have[j] == want[i]) found = true;
            if (!found) return false;
        }
        return true;
    }

    // ---- the relinking side -------------------------------------------------
    bool tickRelink(const ScenarioContext& ctx) {
        char ids[80], b[224];

        if (!waiting_ && issued_ < count() && ctx.elapsedMs >= slotMs(issued_, ctx.isHost)) {
            // Snapshot BEFORE the relink: coopUiConnect's sessionResetForUi()
            // clears the connected roster outright, so this set is the thing the
            // reconnect has to rebuild.
            preN_ = snapshot(ctx, preIds_);
            idsCsv(preIds_, preN_, ids, sizeof(ids));
            _snprintf(b, sizeof(b) - 1, "SCENARIO relink pre t=%lu peers=%u ids={%s}",
                      ctx.elapsedMs, preN_, ids);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);

            if (!ctx.relinkSession) {
                // Same discipline as the other three adapters: the pointer is 0
                // when Plugin.cpp did not supply it. Nothing to measure - fail
                // loudly rather than idle to a vacuous pass.
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO relink skip t=%lu reason=no-adapter", ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                setPassed(false);
                done_ = true;
                return true;
            }

            ++issued_;
            anyIssued_ = true;
            waiting_     = true;
            issuedAtMs_  = ctx.elapsedMs;
            budgetEndMs_ = ctx.elapsedMs + recoverMs();
            // Emitted BEFORE the call so the evidence exists even if the session
            // boundary itself faults (the use-after-free 9011527 fixed lives here).
            _snprintf(b, sizeof(b) - 1, "SCENARIO relink issued t=%lu n=%u",
                      ctx.elapsedMs, issued_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            ctx.relinkSession();
            return false;
        }

        if (waiting_) {
            unsigned int cur[MAX_PLAYERS];
            unsigned int n    = snapshot(ctx, cur);
            bool         back = (preN_ > 0) && containsAll(cur, n, preIds_, preN_);
            if (!back && ctx.elapsedMs < budgetEndMs_) return false;
            waiting_ = false;
            if (!back) allRecovered_ = false;
            idsCsv(cur, n, ids, sizeof(ids));
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO relink post t=%lu peers=%u ids={%s} recoveredMs=%lu ok=%d",
                      ctx.elapsedMs, n, ids,
                      (unsigned long)(ctx.elapsedMs - issuedAtMs_), back ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (!waiting_ && issued_ >= count()) {
            // In `both` mode the two sides INTERLEAVE one recovery budget apart,
            // so this side's last slot is NOT the run's last slot: the host owns
            // the even slots and the designated join the odd ones, and the join's
            // final relink lands a full budget AFTER the host's. Finishing the
            // moment this side is done ends the scenario, which exits the
            // process - and a host that leaves takes the joins' remaining
            // boundaries with it.
            //
            // This never showed before WINDOWS #19 was fixed only because the
            // defect hid it: the host's second recovery used to time out on the
            // full 60 s budget (recoveredMs=60000 ok=0), which happened to keep
            // the host alive exactly long enough. Now it recovers in ~94 ms and
            // the host would otherwise leave 60 s early. Measured:
            // tools/test-runs/20260912_184538_N3_relink - host "SCENARIO RESULT
            // PASS" 18:48:59.550, both joins "disconnected from host" 18:49:08.995
            // and never reconnecting. Hold to the shared window instead; it is
            // the same clock the observing side already backstops on.
            if (role() == 2 && ctx.elapsedMs < windowEndMs()) return false;
            setPassed(anyIssued_ && allRecovered_);
            done_ = true;
            return true;
        }
        return false;
    }

    // ---- the observing side -------------------------------------------------
    // One roster sample. Emits only on a COUNT TRANSITION and counts completed
    // drop->return cycles. `tag` names the clock ctx.elapsedMs is on: "peer" for
    // the armed clock, "prearm" for the gameplay clock (onGameplay).
    void observeRoster(const ScenarioContext& ctx, const char* tag) {
        unsigned int cur[MAX_PLAYERS];
        unsigned int n = snapshot(ctx, cur);
        if (obsKnown_ && n == obsN_) return;
        char ids[80], b[224];
        idsCsv(cur, n, ids, sizeof(ids));
        _snprintf(b, sizeof(b) - 1, "SCENARIO relink %s t=%lu peers=%u ids={%s}",
                  tag, ctx.elapsedMs, n, ids);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (n < obsPeakN_) {
            dropOpen_ = true;
            sawDrop_  = true;
        } else if (dropOpen_ && n >= obsPeakN_) {
            dropOpen_ = false;
            ++cycles_;   // one full drop -> return observed
        }
        if (n > obsPeakN_) obsPeakN_ = n;
        obsKnown_ = true;
        obsN_     = n;
    }

    bool tickObserve(const ScenarioContext& ctx) {
        observeRoster(ctx, "peer");
        // Finish as soon as every scheduled relink has been WITNESSED as a full
        // drop->return, rather than idling to the end of the schedule window.
        // That is the evidence this side exists to produce, and holding on past
        // it is not free: with save sync on, the host re-arms a connect-push to
        // the peer it just re-admitted, so this client may be reloaded shortly
        // after the return - the RESULT must be on disk before that lands. The
        // window is still the backstop, and it FAILS on no drop rather than
        // passing vacuously.
        bool complete = (cycles_ >= count());
        // `both` mode: hold to the shared window for the same reason tickRelink
        // does. This side's cycles are the HOST boundaries - the ones that drop
        // this client's own link - and the last of those is not the run's last
        // slot. Leaving early removes this client from the host's roster while
        // the host still has a boundary to recover, and the host's own
        // relink-post predicate then never sees the id set it discarded.
        //
        // Note what this side can and cannot witness now. A host boundary
        // empties this client's roster and is always visible. The relinking
        // JOIN's boundary is NOT, by design: since the clean-disconnect fix its
        // leave and re-admit land in one main-thread drain, and Plugin.cpp
        // deliberately skips a leave superseded by a same-drain re-admit, so
        // g_connectedPeers never dips - there is no transition to sample.
        // Counting it would make this side's verdict a poll-timing race
        // (measured: 2 of 4 such edges caught in
        // tools/test-runs/20260912_185407_N3_relink, 3 of 4 in ...184538...).
        // The relinking join proves its own recovery from its own log; this
        // side's job is the independent second observation of the HOST
        // boundaries, which is exactly count().
        if (role() == 2 && ctx.elapsedMs < windowEndMs()) return false;
        if (!complete && ctx.elapsedMs < windowEndMs()) return false;
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO relink observed t=%lu drop=%d cycles=%u/%u peak=%u via=%s",
                  ctx.elapsedMs, sawDrop_ ? 1 : 0, cycles_, count(), obsPeakN_,
                  complete ? "cycles" : "window");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        setPassed(complete);
        done_ = true;
        return true;
    }

    static const unsigned int MAX_RELINKS = 8;

    // relinking side
    unsigned int  issued_;
    bool          waiting_;
    unsigned long issuedAtMs_, budgetEndMs_;
    unsigned int  preIds_[MAX_PLAYERS];
    unsigned int  preN_;
    bool          anyIssued_, allRecovered_, done_;
    // observing side
    bool          obsKnown_;
    unsigned int  obsN_, obsPeakN_;
    bool          dropOpen_, sawDrop_;
    unsigned int  cycles_;
    // -1 = not yet latched (pre-arm), 0 = observe, 1 = relink. See onStart.
    int           side_;
};

} // namespace

Scenario* makeSessionScenario(const std::string& name) {
    if (name == "latejoin_probe") return new LatejoinProbeScenario(true);
    if (name == "latejoin_sync")  return new LatejoinProbeScenario(false);
    if (name == "save_probe")     return new SaveProbeScenario();
    if (name == "save_sync")      return new SaveSyncScenario(/*stage1=*/false);
    if (name == "save_stage1")    return new SaveSyncScenario(/*stage1=*/true);
    if (name == "resume_check")   return new ResumeCheckScenario();
    if (name == "load_probe")     return new LoadProbeScenario();
    if (name == "load_sync")      return new LoadSyncScenario();
    if (name == "money_persist")  return new MoneyPersistScenario();
    if (name == "connect_relink") return new ConnectRelinkScenario();
    return 0;
}

} // namespace coop
