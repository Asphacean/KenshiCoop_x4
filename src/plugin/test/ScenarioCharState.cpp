// ScenarioCharState.cpp - carry/furniture/stealth/speed scenarios (monolith
// split from Scenario.cpp, 2026-07-12): carry_order, npc_carry, bed_pose,
// bed_put, cage_put, chain_put, cage_peer_sync, sneak_probe, sneak_pose, sneak_detect,
// speed_sync, speed_probe. Classes are TU-private (anonymous namespace);
// only makeCharStateScenario (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"
#include "PauseSchedule.h" // pause_stress cycle schedule (pure; unit-tested in prototest)

namespace coop {
namespace {


// carry_order (protocol-18 carried-body replication validation, save squad1:
// host tab = leader L0 + second member M2, join tab = leader L1 only). Three
// windows, covering own-tab and BOTH cross-tab directions:
//   A (own-tab, host authors):   host downs M2, L0 picks it up, walks a short
//                                leg, ragdoll-drops it. M2 stays KO'd.
//   B (cross-tab, join carrier): join's L1 picks up the still-down HOST-owned
//                                M2 (carrier owner != carried owner), walks,
//                                drops.
//   C (cross-tab, host carrier): join downs its own L1; host's L0 picks it
//                                up (the other ownership direction), walks,
//                                drops; join revives L1 at the end.
// Both sides log "SCENARIO CARRY" for all three bodies at 2 Hz (isCarrying +
// carried hand, isBeingCarried, position, bodyState). The Test-CarryOrder
// oracle gates per window: the pickup CROSSES (the peer's local pair enters
// the carried state within a latency budget), the carried copy TRACKS its
// carrier while carried (median carrier-carried distance small - the dragged/
// teleported-body artifact shows up as huge gaps), and the drop crosses (the
// peer's copy leaves the carried state within budget).
class CarryOrderScenario : public TimedScenario {
public:
    CarryOrderScenario()
        : TimedScenario("carry_order", 0), recvCount_(0), lastLogMs_(0),
          haveL0_(false), haveM2_(false), haveL1_(false),
          aDown_(false), aPick_(false), aWalk_(false), aDrop_(false),
          bPick_(false), bWalk_(false), bDrop_(false),
          cDown_(false), cPick_(false), cWalk_(false), cDrop_(false),
          cRevive_(false), lastHoldMs_(0) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_ || !haveM2_ || !haveL1_) latchSubjects(ctx);

