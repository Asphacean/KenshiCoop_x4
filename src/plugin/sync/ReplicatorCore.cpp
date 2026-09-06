// ReplicatorCore.cpp - Replicator construction + session lifecycle (monolith
// split from Replicator.cpp, 2026-07-12): the ctor defaults, the Phase 3
// unified entity-lifecycle audit (lifeName/lifeSet/lifeSweep + life_),
// resetSession (the ONE place every cross-tick hub resets on session swap),
// inbound ingest (ingest/ingestInv), tab latching/ranking, and the
// end-of-session smoothness summary.
//
// Shared hubs written here: ALL of them (resetSession clears every map);
// life_ is owned here (lifeSet is the only writer, called from every TU).
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"
#include "../core/OwnRanks.h" // Phase 3: ownerForRank() rank->PlayerId default (resolveRankOwner)

namespace coop {

Replicator::Replicator()
    : catchupK_(CATCHUP_K), snapDist_(SNAP_DIST), snapSeconds_(SNAP_SECONDS),
      combatSoftDist_(COMBAT_SOFT_DIST), combatSnapDist_(COMBAT_SNAP_DIST),
      combatBigSnapDist_(COMBAT_BIG_SNAP_DIST), combatSlideMax_(COMBAT_SLIDE_MAX),
      combatConvergeMs_(COMBAT_CONVERGE_MS),
      sendStamp_(true),
      starveHoldMs_(10000), starveHeldNow_(0),
      leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f), slewSkipFrames_(0),
      zpDown_(0), zpCarried_(0), zpFurn_(0), zpChain_(0), zpCrawl_(0),
      zpSneak_(0), zpSquadIdle_(0), zpAlias_(0), zpStall_(0),
      onsetReentries_(0),
      onActiveEarly_(0), onActiveMid_(0), onActiveSteady_(0),
      onZeroEarly_(0), onZeroMid_(0), onZeroSteady_(0),
      interpLerp_(0), interpSingle_(0), interpClampOld_(0),
      interpExtrap_(0), interpSegSnap_(0),
      hardSnapSquad_(0), hardSnapNpc_(0), hardSnapMid_(0),
      walkReissueSquad_(0), walkReissueNpc_(0), restFlipNpc_(0), restFlipMid_(0),
      combatSnapTotal_(0), combatSoftWalk_(0), combatSlide_(0), combatOrder_(0),
      combatWrongTgt_(0), combatLogTick_(0),
      interpLogTick_(0),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      marchHold_(0), marchSettle_(0), marchRelapse_(0),
      marchHoldDip_(0), marchHoldStop_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      aiSuspend_(false), aiLogTick_(0), nextEventId_(1),
      nextWorldNetId_(1), worldSeeded_(false),
      nextDropId_(1), nextPickupId_(1), nextXferId_(1),
      pendingXferSeq_(0), claimFinalSeq_(0), appliedClaimVerdictSeq_(0),
      xferScanMs_(0), nextTreatId_(1),
      quietRelapse_(0), crawlPhysRestore_(0),
      sitOrders_(0), detachUses_(0), noDetach_(false),
      dmgGuard_(false), reportCombat_(false), nextHitId_(1),
      carrySync_(true), furnSync_(true), chainSync_(true),
      stealthSync_(true), proneSync_(true),
      gateAuthority_(false), trustLogTick_(0),
      trustGrants_(0), trustRevokes_(0),
      authSuppresses_(0), authRestores_(0), authReassertMs_(0), authPruned_(0),
      censusRadius_(0.0f), censusSendMs_(0), censusRecvMs_(0), censusCulls_(0),
      censusOffCell_(0), cellYields_(0), localId_(0xFFFFFFFFu), hostDriveRefusals_(0),
      censusPubTrunc_(false), censusFreshPrev_(false), censusFreshChkMs_(0),
      censusStaleMs_(0), censusStaleEdges_(0), proxyDriftLogMs_(0),
      camHintSendMs_(0), peerCamMs_(0),
      midCursor_(0), midSliceMs_(0), midFastPromoted_(0),
      censusParkDist_(0.0f), censusParks_(0),
      censusWalkDist_(0.0f), censusWalks_(0),
      censusFreezeAi_(true),
      attentionRadius_(0.0f),
      attnFlips_(0), attnWinMs_(0), attnBaseSupp_(0), attnBaseCull_(0),
      attnBaseProxy_(0), attnVetoMs_(0), attnVetoRawN_(0), attnVetoMask_(0),
      auditRows_(false), jailProbe_(false), jailObserve_(false),
      speedLastApplied_(-1.0f), speedMyReq_(-1.0f),
      speedCombatCap_(true),
      speedMyCombat_(false), speedLastSet_(-1.0f),
      speedSeqOut_(1), speedSeqSeen_(0),
      speedLastSendMs_(0), speedCombatSampleMs_(0), speedCombatHoldMs_(0),
      spawnSync_(false), spawnPosLogMs_(0),
      spawnMintRadius_(0.0f), adoptRadius_(0.0f), censusAdopts_(0),
      censusScanMs_(0),
      poolSeen_(-1), poolSent_(-1), poolSentMs_(0), poolTotal_(-1),
      poolSeq_(0), poolAcked_(0),
      moneySync_(true), recruitSync_(true),
      squadSync_(true), tabsSeeded_(0), tabsChanged_(false),
      cellAuth_(false), cellCollapse_(false), collapsed_(false),
      claimSendMs_(0), claimAssertMs_(0), claimMapMs_(0), cellMapSeqOut_(0),
      facSeqOut_(1), facSampleMs_(0), factionSync_(true),
      doorSeqOut_(1), doorSampleMs_(0), doorSync_(true),
      buildSeqOut_(1), buildSampleMs_(0), buildSync_(true),
      bdoorSeqOut_(1), bdoorSampleMs_(0), bdoorSync_(true),
      hungerSync_(true),
      prodSeqOut_(1), prodSampleMs_(0), prodSync_(true),
      researchSeqOut_(1), researchSampleMs_(0), researchSync_(true),
      researchIntentSeeded_(false),
      deedSeqOut_(1), deedSampleMs_(0), deedAuditMs_(0), deedSync_(true),
      fixtureSeqOut_(1), fixtureSampleMs_(0), fixtureSync_(true),
      storeSync_(false), contCensusMs_(0),
      timeSync_(true), timeBrake_(true),
      timeSlew_(1.0f), timeSeqOut_(1), timeSeqSeen_(0),
      timeLastSendMs_(0), timeLastLogMs_(0), timeSlewApplied_(-1.0f),
      platoonT0_(0),
      lifeSweepMs_(0) {
    peerCam_[0] = peerCam_[1] = peerCam_[2] = 0.0f;
}

// ---- Phase 3: unified entity lifecycle ---------------------------------------
// The AUDIT layer over the authority/mint/drive machinery: every decision
// point reports the state it just put a hand into, lifeSet logs the edge, and
// the lifecycle oracle judges the journeys. Mechanics live where they were
// validated; this is the one place their OUTCOMES meet.

const char* Replicator::lifeName(int s) {
    switch (s) {
    case LIFE_DISCOVERED: return "DISCOVERED";
    case LIFE_RESOLVED:   return "RESOLVED";
    case LIFE_HI:         return "HI";
    case LIFE_MID:        return "MID";
    case LIFE_PARKED:     return "PARKED";
    case LIFE_CULLED:     return "CULLED";
    default:              return "UNKNOWN";
    }
}

int Replicator::lifeSet(const Key& k, int to, const char* reason) {
    unsigned long now = nowMs();
    Lifecycle& lc = life_[k];
    int from = lc.state;
    lc.touchMs = now;
    if (from == to) return from;
    lc.state = (u8)to;
    lc.sinceMs = now;
    lc.stuckLogMs = 0;
    // Silent seed: every census-band wilderness NPC is born PARKED, and
    // logging hundreds of those at session start buries the real journeys.
    if (from == LIFE_UNKNOWN && to == LIFE_PARKED) return from;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[life] hand=%u,%u,%u,%u,%u from=%s to=%s reason=%s",
              k.t, k.c, k.cs, k.i, k.s,
              lifeName(from), lifeName(to), reason ? reason : "-");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return from;
}

