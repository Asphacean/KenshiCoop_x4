// ReplicatorAuthority.cpp - host-authority enforcement on the join (monolith
// split from Replicator.cpp, 2026-07-12): applyNpcCensus (protocol 36 census
// intake), enforceHostAuthority (suppress/park/cull of local copies the host
// does not corroborate, far-mint requests), parkDivergedCopy, and the debug
// marker HUD plumbing (debugMark/pruneDebugMarkers).
//
// Shared hubs: owns census_ (per-owner, Task 3 WORLD-03) + suppressed_; reads
// proxyByKey_ (minted by the spawn TU); writes life_ via lifeSet.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

void Replicator::debugMark(Character* c, int colorId, const char* tag) {
    static int en = -1;
    if (en < 0) {
        const char* e = getenv("KENSHICOOP_DEBUG_MARKERS");
        en = (e && e[0] == '1') ? 1 : 0;
    }
    if (en != 1 || !c) return;
    ObjectHand oh;
    bool haveHand = engine::charHandOf(c, oh);
    std::map<Character*, DebugMarker>::iterator it = debugMarkers_.find(c);
    // Two ways an entry found here can be somebody else's, both fatal if trusted:
    // the address was recycled onto a different body, or the GUI has already
    // destroyed the label. Either means forget the entry and mint a fresh label
    // rather than re-caption a dead one.
    if (it != debugMarkers_.end()) {
        bool mine  = haveHand && it->second.index == oh.index &&
                                 it->second.serial == oh.serial;
        bool alive = engine::markerAlive(it->second.label);
        if (!mine || !alive) {
            if (alive) engine::markerDestroy(it->second.label);
            debugMarkers_.erase(it);
            it = debugMarkers_.end();
            // Say so, rate-limited. Without this the guard is invisible and a run
            // that does not crash proves nothing about whether it was needed -
            // which is the whole question, since the bug it prevents took a mint
            // burst to reach. reused = the engine handed a new body an address we
            // still had a label for; dead = the GUI destroyed the label under us.
            static unsigned int nStale = 0;
            if (++nStale <= 20u) {
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[marker] stale entry dropped (%s) n=%u",
                    !mine ? "address reused" : "label destroyed by GUI", nStale);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }
    if (it != debugMarkers_.end() && it->second.color == colorId) return;
    char nm[40];
    engine::charName(c, nm, sizeof(nm));
    char cap[64];
    _snprintf(cap, sizeof(cap) - 1, "%s %s", tag, nm);
    cap[sizeof(cap) - 1] = '\0';
    if (it == debugMarkers_.end()) {
        // No hand means no way to tell this body from the next one to land on
        // this address, so don't cache a label we could never re-identify.
        if (!haveHand) return;
        void* l = engine::markerCreate(c, cap, colorId);
        if (l) {
            DebugMarker m;
            m.label = l; m.color = colorId;
            m.index = oh.index; m.serial = oh.serial;
            debugMarkers_[c] = m;
        }
    } else if (engine::markerUpdate(it->second.label, cap, colorId)) {
        it->second.color = colorId;
    }
}

void Replicator::applyNpcCensus(GameWorld* gw, Inbound& in) {
    std::deque<InboundNpcCensus> got;
    in.drainNpcCensus(got);
    if (got.empty()) return;
    // Task 3 (WORLD-03): PKT_NPC_CENSUS is now Class A (relayed join->join),
    // so a single drain can legitimately hold DISTINCT owners' packets (N-1
    // publishers @ 1 Hz) - processing only got.back() (the old 2-player
    // "latest wins" shortcut) would silently drop every OTHER owner's row,
    // the exact single-set wipe this per-owner refactor exists to close.
    // Keep the LAST entry per owner (within-owner latest-wins, unchanged),
    // then apply each distinct owner's census independently.
    std::map<u32, const InboundNpcCensus*> latestByOwner;
    for (size_t i = 0; i < got.size(); ++i) latestByOwner[got[i].ownerId] = &got[i];
    for (std::map<u32, const InboundNpcCensus*>::iterator li = latestByOwner.begin();
         li != latestByOwner.end(); ++li) {
        applyOneNpcCensus(gw, *li->second);
    }
}

// Shared per-owner apply primitive - see the declaration doc comment in
// Replicator.h. Mirrors the pre-Task-3 applyNpcCensus body exactly, keyed
// into census_[nc.ownerId] instead of the old bare censusHands_/censusOwner_/
// censusPos_/censusPrev_/censusPrevMs_ members - a second author's census no
// longer overwrites the first's existence claims, because each owner now
// gets its OWN CensusSet slice.
void Replicator::applyOneNpcCensus(GameWorld* gw, const InboundNpcCensus& nc) {
    CensusSet& cs = census_[nc.ownerId];
    // Keep the outgoing rows as the previous sample before they are overwritten:
    // two consecutive census positions are the only evidence the join has of how
    // fast the host's copy of an unstreamed body is travelling, and the walk-
    // converge band has to out-pace exactly that.
    cs.prev.swap(cs.pos);
    cs.prevMs = cs.recvMs;
    cs.pos.clear();
    cs.hands.clear();
    unsigned int n = (unsigned int)(nc.hands.size() / 5);
    bool havePos = nc.pos.size() >= (size_t)n * 3;
    // PER-ROW ownership check (2026-08-08). A row only speaks for a cell we
    // agree the SENDER owns - the publish side already filters, but against
    // the SENDER's map, and the two maps are only eventually equal: a claim
    // reaches the peer a round trip after it reaches us, so there is always a
    // window in which each side resolves a cell differently and both believe
    // they author it. cellLastOwner_ can make such a window permanent (see
    // Replicator.h), and rebuilding authority out of converged state to close
    // it was measured strictly worse - so the receiver has to tolerate
    // disagreement rather than the publisher having to avoid it.
    //
    // Judging each row against OUR map is what makes the window harmless: a row
    // may only speak for a cell we agree the sender owns, so while the two
    // disagree, each side simply ignores the other's rows about the contested
    // region and keeps authoring its own copies. Rejected rows are NOT culled
    // or hidden - a rejection means we think the cell is ours, and
    // authorHoldsBody already declines to judge anything in a cell we own, so
    // the body stays ours to author and nobody else's to enforce against.
    unsigned int nOff = 0;
    for (unsigned int i = 0; i < n; ++i) {
        Key k;
        k.t  = nc.hands[i * 5 + 0];
        k.c  = nc.hands[i * 5 + 1];
        k.cs = nc.hands[i * 5 + 2];
        k.i  = nc.hands[i * 5 + 3];
        k.s  = nc.hands[i * 5 + 4];
        CensusPos cp;
        if (havePos) {
            cp.x = nc.pos[i * 3 + 0];
            cp.y = nc.pos[i * 3 + 1];
            cp.z = nc.pos[i * 3 + 2];
        }
        // Positionless rows cannot be placed in a cell, so they cannot be
        // judged and are taken at face value, as before.
        if (cellAuth_ && gw && havePos &&
            authorityFor(gw, cp.x, cp.z) != nc.ownerId) {
            ++nOff;
            continue;
        }
        cs.hands.insert(k);
        if (havePos) cs.pos[k] = cp;
    }
    censusOffCell_ += nOff;
    cs.recvMs = nowMs();
    censusRecvMs_ = cs.recvMs; // aggregate "last ANY census" tracker (censusFresh gate)
    static unsigned long logTick = 0;
    if ((cs.recvMs - logTick) > 10000) {
        logTick = cs.recvMs;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[census] recv owner=%u n=%u kept=%u offcell=%u culls=%lu "
                  "(offcell=%lu cellYields=%lu hostRefus=%lu owners=%u)",
                  nc.ownerId, n, n - nOff, nOff, censusCulls_, censusOffCell_,
                  cellYields_, hostDriveRefusals_, (unsigned)census_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

bool Replicator::censusHasAny(const Key& k) const {
    // Phase 8 review WR-02: gate each owner's slice on ITS OWN recvMs, not
    // the aggregate censusRecvMs_ - a connected-but-silent owner's frozen
    // hands must stop vouching existence once its stamp goes stale, restoring
    // the ~1 s self-limit the old single-set wipe gave for free. The
    // threshold matches the aggregate censusFresh gate (5000 ms), so with a
    // single publishing peer (2-player) the two gates coincide exactly.
    unsigned long now = nowMs();
    for (std::map<u32, CensusSet>::const_iterator it = census_.begin();
         it != census_.end(); ++it) {
        if (it->second.recvMs == 0 ||
            (now - it->second.recvMs) > (unsigned long)CENSUS_OWNER_STALE_MS)
            continue; // stale owner: its claims no longer speak
        if (it->second.hands.find(k) != it->second.hands.end()) return true;
    }
    return false;
}

bool Replicator::censusPosFor(const Key& k, CensusPos* out) const {
    // WR-02: same per-owner staleness skip as censusHasAny - a stale owner's
    // minutes-old positions must not keep parking/walking local copies onto
    // coordinates nobody is authoring anymore.
    unsigned long now = nowMs();
    for (std::map<u32, CensusSet>::const_iterator it = census_.begin();
         it != census_.end(); ++it) {
        if (it->second.recvMs == 0 ||
            (now - it->second.recvMs) > (unsigned long)CENSUS_OWNER_STALE_MS)
            continue; // stale owner: its claims no longer speak
        std::map<Key, CensusPos>::const_iterator pit = it->second.pos.find(k);
        if (pit != it->second.pos.end()) {
            if (out) *out = pit->second;
            return true;
        }
    }
    return false;
}

unsigned int Replicator::censusTotalCount() const {
    unsigned int n = 0;
    for (std::map<u32, CensusSet>::const_iterator it = census_.begin();
         it != census_.end(); ++it) {
        n += (unsigned int)it->second.hands.size();
    }
    return n;
}

void Replicator::enforceHostAuthority(GameWorld* gw, u32 localId) {
    if (!gw) return;
    // Hysteresis (step 5, spike 18): a hard streamed/unstreamed edge churned
    // boundary NPCs. Suppress only after a sustained unstreamed run (~1 s), and
    // restore only after a sustained streamed dwell (~2 s), counted in frames.
    const unsigned int SUPPRESS_AFTER_FRAMES = 75;
    const unsigned int RESTORE_AFTER_FRAMES  = 150;

    // Hands the host streamed a fresh sample for this tick = the authoritative set.
    std::set<Key> keep;
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        if (it->second.fresh) keep.insert(it->first);
    }

    // Proxy bodies are EXEMPT from authority judgment (2026-07-11 census-mint
    // fix): a proxy's LOCAL hand never matches its streamed key, so the wide
    // pass saw every far-minted proxy as a census-absent ghost and froze it
    // ~1 s after binding (spawn_far run 124346: all four proxies culled at
    // their mint position, then the drive's teleports no-opped on the frozen
    // bodies forever). Their existence authority is the census entry for the
    // STREAM key that minted them; drive/starve policy is applyTargets'.
    // ...but exempt from EXISTENCE judgment only. The same exemption used to
    // skip parkDivergedCopy too, and nothing else bounds a proxy's position:
    // the drive corrects it only while the host is streaming it, and a proxy
    // is by definition a body that was out of stream range when it was minted.
    // Left alone it walks off on local AI - 561 u and still swinging in run
    // 20260806_091951 - which is the field report's "an enemy attacking my
    // character on the join, not on the host". Its census row is keyed by the
    // STREAM key that minted it, never its local hand, so keep that mapping.
    std::set<Character*> proxyChars;
    std::map<Character*, Key> proxyKeyOf;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ++it) {
        proxyChars.insert(it->second);
        proxyKeyOf[it->second] = it->first;
    }
    // Un-hide anything suppressed before it became a proxy / driven body (the
    // mint can land on a body a previous pass already judged).
    for (std::map<Key, Character*>::iterator it = suppressed_.begin();
         it != suppressed_.end(); ) {
        if (proxyChars.find(it->second) != proxyChars.end() ||
            drivenChars_.find(it->second) != drivenChars_.end()) {
            engine::restoreNpc(gw, it->second);
            ++authRestores_;
            { char b[128]; _snprintf(b, sizeof(b) - 1,
                "[authority] restore NPC hand=%u,%u (proxy/driven exemption; supp=%u)",
                (unsigned)it->first.i, (unsigned)it->first.s,
                (unsigned)suppressed_.size() - 1);
              b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            suppressed_.erase(it++);
        } else ++it;
    }

    // Enumerate the join's local world NPCs (same interest query as the host).
    const unsigned int MAX_NPCS = 256;
    static Character*  chars[MAX_NPCS]; // main-thread only
    static EntityState states[MAX_NPCS];
    bool nearTrunc = false;
    unsigned int n = engine::listNpcs(gw, chars, states, MAX_NPCS, &nearTrunc);

    // Protocol 36 wide-radius existence pass: enumerate out to the census
    // radius so local-only ghosts get culled at render range instead of at the
    // ~200 u stream bubble (the 2026-07-09 field report). Only while the
    // host's census is FRESH - a silent census (host lagging, channel down)
    // DISABLES wide culling rather than mass-suppressing the loaded area.
    static Character*  wChars[NPC_CENSUS_MAX]; // main-thread only
    static EntityState wStates[NPC_CENSUS_MAX];
    unsigned int wn = 0;
    bool wideTrunc = false;
    bool censusFresh = censusRadius_ > 0.0f && censusRecvMs_ != 0 &&
                       (nowMs() - censusRecvMs_) <= 5000;
    if (censusFresh)
        wn = engine::listNpcsWide(gw, censusRadius_, wChars, wStates, NPC_CENSUS_MAX,
                                  &wideTrunc);

    // Phase 0.5: account the time wide culling spends DISABLED. Staleness is a
    // deliberate fail-open (a silent host must not mass-suppress a loaded area),
    // but it is also unbounded ghost accumulation: nothing is judged while it
    // lasts, and a body that drifts outside censusRadius_ before the census
    // returns is never judged again. Host-side zone streaming stalls the main
    // thread, and travel is a continuous zone stream - so the window opens
    // exactly when it does the most damage.
    {
        unsigned long nowF = nowMs();
        if (censusFreshChkMs_ != 0 && !censusFresh)
            censusStaleMs_ += (nowF - censusFreshChkMs_);
        if (censusFreshChkMs_ != 0 && censusFreshPrev_ && !censusFresh) {
            ++censusStaleEdges_;
            char b[144]; _snprintf(b, sizeof(b) - 1,
                "[census] STALE (wide culling disabled; last recv %lums ago, edges=%lu)",
                censusRecvMs_ ? (nowF - censusRecvMs_) : 0ul, censusStaleEdges_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else if (censusFreshChkMs_ != 0 && !censusFreshPrev_ && censusFresh) {
            char b[144]; _snprintf(b, sizeof(b) - 1,
                "[census] fresh again (wide culling re-enabled; staleMs=%lu edges=%lu)",
                censusStaleMs_, censusStaleEdges_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        censusFreshPrev_  = censusFresh;
        censusFreshChkMs_ = nowF;
    }

    // Proxy divergence telemetry. A minted proxy is exempt from BOTH passes
    // below (its local hand matches no other client, so the wide pass used to
    // read every far-minted proxy as a ghost and freeze it at its mint spot),
    // and that exemption also skips parkDivergedCopy - so nothing bounds how
    // far a proxy's local copy can wander from the body it stands for. The
    // 2026-08-05 field report is what that looks like from the outside: an
    // enemy attacking on the join while the host, holding the same body 213 u
    // away with nobody near it, had it at peace. Measure the gap before
    // deciding what to do about it - one line per proxy per second, against
    // the census position of the STREAM key that minted it.
    if (censusFresh && !proxyChars.empty()) {
        unsigned long nowD = nowMs();
        if ((nowD - proxyDriftLogMs_) >= 1000) {
            proxyDriftLogMs_ = nowD;
            for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
                 it != proxyByKey_.end(); ++it) {
                CensusPos cp;
                if (!censusPosFor(it->first, &cp)) continue;
                float lx = 0, ly = 0, lz = 0;
                if (!engine::readPos(it->second, &lx, &ly, &lz)) continue;
                float d = dist3(lx, ly, lz, cp.x, cp.y, cp.z);
                // How far the OWNER's copy moved since the last sweep. The
                // census is 1 Hz and the game can run at 5x, so a sprinting
                // body's local copy sits a whole census tick behind - it reads
                // as hundreds of units of "drift" while actually tracking
                // faithfully (the giveaway: local lands on the host position
                // from the previous sample). Only a body whose owner is
                // roughly still can be judged on distance alone.
                float hstep = -1.0f;
                std::map<Key, CensusPos>::iterator pv =
                    proxyDriftPrev_.find(it->first);
                if (pv != proxyDriftPrev_.end())
                    hstep = dist3(pv->second.x, pv->second.y, pv->second.z,
                                  cp.x, cp.y, cp.z);
                proxyDriftPrev_[it->first] = cp;
                // streamed=1 means applyTargets has a fresh sample and IS
                // driving it; the uncorrected case is the one that can run away.
                std::map<Key, Driven>::iterator dt = targets_.find(it->first);
                bool streamed = (dt != targets_.end()) && dt->second.fresh;
                engine::CombatRead pc;
                bool fighting = engine::readCombat(it->second, &pc) &&
                                (pc.inCombat || pc.modeActive);
                char b[208]; _snprintf(b, sizeof(b) - 1,
                    "[proxy] drift hand=%u,%u d=%.0f local=%.0f,%.0f host=%.0f,%.0f "
                    "streamed=%d fight=%d hstep=%.0f",
                    (unsigned)it->first.i, (unsigned)it->first.s, d, lx, lz,
                    cp.x, cp.z, streamed ? 1 : 0, fighting ? 1 : 0,
                    hstep);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    // Under presence authority this pass runs on both sides, so the audit label
    // has to follow the role rather than the pass. With cellAuth off it is
    // join-only and this still reads "join".
    const char* roleTag = isHostRole() ? "host" : "join";
    if (auditRows_) {
        notePlatoons(gw, states, n, roleTag);
        notePlatoons(gw, wStates, wn, roleTag);
    }

    // 04-09 gap-closure #2 (census_convergence): every DRIVEN body this
    // client currently binds to a wire key - not just minted proxies - can
    // carry a LOCAL engine hand that differs from that wire key (mint rekey,
    // OR a combat-detach re-container: detachFromTownAI changes the body's
    // container/index without the host ever hearing about it). The
    // classification loop below enumerates bodies through GetCharsInSphere,
    // which only ever sees the LOCAL hand; keying an audit row on it
    // produces a row the host's own wire-keyed dump can never match, in
    // ADDITION to (not instead of) the correctly wire-keyed row the
    // targets_/censusHands_ fallback pass below emits for the SAME body -
    // a permanent spurious "join-only" duplicate every dump. Measured
    // 2026-09-02 (run 20260902_120251_N4, after the mint-only fix below):
    // a combat-detached 'Ninja Guard' (never minted - resolveCharByHand on
    // its OWN wire key still resolves it fine post-detach) logged this
    // exact duplicate for 12 straight dumps.
    //
    // Resolve every targets_ entry to its live pointer ONCE here (proxies
    // via proxyByKey_, everything else via the same resolveCharByHand
    // round-trip the fallback pass already relies on) so the classification
    // loop can key the row on the wire hand the host actually published,
    // whichever pass first enumerates the body.
    std::map<Character*, Key> wireKeyOfChar;
    if (auditRows_) {
        for (std::map<Key, Driven>::iterator wi = targets_.begin();
             wi != targets_.end(); ++wi) {
            const Key& wk = wi->first;
            std::map<Key, Character*>::iterator pxw = proxyByKey_.find(wk);
            if (pxw != proxyByKey_.end()) {
                if (pxw->second) wireKeyOfChar[pxw->second] = wk;
                continue;
            }
            Character* rc = engine::resolveCharByHand(wk.i, wk.s, wk.t,
                                                       wk.c, wk.cs);
            if (rc) wireKeyOfChar[rc] = wk;
        }
    }

    // Attention gate anchors, resolved once for both passes and the audit. A
    // body that no anchor is within attentionRadius_ of is DORMANT: nobody is
    // standing near it and nobody is looking at it, so the host deliberately
    // leaves it out of the census - and census silence over a region the host
    // is not speaking for must not be read as "this body does not exist".
    // That misreading is the whole ghost mechanism: a legitimate local NPC in
    // a place only this client cares about, hidden because the other client
    // never mentioned it.
    float rawAnch[12];
    unsigned int nRawAnch = engine::interestAnchors(gw, rawAnch);
    float attnAnch[12];
    unsigned int nAttnAnch = attentionAnchors(gw, rawAnch, nRawAnch, attnAnch);
    unsigned int authSkip = 0;   // bodies left alone because we author them

    // Prune counters for hands the enumeration no longer sees (left interest),
    // preserving suppressed entries (a hidden body may drop out of the query but
    // must keep its counters so the restore dwell works when it returns).
    std::set<Key> seen;
    for (unsigned int i = 0; i < n; ++i) seen.insert(keyOf(states[i]));
    for (unsigned int i = 0; i < wn; ++i) seen.insert(keyOf(wStates[i]));
    for (std::map<Key, AuthCount>::iterator it = authCount_.begin(); it != authCount_.end(); ) {
        if (seen.find(it->first) == seen.end() &&
            suppressed_.find(it->first) == suppressed_.end()) authCount_.erase(it++);
        else ++it;
    }
    pruneAttention(seen);

    for (unsigned int i = 0; i < n; ++i) {
        // Proxy bodies answer to their streamed key's census entry, not their
        // local hand (which exists on no other client) - never judge them.
        // Reconcile their POSITION against that same census row, though.
        if (proxyChars.find(chars[i]) != proxyChars.end()) {
            reconcileProxy(chars[i], states[i], proxyKeyOf);
            continue;
        }
        Key k = keyOf(states[i]);
        // Presence authority: not every body here answers to us.
        if (authorHoldsBody(gw, localId, k, chars[i], states[i].x, states[i].z)) {
            authCount_[k].unstreamed = 0;
            ++authSkip;
            continue;
        }
        // Streamed = the hand is in this tick's fresh set, OR the body pointer is
        // one applyTargets drove this tick. The pointer check covers combat-
        // detached NPCs: detachFromTownAI re-containers the body, so its LOCAL
        // key differs from the streamed key and the hand lookup alone would
        // suppress an actively-driven combatant (crowd copies froze mid-brawl).
        bool streamed = (keep.find(k) != keep.end()) ||
                        (drivenChars_.find(chars[i]) != drivenChars_.end());
        // Pop-out fix (2026-07-11 field report): existence authority is the
        // CENSUS, drive authority is the STREAM. An NPC at the ~200 u stream-
        // bubble boundary flickers in/out of the host's fresh set (the two
        // clients disagree slightly on its position), and the old streamed-only
        // signal hid REAL host-present NPCs ('Saint'/'Kumo' measured churning
        // suppress->restore->suppress every few seconds). While the census is
        // fresh, a census-present NPC is never suppressed - its local AI copy
        // may drift, but it EXISTS; only census-absent ghosts get hidden. With
        // no fresh census (hatch off / host lagging) the legacy streamed-only
        // behavior stands.
        bool exists = streamed ||
                      (censusFresh && censusHasAny(k));
        std::map<Key, Character*>::iterator s = suppressed_.find(k);
        AuthCount& ac = authCount_[k];
        // Dormant and census-absent: neither client is speaking for this
        // region, so there is nothing to judge. Hold the debounce at zero
        // rather than letting it climb silently - when attention does arrive,
        // the body gets a full SUPPRESS_AFTER_FRAMES to be corroborated
        // instead of being hidden on the first frame someone looks at it.
        if (!exists && !observedAt(k, attnAnch, nAttnAnch,
                                   states[i].x, states[i].y, states[i].z)) {
            ac.unstreamed = 0;
            continue;
        }
        if (exists) { ac.unstreamed = 0; if (ac.streamed < 1000000u) ++ac.streamed; }
        else        { ac.streamed = 0;   if (ac.unstreamed < 1000000u) ++ac.unstreamed; }
        if (exists) {
            // Host owns it again: hand it back once the stream has DWELLED (a
            // boundary NPC that flickers into the set for a frame stays hidden).
            if (s != suppressed_.end() && ac.streamed >= RESTORE_AFTER_FRAMES) {
                engine::restoreNpc(gw, chars[i]);
                suppressed_.erase(s);
                s = suppressed_.end();
                ++authRestores_;
                lifeSet(k, LIFE_RESOLVED, "restore");
                { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                  char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[authority] restore NPC hand=%u,%u name='%s' (supp=%u churn=%lu/%lu)",
                    states[i].hIndex, states[i].hSerial, nm,
                    (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                  b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
            if (s == suppressed_.end() && !streamed) {
                // Driven bodies report their tier (and own their marker) from
                // applyTargets; a census-present LOCAL copy is the park-
                // fallback regime.
                lifeSet(k, LIFE_PARKED, "census-local");
                debugMark(chars[i], 2, lifeName(LIFE_PARKED));
                // world_parity 2026-07-17: the bubble used to be a park-free
                // zone outright (run 185524: sub-50 u seat divergence fought
                // the local seat AI every frame). But a census-present body
                // inside the JOIN's bubble that the HOST does not stream
                // (interest sets only partially overlap - the edge class the
                // manual sessions kept catching, Pao at a steady 451 u) had
                // NO reconciliation at all. parkDivergedCopy's 120 u
                // threshold already exempts the seat-schedule class, and the
                // divergence freeze quiets the AI that used to fight the
                // teleport - so the park now runs here with the exact same
                // gates as the wide pass.
                float drift = parkDivergedCopy(chars[i], states[i], k);
                if (censusFreezeAi_ && drift >= 0.0f)
                    censusFreezeDivergedAi(chars[i], k, drift);
            }
            // NOTE: census position parking below the 120 u threshold stays
            // OFF inside the stream bubble (npc_sync regression, run 185524):
            // a bar NPC whose two schedules seat it ~50 u apart is re-placed
            // by its own seat AI the same frame, so the park never sticks and the fight
            // wrecked tracking/march. Inside the bubble the stream owns
            // position truth; parking is a WIDE-pass render-range tool.
        } else {
            // Host neither streams nor lists it (census-absent ghost): after
            // the debounce, hide + freeze so the local AI can't run a divergent
            // copy on top of the host-driven world.
            if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                // Phase 2 hardening: only RECORD the suppression when the engine
                // call actually landed. A faulted hide used to be booked as done,
                // leaving the body visible forever with no evidence - the silent
                // version of the join-only-enemies field report. On failure the
                // unstreamed streak keeps climbing, so this retries every frame;
                // log the miss once at the threshold crossing.
                if (engine::suppressNpc(gw, chars[i])) {
                    suppressed_[k] = chars[i];
                    ++authSuppresses_;
                    lifeSet(k, LIFE_CULLED, "suppress");
                    debugMark(chars[i], 1, lifeName(LIFE_CULLED));
                    { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                      char b[192]; _snprintf(b, sizeof(b) - 1,
                        "[authority] suppress NPC hand=%u,%u name='%s' (streamed=%u local=%u supp=%u churn=%lu/%lu)",
                        states[i].hIndex, states[i].hSerial, nm, (unsigned)keep.size(), n,
                        (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (ac.unstreamed == SUPPRESS_AFTER_FRAMES) {
                    char b[96]; _snprintf(b, sizeof(b) - 1,
                        "[authority] suppress MISS hand=%u,%u (engine call failed; retrying)",
                        states[i].hIndex, states[i].hSerial);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    // Wide pass (protocol 36): an NPC beyond the stream bubble is never in
    // this tick's fresh set, so its authority signal is EXISTENCE (its hand in
    // the host's census) rather than streaming. NPCs the near pass already
    // judged are skipped by pointer (its streamed logic is authoritative
    // inside the bubble), as is anything applyTargets drove this tick. Same
    // hysteresis counters so a census-boundary NPC doesn't churn.
    if (censusFresh && wn > 0) {
        unsigned long nowR = nowMs(); // recently-driven grace reference
        std::set<Character*> nearSet;
        for (unsigned int i = 0; i < n; ++i) nearSet.insert(chars[i]);
        for (unsigned int i = 0; i < wn; ++i) {
            if (nearSet.find(wChars[i]) != nearSet.end()) continue;
            if (proxyChars.find(wChars[i]) != proxyChars.end()) {
                reconcileProxy(wChars[i], wStates[i], proxyKeyOf);
                continue;
            }
            // Phase 2 mid-band tier: a DRIVEN body used to skip this pass
            // entirely - correct for parking/suppression (the stream owns its
            // position, and hiding a driven body is self-defeating), but it
            // also skipped RESTORE: a suppressed NPC whose hand starts
            // arriving on the mid tier is driven every tick (drivenChars_)
            // yet stays hidden+frozen forever - walk orders no-op on a body
            // removed from the update list, the permanent-zombie shape of
            // the old boundary flicker. Let a driven body reach the restore
            // branch (same dwell), and skip only park/suppress for it.
            // "Driven" includes a grace window (drivenSeen_): a mid-tier
            // body's samples ride the round-robin, and a rotation hiccup
            // must not let the cull streak run while the body is between
            // samples (run 103044: 6 cull/restore cycles per hand).
            bool driven = drivenChars_.find(wChars[i]) != drivenChars_.end();
            if (!driven) {
                std::map<Character*, unsigned long>::iterator ds =
                    drivenSeen_.find(wChars[i]);
                driven = ds != drivenSeen_.end() && (nowR - ds->second) < 8000;
            }
            Key k = keyOf(wStates[i]);
            if (authorHoldsBody(gw, localId, k, wChars[i], wStates[i].x, wStates[i].z)) {
                authCount_[k].unstreamed = 0;
                ++authSkip;
                continue;
            }
            if (driven && suppressed_.find(k) == suppressed_.end()) continue;
            bool exists = censusHasAny(k) ||
                          keep.find(k) != keep.end() || driven;
            std::map<Key, Character*>::iterator s = suppressed_.find(k);
            AuthCount& ac = authCount_[k];
            // Dormancy, same as the near pass - and this is where it matters
            // most. The wide pass reaches out to censusRadius_ (2000 u), far
            // past anywhere either player is looking, so most of what it used
            // to cull sat in regions nobody was watching at all.
            if (!exists && !observedAt(k, attnAnch, nAttnAnch,
                                       wStates[i].x, wStates[i].y, wStates[i].z)) {
                ac.unstreamed = 0;
                continue;
            }
            if (exists) { ac.unstreamed = 0; if (ac.streamed < 1000000u) ++ac.streamed; }
            else        { ac.streamed = 0;   if (ac.unstreamed < 1000000u) ++ac.unstreamed; }
            if (exists) {
                if (s != suppressed_.end() && ac.streamed >= RESTORE_AFTER_FRAMES) {
                    engine::restoreNpc(gw, wChars[i]);
                    suppressed_.erase(s);
                    s = suppressed_.end();
                    ++authRestores_;
                    lifeSet(k, LIFE_RESOLVED, "restore-wide");
                    debugMark(wChars[i], 2, lifeName(LIFE_PARKED));
                    { char nm[48]; engine::charName(wChars[i], nm, sizeof(nm));
                      char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[census] restore NPC hand=%u,%u name='%s' (supp=%u culls=%lu)",
                        wStates[i].hIndex, wStates[i].hSerial, nm,
                        (unsigned)suppressed_.size(), censusCulls_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                }
                // v38 parking, wide-radius flavor (this is where the pack-
                // hidden class lives: census-present wilderness NPCs far
                // outside the stream bubble, each side simulating its own).
                // Driven bodies excluded: the mid/near stream owns them.
                if (s == suppressed_.end() && !driven) {
                    lifeSet(k, LIFE_PARKED, "census-wide");
                    float drift = parkDivergedCopy(wChars[i], wStates[i], k);
                    // Census-band AI freeze: quiesce a diverging body's local AI
                    // so it can't flee/aggro the join's guards (the mining-slave
                    // cascade). drift < 0 = no census row / parking off -> skip.
                    if (censusFreezeAi_ && drift >= 0.0f)
                        censusFreezeDivergedAi(wChars[i], k, drift);
                }
            } else if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                if (engine::suppressNpc(gw, wChars[i])) {
                    suppressed_[k] = wChars[i];
                    ++authSuppresses_;
                    ++censusCulls_;
                    lifeSet(k, LIFE_CULLED, "cull-wide");
                    debugMark(wChars[i], 1, lifeName(LIFE_CULLED));
                    { char nm[48]; engine::charName(wChars[i], nm, sizeof(nm));
                      char b[192]; _snprintf(b, sizeof(b) - 1,
                        "[census] cull NPC hand=%u,%u name='%s' pos=%.0f,%.0f,%.0f "
                        "(census=%u wide=%u supp=%u culls=%lu)",
                        wStates[i].hIndex, wStates[i].hSerial, nm,
                        wStates[i].x, wStates[i].y, wStates[i].z,
                        censusTotalCount(), wn,
                        (unsigned)suppressed_.size(), censusCulls_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (ac.unstreamed == SUPPRESS_AFTER_FRAMES) {
                    char b[96]; _snprintf(b, sizeof(b) - 1,
                        "[census] cull MISS hand=%u,%u (engine call failed; retrying)",
                        wStates[i].hIndex, wStates[i].hSerial);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    // Phase 2 hardening: RE-ASSERT the hide on a ~2 s cadence. suppressNpc is a
    // one-shot (remove-from-update + clearGoals + setVisible(false)), but the
    // engine can undo it on its own: an ambush squad's dialog/combat package
    // re-tasks the body and zone streaming can re-add it to the update list -
    // exactly the field report ("join-only enemies started dialog then attacked
    // and stayed visible"). The call is idempotent, so re-applying to a body the
    // engine never touched is a no-op; keys the host started streaming again are
    // skipped (the restore dwell owns those).
    //
    // Lifetime guard (2026-07-11 join crash): the engine owns every suppressed
    // body and can despawn it at any time - the 17:53 session held 93 hidden
    // wildlife bodies through a zone stream, and the dump shows the engine's
    // own sensory update walking a freed body. Before ANY touch, prove each
    // entry live with a hand round-trip: SEH-read the pointer's CURRENT hand
    // and resolve it back - the same pointer proves the body is alive (this
    // survives engine re-containering, which only changes the hand); anything
    // else means despawned, and the entry + marker + counters are dropped
    // without touching the pointer. A live body whose hand CHANGED (combat
    // detach re-containered it while hidden) migrates its entry to the new key
    // so the hide keeps re-asserting - the old key would never resolve again.
    unsigned long now = nowMs();
    if ((now - authReassertMs_) >= 2000) {
        authReassertMs_ = now;
        unsigned int pruned = 0;
        std::map<Key, Character*> migrated;
        for (std::map<Key, Character*>::iterator it = suppressed_.begin();
             it != suppressed_.end(); ) {
            unsigned int h[5];
            Character* live = 0;
            if (engine::readHand(it->second, h))
                live = engine::resolveCharByHand(h[0], h[1], h[2], h[3], h[4]);
            if (live != it->second) {
                std::map<Character*, DebugMarker>::iterator mi =
                    debugMarkers_.find(it->second);
                if (mi != debugMarkers_.end()) {
                    engine::markerDestroy(mi->second.label);
                    debugMarkers_.erase(mi);
                }
                authCount_.erase(it->first);
                ++pruned; ++authPruned_;
                suppressed_.erase(it++);
                continue;
            }
            Key ck; ck.i = h[0]; ck.s = h[1]; ck.t = h[2];
            ck.c = h[3]; ck.cs = h[4];
            if (ck < it->first || it->first < ck) {
                migrated[ck] = it->second;
                authCount_.erase(it->first);
                suppressed_.erase(it++);
                continue;
            }
            if (keep.find(it->first) != keep.end()) { ++it; continue; }
            // A combat-detached body is driven under a DIFFERENT streamed key
            // (pointer identity survives the re-containering); never re-hide a
            // body applyTargets drove this tick.
            if (drivenChars_.find(it->second) != drivenChars_.end()) { ++it; continue; }
            engine::suppressNpc(gw, it->second);
            ++it;
        }
        for (std::map<Key, Character*>::iterator it = migrated.begin();
             it != migrated.end(); ++it) {
            if (suppressed_.find(it->first) != suppressed_.end()) continue;
            suppressed_[it->first] = it->second;
            if (keep.find(it->first) == keep.end() &&
                drivenChars_.find(it->second) == drivenChars_.end())
                engine::suppressNpc(gw, it->second);
        }
        if (pruned > 0) {
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[authority] pruned %u stale suppressed entries (despawned; supp=%u total=%lu)",
                pruned, (unsigned)suppressed_.size(), authPruned_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Marker map hygiene on the same cadence: drop labels whose Character*
        // wasn't vouched live this pass (this tick's enumerations, driven and
        // proxy sets, plus the just-validated suppressed bodies). A pruned
        // body that comes back into judgment simply gets a fresh label.
        if (!debugMarkers_.empty()) {
            std::set<Character*> vouched;
            for (unsigned int i = 0; i < n; ++i)  vouched.insert(chars[i]);
            for (unsigned int i = 0; i < wn; ++i) vouched.insert(wChars[i]);
            vouched.insert(drivenChars_.begin(), drivenChars_.end());
            vouched.insert(proxyChars.begin(), proxyChars.end());
            for (std::map<Key, Character*>::iterator it = suppressed_.begin();
                 it != suppressed_.end(); ++it) vouched.insert(it->second);
            pruneDebugMarkers(vouched);
        }
    }

    // Existence-audit probe (pack-hidden investigation, 2026-07-11): a 5 s
    // census of what THIS client's world holds vs what the host vouches for,
    // classifying every enumerated NPC into exactly one bucket:
    //   drv   - streamed/driven this tick (host actively drives it)
    //   cen   - census-present, unstreamed (legit local-sim copy, host has it)
    //   hid   - booked suppressed (we hid it)
    //   dorm  - census-absent and unobserved (attention gate): no anchor is
    //           within attentionRadius_, so the host is not speaking for this
    //           region and we are not judging it. Deliberately NOT a ghost -
    //           these are the bodies the gate exists to leave alone.
    //   ghost - census-absent, observed, NOT suppressed (the visible-on-join-
    //           only class: either inside the suppress debounce, judged only
    //           while the census was stale, or escaping judgment entirely)
    // ghost is the bucket the field reports live in - it should only ever be
    // transient (one debounce, ~1 s). Test-ExistenceParity gates on it.
    // KENSHICOOP_DEBUG_CENSUS=1 additionally dumps a row per ghost.
    static unsigned long auditMs = 0; // main-thread only
    if ((now - auditMs) >= 5000) {
        auditMs = now;
        unsigned int cDrv = 0, cCen = 0, cHid = 0, cGhost = 0, cDorm = 0;
        unsigned int cMine = 0;   // bodies in cells WE author (presence authority)
        // Safety check on the attention gate. The design rests on a squad
        // never going dormant just because no camera is on it: every squad-tab
        // LEADER is an interest anchor, so bodies beside a leader are observed
        // by construction. What is NOT covered by construction is a squad
        // member that walked away from its leader - nothing anchors it.
        // dormPc counts dormant bodies within attentionRadius_ of ANY player
        // character, i.e. bodies standing next to somebody's squad that we
        // stopped judging anyway. It must read 0.
        const unsigned int MAX_PC = 32;
        static EntityState pcStates[MAX_PC];  // main-thread only
        unsigned int nPc = 0;
        if (attentionRadius_ > 0.0f)
            nPc = engine::captureSquad(gw, /*leaderOnly*/ false, pcStates, MAX_PC);
        unsigned int cDormPc = 0;
        unsigned int dormPcRows = 0;   // offender dumps this sample (see below)
        static int dumpGhost = -1;
        if (dumpGhost < 0) {
            const char* e = getenv("KENSHICOOP_DEBUG_CENSUS");
            dumpGhost = (e && e[0] == '1') ? 1 : 0;
        }
        std::set<Character*> counted;
        std::set<Key> emittedKeys; // world_parity: hands already dumped
        unsigned int ghostRows = 0;
        // Phase 0.5 horizon probe: how far out do ghosts actually sit? Anything
        // past censusRadius_ is never enumerated at all, so it cannot appear in
        // ANY bucket here - the ghost count silently stops at the horizon while
        // the player keeps seeing bodies past it. If ghostMax rides up against
        // the radius (and ghostEdge is non-zero), the cull horizon is the
        // binding constraint and KENSHICOOP_CENSUS_RADIUS is the lever; if
        // ghosts cluster close in, the cause is debounce/staleness/caps instead.
        // Distances are measured against the RAW anchors - how far out ghosts
        // sit, regardless of who can speak for the region - while the
        // classification below re-uses the vetoed set the passes judged with.
        const float* anch = rawAnch;
        unsigned int nAnch = nRawAnch;
        float ghostMaxD = -1.0f;
        unsigned int ghostEdge = 0;
        const float EDGE_BAND = censusRadius_ * 0.8f;
        for (int pass = 0; pass < 2; ++pass) {
            unsigned int cnt = (pass == 0) ? n : wn;
            Character**  cs  = (pass == 0) ? chars : wChars;
            EntityState* sts = (pass == 0) ? states : wStates;
            for (unsigned int i = 0; i < cnt; ++i) {
                if (!counted.insert(cs[i]).second) continue;
                Key k = keyOf(sts[i]);
                const char* cls;
                if (proxyChars.find(cs[i]) != proxyChars.end() ||
                    keep.find(k) != keep.end() ||
                    drivenChars_.find(cs[i]) != drivenChars_.end()) {
                    cls = "drv"; ++cDrv;
                } else if (suppressed_.find(k) != suppressed_.end()) {
                    cls = "hid"; ++cHid;
                } else if (cellAuth_ &&
                           authorityFor(gw, sts[i].x, sts[i].z) == localId) {
                    // Ours to author - it is not a ghost for lacking a census
                    // row, because nobody but us was ever going to write one.
                    cls = "mine"; ++cMine;
                } else if (censusFresh && censusHasAny(k)) {
                    cls = "cen"; ++cCen;
                } else if (!observedAt(k, attnAnch, nAttnAnch,
                                       sts[i].x, sts[i].y, sts[i].z)) {
                    cls = "dorm"; ++cDorm;
                    for (unsigned int p = 0; p < nPc; ++p) {
                        float dPc = dist3(sts[i].x, sts[i].y, sts[i].z,
                                          pcStates[p].x, pcStates[p].y,
                                          pcStates[p].z);
                        if (dPc > attentionRadius_) continue;
                        ++cDormPc;
                        // A count alone cannot be acted on, because the two ways
                        // to get here need opposite fixes. Either an anchor that
                        // WOULD have covered this body was vetoed (the body is
                        // within reach of a raw anchor but not a surviving one),
                        // or the player character it stands next to is not an
                        // anchor at all - a squad member strung out behind its
                        // tab leader, which is the gap the design notes call out
                        // as uncovered by construction. Measuring the body
                        // against both anchor sets says which, without needing a
                        // theory: raw <= attnR while attn > attnR is the veto,
                        // both > attnR is the straggler. run_apart 20260804
                        // reported dormPc=16 with no way to tell them apart.
                        if (dormPcRows < 3) {
                            ++dormPcRows;
                            float dAttn = -1.0f, dRaw = -1.0f;
                            for (unsigned int a = 0; a < nAttnAnch; ++a) {
                                float d = dist3(sts[i].x, sts[i].y, sts[i].z,
                                                attnAnch[a * 3 + 0],
                                                attnAnch[a * 3 + 1],
                                                attnAnch[a * 3 + 2]);
                                if (dAttn < 0.0f || d < dAttn) dAttn = d;
                            }
                            for (unsigned int a = 0; a < nRawAnch; ++a) {
                                float d = dist3(sts[i].x, sts[i].y, sts[i].z,
                                                rawAnch[a * 3 + 0],
                                                rawAnch[a * 3 + 1],
                                                rawAnch[a * 3 + 2]);
                                if (dRaw < 0.0f || d < dRaw) dRaw = d;
                            }
                            // Is the PC in question one of the anchors? A leader
                            // sits ON its anchor, so ~0 means this body's
                            // neighbour anchors attention and something else is
                            // wrong; a large value means it anchors nothing.
                            float dPcAnchor = -1.0f;
                            for (unsigned int a = 0; a < nRawAnch; ++a) {
                                float d = dist3(pcStates[p].x, pcStates[p].y,
                                                pcStates[p].z,
                                                rawAnch[a * 3 + 0],
                                                rawAnch[a * 3 + 1],
                                                rawAnch[a * 3 + 2]);
                                if (dPcAnchor < 0.0f || d < dPcAnchor) dPcAnchor = d;
                            }
                            // Three causes, told apart by the two distances and
                            // the anchor counts. Same count but a body inside
                            // toRaw and outside toAttn is the cache replaying
                            // stale POSITIONS (the run_apart finding); a smaller
                            // count with the body inside toRaw means the veto
                            // dropped an anchor that covered it; anything else is
                            // a player character that anchors nothing.
                            bool inRaw  = dRaw  >= 0.0f && dRaw  <= attentionRadius_;
                            bool inAttn = dAttn >= 0.0f && dAttn <= attentionRadius_;
                            const char* cause =
                                (inRaw && !inAttn && nAttnAnch == nRawAnch)
                                    ? "stale anchor positions"
                                : (inRaw && nAttnAnch < nRawAnch)
                                    ? "anchor vetoed"
                                    : "pc anchors nothing";
                            char b[288]; _snprintf(b, sizeof(b) - 1,
                                "[audit] dormPc hand=%u,%u toPc=%.0f toAttn=%.0f "
                                "toRaw=%.0f pcToAnchor=%.0f attnR=%.0f "
                                "anchors=%u/%u cause=%s",
                                sts[i].hIndex, sts[i].hSerial, dPc, dAttn, dRaw,
                                dPcAnchor, attentionRadius_, nAttnAnch, nRawAnch,
                                cause);
                            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                        }
                        break;
                    }
                } else {
                    cls = "ghost"; ++cGhost;
                    float nearest = -1.0f;
                    for (unsigned int a = 0; a < nAnch; ++a) {
                        float d = dist3(sts[i].x, sts[i].y, sts[i].z,
                                        anch[a * 3 + 0], anch[a * 3 + 1],
                                        anch[a * 3 + 2]);
                        if (nearest < 0.0f || d < nearest) nearest = d;
                    }
                    if (nearest > ghostMaxD) ghostMaxD = nearest;
                    if (EDGE_BAND > 0.0f && nearest >= EDGE_BAND) ++ghostEdge;
                    if (dumpGhost == 1 && ghostRows < 10) {
                        ++ghostRows;
                        char nm[48]; engine::charName(cs[i], nm, sizeof(nm));
                        char r[192]; _snprintf(r, sizeof(r) - 1,
                            "[audit] ghost hand=%u,%u name='%s' pos=%.0f,%.0f,%.0f "
                            "unstreak=%u",
                            sts[i].hIndex, sts[i].hSerial, nm,
                            sts[i].x, sts[i].y, sts[i].z, authCount_[k].unstreamed);
                        r[sizeof(r) - 1] = '\0'; coop::logLine(r);
                    }
                }
                // travel_parity worldstate rows (join side): one row per
                // enumerated NPC with its authority class, same schema as the
                // host's census dump so the oracle can cross-match by hand.
                //
                // 04-09 gap-closure #2 (census_convergence): if this body is
                // one wireKeyOfChar resolved above (a proxy, or any other
                // driven body whose local hand has drifted from its wire
                // hand), key the row on the WIRE hand instead of the local
                // keyOf(sts[i]) - see the wireKeyOfChar comment for the full
                // measured evidence. This is also what lets the
                // targets_/censusHands_ fallback pass below skip it via
                // emittedKeys instead of emitting the same body a second
                // time under its correct hand.
                if (auditRows_) {
                    Key emitKey = k;
                    const EntityState* emitSt = &sts[i];
                    EntityState wireSt;
                    std::map<Character*, Key>::iterator wkIt =
                        wireKeyOfChar.find(cs[i]);
                    if (wkIt != wireKeyOfChar.end()) {
                        emitKey = wkIt->second;
                        wireSt = sts[i];
                        wireSt.hIndex = emitKey.i; wireSt.hSerial = emitKey.s;
                        wireSt.hType = emitKey.t; wireSt.hContainer = emitKey.c;
                        wireSt.hContainerSerial = emitKey.cs;
                        emitSt = &wireSt;
                    }
                    if (emittedKeys.find(emitKey) == emittedKeys.end()) {
                        emittedKeys.insert(emitKey);
                        emitWnpcRow(cs[i], *emitSt, cls);
                    }
                }
            }
        }
        if (auditRows_) {
            // world_parity third pass: a driven body that is locally IN
            // FURNITURE (bed/cage/chained at the join's fixture) drops out of
            // the spatial character query, so the two passes above never list
            // it and the parity oracle scored it "missing" (guard escorting
            // the arrested PC: 16/25 samples absent while rendering fine).
            // The drive targets are keyed by hand; emit any that resolve but
            // were not enumerated. captureNpcByHand's resolve round-trip is
            // the liveness proof; player-squad members are covered by
            // emitPcRows below.
            for (std::map<Key, Driven>::iterator ti = targets_.begin();
                 ti != targets_.end(); ++ti) {
                const Key& tk = ti->first;
                if (emittedKeys.find(tk) != emittedKeys.end()) continue;
                EntityState ts;
                Character* tc = 0;
                // 04-09 gap-closure: a minted proxy's WIRE key (tk, what the
                // host streams) never equals its own LOCAL engine hand (the
                // [rekey] mapping recorded at mint time), so
                // captureNpcByHand/resolveCharByHand(tk) can never find it -
                // this fallback pass exists precisely for "driven but not
                // spatially enumerated" bodies, yet was blind to every proxy
                // for as long as the near/wide passes above also missed it
                // (measured: ~66s of false host-only census-divergence rows,
                // run 20260902_105146_N4). proxyByKey_ already holds the live
                // pointer; capture through it directly instead of re-deriving
                // a hand that was never going to resolve.
                std::map<Key, Character*>::iterator px = proxyByKey_.find(tk);
                if (px != proxyByKey_.end()) {
                    tc = px->second;
                    if (!tc || !engine::captureNpcByPointer(tc, &ts)) continue;
                } else {
                    if (!engine::captureNpcByHand(gw, tk.i, tk.s, tk.t, tk.c,
                                                  tk.cs, &ts)) continue;
                    tc = engine::resolveCharByHand(tk.i, tk.s, tk.t,
                                                   tk.c, tk.cs);
                    if (!tc) continue;
                }
                // Row keyed by the STREAMED hand (what the host dumps): a
                // combat-detached body's local handle differs, and captureOne
                // read that local one - overwrite so the oracle can pair it.
                ts.hIndex = tk.i; ts.hSerial = tk.s; ts.hType = tk.t;
                ts.hContainer = tk.c; ts.hContainerSerial = tk.cs;
                emittedKeys.insert(tk);
                if (counted.insert(tc).second) ++cDrv;
                emitWnpcRow(tc, ts, "drv");
            }
            // ...and the same for every census-vouched hand (a host-side
            // barracks of SLEEPING guards is contained in beds on the join
            // too - resolving fine, rendering fine, invisible to the spatial
            // query, and not driven so the targets_ pass can't list them
            // either; a re-containered ex-slave answers to a DIFFERENT local
            // hand, so its enumerated row can't pair with the host's - emit
            // it under the census hand too). The resolve round-trip IS the
            // existence answer the parity oracle wants; a hand that fails it
            // here is genuinely absent from this client.
            // Task 3 (WORLD-03): iterate every OWNER's CensusSet rather than
            // one bare set - each owner's vouched hands get the same
            // fallback-emit treatment, deduplicated against emittedKeys
            // across owners (a hand vouched by two owners transiently during
            // a handover is emitted once).
            for (std::map<u32, CensusSet>::iterator oi = census_.begin();
                 oi != census_.end(); ++oi) {
                for (std::set<Key>::iterator ci = oi->second.hands.begin();
                     ci != oi->second.hands.end(); ++ci) {
                    if (emittedKeys.find(*ci) != emittedKeys.end()) continue;
                    EntityState ts;
                    Character* tc = 0;
                    // Same proxy/wire-hand mismatch as the targets_ pass above -
                    // a census-vouched proxy hand can never resolveCharByHand to
                    // its own local object either.
                    std::map<Key, Character*>::iterator px = proxyByKey_.find(*ci);
                    if (px != proxyByKey_.end()) {
                        tc = px->second;
                        if (!tc || !engine::captureNpcByPointer(tc, &ts)) continue;
                    } else {
                        if (!engine::captureNpcByHand(gw, ci->i, ci->s, ci->t, ci->c,
                                                      ci->cs, &ts)) continue;
                        tc = engine::resolveCharByHand(ci->i, ci->s, ci->t,
                                                       ci->c, ci->cs);
                        if (!tc) continue;
                    }
                    ts.hIndex = ci->i; ts.hSerial = ci->s; ts.hType = ci->t;
                    ts.hContainer = ci->c; ts.hContainerSerial = ci->cs;
                    emittedKeys.insert(*ci);
                    bool sup = suppressed_.find(*ci) != suppressed_.end();
                    if (counted.insert(tc).second) { if (sup) ++cHid; else ++cCen; }
                    emitWnpcRow(tc, ts, sup ? "hid" : "cen");
                }
            }
            // world_parity: PC rows on the join too (the peer-driven copies) -
            // the host/join cls=pc pairs are what the PC position gate judges.
            emitPcRows(gw);
            // Phase 0.5 fields are APPENDED: Get-WorldRows' regex stops at
            // ghost= and tolerates any trailing text (fresh= already rides
            // past it), so the oracle contract is unchanged.
            char w[256]; _snprintf(w, sizeof(w) - 1,
                "SCENARIO WORLD n=%u cls=%s drv=%u cen=%u hid=%u ghost=%u fresh=%d "
                "cap=%d ghostMax=%.0f ghostEdge=%u dorm=%u mine=%u",
                (unsigned)counted.size(), roleTag, cDrv, cCen, cHid, cGhost,
                censusFresh ? 1 : 0, (nearTrunc || wideTrunc) ? 1 : 0,
                ghostMaxD, ghostEdge, cDorm, cMine);
            w[sizeof(w) - 1] = '\0'; coop::logLine(w);
        }
        // Phase 0.5 fields appended after parks= (existing prefix untouched):
        //   nearCap/wideCap - the enumeration hit its buffer and judged an
        //                     INCOMPLETE world; absence stopped meaning absent
        //   staleMs/edges   - how long wide culling has been disabled, and how
        //                     often it dropped out (each edge leaks ghosts)
        //   ghostMax/Edge   - how far ghosts reach, and how many sit in the
        //                     outer 20% band next to the un-enumerable horizon
        //   dorm/attnR      - bodies the attention gate left alone, and the
        //                     radius it used (0 = gate off, so dorm is always
        //                     0 and every bucket reads as it did before)
        //   dormPc/pcs      - gate safety violation count: dormant bodies
        //                     within attnR of a player character (must be 0),
        //                     over the pcs squad members it was measured
        //                     against - pcs=0 makes the zero vacuous
        char b[448]; _snprintf(b, sizeof(b) - 1,
            "[audit] exist near=%u wide=%u drv=%u cen=%u hid=%u ghost=%u "
            "supp=%u census=%u fresh=%d parks=%lu walks=%lu "
            "nearCap=%d wideCap=%d staleMs=%lu edges=%lu ghostMax=%.0f ghostEdge=%u "
            "dorm=%u attnR=%.0f dormPc=%u pcs=%u mine=%u skip=%u cells=%u",
            n, wn, cDrv, cCen, cHid, cGhost,
            (unsigned)suppressed_.size(), censusTotalCount(),
            censusFresh ? 1 : 0, censusParks_, censusWalks_,
            nearTrunc ? 1 : 0, wideTrunc ? 1 : 0,
            censusStaleMs_, censusStaleEdges_, ghostMaxD, ghostEdge,
            cDorm, attentionRadius_, cDormPc, nPc,
            cMine, authSkip, (unsigned)claimedCells_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Phase 3 lifecycle upkeep: prune left-interest records + self-audit
    // mintable hands stuck in DISCOVERED (the invisible-raid failure).
    lifeSweep(gw, now);
    measureAttach(now);
}

// Phase C capability veto: drop any anchor whose zone this engine has not
// loaded, so we never claim existence-authority over ground we cannot see.
//
// The first two raw anchors are the squad-tab LEADERS (interestCenters fills
// them before the camera anchors and only ever drops camera ones), which is the
// safety property the gate rests on - an unwatched squad still anchors its own
// surroundings. The veto can in principle remove a leader too, but only when
// its zone is unloaded here, and an unloaded zone holds no enumerable bodies to
// go dormant. Measured: hostZoneUnloaded=0 at 5,200 u separation, and dormPc=0
// (dormant bodies next to a player character) across every gated run.
//
// THROTTLED, and not for CPU cost. `isZoneLoadedAt` reaches into ZoneManager,
// whose members are boost shared_mutex-protected and written by the engine's own
// zone-loader thread. Before this veto the query ran ~1 Hz from the census
// diagnostics; the authority pass runs every tick, so calling it here put
// hundreds of acquisitions per second of main-thread traffic onto a lock the
// loader thread holds while streaming. Nothing measured proves that stalls the
// game loop - the stall signature in the logs predates this code by six weeks -
// but the trade is one-sided: the answer only changes as zones stream, on a
// multi-second scale, so a cached verdict costs nothing and a permanently
// wedged main thread costs the session. Re-query on the interval, or at once if
// an anchor jumped (a camera cut or teleport can cross a zone edge instantly).
//
// The cache holds the VERDICT ONLY. It used to hold the surviving anchors'
// coordinates too, which quietly turned a zone-loading throttle into a 500 ms
// delay on where the players are - harmless while everything measured stood
// still, and 285 u of error once run_apart ran at 570 u/s. attnVetoMask_ carries
// the full account.
unsigned int Replicator::attentionAnchors(GameWorld* gw, const float* raw,
                                          unsigned int nRaw, float* out) {
    if (!raw || !out) return 0;
    const unsigned long ATTN_VETO_MS  = 500;
    const float         ATTN_VETO_JUMP = 100.0f;
    unsigned long nowV = nowMs();
    bool reuse = (attnVetoMs_ != 0) && ((nowV - attnVetoMs_) < ATTN_VETO_MS)
                 && (attnVetoRawN_ == nRaw);
    for (unsigned int a = 0; reuse && a < nRaw; ++a)
        if (dist3(raw[a * 3 + 0], raw[a * 3 + 1], raw[a * 3 + 2],
                  attnVetoRaw_[a * 3 + 0], attnVetoRaw_[a * 3 + 1],
                  attnVetoRaw_[a * 3 + 2]) > ATTN_VETO_JUMP) reuse = false;
    // Re-query the zones, or reuse the verdict - but ALWAYS emit the anchors'
    // current positions. Replaying cached positions is what made a body beside
    // its own squad dormant (see attnVetoMask_).
    unsigned int keepMask;
    if (reuse) {
        keepMask = attnVetoMask_;
    } else {
        keepMask = 0;
        for (unsigned int a = 0; a < nRaw && a < 8; ++a) {
            const float* p = raw + a * 3;
            if (attentionRadius_ > 0.0f &&
                !engine::isZoneLoadedAt(gw, p[0], p[1], p[2])) continue;
            keepMask |= (1u << a);
        }
        attnVetoMs_   = nowV;
        attnVetoRawN_ = nRaw;
        attnVetoMask_ = keepMask;
        for (unsigned int i = 0; i < nRaw * 3 && i < 12; ++i) attnVetoRaw_[i] = raw[i];
    }
    unsigned int nOut = 0;
    unsigned int vetoMask = 0;   // which raw indices were dropped
    for (unsigned int a = 0; a < nRaw && nOut < 4; ++a) {
        if (a < 8 && !(keepMask & (1u << a))) { vetoMask |= (1u << a); continue; }
        const float* p = raw + a * 3;
        out[nOut * 3 + 0] = p[0];
        out[nOut * 3 + 1] = p[1];
        out[nOut * 3 + 2] = p[2];
        ++nOut;
    }
    if (nOut < nRaw && !reuse) {
        static unsigned long vetoLogMs = 0; // main-thread only
        if (vetoLogMs == 0 || (nowV - vetoLogMs) >= 10000) {
            vetoLogMs = nowV;
            // WHICH anchor, and where. interestCenters fills tab leaders first
            // and cameras after, so index 0-1 are leaders in this 2-player scope
            // - and the difference matters completely. Dropping the PEER's tab
            // leader is the veto working: on run_apart the join holds a driven
            // copy of the host's leader 138,000 u away, in ground it has never
            // loaded and cannot speak for. Dropping OUR OWN leader would mean
            // the gate stopped reconciling a player's own surroundings, which is
            // the failure dormPc exists to catch. The old line counted vetoes
            // without saying which, so the two were indistinguishable.
            char idx[72]; unsigned int w = 0;
            for (unsigned int a = 0; a < nRaw && a < 8 && w + 16 < sizeof(idx); ++a) {
                if (!(vetoMask & (1u << a))) continue;
                int k = _snprintf(idx + w, sizeof(idx) - w - 1, "%s%u%s",
                                  w ? "," : "", a, a < 2 ? "(leader)" : "(cam)");
                if (k <= 0) break;
                w += (unsigned int)k;
            }
            idx[w] = '\0';
            char b[192]; _snprintf(b, sizeof(b) - 1,
                "[attn] anchor veto %u/%u (zone not loaded here) dropped=%s",
                nRaw - nOut, nRaw, idx[0] ? idx : "?");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    return nOut;
}

// rebuildClaimedCells + claimsCoLocated (the per-instance std::map-
// iteration-order reduce + co-location collapse heuristic) are RETIRED as of
// Task 2 (protocol 59, WORLD-03) - see CellMap.h's reduceCellMap
// (host-side, continuity -> host-if-party -> lowest playerId) and
// computeAndBroadcastCellMap/applyCellMap for the replacement. Deleted
// rather than left dead: "do NOT leave both paths live" (PLAN.md
// constraint) - a per-instance reduce nobody calls anymore is still a
// second, divergence-prone algorithm sitting next to the real one.

u32 Replicator::authorityFor(GameWorld* gw, float x, float z) const {
    int cx = 0, cz = 0, src = 0;
    return authoritySrc(gw, x, z, &cx, &cz, &src);
}

u32 Replicator::authoritySrc(GameWorld* gw, float x, float z,
                             int* cx, int* cz, int* src) const {
    *cx = 0; *cz = 0; *src = AUTHSRC_NOMAP;
    if (!cellAuth_ || !engine::cellAt(gw, x, z, cx, cz)) {
        return (u32)CELL_OWNER_HOST;
    }
    // Task 2 (WORLD-03): the old independent co-location COLLAPSE short-
    // circuit (AUTHSRC_COLLAPSE) is retired - the per-cell reduce SUPERSEDES
    // it structurally rather than needing a second, separately-evaluated
    // verdict that could disagree with the map. A co-located cell (host +
    // join both hold a slot there) is a FRESH CONTEST the reduce's
    // host-if-party rule already resolves to host (CellMap.h), and that
    // verdict folds into cellLastOwner_ the same tick - so a cell the pair
    // then WALKS AWAY FROM (drops out of claimedCells_ entirely) still
    // reverts to host via the ordinary AUTHSRC_VACATE fallback below,
    // exactly the "428 freezes" case the old collapse hack existed to fix,
    // now closed by construction instead of by a parallel boolean.
    // collapsed_ is retained ONLY as a derived diagnostic (see
    // computeAndBroadcastCellMap/applyCellMap) for the [audit]/[census]
    // dump lines - it never again feeds an authority decision here.
    std::pair<int, int> cell(*cx, *cz);
    std::map<std::pair<int, int>, u32>::const_iterator it = claimedCells_.find(cell);
    if (it != claimedCells_.end()) { *src = AUTHSRC_CLAIM; return it->second; }
    // Nobody is standing here now; the last client that was still speaks for it.
    std::map<std::pair<int, int>, u32>::const_iterator lo = cellLastOwner_.find(cell);
    if (lo != cellLastOwner_.end()) { *src = AUTHSRC_VACATE; return lo->second; }
    *src = AUTHSRC_OPEN;
    return (u32)CELL_OWNER_HOST;
}

bool Replicator::authorHoldsBody(GameWorld* gw, u32 localId, const Key& k,
                                 Character* c, float x, float z) {
    if (!cellAuth_) return false;
    u32 owner = authorityFor(gw, x, z);
    // Ours to author, or nobody's we have heard from: either way this body is
    // not ours to judge. The second case matters as much as the first - a cell
    // whose owner has sent no census is unspoken-for, not empty, and treating
    // silence as absence is the whole ghost mechanism.
    //
    // Task 3 (WORLD-03): the old single-sender censusOwner_ ("the last
    // census we received happens to be from this exact owner") generalizes
    // to "the census OF THAT OWNER has spoken" - census_.find(owner) - now
    // that the intake is per-owner rather than one bare set a second
    // author's arrival used to overwrite. Deliberately does NOT check
    // freshness here (the pre-Task-3 code never did either): a stale-but-
    // present entry still names an owner we have heard from at least once
    // this session, which is the predicate this line has always asked.
    if (owner != localId && census_.find(owner) != census_.end()) return false;
    // INCUMBENT HOLDS. A body the peer's stream is already writing is the peer's,
    // whatever this cell verdict says, because the verdict is not a shared fact:
    // each side evaluates it against ITS OWN copy's position, and the two copies
    // of a fighting NPC drift apart. Measured 2026-08-03 on a 'Hungry bandit'
    // whose copies sat 163 u apart (gap=163.4, task agreement 58%) astride the
    // z=0 boundary between cells 21,31 and 21,32: the host's copy was in the
    // host's cell and the join's copy in the join's, so BOTH sides concluded the
    // body was theirs - each correctly, by its own premise.
    //
    // This used to release the drive and author from the next tick, to avoid two
    // writers on one Character. But releasing is what created the fight: the peer
    // has no idea we did it and keeps streaming, so we release again, 531 times
    // for that one bandit, and each release dropped the body out of the streamed
    // set long enough for the ordinary pass to suppress it (151 suppress/restore
    // pairs, which is what turned up as suppress_churn).
    //
    // Yielding instead is what converges. A stream is an assertion of authorship,
    // so the receiver defers to it and handover happens only when the author
    // STOPS streaming - a decision one side makes alone, needing no agreement
    // about whose cell a drifting body is standing in.
    bool peerDrives = false;
    if (c) peerDrives = drivenChars_.find(c) != drivenChars_.end() ||
                        drivenSeen_.find(c)  != drivenSeen_.end();
    // Say so once per body rather than per tick: the old line was 30% of this
    // scenario's log and said the same thing every frame.
    if (peerDrives && cellYield_.insert(k).second) {
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[cell] yield hand=%u,%u owner=%u - the peer's stream holds this body; "
            "not authoring it while they drive it",
            (unsigned)k.i, (unsigned)k.s, owner);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    if (!peerDrives) cellYield_.erase(k);
    // Either way the body is not the ordinary pass's business, so it must be
    // VISIBLE: authority moving to us, or a drive starting over something we hid
    // on the old author's word, both have to undo that suppression or a handover
    // leaves a town permanently invisible.
    std::map<Key, Character*>::iterator sp = suppressed_.find(k);
    if (sp != suppressed_.end()) {
        engine::restoreNpc(gw, c ? c : sp->second);
        suppressed_.erase(sp);
        ++authRestores_;
        lifeSet(k, LIFE_RESOLVED, "author-restore");
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[cell] restore NPC hand=%u,%u owner=%u (%s; supp=%u)",
            (unsigned)k.i, (unsigned)k.s, owner,
            peerDrives ? "the peer drives this body" : "we author here",
            (unsigned)suppressed_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    return true;
}

void Replicator::syncCellClaims(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId) {
    // Drain unconditionally: with the feature off the queue must still not grow
    // (a peer running with it on would otherwise back up against us forever).
    std::deque<InboundCellClaim> got;
    in.drainCellClaims(got);
    if (!cellAuth_ || !gw) {
        if (!claimSlots_.empty()) {
            claimSlots_.clear(); claimedCells_.clear(); cellLastOwner_.clear();
        }
        collapsed_ = false;   // no claims, nothing to collapse onto
        return;
    }
    unsigned long now = nowMs();
    bool changed = false;
    for (std::deque<InboundCellClaim>::const_iterator it = got.begin();
         it != got.end(); ++it) {
        const CellClaimPacket& p = it->pkt;
        if (p.ownerId == ownerId) continue;   // our own broadcast, echoed back
        std::pair<u32, u32> slot(p.ownerId, p.tabRank);
        std::map<std::pair<u32, u32>, CellClaim>::iterator s = claimSlots_.find(slot);
        // Signed difference so a wrapped seq still orders correctly.
        if (s != claimSlots_.end() && (int)(p.seq - s->second.seq) <= 0) continue;
        bool moved = (s == claimSlots_.end()) ||
                     s->second.cx != p.cellX || s->second.cz != p.cellY;
        CellClaim cc;
        cc.cx = p.cellX; cc.cz = p.cellY; cc.seq = p.seq; cc.recvMs = now;
        claimSlots_[slot] = cc;
        if (moved) {
            changed = true;
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[cell] RECV owner=%u rank=%u cell=%d,%d seq=%u",
                p.ownerId, p.tabRank, p.cellX, p.cellY, p.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Publish our own tabs' cells at 1 Hz.
    if (claimSendMs_ == 0 || (now - claimSendMs_) >= 1000) {
        claimSendMs_ = now;
        bool assertDue = (claimAssertMs_ == 0) ||
                         (now - claimAssertMs_) >= (unsigned long)CELL_ASSERT_MS;
        if (assertDue) claimAssertMs_ = now;
        const unsigned int MAX_SQ = 96;
        static EntityState raw[MAX_SQ];   // main-thread only
        unsigned int nSquad = engine::captureSquad(gw, /*leaderOnly*/ false, raw, MAX_SQ);
        std::vector<std::pair<u32, u32> > ctnrs;
        ctnrs.reserve(nSquad);
        for (unsigned int i = 0; i < nSquad; ++i)
            ctnrs.push_back(std::make_pair(raw[i].hContainer, raw[i].hContainerSerial));
        std::sort(ctnrs.begin(), ctnrs.end());
        ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
        for (unsigned int ci = 0; ci < ctnrs.size(); ++ci) {
            unsigned int rank = tabRankFor(ctnrs[ci], ctnrs);
            if (!ownsTab(ctnrs[ci], rank)) continue;
            // The tab's leader stands for the tab: presence is about where the
            // squad IS, and a scattered squad still has one home cell.
            const EntityState* lead = 0;
            for (unsigned int i = 0; i < nSquad && !lead; ++i)
                if (raw[i].hContainer == ctnrs[ci].first &&
                    raw[i].hContainerSerial == ctnrs[ci].second) lead = &raw[i];
            if (!lead) continue;
            int cx = 0, cz = 0;
            if (!engine::cellAt(gw, lead->x, lead->z, &cx, &cz)) continue;
            std::pair<u32, u32> slot(ownerId, (u32)rank);
            std::map<std::pair<u32, u32>, CellClaim>::iterator cur = claimSlots_.find(slot);
            bool same = (cur != claimSlots_.end()) &&
                        cur->second.cx == cx && cur->second.cz == cz;
            // Dwell: a boundary walker must settle before authority moves.
            CellDwell& d = claimDwell_[(u32)rank];
            if (d.n == 0 || d.cx != cx || d.cz != cz) { d.cx = cx; d.cz = cz; d.n = 1; }
            else if (d.n < 0xFFFFu) ++d.n;
            if (same) { if (!assertDue) continue; }
            else if (d.n < (unsigned int)CELL_DWELL_N) continue;
            u32 seq = ++claimSeqOut_[(u32)rank];
            CellClaimPacket p;
            memset(&p, 0, sizeof(p));
            p.type    = (u8)PKT_CELL_CLAIM;
            p.ownerId = ownerId;
            p.tabRank = (u32)rank;
            p.seq     = seq;
            p.cellX   = cx;
            p.cellY   = cz;
            net.queueCellClaim(p);
            CellClaim cc;
            cc.cx = cx; cc.cz = cz; cc.seq = seq; cc.recvMs = now;
            claimSlots_[slot] = cc;
            if (!same) {
                changed = true;
                char b[144]; _snprintf(b, sizeof(b) - 1,
                    "[cell] CLAIM rank=%u cell=%d,%d seq=%u dwell=%u pos=%.0f,%.0f",
                    rank, cx, cz, seq, d.n, lead->x, lead->z);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    // Protocol 59 (WORLD-03): rebuildClaimedCells (the per-instance
    // std::map-iteration-order reduce) is SUPERSEDED - the host alone
    // reduces + broadcasts via computeAndBroadcastCellMap (Plugin.cpp wires
    // it right after this call, before authorityFor is consulted this
    // tick); a join adopts ONLY the host map via applyCellMap. `changed` is
    // no longer consumed here (it drove rebuildClaimedCells's own dump
    // cadence, now computeAndBroadcastCellMap's).
    (void)changed;
}

// HOST ONLY (protocol 59, WORLD-03) - see the declaration doc comment in
// Replicator.h for the full contract.
void Replicator::computeAndBroadcastCellMap(GameWorld* gw, NetLink& net, u32 localId) {
    (void)gw; (void)localId;
    if (!cellAuth_ || !isHostRole()) return;
    unsigned long now = nowMs();

    // Every currently-connected owner, host included (the host is always
    // "connected" to itself - knownPeers_ tracks JOINED peers only).
    std::set<u32> connectedOwners;
    connectedOwners.insert((u32)CELL_OWNER_HOST);
    for (std::set<u32>::const_iterator it = knownPeers_.begin();
         it != knownPeers_.end(); ++it)
        connectedOwners.insert(*it);

    // The reduce's own input shape (CellMap.h is engine/Wire-free, so
    // claimSlots_'s CellClaim -> CellSlotView copy happens here).
    CellSlotMap slotsView;
    for (std::map<std::pair<u32, u32>, CellClaim>::const_iterator it = claimSlots_.begin();
         it != claimSlots_.end(); ++it) {
        CellSlotView v; v.cx = it->second.cx; v.cz = it->second.cz;
        slotsView[it->first] = v;
    }

    // A COPY, never claimedCells_ itself - reduceCellMap clears its out-map
    // first, so previousMap and outMap must not alias.
    CellOwnerMap previousMap = claimedCells_;
    CellOwnerMap newMap;
    reduceCellMap(slotsView, previousMap, connectedOwners, (u32)CELL_OWNER_HOST, newMap);

    // Phase 8 review WR-04: past CELL_MAP_MAX the broadcast MUST truncate
    // (the wire array is fixed) - so the host adopts EXACTLY the same
    // truncated map it is about to broadcast, never the full reduce. The
    // kept prefix is deterministic (lowest cell keys first - std::map
    // order); the dropped cells fail open to host on every instance alike,
    // instead of splitting into "host honors the real owner, clients fail
    // open" - the silent authority split the one-verdict invariant forbids.
    unsigned int cellsDropped = truncateCellMap(newMap, CELL_MAP_MAX);
    if (cellsDropped > 0) {
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[cell] CELL-MAP TRUNCATED cells=%u exceeds CELL_MAP_MAX=%u; "
            "dropped=%u (host adopts the SAME truncated map it broadcasts)",
            (unsigned)(newMap.size() + cellsDropped), CELL_MAP_MAX, cellsDropped);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    bool changed = (newMap != claimedCells_);
    claimedCells_.swap(newMap);

    // Fold the resolved verdict into cellLastOwner_ (the vacate-fallback
    // history AUTHSRC_VACATE reads) exactly like the old rebuild did for
    // every CLAIMED cell. A cell that dropped OUT of claimedCells_ this pass
    // (vacated, or a departed owner's cell already purged by
    // clearPeerReplicationState) keeps whatever cellLastOwner_ entry it had
    // - reverting a departed owner's cell to host is the disconnect
    // handler's job (it erases the owner's cellLastOwner_ entries too), not
    // this fold's.
    for (CellOwnerMap::const_iterator it = claimedCells_.begin();
         it != claimedCells_.end(); ++it) {
        cellLastOwner_[it->first] = it->second;
    }

    // Task 2 (WORLD-03): collapsed_ is now DERIVED from the reduced map -
    // every claimed cell resolved to host - never a separate
    // claimsCoLocated() evaluation that could disagree with it.
    bool allHost = !claimedCells_.empty();
    for (CellOwnerMap::const_iterator it = claimedCells_.begin();
         it != claimedCells_.end() && allHost; ++it) {
        if (it->second != (u32)CELL_OWNER_HOST) allHost = false;
    }
    collapsed_ = allHost;

    bool assertDue = (claimMapMs_ == 0) ||
                     (now - claimMapMs_) >= (unsigned long)CELL_ASSERT_MS;
    if (!changed && !assertDue) return;
    claimMapMs_ = now;

    CellMapPacket p;
    memset(&p, 0, sizeof(p));
    p.type = (u8)PKT_CELL_MAP;
    p.seq  = ++cellMapSeqOut_;
    unsigned int n = 0;
    for (CellOwnerMap::const_iterator it = claimedCells_.begin();
         it != claimedCells_.end(); ++it) {
        if (n >= CELL_MAP_MAX) {
            // Defensive only since WR-04: claimedCells_ was truncated to
            // CELL_MAP_MAX above, so this branch is unreachable - if it ever
            // fires, host adoption and broadcast have diverged again.
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[cell] MAP-OVERFLOW cells=%u exceeds CELL_MAP_MAX=%u; truncated",
                (unsigned)claimedCells_.size(), CELL_MAP_MAX);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            break;
        }
        p.entries[n].cellX   = it->first.first;
        p.entries[n].cellY   = it->first.second;
        p.entries[n].ownerId = it->second;
        ++n;
    }
    p.count = (u16)n;
    net.queueCellMap(p);

    logCellMapDump("local");
}

// ALL clients (host included) - see the declaration doc comment in
// Replicator.h for the full contract.
void Replicator::applyCellMap(GameWorld* gw, Inbound& in, u32 localId) {
    (void)gw; (void)localId;
    std::deque<InboundCellMap> got;
    in.drainCellMaps(got);
    if (got.empty()) return;
    // Latest-wins: a client's ENet connection has exactly one peer (the
    // host), so this is a genuinely single-sender channel - no per-owner
    // fold is needed (unlike census, which can legitimately queue distinct
    // owners in one drain).
    const CellMapPacket& p = got.back().pkt;

    claimedCells_.clear();
    unsigned int n = (unsigned int)p.count;
    if (n > CELL_MAP_MAX) n = CELL_MAP_MAX; // bounds guard: a corrupt/oversize count never over-reads entries[]
    for (unsigned int i = 0; i < n; ++i) {
        std::pair<int, int> cell(p.entries[i].cellX, p.entries[i].cellY);
        claimedCells_[cell] = p.entries[i].ownerId;
    }
    for (CellOwnerMap::const_iterator it = claimedCells_.begin();
         it != claimedCells_.end(); ++it) {
        cellLastOwner_[it->first] = it->second;
    }

    bool allHost = !claimedCells_.empty();
    for (CellOwnerMap::const_iterator it = claimedCells_.begin();
         it != claimedCells_.end() && allHost; ++it) {
        if (it->second != (u32)CELL_OWNER_HOST) allHost = false;
    }
    collapsed_ = allHost;

    logCellMapDump("host");
}

// Shared [cell] MAP log-dump helper - see the declaration doc comment in
// Replicator.h.
void Replicator::logCellMapDump(const char* src) const {
    char b[288];
    // Appended AFTER slots=/collapse= so the oracle's map parser is
    // unaffected - Get-CellMap takes the tail with (.*)$ and scans it for
    // 'x,z=owner' triples, which neither 'collapse=1' nor 'src=host' can
    // match.
    int off = _snprintf(b, sizeof(b) - 1, "[cell] MAP cells=%u slots=%u collapse=%u",
                        (unsigned)claimedCells_.size(),
                        (unsigned)claimSlots_.size(),
                        collapsed_ ? 1u : 0u);
    if (off < 0) off = 0;
    for (std::map<std::pair<int, int>, u32>::const_iterator mi = claimedCells_.begin();
         mi != claimedCells_.end() && off < (int)sizeof(b) - 32; ++mi) {
        int w = _snprintf(b + off, sizeof(b) - 1 - off, " %d,%d=%u",
                          mi->first.first, mi->first.second, mi->second);
        if (w < 0) break;
        off += w;
    }
    if (off < (int)sizeof(b) - 16) {
        int w2 = _snprintf(b + off, sizeof(b) - 1 - off, " src=%s", src);
        if (w2 > 0) off += w2;
    }
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

unsigned int Replicator::peerAnchors(GameWorld* gw, float* out) {
    if (!gw || !out) return 0;
    unsigned int nOut = 0;
    const unsigned int MAX_SQ = 96;
    static EntityState raw[MAX_SQ];   // main-thread only
    unsigned int nSquad = engine::captureSquad(gw, /*leaderOnly*/ false, raw, MAX_SQ);
    std::vector<std::pair<u32, u32> > ctnrs;
    ctnrs.reserve(nSquad);
    for (unsigned int i = 0; i < nSquad; ++i)
        ctnrs.push_back(std::make_pair(raw[i].hContainer, raw[i].hContainerSerial));
    std::sort(ctnrs.begin(), ctnrs.end());
    ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
    // EVERY tab leader, ours included - this has to model what the PEER's
    // attentionAnchors will compute, and that anchors on all tab leaders
    // because a body standing next to any player character must never be
    // dormant (the dormPc safety property). Restricting this to the peer's own
    // tabs was a wrong model of the peer's attention, and the two predicates
    // disagreeing is exactly the failure this channel exists to prevent: on
    // split_far2 the host stopped censusing its own town because the join's
    // TAB was 5200 u away, while the join went on attending it through its
    // local copy of the host's leader - and suppressed 26 real bodies against
    // the resulting silence.
    for (unsigned int ci = 0; ci < ctnrs.size() && nOut < 3; ++ci) {
        for (unsigned int i = 0; i < nSquad; ++i) {
            if (raw[i].hContainer != ctnrs[ci].first ||
                raw[i].hContainerSerial != ctnrs[ci].second) continue;
            out[nOut * 3 + 0] = raw[i].x;
            out[nOut * 3 + 1] = raw[i].y;
            out[nOut * 3 + 2] = raw[i].z;
            ++nOut;
            break;
        }
    }
    float pc[3];
    if (nOut < 4 && engine::peerCamAnchor(pc)) {
        out[nOut * 3 + 0] = pc[0];
        out[nOut * 3 + 1] = pc[1];
        out[nOut * 3 + 2] = pc[2];
        ++nOut;
    }
    return nOut;
}

bool Replicator::observedAt(const Key& k, const float* anchors,
                            unsigned int nAnchor, float x, float y, float z) {
    return observedIn(attnObs_, /*countFlips*/ true, k, anchors, nAnchor, x, y, z);
}

bool Replicator::observedByPeer(const Key& k, const float* anchors,
                                unsigned int nAnchor, float x, float y, float z) {
    return observedIn(attnObsPeer_, /*countFlips*/ false, k, anchors, nAnchor, x, y, z);
}

bool Replicator::observedIn(std::map<Key, bool>& obs, bool countFlips, const Key& k,
                            const float* anchors, unsigned int nAnchor,
                            float x, float y, float z) {
    if (attentionRadius_ <= 0.0f) return true;   // gate off
    // No anchors at all means no players in gameplay (interestCenters returns
    // 0 before the squads exist). Fail OPEN - a startup tick must not declare
    // the whole world dormant.
    if (!anchors || nAnchor == 0) return true;
    // Leaving costs 25% more reach than entering. Both clients run this over
    // anchor sets that agree only approximately, so a body parked on the
    // threshold would flip out of phase between them without the deadband.
    const float ATTN_LEAVE_SCALE = 1.25f;
    std::map<Key, bool>::iterator it = obs.find(k);
    bool was = (it != obs.end()) && it->second;
    float r = was ? attentionRadius_ * ATTN_LEAVE_SCALE : attentionRadius_;
    bool now = false;
    for (unsigned int a = 0; a < nAnchor && !now; ++a) {
        float d = dist3(x, y, z, anchors[a * 3 + 0], anchors[a * 3 + 1],
                        anchors[a * 3 + 2]);
        now = (d <= r);
    }
    if (countFlips && now && !was) ++attnFlips_;   // Phase B: an attach to pay for
    if (it != obs.end()) it->second = now;
    else                 obs[k] = now;
    return now;
}

void Replicator::measureAttach(unsigned long now) {
    // Phase B, measurement only: what does an attach actually cost? Open a
    // window on the first one and report the suppressions, culls and proxy
    // mints that landed inside it. A window with nothing to show for it stays
    // silent - bodies drift across the threshold constantly as squads walk,
    // and one line per quiet crossing would bury the bursts this exists to
    // find.
    const unsigned long ATTN_WIN_MS = 3000;
    if (attnFlips_ > 0 && attnWinMs_ == 0) {
        attnWinMs_     = now;
        attnBaseSupp_  = authSuppresses_;
        attnBaseCull_  = censusCulls_;
        attnBaseProxy_ = (unsigned int)proxyByKey_.size();
    }
    if (attnWinMs_ == 0 || (now - attnWinMs_) < ATTN_WIN_MS) return;
    unsigned long dSupp = authSuppresses_ - attnBaseSupp_;
    unsigned long dCull = censusCulls_ - attnBaseCull_;
    int dProxy = (int)proxyByKey_.size() - (int)attnBaseProxy_;
    if (attnFlips_ >= 4 || dSupp || dCull || dProxy) {
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[attn] attach bodies=%lu hid=+%lu culled=+%lu minted=%+d winMs=%lu",
            attnFlips_, dSupp, dCull, dProxy, (unsigned long)ATTN_WIN_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    attnFlips_ = 0;
    attnWinMs_ = 0;
}

void Replicator::pruneAttention(const std::set<Key>& seen) {
    for (std::map<Key, bool>::iterator it = attnObs_.begin();
         it != attnObs_.end(); ) {
        if (seen.find(it->first) == seen.end()) attnObs_.erase(it++);
        else ++it;
    }
    for (std::map<Key, bool>::iterator it = attnObsPeer_.begin();
         it != attnObsPeer_.end(); ) {
        if (seen.find(it->first) == seen.end()) attnObsPeer_.erase(it++);
        else ++it;
    }
}

void Replicator::reconcileProxy(Character* c, const EntityState& st,
                                const std::map<Character*, Key>& keyOf) {
    std::map<Character*, Key>::const_iterator pk = keyOf.find(c);
    if (pk == keyOf.end()) return;
    // st carries the LOCAL enumeration's hand; the census row answers to the
    // stream key, so the drift is measured and the park keyed by that.
    float drift = parkDivergedCopy(c, st, pk->second);
    if (censusFreezeAi_ && drift >= 0.0f)
        censusFreezeDivergedAi(c, pk->second, drift);
}

float Replicator::parkDivergedCopy(Character* c, const EntityState& st, const Key& k) {
    // v38 pack-hidden fix: existence culling exempts census-present NPCs, but
    // both clients then run INDEPENDENT sims of the same body - the join's
    // copy can stand somewhere the host's copy isn't (the "pack hidden" save:
    // a pack visible on the join with no host counterpart at that spot).
    // The census now carries the host position per row; reconcile a local
    // copy that drifted past the park distance with a halt+teleport onto the
    // host's spot. 120 u default: ABOVE town-schedule divergence (two sims
    // seating the same bar NPC ~50 u apart - run 185524), so only genuinely
    // divergent wanderers (measured 500-900 u) trip it.
    if (censusParkDist_ <= 0.0f) return -1.0f;
    // Task 3 (WORLD-03): find WHICH owner's CensusSet carries `k`, so the
    // step-speed calc further down reads that SAME owner's prev/prevMs
    // rather than accidentally pairing one owner's current row with a
    // DIFFERENT owner's previous one.
    u32 rowOwner = 0; bool haveRow = false;
    CensusPos curPos;
    unsigned long nowStale = nowMs();
    for (std::map<u32, CensusSet>::const_iterator oi = census_.begin();
         oi != census_.end(); ++oi) {
        // Phase 8 review WR-02: a stale owner's slice is skipped here too -
        // parking/walking a local copy toward a silent owner's minutes-old
        // position is exactly the "correct toward stale coordinates" failure
        // the per-owner freshness gate exists to stop.
        if (oi->second.recvMs == 0 ||
            (nowStale - oi->second.recvMs) > (unsigned long)CENSUS_OWNER_STALE_MS)
            continue;
        std::map<Key, CensusPos>::const_iterator pit = oi->second.pos.find(k);
        if (pit != oi->second.pos.end()) {
            rowOwner = oi->first; curPos = pit->second; haveRow = true;
            break;
        }
    }
    if (!haveRow) return -1.0f;
    // HORIZONTAL divergence only (2026-08-06). This was dist3, and height is the
    // one axis a park cannot correct: park writes the transform, then the engine
    // settles the body onto whatever is underfoot LOCALLY, so a copy the two
    // clients disagree about vertically lands straight back where it started.
    // Walking into Bad Teeth measured one - a Holy Priest at an IDENTICAL x/z,
    // 129 u higher on the join than on the host (582 against 453: two engines
    // disagreeing about a floor) - parked once a second for the whole run at an
    // unchanging d=129, and took 84 of that run's 221 teleports on its own.
    // Ground position is ours to reconcile; height belongs to the collision that
    // owns it, and measuring an axis we cannot move only manufactures work.
    float ddx = st.x - curPos.x, ddz = st.z - curPos.z;
    float d  = std::sqrt(ddx * ddx + ddz * ddz);
    float dv = std::fabs(st.y - curPos.y);
    unsigned long nowP = nowMs();
    // One threshold to start correcting, a closer one to stop. A single line
    // would make the walk band below stutter for exactly the reason it exists:
    // the body walks until it is a hair inside the line, we stop ordering, the
    // freeze halts it, and a counterpart that is still walking re-opens the gap
    // within the second - so the body would spend the town walking half a second
    // and standing still half a second. Converging PAST the line and only
    // re-arming at it lets one order cover several census rows.
    std::map<Key, CensusFix>::iterator cf = censusFix_.find(k);
    bool walkingNow = cf != censusFix_.end() && cf->second.walkMs != 0 &&
                      (nowP - cf->second.walkMs) < 1500;
    if (d <= (walkingNow ? censusParkDist_ * 0.5f : censusParkDist_)) {
        // Converged. Drop the walk destination so the next real divergence
        // issues a fresh one, and clear the futile streak - a body that closed
        // the gap is not stuck.
        if (cf != censusFix_.end()) { cf->second.haveDest = false;
                                      cf->second.fails = 0;
                                      cf->second.walkMs = 0; }
        return d;
    }
    // WALK THE GAP instead of jumping it (2026-08-06). Teleporting is the only
    // correction this path had, and for anything that WALKS it is the wrong one.
    // A patrol's two copies cannot help diverging: the host's walks its route,
    // the join's is AI-frozen the moment it crosses this threshold and so cannot
    // follow, and the census position keeps advancing - so the gap re-opens as
    // fast as it is closed and the only tool available is another teleport. Bad
    // Teeth measured the result: 137 of one run's 221 teleports were patrol
    // templates (Holy Sentinel, Servant, Paladin), median hop 154 u, one body
    // jumping every two seconds for the length of the run. Nothing was broken;
    // the correction itself was the artefact the player sees.
    //
    // A body that is merely BEHIND its counterpart should catch up the way it
    // would in the world, on its feet. walkTo routes through the engine's own
    // locomotion, so the body paths around what is in the way, grounds itself,
    // and plays a real walk clip - and it costs nothing extra, since the census
    // position it is already being corrected onto is the destination.
    //
    // The teleport stays for the class it was written for. The pack-hidden case
    // is a copy that is not behind its counterpart but somewhere else entirely
    // (measured 500-900 u), where there is nothing to converge to on foot: it
    // would spend a minute walking a route its counterpart never took, through
    // whatever lies between. Past the band ceiling, jump.
    if (censusWalkDist_ > 0.0f && d <= censusWalkDist_) {
        CensusFix& f = censusFix_[k];
        // What the counterpart is doing, from the step between the last two
        // census rows - the only thing the join can know about an unstreamed
        // body's motion, since the census carries position and nothing else.
        float stepX = 0.0f, stepZ = 0.0f, hostSpd = 0.0f;
        const CensusSet& rowSet = census_[rowOwner];
        std::map<Key, CensusPos>::const_iterator pv = rowSet.prev.find(k);
        if (pv != rowSet.prev.end() && rowSet.prevMs &&
            rowSet.recvMs > rowSet.prevMs) {
            stepX = curPos.x - pv->second.x;
            stepZ = curPos.z - pv->second.z;
            float dt = (float)(rowSet.recvMs - rowSet.prevMs) / 1000.0f;
            // Converted OUT of world units per second and INTO the engine's
            // speed scale: the step is real displacement and so already carries
            // the game-speed multiplier, while walkTo commands a speed the
            // multiplier is then applied to. Miss that and a 5x session orders
            // five times the intended sprint.
            float mult = (speedLastSet_ > 1.0f) ? speedLastSet_ : 1.0f;
            hostSpd = std::sqrt(stepX * stepX + stepZ * stepZ) / dt / mult;
        }
        // Walk to where the counterpart is GOING, not where it was. A census row
        // is already up to a second old when it arrives, so a body sent to the
        // reported spot arrives at a place its counterpart has left and stops
        // there to wait for the next row. Leading by the last step - the
        // distance it covered over exactly one row - keeps the destination
        // roughly one row ahead, which is where it will be when the order runs
        // out. A counterpart that stopped has a zero step and so is never led,
        // which is what keeps the lead from overshooting a body at rest.
        float tx = curPos.x + stepX, tz = curPos.z + stepZ;
        // Re-issue only when the destination actually moved (the locomotion-
        // drive lesson: a per-frame walkTo restarts the path and renders as
        // stutter). Census rows arrive at ~1 Hz, so in practice this is one
        // order per row for a moving target and one in total for a still one.
        float moved = f.haveDest
            ? std::sqrt((tx - f.dx) * (tx - f.dx) + (tz - f.dz) * (tz - f.dz))
            : (REISSUE_DIST + 1.0f);
        if (moved > REISSUE_DIST) {
            // Speed has to EXCEED the pace of what it is chasing or the gap
            // never closes - the body would trail at a fixed distance forever,
            // permanently over the threshold.
            float base = (hostSpd > 1.0f) ? hostSpd : 12.0f;
            float spd  = base + d;          // wider gap, harder catch-up
            float cap  = base * 2.5f;       // ...but never a teleport on legs
            if (spd > cap) spd = cap;
            engine::walkTo(c, tx, curPos.y, tz, spd);
            f.haveDest = true; f.dx = tx; f.dz = tz;
            ++censusWalks_;
            static unsigned long walkLogTick = 0; // main-thread only, ~1 line/s
            if ((nowP - walkLogTick) >= 1000) {
                walkLogTick = nowP;
                char nm[48]; engine::charName(c, nm, sizeof(nm));
                char b[224]; _snprintf(b, sizeof(b) - 1,
                    "[census] walk hand=%u,%u name='%s' d=%.0f hostSpd=%.0f "
                    "lead=%.0f spd=%.0f band=%.0f (walks=%lu parks=%lu)",
                    k.i, k.s, nm, d, hostSpd,
                    std::sqrt(stepX * stepX + stepZ * stepZ), spd,
                    censusWalkDist_, censusWalks_, censusParks_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // Refreshed every tick, not just on re-issue: this is what tells the
        // divergence freeze below that the body is under a walk order of ours
        // and its per-tick haltMovement would cancel the very correction in
        // progress.
        f.walkMs = nowP;
        f.fails  = 0;
        return d;
    }
    // TELEPORT. A body that reaches this point failed the walk band above, so
    // it is not behind its counterpart, it is somewhere else.
    //
    // Per-key cooldown (npc_sync regression, run 185524): the engine's own
    // schedule AI can re-place the body the same frame (a seated copy), so an
    // unthrottled park re-teleported every frame and wrecked tracking. One
    // park per key per cooldown bounds the fight; a free wanderer sticks on
    // the first try.
    // A FROZEN body (AI suspended for repeat divergence) reparks on a 1 s
    // cooldown: run 014948 showed a frozen slave still re-pathed to ~600 u
    // between 5 s parks (an in-flight movement goal survives the suspend),
    // so the clamp must out-pace the walk. Unfrozen bodies keep the 5 s
    // cooldown that protects seat-schedule NPCs from park thrash.
    unsigned long cool = censusFrozen_.count(k) ? 1000 : 5000;
    // Futile-park backoff. A park that lands and a park that silently does
    // nothing look identical from the call site, so the only evidence is the
    // next tick's position: a body still standing where we last teleported it
    // was not moved BY us either. Repeating a correction that demonstrably does
    // not take is pure cost, and it is not hypothetical - the vertical case
    // above spun at 1 Hz for an entire run, and the chained Nutto (parks=543 at
    // a constant d=381) needed its own anchor-break fix to escape the same loop.
    // This is the general form of both: keep retrying, because a zone reload or
    // an unchaining can make it work later, but a minute apart instead of a
    // second. Deliberately NOT a give-up - a body that is genuinely diverged
    // still deserves the occasional attempt.
    const float         PARK_MOVED_EPS   = 5.0f;
    const unsigned int  PARK_FUTILE_MAX  = 3;
    const unsigned long PARK_FUTILE_COOL = 60000;
    std::map<Key, CensusFix>::iterator fs = censusFix_.find(k);
    bool unmoved = fs != censusFix_.end() && fs->second.fails > 0 &&
                   std::fabs(st.x - fs->second.px) < PARK_MOVED_EPS &&
                   std::fabs(st.z - fs->second.pz) < PARK_MOVED_EPS;
    if (unmoved && fs->second.fails >= PARK_FUTILE_MAX) cool = PARK_FUTILE_COOL;
    std::map<Key, unsigned long>::iterator pm = parkMs_.find(k);
    if (pm != parkMs_.end() && (nowP - pm->second) < cool) return d;
    parkMs_[k] = nowP;
    {
        CensusFix& f = censusFix_[k];
        // A first park has nothing to compare against, so the streak starts at
        // 1 and the entry records where we are about to move the body from.
        bool same = f.fails > 0 &&
                    std::fabs(st.x - f.px) < PARK_MOVED_EPS &&
                    std::fabs(st.z - f.pz) < PARK_MOVED_EPS;
        f.fails = same ? f.fails + 1 : 1;
        if (same && f.fails == PARK_FUTILE_MAX) {
            char nm[48]; engine::charName(c, nm, sizeof(nm));
            char b[208]; _snprintf(b, sizeof(b) - 1,
                "[census] park FUTILE hand=%u,%u name='%s' d=%.0f dv=%.0f "
                "(unmoved across %u parks; retrying every %lus)",
                k.i, k.s, nm, d, dv, PARK_FUTILE_MAX,
                PARK_FUTILE_COOL / 1000);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        f.px = st.x; f.pz = st.z;
        f.haveDest = false; // a teleport invalidates any walk destination
    }
    // Anchor break (world_parity 2026-07-17): a census copy chained/caged at
    // the WRONG fixture (cross-client furniture identity is unreliable) is
    // position-anchored - every park teleport snapped straight back (Nutto:
    // parks=543, local pos constant at d=381 all run). Release the local
    // furniture first, then park. A chained body is NOT re-chained here:
    // kind-3 re-entry (setChainedMode with the LOCAL slaveOwner) snapped the
    // body straight back to that owner's spot ~500 u away (run 012555:
    // Lungrot re-anchored kind=3 at d~450-520 on EVERY 5 s park, forever).
    // The divergence freeze below keeps the parked body inert instead; the
    // lock-state stream still owns the shackle item itself.
    engine::FurnitureRead lfr;
    bool anchored = engine::readFurniture(c, &lfr) && lfr.valid && lfr.kind != 0;
    if (anchored) {
        engine::applyFurniture(0, c, lfr.furn, lfr.kind, false);
        engine::endAction(c);
        char nm[48]; engine::charName(c, nm, sizeof(nm));
        char b[144]; _snprintf(b, sizeof(b) - 1,
            "[census] park ANCHOR-BREAK hand=%u,%u name='%s' kind=%d d=%.0f",
            k.i, k.s, nm, lfr.kind, d);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    if (engine::park(c, curPos.x, curPos.y, curPos.z, st.heading)) {
        ++censusParks_;
        static unsigned long logTick = 0; // main-thread only, ~4 lines/s
        unsigned long now = nowMs();
        if ((now - logTick) >= 250) {
            logTick = now;
            char nm[48]; engine::charName(c, nm, sizeof(nm));
            char b[208]; _snprintf(b, sizeof(b) - 1,
                "[census] park hand=%u,%u name='%s' d=%.0f dv=%.0f "
                "local=%.0f,%.0f,%.0f host=%.0f,%.0f,%.0f (parks=%lu)",
                k.i, k.s, nm, d, dv, st.x, st.y, st.z,
                curPos.x, curPos.y, curPos.z, censusParks_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    return d;
}

// Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI, join only): the position
// park above teleports a diverged census-band body back to the host's spot, but
// its LOCAL AI kept re-deciding to flee/fight (a captive/working slave with no
// supervisor on the join), so it ran off and aggroed the join's guards while the
// host had it working. Suspend its AI while it is diverging. Divergence-gated so
// well-tracking census NPCs (bar-seaters ~50 u apart) keep their local AI: only
// a body that crossed censusParkDist_ is frozen, HELD ~5 s past the last over-
// threshold tick (so the park zeroing its drift can't oscillate it back into
// fleeing), then re-checked (released if it settled, re-frozen if it diverges
// again). Runs AFTER applyTargets' per-tick clearAiSuspend(), so the suspend
// added here stands for the tick; addAiSuspend is a no-op if the AI-suspend
// detour is not installed (KENSHICOOP_AI_SUSPEND=0).
void Replicator::censusFreezeDivergedAi(Character* c, const Key& k, float drift) {
    if (!c) return;
    // 20 s hold (was 5 s): a diverged working slave released after only 5 s
    // below-threshold walked back toward its local job spot / owner and was
    // over the 120 u park line again before the next 5 s park cooldown fired
    // (run 012555: Lungrot oscillated 300-500 u every park, all run). The
    // longer hold keeps a repeat offender inert across several park cycles;
    // a genuinely settled body still releases and never re-arms.
    const unsigned long HOLD_MS = 20000;
    unsigned long now = nowMs();
    bool over = (drift > censusParkDist_); // censusParkDist_ > 0 implied (drift >= 0)
    std::map<Key, unsigned long>::iterator it = censusFrozen_.find(k);
    bool wasFrozen = (it != censusFrozen_.end());
    if (over) {
        censusFrozen_[k] = now;                 // (re)arm / refresh the hold
        if (!wasFrozen) engine::endAction(c);   // drop the in-progress flee/attack once
    } else if (wasFrozen) {
        if ((now - it->second) >= HOLD_MS) {    // settled: release, resume local AI
            censusFrozen_.erase(it);
            return;
        }
    } else {
        return;                                 // never diverged: leave local AI alone
    }
    engine::addAiSuspend(c);                     // quiesce AI decisions this tick
    // A destination committed BEFORE the suspend keeps the body running (run
    // 014948: frozen slave re-pathed ~600 u between parks); kill it per tick.
    //
    // UNLESS the destination is OURS. That same survival is what makes the
    // walk-converge band work at all - the suspend stops the body deciding
    // where to go while our order carries it where the host's copy is - and
    // halting it every tick would cancel the correction in progress and leave
    // the teleport as the only way home, which is the loop the band exists to
    // break. The AI stays suspended either way, so what walks is only ever us.
    std::map<Key, CensusFix>::iterator wf = censusFix_.find(k);
    bool walking = wf != censusFix_.end() && wf->second.walkMs != 0 &&
                   (now - wf->second.walkMs) < 1500;
    if (!walking) engine::haltMovement(c);
    static unsigned long logTick = 0;           // main-thread only, ~4 lines/s
    if ((now - logTick) >= 250) {
        logTick = now;
        char nm[48]; engine::charName(c, nm, sizeof(nm));
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[census] FREEZE hand=%u,%u name='%s' d=%.0f (frozen=%u)",
            k.i, k.s, nm, drift, (unsigned)censusFrozen_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::pruneDebugMarkers(const std::set<Character*>& live) {
    for (std::map<Character*, DebugMarker>::iterator it = debugMarkers_.begin();
         it != debugMarkers_.end(); ) {
        // A green DRV label is only ever written while a body is being driven,
        // and nothing re-captions it when the drive stops: the body stays
        // enumerated, so it stays 'live' here, and the label outlives the fact
        // it asserts. That turns the HUD into a record of everything this client
        // has EVER driven, which reads on screen as both clients driving the
        // same body - the thing the tag exists to let you rule out. Drop it as
        // soon as the body leaves the driven set; a resumed drive re-creates it
        // on the next tick.
        bool staleDrive = (it->second.color == 0) &&
                          (drivenChars_.find(it->first) == drivenChars_.end());
        if (staleDrive || live.find(it->first) == live.end()) {
            engine::markerDestroy(it->second.label);
            debugMarkers_.erase(it++);
        } else ++it;
    }
}


} // namespace coop