        // ---- Window A (host, own-tab): down M2, L0 carries it ----------------
        if (ctx.isHost && haveM2_ && !aDown_ && ctx.elapsedMs >= A_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
            logAct("A down", m2Hand_, ok); aDown_ = true;
        }
        if (ctx.isHost && haveL0_ && haveM2_ && !aPick_ && ctx.elapsedMs >= A_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, l0Hand_, m2Hand_);
            logAct("A pickup", m2Hand_, ok); aPick_ = true;
        }
        if (ctx.isHost && haveL0_ && !aWalk_ && ctx.elapsedMs >= A_WALK_AT_MS) {
            aWalk_ = walkLeg(l0Hand_, "A");
        }
        if (ctx.isHost && haveL0_ && !aDrop_ && ctx.elapsedMs >= A_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, l0Hand_, /*ragdoll*/true);
            logAct("A drop", l0Hand_, ok); aDrop_ = true;
        }

        // Keep the KO'd subjects DOWN through their carry legs: the scaffold KO
        // (knockoutForceTimer 8s) expires mid-carry and the engine truthfully
        // stands the body back up, ending the carry at the drop (run 191905:
        // M2 revived as it was dropped, bs 2->0, so the oracle's still-down
        // gate read bs=0). Each subject's OWNER re-tops the timer every 2s
        // (holdDown = timer-only, no fresh knockout on the shoulder) while its
        // windows need the body down - owner-side, so the authoritative
        // medical stream carries the KO state to the peer.
        if (ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            if (ctx.isHost && haveM2_ && aDown_ && ctx.elapsedMs < HOLD_M2_UNTIL_MS)
                holdSubject(m2Hand_);
            if (!ctx.isHost && haveL1_ && cDown_ && !cRevive_)
                holdSubject(l1Hand_);
        }

        // ---- Window B (join carrier, cross-tab): L1 carries the host's M2 ----
        if (!ctx.isHost && haveL1_ && haveM2_ && !bPick_ && ctx.elapsedMs >= B_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, l1Hand_, m2Hand_);
            logAct("B pickup", m2Hand_, ok); bPick_ = true;
        }
        if (!ctx.isHost && haveL1_ && !bWalk_ && ctx.elapsedMs >= B_WALK_AT_MS) {
            bWalk_ = walkLeg(l1Hand_, "B");
        }
        if (!ctx.isHost && haveL1_ && !bDrop_ && ctx.elapsedMs >= B_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, l1Hand_, /*ragdoll*/true);
            logAct("B drop", l1Hand_, ok); bDrop_ = true;
        }

        // ---- Window C (host carrier, cross-tab): L0 carries the join's L1 ----
        if (!ctx.isHost && haveL1_ && !cDown_ && ctx.elapsedMs >= C_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, l1Hand_);
            logAct("C down", l1Hand_, ok); cDown_ = true;
        }
        if (ctx.isHost && haveL0_ && haveL1_ && !cPick_ && ctx.elapsedMs >= C_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, l0Hand_, l1Hand_);
            logAct("C pickup", l1Hand_, ok); cPick_ = true;
        }
        if (ctx.isHost && haveL0_ && !cWalk_ && ctx.elapsedMs >= C_WALK_AT_MS) {
            cWalk_ = walkLeg(l0Hand_, "C");
        }
        if (ctx.isHost && haveL0_ && !cDrop_ && ctx.elapsedMs >= C_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, l0Hand_, /*ragdoll*/true);
            logAct("C drop", l0Hand_, ok); cDrop_ = true;
        }
        if (!ctx.isHost && haveL1_ && !cRevive_ && ctx.elapsedMs >= C_REVIVE_AT_MS) {
            bool ok = engine::reviveSubject(ctx.gw, l1Hand_);
            logAct("C revive", l1Hand_, ok); cRevive_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL0_) logCarryLine(l0Hand_, ctx.elapsedMs);
            if (haveM2_) logCarryLine(m2Hand_, ctx.elapsedMs);
            if (haveL1_) logCarryLine(l1Hand_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aDown_ && aPick_ && aDrop_ && cPick_ && cDrop_)
                                 : (bPick_ && bDrop_ && cDown_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared timeline (peer-armed clock on both sides). Each carry leg is
    // pickup +2s walk +8s drop, with 6s of settle between windows.
    static const unsigned long A_DOWN_AT_MS    = 8000;
    static const unsigned long A_PICK_AT_MS    = 12000;
    static const unsigned long A_WALK_AT_MS    = 14000;
    static const unsigned long A_DROP_AT_MS    = 22000;
    static const unsigned long B_PICK_AT_MS    = 30000;
    static const unsigned long B_WALK_AT_MS    = 32000;
    static const unsigned long B_DROP_AT_MS    = 40000;
    static const unsigned long C_DOWN_AT_MS    = 46000;
    static const unsigned long C_PICK_AT_MS    = 50000;
    static const unsigned long C_WALK_AT_MS    = 52000;
    static const unsigned long C_DROP_AT_MS    = 60000;
    static const unsigned long C_REVIVE_AT_MS  = 64000;
    // Keep M2 KO'd through both its carry legs (A + B) plus post-drop settle;
    // after this it may wake naturally (nothing gates on it).
    static const unsigned long HOLD_M2_UNTIL_MS = 44000;
    static const unsigned long HOST_DURATION_MS = 72000;
    static const unsigned long JOIN_DURATION_MS = 68000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Re-top the KO timer on our own KO'd subject (timer-only; never a fresh
    // knockout while the body rides a shoulder).
    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    // Order the carrier to walk a short leg (+10u east of where it stands).
    // The carried body must FOLLOW via its local shoulder attach on both sides.
    bool walkLeg(const unsigned int h[5], const char* wTag) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false;
        float x, y, z;
        if (!engine::readPos(c, &x, &y, &z)) return false;
        bool ok = engine::orderMoveTo(c, x + 10.0f, y, z);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s walk hand=%u,%u ok=%d",
                  wTag, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

    // One "SCENARIO CARRY" line: this body's LOCAL carry relationship + pose.
    void logCarryLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        engine::CarryRead cr;
        if (!engine::readCarry(c, &cr) || !cr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CARRY hand=%u,%u t=%lu carrying=%d carried=%u,%u "
                  "beingCarried=%d pos=%.2f,%.2f,%.2f bs=%u",
                  h[3], h[4], t, cr.carrying ? 1 : 0,
                  cr.carried[3], cr.carried[4], cr.beingCarried ? 1 : 0,
                  x, y, z, (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveL0_) {
            int idx = tabLeaderIdx(sq, n, 0);
            if (idx >= 0) {
                handFromEntity(sq[idx], l0Hand_);
                haveL0_ = true;
                logSubject("L0", l0Hand_);
            }
        }
        if (haveL0_ && !haveM2_) {
            // Host tab's SECOND member: the lowest hand of rank 0 that is not L0.
            int best = -1;
            for (unsigned int i = 0; i < n; ++i) {
                if (tabRankOf(sq, n, i) != 0) continue;
                unsigned int h[5]; handFromEntity(sq[i], h);
                if (h[3] == l0Hand_[3] && h[4] == l0Hand_[4]) continue;
                if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
            }
            if (best >= 0) {
                handFromEntity(sq[best], m2Hand_);
                haveM2_ = true;
                logSubject("M2", m2Hand_);
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_);
                haveL1_ = true;
                logSubject("L1", l1Hand_);
            }
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRY %s hand=%u,%u", who, h[3], h[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_, haveM2_, haveL1_;
    bool          aDown_, aPick_, aWalk_, aDrop_;
    bool          bPick_, bWalk_, bDrop_;
    bool          cDown_, cPick_, cWalk_, cDrop_, cRevive_;
    unsigned long lastHoldMs_;
    unsigned int  l0Hand_[5];
    unsigned int  m2Hand_[5];
    unsigned int  l1Hand_[5];
};

// npc_carry (protocol 18, world-NPC carrier extension): the 2026-07-07 remote
// session's third gap - a HOST-side world NPC hauling a downed player character
// never replicated to the join. One window: the host KO's its second member
// (M2), then DIRECTS the nearest world NPC to pick it up, walk a leg, and drop
// it (carrySubject/dropSubject run the same engine calls the NPC AI does). The
// NPC is host-owned world state, so the edge events now ride publishOwned's NPC
// extension and the join's replicator must execute the pickup on its LOCAL
// NPC copy. The join is a passive observer: it never knows a priori which NPC
// the host chose - it DETECTS the carrier from its own local world (the NPC
// copy whose readCarry.carried == M2's hand) and latches it, which is itself
// evidence the pickup applied locally. Both sides log the same "SCENARIO CARRY"
// series as carry_order; Test-NpcCarry gates pickup-crossed / tracks-carrier /
// drop-crossed on the M2 + NPC series.
class NpcCarryScenario : public TimedScenario {
public:
    NpcCarryScenario()
        : TimedScenario("npc_carry", 0), recvCount_(0), lastLogMs_(0),
          haveL0_(false), haveM2_(false), haveNpc_(false),
          nDown_(false), nPick_(false), nWalk_(false), nDrop_(false),
          lastHoldMs_(0) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_ || !haveM2_) latchSubjects(ctx);

        // Host: pick the carrier - the world NPC nearest L0 (upright ones only;
        // a KO'd body can't carry). Latched once, logged as the NPC role line.
        if (ctx.isHost && haveL0_ && !haveNpc_ && ctx.elapsedMs >= NPC_LATCH_AT_MS)
            latchHostNpc(ctx);
        // Join: detect the carrier - the local NPC copy that reads carrying=M2.
        // Finding one IS the feature working (the join executed the pickup).
        if (!ctx.isHost && haveM2_ && !haveNpc_) detectJoinCarrier(ctx);

        // ---- Window N (host): down M2, NPC carries it, walks, drops ----------
        if (ctx.isHost && haveM2_ && !nDown_ && ctx.elapsedMs >= N_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
            logAct("N down", m2Hand_, ok); nDown_ = true;
        }
        if (ctx.isHost && haveNpc_ && haveM2_ && !nPick_ && ctx.elapsedMs >= N_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, npcHand_, m2Hand_);
            logAct("N pickup", m2Hand_, ok); nPick_ = true;
        }
        if (ctx.isHost && haveNpc_ && !nWalk_ && ctx.elapsedMs >= N_WALK_AT_MS) {
            nWalk_ = walkLeg(npcHand_, "N");
        }
        if (ctx.isHost && haveNpc_ && !nDrop_ && ctx.elapsedMs >= N_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, npcHand_, /*ragdoll*/true);
            logAct("N drop", npcHand_, ok); nDrop_ = true;
        }

        // Keep M2 KO'd through the carry leg (the scaffold KO timer expires
        // mid-carry otherwise - the carry_order lesson). Host-owned, so the
        // host re-tops it; timer-only, never a fresh knockout on the shoulder.
        if (ctx.isHost && haveM2_ && nDown_ && ctx.elapsedMs < HOLD_M2_UNTIL_MS &&
            ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            holdSubject(m2Hand_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveM2_)  logCarryLine(m2Hand_, ctx.elapsedMs);
            if (haveNpc_) logCarryLine(npcHand_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (haveNpc_ && nDown_ && nPick_ && nDrop_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long NPC_LATCH_AT_MS  = 6000;
    static const unsigned long N_DOWN_AT_MS     = 8000;
    static const unsigned long N_PICK_AT_MS     = 12000;
    static const unsigned long N_WALK_AT_MS     = 14000;
    static const unsigned long N_DROP_AT_MS     = 24000;
    static const unsigned long HOLD_M2_UNTIL_MS = 30000;
    static const unsigned long HOST_DURATION_MS = 40000;
    static const unsigned long JOIN_DURATION_MS = 36000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const unsigned int  MAX_NPCS         = 96;

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    bool walkLeg(const unsigned int h[5], const char* wTag) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false;
        float x, y, z;
        if (!engine::readPos(c, &x, &y, &z)) return false;
        bool ok = engine::orderMoveTo(c, x + 10.0f, y, z);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s walk hand=%u,%u ok=%d",
                  wTag, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

    void logCarryLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        engine::CarryRead cr;
        if (!engine::readCarry(c, &cr) || !cr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CARRY hand=%u,%u t=%lu carrying=%d carried=%u,%u "
                  "beingCarried=%d pos=%.2f,%.2f,%.2f bs=%u",
                  h[3], h[4], t, cr.carrying ? 1 : 0,
                  cr.carried[3], cr.carried[4], cr.beingCarried ? 1 : 0,
                  x, y, z, (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveL0_) {
            int idx = tabLeaderIdx(sq, n, 0);
            if (idx >= 0) {
                handFromEntity(sq[idx], l0Hand_);
                haveL0_ = true;
                logSubject("L0", l0Hand_);
            }
        }
        if (haveL0_ && !haveM2_) {
            int best = -1;
            for (unsigned int i = 0; i < n; ++i) {
                if (tabRankOf(sq, n, i) != 0) continue;
                unsigned int h[5]; handFromEntity(sq[i], h);
                if (h[3] == l0Hand_[3] && h[4] == l0Hand_[4]) continue;
                if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
            }
            if (best >= 0) {
                handFromEntity(sq[best], m2Hand_);
                haveM2_ = true;
                logSubject("M2", m2Hand_);
            }
        }
    }

    // Host: nearest UPRIGHT world NPC to L0 becomes the directed carrier.
    void latchHostNpc(const ScenarioContext& ctx) {
        Character* l0 = engine::resolveCharByHand(l0Hand_[3], l0Hand_[4],
                                                  l0Hand_[0], l0Hand_[1], l0Hand_[2]);
        float lx, ly, lz;
        if (!l0 || !engine::readPos(l0, &lx, &ly, &lz)) return;
        static Character*  chars[MAX_NPCS]; // main-thread only
        static EntityState states[MAX_NPCS];
        unsigned int n = engine::listNpcs(ctx.gw, chars, states, MAX_NPCS);
        int best = -1; float bestD2 = 1e18f;
        for (unsigned int i = 0; i < n; ++i) {
            if (coop::bodyIsDown(states[i].bodyState) ||
                (states[i].bodyState & BODY_DEAD) != 0) continue;
            float dx = states[i].x - lx, dy = states[i].y - ly, dz = states[i].z - lz;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < bestD2) { bestD2 = d2; best = (int)i; }
        }
        if (best < 0) return;
        npcHand_[0] = states[best].hType;
        npcHand_[1] = states[best].hContainer;
        npcHand_[2] = states[best].hContainerSerial;
        npcHand_[3] = states[best].hIndex;
        npcHand_[4] = states[best].hSerial;
        haveNpc_ = true;
        logSubject("NPC", npcHand_);
    }

    // Join: the carrier reveals itself - the local NPC copy carrying M2.
    void detectJoinCarrier(const ScenarioContext& ctx) {
        static Character*  chars[MAX_NPCS]; // main-thread only
        static EntityState states[MAX_NPCS];
        unsigned int n = engine::listNpcs(ctx.gw, chars, states, MAX_NPCS);
        for (unsigned int i = 0; i < n; ++i) {
            engine::CarryRead cr;
            if (!engine::readCarry(chars[i], &cr) || !cr.carrying) continue;
            if (cr.carried[3] != m2Hand_[3] || cr.carried[4] != m2Hand_[4]) continue;
            npcHand_[0] = states[i].hType;
            npcHand_[1] = states[i].hContainer;
            npcHand_[2] = states[i].hContainerSerial;
            npcHand_[3] = states[i].hIndex;
            npcHand_[4] = states[i].hSerial;
            haveNpc_ = true;
            logSubject("NPC", npcHand_);
            return;
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRY %s hand=%u,%u", who, h[3], h[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_, haveM2_, haveNpc_;
    bool          nDown_, nPick_, nWalk_, nDrop_;
    unsigned long lastHoldMs_;
    unsigned int  l0Hand_[5];
    unsigned int  m2Hand_[5];
    unsigned int  npcHand_[5];
};

// bed_pose (protocol 19 phase 1, conscious bed use): USE_BED / USE_BED_ORDER
// have been on the reproducible-pose allowlist since the fixture-pose work but
// were never runtime-validated (spike 24 PARTIAL). Save 'bedcage1' bakes a Camp
// Bed + Prisoner Cage with save-stable hands next to the squad. The HOST orders
// its own leader (L0) to USE_BED_ORDER at the baked bed via the player-order
// path; the stream carries the committed bed task + fixture subject hand and
// the JOIN's driven L0 copy must commit the same pose at the same bed. Both
// sides log the standard MEMBER/RECV series (task= + pelvis=); Test-BedPose
// anchors on the "SCENARIO BED ORDER" marker and gates host-committed /
// join-committed / co-located.
class BedPoseScenario : public TimedScenario {
public:
    BedPoseScenario()
        : TimedScenario("bed_pose", 0), recvCount_(0), lastLogMs_(0), haveL0_(false),
          orderLogged_(false), lastOrderMs_(0), orderOk_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_) latchLeader(ctx);

        // HOST, order phase: send L0 to the baked bed. orderUseBed is GUARDED
        // (no-op once L0 is on a bed task), so the throttled re-issue recovers
        // a failed first order without ever standing the sleeper back up.
        if (ctx.isHost && haveL0_ && ctx.elapsedMs >= ORDER_AT_MS &&
            (!orderLogged_ || (!orderOk_ && ctx.elapsedMs - lastOrderMs_ >= 4000))) {
            lastOrderMs_ = ctx.elapsedMs;
            int ordered = 0, useBed = 0;
            orderOk_ = engine::orderUseBed(ctx.gw, l0Hand_, &ordered, &useBed);
            if (!orderLogged_) {
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO BED ORDER issued hand=%u,%u task=%d accept=%d,%d ok=%d",
                          l0Hand_[3], l0Hand_[4], ordered, ordered, useBed,
                          orderOk_ ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                orderLogged_ = true;
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (orderLogged_ && orderOk_) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long ORDER_AT_MS      = 14000; // join logs standing baseline first
    static const unsigned long HOST_DURATION_MS = 60000; // approach + in-bed observation
    static const unsigned long JOIN_DURATION_MS = 54000;
    static const unsigned int  MAX_SQUAD        = 32;

    void latchLeader(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 0); // host tab's leader on BOTH sides
        if (idx >= 0) {
            handFromEntity(sq[idx], l0Hand_);
            haveL0_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO BED L0 hand=%u,%u",
                      l0Hand_[3], l0Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_;
    bool          orderLogged_;
    unsigned long lastOrderMs_;
    bool          orderOk_;
    unsigned int  l0Hand_[5];
};

// bed_wake (protocol 19, conscious bed EXIT / wake-and-move): bed_pose only
// validated ENTERING and HOLDING the pose - never the transition OUT. A host
// PC that sleeps, then wakes and WALKS left the join copy stuck sleeping (no
// reliable EXIT edge for a bed pose; the join relied on a 3 s self-heal and
// was never AI-suspended, so its local AI re-slept it - pole save 2026-07-17).
// This drives the full arc on save 'bedcage1': host orders L0 into the baked
// bed (USE_BED_ORDER), waits for the JOIN to commit the pose, then issues a
// MOVE order ~25u away. The join's driven L0 copy must LEAVE the bed (bs loses
// BODY_IN_BED, the [furn] BED FAST-EXIT fires) and co-locate with the host.
// Test-BedWake anchors on "SCENARIO BEDWAKE ORDER/MOVE" and gates:
// host-entered / host-moved-away / join-left-bed / join-co-located.
class BedWakeScenario : public TimedScenario {
public:
    BedWakeScenario()
        : TimedScenario("bed_wake", 0), recvCount_(0), lastLogMs_(0), haveL0_(false),
          orderLogged_(false), lastOrderMs_(0), orderOk_(false),
          moveLogged_(false), lastMoveMs_(0), moveOk_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_) latchLeader(ctx);

        // Phase 1 - host orders L0 to the baked bed (guarded re-issue on failure,
        // same as bed_pose: orderUseBed is a no-op once L0 is on a bed task).
        if (ctx.isHost && haveL0_ && ctx.elapsedMs >= ORDER_AT_MS &&
            ctx.elapsedMs < MOVE_AT_MS &&
            (!orderLogged_ || (!orderOk_ && ctx.elapsedMs - lastOrderMs_ >= 4000))) {
            lastOrderMs_ = ctx.elapsedMs;
            int ordered = 0, useBed = 0;
            orderOk_ = engine::orderUseBed(ctx.gw, l0Hand_, &ordered, &useBed);
            if (!orderLogged_) {
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO BEDWAKE ORDER issued hand=%u,%u task=%d ok=%d",
                          l0Hand_[3], l0Hand_[4], ordered, orderOk_ ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                orderLogged_ = true;
            }
        }

        // Phase 2 - after the join has had time to commit the pose, wake L0 and
        // walk it well clear of the bed. Re-issue while it is still reported
        // IN_BED (a move order can be absorbed on the first sleeping frame).
        if (ctx.isHost && haveL0_ && ctx.elapsedMs >= MOVE_AT_MS &&
            (!moveLogged_ || (ctx.elapsedMs - lastMoveMs_ >= 3000 && stillInBed(ctx)))) {
            lastMoveMs_ = ctx.elapsedMs;
            Character* l0 = engine::resolveCharByHand(l0Hand_[3], l0Hand_[4],
                                                      l0Hand_[0], l0Hand_[1],
                                                      l0Hand_[2]);
            float x = 0, y = 0, z = 0;
            if (l0 && engine::readPos(l0, &x, &y, &z)) {
                moveOk_ = engine::orderMoveTo(l0, x + 25.0f, y, z);
            }
            if (!moveLogged_) {
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO BEDWAKE MOVE issued hand=%u,%u to=%.2f,%.2f,%.2f ok=%d",
                          l0Hand_[3], l0Hand_[4], x + 25.0f, y, z, moveOk_ ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                moveLogged_ = true;
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (orderLogged_ && orderOk_ && moveLogged_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long ORDER_AT_MS      = 14000; // join logs standing baseline first
    static const unsigned long MOVE_AT_MS       = 34000; // give the join time to commit the pose
    static const unsigned long HOST_DURATION_MS = 64000; // enter + observe + wake-move + follow
    static const unsigned long JOIN_DURATION_MS = 60000;
    static const unsigned int  MAX_SQUAD        = 32;

    // Host-side: is L0 still reported occupying the bed (drives the move re-issue)?
    bool stillInBed(const ScenarioContext& ctx) {
        (void)ctx;
        Character* l0 = engine::resolveCharByHand(l0Hand_[3], l0Hand_[4],
                                                  l0Hand_[0], l0Hand_[1],
                                                  l0Hand_[2]);
        if (!l0) return false;
        return (engine::readBodyState(l0) & BODY_IN_BED) != 0;
    }

    void latchLeader(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 0); // host tab's leader on BOTH sides
        if (idx >= 0) {
            handFromEntity(sq[idx], l0Hand_);
            haveL0_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO BEDWAKE L0 hand=%u,%u",
                      l0Hand_[3], l0Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_;
    bool          orderLogged_;
    unsigned long lastOrderMs_;
    bool          orderOk_;
    bool          moveLogged_;
    unsigned long lastMoveMs_;
    bool          moveOk_;
    unsigned int  l0Hand_[5];
};

// bed_lay (protocol 19, UNCONSCIOUS place-in-bed LAYING POSE + wake-and-exit):
// bed_pose validates a CONSCIOUS SLEEP ORDER (task=USE_BED, walk-in + lie down)
// and bed_put validates UNCONSCIOUS occupancy (BODY_IN_BED bit crossing). Neither
// checks that a KO'd body DROPPED into a bed - the real "carry an unconscious
// squadmate to a bed" case - actually renders the LAYING pose on both clients,
// nor that it can get back OUT when it wakes. (A CONSCIOUS placement was tried
// first and proved a dead end: Kenshi itself nondeterministically leaves a
// conscious placed body STANDING on the mattress, and the join mirrors that
// faithfully - so "standing on the bed" for a conscious body is base-game
// behavior, not a coop bug: manual + bed_lay-conscious run 2026-07-17.) This
// drives the deterministic arc on save 'bedcage1', once host-own (M2) and once
// join-own (L1): KO the subject (held down), DROP it into the baked bed
// (setBedMode, re-issued until it lands), observe it LAYING, then REVIVE it, take
// it out and MOVE it clear. Test-BedLay gates from the AUTHORITATIVE pelvis
// height + BODY_IN_BED in the MEMBER/RECV series: (1) both clients read the KO'd
// body IN_BED with a LOW (laying) pelvis, and (2) after the wake both clients
// have it OUT of the bed and co-located (it can get up and leave).
class BedLayScenario : public TimedScenario {
public:
    BedLayScenario()
        : TimedScenario("bed_lay", 0), recvCount_(0), lastLogMs_(0),
          haveM2_(false), haveL1_(false),
          lastPutMs_(0), lastHoldMs_(0), lastMoveMs_(0),
          aDown_(false), aPut_(false), aPutOk_(false), aWake_(false), aMoved_(false),
          bDown_(false), bPut_(false), bPutOk_(false), bWake_(false), bMoved_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveM2_ || !haveL1_) latchSubjects(ctx);

        // ---- Window A (host owns M2): KO -> drop in bed -> revive -> move ----
        if (ctx.isHost && haveM2_) {
            if (!aDown_ && ctx.elapsedMs >= A_DOWN_AT_MS) {
                bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
                logAct("A down", m2Hand_, ok); aDown_ = true;
            }
            // Drop the KO'd body into the bed; re-issue until it occupies it.
            if (aDown_ && !aWake_ && ctx.elapsedMs >= A_PUT_AT_MS &&
                ctx.elapsedMs < A_WAKE_AT_MS &&
                (!aPut_ || (ctx.elapsedMs - lastPutMs_ >= REPUT_MS && !subjectInBed(m2Hand_)))) {
                lastPutMs_ = ctx.elapsedMs;
                bool ok = engine::putSubjectInFurniture(ctx.gw, m2Hand_, 1, true);
                if (ok) aPutOk_ = true;
                if (!aPut_) { logPut("A", m2Hand_, ok); aPut_ = true; }
            }
            // Wake: revive + take out of the bed (deterministic exit trigger).
            if (!aWake_ && ctx.elapsedMs >= A_WAKE_AT_MS) {
                bool rok = engine::reviveSubject(ctx.gw, m2Hand_);
                engine::putSubjectInFurniture(ctx.gw, m2Hand_, 1, false);
                aWake_ = true; logAct("A wake", m2Hand_, rok);
            }
            // Move clear so the copy has to leave + follow (re-issue while in bed).
            if (aWake_ && (!aMoved_ ||
                (ctx.elapsedMs - lastMoveMs_ >= 3000 && subjectInBed(m2Hand_)))) {
                lastMoveMs_ = ctx.elapsedMs;
                if (issueMove(m2Hand_) && !aMoved_) { logAct("A move", m2Hand_, true); aMoved_ = true; }
            }
        }

        // ---- Window B (join owns L1): the same over L1 ----------------------
        if (!ctx.isHost && haveL1_) {
            if (!bDown_ && ctx.elapsedMs >= B_DOWN_AT_MS) {
                bool ok = engine::orderDownSubject(ctx.gw, l1Hand_);
                logAct("B down", l1Hand_, ok); bDown_ = true;
            }
            if (bDown_ && !bWake_ && ctx.elapsedMs >= B_PUT_AT_MS &&
                ctx.elapsedMs < B_WAKE_AT_MS &&
                (!bPut_ || (ctx.elapsedMs - lastPutMs_ >= REPUT_MS && !subjectInBed(l1Hand_)))) {
                lastPutMs_ = ctx.elapsedMs;
                bool ok = engine::putSubjectInFurniture(ctx.gw, l1Hand_, 1, true);
                if (ok) bPutOk_ = true;
                if (!bPut_) { logPut("B", l1Hand_, ok); bPut_ = true; }
            }
            if (!bWake_ && ctx.elapsedMs >= B_WAKE_AT_MS) {
                bool rok = engine::reviveSubject(ctx.gw, l1Hand_);
                engine::putSubjectInFurniture(ctx.gw, l1Hand_, 1, false);
                bWake_ = true; logAct("B wake", l1Hand_, rok);
            }
            if (bWake_ && (!bMoved_ ||
                (ctx.elapsedMs - lastMoveMs_ >= 3000 && subjectInBed(l1Hand_)))) {
                lastMoveMs_ = ctx.elapsedMs;
                if (issueMove(l1Hand_) && !bMoved_) { logAct("B move", l1Hand_, true); bMoved_ = true; }
            }
        }

        // Owner-side KO hold (timer-only re-top) through each subject's lay window.
        if (ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            if (ctx.isHost && haveM2_ && aDown_ && !aWake_) holdSubject(m2Hand_);
            if (!ctx.isHost && haveL1_ && bDown_ && !bWake_) holdSubject(l1Hand_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aDown_ && aPut_ && aPutOk_ && aWake_ && aMoved_)
                                 : (bDown_ && bPut_ && bPutOk_ && bWake_ && bMoved_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long A_DOWN_AT_MS     = 6000;
    static const unsigned long A_PUT_AT_MS      = 12000;
    static const unsigned long A_WAKE_AT_MS     = 30000;
    static const unsigned long B_DOWN_AT_MS     = 46000;
    static const unsigned long B_PUT_AT_MS      = 52000;
    static const unsigned long B_WAKE_AT_MS     = 70000;
    static const unsigned long HOST_DURATION_MS = 88000;
    static const unsigned long JOIN_DURATION_MS = 84000;
    static const unsigned long REPUT_MS         = 1500;
    static const unsigned int  MAX_SQUAD        = 32;

    // Is this subject currently occupying a bed (drives the re-put/re-move throttle)?
    bool subjectInBed(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false;
        engine::FurnitureRead fr;
        return engine::readFurniture(c, &fr) && fr.valid && fr.kind == 1;
    }

    // Order the subject 25u clear of its current position (post-wake exit).
    bool issueMove(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false;
        float x = 0, y = 0, z = 0;
        if (!engine::readPos(c, &x, &y, &z)) return false;
        return engine::orderMoveTo(c, x + 25.0f, y, z);
    }

    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO BEDLAY %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logPut(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO BEDLAY PUT %s hand=%u,%u kind=1 ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveM2_) {
            // Host tab's SECOND member (lowest non-leader hand of rank 0).
            int lidx = tabLeaderIdx(sq, n, 0);
            if (lidx >= 0) {
                unsigned int lh[5]; handFromEntity(sq[lidx], lh);
                int best = -1;
                for (unsigned int i = 0; i < n; ++i) {
                    if (tabRankOf(sq, n, i) != 0) continue;
                    unsigned int h[5]; handFromEntity(sq[i], h);
                    if (h[3] == lh[3] && h[4] == lh[4]) continue;
                    if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
                }
                if (best >= 0) {
                    handFromEntity(sq[best], m2Hand_);
                    haveM2_ = true;
                    logSubject("M2", m2Hand_);
                }
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_);
                haveL1_ = true;
                logSubject("L1", l1Hand_);
            }
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO BEDLAY %s hand=%u,%u", who, h[3], h[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveM2_, haveL1_;
    unsigned long lastPutMs_, lastHoldMs_, lastMoveMs_;
    bool          aDown_, aPut_, aPutOk_, aWake_, aMoved_;
    bool          bDown_, bPut_, bPutOk_, bWake_, bMoved_;
    unsigned int  m2Hand_[5];
    unsigned int  l1Hand_[5];
};

// bed_put / cage_put / chain_put / pole_put (protocol 19 phase 3, unconscious
// placement): save 'bedcage1' (bed/cage) or 'pole1' (pole). chain_put (protocol
// 41 chained/pole STATE) needs no baked fixture - it self-chains the subject
// (setChainedMode) to exercise the isChained -> BODY_CHAINED crossing. pole_put
// (kind=4) places the subject on a baked PRISONER POLE via the engine's prison
// path (setPrisonMode -> occupant reads in=2), the SAME containment as a cage
// but on a pole model, so it's the controlled visual of a body ON A POLE. Two
// sequential owner-side windows against the SAME subject slot (one at a time):
//   Window A (host own-tab):  KO M2 (host tab's second member), place it in
//     the fixture via the putSubjectInFurniture scaffold, hold it there, then
//     take it back out. The JOIN's driven M2 copy must mirror enter + exit.
//   Window B (join own-tab):  same shape with the join's leader L1 - the
//     reverse ownership direction over the identical machinery.
// The KO is held down by its OWNER's holdDown re-top every 2 s (timer-only,
// the carry_order lesson) so the scaffold KO can't expire mid-occupancy.
// Both sides log the standard MEMBER/RECV series plus a per-subject
// "SCENARIO FURN hand=i,s t=ms in=<0|1|2> furn=i,s pos=x,y,z bs=n" line at
// 2 Hz reading the LOCAL character (on the peer that is the driven copy -
// exactly what must have entered). Test-FurnPut gates on both windows.
class FurnPutScenario : public TimedScenario {
public:
    explicit FurnPutScenario(int kind)
        : TimedScenario(kind == 4 ? "pole_put"
                      : (kind == 3 ? "chain_put"
                      : (kind == 2 ? "cage_put" : "bed_put")), 0),
          kind_(kind), recvCount_(0), lastLogMs_(0),
          haveM2_(false), haveL1_(false), lastHoldMs_(0),
          aDown_(false), aPut_(false), aOut_(false),
          bDown_(false), bPut_(false), bOut_(false),
          aPutOk_(false), bPutOk_(false), lastPutMs_(0) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveM2_ || !haveL1_) latchSubjects(ctx);

        // ---- Window A (host, own-tab): KO M2, put it in, take it out -------
        if (ctx.isHost && haveM2_ && !aDown_ && ctx.elapsedMs >= A_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
            logAct("A down", m2Hand_, ok); aDown_ = true;
        }
        if (ctx.isHost && haveM2_ && ctx.elapsedMs >= A_PUT_AT_MS &&
            ctx.elapsedMs < A_OUT_AT_MS &&
            (!aPut_ || (!aPutOk_ && ctx.elapsedMs - lastPutMs_ >= 3000))) {
            // Throttled re-issue until the engine accepts (the bed_pose lesson:
            // never give up on the first frame's transient failure).
            lastPutMs_ = ctx.elapsedMs;
            aPutOk_ = engine::putSubjectInFurniture(ctx.gw, m2Hand_, kind_, true);
            if (!aPut_) { logAct("A put", m2Hand_, aPutOk_); aPut_ = true; }
        }
        if (ctx.isHost && haveM2_ && !aOut_ && ctx.elapsedMs >= A_OUT_AT_MS) {
            bool ok = engine::putSubjectInFurniture(ctx.gw, m2Hand_, kind_, false);
            logAct("A out", m2Hand_, ok); aOut_ = true;
        }

        // ---- Window B (join, own-tab): the same over L1 ---------------------
        if (!ctx.isHost && haveL1_ && !bDown_ && ctx.elapsedMs >= B_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, l1Hand_);
            logAct("B down", l1Hand_, ok); bDown_ = true;
        }
        if (!ctx.isHost && haveL1_ && ctx.elapsedMs >= B_PUT_AT_MS &&
            ctx.elapsedMs < B_OUT_AT_MS &&
            (!bPut_ || (!bPutOk_ && ctx.elapsedMs - lastPutMs_ >= 3000))) {
            lastPutMs_ = ctx.elapsedMs;
            bPutOk_ = engine::putSubjectInFurniture(ctx.gw, l1Hand_, kind_, true);
            if (!bPut_) { logAct("B put", l1Hand_, bPutOk_); bPut_ = true; }
        }
        if (!ctx.isHost && haveL1_ && !bOut_ && ctx.elapsedMs >= B_OUT_AT_MS) {
            bool ok = engine::putSubjectInFurniture(ctx.gw, l1Hand_, kind_, false);
            logAct("B out", l1Hand_, ok); bOut_ = true;
        }

        // Owner-side KO hold (timer-only re-top) through each subject's window.
        if (ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            if (ctx.isHost && haveM2_ && aDown_ && ctx.elapsedMs < A_HOLD_UNTIL_MS)
                holdSubject(m2Hand_);
            if (!ctx.isHost && haveL1_ && bDown_ && ctx.elapsedMs < B_HOLD_UNTIL_MS)
                holdSubject(l1Hand_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveM2_) logFurnLine(m2Hand_, ctx.elapsedMs);
            if (haveL1_) logFurnLine(l1Hand_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aDown_ && aPut_ && aPutOk_ && aOut_)
                                 : (bDown_ && bPut_ && bPutOk_ && bOut_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared timeline (peer-armed clock on both sides): KO settles 6 s before
    // the put; ~16 s in the furniture; 6 s of settle between the windows.
    static const unsigned long A_DOWN_AT_MS    = 8000;
    static const unsigned long A_PUT_AT_MS     = 14000;
    static const unsigned long A_OUT_AT_MS     = 30000;
    static const unsigned long A_HOLD_UNTIL_MS = 34000;
    static const unsigned long B_DOWN_AT_MS    = 36000;
    static const unsigned long B_PUT_AT_MS     = 42000;
    static const unsigned long B_OUT_AT_MS     = 58000;
    static const unsigned long B_HOLD_UNTIL_MS = 62000;
    static const unsigned long HOST_DURATION_MS = 70000;
    static const unsigned long JOIN_DURATION_MS = 66000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FURNACT %s hand=%u,%u kind=%d ok=%d",
                  what, h[3], h[4], kind_, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    // One "SCENARIO FURN" line: this body's LOCAL occupancy + position.
    void logFurnLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        engine::FurnitureRead fr;
        if (!engine::readFurniture(c, &fr) || !fr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO FURN hand=%u,%u t=%lu in=%d furn=%u,%u pos=%.2f,%.2f,%.2f bs=%u",
                  h[3], h[4], t, fr.kind, fr.furn[3], fr.furn[4], x, y, z,
                  (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveM2_) {
            // Host tab's SECOND member (the lowest non-leader hand of rank 0).
            int lidx = tabLeaderIdx(sq, n, 0);
            if (lidx >= 0) {
                unsigned int lh[5]; handFromEntity(sq[lidx], lh);
                int best = -1;
                for (unsigned int i = 0; i < n; ++i) {
                    if (tabRankOf(sq, n, i) != 0) continue;
                    unsigned int h[5]; handFromEntity(sq[i], h);
                    if (h[3] == lh[3] && h[4] == lh[4]) continue;
                    if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
                }
                if (best >= 0) {
                    handFromEntity(sq[best], m2Hand_);
                    haveM2_ = true;
                    logSubject("M2", m2Hand_);
                }
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_);
                haveL1_ = true;
                logSubject("L1", l1Hand_);
            }
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FURN %s hand=%u,%u kind=%d",
                  who, h[3], h[4], kind_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    int           kind_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveM2_, haveL1_;
    unsigned long lastHoldMs_;
    bool          aDown_, aPut_, aOut_;
    bool          bDown_, bPut_, bOut_;
    bool          aPutOk_, bPutOk_;
    unsigned long lastPutMs_;
    unsigned int  m2Hand_[5];
    unsigned int  l1Hand_[5];
};

// cage_peer_sync (protocol 36, third-party placement): the guard-jails-the-
// join-PC reproduction. In the 2026-07-09 session a HOST-sim guard placed the
// join's KO'd PC into a cage; the occupant's owner never saw the action, so
// the occupant-owner EVT_ENTER_FURNITURE could not fire and the host's 3 s
// furniture self-heal ejected the driven copy over and over ("the host kept
// taking it out of the cage"). One window over the join's leader L1 (save
// 'bedcage1', the baked Prisoner Cage):
//   t=8s   JOIN downs its OWN L1 (owner-side KO, streams to the host) and
//          re-tops the KO every 2 s through the window (the carry lesson).
//   t=14s  HOST places its DRIVEN L1 copy into the cage (the guard action,
//          reproduced programmatically; throttled re-issue until accepted).
//          The host must author "[furn] SEND PEER-ENTER", hold its self-heal
//          exit, and the JOIN must apply the enter to its own KO'd body -
//          after which its stream carries BODY_IN_CAGE and both sides
//          converge (no eject through the 26 s hold window).
//   t=44s  JOIN exits its OWN body (owner-authored exit, the symmetric path).
// Both sides log the 2 Hz "SCENARIO FURN hand=..." occupancy series for L1;
// Test-CagePeer gates author/apply/occupancy/no-eject/exit-clean.
class CagePeerScenario : public TimedScenario {
public:
    CagePeerScenario()
        : TimedScenario("cage_peer_sync", 0), recvCount_(0), lastLogMs_(0), haveL1_(false),
          lastHoldMs_(0), downDone_(false), putDone_(false), outDone_(false),
          putOk_(false), lastPutMs_(0) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL1_) latchL1(ctx);

        if (!ctx.isHost && haveL1_) {
            // Owner-side KO + hold (the join owns L1).
            if (!downDone_ && ctx.elapsedMs >= DOWN_AT_MS) {
                bool ok = engine::orderDownSubject(ctx.gw, l1Hand_);
                logAct("join down", ok);
                downDone_ = true;
            }
            if (downDone_ && ctx.elapsedMs < HOLD_UNTIL_MS &&
                ctx.elapsedMs - lastHoldMs_ >= 2000) {
                lastHoldMs_ = ctx.elapsedMs;
                Character* c = engine::resolveCharByHand(
                    l1Hand_[3], l1Hand_[4], l1Hand_[0], l1Hand_[1], l1Hand_[2]);
                if (c) engine::holdDown(c);
            }
            // Owner-authored exit: the join frees its own body.
            if (!outDone_ && ctx.elapsedMs >= OUT_AT_MS) {
                bool ok = engine::putSubjectInFurniture(ctx.gw, l1Hand_, KIND, false);
                logAct("join out", ok);
                outDone_ = true;
            }
        }
        if (ctx.isHost && haveL1_ && ctx.elapsedMs >= PUT_AT_MS &&
            ctx.elapsedMs < OUT_AT_MS &&
            (!putDone_ || (!putOk_ && ctx.elapsedMs - lastPutMs_ >= 3000))) {
            // The guard action: the HOST places the peer-owned driven copy.
            lastPutMs_ = ctx.elapsedMs;
            putOk_ = engine::putSubjectInFurniture(ctx.gw, l1Hand_, KIND, true);
            if (!putDone_) { logAct("host put", putOk_); putDone_ = true; }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL1_) logFurnLine(ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (putDone_ && putOk_)
                                 : (downDone_ && outDone_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const int           KIND = 2; // cage (setPrisonMode)
    static const unsigned long DOWN_AT_MS      = 8000;
    static const unsigned long PUT_AT_MS       = 14000;
    static const unsigned long HOLD_UNTIL_MS   = 42000;
    static const unsigned long OUT_AT_MS       = 44000;
    static const unsigned long JOIN_DURATION_MS = 56000;
    static const unsigned long HOST_DURATION_MS = 62000;
    static const unsigned int  MAX_SQUAD        = 32;

    void latchL1(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 1);
        if (idx < 0) return;
        handFromEntity(sq[idx], l1Hand_);
        haveL1_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FURN L1 hand=%u,%u kind=%d",
                  l1Hand_[3], l1Hand_[4], KIND);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logAct(const char* what, bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FURNACT %s hand=%u,%u kind=%d ok=%d",
                  what, l1Hand_[3], l1Hand_[4], KIND, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // One "SCENARIO FURN" line: L1's LOCAL occupancy + position on this client.
    void logFurnLine(unsigned long t) {
        Character* c = engine::resolveCharByHand(
            l1Hand_[3], l1Hand_[4], l1Hand_[0], l1Hand_[1], l1Hand_[2]);
        if (!c) return;
        engine::FurnitureRead fr;
        if (!engine::readFurniture(c, &fr) || !fr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO FURN hand=%u,%u t=%lu in=%d furn=%u,%u pos=%.2f,%.2f,%.2f bs=%u",
                  l1Hand_[3], l1Hand_[4], t, fr.kind, fr.furn[3], fr.furn[4], x, y, z,
                  (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL1_;
    unsigned long lastHoldMs_;
    bool          downDone_, putDone_, outDone_;
    bool          putOk_;
    unsigned long lastPutMs_;
    unsigned int  l1Hand_[5];
};

// sneak_probe (protocol 20 phase 0, host-side spike): does the engine's stealth
// detection fire against a DRIVEN copy, and is whoSeesMeSneaking safely
// readable? The HOST directly sets stealthMode on its driven copy of the
// join's leader (L1) near the bar NPCs (save 'sync'), then logs the copy's
// readStealth series at 2 Hz: mode / unseen / map size / top seer entries.
// The JOIN logs the same series off its OWN L1 (baseline: mode stays off
// locally - nothing streams stealth yet in this spike). Log-only; the oracle
// just gates "host saw detection entries while the mode was on".
class SneakProbeScenario : public TimedScenario {
public:
    SneakProbeScenario()
        : TimedScenario("sneak_probe", 0), recvCount_(0), lastLogMs_(0), haveL1_(false),
          onDone_(false), offDone_(false), onOk_(false), lastActMs_(0),
          sawSeer_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL1_) latchL1(ctx);

        // HOST: force stealth mode on the DRIVEN L1 copy (throttled re-issue
        // until the engine call lands), then release it near the end.
        if (ctx.isHost && haveL1_ && ctx.elapsedMs >= ON_AT_MS &&
            ctx.elapsedMs < OFF_AT_MS &&
            (!onDone_ || (!onOk_ && ctx.elapsedMs - lastActMs_ >= 3000))) {
            lastActMs_ = ctx.elapsedMs;
            onOk_ = engine::sneakSubject(ctx.gw, l1Hand_, true);
            if (!onDone_) { logAct("on", onOk_); onDone_ = true; }
        }
        if (ctx.isHost && haveL1_ && !offDone_ && ctx.elapsedMs >= OFF_AT_MS) {
            bool ok = engine::sneakSubject(ctx.gw, l1Hand_, false);
            logAct("off", ok); offDone_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL1_) logSneakLine(ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (onDone_ && onOk_ && sawSeer_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long ON_AT_MS         = 8000;
    static const unsigned long OFF_AT_MS        = 40000;
    static const unsigned long HOST_DURATION_MS = 50000;
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, bool ok) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKACT %s hand=%u,%u ok=%d",
                  what, l1Hand_[3], l1Hand_[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // One "SCENARIO SNEAKPROBE" line: L1's LOCAL stealth state + top seers.
    void logSneakLine(unsigned long t) {
        Character* c = engine::resolveCharByHand(l1Hand_[3], l1Hand_[4],
                                                 l1Hand_[0], l1Hand_[1], l1Hand_[2]);
        if (!c) return;
        engine::StealthRead sr;
        if (!engine::readStealth(c, &sr) || !sr.valid) {
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKPROBE t=%lu readfail", t);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }
        if (sr.nSeers > 0) sawSeer_ = true;
        char b[288]; int off = _snprintf(b, sizeof(b) - 1,
            "SCENARIO SNEAKPROBE hand=%u,%u t=%lu mode=%d unseen=%u map=%u",
            l1Hand_[3], l1Hand_[4], t, sr.sneaking ? 1 : 0,
            (unsigned)sr.unseen, sr.mapSize);
        for (unsigned int i = 0; i < sr.nSeers && i < 4 && off > 0 &&
                                 off < (int)sizeof(b) - 48; ++i) {
            off += _snprintf(b + off, sizeof(b) - 1 - off,
                             " seer=%u,%u:%u:%.2f",
                             sr.seers[i].npc[3], sr.seers[i].npc[4],
                             (unsigned)sr.seers[i].see, sr.seers[i].prog);
        }
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchL1(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 1); // join tab's leader on BOTH sides
        if (idx >= 0) {
            handFromEntity(sq[idx], l1Hand_);
            haveL1_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L1 hand=%u,%u",
                      l1Hand_[3], l1Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL1_;
    bool          onDone_, offDone_, onOk_;
    unsigned long lastActMs_;
    bool          sawSeer_;
    unsigned int  l1Hand_[5];
};

// sneak_pose (protocol 20 phase 4): stealth POSTURE crossing, both ownership
// directions. Window A: the HOST toggles stealth on its own leader (L0) ON at
// T+10 s, OFF at T+35 s - the join's driven copy must follow via the streamed
// BODY_SNEAK bit. Window B: the JOIN does the same with its leader (L1) at
// T+45/T+70 - the host's copy must follow. Both sides log a 2 Hz
// "SCENARIO SNEAK hand=i,s t=ms mode=0|1 bs=n" series for BOTH subjects; the
// oracle asserts the PEER's copy flips mode within budget on all four edges.
class SneakPoseScenario : public TimedScenario {
public:
    SneakPoseScenario()
        : TimedScenario("sneak_pose", 0), recvCount_(0), lastLogMs_(0),
          haveL0_(false), haveL1_(false),
          aOnDone_(false), aOffDone_(false), bOnDone_(false), bOffDone_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_ || !haveL1_) latchLeaders(ctx);

        // Window A: HOST toggles ITS OWN leader. Window B: JOIN toggles its own.
        if (ctx.isHost && haveL0_) {
            if (!aOnDone_ && ctx.elapsedMs >= A_ON_MS) {
                aOnDone_ = true; logAct("A-on", engine::sneakSubject(ctx.gw, l0Hand_, true), l0Hand_);
            }
            if (!aOffDone_ && ctx.elapsedMs >= A_OFF_MS) {
                aOffDone_ = true; logAct("A-off", engine::sneakSubject(ctx.gw, l0Hand_, false), l0Hand_);
            }
        }
        if (!ctx.isHost && haveL1_) {
            if (!bOnDone_ && ctx.elapsedMs >= B_ON_MS) {
                bOnDone_ = true; logAct("B-on", engine::sneakSubject(ctx.gw, l1Hand_, true), l1Hand_);
            }
            if (!bOffDone_ && ctx.elapsedMs >= B_OFF_MS) {
                bOffDone_ = true; logAct("B-off", engine::sneakSubject(ctx.gw, l1Hand_, false), l1Hand_);
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL0_) logSneakLine(ctx.elapsedMs, l0Hand_);
            if (haveL1_) logSneakLine(ctx.elapsedMs, l1Hand_);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aOnDone_ && aOffDone_) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long A_ON_MS  = 10000;
    static const unsigned long A_OFF_MS = 35000;
    static const unsigned long B_ON_MS  = 45000;
    static const unsigned long B_OFF_MS = 70000;
    static const unsigned long HOST_DURATION_MS = 85000;
    static const unsigned long JOIN_DURATION_MS = 80000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, bool ok, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKACT %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logSneakLine(unsigned long t, const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        int mode = engine::readStealthMode(c);
        unsigned short bs = engine::readBodyState(c);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK hand=%u,%u t=%lu mode=%d bs=%u",
                  h[3], h[4], t, mode, (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchLeaders(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveL0_) {
            int idx = tabLeaderIdx(sq, n, 0);
            if (idx >= 0) {
                handFromEntity(sq[idx], l0Hand_); haveL0_ = true;
                char b[96]; _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L0 hand=%u,%u",
                                      l0Hand_[3], l0Hand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_); haveL1_ = true;
                char b[96]; _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L1 hand=%u,%u",
                                      l1Hand_[3], l1Hand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_, haveL1_;
    bool          aOnDone_, aOffDone_, bOnDone_, bOffDone_;
    unsigned int  l0Hand_[5];
    unsigned int  l1Hand_[5];
};

// sneak_detect (protocol 20 phase 4): DETECTION-INDICATOR crossing. The JOIN
// toggles stealth on its own leader (L1) near the bar NPCs (save 'sync'). The
// host's world is the detection authority: its NPCs fill whoSeesMeSneaking on
// its DRIVEN copy of L1 (spike-proven), publishStealth streams it back, and
// the join replays it onto its OWN L1 - both sides log a 2 Hz "SCENARIO
// DETECT hand=i,s t=ms mode=d map=n see=..." series off their local L1. The
// oracle asserts the join accumulated entries while sneaking (via the
// feedback channel - "[sneak] DETECT RECV ... applied>=1" proves the path)
// and that they cleared after the sneak ended.
class SneakDetectScenario : public TimedScenario {
public:
    SneakDetectScenario()
        : TimedScenario("sneak_detect", 0), recvCount_(0), lastLogMs_(0), haveL1_(false),
          onDone_(false), offDone_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL1_) latchL1(ctx);

        // The JOIN owns the sneaker: it performs the toggles.
        if (!ctx.isHost && haveL1_) {
            if (!onDone_ && ctx.elapsedMs >= ON_AT_MS) {
                onDone_ = true;
                bool ok = engine::sneakSubject(ctx.gw, l1Hand_, true);
                logAct("on", ok);
            }
            if (!offDone_ && ctx.elapsedMs >= OFF_AT_MS) {
                offDone_ = true;
                bool ok = engine::sneakSubject(ctx.gw, l1Hand_, false);
                logAct("off", ok);
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL1_) logDetectLine(ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? true : (recvCount_ >= 1 && onDone_ && offDone_);
            return true;
        }
        return false;
    }

private:
    static const unsigned long ON_AT_MS         = 10000;
    static const unsigned long OFF_AT_MS        = 45000;
    static const unsigned long HOST_DURATION_MS = 62000;
    static const unsigned long JOIN_DURATION_MS = 58000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, bool ok) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKACT %s hand=%u,%u ok=%d",
                  what, l1Hand_[3], l1Hand_[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logDetectLine(unsigned long t) {
        Character* c = engine::resolveCharByHand(l1Hand_[3], l1Hand_[4],
                                                 l1Hand_[0], l1Hand_[1], l1Hand_[2]);
        if (!c) return;
        engine::StealthRead sr;
        if (!engine::readStealth(c, &sr) || !sr.valid) return;
        char b[288]; int off = _snprintf(b, sizeof(b) - 1,
            "SCENARIO DETECT hand=%u,%u t=%lu mode=%d unseen=%u map=%u",
            l1Hand_[3], l1Hand_[4], t, sr.sneaking ? 1 : 0,
            (unsigned)sr.unseen, sr.mapSize);
        for (unsigned int i = 0; i < sr.nSeers && i < 4 && off > 0 &&
                                 off < (int)sizeof(b) - 48; ++i) {
            off += _snprintf(b + off, sizeof(b) - 1 - off,
                             " seer=%u,%u:%u:%.2f",
                             sr.seers[i].npc[3], sr.seers[i].npc[4],
                             (unsigned)sr.seers[i].see, sr.seers[i].prog);
        }
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchL1(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 1);
        if (idx >= 0) {
            handFromEntity(sq[idx], l1Hand_);
            haveL1_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L1 hand=%u,%u",
                      l1Hand_[3], l1Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL1_;
    bool          onDone_, offDone_;
    unsigned int  l1Hand_[5];
};

// speed_sync (consensus game-speed validation): both clients run the default-on
// speed-sync module; the scenario SIMULATES user speed clicks by writing the
// engine's speed directly (writeGameSpeed != the replicator's lastApplied ->
// detected as a user request, exactly like a real click). Save 'sync' (the bar
// with armed NPCs for the combat phase). Timeline (peer-ready armed), each step
// exercising one consensus rule:
//   T+10 s HOST clicks 3x -> DENIED (join still requests 1x; min holds 1x -
//          the "both must raise" rule; the host engine snaps back).
//   T+22 s JOIN clicks 3x -> requests now 3/3 -> both settle at 3x.
//   T+34 s JOIN clicks 1x -> min rule: EITHER can lower -> both drop to 1x.
//   T+44 s JOIN clicks 3x -> both back at 3x.
//   T+52 s HOST orders a bar NPC onto its OWN leader -> own-squad combat ->
//          the consensus cap demotes the effective speed to 1x mid-3x.
// Both sides log "SCENARIO SPEED t=<ms> mult=<f> paused=<n>" at ~2 Hz; the
// Test-SpeedSync oracle time-aligns the two series (CLOCKSYNC-corrected) and
// gates the transition count, the denied lone raise, each transition's follow
// latency, the match fraction, and the combat window sitting at 1x.
class SpeedSyncScenario : public TimedScenario {
public:
    SpeedSyncScenario()
        : TimedScenario("speed_sync", 0), recvCount_(0), lastLogMs_(0), lastOrderMs_(0),
          haveOwn_(false), haveStriker_(false), hostClicked_(false),
          hostClicked1_(false), hostClicked3b_(false),
          joinClicked3a_(false), joinClicked1_(false), joinClicked3b_(false),
          combatIssued_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        if (!haveOwn_) latchLeader(ctx, ownRank);

        // Simulated user clicks. writeGameSpeed goes through the engine's own
        // (hooked) setters, so each call registers as captured USER INTENT -
        // the same path a real UI click takes. The middle legs exercise the
        // previously-impossible case: a click EQUAL to the current effective
        // (join lowers its stale 3x vote by clicking 1x while the effective is
        // already 1x), which the old state-diff detector could never see.
        if (ctx.isHost && !hostClicked_ && ctx.elapsedMs >= HOST_3X_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("host", 3.0f, ok, "");
            hostClicked_ = true;
        }
        if (!ctx.isHost && !joinClicked3a_ && ctx.elapsedMs >= JOIN_3XA_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("join", 3.0f, ok, "");
            joinClicked3a_ = true;
        }
        // Host lowers to 1x: effective drops to 1x but the JOIN's 3x vote is
        // now stale-high (the stuck-vote setup).
        if (ctx.isHost && !hostClicked1_ && ctx.elapsedMs >= HOST_1X_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 1.0f, false);
            logClick("host", 1.0f, ok, " tag=lower");
            hostClicked1_ = true;
        }
        // The SAME-VALUE click: join clicks 1x while the effective is already
        // 1x. Engine state doesn't change - only the intent hooks can see it.
        if (!ctx.isHost && !joinClicked1_ && ctx.elapsedMs >= JOIN_1X_SAME_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 1.0f, false);
            logClick("join", 1.0f, ok, " tag=samevalue");
            joinClicked1_ = true;
        }
        // Host re-raises to 3x: must be DENIED (join's vote is now 1x) - the
        // assertion that the same-value click actually landed as a vote.
        if (ctx.isHost && !hostClicked3b_ && ctx.elapsedMs >= HOST_3XB_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("host", 3.0f, ok, " tag=reraise");
            hostClicked3b_ = true;
        }
        if (!ctx.isHost && !joinClicked3b_ && ctx.elapsedMs >= JOIN_3XB_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("join", 3.0f, ok, " tag=raise2");
            joinClicked3b_ = true;
        }

        // Combat phase: a bar NPC onto the host's OWN leader, so the host's
        // own-squad combat flag trips and the consensus cap demotes to 1x.
        // Re-ordered every 2.5 s (safe no-op while fighting); a KO'd striker
        // is replaced (the player_combat restrike lesson).
        if (ctx.isHost && haveOwn_ && ctx.elapsedMs >= COMBAT_AT_MS &&
            (ctx.elapsedMs - lastOrderMs_ >= 2500 || lastOrderMs_ == 0)) {
            lastOrderMs_ = ctx.elapsedMs;
            bool pick = !haveStriker_;
            if (haveStriker_) {
                engine::MedicalRead mr;
                if (!engine::readMedicalByHand(striker_, &mr) || !mr.valid ||
                    mr.unconscious || mr.dead)
                    pick = true;
            }
            if (pick)
                haveStriker_ = engine::pickCombatVictim(ctx.gw, ownHand_,
                                                        haveStriker_ ? striker_ : 0,
                                                        striker_);
            if (haveStriker_) {
                bool ok = engine::orderAttackByHand(ctx.gw, striker_, ownHand_);
                if (!combatIssued_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO SPEEDSYNC combat issued atk=%u,%u vic=%u,%u ok=%d",
                              striker_[3], striker_[4], ownHand_[3], ownHand_[4],
                              ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    combatIssued_ = true;
                }
            } else if (!combatIssued_) {
                coop::logLine("SCENARIO SPEEDSYNC combat pick FAILED (no upright NPC)");
                combatIssued_ = true;
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // The SPEED series the oracle compares across the two clients.
            // buttons= is the MyGUI speed-button highlight (the VOTE indicator);
            // Phase 5 gates that it tracks the vote and returns to it after the
            // combat cap clears (mult can be capped to 1x while buttons show 3x).
            float mult = 0.0f; bool paused = false;
            if (engine::readGameSpeed(ctx.gw, &mult, &paused)) {
                char btn[16]; btn[0] = '\0';
                int nBtn = engine::readSpeedButtons(btn, sizeof(btn));
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPEED t=%lu mult=%.2f paused=%d nbtn=%d buttons=%s",
                          ctx.elapsedMs, mult, paused ? 1 : 0, nBtn, btn);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // Squad MEMBER/RECV series (harness anchors + advisory transform
            // oracles; also proves the 3x window doesn't break the stream).
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (hostClicked_ && hostClicked1_ &&
                                    hostClicked3b_ && combatIssued_)
                                 : (joinClicked3a_ && joinClicked1_ &&
                                    joinClicked3b_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared (peer-ready-armed) timeline; the combat window runs 62 s -> end.
    static const unsigned long HOST_3X_AT_MS      = 10000;
    static const unsigned long JOIN_3XA_AT_MS     = 22000;
    static const unsigned long HOST_1X_AT_MS      = 30000;
    static const unsigned long JOIN_1X_SAME_AT_MS = 38000;
    static const unsigned long HOST_3XB_AT_MS     = 46000;
    static const unsigned long JOIN_3XB_AT_MS     = 54000;
    static const unsigned long COMBAT_AT_MS       = 62000;
    static const unsigned long HOST_DURATION_MS   = 85000;
    static const unsigned long JOIN_DURATION_MS   = 78000;
    static const unsigned int  MAX_SQUAD          = 32;

    static void logClick(const char* who, float mult, bool ok, const char* tag) {
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPEEDSYNC %s click mult=%.1f ok=%d%s",
                  who, mult, ok ? 1 : 0, tag);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchLeader(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) return;
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPEEDSYNC own rank=%u hand=%u,%u",
                  ownRank, ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          haveOwn_;
    bool          haveStriker_;
    bool          hostClicked_;
    bool          hostClicked1_;
    bool          hostClicked3b_;
    bool          joinClicked3a_;
    bool          joinClicked1_;
    bool          joinClicked3b_;
    bool          combatIssued_;
    unsigned int  ownHand_[5];
    unsigned int  striker_[5];
};

// pause_stress (DEBUG-ONLY repro driver for .planning/debug/crash-av-worker-thread.md,
// lead E-21 - NOT a gate and deliberately NOT in scripts/scenarios.psd1).
//
// E-21 established that the one stimulus provably common to BOTH faulting
// processes of the 0xc0000005 READ 0xFFFFFFFFFFFFFFFF double crash was a
// REPLICATED PAUSE (`[speed] REQ RECV owner=1 mult=0.00 paused=1` -> `[speed]
// SET mult=0.00 paused=1`), applied 3.7 s before the client's fault and 7.9 s
// before the host's. speed_sync already simulates user speed clicks, but only
// ever 3x/1x - it never drives mult=0.00/paused=1, so it cannot exercise this.
//
// This scenario cycles the session through the pause repeatedly and holds it
// there for the width of the observed fault window, so a run gets many chances
// at a window a normal session enters once. Two design points matter:
//   * The INITIATOR ALTERNATES between host and join. E-20 proved the fault
//     site is role-INDEPENDENT (both roles faulted at the identical RVA), so a
//     driver that only ever pauses from one side would only test half the
//     replication direction. Even cycles: host votes. Odd cycles: join votes.
//     The non-initiator only observes, so the pause it applies arrives over the
//     wire exactly as it did in the field.
//   * The HOLD spans the fault window. The field faults landed 3.7 s and 7.9 s
//     after the pause, so the hold is 14 s - comfortably past both - and the
//     resume gap is short, to fit as many cycles as possible into a run.
//
// ctx.elapsedMs is GetTickCount()-based wall clock (Plugin.cpp:2570), NOT game
// time, so the schedule keeps advancing while the world is paused and the
// resume always fires. This is load-bearing: on a game-time clock the first
// pause would wedge the scenario forever.
//
// Both sides log `SCENARIO PAUSESTRESS OBS t=<ms> mult=<f> paused=<n>` at 1 Hz
// (the observed EFFECTIVE speed, i.e. post-consensus) plus one
// `SCENARIO PAUSESTRESS cycle=<n> who=<host|join> act=<pause|resume> ok=<n>`
// per vote. Read those against the plugin's own `[speed] REQ`/`REQ RECV`/`SET`
// lines to confirm each pause really replicated before judging a run.
//
// The cycle schedule itself lives in test/PauseSchedule.h as pure functions.
// It is the only half of this driver that can be verified WITHOUT a live rig,
// and src/prototest::testPauseSchedule() asserts it there - including the
// role-alternation invariant and the hold-spans-the-fault-window property. The
// scenario CALLS those functions rather than keeping a private copy, so those
// checks guard what actually ships.
class PauseStressScenario : public TimedScenario {
public:
    PauseStressScenario()
        : TimedScenario("pause_stress", 0), lastLogMs_(0), cyclesIssued_(0),
          pausesObserved_(0), edge_(pauseEdgeInit()), sawPausedNow_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const PausePhase ph = pausePhaseAt(pauseStressGeometry(), ctx.elapsedMs);
        const int  cycle      = ph.cycle;
        const bool wantPaused = ph.wantPaused;

        if (pauseShouldAct(ph, ctx.isHost, edge_)) {
            // One write per phase edge. writeGameSpeed goes through the
            // engine's hooked setters, so it registers as captured USER INTENT
            // - the same path a real UI click takes, which is what makes the
            // vote replicate.
            bool ok = wantPaused ? engine::writeGameSpeed(ctx.gw, 0.0f, true)
                                 : engine::writeGameSpeed(ctx.gw, 1.0f, false);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO PAUSESTRESS cycle=%d who=%s act=%s ok=%d",
                      cycle, ctx.isHost ? "host" : "join",
                      wantPaused ? "pause" : "resume", ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            if (wantPaused && ok) ++cyclesIssued_;
            pauseNoteActed(ph, edge_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 1000 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            float mult = 0.0f; bool paused = false;
            if (engine::readGameSpeed(ctx.gw, &mult, &paused)) {
                // Count the EDGE, not the samples, so the count is a count of
                // replicated pauses rather than of ticks spent paused.
                if (paused && !sawPausedNow_) ++pausesObserved_;
                sawPausedNow_ = paused;
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO PAUSESTRESS OBS t=%lu mult=%.2f paused=%d "
                          "cycle=%d want=%d issued=%u seen=%u",
                          ctx.elapsedMs, mult, paused ? 1 : 0, cycle,
                          wantPaused ? 1 : 0, cyclesIssued_, pausesObserved_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        unsigned long dur = ctx.isHost ? pauseStressHostDurationMs()
                                       : pauseStressJoinDurationMs();
        if (ctx.elapsedMs >= dur) {
            // "Passed" only means the driver did its job: it drove pauses and
            // this process actually observed the paused world. It says NOTHING
            // about the crash - a clean run is a NON-reproduction, which per
            // E-26 eliminates nothing.
            passed_ = (pausesObserved_ >= 2);
            return true;
        }
        return false;
    }

private:
    unsigned long  lastLogMs_;
    unsigned int   cyclesIssued_;
    unsigned int   pausesObserved_;
    PauseEdgeState edge_;
    bool           sawPausedNow_;
};

// speed_probe (vote-decoupling phase-0 spike, HOST-side, log-only): prove the
// three claims the decoupled design rests on, with speedSync forced OFF so the
// replicator can't fight the probe:
//   1. QUIET writes (setFrameSpeedMultiplier + guarded userPause) drive the
//      sim multiplier and it STICKS - nothing re-syncs it from the buttons.
//   2. QUIET writes leave the UI speed buttons untouched (the buttons=...
//      series stays constant across quiet acts), while a LOUD writeGameSpeed
//      (a simulated real click) moves them.
//   3. The intent hooks capture the loud click as a vote (INTENT line) and
//      stay silent for quiet writes.
// The join idles and logs its own series (harness anchor only).
class SpeedProbeScenario : public TimedScenario {
public:
    SpeedProbeScenario()
        : TimedScenario("speed_probe", 0), lastLogMs_(0), quiet3_(false), loud2_(false),
          quiet1_(false), quietPause_(false), quietResume_(false),
          actsOk_(true) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        // Drain captured intent every tick (both clients): quiet acts must NOT
        // produce INTENT lines; the loud click at 16 s must.
        float im = 0.0f; bool ip = false;
        while (engine::consumeSpeedIntent(ctx.gw, &im, &ip)) {
            char b[96];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SPEEDPROBE INTENT t=%lu mult=%.2f paused=%d",
                      ctx.elapsedMs, im, ip ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.isHost) {
            if (!quiet3_ && ctx.elapsedMs >= QUIET3_AT_MS) {
                quiet3_ = true;
                act("quiet3", engine::writeGameSpeedQuiet(ctx.gw, 3.0f, false), ctx);
            }
            if (!loud2_ && ctx.elapsedMs >= LOUD2_AT_MS) {
                loud2_ = true;
                act("loud2", engine::writeGameSpeed(ctx.gw, 2.0f, false), ctx);
            }
            if (!quiet1_ && ctx.elapsedMs >= QUIET1_AT_MS) {
                quiet1_ = true;
                act("quiet1", engine::writeGameSpeedQuiet(ctx.gw, 1.0f, false), ctx);
            }
            if (!quietPause_ && ctx.elapsedMs >= QPAUSE_AT_MS) {
                quietPause_ = true;
                act("quietpause", engine::writeGameSpeedQuiet(ctx.gw, 1.0f, true), ctx);
            }
            if (!quietResume_ && ctx.elapsedMs >= QRESUME_AT_MS) {
                quietResume_ = true;
                act("quietresume", engine::writeGameSpeedQuiet(ctx.gw, 1.0f, false), ctx);
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            float mult = 0.0f; bool paused = false;
            char btn[16]; btn[0] = '\0';
            int nBtn = engine::readSpeedButtons(btn, sizeof(btn));
            if (engine::readGameSpeed(ctx.gw, &mult, &paused)) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPEEDPROBE t=%lu mult=%.2f paused=%d nbtn=%d buttons=%s",
                          ctx.elapsedMs, mult, paused ? 1 : 0, nBtn,
                          (nBtn > 0) ? btn : "?");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost
                ? (quiet3_ && loud2_ && quiet1_ && quietPause_ &&
                   quietResume_ && actsOk_)
                : true; // join is a passive anchor; the oracle judges the host series
            return true;
        }
        return false;
    }

private:
    static const unsigned long QUIET3_AT_MS     = 8000;
    static const unsigned long LOUD2_AT_MS      = 16000;
    static const unsigned long QUIET1_AT_MS     = 24000;
    static const unsigned long QPAUSE_AT_MS     = 32000;
    static const unsigned long QRESUME_AT_MS    = 40000;
    static const unsigned long HOST_DURATION_MS = 48000;
    static const unsigned long JOIN_DURATION_MS = 44000;

    void act(const char* what, bool ok, const ScenarioContext& ctx) {
        if (!ok) actsOk_ = false;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPEEDPROBE act=%s t=%lu ok=%d",
                  what, ctx.elapsedMs, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned long lastLogMs_;
    bool          quiet3_;
    bool          loud2_;
    bool          quiet1_;
    bool          quietPause_;
    bool          quietResume_;
    bool          actsOk_;
};

// shackle_probe (Phase 6 6a evidence spike, log-only, BOTH clients): enumerate
// nearby world NPCs (a shackled prisoner is a slave, never in the player squad)
// and emit a "SCENARIO SHACKLE hand=i,s t=ms chained=.. shackleItem=.. lock=..
// owner=i,s" line at ~2 Hz for every body that is chained or carries a shackle
// item. The Test-ShackleProbe oracle time-aligns the owner's and peer's view of
// each shackled prisoner and flags any lock/chained divergence. Reads only - no
// behavior change ships in 6a.
// shackle_sync (Phase 6 6b validation, BOTH clients) reuses the SAME emission:
// with the protocol-42 locked bit + non-owner unlock guard shipping, Test-
// ShackleSync turns the probe's characterization metrics into a STRICT gate -
// a shared prisoner whose owner reports chained/locked while the peer's driven
// copy reports it cleared is now a FAIL (the guard is supposed to prevent it).
class ShackleProbeScenario : public TimedScenario {
public:
    ShackleProbeScenario(const char* nm)
        : TimedScenario(nm, 0), lastLogMs_(0), sawShackled_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* chars[MAXN];
            EntityState st[MAXN];
            unsigned int n = engine::listNpcs(ctx.gw, chars, st, MAXN);
            unsigned int shackled = 0;
            for (unsigned int i = 0; i < n; ++i) {
                engine::ShackleRead sr;
                if (!engine::readShackle(chars[i], &sr) || !sr.valid) continue;
                if (!sr.chained && !sr.hasShackleItem) continue;
                ++shackled;
                sawShackled_ = true;
                char b[192];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SHACKLE hand=%u,%u t=%lu chained=%d "
                          "shackleItem=%d lock=%d owner=%u,%u",
                          st[i].hIndex, st[i].hSerial, ctx.elapsedMs,
                          sr.chained ? 1 : 0, sr.hasShackleItem ? 1 : 0,
                          sr.lockPresent ? 1 : 0, sr.owner[3], sr.owner[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            char c[96];
            _snprintf(c, sizeof(c) - 1,
                      "SCENARIO SHACKLE COUNT t=%lu npcs=%u shackled=%u",
                      ctx.elapsedMs, n, shackled);
            c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Log-only spike: passing just means it ran to completion and
            // emitted its series. The oracle judges cross-client parity and the
            // shackled-sighting requirement.
            passed_ = true;
            (void)sawShackled_;
            return true;
        }
        return false;
    }

private:
    static const unsigned int  MAXN            = 64;
    static const unsigned long HOST_DURATION_MS = 40000;
    static const unsigned long JOIN_DURATION_MS = 36000;
    unsigned long lastLogMs_;
    bool          sawShackled_;
};

// ===========================================================================
// lockpick_escape (rebirth1, BOTH clients) - the anchored-to-free crossing.
//
// Every furniture scenario so far moves a body INTO a held state and asserts
// the peer follows. This one is the release, and the release is the harder
// direction: an anchored body is pinned by furniture the peer can see, while a
// freed one is loose in the world and has to start walking on both clients at
// once. rebirth1 is the fixture because the caged bodies are the PLAYER SQUAD
// (a Rebirth slave start), so the subject is owned - not a world prisoner
// either side may drive.
//
// The escape is the ENGINE'S, not ours. There is no "pick this lock" entry
// point, but the engine models prisoner escape natively, so the scenario only
// removes the reason the AI would not try: it raises the subject's LOCKPICKING
// to a level at which getLockpickChance is high, and then watches. Everything
// after the raise - the attempt, the roll, CRIME_LOCKPICKING, the transition to
// ESCAPING_SLAVE - is the engine's own code path, which is the whole point.
//
// The series carries getLockpickChance for a reason worth stating: a run where
// nothing happens is otherwise unreadable. chance > 0 with no escape means the
// AI never attempted; chance == 0 means the engine was never going to let it
// through and waiting longer is pointless; chance < 0 means the read lever
// itself did not resolve. Those need three different fixes.
//
// Log-only pass, like shackle_probe: reaching the end and emitting the series
// is success here, and Test-LockpickEscape judges the crossing from the two
// logs. The scenario must NOT decide the escape happened - the whole question
// is whether both clients agree that it did.
// escape_cohesion (same class, cohesion mode) extends the above from "did the
// release cross" to "do the two clients RENDER the same world while the freed
// body travels". Three additions, and each one exists to remove a different
// excuse for a disagreement:
//   * BOTH clients point their camera at the SAME body (the join's escapee), so
//     the two screenshots are directly comparable and neither side can be
//     accused of simply looking elsewhere;
//   * the waypoint is placed just PAST the nearest zone-cell boundary, because
//     a cell edge is where presence authority changes hands - the seam the
//     2026-08-08 dual-drive work lives on - so the walk crosses it deliberately
//     instead of hoping to;
//   * the per-sample series carries the subject's CELL, so a divergence can be
//     read against the handover rather than guessed at.
// Note the camera is not a passive viewport here: protocol 43 folds it into the
// interest anchors, so aiming both at one body widens what both clients stream
// around it. That is deliberate - symmetric interest is what makes "do both see
// the same thing" a fair question - but it means this measures cohesion under a
// shared spotlight, not under independent attention.
class EscapeScenario : public TimedScenario {
public:
    EscapeScenario(const char* nm, bool cohesion)
        : TimedScenario(nm, 0), cohesion_(cohesion), lastLogMs_(0),
          haveSubj_(false), raiseLogged_(false), sawHeld_(false),
          forceLogged_(false), haveHome_(false), haveTarget_(false),
          camLogged_(false), lastWalkMs_(0), lastCamMs_(0), freedAtMs_(0),
          homeX_(0), homeY_(0), homeZ_(0), tgtX_(0), tgtZ_(0) {
        subjHand_[0] = subjHand_[1] = subjHand_[2] = subjHand_[3] = subjHand_[4] = 0;
    }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        // The JOIN's tab leader escapes: it is the join that owns it, so the
        // raise is an owner-authoritative write, and the HOST is then the peer
        // whose driven copy has to follow the release. That is the direction
        // the dual-drive work made suspect, and the direction protocol 42's
        // SHACKLE RELOCK self-heal is most likely to fight.
        const unsigned int SUBJ_RANK = 1;
        if (!haveSubj_) latchSubject(ctx, SUBJ_RANK);

        // Both clients watch the SAME body, from the first tick, at the cadence
        // the split scenarios settled on. Re-issued rather than set once because
        // the engine drops the follow whenever the target is re-anchored - which
        // a cage exit does.
        if (cohesion_ && haveSubj_ &&
            (lastCamMs_ == 0 || ctx.elapsedMs - lastCamMs_ >= CAM_REFOCUS_MS)) {
            lastCamMs_ = ctx.elapsedMs;
            focusCamera(ctx);
        }

        if (haveSubj_ && !raiseLogged_ && ctx.elapsedMs >= RAISE_AT_MS) {
            raiseLogged_ = true;
            // Only the OWNER may write stats; the host logs its abstention so
            // the oracle can tell "the join never raised" from "the host did".
            bool ok = false;
            if (!ctx.isHost) {
                ok = engine::raiseSubjectStat(ctx.gw, subjHand_,
                                              STAT_LOCKPICKING_ID, RAISE_TO);
            }
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO ESCAPE raise side=%s hand=%u,%u stat=%d to=%.0f ok=%d",
                      ctx.isHost ? "host" : "join", subjHand_[3], subjHand_[4],
                      STAT_LOCKPICKING_ID, RAISE_TO, ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Fallback: if the engine has not let the subject out by FORCE_AT_MS,
        // the OWNER releases it outright. Measured on rebirth1 (run
        // 20260808_145843): with lockpicking raised 1 -> 100, getLockpickChance
        // stayed at exactly 0.000 for the full 180 s - Rebirth's cage locks are
        // not pickable at any skill, so waiting is not a slow path, it is a
        // dead one. The lockpick attempt above is still made and still
        // measured, because if a future fixture or patch makes it viable the
        // series will say so; but the crossing this scenario exists to test is
        // anchored -> free, and that must not depend on the engine agreeing to
        // open the door.
        if (haveSubj_ && !forceLogged_ && !freedAtMs_ && !ctx.isHost &&
            ctx.elapsedMs >= FORCE_AT_MS) {
            forceLogged_ = true;
            forceRelease(ctx);
        }

        if (haveSubj_ && (lastLogMs_ == 0 || ctx.elapsedMs - lastLogMs_ >= 500)) {
            lastLogMs_ = ctx.elapsedMs;
            logEscapeLine(ctx);
        }

        // A released prisoner that just stands there proves nothing about the
        // peer's ability to render a freed body, so the owner walks it away and
        // keeps re-issuing (the reissue cadence the movement scenarios use).
        if (haveSubj_ && !ctx.isHost && freedAtMs_ &&
            (lastWalkMs_ == 0 || ctx.elapsedMs - lastWalkMs_ >= WALK_REISSUE_MS)) {
            lastWalkMs_ = ctx.elapsedMs;
            walkAway(ctx);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Premise, not verdict: the run is only meaningful if we ever found
            // the caged subject. Whether it got out, and whether both clients
            // saw it get out, is the oracle's call.
            passed_ = haveSubj_;
            return true;
        }
        return false;
    }

private:
    // StatsEnumerated::STAT_LOCKPICKING. raiseSubjectStat takes the index as an
    // int (same as stats_sync's hardcoded STRENGTH/STEALTH pair).
    static const int           STAT_LOCKPICKING_ID = 37;
    static const unsigned long RAISE_AT_MS      = 10000;
    // 60 s of watching the raised subject before forcing the release: long
    // enough that an engine-driven escape would be visible in the series, short
    // enough to leave most of the window for the crossing itself.
    static const unsigned long FORCE_AT_MS      = 60000;
    static const unsigned long WALK_REISSUE_MS  = 4000;
    static const unsigned long CAM_REFOCUS_MS   = 5000;
    static const unsigned long HOST_DURATION_MS = 180000;
    static const unsigned long JOIN_DURATION_MS = 174000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         RAISE_TO;
    static const float         WALK_DIST;
    static const float         WALK_SPEED;
    // How far PAST the boundary to aim. Far enough that arriving is unambiguous
    // rather than a body hovering on the line and flapping the claim, close
    // enough that the leg stays inside one cell's worth of terrain.
    static const float         PAST_EDGE;

    Character* subject() {
        return engine::resolveCharByHand(subjHand_[3], subjHand_[4], subjHand_[0],
                                         subjHand_[1], subjHand_[2]);
    }

    // Point THIS client's camera at the subject.
    //
    // Two resolve paths are tried because run 20260808_183753 showed the host's
    // camera never moving while the join's tracked perfectly, from identical
    // code on an identically-resolved hand. The squad-array path is the one
    // SplitFarScenario uses and is therefore the one known to work on a peer's
    // body; resolveCharByHand is the fallback. The line reports which pointer
    // each side used and whether the engine accepted the call, so a repeat of
    // that asymmetry says WHERE it comes from instead of just that it happened.
    void focusCamera(const ScenarioContext& ctx) {
        Character* byHand = subject();
        Character* bySquad = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5]; handFromEntity(sq[i], h);
            if (h[3] == subjHand_[3] && h[4] == subjHand_[4]) {
                bySquad = engine::resolve(sq[i]);
                break;
            }
        }
        Character* use = bySquad ? bySquad : byHand;
        bool ok = use && engine::cameraFocusOn(ctx.gw, use);
        if (camLogged_) return;
        camLogged_ = true;
        float x = 0, y = 0, z = 0;
        if (use) engine::readPos(use, &x, &y, &z);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO ESCAPE cam side=%s t=%lu ok=%d src=%s byHand=%d "
                  "bySquad=%d squadN=%u pos=%.1f,%.1f,%.1f",
                  ctx.isHost ? "host" : "join", (unsigned long)ctx.elapsedMs,
                  ok ? 1 : 0, bySquad ? "squad" : "hand", byHand ? 1 : 0,
                  bySquad ? 1 : 0, n, x, y, z);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Owner-side release: drop BOTH holds, because rebirth1 applies both - the
    // subject reads furn=2 (prison cage) AND chained=1, and clearing only one
    // leaves the body anchored with no visible reason why.
    void forceRelease(const ScenarioContext& ctx) {
        Character* c = engine::resolveCharByHand(subjHand_[3], subjHand_[4],
                                                 subjHand_[0], subjHand_[1],
                                                 subjHand_[2]);
        if (!c) return;
        engine::FurnitureRead fr;
        engine::ShackleRead sr;
        bool haveFurn = engine::readFurniture(c, &fr) && fr.valid;
        bool haveSh   = engine::readShackle(c, &sr) && sr.valid;
        bool okFurn = false, okChain = false;
        if (haveFurn && (fr.kind == 1 || fr.kind == 2))
            okFurn = engine::applyFurniture(ctx.gw, c, fr.furn, fr.kind, false);
        if (haveSh && sr.chained)
            okChain = engine::applyFurniture(ctx.gw, c, sr.owner, 3, false);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO ESCAPE force hand=%u,%u t=%lu kind=%d furnOff=%d chainOff=%d",
                  subjHand_[3], subjHand_[4], ctx.elapsedMs,
                  haveFurn ? fr.kind : -1, okFurn ? 1 : 0, okChain ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Walk the freed subject away from where it was held, re-issued so a leg
    // interrupted by terrain or a guard resumes instead of dying quietly.
    //
    // lockpick_escape keeps this deliberately dumb - a fixed short offset, a
    // locomotion probe rather than a pathfinding test, because a caged start is
    // usually walled in. escape_cohesion needs the opposite: the point is to
    // cross a cell boundary, so it aims past the nearest one.
    void walkAway(const ScenarioContext& ctx) {
        Character* c = subject();
        if (!c) return;
        if (!haveHome_) {
            engine::readPos(c, &homeX_, &homeY_, &homeZ_);
            haveHome_ = true;
        }
        if (!cohesion_) {
            engine::walkTo(c, homeX_ + WALK_DIST, homeY_, homeZ_, WALK_SPEED);
            return;
        }
        if (!haveTarget_) pickCellTarget(ctx);
        if (haveTarget_) engine::walkTo(c, tgtX_, homeY_, tgtZ_, WALK_SPEED);
        else engine::walkTo(c, homeX_ + WALK_DIST, homeY_, homeZ_, WALK_SPEED);
    }

    // Aim just past the NEAREST zone-cell boundary. All four directions are
    // probed and the closest wins, because which way the seam runs is a
    // property of where the fixture's cage happens to sit, not something to
    // hardcode - and a wrong guess would walk the length of a whole cell.
    // Falls back to the fixed offset (haveTarget_ stays false) if the mapping
    // is unreadable, so a bad read degrades to lockpick_escape's behavior
    // rather than parking the subject at the origin.
    void pickCellTarget(const ScenarioContext& ctx) {
        struct Cand { bool axisX; float dir; };
        static const Cand kCands[4] = {
            { true,  1.0f }, { true, -1.0f }, { false, 1.0f }, { false, -1.0f }
        };
        bool  bestOk = false;
        float bestDist = 0.0f, bestEdge = 0.0f;
        bool  bestAxisX = true;
        float bestDir = 1.0f;
        for (int i = 0; i < 4; ++i) {
            const bool  axisX = kCands[i].axisX;
            const float fixed = axisX ? homeZ_ : homeX_;
            const float start = axisX ? homeX_ : homeZ_;
            float edge = 0.0f;
            if (!findCellEdge(ctx.gw, axisX, fixed, start, kCands[i].dir, &edge))
                continue;
            const float d = (edge > start) ? (edge - start) : (start - edge);
            if (!bestOk || d < bestDist) {
                bestOk = true; bestDist = d; bestEdge = edge;
                bestAxisX = axisX; bestDir = kCands[i].dir;
            }
        }
        int cx = 0, cz = 0;
        engine::cellAt(ctx.gw, homeX_, homeZ_, &cx, &cz);
        if (bestOk) {
            const float past = bestEdge + bestDir * PAST_EDGE;
            tgtX_ = bestAxisX ? past : homeX_;
            tgtZ_ = bestAxisX ? homeZ_ : past;
            haveTarget_ = true;
        }
        // Emitted once, on the owner, and named so the harness can hang the
        // screenshot on it: the interesting frame is mid-walk, not at the cage.
        char b[240];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO ESCAPE walk hand=%u,%u t=%lu ok=%d axis=%s dist=%.1f "
                  "edge=%.1f from=%.1f,%.1f to=%.1f,%.1f cell=%d,%d",
                  subjHand_[3], subjHand_[4], (unsigned long)ctx.elapsedMs,
                  haveTarget_ ? 1 : 0, bestAxisX ? "x" : "z", bestDist, bestEdge,
                  homeX_, homeZ_, tgtX_, tgtZ_, cx, cz);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubject(const ScenarioContext& ctx, unsigned int rank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, rank);
        if (idx < 0) return;
        handFromEntity(sq[idx], subjHand_);
        haveSubj_ = true;
        // Record the STARTING state next to the identity: the oracle needs a
        // "before" to call a 1->0 lock transition, and if the fixture ever
        // drifts to an unshackled start this line is what says so.
        engine::ShackleRead sr;
        Character* c = engine::resolveCharByHand(subjHand_[3], subjHand_[4],
                                                 subjHand_[0], subjHand_[1],
                                                 subjHand_[2]);
        bool ok = c && engine::readShackle(c, &sr) && sr.valid;
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO ESCAPE subj side=%s rank=%u hand=%u,%u chained=%d "
                  "shackleItem=%d lock=%d slave=%d",
                  ctx.isHost ? "host" : "join", rank, subjHand_[3], subjHand_[4],
                  (ok && sr.chained) ? 1 : 0, (ok && sr.hasShackleItem) ? 1 : 0,
                  (ok && sr.lockPresent) ? 1 : 0,
                  c ? engine::readSlaveState(c) : -1);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // One "SCENARIO ESCAPE" line: the subject's state as THIS client sees its
    // own copy. Both clients emit it for the same hand, which is what lets the
    // oracle put the owner's release and the peer's release on one timeline.
    void logEscapeLine(const ScenarioContext& ctx) {
        Character* c = engine::resolveCharByHand(subjHand_[3], subjHand_[4],
                                                 subjHand_[0], subjHand_[1],
                                                 subjHand_[2]);
        if (!c) return;
        engine::ShackleRead sr;
        if (!engine::readShackle(c, &sr) || !sr.valid) return;
        engine::FurnitureRead fr;
        bool haveFurn = engine::readFurniture(c, &fr) && fr.valid;
        engine::StatsRead st;
        bool haveStats = engine::readStats(c, &st) && st.valid;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        // "Held" must be OBSERVED before "freed" can mean anything: a caged
        // body need carry no shackle item at all, so an unlatched test would
        // report the subject free on the first sample and call that an escape.
        bool held = sr.chained || sr.lockPresent ||
                    (haveFurn && (fr.kind == 2 || fr.kind == 3));
        if (held) sawHeld_ = true;
        // First tick this client saw every hold gone - reported once, so the
        // oracle reads a crossing instant per side rather than diffing samples.
        if (sawHeld_ && !freedAtMs_ && !held) {
            freedAtMs_ = ctx.elapsedMs ? ctx.elapsedMs : 1;
            char f[144];
            _snprintf(f, sizeof(f) - 1,
                      "SCENARIO ESCAPE freed side=%s hand=%u,%u t=%lu",
                      ctx.isHost ? "host" : "join", subjHand_[3], subjHand_[4],
                      freedAtMs_);
            f[sizeof(f) - 1] = '\0'; coop::logLine(f);
        }
        // The CELL is appended last so the field order the existing oracle
        // parses stays byte-identical - cohesion mode adds evidence to the same
        // line rather than forking the format.
        int cx = 0, cz = 0;
        engine::cellAt(ctx.gw, x, z, &cx, &cz);
        char b[288];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO ESCAPE hand=%u,%u t=%lu side=%s chained=%d "
                  "shackleItem=%d lock=%d pick=%.3f slave=%d furn=%d bs=%u "
                  "pick_stat=%.1f pos=%.2f,%.2f,%.2f cell=%d,%d",
                  subjHand_[3], subjHand_[4], ctx.elapsedMs,
                  ctx.isHost ? "host" : "join",
                  sr.chained ? 1 : 0, sr.hasShackleItem ? 1 : 0,
                  sr.lockPresent ? 1 : 0, sr.lockpickChance,
                  engine::readSlaveState(c), haveFurn ? fr.kind : -1,
                  (unsigned)engine::readBodyState(c),
                  haveStats ? st.stats[STAT_LOCKPICKING_ID] : -1.0f,
                  x, y, z, cx, cz);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    const bool    cohesion_;
    unsigned long lastLogMs_;
    bool          haveSubj_;
    bool          raiseLogged_;
    bool          sawHeld_;
    bool          forceLogged_;
    bool          haveHome_;
    bool          haveTarget_;
    bool          camLogged_;
    unsigned long lastWalkMs_;
    unsigned long lastCamMs_;
    unsigned long freedAtMs_;
    float         homeX_, homeY_, homeZ_;
    float         tgtX_, tgtZ_;
    unsigned int  subjHand_[5];
};

const float EscapeScenario::RAISE_TO   = 100.0f;
const float EscapeScenario::WALK_DIST  = 60.0f;
const float EscapeScenario::WALK_SPEED = 30.0f;
const float EscapeScenario::PAST_EDGE  = 150.0f;

} // namespace

Scenario* makeCharStateScenario(const std::string& name) {
    if (name == "lockpick_escape") return new EscapeScenario("lockpick_escape", false);
    if (name == "escape_cohesion") return new EscapeScenario("escape_cohesion", true);
    if (name == "carry_order")  return new CarryOrderScenario();
    if (name == "npc_carry")    return new NpcCarryScenario();
    if (name == "bed_pose")     return new BedPoseScenario();
    if (name == "bed_wake")     return new BedWakeScenario();
    if (name == "bed_lay")      return new BedLayScenario();
    if (name == "bed_put")      return new FurnPutScenario(1);
    if (name == "cage_put")     return new FurnPutScenario(2);
    if (name == "chain_put")    return new FurnPutScenario(3);
    if (name == "pole_put")     return new FurnPutScenario(4);
    if (name == "cage_peer_sync") return new CagePeerScenario();
    if (name == "sneak_probe")  return new SneakProbeScenario();
    if (name == "sneak_pose")   return new SneakPoseScenario();
    if (name == "sneak_detect") return new SneakDetectScenario();
    if (name == "speed_sync")   return new SpeedSyncScenario();
    if (name == "speed_probe")  return new SpeedProbeScenario();
    if (name == "pause_stress") return new PauseStressScenario();
    if (name == "shackle_probe") return new ShackleProbeScenario("shackle_probe");
    if (name == "shackle_sync")  return new ShackleProbeScenario("shackle_sync");
    return 0;
}

} // namespace coop