void Replicator::lifeSweep(GameWorld* gw, unsigned long now) {
    if ((now - lifeSweepMs_) < 5000) return;
    lifeSweepMs_ = now;
    // A hand not confirmed for a while left interest (or its body despawned):
    // drop the record so the map tracks the live world, and a re-appearance
    // starts a fresh journey from UNKNOWN.
    const unsigned long PRUNE_MS = 30000;
    // Self-audit: DISCOVERED means "the host swears this exists and we have
    // no body" - the mint pipeline should resolve that within its dwell +
    // round-trip budget WHEN the hand's census position sits in a locally
    // loaded zone (an unloaded far zone defers legitimately, forever if need
    // be). Older than STUCK_MS is the invisible-raid failure (the join
    // fights nothing); one line per hand per STUCK_LOG_MS so the lifecycle
    // oracle sees it without the log flooding.
    const unsigned long STUCK_MS     = 30000;
    const unsigned long STUCK_LOG_MS = 15000;
    for (std::map<Key, Lifecycle>::iterator it = life_.begin();
         it != life_.end(); ) {
        Lifecycle& lc = it->second;
        if ((now - lc.touchMs) > PRUNE_MS) { life_.erase(it++); continue; }
        if (lc.state == LIFE_DISCOVERED && (now - lc.sinceMs) > STUCK_MS &&
            (lc.stuckLogMs == 0 || (now - lc.stuckLogMs) > STUCK_LOG_MS)) {
            CensusPos cp;
            bool mintable = censusPosFor(it->first, &cp) &&
                            engine::isZoneLoadedAt(gw, cp.x, cp.y, cp.z);
            if (mintable) {
                lc.stuckLogMs = now;
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "[life] STUCK hand=%u,%u,%u,%u,%u state=DISCOVERED age=%lus (mintable)",
                          it->first.t, it->first.c, it->first.cs, it->first.i,
                          it->first.s, (now - lc.sinceMs) / 1000);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                lc.stuckLogMs = now; // re-check the zone on the next cadence
            }
        }
        ++it;
    }
}

void Replicator::resetSession() {
    // Pointer caches (dangling after the swap) + everything keyed off them.
    targets_.clear();          // interp buffers + per-body drive state
    drivenChars_.clear();
    drivenSeen_.clear();       // recently-driven grace (pointers dangle after swap)
    canonicalOf_.clear();      // capture-translation reverse map (same pointers)
    rekeyLogged_.clear();      // and the change-detector for its [rekey] line
    jailObs_.clear();          // jail-observe spike per-captive last sample
    proxyByKey_.clear();
    suppressed_.clear();
    midBand_.clear();          // host mid-band round-robin (rebuilt by next census)
    midCursor_ = 0; midSliceMs_ = 0;
    life_.clear();             // Phase 3 lifecycle: the OLD world's journeys
    lifeSweepMs_ = 0;
    // Debug markers hold raw Character* from the OLD world plus GUI label
    // objects we own - destroy the labels and drop the map before either
    // dangles into the new session.
    for (std::map<Character*, DebugMarker>::iterator mi = debugMarkers_.begin();
         mi != debugMarkers_.end(); ++mi)
        engine::markerDestroy(mi->second.label);
    debugMarkers_.clear();
    hostBody_.clear();
    attackerOf_.clear();
    combatCapMs_.clear();
    authCount_.clear();
    ownHands_.clear();
    // Protocol 36 (Task 3, WORLD-03: per-owner census_): the existence
    // census describes the OLD world's hands, for every author - every
    // owner's CensusSet is dropped, and each re-publishes within a second
    // of the new world going live.
    census_.clear();
    censusFix_.clear();
    parkMs_.clear();
    censusRecvMs_ = 0;
    censusSendMs_ = 0;
    // Protocol 43: the camera hint describes the OLD world's coordinates.
    camHintSendMs_ = 0;
    peerCamMs_ = 0;
    // Attention latches are per-hand state about the OLD world's geometry.
    attnObs_.clear();
    attnObsPeer_.clear();
    // Claims describe where bodies stand in the OLD world.
    claimSlots_.clear();
    claimedCells_.clear();
    cellLastOwner_.clear();
    claimDwell_.clear();
    cellYield_.clear();
    claimSendMs_   = 0;
    claimAssertMs_ = 0;
    claimMapMs_    = 0;
    attnFlips_ = 0;
    attnWinMs_ = 0;
    // The zone verdict describes the OLD world's streaming state.
    attnVetoMs_ = 0;
    attnVetoRawN_ = 0;
    attnVetoMask_ = 0;
    furnPeerPend_.clear();
    ownFurnExit_.clear();
    // Session maps + change-gate baselines (they describe the OLD world; the
    // reloaded save re-seeds them on first sample).
    ownBuilds_.clear();
    peerBuilds_.clear();
    mintByLocal_.clear();
    bdoorRows_.clear();
    doorRows_.clear();
    prodRows_.clear();
    researchRows_.clear(); // protocol 38: re-baselined from the new world's store
    // Phase 8 review WR-03: the one-shot baseline-seed flag has the SAME
    // lifetime as the rows it guards - leaving it latched across a world
    // reload made the next publishResearch pass on a join skip the silent
    // seed and queue the ENTIRE ~384-sid known set as reliable intents (a
    // CH_RELIABLE burst right after a coordinated load, plus ~384 spurious
    // "[research] SEND (join intent)" lines on the tag the oracle parses).
    researchIntentSeeded_ = false;
    deedRows_.clear();     // protocol 54: re-baselined from the new world's owned set
    facRows_.clear();
    invPub_.clear();
    invRecv_.clear();
    ownedContainers_.clear();
    censusContainers_.clear(); // protocol 34: re-censused in the new world
    worldTrack_.clear();
    worldProxies_.clear();
    worldSeeded_ = false; // re-baseline the reloaded world's save-native items
    weaponCensus_.clear();
    appliedDrops_.clear();
    appliedPickups_.clear();
    groundedWeapons_.clear();
    pendingPickups_.clear(); // the objects those intents named belong to the old world
    // Protocol 37: every container hand and Item* baseline is stale in the new world.
    xferBase_.clear();
    xferSeeded_.clear();
    xferPend_.clear();
    xferLatch_.clear();
    xferDefer_.clear();
    xferOut_.clear();
    appliedXfers_.clear();
    wdSuppress_.clear();
    xferScanMs_ = 0;
    // Protocol 58 (Phase 7 review CR-01): the two-phase transfer / claim-
    // arbitration state is keyed (playerId, per-sender-monotonic id) and every
    // netId/transferId/claim identity in it describes the OLD world. Keeping
    // it across a reload lets stale "commit is final" records answer the NEW
    // world's recycled keys with old verdicts (re-sent stale winners destroy
    // new items on both ends; stale COMMITTED transfer keys silently swallow
    // fresh intents), stale open windows finalize post-reload against
    // old-world netIds, and stale myClaims_ entries trigger a bogus destroy-
    // from-bag rollback on a recycled key. All of it dies with the session.
    // (The seq counters are deliberately preserved - monotonic insertion
    // order across sessions is harmless, same as the outbound seq counters.)
    pendingXfer_.clear();
    claimWindows_.clear();
    claimFinals_.clear();
    myClaims_.clear();
    appliedClaimVerdicts_.clear();
    medPub_.clear();
    medRecv_.clear();
    medNpc_.clear();
    statsPub_.clear();
    // Protocol 52: the pool baseline describes the OLD world's wallet, so a
    // reload must re-seed it - otherwise the newly loaded save's cats read as
    // one giant local purchase. Pending deltas die with the session they were
    // measured in.
    poolSeen_ = -1; poolSent_ = -1; poolSentMs_ = 0; poolTotal_ = -1;
    poolSeq_ = 0; poolAcked_ = 0;
    poolFoldSeen_.clear(); // Phase 5 (ID-01): per-owner fold high-water is
                            // also world-scoped, same as poolAcked_ above.
    // Protocol 60 (Phase 9 Plan 01, CONS-01): the arbitration state describes
    // the OLD world's contention too - a stale open window finalizing post-
    // reload would settle against owners/seqs that no longer mean anything.
    moneyFold_ = coop::MoneyFoldState();
    poolPending_.clear();
    moneyDeferred_.clear(); // WR-03 deferrals describe the OLD world's queue too
    stealthPub_.clear();
    stealthRecv_.clear(); // WR-01: receive-side clear-scoping state is session-scoped too
    pinOwned_.clear();
    pinPeer_.clear();
    pinnedOwner_.clear(); // 06-01 GAP-3: additive author record, same lifetime as pinPeer_
    moveEcho_.clear();
    exitedOwn_.clear();
    // Protocol 35: the rank latch + the engine's pointer->hand baseline both
    // describe the OLD world (containers and Character* dangle after a swap);
    // the reloaded save re-seeds them at first census/poll.
    tabRank_.clear();
    tabOwned_.clear();
    tabsSeeded_ = 0;
    rekeyedOld_.clear();
    engine::clearSquadRoster();
    probed_.clear();
    spawnReq_.clear();
    unresolvedHands_.clear();
    forceReqHands_.clear();
    spawnLogged_.clear();
    spawnReplyMs_.clear();
    censusScanMs_ = 0;
    // Speed/time consensus: re-seed from the fresh world's live state (the
    // save's speed becomes the new baseline; the join's slew re-measures).
    speedLastApplied_ = -1.0f;
    speedMyReq_       = -1.0f;
    // Phase 9 Plan 02 (CONS-02): the per-owner vote map describes the OLD
    // world's session too (poolFoldSeen_'s own rationale, verbatim) - a
    // reload re-seeds from the fresh world's live state via the first REQ.
    speedVotes_.clear();
    speedMyCombat_    = false;
    speedLastSet_     = -1.0f;
    speedSeqSeen_     = 0; // join-only now; the host's per-sender guard lives in speedVotes_
    speedLastSendMs_  = 0;
    speedCombatSampleMs_ = 0;
    speedCombatHoldMs_ = 0;
    timeSlew_         = 1.0f;
    timeSeqSeen_      = 0; // join-only now; the host's per-owner guard lives in timeReports_
    // Phase 9 Plan 02 (CONS-03): the per-owner report map describes the OLD
    // world's session too - a reload re-seeds from the fresh world's live
    // state via the first report (the speedVotes_ rationale, verbatim).
    timeReports_.clear();
    timeLastSendMs_   = 0;
    timeSlewApplied_  = -1.0f;
    // Sample-cadence clocks restart.
    facSampleMs_ = doorSampleMs_ = buildSampleMs_ = bdoorSampleMs_ = 0;
    prodSampleMs_ = 0;
    researchSampleMs_ = 0;
    deedSampleMs_ = 0;
    deedAuditMs_ = 0;
    // A coordinated load re-instantiates every runtime fixture, so both halves
    // of the protocol-55 pairing are stale: re-announce and re-match from
    // scratch rather than translate a hand into a destroyed object.
    fixtureSampleMs_ = 0;
    fixtureOut_.clear();
    fixtureMap_.clear();
    contCensusMs_ = 0;
    authReassertMs_ = 0;
    // Config gates, ownRanks_ and every OUTBOUND seq counter are deliberately
    // preserved (see the header comment).
    coop::logLine("[load] session reset: pointer caches, session maps, change gates cleared");
}

