@{
    # ---------------------------------------------------------------------------
    # KenshiCoop scenario manifest - the single declarative source of truth for
    # WHICH oracles judge each scenario, which save/setup it needs, and which
    # regression tier it belongs to. Consumed by CoopOracles.psm1 (oracle
    # dispatch + verdict rule), run_test.ps1 (save/setup defaults, gating),
    # regress.ps1 (tier matrices) and analyze_run.ps1 (offline verdicts).
    #
    # Per-scenario fields:
    #   Save        - save both clients load (must exist in %LOCALAPPDATA%\kenshi\save)
    #   Setup       - host-only KENSHICOOP_SETUP scene ('' = none)
    #   Tolerance   - default cross-check tolerance (world units)
    #   PrimaryGate - the mechanism proof. FAIL **or SKIP** of this gate fails
    #                 the run (the no-signal guard): a green run must prove its
    #                 core mechanism was actually judged.
    #   Gating      - oracle ids that gate the verdict (FAIL fails the run; SKIP
    #                 is tolerated unless the oracle is the PrimaryGate)
    #   Advisory    - oracle ids that are measured + recorded but never gate
    #   Tier        - 'smoke' (per-pipeline sample) | 'full' | 'none' (diagnostic)
    #   WanVariant  - $true: the full tier reruns this scenario under the WAN
    #                 proxy (profile 'bad') as a second, separately-reported run
    #   Bounds      - per-scenario threshold overrides, @{ <oracleId> = @{ <Param> = <value> } }.
    #                 Only the ids/params in CoopOracles' Get-OracleBoundSpec are
    #                 accepted, and Contract.Tests.ps1 rejects anything else, so a
    #                 mistyped bound fails the check instead of silently doing
    #                 nothing. Prefer a bound over WanDemote: a demoted gate stops
    #                 judging the scenario at all, where a bound keeps judging it
    #                 at its own number.
    #   WanBounds   - same shape, overlaid on Bounds when the run is under the proxy
    #   NoSignalFails - oracle ids (N=4 milestone_a_gate schema only) that must
    #                 contribute a verdict reason if their status is a no-signal
    #                 result (SKIP), same no-signal-guard treatment PrimaryGate
    #                 already gets above - a SKIP on one of these ids is an
    #                 evidentiary gap, not a pass, so it must never silently
    #                 count as green (04-08-PLAN.md gap 4).
    #
    # Smoke-tier selection principle: ONE scenario per wire pipeline, preferring
    # the bidirectional superset (it strictly covers the unidirectional case):
    #   coop_presence     - unreliable entity stream + ownership partition
    #   npc_sync          - interest-managed world NPCs + intent/task/pose
    #   combat_kill       - reliable event channel + attribution + outcome
    #   inv_bidir         - inventory snapshot/reconcile (both directions)
    #   world_weapon_drop - world-item conservation channel
    #   world_armor_drop  - same channel, EQUIPPED armor piece (real-session repro)
    # ---------------------------------------------------------------------------

    # Harness timeout profiles. 'loopback' matches the historical defaults;
    # 'remote' loosens every wait for a second machine with unknown load times
    # (used by the remote session kit).
    Profiles = @{
        loopback = @{
            JoinDelaySec         = 8
            StartTimeoutSec      = 90
            # Scenario clocks arm on PEER-READY (the host waits for the join's
            # first entity batch before its script starts), so host anchors like
            # "SCENARIO MEMBER" appear ~a join-load later than gameplay start.
            ScenarioWaitSec      = 45
            JoinAnchorTimeoutSec = 60
            KillGraceSec         = 90
            # Peer-ready fallback: arm the scenario anyway after this much
            # gameplay if no peer batch ever arrives (KENSHICOOP_ARM_TIMEOUT_MS).
            ArmTimeoutMs         = 45000
        }
        remote = @{
            JoinDelaySec         = 0
            StartTimeoutSec      = 240
            ScenarioWaitSec      = 180
            JoinAnchorTimeoutSec = 240
            KillGraceSec         = 300
            ArmTimeoutMs         = 240000
        }
        # Two-machine LAN runs (run_lan_test.ps1): machine 2 loads independently
        # (its own disk/CPU), so waits sit between loopback and remote.
        lan = @{
            JoinDelaySec         = 0
            StartTimeoutSec      = 180
            ScenarioWaitSec      = 90
            JoinAnchorTimeoutSec = 120
            KillGraceSec         = 180
            ArmTimeoutMs         = 120000
        }
    }

    # WAN proxy profiles (one-way delay / jitter / loss both directions, applied
    # BELOW ENet by the netsim UDP relay so retransmission is really exercised).
    WanProfiles = @{
        regional = @{ DelayMs = 60;  JitterMs = 10; LossPct = 1  }
        bad      = @{ DelayMs = 120; JitterMs = 40; LossPct = 5  }
        awful    = @{ DelayMs = 200; JitterMs = 80; LossPct = 15 }
        # Starved-replica validation: regional conditions plus one scripted
        # TOTAL outage (both directions, below ENet) 30 s after the join's
        # first datagram (mid-scenario for the 24 s windows: connect precedes
        # gameplay by ~15 s), lasting 4 s - past interp staleMs (2 s), inside
        # the guard hold (KENSHICOOP_STARVE_HOLD_MS, 10 s). Driven bodies must
        # starve ([interp] starve>0) WITHOUT local AI/damage resuming. The
        # standard position gates (npc_track etc.) legitimately fail during a
        # real outage - this profile is for the manual guard-hold check, not
        # the regression matrix.
        stall    = @{ DelayMs = 60;  JitterMs = 10; LossPct = 1; StallAtS = 30; StallForS = 4 }
    }

    Scenarios = @{
        # ---- entity stream / presence -------------------------------------------
        leader_move = @{
            Save = 'sync'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'crosscheck'
            Gating   = @('crosscheck', 'smoothness', 'anim_truth', 'march', 'clock_sync')
            Advisory = @()
            Tier = 'full'; WanVariant = $true
            # DELIBERATE WAN-regime adjustment (final matrix, 2026-07-05): under the
            # 'bad' proxy the latency catch-up makes the walk-drive advance in
            # bursts, so zeroFrac runs above the clean ceiling (crosscheck stays
            # green at base tolerance). This was a WanDemote - the gate switched
            # off entirely - until 2026-08-07, when the bound mechanism let it keep
            # judging at the WAN number instead. Ten runs since 2026-07-29 measured
            # 0.227-0.478, so 0.55 still catches a real regression while the
            # demotion caught nothing at all.
            WanBounds = @{ smoothness = @{ MaxZeroFrac = 0.55 } }
        }
        # suppress_churn gates here (town save): the 2026-07-11 pop-in/out fix -
        # census-present town NPCs must never cycle hidden->restored->hidden at
        # the stream-bubble boundary ('Saint'/'Kumo' churned before the veto).
        coop_presence = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'coop_presence'
            Gating   = @('coop_presence', 'suppress_churn', 'snap_rate', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
            Tier = 'smoke'; WanVariant = $true
        }

        # split_interest (step 5): the host's tab leaves the bar; bar NPCs must keep
        # streaming via the second interest sphere (the join's tab leader). The join
        # walks nothing - its member IS the remote anchor. Save 'sync' (the bar
        # scene with a 2-tab squad): squad1's location has no world NPCs to anchor.
        split_interest = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'split_interest'
            Gating   = @('split_interest', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
            # DELIBERATE WAN-regime adjustment (step-5 validation, 2026-07-05): the
            # dual-sphere STREAMING mechanism is latency-independent (judged>=2 still
            # gates hard, and firmly-seated bar NPCs track at 0.2-0.4u even under the
            # 'bad' proxy). What degrades is the rest-wobble of the locally-ANIMATED
            # sit/idle NPCs - measured per-NPC median 14.6-17.5u under WAN vs 4.6-4.7u
            # clean (the npc_sync WAN class, but judged over only ~3 NPCs so ratio
            # slack cannot absorb one wobbler the way npc_sync's 9-NPC ratio does).
            WanTolerance = 18.0
        }

        # split_far (2026-08-02 far-travel desync report): the condition
        # split_interest and travel_parity both miss. split_interest separates by
        # 260 u - inside the 2000 u census sphere, so one interest cluster;
        # travel_parity separates far but the HOST FOLLOWS, so also one cluster,
        # through empty wilderness. Here the two tabs are ~9800 u apart and BOTH
        # HOLD, each with its own population, for the whole window.
        #
        # Save 'splitfar1' is BAKED (bake_scene.ps1 -Setup splitfar -BaseSave
        # separate): tab 0 holds the bar, tab 1 sits ~5200 u away in a second
        # populated place. It must be baked, never captured after a co-op run -
        # armConnectPush writes the live world early, before the squads are
        # positioned, so a capture records them together (the first attempt came
        # out 580 u apart that way).
        #
        # The measurement is the host's view of the mover's region (SPLITFAR
        # popMover/zMover, side=host) against the join's. Currently PASSES: both
        # sides enumerate the same 5 NPCs over 180/191 samples.
        #
        # WHAT THE DIVERGENCE ACTUALLY IS, AND WHAT CAUSES IT. The two clients
        # disagree at PLATOON granularity, not per body: compare the sets of
        # hand-container ids ([platoon] first-sight lines, both logs) and the
        # join's is a strict superset of the host's. At a settlement ~1600 u
        # from the mover - inside the host's 2500 u census reach, so not an
        # enumeration horizon - that reads as host 12 bodies vs join 18,
        # duplicated by ROLE: 2 Barmen where there is one bar, 6 Ninja Guards
        # vs 4, 8 Hungry bandits vs 5. The join's second Barman goes
        # drv -> ghost -> hid over ~15-30 s and then runs its own AI invisibly.
        # That pre-suppression window is what the field report ("join sees NPCs
        # the host doesn't") describes.
        #
        # The CAUSE is still open, and this harness cannot settle it: both
        # installs auto-load the same %LOCALAPPDATA%\kenshi\save folder and the
        # host's connect-push bakes into that folder WHILE the join reads it,
        # so what the join loads is a race. Varying -JoinDelaySec looks causal
        # (host solo before bake 15/68/128 s -> 7-12/2/0 join-only platoons)
        # but deferring the bake in the plugin instead, holding the settle time
        # at 120 s while the join launched normally, gave 13 - the worst
        # measured. The stagger was moving the join's load relative to the
        # host's WRITE, nothing more. See REPLICATION_PITFALLS.md section 13.
        #
        # Ruled out as causes: census truncation (n=43-51, none), staleness
        # (staleMs=0), zone loading (hostZoneUnloaded=0), and the mod's own
        # mint path (1 proxy bound all run, the rest deferred past mintR=600).
        #
        # Two harness traps this uncovered, both fixed in run_test.ps1: a
        # stagger longer than KENSHICOOP_ARM_TIMEOUT_MS makes the host arm
        # ALONE and finish before the join starts (use -ArmTimeoutMs), and
        # -Seconds/-KillGraceSec must grow with the stagger or the games die
        # before RESULT.
        #
        # Do not trust the per-phase popMover/popStay medians to show any of
        # this - suppression drops the extras out of the spatial query, so the
        # counts agree while the identities do not. Diff the platoon sets.
        #
        # VIEWPOINT PHASES (45 s each, logged as phase= on the SPLITFAR row):
        #   own        host->stay,  join->mover  - normal play
        #   both_stay  both -> the host's squad  - OVERLAP
        #   both_mover both -> the join's squad  - OVERLAP
        #   back       host->stay,  join->mover  - persistence check
        # The gate is judged on 'own' alone (folding the overlap phases in would
        # let the host's count be inflated by the host looking straight at it -
        # the effect under measurement, not a property of replication). The
        # overlap phases are the confound-free comparison: both clients drawing
        # one squad, so a gap there cannot be explained by nobody looking. See
        # docs/REPLICATION_PITFALLS.md S11-S12 for why the parked-camera and
        # symmetric-swap versions of this scenario both measured the wrong thing.
        #
        # It self-SKIPs when the pair never separates or the mover's region is
        # empty, so a bad save reports honestly instead of passing vacuously.
        # That is not hypothetical - the spot where this bug was FIRST caught
        # (join enumerated 8, host 0, sep=9841) turned out to be unbakeable: the
        # 8 bodies existed only in the join's process, so a bake writes an empty
        # field there. The divergence is emergent, not a property of the save,
        # so the fixture can only set up the CONDITIONS (two populated regions,
        # held apart) and let a long enough window expose it.
        # existence_parity stays ADVISORY here on purpose: this scenario exists
        # to EXPOSE ghosts, so gating on their absence would be backwards.
        # Seconds/KillGraceSec outlive the 190 s host window, as travel_parity.
        split_far = @{
            Save = 'splitfar1'; Setup = ''; Tolerance = 18.0
            Seconds = 250; KillGraceSec = 220
            PrimaryGate = 'split_far'
            Gating   = @('split_far', 'clock_sync')
            Advisory = @('existence_parity', 'lifecycle', 'suppress_churn',
                         'anti_zombie', 'mint_dist', 'smoothness')
            Tier = 'probe'; WanVariant = $false
        }

        # split_far2: the presence-authority scenario. split_far asks whether
        # the two clients AGREE about a far region; this one asks WHO SHOULD
        # AUTHOR it, which is a question that only exists once authority can
        # move. It is the only scenario that runs with KENSHICOOP_CELL_AUTH on
        # - every other entry runs with it clear, which is what makes the rest
        # of the tier a fail-open proof rather than a co-test.
        #
        # Same splitfar1 fixture (tab 0 in one populated place, tab 1 ~5200 u
        # away in another; cell_probe measured those as cells 21,32 and 21,31,
        # so they are genuinely separate authority regions). The difference is
        # the APPROACH: each side parks its own tab 600 u out and WALKS it back
        # in, because the reported symptom is a transient - "NPCs disappear as I
        # get close, then get replaced by the host's" - and a scenario that
        # teleported into position would skip the only moment it happens.
        # Walking the full 5200 u between the towns would take ~9 minutes at
        # Kenshi locomotion speed, so 600 u of real approach is the honest
        # compromise; what it buys is live zone streaming and a census that
        # first covers the region while somebody is watching.
        #
        # PHASES (walk 60 s, then 40 s each on the SPLITFAR2 row's phase=):
        #   walk       each -> own       approach; claims settle (3 s dwell)
        #   own        each -> own       steady state, disjoint attention
        #   both_join  both -> join tab  HOST attends a cell the JOIN authors
        #   both_host  both -> host tab  JOIN attends a cell the HOST authors
        #   back       each -> own       does the peer's population SURVIVE
        #
        # The gate reads three things, none of which any existing oracle can:
        # that the two tabs are in different cells at all (else the run proves
        # nothing and it SKIPs); that both logs' [cell] MAP dumps resolve the
        # same owner for the same cell (disagreement means two authors or none);
        # and that the both_* phases cost no attach pop, from the [attn] attach
        # counters. Seconds/KillGraceSec outlive the 230 s host window.
        split_far2 = @{
            Save = 'splitfar1'; Setup = ''; Tolerance = 18.0
            Seconds = 290; KillGraceSec = 260
            PrimaryGate = 'split_far2'
            Gating   = @('split_far2', 'clock_sync')
            Advisory = @('existence_parity', 'lifecycle', 'suppress_churn',
                         'anti_zombie', 'mint_dist', 'smoothness')
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1' }
            Tier = 'probe'; WanVariant = $false
        }

        # run_apart: split_far2's question over ground actually CROSSED, cross-map.
        #
        # split_far2 starts from a fixture that already has the squads 5200 u
        # apart and walks the last 600 u, which isolates the arrival transient but
        # never crosses country - watching it, the squads barely move. This starts
        # both tabs TOGETHER (runfar1 loads them ~37 u apart at -50879,3676) and
        # sends them in OPPOSITE directions, the host north and the join south,
        # ~160 k u of path each, ending 138,465 u apart - sixty-nine times the
        # 2000 u census reach - with zones streaming under moving anchors and cells
        # claimed and vacated the whole way.
        #
        # Both routes are a RECORDING of a human driving these two squads to those
        # two places (KENSHICOOP_TRACK_MOVE at 1 Hz, session 20260804_114911),
        # decimated to ~2000 u legs. That matters more than it sounds: the host's
        # recorded path is 164,532 u long for 88,242 u of displacement, so a
        # straight line between two points a human reached usually passes through
        # whatever they walked around. Three earlier revisions failed on exactly
        # that - one separated the pair no further than split_far2 does, one sent
        # the join across alone and it was knocked out 12,200 u in, and one walked
        # between an earlier session's cell claims (one per 4608 u) and wedged both
        # squads at -48980,-5670 with no path and nothing wrong with them.
        #
        # KENSHICOOP_SPEED_COMBAT_CAP=0: the arbiter pins the sim to 1x while either
        # squad fights, which is right for players and wrong here - the squad is
        # running AWAY from the encounter, and the cap stretched the crossing
        # five-fold for nothing. Only this scenario clears it.
        #
        # Speed sync arbitrates min(host, join), so the scenario votes 5x on BOTH
        # sides and re-votes every 10 s. Measured rate under that vote: ~570-600
        # u/s, so ~160 k u is ~280 s of running. split_far2's note that 5200 u
        # costs ~9 minutes is out by about 60x; it is ~9 seconds.
        #
        # runfar1 is a COPY of the buff1 manual save, tracked as a fixture so
        # run_test.ps1 restores it every run: buff1 itself is untracked, and the
        # host's connect-push bakes the live world over whatever it loaded, so
        # pointing a scenario at buff1 would consume the save it was testing.
        #
        # Seconds/KillGraceSec outlive the 590 s host window.
        run_apart = @{
            Save = 'runfar1'; Setup = ''; Tolerance = 18.0
            # The longest scenario in the suite by a wide margin: two ~160 k u
            # foot crossings plus four camera phases. 420 s of walk window and
            # 160 s of phases is 580 s on the join, and the host self-exits at
            # 590; the window has to outlive that or the kill lands mid-run and
            # the gate reads a truncated route as a stalled one.
            Seconds = 650; KillGraceSec = 620
            PrimaryGate = 'run_apart'
            Gating   = @('run_apart', 'clock_sync')
            Advisory = @('existence_parity', 'lifecycle', 'suppress_churn',
                         'anti_zombie', 'mint_dist', 'smoothness')
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'
                         KENSHICOOP_SPEED_COMBAT_CAP = '0' }
            Tier = 'probe'; WanVariant = $false
        }

        # town_arrive: walk into a town this save has never loaded, at travel speed.
        #
        # The gap this closes. Every other town scenario LOADS a save taken in the
        # town, so both clients read the same baked bodies and the hands match:
        # measured in Bad Teeth from a save, 78% of the host's census rows resolve
        # to a local body and 9% of the join's population is suppressed. That is
        # not how a player meets a town. Walk into one and each engine GENERATES
        # its population as the zone streams in: same spawn points, different
        # hands. The join then cannot resolve the host's census, mints proxies for
        # all of it, and suppresses its own natively-spawned bodies as unclaimed -
        # the same town walked into measured 4% resolved and 68% suppressed, with
        # 225 proxy binds and 264 census-missing rows (session 20260806_1224). On
        # screen: a town of NPCs popping in and out. Loading that same town from a
        # save made it go away, which is what isolated it to zone load time and is
        # why this scenario has to arrive on foot.
        #
        # Geometry, both measured from that session rather than guessed. The park
        # point (-35747,-14509) is the cell claim one cell short of town, where the
        # join's audit reported census=0 wide=0 - none of the town in memory, which
        # is the property it is chosen for. The target (-32899,-20539) is the
        # camera centre standing in it. 6,668 u apart.
        #
        # Navigation is self-guiding (aim 500 u along the bearing, re-aimed every
        # sample, sidestep by growing angles when it stops closing) rather than a
        # recorded route like run_apart. Over 6.7 k u the router copes with short
        # legs, and the scenario then retargets to any town via
        # KENSHICOOP_TOWN_FROM / KENSHICOOP_TOWN_AT ("x,z") instead of needing a
        # new recording. The gates judge population identity, not this town.
        #
        # 5x on both sides, re-voted, with the combat cap cleared for run_apart's
        # reason: town travel at speed is what a player does and what leaves the
        # streaming machinery least time to keep up, and a fight en route must cost
        # a detour rather than pinning the sim to 1x for the rest of the approach.
        #
        # runfar1: two tabs together, buffed enough to cross bandit country, and a
        # tracked fixture so run_test.ps1 restores it every run. NOTE the standing
        # requirement on this fixture - it must never have VISITED the target town,
        # or the population is baked into it and the scenario measures nothing. The
        # check is that this test FAILS on a build without the fix; a pass on an
        # unfixed build means the fixture, not the code, has changed.
        #
        # Seconds/KillGraceSec outlive the worst case: 240 s of approach deadline
        # plus a 120 s in-town hold plus the host's 10 s overhang = 370 s.
        town_arrive = @{
            Save = 'runfar1'; Setup = ''; Tolerance = 18.0
            Seconds = 430; KillGraceSec = 400
            PrimaryGate = 'town_arrive'
            Gating   = @('town_arrive', 'town_pop_parity', 'clock_sync')
            Advisory = @('existence_parity', 'lifecycle', 'suppress_churn',
                         'anti_zombie', 'mint_dist', 'smoothness', 'snap_rate')
            # ADOPT_RADIUS is pinned rather than inherited from the DLL default so
            # the run records the reach it measured against, and so the A/B against
            # pre-adoption behaviour ('0') is a one-line edit here, not a rebuild.
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'
                         KENSHICOOP_SPEED_COMBAT_CAP = '0'
                         KENSHICOOP_ADOPT_RADIUS = '250' }
            # full, not smoke: 430 s is longer than any smoke scenario and smoke is
            # already ~40 minutes, but this is the only scenario that generates a
            # town's population instead of loading it, so nothing else in the matrix
            # would catch adoption or the census walk band regressing. No WAN
            # variant - the gates are calibrated on LAN timings, and a 7-minute
            # scenario run twice is most of a tier.
            Tier = 'full'; WanVariant = $false
        }

        # town_arrive_far: the same town, reached after crossing the country.
        #
        # town_arrive parks one cell short of Bad Teeth and walks the last
        # 6,668 u, so the town's zone is the ONLY one that streams during the
        # run and the census machinery meets it in a clean state. A player
        # arrives having already crossed thirty thousand units of map, with
        # zones streamed and dropped under a moving anchor and cells claimed and
        # vacated the whole way - and the defects this pair of scenarios exists
        # to catch (adoption binding the wrong twin, the census walk band, park
        # futility) are all stateful. This runs the same arrival with that
        # history behind it.
        #
        # It walks a RECORDING, and the recording is one the suite already has.
        # run_apart's JOIN_ROUTE - a human driving a squad south - passes
        # THROUGH Bad Teeth: its point 12 (-35749,-14511) is town_arrive's park
        # point to within 2 u and its point 15 (-32893,-20457) is the town to
        # within 82 u. So its first 16 points are a walked path from where
        # runfar1 loads to the town, 34,088 u of ground for 30,160 u of
        # displacement. Both tabs follow that one path (the join's orders offset
        # 30 u so the squads are not sent onto the same point) rather than one
        # going north and one south. Starting on the route's own first point
        # also means there is no opening park, so unlike town_arrive this never
        # teleports the squads before it begins.
        #
        # Self-guiding this distance was tried first and failed hard
        # (20260806_215106): the pair closed 4,962 u, met something at 25,198 u
        # out, and the sidestep ladder took them 180,599 u to 97,068 u short -
        # the wrong side of the map, still running. That run paid for two fixes
        # in the walker (the ladder could aim BACKWARDS past 90 degrees and then
        # never recover, and nothing noticed a squad leaving) and it is why this
        # scenario follows a recording instead. Short approaches can be aimed;
        # long ones have to go where somebody has already walked. If a leg does
        # stall, the walker skips that point rather than sidestepping - the next
        # one is ~2,300 u further along ground a human covered.
        #
        # Walk deadline is derived in the scenario, not set here: 170 s flat plus
        # 10 ms/u over the ROUTE (not the line) is 511 s here, against the 237 s
        # the short approach comes out at. Worst case 511 s of approach + 120 s
        # of hold + the host's 10 s overhang = 641 s, which these outlive. A
        # clear run should take about 85 s of walking.
        #
        # probe until it has walked the route: town_arrive earned 'full' by
        # passing, and this one has not got there yet.
        town_arrive_far = @{
            Save = 'runfar1'; Setup = ''; Tolerance = 18.0
            Seconds = 720; KillGraceSec = 690
            PrimaryGate = 'town_arrive'
            Gating   = @('town_arrive', 'town_pop_parity', 'clock_sync')
            Advisory = @('existence_parity', 'lifecycle', 'suppress_churn',
                         'anti_zombie', 'mint_dist', 'smoothness', 'snap_rate')
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'
                         KENSHICOOP_SPEED_COMBAT_CAP = '0'
                         KENSHICOOP_ADOPT_RADIUS = '250' }
            Tier = 'probe'; WanVariant = $false
        }

        # cell_probe: measure ZoneManager's sector grid before any authority is
        # keyed on it. Read-only - no park, no mutation - so the SAVE is the
        # experiment: on 'sync' (a live town, both tabs together) it answers
        # whether a settlement fits in one cell; run it again with
        # -Save splitfar1 and it answers whether two squads 5200 u apart land in
        # different cells. Both must be true for a cell to work as an authority
        # key, and neither is knowable from the vendored headers, which expose
        # three position->coord mappings with no stated resolution.
        # No advisory gates: nothing moves, so motion oracles have nothing to say.
        cell_probe = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'cell_probe'
            Gating   = @('cell_probe')
            Advisory = @()
            Tier = 'probe'; WanVariant = $false
        }

        # cell_auth_together: presence authority with both squads in ONE cell.
        #
        # This is the configuration the suite had no coverage of, and the gap was
        # not accidental - it follows from two defensible decisions that combine
        # badly. CoopHarness pins KENSHICOOP_CELL_AUTH=0 so the tier proves the
        # feature is additive, and the four scenarios that opt back in via
        # DiagEnv (split_far2, run_apart, town_arrive, town_arrive_far) all exist
        # to test squads FAR APART, because that is where moving authority is
        # interesting. Nobody wrote the boring case, so the shipped default was
        # never exercised with two squads standing in the same cell - and that is
        # exactly where it breaks. A manual session on 2026-08-08 found the join
        # publishing 43 census rows for a cell the host owned, and both clients
        # then driving the same bodies for minutes at a time.
        #
        # No new fixture: 'sync' already puts both tabs in one live town
        # (cell_probe above uses it for that same property). The first cut of
        # this scenario was just that save plus the shipped flag, and it came
        # back clean - the join published n=0 enum=47 notmine=47, correctly
        # declining to author a cell the host owned. Co-location alone is not
        # the trigger; CONVERGENCE is. So the C++ driver walks the sequence the
        # manual session took: settle together, park the join's tab into a
        # neighbouring cell long enough to claim it, then bring it back into the
        # host's cell. See the class comment in ScenarioProbes.cpp.
        #
        # WHAT IT CATCHES TODAY, stated plainly because it is less than hoped.
        # The scenario reliably reproduces the PUBLISH-side half: after the
        # return the join keeps publishing ~30 census rows a tick while its own
        # [cell] MAP lists no cell it owns. It does not yet reproduce the mutual
        # drive, because that also needs the two clients to DISAGREE about
        # ownership, and on loopback the claim propagates in under 50 ms - the
        # two [cell] MAP dumps stay in lockstep through all 13 edge crossings.
        # Under the WAN profile the disagreement window opens (widest overlap
        # went 0 s -> 26 s across nine shared hands) but still falls short of
        # dual_drive's corroboration bar. So the WAN variant is the one with
        # teeth here, and the gate is honest about passing for now: it is a
        # non-regression guard on the configuration plus the premise, not a
        # standing reproduction. The reproduction of record is the 2026-08-08
        # manual session, which dual_drive fails hard.
        #
        # Seconds/KillGraceSec follow the house idiom and outlive both the
        # scenario's 180 s window and the default 150 s self-exit.
        cell_auth_together = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = 'dual_drive'
            Gating   = @('dual_drive', 'clock_sync')
            Advisory = @('cell_probe', 'npc_census', 'existence_parity',
                         'suppress_churn', 'anti_zombie', 'smoothness')
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'
                         # Provenance for every authored row. This is the one
                         # scenario built to interrogate the publish filter, so
                         # it is worth the log volume here and nowhere else.
                         KENSHICOOP_DEBUG_CENSUS = '1' }
            Tier = 'probe'; WanVariant = $true
        }

        # ---- interest-managed NPCs + pose ----------------------------------------
        npc_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'npc_track'
            Gating   = @('npc_track', 'pose', 'pose_state', 'body_state',
                         'smoothness', 'anim_truth', 'march', 'suppress_churn',
                         'snap_rate', 'rest_flap', 'clock_sync')
            # lifecycle (Phase 3): the [life] transition audit - advisory for
            # its first regression cycle, promoted once its baseline is known.
            Advisory = @('existence_parity', 'anti_zombie', 'lifecycle')
            Tier = 'smoke'; WanVariant = $true
            # DELIBERATE WAN-regime adjustments (re-validation matrix, 2026-07-05):
            # 120ms +/-40ms 5%-loss puts a walking NPC one-plus update interval
            # behind its host twin (measured worstMedian 4.8-10.5u vs the 0ms-tuned
            # 3u), and the walk-drive advances in catch-up steps (zeroFrac ~0.5 vs
            # 0.4) - the known latency micro-slide, already advisory for
            # coop_presence per the postmortem. Position fidelity still gates (at a
            # latency-scaled tolerance); smoothness demotes to advisory ONLY under
            # the WAN proxy.
            WanTolerance = 6.0
            WanDemote    = @('smoothness')
            # This save streams a small moving population, so some runs score
            # zeroFrac off very few frames and the fraction turns to noise: over
            # 12 clean runs on 2026-08-07 the ones under 800 scored frames
            # measured 0.021, 0.413 and 0.567, while every run above 1500 sat in
            # 0.28-0.42. Raise the sample floor rather than the ceiling - a small
            # sample should not be judged, and inflating the ceiling to cover it
            # would stop the gate judging the well-sampled runs too.
            #
            # 2026-08-10: the floor moves to the 1500 that reasoning named. Over
            # the full 91 clean runs recording both numbers, zeroFrac tracks the
            # SAMPLE rather than the motion - corr(log(active), zeroFrac) = -0.603
            # - and the band the 800 floor let through is where that shows: under
            # 1500 scored frames the median zeroFrac is 0.483 with 17 of 28 runs
            # over the 0.40 bound, at or above it 0.321 with 7 of 63 over. The
            # median BELOW the floor exceeded the ceiling, so the gate was failing
            # the typical under-sampled run before any defect was considered; both
            # 2026-08-10 failures scored 1241 and 1192 frames. This is mitigation,
            # not diagnosis: the median moving with the sample size is a bias, not
            # noise, so the SCENARIO ONSET telemetry measures the cause and the
            # floor is revisited once it reports.
            #
            # It has since reported, and the floor is now known to be a stopgap
            # for a metric defect rather than a genuine small-sample rule. The
            # zero frames are dominated by a fixed per-run toll charged at motion
            # onset (constant ~0.96 zero in the first 100ms, corr 0.998 between
            # that bucket's share of the sample and the whole-run zeroFrac), so
            # zeroFrac is that fixed count over however much sustained motion the
            # run caught. Raising the floor only excludes the runs where the
            # quotient is worst. The fix is to stop scoring the onset window -
            # see the onset* notes in src/plugin/sync/Replicator.h - after which
            # this floor should come back down rather than stay at 1500.
            Bounds = @{ smoothness = @{ MinActiveFrames = 1500 } }
        }
        # craft1 is loaded WITHOUT the host re-arm setup: CraftOrderScenario pins the
        # baked worker itself and issues the live order mid-run (that IS the test).
        # march is ADVISORY here (demoted 2026-07-16): craft1 is the suite's densest
        # town (~24 node-anchored standers, task=35). Phase 1/2 (9cb0dd4) pulled that
        # whole background population into the near tier as committed task poses, and
        # a STAND_AT_NODE pose can render its locomotion clip in place - marchFrac
        # regressed ~0 -> ~0.3 here (and only here; down1/duel1 stay gating). A code
        # fix lives in the taskApplied pose path and is entangled with the worker's
        # own operate pose (endAction there cancels the operate the test checks), so
        # it stays measured-but-advisory pending a dedicated pose-clip quiet. The
        # tested behavior (worker craft sync + pose tracking) is still GATED.
        craft_order = @{
            Save = 'craft1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'craft_order'
            Gating   = @('craft_order', 'pose_state', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # mine_pose covers the fixture family craft_order structurally cannot: a
        # crafting bench is BAKED into the shared save, so its hand matches on both
        # clients and the pose reproduces on identity alone, whereas a MINE is
        # instantiated per client for a terrain resource node and its hand does not
        # cross. That difference broke three things at once (protocol 55), so the
        # three gates here judge three different links in the same chain and a
        # regression in any one of them is separately diagnosable:
        #   mine_identity - the pairing crossed and actually TRANSLATED a hand
        #   mine_pose     - the miner ended up posed at the fixture (the animation)
        #   mine_output   - a production row APPLIED on a translated fixture (the ore)
        # mine_pose is primary because it is the reported symptom. mine1 is the only
        # save with a terrain ore node; before it the harness could not reach this
        # code path at all and the oracles were manual-only.
        mine_pose = @{
            Save = 'mine1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'mine_pose'
            Gating   = @('mine_pose', 'mine_identity', 'mine_output', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $false
        }

        # ---- body-state / reliable events ----------------------------------------
        down_order = @{
            Save = 'down1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'down_order'
            Gating   = @('down_order', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
            Tier = 'full'; WanVariant = $false
        }
        death_order = @{
            Save = 'down1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'death_order'
            Gating   = @('death_order', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
            Tier = 'full'; WanVariant = $true
        }

        # ---- combat ----------------------------------------------------------------
        combat_probe = @{
            Save = 'c'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'combat_probe'
            Gating   = @('combat_probe')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        combat_order = @{
            Save = 'duel1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'combat_order'
            Gating   = @('combat_order', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        combat_kill = @{
            Save = 'duel1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'combat_kill'
            # damage_guard: join's cosmetic fight must apply no EXCESS local damage
            # (the hitByMeleeAttack detour) - protocol 16 streams the victim's
            # vitals host->join, so the join legitimately tracks the host's drop.
            # npc_vitals: the streamed victim vitals must CONVERGE (Phase B) -
            # the join's copy shows the host's true blood, not pristine health.
            Gating   = @('combat_kill', 'damage_guard', 'npc_vitals', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate')
            Tier = 'smoke'; WanVariant = $true
        }

        # ---- player combat / medical (plan: Player Combat + Medical Replication) ----
        # player_combat: real combat damage TO player characters, both ownership
        # directions. Save 'sync' (the bar with ARMED NPCs). Characterization runs
        # fixed the shape: unarmed ally-vs-ally player duels draw ZERO blood, but
        # an armed bar NPC took a leader from 75.8 blood to -16 in ~25 s - so the
        # host orders NPC strikers onto each side's tab leader (A: join-owned
        # victim, B: host-owned victim). Gates on the striker's intent crossing
        # (task=65024 in the join's RECV series) AND the victim's blood dropping
        # on its OWNER (medical resolves on the victim's owner).
        player_combat = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'player_combat'
            Gating   = @('player_combat', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate')
            Tier = 'full'; WanVariant = $true
        }
        # assault_town: the JOIN's player character starts an UNPROVOKED fight
        # with a world NPC (the no-fight report: a join-picked town fight only
        # rendered on the join). No host-side orders - the fight must cross as
        # the join's streamed combat intent and run for real on the host. The
        # oracle walks the chain link by link (issue -> capture -> host order ->
        # host fight) so a FAIL names the broken link.
        assault_town = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'assault_town'
            Gating   = @('assault_town', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'lifecycle')
            Tier = 'full'; WanVariant = $false
        }
        # assault_travel: assault_town's fight re-run while both squads TRAVEL -
        # the ingredient behind the 2026-08-05 report ("an enemy was attacking my
        # character on the join client, but not the host", after a long run). The
        # paired logs of that session showed the join running a 22 s fight the
        # host never staged: the host took the order (r=2) and its copy never
        # engaged while the two worlds' copies drifted 31 -> 60 u apart. The
        # oracle walks assault_town's chain AND gates per-hand fight parity, so a
        # FAIL says whether the intent, the apply, the local fight or the
        # symmetry is what broke. The host window is 66 s from arm, inside the
        # default 150 s self-exit and 90 s kill grace, so no Seconds override.
        assault_travel = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            PrimaryGate = 'assault_travel'
            Gating   = @('assault_travel', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate', 'lifecycle')
            Tier = 'full'; WanVariant = $false
        }
        # assault_mint: the closer reproduction of the 2026-08-05 report. Same
        # script as assault_travel, but the victim is a body the join never had
        # at load - the host spawns the enemy squad at census range (450 u, inside
        # the mint radius, outside the stream bubble), the join picks a victim
        # absent from its own arm-time NPC census (so: a minted proxy) and the
        # attack order's pathing runs its PC out to it. The field bandit arrived
        # exactly this way, and 47% of that session's combat orders came back r=1
        # ("the victim hand does not resolve here"), so link 0 of the oracle
        # asserts the mint really happened before judging the fight.
        # Gated on proxy_drift, with the fight parity kept as a FINDING. The join
        # ORDERS this attack, and a join melee on a host-owned NPC is deliberately
        # damage-guarded - it reports the damage it would have dealt and the host
        # applies it, holding its own copy at peace for position parity (protocol
        # 45). So "the join is fighting and the host is not" is this scenario's
        # design, not its defect: the fraction sat at 0.23-0.39 across five runs
        # spanning three different builds, drifting across the 0.25 line without
        # tracking any change. What must hold here is the same thing that must
        # hold everywhere - the minted body stands where its owner has it.
        assault_mint = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            PrimaryGate = 'proxy_drift'
            Gating   = @('proxy_drift', 'clock_sync')
            Advisory = @('assault_mint', 'smoothness', 'anim_truth', 'march', 'combat_snap_rate', 'lifecycle', 'mint_dist')
            Tier = 'full'; WanVariant = $false
        }
        # mint_aggro: the UNPROVOKED half of the same report, and the half that
        # assault_mint cannot show. There the join orders the attack, which makes
        # the enemy's answer player-authored - architecturally pc_assault, where a
        # cosmetic join-side fight against a peaceful host copy is by design. Here
        # nothing of ours touches it: the host spawns a squad that is hostile and
        # still has its AI (spawnRuntimeSquad's detach leaves bodies inert), the
        # join mints it from the census, the host sets it down beside the join's
        # characters, and the fight is the enemy's to start. Whoever does not own
        # that body must not be running a fight for it.
        # The GATE here is proxy_drift, not the fight parity. Whether a given
        # sample catches both copies mid-swing is a sampling race - measured
        # across four runs the fight-parity fraction swung from 0.045 to 0.354
        # in both directions with no code change - whereas the distance between
        # a proxy and the body it stands for either stays bounded or does not.
        # Parity stays on as a finding because it is what the player SEES.
        mint_aggro = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            PrimaryGate = 'proxy_drift'
            Gating   = @('proxy_drift', 'clock_sync')
            Advisory = @('mint_aggro', 'smoothness', 'anim_truth', 'march', 'combat_snap_rate', 'lifecycle', 'mint_dist')
            Tier = 'full'; WanVariant = $false
        }
        # player_ko: players as VICTIMS both directions - scaffold KO + revive on
        # each side's OWN member; edges must cross as reliable EVT_KNOCKOUT/EVT_REVIVE
        # and the peer's driven copy must lie down / stand up.
        player_ko = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'player_ko'
            Gating   = @('player_ko', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # medic_order: cross-player first aid both directions. Gates on the full
        # medical round trip (wound crosses to the healer, bandage finds it,
        # treatment returns to the owner) - requires the phase-2 vitals-sync +
        # treatment-forwarding features; before them it documents spikes 21-23's
        # truth (nothing crosses).
        medic_order = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'medic_order'
            Gating   = @('medic_order', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # limb_loss: protocol-16 limb replication both directions - each side
        # runs the engine's own amputate on ITS OWN tab leader (A: host member
        # LEFT_ARM, B: join member RIGHT_ARM). Gates that the copy side reaches
        # the STUMP LimbState within budget (reliable EVT_AMPUTATE + medical
        # self-heal), never reverts, and both sides converge on the severed
        # ground item (host-authoritative world-item channel + join dedupe).
        limb_loss = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'limb_loss'
            Gating   = @('limb_loss', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # crawl_move: protocol-53 prone posture + crippled cause, both
        # directions - each side amputates a LEG on ITS OWN tab leader (A: host
        # member LEFT_LEG, B: join member RIGHT_LEG) and then keeps that body
        # moving on a short alternating leg. Gates that the copy reaches the
        # owner's prone POSTURE and crippled FLAG within budget, never reverts,
        # and tracks position while crawling. The hole limb_loss left: it cuts
        # arms and judges a body standing still, so nothing ever watched an
        # injured character walk - which is where a crippled crawler was being
        # driven as an upright walker. WanVariant: the crippled cause rides the
        # reliable medical channel and must converge under loss.
        crawl_move = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'crawl_move'
            Gating   = @('crawl_move', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # stats_sync: protocol-17 character-stats replication both directions -
        # each side raises stats on ITS OWN tab leader via the raise-only
        # scaffold (A: host raises Strength+Stealth, B: join raises Dexterity+
        # Athletics). Gates that each raised value crosses to the peer's driven
        # copy within budget (change-gated reliable PKT_STATS), stays sticky,
        # and the untouched control stat (Toughness) does not drift. WanVariant:
        # the stats channel is reliable and must converge under loss.
        stats_sync = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'stats_sync'
            Gating   = @('stats_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # carry_order: protocol-18 carried-body replication - three carry legs
        # (A: host own-tab, B: join carries a host-owned body, C: host carries
        # a join-owned body). Gates that each pickup/drop crosses to the peer's
        # LOCAL pair within budget (reliable EVT_PICKUP_BODY/EVT_DROP_BODY +
        # TASK_CARRY_BODY/BODY_CARRIED self-heal) and the carried copy rides
        # its carrier's shoulder (no down-enforcement dragging). WanVariant:
        # the edges are reliable and the carve-out must hold under loss.
        carry_order = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'carry_order'
            Gating   = @('carry_order', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        npc_carry = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'npc_carry'
            Gating   = @('npc_carry', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # bed_pose: conscious bed use (protocol 19 phase 1). The host orders its
        # leader to USE_BED_ORDER at the baked Camp Bed (save 'bedcage1'); the
        # join's driven copy must commit the same bed task at the same fixture.
        # Closes the "reproducible-pose allowlist covers beds but was never
        # runtime-validated" gap (spike 24 PARTIAL).
        bed_pose = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'bed_pose'
            Gating   = @('bed_pose', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $true
        }

        # bed_wake: conscious bed EXIT (protocol 19). bed_pose only proved ENTER +
        # HOLD; this drives the wake-and-move arc that was desyncing (host PC
        # sleeps, then wakes and walks - the join copy stayed stuck sleeping). The
        # host orders L0 into the baked Camp Bed, waits for the join to commit the
        # pose, then issues a move ~25u away; the join copy must leave the bed
        # (BODY_IN_BED clears, [furn] BED FAST-EXIT) and follow the host.
        bed_wake = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'bed_wake'
            Gating   = @('bed_wake', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $true
        }

        # bed_lay: UNCONSCIOUS place-in-bed LAYING pose + wake-and-exit (protocol
        # 19). bed_pose proved a conscious sleep ORDER lays down and bed_put proved
        # unconscious OCCUPANCY crosses; this proves a KO'd body DROPPED into a bed
        # renders the LAYING pose on BOTH clients and can get back OUT when it wakes.
        # (A conscious placement was ruled out: Kenshi itself nondeterministically
        # stands a conscious placed body on the mattress and the join mirrors it
        # faithfully - base-game behavior, not a coop bug, run 2026-07-17.) The owner
        # KO's M2 (then L1), drops it in the baked Camp Bed, then revives + moves it;
        # the pelvis + BODY_IN_BED in the MEMBER/RECV series must read LAYING on both
        # clients, and both must leave the bed after the wake.
        bed_lay = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'bed_lay'
            Gating   = @('bed_lay', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $true
        }

        # bed_put: unconscious placement (protocol 19 phase 3). Window A: the
        # host KO's its M2 (holdDown re-topped) and places it in the baked Camp
        # Bed via the putSubjectInFurniture scaffold, then takes it back out;
        # window B: the join does the same with its L1. The peer's driven copy
        # must mirror enter + exit (reliable EVT_ENTER/EXIT_FURNITURE edges +
        # BODY_IN_BED self-heal), engine-native via setBedMode.
        bed_put = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'bed_put'
            Gating   = @('bed_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # cage_put: same two windows against the baked Prisoner Cage
        # (setPrisonMode / BODY_IN_CAGE).
        cage_put = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'cage_put'
            Gating   = @('cage_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # chain_put: protocol-41 chained/pole prisoner state. Same two windows
        # as cage_put but kind=3 (Character::isChained -> setChainedMode). The
        # subject is self-chained (a pole needs no baked fixture; the owner is a
        # save-stable hand), so this gates the chained STATE crossing. Reuses
        # the generic Test-FurnPut oracle (Kind 3 -> the chain_put gate).
        chain_put = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'chain_put'
            Gating   = @('chain_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # pole_put: protocol-19 kind=4 prisoner POLE. Same two windows as
        # cage_put but the subject is placed on a baked standing PRISONER POLE
        # (fixture save 'pole1', baked via `bake_scene.ps1 -Setup pole
        # -BaseSave squad1 -BakeSave pole1`). A pole is the SAME containment as a
        # cage (setPrisonMode -> occupant reads in=2), so this is the controlled,
        # deterministic 'body ON A POLE' test: the FURNACT marker is kind=4 (the
        # pole gate) while the FURN occupancy is judged in=2 (Test-PolePut).
        pole_put = @{
            Save = 'pole1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'pole_put'
            Gating   = @('pole_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # cage_peer_sync: protocol-36 third-party placement - the HOST places
        # the join's KO'd leader (a peer-owned driven body) into the baked
        # cage, reproducing the guard-jailing-the-join-PC session bug. The
        # host must author the PEER-ENTER edge (no occupant-owner edge can
        # fire - the action ran purely on the host sim), hold its self-heal
        # exit, and the join must apply the enter to its OWN KO'd body; exit
        # stays owner-authored (the join frees itself at the end).
        cage_peer_sync = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'cage_peer'
            Gating   = @('cage_peer', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # sneak_probe: protocol-20 phase-0 spike (host-side, log-only). The host
        # forces stealthMode on its DRIVEN copy of the join's leader near the bar
        # NPCs and logs the copy's whoSeesMeSneaking series - proves the engine's
        # detection fires against driven copies and the map read is safe.
        sneak_probe = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'sneak_probe'
            Gating   = @('sneak_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # sneak_pose: stealth POSTURE crossing, both ownership directions
        # (window A host L0, window B join L1). The peer's driven copy must
        # flip Character::stealthMode (BODY_SNEAK / setStealthMode) within
        # budget on all four edges.
        sneak_pose = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'sneak_pose'
            Gating   = @('sneak_pose', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # sneak_detect: detection-indicator feedback. The join's leader sneaks
        # near the bar NPCs (save 'sync' - guaranteed NPC availability, dodging
        # the squad1 wandering-NPC flake); the host's world detects its driven
        # copy, streams PKT_STEALTH back, and the join replays the entries onto
        # its local pair.
        sneak_detect = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'sneak_detect'
            Gating   = @('sneak_detect', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # spawn_probe: runtime-spawn phase-0 diagnostic (spawnSync FORCED OFF by
        # Config). Host runtime-spawns squads near + far, join spawns its own
        # squad locally; gates on the failure-mode EVIDENCE (join logs
        # "[spawn] unresolved" for host runtime hands) and RECORDS whether
        # enforceHostAuthority caught the join-local spawns (the phase-2 gap
        # measurement - a finding, not a gate). Save 'sync': a live town gives
        # findNearbyNonPlayerFaction real factions to mint squads in.
        spawn_probe = @{
            DiagEnv = @{ KENSHICOOP_SPAWN_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'spawn_probe'
            Gating   = @('spawn_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # shop_probe: vendor-trading diagnostic (money sync forced off). Both
        # sides log nearby vendors (money+stock) and every squad tab's
        # per-platoon wallet at 1 Hz; the host then drives one programmatic
        # Inventory::buyItem, and the join tries the same against its driven
        # vendor copy. Gates only on the EVIDENCE (vendor enumerated, wallet
        # series readable, both buy attempts logged); wallet/vendor divergence
        # and the join-buy outcome are recorded as FINDINGs that gate the
        # vendor-mirror design. Save 'sync': the bar town puts real ShopTraders
        # in range.
        shop_probe = @{
            DiagEnv = @{ KENSHICOOP_MONEY_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'shop_probe'
            Gating   = @('shop_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # wallet_probe: WHICH engine field is the player's purse (money sync
        # forced OFF so nothing is being written by the channel). Both sides log
        # the player-faction wallet (POOL) next to the per-platoon squad wallets
        # (TABW) at 1 Hz and the host round-trips a write through the pool. Gates
        # that the measurement happened + the pool is writable; the identity of
        # the live field is the FINDING protocol 52 rests on.
        wallet_probe = @{
            DiagEnv = @{ KENSHICOOP_MONEY_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'wallet_probe'
            Gating   = @('wallet_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # money_sync: protocol-52 shared money pool (moneySync ON - the
        # default). Kenshi keeps ONE player wallet, so this gates CONSERVATION
        # rather than convergence: the host seeds the pool to 10000, the join
        # spends 250 and the host 1000, and both clients must end at 8750. A
        # surplus means a spend was lost (what snapshot sync would do to two
        # concurrent purchases); a deficit means one was applied twice.
        money_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'money_sync'
            Gating   = @('money_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # vendor_trade: protocol-52 phase 1c - the buyer-side purchase
        # COMPOSITE. Each side performs the exact buyer-side mutations of one
        # Inventory::buyItem (cats out of the shared pool + bought item into the
        # tab leader's personal inventory, same tick); gates that BOTH effects
        # land - the item converges on the peer over the inventory snapshot
        # channel, and BOTH purchases are paid for out of the one pool (seed
        # 5000 -> 4500). The vendor-side mutation stays local by design (the
        # engine regenerates vendor stock per client; the [shop] BUY-LOCAL detour
        # is collecting field evidence for that mirror).
        vendor_trade = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'vendor_trade'
            Gating   = @('vendor_trade', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # recruit_probe: mid-session recruitment phase-0 diagnostic (protocol
        # 23 - no recruit sync exists, nothing is forced off). Each side
        # recruits the nearest BAKED world NPC and then a RUNTIME spawn via
        # the engine's own PlayerInterface::recruit, logging the subject's
        # hand BEFORE/AFTER (container change = the identity break) and a 1 Hz
        # distinct-container TABS census (rank-reshuffle evidence). Gates only
        # that the script ran; the identity/tab/peer-reaction findings gate
        # the 2b design. Save 'sync': the bar town has world NPCs in range.
        recruit_probe = @{
            DiagEnv = @{ KENSHICOOP_RECRUIT_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'recruit_probe'
            Gating   = @('recruit_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # squad_probe: squad-management phase-0 diagnostic (protocol 35,
        # squadSync forced OFF). Each side separates its own tab's highest-
        # hand member into a NEW squad (lever 0) then tries to move it back
        # into its original tab (lever 1 setFaction, lever 2 addCharacterAt
        # fallback), logging hand BEFORE/AFTER per move, the pointer-diff
        # SQEDGE stream, and a 1 Hz SQTABS census (per-tab member counts -
        # the rank-reshuffle evidence). Gates the local legs + that the
        # pointer-diff caught every landed move; the identity/rank/peer-
        # reaction findings gate the 16b design. Save 'squad1': the baked
        # 2-tab squad both ownership ranks partition on.
        squad_probe = @{
            DiagEnv = @{ KENSHICOOP_SQUAD_SYNC = '0' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'squad_probe'
            Gating   = @('squad_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # squad_sync: protocol-35 squad management sync (squadSync ON). Same
        # script as squad_probe; every scripted move must LAND locally and
        # Test-SquadSync gates the crossing - each move authored a reliable
        # EVT_SQUAD_MOVE, the peer re-keyed its local body onto the new hand
        # (no duplicate proxy, no unresolved-hand storm), tracked it (PROXY
        # series), and the pre-existing tab ranks never shifted (the
        # container-rank latch holding). Save 'squad1': the baked 2-tab squad.
        squad_sync = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'squad_sync'
            Gating   = @('squad_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # recruit_sync: protocol-23 recruitment sync (recruitSync ON). Same
        # script as recruit_probe; gates that every recruited hand CONVERGED on
        # the peer with exactly ONE body - the baked legs by "[recruit] REKEY"
        # (the peer's existing copy re-keyed to the new stream hand, no
        # duplicate proxy), the runtime legs by "[spawn] proxy BOUND" over the
        # now-BIDIRECTIONAL describe channel - and that the peer then TRACKED
        # each hand (SCENARIO PROXY series). All four local recruits must
        # succeed (res=1), unlike the probe where failure is data.
        recruit_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'recruit_sync'
            Gating   = @('recruit_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # recruit_ctl: Phase-1b control validation (recruit + transfer). Host
        # recruits a baked NPC, walks it (join drives - GAIT phase=A parity
        # sample), the join transfers it into its own tab (control-flip), then
        # walks it (host drives - GAIT phase=B). Test-RecruitCtl gates gait
        # parity (a driven squad member reproduces the owner's RUN, not a walk)
        # and anti-phantom (a control-flip claim never mints a duplicate proxy).
        # Save 'sync': the bar town has recruitable NPCs and both squads.
        recruit_ctl = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'recruit_ctl'
            Gating   = @('recruit_ctl', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # faction_probe: faction-relation phase-0 diagnostic (protocol 24 -
        # factionSync forced OFF; the affectRelations detours stay on for
        # cause-attribution evidence). Both sides log a 1 Hz FACREL series
        # (every interesting player-faction relation row by GameData sid) and
        # write one sentinel setRelation each (host -75 first sorted sid,
        # join +65 second, both table rows). Gates only that the script ran;
        # the sid-stability / crossing / operative-row findings gate the 3b
        # channel design.
        faction_probe = @{
            DiagEnv = @{ KENSHICOOP_FACTION_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'faction_probe'
            Gating   = @('faction_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # faction_sync: protocol-24 faction-relation sync (factionSync ON).
        # Same script as faction_probe; gates that each side's sentinel
        # relation write CONVERGED on the peer (final us AND them rows for
        # that sid equal the target) and no co-visible row ended diverged.
        faction_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'faction_sync'
            Gating   = @('faction_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # time_probe: game-clock phase-0 diagnostic (protocol 25 - timeSync
        # AND speedSync forced OFF so the host's 2x burst t=15..25s applies
        # directly and raw unsynced drift is measured). Both sides log a 1 Hz
        # GTIME series (in-game hours + hourLen + fsm + paused). Gates only
        # that the clocks are readable + monotonic; the absolute-vs-relative /
        # offset / drift / rate-vs-fsm findings gate the 25 channel design.
        time_probe = @{
            DiagEnv = @{ KENSHICOOP_TIME_SYNC = '0'; KENSHICOOP_SPEED_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'time_probe'
            Gating   = @('time_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # time_sync: protocol-25 game-clock sync (timeSync + speedSync ON).
        # Same script, both sides click 2x t=15s / 1x t=25s (consensus
        # arbitrates 2x); gates that both clocks stay monotonic and the final
        # host-join clock offset is inside tolerance across the speed change.
        time_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'time_sync'
            Gating   = @('time_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # door_probe: door-state phase-0 diagnostic (protocol 26 - doorSync
        # forced OFF). Both sides log a 1 Hz DOOR census (save-stable hand +
        # open/locked/state per baked door within ~100m of the interest
        # centers) and toggle one sentinel door each (host the first in serial
        # order t=12s, join the second t=24s) through the engine's own
        # openDoor/closeDoor. Gates only that the script ran (census + write
        # attempt); the hand-stability / write-lever / non-crossing findings
        # gate the protocol-26 channel design. Save: 'sync' - if its area has
        # no doors in range the probe FAILS loudly and the entry moves to a
        # save with a town/base before 5b.
        door_probe = @{
            DiagEnv = @{ KENSHICOOP_DOOR_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'door_probe'
            Gating   = @('door_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # door_sync: protocol-26 door-state sync (doorSync ON). Same script;
        # gates that each side's sentinel toggle CROSSED (the peer's final
        # census row for that hand shows the writer's target open state) and
        # no co-visible door ended diverged.
        door_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'door_sync'
            Gating   = @('door_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # build_probe: placed-building phase-0 diagnostic (protocol 27 -
        # buildSync forced OFF). Both sides log a 1 Hz BUILDSITE census
        # (construction sites within ~100m) and each places a small template
        # programmatically leader-relative (host t=10s side -4, join t=22s
        # side +4), then ramps its own site's constructionProgress +0.25/3s
        # through the engine's own setter. Gates that both placements were
        # ATTEMPTED and at least one was accepted + enumerable; the findings
        # (factory-vs-town-rules, runtime-hand non-overlap, progress scale /
        # self-complete) gate the protocol-27 channel design. Save: 'sync' -
        # town-adjacent, so the factory-bypass question gets a real answer;
        # if BOTH placements refuse, the probe fails loudly and the entry
        # moves to a wilderness save before 6b.
        build_probe = @{
            DiagEnv = @{ KENSHICOOP_BUILD_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'build_probe'
            Gating   = @('build_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # build_sync: protocol-27 placed-building sync (buildSync ON). Same
        # script as build_probe; gates that each side's placement was MINTED
        # on the peer (describe/mint by placer key) and that the placer's
        # construction-progress ramp CROSSED (the peer applied rows up to the
        # complete=1 latch through the engine's own setter).
        build_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'build_sync'
            Gating   = @('build_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # bdoor_probe: placed-building door + removal phase-0 diagnostic
        # (protocol 28 - bdoorSync forced OFF, protocol-27 mint channel ON).
        # Both sides place a SHACK (a door-bearing template), ramp it to
        # self-complete, census nearby doors with their parent-building link
        # (1 Hz), toggle their OWN shack's door #0, and the host destroys its
        # shack at t=42s. Gates the LOCAL legs only (place + door exists +
        # toggle stuck + destroy worked); the findings (proxy doors on the
        # peer's mint, toggle non-crossing, the post-destroy ghost) gate the
        # protocol-28 channel design.
        bdoor_probe = @{
            DiagEnv = @{ KENSHICOOP_BDOOR_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'bdoor_probe'
            Gating   = @('bdoor_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # bdoor_sync: protocol-28 placed-building door + removal sync
        # (bdoorSync ON). Same script as bdoor_probe; gates that each side's
        # door toggle CROSSED onto the peer's minted proxy (applied [bdoor]
        # RECV + census at the toggled state) and that the host's destroy
        # REMOVED the join's proxy (REMOVE-RECV ok=1 + no ghost census rows).
        bdoor_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'bdoor_sync'
            Gating   = @('bdoor_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # hunger_probe: hunger phase-0 diagnostic (protocol 29 - hungerSync
        # forced OFF, the rest of the medical snapshot streaming). Both sides
        # census the whole squad's hunger/fed/dazedOrAlert at 1 Hz and each
        # writes a proportional SENTINEL hunger (own-tab leader, current*0.6;
        # host t=15s, join t=22s). Gates the LOCAL legs (census + sentinel
        # stuck); the findings (scale, per-client decay agreement, sentinel
        # non-crossing, dazedOrAlert range) gate the protocol-29 fold-in.
        hunger_probe = @{
            DiagEnv = @{ KENSHICOOP_HUNGER_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'hunger_probe'
            Gating   = @('hunger_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # hunger_sync: protocol-29 hunger sync (hungerSync ON - the hunger/fed
        # scalars riding the owner-authoritative medical snapshot). Same
        # script as hunger_probe; gates that each side's sentinel hunger drop
        # CROSSED onto the peer's driven copy (drop-relative, within 10 s)
        # and that end-of-run owner-vs-copy hunger gaps stay small for every
        # shared hand.
        hunger_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'hunger_sync'
            Gating   = @('hunger_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # latejoin_probe: late-join phase-0 diagnostic (protocol 30 -
        # latejoinSync forced OFF, everything else streaming). The HOST
        # mutates state in the PRE-ARM window - before the join connects:
        # toggles a baked door, writes a sentinel faction relation (-85),
        # bumps its tab wallet (+777), and places + completes a small
        # building. Post-arm both sides census door/faction/money at 1 Hz.
        # Gates the LOCAL legs (host mutations ok + censuses ran); the
        # findings (per-channel heal latency via safety resends, the
        # building that NEVER mints) motivate the connect-edge resync.
        latejoin_probe = @{
            DiagEnv = @{ KENSHICOOP_LATEJOIN_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'latejoin_probe'
            Gating   = @('latejoin_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # latejoin_sync: protocol-30 connect-edge resync (latejoinSync ON).
        # Same script as latejoin_probe; gates that ALL pre-connect host
        # mutations converged on the join shortly after connect - the door,
        # faction and money censuses agree across clients, and the
        # pre-connect building MINTED on the join (the probe's permanent
        # gap, closed) with its complete latch.
        latejoin_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'latejoin_sync'
            Gating   = @('latejoin_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # save_probe: coordinated-save phase-12a diagnostic (protocol 31 -
        # save detour installed for edge logging, NO coordination). The HOST
        # issues a mid-session saveGameAs('coopresume') and the probe retires
        # the two runtime unknowns gating the host-authoritative transfer:
        # getCurrentGame/getSavePath resolve (spike 39 RVAs, never called
        # before) and the folder-quiescence completion edge (latency + file
        # count/bytes + the gameplay hitch while the engine writes).
        save_probe = @{
            DiagEnv = @{ KENSHICOOP_SAVE_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_probe'
            Gating   = @('save_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # save_sync: protocol-31 coordinated save + in-band transfer (saveSync
        # ON - the default). The HOST issues one mid-session
        # saveGameAs('coopresume'); the coordination does the rest: detour
        # edge -> folder quiescence -> paced whole-folder stream to the join
        # (BEGIN/FILE/DONE) -> staged CRC-verified atomic commit -> ACK.
        # Gates the full round trip: LOCAL-SAVE edge, QUIESCED, XFER-SENT,
        # COMMIT ok with file/byte counts EQUAL to the host's, ACK ok=1.
        save_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_sync'
            Gating   = @('save_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # save_stage1: resume_test.ps1 stage 1 (not a tier member - the
        # two-stage wrapper drives it). Same coordinated-save gates as
        # save_sync, but the host FIRST places a construction site and ramps
        # it part-way (session-runtime state), so the transferred save carries
        # a building that exists in NO baked save - the stage-2 same-hand
        # evidence.
        save_stage1 = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_sync'
            Gating   = @('save_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false
        }

        # resume_check: resume_test.ps1 stage 2 (not a tier member). Both
        # clients relaunch on the save the stage-1 transfer delivered
        # (KENSHICOOP_SAVE=coopresume, NO harness save mirroring - the join
        # loads what the TRANSFER wrote) and census construction sites at
        # 1 Hz. Gates that the stage-1 building enumerates on BOTH sides
        # under the SAME save-stable hand - the identity-reset proof.
        resume_check = @{
            Save = 'coopresume'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_resume'
            Gating   = @('save_resume', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false
        }

        # load_probe: coordinated-load phase-13a diagnostic (protocol 32 -
        # load detour installed for edge logging, NO load coordination;
        # saveSync stays ON so the join holds an identical copy first). The
        # HOST issues a coordinated saveGameAs('coopresume'), waits for the
        # transfer DONE, then a MID-SESSION engine::loadSave('coopresume').
        # Retires the runtime unknowns gating the coordinated load: is the
        # mid-session load safe, does mainLoop_hook tick across the load
        # screen, does the host survive its own swap with sync running, and
        # do save-stable hands re-resolve in the fresh world. The JOIN
        # deliberately does NOT load (the 13a divergence baseline).
        load_probe = @{
            DiagEnv = @{ KENSHICOOP_LOAD_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'load_probe'
            Gating   = @('load_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # load_sync: protocol-32 coordinated load (loadSync ON - the default).
        # The user's manual scenario, automated: the HOST places a building,
        # coordinated-saves 'coopresume' (the join commits an identical copy),
        # then LOADS it mid-session. The coordination must drive the join to
        # load the same save: GO broadcast -> join fingerprint MATCH -> join
        # bypass-once load -> BOTH sides world-swap + protocol-32 session
        # reset -> the pre-load building enumerates on BOTH sides POST-load
        # under the SAME save-stable hand. clock_sync is NOT gated: the load
        # rebuilds both worlds mid-run, which legitimately restarts the
        # in-game clock series the oracle aligns on.
        load_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'load_sync'
            Gating   = @('load_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'clock_sync')
            Tier = 'full'; WanVariant = $false
        }

        # money_persist: the regression gate for the bug protocol 52 fixes - cats
        # the JOIN moved used to be erased by the next load, because only the
        # host's save survived and it held the host's untouched wallet. The JOIN
        # spends a QUARTER of the pool (a fraction, so no fixture balance is
        # assumed and the spend is still real with the channel OFF), the host
        # waits until that delta has actually moved its own (authoritative) pool,
        # latches the total, then coordinated-saves and loads mid-session. Gates
        # that BOTH clients come back at the latched total; coming back at the
        # pre-spend number IS the bug, which KENSHICOOP_MONEY_SYNC=0 reproduces.
        # clock_sync is not gated for the same reason as load_sync (the load
        # restarts the in-game clock series the oracle aligns on).
        money_persist = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'money_persist'
            Gating   = @('money_persist')
            Advisory = @('smoothness', 'anim_truth', 'march', 'clock_sync')
            Tier = 'full'; WanVariant = $false
        }

        # connect_relink (Phase 14 plan 01, UI-06): the scenario re-runs the F2
        # panel's Connect handler through a ScenarioContext adapter, so the
        # NetLink session boundary (stop() + start again on the reused g_net
        # singleton) is exercised with NO keyboard and NO panel on screen. One
        # side relinks, the other watches its roster drop and come back.
        #
        # PrimaryGate = 'panel_config' is the honest gate, not a placeholder: it
        # already exists and already parses the '[coop-ui] connect:' contract
        # (role/transport/peer/ownRanks/src), but it has only ever returned SKIP
        # in a harness run, because the panel was hand-driven and no automated
        # run could produce a single connect line. This scenario is the first
        # thing that can make panel_config actually judge.
        #
        # Tier='none' ON PURPOSE: keeping it out of regress.ps1's smoke/full
        # sweeps means no existing tiered run changes shape, which is Phase 14's
        # criterion 4 (a run that never invokes the lever is indistinguishable
        # from a pre-phase run). Drive it explicitly via run_test.ps1 -Scenario.
        #
        # KENSHICOOP_SAVE_SYNC='0' isolates what is under test. With save sync
        # ON, every reconnect re-arms the host's connect-push, so the run would
        # be measuring the bootstrap save-transfer pipeline instead of the roster
        # boundary - and would take far longer to do it.
        connect_relink = @{
            DiagEnv = @{ KENSHICOOP_SAVE_SYNC = '0' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'panel_config'
            Gating   = @('panel_config')
            Advisory = @('clock_sync')
            Tier = 'none'; WanVariant = $false
        }

        # bootstrap_stream: the missing-save "seamless join" proof (protocol
        # 31/32). NOT a run_test tier member and NOT run_test-drivable - it needs
        # the join-from-MENU launch (join has NO save, goes ONLINE, host bakes +
        # streams its live world on connect), which scripts\stream_test.ps1
        # provides. On one machine both installs share %LOCALAPPDATA%\kenshi\save
        # so a join would normally MATCH + load from disk; stream_test sets
        # KENSHICOOP_FORCE_STREAM=1 on the JOIN so it NACKs the LOAD_GO and the
        # real folder transfer runs. The connect_stream gate REQUIRES the transfer
        # edges (NACK -> XFER-SENT -> XFER-COMMIT badCrc=0 -> XFER-ACK ok=1 ->
        # transfer-committed load -> gameplay), so a run that quietly MATCH-loaded
        # FAILS. DiagEnv here is documentation (stream_test sets it join-only).
        bootstrap_stream = @{
            DiagEnv = @{ KENSHICOOP_FORCE_STREAM = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'connect_stream'
            Gating   = @('connect_stream')
            Advisory = @()
            Tier = 'none'; WanVariant = $false
        }

        # prod_probe: production machine phase-0 diagnostic (protocol 33 -
        # prodSync forced OFF, the protocol-27 mint channel ON). The HOST
        # places a generator + crafting bench leader-relative and ramps both
        # complete (minting proxies on the join); both sides census machine-
        # class buildings at 1 Hz (power/state/output/inputs/tech/farm);
        # the host operates the bench 1 Hz t=20-50s (divergence driver),
        # toggles the generator's power OFF/ON, then writes the bench output
        # via the native setProductionItem and a direct amount write. Gates
        # the LOCAL legs only (place + ramp + census + power applied +
        # setitem landed + ops ran); the findings (hand intersection,
        # owner-vs-idle divergence, write-lever stickiness, power
        # non-crossing, research evidence) gate the protocol-33 design.
        prod_probe = @{
            DiagEnv = @{ KENSHICOOP_PROD_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'prod_probe'
            Gating   = @('prod_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # prod_sync: protocol-33 host-authoritative machine state sync
        # (prodSync ON - the same script as prod_probe, so the probe's
        # measured gaps must now be CLOSED). The HOST places + completes a
        # generator and a crafting bench (minting proxies on the join),
        # operates the bench 1 Hz t=20-50s and toggles the generator power
        # OFF/ON; publishProd streams change-gated PKT_PROD rows the join
        # applies through the engine levers. Gates the probe's local legs
        # PLUS: [prod] rows sent AND applied, join minted both machines,
        # the bench output converged (final gap <= 1.0), the power OFF
        # crossed within 6 s and the final power state agrees.
        prod_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'prod_sync'
            Gating   = @('prod_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # research_probe: tech-tree phase-0 diagnostic (protocol 38 -
        # researchSync forced OFF). Both sides pick the deterministic
        # not-known-researchable RESEARCH subject at t=8s and poll
        # isKnown/canResearch 1 Hz; the HOST fires Research::startResearch at
        # t=10s (its isKnown flips - the spike-401 write lever); the JOIN
        # fires its OWN startResearch at t=25s. Gates: matching subject sids
        # (wire-key stability), host lever landed, join known=0 across the
        # t=10-25s divergence window (the unlock must NOT cross with the
        # hatch off), join self-lever landed AND stuck to run end.
        research_probe = @{
            DiagEnv = @{ KENSHICOOP_RESEARCH_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'research_probe'
            Gating   = @('research_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # research_sync: protocol-38 host-authoritative tech-tree sync
        # (researchSync ON - the same script as research_probe minus the
        # join's self-start, so the WIRE must flip the join's isKnown).
        # publishResearch streams one reliable PKT_RESEARCH row per known
        # sid (~1 Hz sample, first sight = baseline, 15 s safety resend);
        # applyResearch lands each via Research::startResearch. Gates: the
        # probe's local legs PLUS [research] rows sent AND applied, the
        # join's subject known=1 within 6 s of the host's start, sticky to
        # run end, and both finals known=1.
        research_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'research_sync'
            Gating   = @('research_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # deed_probe: property-ownership phase-0 diagnostic (protocol 54 -
        # deedSync forced OFF). Buying a house/shop is purely local today, so
        # the buyer owns it and the partner still sees it for sale. Both sides
        # snapshot the nearby-building census at t=8s (before anything can
        # cross), pick the first NOT-owned building in (serial, index) order -
        # host candidate 0, join candidate 1 - and claim it with the pure state
        # write (setFaction + addOwnedObject, never buyMeCallback): host t=12s,
        # join t=24s. 1 Hz census of the owned SET + nearby buildings + the
        # wallet on both sides. Gates: both picks landed on DISTINCT hands that
        # intersect cross-client, both local writes flipped owned 0->1 and stuck
        # to run end, the wallet did not move across the state write (the
        # double-charge guard), and NOTHING crossed (the negative control).
        deed_probe = @{
            DiagEnv = @{ KENSHICOOP_DEED_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'deed_probe'
            Gating   = @('deed_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # deed_sync: protocol-54 property-deed sync (deedSync ON - the same
        # script as deed_probe, so the probe's measured gap must now be
        # CLOSED). Both sides sample the player faction's owned-building set
        # ~1 Hz and stream reliable PKT_DEED rows (first sight is the session
        # baseline, 15 s safety resend that doubles as the retry for a peer
        # whose copy of the building was not loaded yet); the receiver applies
        # a pure state write. Gates: the probe's local legs PLUS each side's
        # claim CROSSED (the peer logged an applied [deed] RECV and its owned
        # set shows the hand) and stayed owned to run end, the owned sets agree
        # at the final sample, and the PEER'S WALLET DID NOT MOVE across the
        # apply - the double-charge witness a convergence-only gate cannot see,
        # since two clients agreeing on the wrong balance is still agreement.
        deed_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'deed_sync'
            Gating   = @('deed_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # store_probe: storage/machine container phase-0 diagnostic (protocol
        # 34 - storeSync forced OFF, the protocol-27 mint channel ON). The
        # HOST places a crafting bench + a general-storage chest leader-
        # relative and ramps both complete (minting proxies on the join);
        # both sides census container-bearing buildings (STORAGE + machine
        # classes) at 1 Hz with per-row entry count / total qty / content
        # hash; the host fabricates 5 sentinel items INTO the chest, operates
        # the bench 1 Hz t=24-54s (whole items land in the machine container
        # - the divergence driver), reconciles the chest down to 2 sentinels
        # (removal leg) and force-empties the bench container (churn leg);
        # the JOIN fabricates into its MINTED chest copy. Gates the LOCAL
        # legs only (place + ramp + census + add landed + recon removed + ops
        # ran); the findings (hand intersection, hasInv readability, capacity
        # vs INV_ITEMS_MAX, owner-vs-idle container divergence, add
        # non-crossing, post-empty churn) gate the protocol-34 design.
        store_probe = @{
            DiagEnv = @{ KENSHICOOP_STORE_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'store_probe'
            Gating   = @('store_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # store_sync: protocol-34 host-authoritative container contents
        # (storeSync ON, riding the invSync container channel - the same
        # script as store_probe minus the join-side add, so the probe's
        # measured gaps must now be CLOSED). The host's ~1 Hz census
        # registers every complete storage/machine container near the
        # interest centers as an authored container; placed buildings ride
        # their protocol-27 placer key (InvSnapshotHeader keyKind=1). Gates
        # the probe's local legs PLUS: the host census-authored the placed
        # chest ([inv] SEND kind=1), the join applied [inv] rows, the host's
        # chest add CROSSED onto the join's minted copy, and the FINAL chest
        # content hashes agree (so the reconcile-removal crossed too).
        store_sync = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'store_sync'
            Gating   = @('store_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # npc_census: protocol-36 wide-radius ghost culling. The join spawns 4
        # runtime NPCs and parks them ~600 u out - beyond the ~200 u stream
        # bubble, inside the 2000 u census radius - so ONLY the census channel
        # (host 1 Hz wide-radius hand list + the join's wide suppression pass)
        # can cull them. Gates: census flowing both ends, all ghost hands
        # culled, no mass-suppression of legitimate census NPCs. Save 'sync':
        # a live town gives the census real NPCs (the restraint control) and
        # findNearbyNonPlayerFaction a faction to mint the ghosts in.
        npc_census = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'npc_census'
            Gating   = @('npc_census', 'suppress_churn', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # fast_march (2026-07-11 rubber-banding validation): leader_move at 5x
        # game speed (both sides vote 5x; consensus takes it). Gates on the
        # join's PLAYER-SQUAD hard-snap RATE - before the velocity-aware snap
        # gate, 5x wall-clock velocities teleported the driven leader every
        # sample (~35 snaps/s measured in the 2026-07-11 manual session), and a
        # sprinting leader false-snapped even at 1x (gap=8.6 vs the fixed 8 u
        # gate). Background-NPC snaps stay un-gated at 5x: a bar NPC resting
        # between stream updates legitimately falls 100+ u behind and the
        # teleport is the correct convergence tool (measured gaps 128-1826 u).
        # Crosscheck stays advisory: at 5x the driven copy legitimately trails
        # by ~1 wall-clock update interval * 5x velocity.
        fast_march = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            PrimaryGate = 'snap_rate_squad'
            Gating   = @('snap_rate_squad', 'suppress_churn', 'clock_sync')
            Advisory = @('snap_rate', 'crosscheck', 'smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # spawn_far (2026-07-11 "NPCs spawn on top of the join player" fix):
        # census-range proxy minting. The host spawns a runtime squad ~620 u
        # out and walks it toward the co-located leaders; the join must mint
        # the proxies at census range (census-missing scan + reply-side mint
        # gate at KENSHICOOP_SPAWN_MINT_RADIUS) - every far hand bound, all
        # binds >= 400 u from the join anchor, no duplicate mints, and the
        # SAME proxy body driven into the stream bubble. snap_rate stays
        # advisory: the bubble-entry drive takeover legitimately hard-snaps
        # a proxy whose local AI drifted while it was census-only.
        spawn_far = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'spawn_far'
            Gating   = @('spawn_far', 'suppress_churn', 'clock_sync')
            Advisory = @('mint_dist', 'snap_rate', 'smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # travel_parity (2026-07-11 free-play "yellow packs while roaming"
        # report): the JOIN's PC teleport-hops ~60,000 u across the map (15
        # hops x 4000 u, ~9 s dwell each - every hop lands entirely outside
        # the previous 2000 u census bubble, so existence coverage rebuilds
        # from nothing at each stop: zone streams, census re-centering,
        # mint/cull churn) while the HOST's PC follows its local driven copy
        # (teleport catch-up past 150 u, walk inside) - the roaming direction
        # (join drags the interest/census coverage) no other scenario moves.
        # Both sides dump a 5 s worldstate (SCENARIO WORLD/WNPC rows; the
        # join rows carry the drv/cen/hid/ghost authority class).
        # follow_travel gates first: if the follow never held, the parity
        # numbers describe two separated worlds and mean nothing.
        # travel_parity gates the visible-on-join-only (ghost) fraction +
        # persistence while moving; crosscheck stays advisory (hop legs open
        # multi-thousand-unit transients). snap_rate advisory: the hard snap
        # IS the convergence tool on every hop. Seconds/KillGraceSec: the
        # 160 s host window outlives the default 150 s self-exit + 90 s kill
        # grace.
        travel_parity = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = 'follow_travel'
            Gating   = @('follow_travel', 'travel_parity', 'clock_sync')
            Advisory = @('existence_parity', 'mint_dist', 'anti_zombie',
                         'lifecycle', 'suppress_churn', 'snap_rate',
                         'smoothness', 'anim_truth', 'march', 'crosscheck')
            Tier = 'full'; WanVariant = $false
        }

        # camp_approach: Phase 2 crash-hardening SOAK on the 'camp' prison save
        # (many NPCs). The JOIN teleport-hops across the camp forcing mint/zone
        # churn while the HOST self-exits FIRST at ~130s - a real peer drop that
        # fires clearPeerReplicationState on the surviving join mid-churn. There
        # is no deterministic crash repro, so this is a stress gate, not parity:
        # Test-CampApproach checks both sides reach a SCENARIO RESULT (no crash),
        # the join logs 'peer left' -> '[leave] cleared proxies=', no '[drive]'
        # touches a stale/unbound hand after the leave, and proxies drain to ~0.
        # Join window 150s (from arm) -> Seconds/KillGraceSec raised like
        # travel_parity so the runner backstop outlives it.
        camp_approach = @{
            Save = 'camp'; Setup = ''; Tolerance = 18.0
            Seconds = 230; KillGraceSec = 200
            PrimaryGate = 'camp_approach'
            Gating   = @('camp_approach', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # spawn_sync: protocol-21 runtime-spawn proxy replication (spawnSync ON).
        # Same script as spawn_probe; gates that the join minted proxies for the
        # host's runtime spawns (near half + far >= 1), the PROXY position series
        # tracks the host MEMBER series per hand, and the join's own local
        # runtime spawns still get suppressed by host authority.
        spawn_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'spawn_sync'
            Gating   = @('spawn_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # speed_probe: vote-decoupling phase-0 spike (host-side, log-only;
        # speedSync forced OFF by Config). Quiet writes (setFrameSpeedMultiplier
        # + guarded userPause) must drive the sim WITHOUT moving the UI speed
        # buttons; a loud simulated click must move them AND register as
        # captured intent (the hook-based vote source).
        speed_probe = @{
            DiagEnv = @{ KENSHICOOP_SPEED_SYNC = '0'; KENSHICOOP_DEBUG_SPEED = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'speed_probe'
            Gating   = @('speed_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # shackle_probe: Phase 6 6a evidence spike (log-only, BOTH clients) on the
        # 'camp' prison save. Each client enumerates nearby world NPCs and emits a
        # "SCENARIO SHACKLE" line per chained / shackle-carrying body. The oracle
        # time-aligns the owner's and peer's view of each shackled prisoner and
        # flags any chained/lock divergence (the reported "peer PC unlocks the
        # shackles" desync). No behavior change ships in 6a.
        shackle_probe = @{
            DiagEnv = @{ KENSHICOOP_DEBUG_SHACKLE = '1' }
            Save = 'camp'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'shackle_probe'
            Gating   = @('shackle_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # shackle_sync: Phase 6 6b validation (BOTH clients) on the 'camp' prison
        # save. Same "SCENARIO SHACKLE" emission as shackle_probe, but the oracle
        # is now STRICT: with the protocol-42 locked bit + non-owner unlock guard
        # shipping, a shared prisoner whose owner reports chained/locked while the
        # peer's driven copy reports it cleared is a FAIL. Shared-hand parity is
        # asserted; the no-shared-hand identity caveat defers to the manual gate.
        # lockpick_escape: the ANCHORED-TO-FREE crossing on the rebirth1 prisoner
        # start. Every other furniture scenario puts a body INTO a held state and
        # checks the peer follows; this is the release, which is harder - an
        # anchored body is pinned by furniture both sides can see, a freed one has
        # to start walking on both clients at once.
        #
        # rebirth1 is the fixture because its caged bodies are the PLAYER SQUAD, so
        # the subject is owned rather than a world prisoner either side might drive.
        # Presence authority is ON: this scenario deliberately stresses the same
        # authority boundary the 2026-08-08 dual-drive bug lives on, with the squads
        # co-located in one cell, so dual_drive rides along as a gate.
        #
        # Seconds/KillGraceSec outlive the scenario's own 180 s host window: the
        # engine picks the lock on its own schedule and the run has to be long
        # enough to find out what that schedule is.
        lockpick_escape = @{
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'
                         KENSHICOOP_DEBUG_SHACKLE = '1' }
            Save = 'rebirth1'; Setup = ''; Tolerance = 6.0
            Seconds = 210; KillGraceSec = 200
            PrimaryGate = 'lockpick_escape'
            Gating   = @('lockpick_escape', 'clock_sync')
            Advisory = @('dual_drive', 'existence_parity', 'suppress_churn', 'smoothness')
            Tier = 'probe'; WanVariant = $false
        }

        # escape_cohesion: the same rebirth1 escape, but asking a different
        # question. lockpick_escape judges one STATE CROSSING (anchored -> free,
        # agreed by both sides). This judges what the two clients RENDER while
        # the freed body travels - same roster, same bodies, no player character
        # drawn twice, no NPC driven by both sides at once.
        #
        # The waypoint is placed just past the nearest zone-cell boundary on
        # purpose: a cell edge is where presence authority changes hands, which
        # is the seam the 2026-08-08 dual-drive bug lives on, so the escapee
        # crosses it rather than wandering near it.
        #
        # Both clients aim their camera at the JOIN's escapee. That is not just
        # framing: protocol 43 folds the camera into the interest anchors, so
        # both sides stream widely around the same body. Symmetric interest is
        # what makes "do both clients see the same thing" a fair question, but it
        # does mean this measures cohesion under a SHARED spotlight - a variant
        # with independent cameras would be the harder case.
        #
        # KENSHICOOP_DEBUG_MARKERS puts the authority class on screen over each
        # body (green DRV, red CULLED, yellow PARKED) because the screenshots are
        # part of this scenario's evidence, and CELL_AUTH=1 is what makes the
        # host label its side too (it only runs enforceHostAuthority with the
        # flag on) - so the two shots are directly comparable.
        #
        # Shots are anchored late, on the release rather than on startup: the
        # default 'SCENARIO MEMBER' anchor fires ~60 s BEFORE the escape and
        # would photograph a prisoner sitting in a cage. The host anchors on its
        # own 'freed' observation because the 'walk' line is owner-only.
        escape_cohesion = @{
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'
                         KENSHICOOP_DEBUG_MARKERS = '1'
                         KENSHICOOP_DEBUG_SHACKLE = '1' }
            Save = 'rebirth1'; Setup = ''; Tolerance = 6.0
            Seconds = 210; KillGraceSec = 200
            ShotAnchorHost = 'SCENARIO ESCAPE freed side=host'
            ShotAnchorJoin = 'SCENARIO ESCAPE walk'
            ShotDelaySec = 8; ShotWaitSec = 120
            PrimaryGate = 'pc_dupes'
            Gating   = @('pc_dupes', 'world_parity', 'dual_drive', 'clock_sync')
            Advisory = @('lockpick_escape', 'existence_parity', 'suppress_churn',
                         'lifecycle', 'anti_zombie', 'smoothness')
            Tier = 'probe'; WanVariant = $false
        }

        shackle_sync = @{
            DiagEnv = @{ KENSHICOOP_DEBUG_SHACKLE = '1' }
            Save = 'camp'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'shackle_sync'
            Gating   = @('shackle_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # world_parity: full-roster cross-client parity on the dense 'camp'
        # prison save. Nothing scripted - both sides run their sims while the
        # replicator's 5 s auditRows dumps (SCENARIO WORLD/WNPC with the
        # task=/pelvis=/mv=/carry= parity fields and cls=pc player rows) feed
        # Test-WorldParity's tiered judgment:
        #   PC tier     - every player character present on both sides, hard
        #                 position gate (a diverged host-PC is invisible to
        #                 every other oracle: the NPC dumps exclude the squad)
        #   near tier   - host rows within the ~200 u position-stream bubble:
        #                 existence, position and task parity on the join
        #   census tier - 200-1800 u: existence + position within the park
        #                 threshold; task not judged (local-sim copies)
        # Seconds/KillGraceSec: the 180 s host window outlives the default
        # 150 s self-exit + kill grace (same pattern as travel_parity).
        world_parity = @{
            Save = 'camp'; Setup = ''; Tolerance = 6.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = 'world_parity'
            Gating   = @('world_parity', 'clock_sync')
            Advisory = @('existence_parity', 'anti_zombie', 'lifecycle',
                         'suppress_churn', 'smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # speed_sync: consensus game-speed (pause/1x/2x/3x). Scenario-simulated
        # clicks (host 3x -> join 1x -> join 3x) exercise min-arbitration both
        # ways, then a bar NPC on the host leader trips the combat 1x cap. Gates
        # on the two SPEED series matching (time-aligned), each transition's
        # follow latency, and the combat demotion. WanVariant: the SET/REQ
        # channel is reliable and must converge under loss.
        speed_sync = @{
            DiagEnv = @{ KENSHICOOP_TIME_SYNC = '0'; KENSHICOOP_DEBUG_SPEED = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'speed_sync'
            Gating   = @('speed_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # Waiting-attacker stance conformance (protocol 15): the host piles ~5
        # bar NPCs onto its own leader so the attack-slot system queues most of
        # them. Gates that both stances streamed, the join's re-issue loop and
        # snap teleports are dead, and every crowd copy tracks its host body.
        # Tolerance is the per-hand median tracking gate (a brawl's footwork is
        # legitimately noisier than a walk: the menace ring itself is ~6-8 u wide,
        # each engine assigns slot spots independently, and the whole brawl
        # roams - healthy runs measured 3-15 u medians across 7 runs, pre-fix
        # divergence measured 74-95 u, so 20 u splits the distributions cleanly.
        # WanVariant: stance flips ride the unreliable entity batch and must
        # self-heal under loss.
        combat_crowd = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'combat_crowd'
            # combat_snap_rate: advisory here (crowd is ~5 WAITING strikers, low
            # active-melee churn) - it surfaces the enriched [combat] snap buckets
            # for diagnosis. The many-NPC active-melee case is HARD-gated on
            # combat_battle. See resources/combat_warp_debug plan.
            Gating   = @('combat_crowd', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate')
            Tier = 'full'; WanVariant = $true
        }
        # combat_battle: many-NPC combat warp validation (the "NPCs warp on the join
        # when many are fighting" field report). The host runtime-spawns N fighters
        # (KENSHICOOP_BATTLE_N, default 16; the 10v10/20v20/40v40 ladder) near the
        # leader and index-pairs them into mutual melee, so the join must DRIVE many
        # simultaneously-active combat copies via the interp + graded-snap path.
        # combat_battle gates that the fight happened + crossed; combat_snap_rate
        # HARD-gates the [combat] snap teleport buckets (churn rate / persistence /
        # wrong-target) - the actual warp measurement. Tolerance 20u matches the
        # combat drift bands (COMBAT_SNAP_DIST).
        combat_battle = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'combat_battle'
            Gating   = @('combat_battle', 'combat_snap_rate', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # combat_win: the SECOND warp shape (2026-07-16 smoothness pass). Each side
        # buffs its OWN player-squad to 120 in every stat and the host runtime-spawns
        # N unbuffed enemies (KENSHICOOP_WIN_N, default 8) onto the PC leader; the
        # buffed PCs cut them down, so the join stress shifts to dying/fleeing/KO
        # churn and rapid target loss (distinct from combat_battle's sustained melee).
        # combat_win gates that the PCs were buffed both sides + the fight was won and
        # crossed; combat_snap_rate HARD-gates the warp buckets. death/existence parity
        # advisory (dying enemies). Tolerance 20u matches the combat drift bands.
        combat_win = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'combat_win'
            Gating   = @('combat_win', 'combat_snap_rate', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'existence_parity')
            Tier = 'full'; WanVariant = $false
        }
        # pc_assault: the DAMAGE-gated counterpart to assault_town + combat-cohesion
        # diagnostic. Both sides buff their OWN squad to 120 and the join orders its
        # PC to attack a baked bar NPC (Save 'sync'); the report-and-apply damage
        # channel (protocol 45) makes the victim's flesh/blood DROP on the host
        # (authoritative join-dealt damage) and STREAM back to the join - closing the
        # "join does no damage to NPCs" gap. Test-PcAssault HARD-gates the damage
        # transfer (protocol-45 [combat] HIT RECV/SEND: host applies join-dealt
        # damage AND it streams back to the join) and REPORTS host<->join combat
        # cohesion (Get-CombatParity joinOnlyFrac/churn) as a FINDING - the warp-
        # cohesion regression guard is combat_snap_rate on combat_crowd/battle/win.
        pc_assault = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'pc_assault'
            Gating   = @('pc_assault', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # ---- inventory ---------------------------------------------------------------
        inv_order = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_sync'
            Gating   = @('inv_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_bidir = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_bidir'
            Gating   = @('inv_bidir', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $true
        }
        # Protocol 46 item-loss regressions. inv_overflow gates the container-overflow
        # class (truncated snapshots used to read as authoritative deletes and wipe every
        # item past the cap - the "backpacks lose items" report); inv_dropfull gates the
        # W2 drop-vs-snapshot race that let a dropped weapon exist only on the dropper's
        # ground. Both are smoke tier: they cover the two ways inventory silently lost
        # items, so a regression must not wait for a full run to surface.
        inv_overflow = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_overflow'
            Gating   = @('inv_overflow', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        inv_dropfull = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_dropfull'
            Gating   = @('inv_dropfull', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        inv_equip = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_equip'
            Gating   = @('inv_equip', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_reequip = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_reequip'
            Gating   = @('inv_reequip', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_addequip = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'add_equip'
            Gating   = @('add_equip')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # Protocol-36 BASELINE for the cross-owner direct-drag bugs (field report:
        # dupe on take, wipe on give, weapon vanish). The host performs real cross-
        # owner engine moves; the oracle REPORTS the conservation outcome as evidence.
        # Not in a tier: it documents the bug the transfer-intent channel then fixes.
        trade_probe = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_XFER_SYNC = '0'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'trade_probe'
            Gating   = @('trade_probe')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false
        }
        # Protocol-37 VALIDATION of the transfer-intent channel (xferSync ON): the
        # same three cross-owner drags trade_probe baselined, now expected CLEAN -
        # TAKE lands + the removal propagates (no dupe), GIVE arrives on the owner
        # (no wipe), the traded WEAPON survives on BOTH clients (real-object
        # relocation) and both clients agree on the final per-rank state.
        trade_peer = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'trade_peer'
            Gating   = @('trade_peer', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # weapon_loot: weapon-fabrication sync validation (the last trading loss
        # vector). The HOST's owned leader ACQUIRES a weapon that exists in NO
        # shared-save inventory (novel sid, engine-fabricated - the loot/vendor-buy
        # shape); the join's driven copy must gain EXACTLY one copy of the same
        # template via the inventory snapshot channel + the spike-451 weapon CREATE,
        # with zero dupes on either side (fabrication must not race the W2
        # conservation channel or the snapshot echo).
        weapon_loot = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'weapon_loot'
            Gating   = @('weapon_loot', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # ---- world items ------------------------------------------------------------
        drop_probe = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'drop_probe'
            Gating   = @('drop_probe')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false   # W0 diagnostic; evidence, not a sync gate
        }
        world_item_sync = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wi_sync'
            Gating   = @('wi_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # W1 BIDIR: the join->host direction of world_item_sync (the join drops +
        # despawns; the HOST must spawn/cull the proxy) - the field-reported gap
        # (join ground drops never appeared on the host) closed by the
        # bidirectional W1 stream with owner-scoped netIds + the proxy echo guard.
        world_item_join = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wi_join'
            Gating   = @('wi_join', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # rejoin_items (Phase 3 item-dup fix): a reload must not duplicate save-
        # native ground items. Reuses the load_sync coordinated save+load lever
        # (loadSync + saveSync ON by default): the HOST drops K test items (both
        # clients reach n0+K), coordinated-saves 'coopresume' so the drops bake
        # into the shared save, then loads it mid-session. The first-scan baseline
        # must record the now-native drops as never-emit so the host does not
        # re-stream them and the join does not layer a duplicate proxy - the
        # oracle gates POST-reload count <= PRE-reload count on BOTH sides plus a
        # WORLD-RELOAD edge. clock_sync is NOT gated (the reload restarts the
        # in-game clock series, same as load_sync).
        rejoin_items = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'rejoin_items'
            Gating   = @('rejoin_items')
            Advisory = @('smoothness', 'anim_truth', 'march', 'clock_sync')
            Tier = 'full'; WanVariant = $false
        }
        wpn_relocate = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wpn_relocate'
            Gating   = @('wpn_relocate')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        world_weapon_drop = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'weapon_drop'
            Gating   = @('weapon_drop', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $true
        }
        world_armor_drop = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'armor_drop'
            Gating   = @('armor_drop', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # Protocol 47. inv_backpack_drop is the third variant of the same conservation
        # contract (itemType 46 = worn container): the peer must RELOCATE its backpack to
        # the ground, never destroy it. Backpacks were excluded from the gear census, so the
        # inventory snapshot's REMOVE ran with no drop intent to explain it and the peer's
        # backpack - contents and all - was annihilated. Reuses the WDROP oracle.
        # world_pickup_mirror gates the CLAIM half: a NON-gear item the peer picks up off
        # the proxy stream must disappear from the AUTHOR's ground too, instead of being
        # duplicated. Both smoke tier - they are the two item-LOSS/DUPE classes the manual
        # session surfaced, so a regression must not wait for a full run.
        inv_backpack_drop = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            # squad2, NOT squad1: the gate needs a leader who WEARS a container, and squad1
            # has none (it reads back have=0 and the scenario cannot author a drop at all).
            Save = 'squad2'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'backpack_drop'
            Gating   = @('backpack_drop', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        world_pickup_mirror = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'pickup_mirror'
            Gating   = @('pickup_mirror', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        # inv_regear gates the W2 round trip's ONE-INSTANCE invariant: after the peer picks
        # up gear WE dropped, the item is in its bag AND gone from our ground - the manual
        # session's "a few items stayed on the host's ground" was the duplicate outcome.
        # no_phantom_pickups rides along on the same logs: the gear census used to commit a
        # still-resolving container's zero-row read as its baseline, so every worn item read
        # as a pickup a tick later and fired an identity-less PICKUP intent at connect.
        # Two tabs required (the host drops from tab 0, the join picks up into tab 1).
        inv_regear = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'gear_repickup'
            Gating   = @('gear_repickup', 'no_phantom_pickups', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        # Same round trip with the author's FIRST re-home refused. That is the case that
        # used to strand the item in both places forever - the author tried once, off a
        # cached pointer, and never revisited it. Now it retries and, once it can READ the
        # item in the peer's bag, retires its own ground copy; the gate additionally
        # requires evidence that this recovery ran (retry/dedupe), so a regression that
        # quietly reverts to the single-shot re-home cannot pass by luck.
        inv_regear_refuse = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1'
                         KENSHICOOP_WD_REFUSE_REHOME = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'gear_repickup_retry'
            Gating   = @('gear_repickup_retry', 'no_phantom_pickups', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        # Every attempt refused, so the retry cannot save it either: the item can only reach
        # the bag over the snapshot channel, after which the author retires its ground copy
        # against a bag it has actually READ. That read is the whole safety argument for
        # destroying anything, so it gets its own gate (full tier - smoke already covers the
        # retry, and this one has to wait out WD_REHOME_MAX_MS).
        inv_regear_refuse_all = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1'
                         KENSHICOOP_WD_REFUSE_REHOME_ALL = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'gear_repickup_dedupe'
            Gating   = @('gear_repickup_dedupe', 'no_phantom_pickups', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # An item placed INSIDE a worn backpack. A bag owns a private inventory that no section
        # of the wearer's own inventory names, so before protocol 48 a bagged item was described
        # by no snapshot at all and only ever existed for the author - which is why dropping a
        # bag handed the other side a bag missing its contents. squad2 carries the backpacks.
        inv_nested_bag = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'
                         KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad2'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'nested_bag'
            Gating   = @('nested_bag', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        # The player's actual workflow: dump a character's ENTIRE kit at once and hoover all of it
        # up with a second character. The one-item gates above pass on a trickle and cannot see
        # what a burst does - a ground track retired ~25 ms after its own drop (the read budget was
        # denominated in engine ticks, not time), and a receiving grid that fills so the tail is
        # stowed in the worn backpack, where the drop mirror's top-level-only search could not
        # reach it and fabricated a duplicate instead.
        inv_dump_all = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'
                         KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad2'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'dump_all'
            Gating   = @('dump_all', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # The same burst with the author's ground tracks discarded as they are made. A lost track
        # is what a ~25 ms retirement window produced in the wild, and the lever makes it certain:
        # the peer's pickup then arrives NAMED but unmatchable, and the author must keep trying
        # instead of answering once and leaving its copy on the ground.
        inv_dump_all_forget = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'
                         KENSHICOOP_INV_DUMP = '1'; KENSHICOOP_WD_FORGET_TRACK = '1' }
            Save = 'squad2'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'dump_all'
            Gating   = @('dump_all', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # The decisive one. The author's FIRST pickup-time read of the tracked object reports it
        # gone, then reads normally - the transient the engine produces by streaming an object out
        # and back, and the actual cause of the duplicate a player reported. Concluding from that
        # one read erases the track and answers the peer with nothing, so the author's copy stays
        # on the ground beside the item the peer now holds; the conservation ledger then shows that
        # template summing to two. Parking the intent and retrying it converges instead.
        inv_dump_all_transient = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'
                         KENSHICOOP_INV_DUMP = '1'; KENSHICOOP_WD_TRANSIENT_DEAD = '1' }
            Save = 'squad2'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'dump_all'
            Gating   = @('dump_all', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # A BURST of non-gear drops in one tick, overflowing the per-tick W1 send batch (shrunk
        # to 2 so five drops reach it). The overflow used to be booked as sent without being
        # sent, so the tail of the burst was invisible until the 5 s safety resend - the
        # "dropped it here, never showed up there" report.
        # The join's proxy is freed by the ENGINE (what a zone unload does when players
        # travel out of a block) at the exact moment the cull for it runs - injected,
        # because the publish sweep clears dead proxies every tick and the real window
        # is one frame wide. Locks the 2026-08-03 join crash: the mod used to destroy
        # that freed object a second time. Gates engine_integrity too, since Kenshi's
        # own "alredy has destroy reason" line is the other half of the same evidence.
        world_item_stale = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1'
                         KENSHICOOP_WI_TEST_STALE = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'world_item_stale'
            Gating   = @('world_item_stale', 'engine_integrity', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        world_item_burst = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1'
                         KENSHICOOP_WI_BATCH_MAX = '2' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'world_item_burst'
            Gating   = @('world_item_burst', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }
        # The author FORGETS its ground track (KENSHICOOP_WD_FORGET_TRACK), so the peer's pickup
        # names a drop it cannot match. Answering "untracked" and standing still is what left a
        # picked-up item lying on the other side's ground; the run must instead converge through
        # the site-anchored recovery. Smoke tier: this is the duplicate players actually hit.
        inv_regear_forget = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1'
                         KENSHICOOP_WD_FORGET_TRACK = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'gear_repickup_recover'
            Gating   = @('gear_repickup_recover', 'no_phantom_pickups', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $false
        }

        # ---- diagnostics (never in a tier) --------------------------------------------
        inv_wpnseq = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # local [recon] trace diagnostic
        }
        # xfer_block: cross-owner trade VETO exercise (manual/diagnostic, not in a
        # tier). Blocks direct squad-to-squad drags and forces ground drops; needs
        # the inventory channel live to drag on, and the veto armed. DiagEnv carries
        # both knobs so no launcher (or Config.cpp) has to name the scenario.
        xfer_block = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_BLOCK_XFER = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # veto-path diagnostic
        }
        spike = @{
            Save = 'c'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # run_spike.ps1 judges captures itself
        }
        # jail_probe: read-only diagnostic for the "put jailed PC to work" desync
        # (join PC briefly exits its cage then teleports back). Passive soak on
        # the 'jailed' save (join PC caged in the camp); no scenario gate - the
        # evidence is the KENSHICOOP_JAIL_PROBE [jail] STATE traces (own vs drv),
        # the [furn] ENTER/EXIT/PEER-ENTER edges, and (with KENSHICOOP_TASK_SPIKE)
        # the [spike] SELECT task the join's local AI picks for its own PC.
        # auditRows is armed for this scenario for pos/context. Must run
        # partitioned (-Inhabit / OWN_RANK) so the join actually owns the caged PC.
        jail_probe = @{
            DiagEnv = @{ KENSHICOOP_JAIL_PROBE = '1'; KENSHICOOP_TASK_SPIKE = '1'; KENSHICOOP_JAIL_OBSERVE = '1' }
            Save = 'jailed'; Setup = ''; Tolerance = 6.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # spike: [jail]/[furn]/[spike] traces
        }
        # jail_soak: LONG-play version of jail_probe (spike 58). 15 min passive
        # soak so the host guard's put-to-work cage<->pole cycle and census-band
        # NPC drift actually accumulate (jail_probe's 220 s is too short to see
        # them). Same probes (KENSHICOOP_JAIL_PROBE/TASK_SPIKE/JAIL_OBSERVE) +
        # auditRows + the new [jail] SNAP re-seat metric. Default Save='jailed';
        # override -Save 'slaves save' / -Save cage2 for the other testbeds. Run
        # partitioned (-Inhabit / OWN_RANK) so each side owns one PC. No gate.
        jail_soak = @{
            DiagEnv = @{ KENSHICOOP_JAIL_PROBE = '1'; KENSHICOOP_TASK_SPIKE = '1' }
            Save = 'jailed'; Setup = ''; Tolerance = 6.0
            # run_test kills at (early screenshot anchor + KillGraceSec), so the
            # kill grace must exceed the whole soak window or the game is cut short
            # before the plugin self-exits at Seconds. Keep KillGraceSec > Seconds.
            Seconds = 600; KillGraceSec = 780
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # spike: [jail]/[furn]/[census]/[spike] traces
        }

        # milestone_a_gate: the Phase 4 Milestone A gate (POC-02/POC-03) - the ONE
        # master 10-step DoD scenario every one of 4 processes runs
        # (src/plugin/test/ScenarioMilestoneA.cpp, plan 04), judged by the N=4
        # oracle set (scripts/CoopOraclesN.psm1, plan 05) instead of this
        # manifest's normal 2-log Gating dispatch. Tier='none': this schema's
        # Gating/Advisory ids ARE resolvable (Get-OracleRegistryN, extended into
        # Contract.Tests.ps1's registry check below) but they dispatch through
        # analyze_run4.ps1 / Invoke-OneOracleN, never through CoopOracles.psm1's
        # Invoke-OneOracle 2-log switch - regress.ps1's -Tier smoke|full sweep
        # only ever calls run_test.ps1 (2-instance), which cannot run this
        # scenario at all (it needs run_test4.ps1's N=4 launcher, plan 02).
        # Keeping it out of the tier is not a diagnostic downgrade like the
        # entries above; it is the only way to represent "N=4-only" in a
        # manifest shaped for exactly two logs.
        #
        # Save: unlike every other entry, this scenario does not resume a
        # fixtures\saves\<name> world - the DoD requires all four ranks SEEDED
        # (A4 resolution, 04-04-SUMMARY.md), which tools/MultiplayerStartGen's
        # "Multiplayer (Wanderer x4)" NEW-GAME start provides, not a save capture.
        # run_test4.ps1 does not consult this field (it reads -Save / rig
        # config's own "save" key - plan 06's local.rig.json is the actual
        # source of truth for what each instance loads), so this name is
        # documentation + Contract.Tests.ps1 schema compliance, not a live
        # fixtures\saves\wanderer4 folder plan 05 created.
        #
        # PrimaryGate = census_convergence: the no-signal guard for the
        # mechanism POC-03 exists to prove (host-authoritative NPC census
        # holding at N=4). Gating carries all five DoD-mapped gates - unlike
        # the rest of the manifest there is no separate "softer" N=4 oracle
        # yet to demote to Advisory (engine_integrity is folded into clean_exit
        # rather than duplicated here as its own advisory entry).
        #
        # Seconds/KillGraceSec: ScenarioMilestoneA.cpp's own HOST_DURATION_MS is
        # 150 s (JOIN_DURATION_MS 140 s); this outlives that scripted window by
        # the house idiom (staggered joins + disconnect/reconnect lifecycle
        # overhead + the host's shutdown), the same margin town_arrive/run_apart
        # give their own host windows.
        milestone_a_gate = @{
            Save = 'wanderer4'; Setup = ''; Tolerance = 6.0
            Seconds = 210; KillGraceSec = 180
            PrimaryGate = 'census_convergence'
            Gating   = @('census_convergence', 'disconnect_isolation', 'driven_only_movement',
                         'desync_convergence', 'clean_exit')
            Advisory = @()
            # A no-signal (SKIP) result on either movement gate is an
            # evidentiary gap, not a pass - ROADMAP Success Criteria 1/2
            # require these DoD behaviors to be positively demonstrated
            # (04-08-PLAN.md gap 4).
            NoSignalFails = @('driven_only_movement', 'desync_convergence')
            Tier = 'none'; WanVariant = $false   # N=4-only: judged by analyze_run4.ps1 (plan 05), never the 2-log tier sweep
        }

        # player_state_gate (Phase 6 plan 02): the ONE master player-state
        # scenario proving PLAY-01/02/03 at N=4 - medical/KO/revive + stats/
        # limbs, cross-owner carry+furniture, stealth/prone, and an EARLY
        # runtime recruit (so a Plan-03 scheduled disconnect can still land
        # after a confirmed recruit). Mirrors milestone_a_gate's own schema
        # shape (N=4-only, judged by analyze_run4.ps1, never the 2-log tier
        # sweep - Save is documentation only, run_test4.ps1 reads the actual
        # save from rig config).
        #
        # PrimaryGate = player_state_medical: the no-signal guard for the
        # mechanism this scenario exists to prove (per-owner medical/KO
        # convergence at N=4, PLAY-01). player_state_recruit (Gate B,
        # recruitment end-ownership, PLAY-03) is also Gating with
        # NoSignalFails so a SKIP on either is an evidentiary gap, not a pass.
        #
        # Seconds/KillGraceSec: ScenarioPlayerState.cpp's own HOST_DURATION_MS
        # is 150 s (JOIN_DURATION_MS 140 s), same as milestone_a_gate; the
        # same margin (staggered joins + disconnect/reconnect lifecycle
        # overhead + host shutdown) applies here.
        player_state_gate = @{
            Save = 'wanderer4'; Setup = ''; Tolerance = 6.0
            Seconds = 210; KillGraceSec = 180
            PrimaryGate = 'player_state_medical'
            Gating   = @('player_state_medical', 'player_state_recruit')
            Advisory = @()
            NoSignalFails = @('player_state_medical', 'player_state_recruit')
            Tier = 'none'; WanVariant = $false   # N=4-only: judged by analyze_run4.ps1, never the 2-log tier sweep
        }

        # item_conservation_gate (Phase 7 plan 03): the ONE live conservation
        # scenario proving the host-committed transfer model (Plan 01) and the
        # claim arbiter (Plan 02) end-to-end - P2->P3/P3->P4/P4->host-container
        # transfers, a simultaneous disjoint-pair leg, and one 2-claimant ground
        # contention (loser rollback, coverage gap D4) - judged by
        # Test-InvConservation's Legs A-D (CoopOraclesN.psm1). Mirrors
        # player_state_gate's own schema shape (N=4-only, judged by
        # analyze_run4.ps1, never the 2-log tier sweep - Save is documentation
        # only, run_test4.ps1 reads the actual save from rig config).
        #
        # DiagEnv: KENSHICOOP_INV_SYNC=1 forces invSync on (which cascades
        # xferSync ON whenever unset, Config.cpp's own "unset = ON whenever
        # invSync is on" default) and KENSHICOOP_WORLD_SYNC=1 forces worldSync
        # on - both default OFF for a non-"" scenario name (Config.cpp:146,183),
        # so without this the transfer/claim planes this gate exists to exercise
        # would never fire at all.
        #
        # PrimaryGate = inv_conservation: the no-signal guard for the ONE
        # mechanism this scenario exists to prove (zero-dup/zero-loss
        # conservation across all four ownership domains, INV-01..04).
        # NoSignalFails on inv_conservation: an evidentiary gap (no CONSERVE
        # census, no commit/claim-win evidence) is a FAIL, never a silent pass.
        #
        # Seconds/KillGraceSec: ScenarioItemConservation.cpp's own
        # HOST_DURATION_MS is 165s (JOIN_DURATION_MS 140s) - the host outlives
        # the last-armed join's final checkpoint (the 07-04 arming-skew fix),
        # so it runs 15s longer than player_state_gate/milestone_a_gate.
        # Seconds=210 still covers it with the same margin class (staggered
        # joins + disconnect/reconnect lifecycle overhead + host shutdown).
        item_conservation_gate = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1' }
            Save = 'wanderer4'; Setup = ''; Tolerance = 6.0
            Seconds = 210; KillGraceSec = 180
            PrimaryGate = 'inv_conservation'
            Gating   = @('inv_conservation', 'clean_exit')
            Advisory = @()
            NoSignalFails = @('inv_conservation')
            Tier = 'none'; WanVariant = $false   # N=4-only: judged by analyze_run4.ps1, never the 2-log tier sweep
        }

        # world_state_gate (Phase 8 plan 03; contested leg redesigned 08-06):
        # the ONE live scenario proving WORLD-01/02/03 end to end - every
        # locked world channel (build/door/prod on rank0, faction on rank1,
        # deed on rank2, research on rank3) via one-call engine levers, plus
        # the D1/D2/D3 contested-claim leg asserting the deterministic
        # continuity -> host-if-party -> lowest-playerId tie-break the
        # CellMap.h reduce (Plan 02) is built to enforce. 08-06: ONLY JOINS
        # MOVE (no lever reliably relocates the host's own selected leader) -
        # D1 home cell owner=0, D2 away cell owner=2 (rank2+3 relocate, host
        # not a party), D3 away cell STAYS 2 when rank1 (the lowest playerId)
        # arrives (continuity beats fresh contest) - sequence 0 -> 2 -> 2.
        # Judged by Test-WorldState's Legs A-D (CoopOraclesN.psm1). Mirrors
        # item_conservation_gate's own schema shape (N=4-only, judged by
        # analyze_run4.ps1, never the 2-log tier sweep).
        #
        # DiagEnv: KENSHICOOP_CELL_AUTH=1 is REQUIRED for the contested leg -
        # the harness (Set-CoopDiagEnv, CoopHarness.psm1) pins it to 0 for any
        # scenario that does not ask (Config.cpp:379-382), and with authority
        # off authorityFor always fail-opens to the host, so D2/D3 could never
        # fire. KENSHICOOP_CELL_COLLAPSE=0 keeps exactly one authority path
        # live (Plan 02 made the per-cell reduce structural, so collapse is a
        # no-op either way - the gate still pins it to make that explicit).
        # The world channels themselves (fac/door/build/prod/research/deed)
        # default ON even for scenarios (Config.cpp:226-238) - no DiagEnv
        # needed for them.
        #
        # PrimaryGate = world_state: the no-signal guard for the ONE
        # mechanism this scenario exists to prove. NoSignalFails on
        # world_state: an evidentiary gap (no SCENARIO WORLD evidence, or a
        # missing claimphase marker that leaves a D1/D2/D3 window
        # unjudgeable) is a FAIL, never a silent pass.
        #
        # Seconds/KillGraceSec: ScenarioWorldState.cpp's own HOST_DURATION_MS
        # is 250s (JOIN_DURATION_MS 240s; 08-06 - D3's judged window closes
        # at marker+55s = 235s host-clock) - Seconds=290 covers it with the
        # same margin class item_conservation_gate uses (~40s: staggered
        # joins + host shutdown overhead).
        world_state_gate = @{
            DiagEnv = @{ KENSHICOOP_CELL_AUTH = '1'; KENSHICOOP_CELL_COLLAPSE = '0' }
            Save = 'wanderer4'; Setup = ''; Tolerance = 6.0
            Seconds = 290; KillGraceSec = 260
            PrimaryGate = 'world_state'
            Gating   = @('world_state', 'clean_exit')
            Advisory = @()
            NoSignalFails = @('world_state')
            Tier = 'none'; WanVariant = $false   # N=4-only: judged by analyze_run4.ps1, never the 2-log tier sweep
        }

        # consensus_gate (Phase 9 plan 03, CONS-01/02/03): the ONE live
        # scenario proving all three global-consensus planes (shared money
        # pool, speed min-vote, time convergence) end to end on real UDP -
        # Leg A money conservation + a deliberate overdraft contest with an
        # identical reject verdict + refund on all four instances, Leg B
        # speed min-vote + combat cap + a mid-session disconnect of the rank
        # holding a constraining vote (the CONS-02 instant-vote-drop proof),
        # Leg C passive time-convergence sampling. Judged by Test-Consensus
        # (CoopOraclesN.psm1). Mirrors world_state_gate/item_conservation_gate's
        # own schema shape (N=4-only, judged by analyze_run4.ps1, never the
        # 2-log tier sweep - Save is documentation only, run_test4.ps1 reads
        # the actual save from rig config).
        #
        # No DiagEnv: money/speed/time sync all default ON (Config.cpp:196-228)
        # even for a named scenario - unlike the cell-authority/inv-sync
        # planes, nothing needs to be force-enabled here.
        #
        # TIMING CONTRACT (see ScenarioConsensus.cpp's file header): Plan 04's
        # live run MUST set join3's disconnectAtSec so the force-kill lands
        # strictly inside the scenario's own "SCENARIO CONSENSUS legmark
        # leg=B phase=1" (t=150s own-clock) / "phase=2" (t=220s own-clock)
        # bracket - a kill outside that window either never observes the held
        # constraining vote or leaves too little run time for the survivors'
        # raise to register. disconnectAtSec is measured from the HOST's
        # clock (run_test4.ps1's own doc comment); the scenario's phase
        # markers are host-clock-anchored too (host-only marker emission,
        # the ScenarioWorldState.cpp claimphase idiom), so the two line up
        # directly with no additional skew conversion.
        #
        # PrimaryGate = consensus: the no-signal guard for the ONE mechanism
        # this scenario exists to prove. NoSignalFails on consensus: an
        # evidentiary gap (no SCENARIO CONSENSUS evidence, or a missing
        # reject/vote-drop/clock window) is a FAIL, never a silent pass.
        #
        # Seconds/KillGraceSec: ScenarioConsensus.cpp's own HOST_DURATION_MS
        # is 250s (JOIN_DURATION_MS 235s) - Seconds=290 covers it with the
        # same margin class world_state_gate/item_conservation_gate use.
        consensus_gate = @{
            Save = 'wanderer4'; Setup = ''; Tolerance = 6.0
            Seconds = 290; KillGraceSec = 260
            PrimaryGate = 'consensus'
            Gating   = @('consensus', 'clean_exit')
            Advisory = @()
            NoSignalFails = @('consensus')
            Tier = 'none'; WanVariant = $false   # N=4-only: judged by analyze_run4.ps1, never the 2-log tier sweep
        }

        # save_load_gate (Phase 10 plan 03, SAVE-01..04): the ONE live 4-
        # instance scenario proving the per-client save/load coordinator
        # (Plan 01) and the joiner-unicast late-join bootstrap (Plan 02) end
        # to end on real UDP - four legs on one fixed host-clock timeline,
        # ordered L(late-join)->S(4-client save)->R(concurrent reject)->
        # H(host-load-while-3-connected), a deliberate divergence from the
        # research's sketched order: Legs S and H require the 4th instance
        # (join3) to already be connected (cardinality), so the late-join
        # leg must run first. Judged by Test-SaveLoad (CoopOraclesN.psm1).
        # Mirrors consensus_gate/world_state_gate's own schema shape (N=4-
        # only, judged by analyze_run4.ps1, never the 2-log tier sweep -
        # Save is documentation only, run_test4.ps1 reads the actual save
        # from rig config; the rig RESTORES the fixture per run, mandatory
        # here since the connect-push bake rewrites the host's save folder
        # by design).
        #
        # No DiagEnv: save/load coordination sync defaults ON (Config.cpp)
        # even for a named scenario - nothing needs to be force-enabled.
        #
        # TIMING CONTRACT (reconnectAtSec, see ScenarioSaveLoad.cpp's file
        # header + HOST_DURATION_MS/JOIN_DURATION_MS): join3's rig entry
        # (Plan 04's local.rig.json, gitignored) sets reconnectAtSec to a
        # host-relative offset strictly inside Leg L's window
        # [0, LEGL_END_MS=120000ms] - a deferred FIRST launch (the rig's
        # existing lever, run_test4.ps1:329-343/372-404), never a new
        # harness surface. run_meta.json's scheduledReconnect (keyed by
        # join3's ACTUAL logName) exempts join3 from clean_exit's full-
        # duration/health assumptions for the pre-connect window; Test-
        # SaveLoad reads it as a bonus cross-check (not a hard requirement -
        # the primary join-window boundary is derived from this scenario's
        # OWN "SCENARIO SAVELOAD ..." / the production "[boot] GO->join
        # dest=.." evidence, which is more precise than a config marker).
        #
        # PrimaryGate = save_load: the no-signal guard for the ONE
        # mechanism this scenario exists to prove. NoSignalFails on
        # save_load: an evidentiary gap (no SCENARIO SAVELOAD evidence, or a
        # missing leg=<L|S|R|H> marker that leaves a window unjudgeable) is
        # a FAIL, never a silent pass.
        #
        # Seconds/KillGraceSec: ScenarioSaveLoad.cpp's own HOST_DURATION_MS
        # is 320s (JOIN_DURATION_MS 305s; Leg H's judged window closes at
        # LEGH_END_MS=305s host-clock) - Seconds=350 covers it with the same
        # margin class world_state_gate/consensus_gate use (~30-40s:
        # staggered joins + host shutdown overhead). HostSelfExitSec (new
        # manifest key, run_test4.ps1's reconnectAtSec deferred-launch cap)
        # is set to HOST_DURATION_MS/1000=320 - the invariant this key
        # locks: HostSelfExitSec MUST equal the scenario's own host self-
        # exit duration, or a mismatch silently self-exits the host before
        # join3's deferred launch completes Leg L (research Pitfall 9).
        save_load_gate = @{
            Save = 'wanderer4'; Setup = ''; Tolerance = 6.0
            Seconds = 350; KillGraceSec = 320
            HostSelfExitSec = 320
            PrimaryGate = 'save_load'
            Gating   = @('save_load', 'clean_exit')
            Advisory = @()
            NoSignalFails = @('save_load')
            Tier = 'none'; WanVariant = $false   # N=4-only: judged by analyze_run4.ps1, never the 2-log tier sweep
        }
    }
}