void Replicator::clearPeerReplicationState(GameWorld* gw, u32 departing) {
    // Phase 3 Plan 04 (PEER-02/03) owner-scoped rewrite: the old version
    // called the global resetSession() unconditionally, which at N>=3 would
    // destroy every OTHER connected peer's proxies/interp state the instant
    // ANY one peer disconnected. This version touches ONLY `departing`'s
    // entries; resetSession() is no longer called from here at all (it stays
    // reserved for the session-end / world-reload path).
    //
    // Ownership of a proxyByKey_ entry is resolved via its matching targets_
    // entry's Driven.owner tag (set in ingest() from the wire's ownerId,
    // Plan 02) - the same wire key drives both maps. A proxy with no
    // matching targets_ entry (owner unknown) is left untouched rather than
    // guessed at.
    //
    // Phase 5 (ID-03): this is the "disconnect destroys only that author's
    // proxies" guarantee - verified here to scope BOTH the proxyByKey_
    // despawn loop (via the targets_[key].owner cross-reference just above)
    // and the remaining targets_ erase loop below to `departing` only. Its
    // correctness depends on that owner cross-reference staying trustworthy,
    // which is exactly what ingest()'s crossOwnerCollision guard protects -
    // a silently cross-owner-overwritten Driven.owner would make THIS
    // function clear (or fail to clear) the wrong author's entries.
    //
    // ADOPTED entries are released, not destroyed (destroyIfMinted): a body
    // we adopted is one this client generated itself, so the peer leaving
    // makes it ours again rather than making it garbage. Destroying them
    // here would empty a whole town on disconnect, and bake that emptiness
    // into the next save.
    unsigned int cleared = 0, released = 0;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ) {
        std::map<Key, Driven>::iterator ti = targets_.find(it->first);
        u32 owner = (ti != targets_.end()) ? ti->second.owner : OWNER_NONE;
        if (owner != departing) { ++it; continue; }
        if (gw && it->second) {
            if (destroyIfMinted(gw, it->second)) ++cleared; else ++released;
            mintedBodies_.erase(it->second);
        }
        proxyByKey_.erase(it++);
    }
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[leave] cleared proxies=%u released=%u owner=%u",
              cleared, released, departing);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // World-item proxies (Phase 3): the world stays LIVE across a peer leave /
    // reconnect (no engine world swap), so only the DEPARTING owner's proxy
    // RootObjects need destroying here - a surviving peer's ground-item
    // proxies must keep standing exactly as they are. worldProxies_ is keyed
    // by (ownerId, netId), so ownership is the pair's first element directly
    // - no lookup needed. The world surviving does NOT mean the proxy did:
    // the peer may have left after a long trek, and anything whose block
    // unloaded on the way is already destroyed, so each one is re-resolved.
    unsigned int wcleared = 0, wstale = 0;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator wi = worldProxies_.begin();
         wi != worldProxies_.end(); ) {
        if (wi->first.first != departing) { ++wi; continue; }
        RootObject* live = gw ? liveWorldProxy(wi->second) : 0;
        if (!live) { ++wstale; worldProxies_.erase(wi++); continue; }
        if (engine::removeWorldItemProxy(gw, live)) ++wcleared;
        worldProxies_.erase(wi++);
    }
    _snprintf(b, sizeof(b) - 1, "[leave] cleared worldProxies=%u stale=%u owner=%u",
              wcleared, wstale, departing);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // Remaining owner-scoped targets_ entries (interp rings + drive flags)
    // not already erased above (e.g. a driven body resolved directly with no
    // minted proxy at all) - drop only THIS owner's entries.
    for (std::map<Key, Driven>::iterator it = targets_.begin();
         it != targets_.end(); ) {
        if (it->second.owner == departing) targets_.erase(it++);
        else ++it;
    }
    // 06-01 (GAP-3/PLAY-03): pinPeer_/pinnedOwner_ ARE now owner-scoped
    // cleaned here - the one exception to the "left untouched" rule below.
    // pinnedOwner_ (PinOwner.h) is what makes this safe: without an author
    // record, a set entry cannot be attributed to `departing` without risking
    // a SURVIVING peer's pin too (exactly why this was deliberately skipped
    // before). A departed author's recruit/squad-move pins are released so
    // the world partition can re-adopt any still-live BAKED body (the
    // ADOPTED "released, not destroyed" disposition above) and a survivor's
    // publish veto (ReplicatorPublish.cpp:94) stops permanently vetoing a
    // hand no instance will ever publish again. A RUNTIME-minted proxy for
    // one of these hands was already despawned by the targets_.owner==
    // departing loop above - releasing its pin here is a harmless no-op for
    // that case.
    {
        std::vector<Key> departedPins;
        for (std::map<Key, u32>::const_iterator pit = pinnedOwner_.begin();
             pit != pinnedOwner_.end(); ++pit) {
            if (pit->second == departing) departedPins.push_back(pit->first);
        }
        for (std::vector<Key>::const_iterator dit = departedPins.begin();
             dit != departedPins.end(); ++dit) {
            pinPeer_.erase(*dit);
        }
        unsigned int pinsReleased = coop::pinEraseOwner(pinnedOwner_, departing);
        _snprintf(b, sizeof(b) - 1, "[leave] cleared pins=%u owner=%u",
                  pinsReleased, departing);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    // Protocol 58 (Phase 7 Plan 01 Task 3, INV-02/INV-04): pendingXfer_ IS
    // now owner-scoped cleaned here too - the transfer-table carve-out out
    // of the "left untouched" rule below, mirroring pinnedOwner_'s Phase 6
    // precedent immediately above. xferEraseOwner (XferCommit.h) implements
    // host-final-else-rollback: a PENDING (pre-commit) intent authored by
    // `departing` is VOIDED - nobody ever applied it (the host had not
    // committed yet), so erasing the pending entry is a pure void with no
    // rollback mutation needed; a COMMITTED intent STANDS - the destination
    // already has the item on every survivor, and the departed player's OWN
    // copies are cleaned by the owner-scoped proxy/target teardown above,
    // NEVER the transferred item itself. This is the never-both-never-
    // neither invariant. pendingXfer_ is HOST-ONLY state (empty on a join,
    // where this call is a harmless no-op) - safe to run unconditionally on
    // every instance, same as every other owner-scoped block above. Also
    // erases any lingering xferLatch_ entries keyed to the departing owner's
    // OWN container hands (resolved via ownerOfHand, the same lookup
    // detectAndPublishTransfers uses to put srcOwnerId/dstOwnerId on the
    // wire) - the true owner who would have republished the catch-up
    // snapshot that naturally releases a latch is gone, so without this a
    // survivor's reconcile would sit blocked until the 10 s XFER_GRACE_MS
    // deadline instead of converging immediately.
    {
        unsigned int voided = 0, stood = 0;
        coop::xferEraseOwner(pendingXfer_, departing, &voided, &stood);
        unsigned int latchesCleared = 0;
        for (std::map<Key, std::map<XKey, XferLatch> >::iterator li = xferLatch_.begin();
             li != xferLatch_.end(); ) {
            unsigned int hand[5];
            handForContainerKey(li->first, hand);
            if (ownerOfHand(hand) == departing) {
                latchesCleared += (unsigned int)li->second.size();
                xferLatch_.erase(li++);
            } else {
                ++li;
            }
        }
        _snprintf(b, sizeof(b) - 1, "[leave] xfer voided=%u stood=%u owner=%u",
                  voided, stood, departing);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (latchesCleared > 0) {
            _snprintf(b, sizeof(b) - 1, "[leave] xfer latches cleared=%u owner=%u",
                      latchesCleared, departing);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Protocol 58 (Phase 7 Plan 02, INV-03/INV-04): claimWindows_ is
    // HOST-ONLY state (empty on a join, harmless no-op there, same as
    // pendingXfer_ above) - a departed CLAIMANT's bid must never win a
    // contention it can no longer participate in. coop::claimEraseClaimant
    // erases every entry authored by `departing` from every OPEN window;
    // a window left with zero claims is erased outright (nothing left to
    // finalize - the "remaining claimant wins" nettest leg this enables).
    // Windows the departing player did NOT bid into are untouched.
    {
        unsigned int claimsErased = coop::claimEraseClaimant(claimWindows_, departing);
        if (claimsErased > 0) {
            _snprintf(b, sizeof(b) - 1, "[leave] claim entries voided=%u owner=%u",
                      claimsErased, departing);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Protocol 59 (Phase 8 Plan 02 Task 3, WORLD-03): claimSlots_/census_/
    // cellLastOwner_ ARE now owner-scoped cleaned here - the presence-
    // authority carve-out out of the "left untouched" rule below, mirroring
    // pinnedOwner_'s Phase 6 precedent and pendingXfer_'s Phase 7 one. A
    // departed owner's cell-claim slots and existence census are erased so
    // they can never win a future reduce or vouch for a body's existence
    // again; cellLastOwner_ entries STILL NAMING the departed owner are also
    // erased (not just claimSlots_) - otherwise the AUTHSRC_VACATE fallback
    // in authoritySrc would keep resolving that owner's vacated cells to a
    // client who is no longer connected, the "428 freeze" shape all over
    // again but permanent instead of transient (nobody left to author those
    // bodies at all). This is the LOCKED revert-to-host disposition
    // (research Open Question 2): once erased, the cell is absent from both
    // claimSlots_ and cellLastOwner_, so the host's own claimedCells_ will
    // fail all the way open to AUTHSRC_OPEN (host) on its very next
    // computeAndBroadcastCellMap call - which the tick order already
    // guarantees runs AFTER this (processNetEvents, which calls
    // clearPeerReplicationState, precedes tickReplicateApply in
    // mainLoop_hook), so no forced out-of-band re-reduce is needed here.
    {
        unsigned int claimSlotsErased = 0;
        for (std::map<std::pair<u32, u32>, CellClaim>::iterator it = claimSlots_.begin();
             it != claimSlots_.end(); ) {
            if (it->first.first == departing) { claimSlots_.erase(it++); ++claimSlotsErased; }
            else ++it;
        }
        unsigned int lastOwnerErased = 0;
        for (std::map<std::pair<int, int>, u32>::iterator it = cellLastOwner_.begin();
             it != cellLastOwner_.end(); ) {
            if (it->second == departing) { cellLastOwner_.erase(it++); ++lastOwnerErased; }
            else ++it;
        }
        unsigned int censusErased = (unsigned int)census_.erase(departing);
        if (claimSlotsErased + lastOwnerErased + censusErased > 0) {
            _snprintf(b, sizeof(b) - 1,
                      "[leave] cell claims=%u lastOwner=%u census owner=%u",
                      claimSlotsErased, lastOwnerErased, departing);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // WR-01: drop the departed author's stealth clear-scoping records. Like
    // pinnedOwner_, these ARE owner-attributable (lastAuthor is the
    // authenticated sender). Without this, a surviving author's falling-edge
    // empty snapshot inside the departed author's STEALTH_RESEND_MS quiet
    // window would be deferred against an author that will never send again,
    // and a one-shot falling edge is never resent - the frozen seer set would
    // linger until the next detection cycle.
    for (std::map<Key, StealthRecv>::iterator sri = stealthRecv_.begin();
         sri != stealthRecv_.end(); ) {
        if (sri->second.lastAuthor == departing) stealthRecv_.erase(sri++);
        else ++sri;
    }
    // Protocol 60 (Phase 9 Plan 01, CONS-01): drop the departing owner's
    // queued (unfolded) deltas from an OPEN overdraft window only - a
    // departed buyer's contested spend must never win a fold it can no
    // longer receive (the claimEraseClaimant shape, ClaimArbiter.h:250-263,
    // applied to the one shared money window). Deliberately does NOT touch
    // poolFoldSeen_/moneyFold_.processed here (see the leave-alone note
    // below) - the connect-edge purge owns clearing those, protecting
    // against a half-dead link's late duplicates in the meantime.
    unsigned int moneyWindowErased = coop::moneyEraseOwner(moneyFold_, departing);
    // Phase 9 review WR-03 follow-through: the departing owner's DEFERRED
    // (drained-but-not-yet-offered) deltas drop too - the moneyEraseOwner
    // rationale verbatim ("a departed buyer's contested spend must never win
    // a fold it can no longer receive"), applied to the retry queue.
    unsigned int moneyDeferredErased = 0;
    for (std::deque<InboundMoneyDelta>::iterator di = moneyDeferred_.begin();
         di != moneyDeferred_.end(); ) {
        if (di->pkt.ownerId == departing) { di = moneyDeferred_.erase(di); ++moneyDeferredErased; }
        else ++di;
    }
    if (moneyWindowErased + moneyDeferredErased > 0) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[leave] money window queued=%u deferred=%u owner=%u",
                  moneyWindowErased, moneyDeferredErased, departing);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    // Protocol 60 (Phase 9 Plan 02, CONS-02/CONS-03): the departing owner's
    // speed vote and time report ARE owner-attributable disconnect drops
    // here (the pinnedOwner_/moneyEraseOwner precedent, verbatim) - without
    // this a join that leaves mid-pause/mid-combat-cap leaves the host
    // capped forever (T-09-07), and a departed laggard's stale clock report
    // keeps braking the host forever (T-09-09). Erasing the entry is the
    // WHOLE "instant vote drop" / "brake stops" mechanism: the next
    // arbitration/brake tick simply no longer sees this owner, so
    // speedReduce()'s min naturally raises and the time brake's
    // most-behind scan naturally excludes it - no separate recompute call
    // needed here.
    unsigned int speedVoteErased = (unsigned int)speedVotes_.erase(departing);
    unsigned int timeReportErased = (unsigned int)timeReports_.erase(departing);
    if (speedVoteErased + timeReportErased > 0) {
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "[leave] speed vote=%u time report=%u owner=%u",
                  speedVoteErased, timeReportErased, departing);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    // Every other resetSession()-cleared member (attnObs_/attnObsPeer_,
    // invPub_/invRecv_, xferBase_/xferPend_ (xferLatch_ now PARTIALLY
    // owner-scoped above - see the pendingXfer_ block; the rest of its
    // entries, keyed to containers NOT owned by the departing peer, stay
    // untouched here same as before), poolSeen_/poolAcked_/poolFoldSeen_/
    // moneyFold_.processed (protocol 60: KEPT at disconnect - the money-
    // window exception above is claimant-scoped, not author-scoped, mirroring
    // claimEraseClaimant vs claimEraseAuthor; the connect-edge purge is what
    // erases the processed high-water, ReplicatorCore.cpp's
    // purgeAuthorConservationState), pinOwned_, tabRank_/tabOwned_, and the
    // rest of that ~70-member list) is deliberately LEFT UNTOUCHED here: none
    // of it is keyed in a way that safely attributes individual entries to
    // `departing` without risking a surviving peer's or the local player's
    // own state, and per CONTEXT.md's "session-global cleanup only at
    // session end" carve-out that global wipe stays reserved for
    // resetSession()'s own callers (world reload; sessionResetForUi's own-
    // disconnect path), never a single peer's leave. pendingXfer_ (protocol
    // 58, Phase 7) is the transfer-table exception to this rule;
    // claimSlots_/census_/cellLastOwner_ (protocol 59, Phase 8 Plan 02 Task
    // 3) is the WORLD-03 exception; the money window above (protocol 60,
    // Phase 9 Plan 01) is the CONS-01 exception; speedVotes_/timeReports_
    // above (protocol 60, Phase 9 Plan 02) are the CONS-02/CONS-03
    // exceptions - see the blocks above, exactly like pinnedOwner_ was
    // Phase 6's exception.
}

// Protocol 58 (Phase 7 review CR-01) - see the declaration doc comment in
// Replicator.h for the full contract and the connect-edge-not-disconnect
// design rationale. Scope: every (authorId, per-sender-monotonic id) keyed
// structure in the item-conservation planes whose id space restarts with a
// fresh client process. HOST-only tables (pendingXfer_/claimWindows_/
// claimFinals_) are empty on a join, where those erases are harmless no-ops;
// the client-side dedup/rollback records (myClaims_, appliedClaimVerdicts_,
// appliedXfers_, appliedDrops_, appliedPickups_) are purged on EVERY
// instance - a stale entry there would silently SWALLOW the rejoined
// author's fresh intents/verdicts (or, for myClaims_, trigger a bogus
// destroy-from-bag rollback on a recycled netId). Erasing dedup memory never
// mutates items and can never re-apply an old packet: the only connection
// that could have replayed those keys is dead.
void Replicator::purgeAuthorConservationState(u32 authorId) {
    unsigned int xfers   = coop::xferPurgeAuthor(pendingXfer_, authorId);
    unsigned int windows = coop::claimEraseAuthor(claimWindows_, authorId);
    unsigned int finals  = coop::claimEraseAuthorFinals(claimFinals_, authorId);
    unsigned int dedup   = 0;
    for (std::map<std::pair<u32, u32>, PendingClaimIdentity>::iterator it = myClaims_.begin();
         it != myClaims_.end(); ) {
        if (it->first.first == authorId) { myClaims_.erase(it++); ++dedup; }
        else ++it;
    }
    for (std::map<std::pair<u32, u32>, coop::ClaimFinal>::iterator it = appliedClaimVerdicts_.begin();
         it != appliedClaimVerdicts_.end(); ) {
        if (it->first.first == authorId) { appliedClaimVerdicts_.erase(it++); ++dedup; }
        else ++it;
    }
    for (std::set<std::pair<u32, u32> >::iterator it = appliedXfers_.begin();
         it != appliedXfers_.end(); ) {
        if (it->first == authorId) { appliedXfers_.erase(it++); ++dedup; }
        else ++it;
    }
    for (std::set<std::pair<u32, u32> >::iterator it = appliedDrops_.begin();
         it != appliedDrops_.end(); ) {
        if (it->first == authorId) { appliedDrops_.erase(it++); ++dedup; }
        else ++it;
    }
    for (std::set<std::pair<u32, u32> >::iterator it = appliedPickups_.begin();
         it != appliedPickups_.end(); ) {
        if (it->first == authorId) { appliedPickups_.erase(it++); ++dedup; }
        else ++it;
    }
    // Phase 10 Plan 02 (SAVE-04, 10-RESEARCH.md "Rejoin vs Late-Join
    // Identity" - the one genuine purge hole found): appliedEvents_ (the
    // Phase 5 (ownerId,eventId) one-shot event dedup, Replicator.h) was
    // purged NOWHERE - not here, not at disconnect. A fresh process on a
    // reused slot restarts its event counter at 1, so its first KO/death/
    // carry/furniture events would otherwise silently collide with the
    // PREVIOUS occupant's dedup pairs and never apply. Same owner-erase
    // shape as appliedDrops_/appliedXfers_/appliedPickups_ above.
    unsigned int events = 0;
    for (std::set<std::pair<u32, u32> >::iterator it = appliedEvents_.begin();
         it != appliedEvents_.end(); ) {
        if (it->first == authorId) { appliedEvents_.erase(it++); ++events; }
        else ++it;
    }
    // Protocol 59 (Phase 8 Plan 02 Task 3, WORLD-03): claimSlots_ joins the
    // connect-edge purge - the SAME defense-in-depth this function already
    // applies to pendingXfer_/claimWindows_/claimFinals_ despite
    // clearPeerReplicationState ALSO owner-scope-cleaning them at disconnect
    // (belt-and-suspenders against a disconnect edge that ran with no
    // GameWorld, or any other path that skipped it). Load-bearing here
    // specifically because syncCellClaims' intake uses a SIGNED-difference
    // wrap guard keyed (ownerId, tabRank) -> last-seen seq
    // (ReplicatorAuthority.cpp: `(int)(p.seq - s->second.seq) <= 0`): a
    // stale slot surviving from the PREVIOUS connection at a high seq would
    // silently DROP the reconnected owner's restarted seq=1 claim as
    // "stale," leaving its cells unclaimed until the seq counter climbs back
    // past the old high-water mark - a claim-silence window the host's next
    // reduce would read as VACANCY (fail-open to host) instead of the
    // rejoining owner's real presence.
    unsigned int claimSlots = 0;
    for (std::map<std::pair<u32, u32>, CellClaim>::iterator it = claimSlots_.begin();
         it != claimSlots_.end(); ) {
        if (it->first.first == authorId) { claimSlots_.erase(it++); ++claimSlots; }
        else ++it;
    }
    // Phase 8 review WR-01: the CHANNEL seq guards join the connect-edge
    // purge - the claimSlots_ rationale verbatim ("a fresh client process
    // restarts its per-sender counters at 1"): a rejoining author's door/
    // build-door/faction/deed rows would otherwise be dropped by
    // gateSeqAcceptPerSender against the PREVIOUS connection's high-water
    // seq until the restarted counter climbed past it - silently diverging
    // that author's doors/locks/relations/deeds for the rest of the session.
    // The per-sender maps (WORLD-01 for doors/bdoors, review CR-01 for fac/
    // deed) make the purge exactly targetable: erase only the rejoining
    // author's key from every row, no other sender's state touched. prod is
    // the one scalar left: PKT_PROD is a host->join broadcast (a join never
    // publishes), so the host is the ONLY author its seqSeen ever tracks -
    // reset it only when the restarted author IS the host (a join
    // reconnecting to a restarted host), where it is unambiguous.
    unsigned int chanRows = 0;
    for (std::map<Key, DoorRow>::iterator it = doorRows_.begin();
         it != doorRows_.end(); ++it)
        chanRows += (unsigned int)it->second.seqSeen.erase(authorId);
    for (std::map<std::pair<Key, int>, BdoorRow>::iterator it = bdoorRows_.begin();
         it != bdoorRows_.end(); ++it)
        chanRows += (unsigned int)it->second.seqSeen.erase(authorId);
    for (std::map<std::string, FacRow>::iterator it = facRows_.begin();
         it != facRows_.end(); ++it)
        chanRows += (unsigned int)it->second.seqSeen.erase(authorId);
    for (std::map<Key, DeedRow>::iterator it = deedRows_.begin();
         it != deedRows_.end(); ++it)
        chanRows += (unsigned int)it->second.seqSeen.erase(authorId);
    if (authorId == (u32)CELL_OWNER_HOST) {
        for (std::map<std::pair<int, Key>, ProdRow>::iterator it = prodRows_.begin();
             it != prodRows_.end(); ++it)
            if (it->second.seqSeen != 0) { it->second.seqSeen = 0; ++chanRows; }
    }
    // Protocol 60 (Phase 9 Plan 01, CONS-01): the money plane joins the
    // connect-edge purge - the claimSlots_ rationale verbatim ("a fresh
    // client process restarts poolSeq_ at 1"): without this, a rejoined
    // join's restarted seq=1 delta would be answered MONEY_DUP forever
    // against the PREVIOUS connection's high-water in moneyFold_.processed
    // (and, defensively, the now-vestigial poolFoldSeen_ - see its doc
    // comment in Replicator.h), silently dropping every purchase the
    // rejoined join makes for the rest of the session. moneyEraseOwner also
    // drops the author's queued-but-unfolded window deltas, in case the
    // disconnect edge's own owner-scoped erase (clearPeerReplicationState)
    // did not run (belt-and-suspenders, same defense-in-depth rationale as
    // claimSlots_/claimWindows_ above).
    // Phase 9 review IN-02: poolFoldSeen_ is VESTIGIAL (moneyFold_.processed
    // replaced it as the ack vector's source - see its doc comment,
    // Replicator.h). Still erased here for hygiene, but NOT counted: the
    // purge log's money= tally names LIVE structures only, so log-based
    // debugging never reads money=2 when only one live structure was cleared.
    poolFoldSeen_.erase(authorId);
    unsigned int money = (unsigned int)moneyFold_.processed.erase(authorId);
    money += coop::moneyEraseOwner(moneyFold_, authorId);
    // Phase 9 review WR-03 follow-through: the rejoining author's DEFERRED
    // (drained-but-not-yet-offered) deltas from the PREVIOUS connection are
    // stale seq-space too - retrying them against the restarted counter
    // would fold old-session spends into the new session's pool.
    for (std::deque<InboundMoneyDelta>::iterator di = moneyDeferred_.begin();
         di != moneyDeferred_.end(); ) {
        if (di->pkt.ownerId == authorId) { di = moneyDeferred_.erase(di); ++money; }
        else ++di;
    }
    // Protocol 60 (Phase 9 Plan 02, CONS-02/CONS-03): the speed vote and
    // time report maps join the connect-edge purge too - the claimSlots_/
    // money rationale verbatim, applied to the two consensus planes. HOST-
    // only maps (empty on a join, harmless no-op there, same as
    // moneyFold_.processed above): without this, a rejoining owner's FIRST
    // fresh REQ/report after reconnect would be compared against a stale
    // SpeedVoteRec.seqSeen/TimeReport.seqSeen left from the PREVIOUS
    // connection's high-water and, if the restarted counter has not yet
    // climbed past it, silently dropped as stale - the exact door/faction
    // seq-high-water rationale, applied to the two consensus planes. The
    // disconnect edge (clearPeerReplicationState) already erases these
    // entries too; this is belt-and-suspenders for a disconnect edge that
    // ran with no GameWorld, or any other path that skipped it.
    unsigned int speedTime = (unsigned int)speedVotes_.erase(authorId);
    speedTime += (unsigned int)timeReports_.erase(authorId);
    if (xfers + windows + finals + dedup + claimSlots + chanRows + money +
        speedTime + events > 0) {
        char b[320];
        _snprintf(b, sizeof(b) - 1,
                  "[rejoin] purged stale conservation keys owner=%u xfers=%u "
                  "windows=%u finals=%u dedup=%u claimSlots=%u chanRows=%u "
                  "money=%u speedTime=%u events=%u",
                  authorId, xfers, windows, finals, dedup, claimSlots, chanRows,
                  money, speedTime, events);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::ingest(Inbound& in) {
    std::deque<InboundEntity> got;
    in.drainEntities(got);
    if (got.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<InboundEntity>::iterator it = got.begin(); it != got.end(); ++it) {
        // Wire v35: index the interp ring on the SENDER's capture time mapped
        // into the local clock, not the arrival time - path jitter (Steam
        // relay) otherwise smears straight into the snapshot spacing and the
        // buffer starves into extrapolation/snap cycles (the jumpy remote-
        // player movement). Mapping = sendMs + min-tracked offset (see
        // PeerClock); clamped to 'now' so a stamp can never land in the future.
        unsigned long t = now;
        if (sendStamp_) {
            PeerClock& pc = peerClock_[it->ownerId];
            long off = (long)(now - (unsigned long)it->sendMs);
            if (!pc.have) {
                pc.offsetMs = off; pc.have = true; pc.lastCreepMs = now;
            } else {
                unsigned long dt = now - pc.lastCreepMs;
                if (dt >= 500) { // creep ~2 ms/s toward slower routes
                    pc.offsetMs += (long)(dt / 500);
                    pc.lastCreepMs = now;
                }
                if (off < pc.offsetMs) pc.offsetMs = off;
            }
            t = (unsigned long)((long)it->sendMs + pc.offsetMs);
            if ((long)(t - now) > 0) t = now;
        }
        Driven& d = targets_[keyOf(it->e)];
        // Phase 5 (ID-03): NARROW cross-author collision guard, by decision
        // (RESEARCH "Runtime-Proxy Author Scoping" Option 2), not the full
        // std::map<std::pair<u32,Key>,...> rekey of targets_/proxyByKey_.
        // A baked hand cannot legitimately change owner under the OWN_RANKS
        // partition, so the overwrite below stays last-writer-wins - this
        // guard's job is only to make an ILLEGITIMATE cross-author clash on a
        // runtime-minted (non-baked) hand loud and attributable instead of a
        // silent merge into another author's driven copy. d.owner defaults to
        // OWNER_NONE for a freshly-created placeholder, so a first sight is
        // never flagged. CONDITIONAL FALLBACK: if this fail-safe log fires
        // during the Plan 03 live gate (evidence of a real cross-author
        // collision), the remediation is the full (ownerId,Key) rekey held in
        // reserve - narrow guard now, full rekey only on evidence.
        if (coop::crossOwnerCollision(d.owner, it->ownerId, OWNER_NONE)) {
            char cb[176]; _snprintf(cb, sizeof(cb) - 1,
                "[collision] cross-author runtime-hand clash key=%u,%u,%u,%u,%u "
                "existingOwner=%u incomingOwner=%u",
                keyOf(it->e).t, keyOf(it->e).c, keyOf(it->e).cs,
                keyOf(it->e).i, keyOf(it->e).s, d.owner, it->ownerId);
            cb[sizeof(cb) - 1] = '\0'; coop::logErrLine(cb);
        }
        // Phase 3 (OWN-02): tag this entry with the PlayerId whose stream
        // just drove it, so distinct remote squads (owners 2, 3, ...) can be
        // told apart while interpolating multiple owners simultaneously
        // (POC-01). it->ownerId is already in scope above (peerClock_).
        d.owner = it->ownerId;
        d.interp.push(it->e, t, now);
        d.lastSeenMs = now;
    }
}

void Replicator::setOwnedContainerHand(const unsigned int hand[5]) {
    ownedContainers_.clear();
    Key k; k.t = hand[0]; k.c = hand[1]; k.cs = hand[2]; k.i = hand[3]; k.s = hand[4];
    ownedContainers_.insert(k);
}

void Replicator::ingestInv(Inbound& in) {
    std::deque<InboundInv> got;
    in.drainInv(got);
    for (std::deque<InboundInv>::iterator it = got.begin(); it != got.end(); ++it) {
        Key k; k.t = it->cKey[0]; k.c = it->cKey[1]; k.cs = it->cKey[2];
        k.i = it->cKey[3]; k.s = it->cKey[4];
        // Protocol 34: a placer-key row resolves through OUR build maps to
        // the LOCAL building hand (own placement = own hand; the host's
        // placement = our minted proxy). An unresolvable key (mint not
        // landed yet / refused / tombstoned) is dropped - the sender's 5 s
        // safety resend re-delivers once the mint exists.
        if (it->keyKind == 1) {
            std::map<Key, OwnBuild>::iterator ob = ownBuilds_.find(k);
            if (ob != ownBuilds_.end()) {
                if (ob->second.removed) continue;
                k.t = ob->second.hand[0]; k.c = ob->second.hand[1];
                k.cs = ob->second.hand[2]; k.i = ob->second.hand[3];
                k.s = ob->second.hand[4];
            } else {
                std::map<Key, PeerBuild>::iterator pb = peerBuilds_.find(k);
                if (pb == peerBuilds_.end() || pb->second.minted != 1 ||
                    pb->second.removed)
                    continue;
                k.t = pb->second.localHand[0]; k.c = pb->second.localHand[1];
                k.cs = pb->second.localHand[2]; k.i = pb->second.localHand[3];
                k.s = pb->second.localHand[4];
            }
        }
        InvRecv& r = invRecv_[k];
        r.ownerId   = it->ownerId;
        r.items     = it->items; // latest snapshot supersedes
        r.dirty     = true;
        r.truncated = (it->flags & INV_FLAG_TRUNCATED) != 0;
    }
}

void Replicator::latchTabs(const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return; // legacy per-tick ranking needs no state
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabRank_.find(ctnrs[i]) != tabRank_.end()) continue;
        unsigned int next = (unsigned int)tabRank_.size();
        tabRank_[ctnrs[i]] = next;
        if (next >= 2) { // session-start seeding of the standard 2-tab save is silent
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[squad] LATCH cont=%u,%u rank=%u",
                      ctnrs[i].first, ctnrs[i].second, next);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::decideTabs(const EntityState* raw, unsigned int nSquad,
                            const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return;   // legacy per-tick ranking owns nothing durably
    bool seeding = (tabsSeeded_ == 0);
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabOwned_.find(ctnrs[i]) != tabOwned_.end()) continue;
        unsigned int rank = tabRankFor(ctnrs[i], ctnrs);
        bool owned;
        const char* why;
        if (seeding) {
            // The save's own tabs, ranked identically on both clients: the
            // historical rule, so nothing about a normal session changes.
            owned = ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
            why = "seed";
        } else {
            // A tab that did not exist at session start. Its rank is a local
            // number the peer does not share, so the rank rule cannot answer -
            // ask who authored the bodies standing in it. The pins are still
            // present at this moment (the move/recruit that created the tab is
            // what put them there); capturing the answer HERE is what makes it
            // survive the later EXIT that clears them.
            int verdict = 0;   // +1 ours, -1 peer's, 0 nobody claims it
            for (unsigned int m = 0; m < nSquad; ++m) {
                if (raw[m].hContainer != ctnrs[i].first ||
                    raw[m].hContainerSerial != ctnrs[i].second) continue;
                Key k = keyOf(raw[m]);
                if (pinOwned_.count(k)) { verdict = 1; break; }
                if (pinPeer_.count(k))  verdict = -1;
            }
            owned = (verdict == 1) ? true
                                   : ((verdict == -1) ? false : isHostRole());
            why = (verdict == 1) ? "pin-own"
                                 : ((verdict == -1) ? "pin-peer" : "host-fallback");
        }
        tabOwned_[ctnrs[i]] = owned;
        // Phase 3 Plan 03: a NEW tab verdict (seeding or dynamic) is exactly
        // "the tab set changed" - publishOwned (ReplicatorPublish.cpp) checks
        // this once per tick to trigger announceOwnRanks() on the host.
        tabsChanged_ = true;
        if (!seeding) {
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[squad] TABOWN cont=%u,%u rank=%u own=%d via=%s",
                      ctnrs[i].first, ctnrs[i].second, rank, owned ? 1 : 0, why);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Freeze the shared-rank prefix at the end of the seeding pass.
    if (seeding && !ctnrs.empty()) tabsSeeded_ = (unsigned int)tabRank_.size();
}

bool Replicator::ownsTab(const std::pair<u32, u32>& key, unsigned int rank) const {
    std::map<std::pair<u32, u32>, bool>::const_iterator it = tabOwned_.find(key);
    if (it != tabOwned_.end()) return it->second;
    // Not yet decided (squadSync_ off, or the first tick of a fresh container):
    // the historical rule, so this is never MORE permissive than before.
    return ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
}

// Phase 3 Plan 03 (OWN-01/OWN-03): apply the host-announced authoritative
// map<PlayerId,set<rank>> wholesale - the host always broadcasts the FULL
// roster, never a delta, so a straight assignment is correct (a player who
// dropped off the map is simply absent from every set, same as never
// announced).
void Replicator::setAllOwnRanks(const std::map<u32, std::set<unsigned int> >& m) {
    allOwnRanks_ = m;
}

// Phase 3 Plan 03 (OWN-01/OWN-03): rank -> owner resolution. Consults the
// host-announced allOwnRanks_ first (a linear scan over at most MAX_PLAYERS
// entries - negligible next to the per-tick squad scan that calls this once
// per member); falls back to the deterministic rank=playerId default
// (ownerForRank) when allOwnRanks_ is empty (no announcement received/built
// yet) OR when no entry in it claims this rank (an unassigned rank still
// resolves to its own default owner, never OWNER_NONE).
u32 Replicator::resolveRankOwner(unsigned int rank) const {
    for (std::map<u32, std::set<unsigned int> >::const_iterator it = allOwnRanks_.begin();
         it != allOwnRanks_.end(); ++it) {
        if (it->second.count(rank)) return it->first;
    }
    return ownerForRank(rank);
}

unsigned int Replicator::tabRankFor(const std::pair<u32, u32>& key,
                                    const std::vector<std::pair<u32, u32> >& ctnrs) const {
    if (squadSync_) {
        std::map<std::pair<u32, u32>, unsigned int>::const_iterator it =
            tabRank_.find(key);
        return it == tabRank_.end() ? 0xFFFFFFFFu : it->second;
    }
    return (unsigned int)(std::lower_bound(ctnrs.begin(), ctnrs.end(), key)
                          - ctnrs.begin());
}

void Replicator::logSmoothSummary() {
    float zeroFrac = (activeFrames_ > 0)
                         ? (float)zeroWhileActive_ / (float)activeFrames_
                         : 0.0f;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO SMOOTH active=%lu zeroWhileActive=%lu zeroFrac=%.3f maxStep=%.3f slewSkip=%lu",
              activeFrames_, zeroWhileActive_, zeroFrac, maxStep_, slewSkipFrames_);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    // zeroFrac population audit (see the zp* declarations): what the frozen
    // frames actually WERE. Everything but `free` is a body that could not have
    // walked that frame, so it bounds how much of zeroFrac is even addressable.
    char zp[208];
    _snprintf(zp, sizeof(zp) - 1,
              "SCENARIO ZEROPOP zero=%lu down=%lu carried=%lu furn=%lu chain=%lu "
              "crawl=%lu sneak=%lu squadIdle=%lu alias=%lu stall=%lu",
              zeroWhileActive_, zpDown_, zpCarried_, zpFurn_, zpChain_,
              zpCrawl_, zpSneak_, zpSquadIdle_, zpAlias_, zpStall_);
    zp[sizeof(zp) - 1] = '\0';
    coop::logLine(zp);

    // Motion-onset audit (see the onset* declarations): is zeroFrac reporting
    // stutter, or is it reporting how often the body re-entered scoring? Each
    // bucket's fraction is what to read - if early runs far above steady, the
    // metric is charging render catch-up at motion onset, and the low-sample
    // runs fail because bursty motion pays that toll repeatedly.
    float eF = (onActiveEarly_  > 0) ? (float)onZeroEarly_  / (float)onActiveEarly_  : 0.0f;
    float mF = (onActiveMid_    > 0) ? (float)onZeroMid_    / (float)onActiveMid_    : 0.0f;
    float sF = (onActiveSteady_ > 0) ? (float)onZeroSteady_ / (float)onActiveSteady_ : 0.0f;
    char on[240];
    _snprintf(on, sizeof(on) - 1,
              "SCENARIO ONSET reentries=%lu earlyN=%lu earlyZero=%lu earlyFrac=%.3f "
              "midN=%lu midZero=%lu midFrac=%.3f steadyN=%lu steadyZero=%lu steadyFrac=%.3f",
              onsetReentries_, onActiveEarly_, onZeroEarly_, eF,
              onActiveMid_, onZeroMid_, mF,
              onActiveSteady_, onZeroSteady_, sF);
    on[sizeof(on) - 1] = '\0';
    coop::logLine(on);

    // Anim-truth oracle: fraction of translating frames that did NOT report a
    // real walk state. Low == engine is walking the body (Stage 3 goal); high ==
    // the body slides a static pose (the float bug).
    unsigned long floatFrames = (translateFrames_ > walkTruthFrames_)
                                    ? (translateFrames_ - walkTruthFrames_) : 0;
    float floatFrac = (translateFrames_ > 0)
                          ? (float)floatFrames / (float)translateFrames_
                          : 0.0f;
    char a[160];
    _snprintf(a, sizeof(a) - 1,
              "SCENARIO ANIM translate=%lu walkTruth=%lu floatFrac=%.3f",
              translateFrames_, walkTruthFrames_, floatFrac);
    a[sizeof(a) - 1] = '\0';
    coop::logLine(a);

    // March-in-place oracle: of the at-rest frames, fraction where the body played
    // a walk clip while NOT moving. High == "walking on the spot" (the failure the
    // float oracle cannot see, e.g. a host-seated NPC stuck walking on the join).
    float marchFrac = (restSampleFrames_ > 0)
                          ? (float)marchFrames_ / (float)restSampleFrames_
                          : 0.0f;
    // hold/settle/rlps attribute those march frames (see the counter decls). They
    // are appended AFTER marchFrac so Test-MarchInPlace's regex still matches.
    char m[224];
    _snprintf(m, sizeof(m) - 1,
              "SCENARIO MARCH restSamples=%lu march=%lu marchFrac=%.3f "
              "hold=%lu settle=%lu rlps=%lu holdDip=%lu holdStop=%lu",
              restSampleFrames_, marchFrames_, marchFrac,
              marchHold_, marchSettle_, marchRelapse_,
              marchHoldDip_, marchHoldStop_);
    m[sizeof(m) - 1] = '\0';
    coop::logLine(m);

    // Step-2 pruning evidence: how often the legacy quieting patchwork actually
    // fired this run. Sustained relapse=0 across regressions = the I11 re-quiet is
    // dead code under default AI-suspend and can be deleted; detach counts feed the
    // KENSHICOOP_NO_DETACH A/B decision.
    char q[160];
    _snprintf(q, sizeof(q) - 1,
              "SCENARIO QUIET relapse=%lu sitOrders=%lu detach=%lu noDetach=%d "
              "crawlPhys=%lu",
              quietRelapse_, sitOrders_, detachUses_, noDetach_ ? 1 : 0,
              crawlPhysRestore_);
    q[sizeof(q) - 1] = '\0';
    coop::logLine(q);

    // Step-4 evidence: how much of the driven set the divergence gate handed back
    // to local AI. grants>0 = the mechanism engages; the npc_track oracle proves
    // trusted bodies still track.
    if (gateAuthority_) {
        char t[128];
        _snprintf(t, sizeof(t) - 1,
                  "SCENARIO TRUST grants=%lu revokes=%lu", trustGrants_, trustRevokes_);
        t[sizeof(t) - 1] = '\0';
        coop::logLine(t);
    }

    // Step-5 evidence: suppression churn under the hysteresis band (split_interest
    // metric; boundary flip-flops show up as high counts).
    char au[112];
    _snprintf(au, sizeof(au) - 1,
              "SCENARIO AUTH suppresses=%lu restores=%lu", authSuppresses_, authRestores_);
    au[sizeof(au) - 1] = '\0';
    coop::logLine(au);

    // Protocol 36 jumpiness evidence: what regime the interp buffer ran in
    // (extrapFrac = starvation share of all samples) and how often the drive
    // layer had to hard-snap / re-path. Under WAN jitter these are the numbers
    // the interp fixes must move.
    {
        unsigned long total = interpLerp_ + interpSingle_ + interpClampOld_ +
                              interpExtrap_ + interpSegSnap_;
        float extrapFrac = (total > 0)
                               ? (float)(interpExtrap_ + interpClampOld_) / (float)total
                               : 0.0f;
        char ip[240];
        _snprintf(ip, sizeof(ip) - 1,
                  "SCENARIO INTERP samples=%lu lerp=%lu extrap=%lu clamp=%lu seg=%lu "
                  "extrapFrac=%.3f snapSq=%lu snapNpc=%lu reissueSq=%lu reissueNpc=%lu "
                  "restFlip=%lu pruned=%lu",
                  total, interpLerp_, interpExtrap_, interpClampOld_, interpSegSnap_,
                  extrapFrac, hardSnapSquad_, hardSnapNpc_,
                  walkReissueSquad_, walkReissueNpc_, restFlipNpc_, authPruned_);
        ip[sizeof(ip) - 1] = '\0';
        coop::logLine(ip);
    }

    // Step-3 evidence: how many locally-simulated melee hits the damage guard
    // intercepted vs passed through. guarded>0 in a combat scenario proves the
    // hook engaged (complements the blood-flat vitals check).
    if (dmgGuard_) {
        unsigned long guarded = 0, passed = 0;
        engine::damageGuardStats(&guarded, &passed);
        char dg[112];
        _snprintf(dg, sizeof(dg) - 1,
                  "SCENARIO DMGGUARD guarded=%lu passed=%lu", guarded, passed);
        dg[sizeof(dg) - 1] = '\0';
        coop::logLine(dg);
    }
}


} // namespace coop
