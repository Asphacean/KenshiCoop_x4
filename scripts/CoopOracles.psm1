# CoopOracles.psm1 - the KenshiCoop validation oracle library.
#
# Every oracle that judges a host/join log pair lives here (ported from the old
# inline run_test.ps1 implementations) so the SAME verdict code runs in three
# contexts:
#   * live loopback runs  (run_test.ps1 calls Invoke-RunAnalysis at the end)
#   * regression tiers    (regress.ps1 consumes the per-run verdict.json)
#   * remote sessions     (analyze_run.ps1 judges logs collected from a friend)
#
# Three-state verdicts: each oracle returns 'PASS' | 'FAIL' | 'SKIP' and records
# a gate entry (name, status, metrics, detail) via Add-GateResult. 'SKIP' means
# "could not judge" (missing instrumentation / too small a sample) and is
# surfaced - never silently converted to a pass. A scenario's PRIMARY gate
# (declared in scenarios.psd1) failing OR skipping fails the whole run: a green
# run must PROVE its mechanism was judged (the no-signal guard).
#
# Clock alignment: Get-ScenarioSeries and every marker-timestamp parse apply the
# per-log CLOCKSYNC offset (logged by the plugin's wire time-sync), so
# time-aligned oracles work when host and join run on machines whose wall
# clocks disagree. Logs without CLOCKSYNC lines get offset 0 (the legacy
# same-machine behaviour).

# ---- Gate result infrastructure ---------------------------------------------

$script:Gates = New-Object System.Collections.ArrayList
$script:ClockOffsetCache = @{}

function Reset-GateResults {
    $script:Gates = New-Object System.Collections.ArrayList
    $script:ClockOffsetCache = @{}
}

# Record one gate verdict. Status: PASS | FAIL | SKIP. Metrics is a hashtable of
# measured values (persisted into verdict.json for trending).
function Add-GateResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][ValidateSet("PASS", "FAIL", "SKIP")][string]$Status,
        [hashtable]$Metrics = @{},
        [string]$Detail = ""
    )
    [void]$script:Gates.Add([pscustomobject]@{
        gate    = $Name
        status  = $Status
        metrics = $Metrics
        detail  = $Detail
    })
    return $Status
}

function Get-GateResults { return @($script:Gates) }

# Merge two direction/sub-check statuses: FAIL dominates, then SKIP, then PASS.
function Merge-Status {
    param([string[]]$Statuses)
    if ($Statuses -contains "FAIL") { return "FAIL" }
    if ($Statuses -contains "SKIP") { return "SKIP" }
    return "PASS"
}

# ---- Oracle fragments (monolith split, 2026-07-12) ---------------------------
# The oracle bodies live in scripts/oracles/*.ps1, DOT-SOURCED into this
# module's scope so every $script: variable (Gates, ClockOffsetCache, the
# *Regex patterns) stays shared exactly as before the split. Order matters
# only for readability - fragments define functions, they run nothing.
# Importers (run_test.ps1, analyze_run.ps1, regress.ps1) are unchanged.
. (Join-Path $PSScriptRoot 'oracles\Parsing.ps1')
. (Join-Path $PSScriptRoot 'oracles\CoreChecks.ps1')
. (Join-Path $PSScriptRoot 'oracles\Npc.ps1')
. (Join-Path $PSScriptRoot 'oracles\Combat.ps1')
. (Join-Path $PSScriptRoot 'oracles\Medical.ps1')
. (Join-Path $PSScriptRoot 'oracles\Inventory.ps1')
. (Join-Path $PSScriptRoot 'oracles\World.ps1')
. (Join-Path $PSScriptRoot 'oracles\Session.ps1')
. (Join-Path $PSScriptRoot 'oracles\Motion.ps1')
. (Join-Path $PSScriptRoot 'oracles\Panel.ps1')

# ---- Manifest + top-level analysis ---------------------------------------------------

# Load scripts/scenarios.psd1 (or an explicit path).
function Get-ScenarioManifest {
    param([string]$Path = "")
    if ($Path -eq "") { $Path = Join-Path $PSScriptRoot "scenarios.psd1" }
    if (-not (Test-Path $Path)) { throw "Scenario manifest not found: $Path" }
    return Import-PowerShellDataFile -Path $Path
}

# Gates Invoke-Oracles evaluates ITSELF, before manifest dispatch, because they
# read run artifacts (the archived engine log) rather than the two client logs
# that Invoke-OneOracle is handed. A scenario may still name one in its Gating
# list to be judged on it - the dispatch is skipped, the result already stands.
# Exported so the contract check counts these as real oracle ids instead of
# reporting a manifest that names one as a dangling reference.
function Get-PreRunGates { return @("engine_integrity") }

# Which oracles accept a manifest Bounds override, and which parameter names
# each will take. The contract check reads this, so a mistyped scenario bound is
# rejected at check time instead of silently doing nothing at run time - a
# threshold that looks configured but is not is worse than no knob at all.
function Get-OracleBoundSpec {
    return @{
        smoothness = @("MaxZeroFrac", "MinActiveFrames")
        march      = @("MaxIdleFrac", "MaxMarchFrac")
        dual_drive = @("MinOverlapMs", "MaxHands", "MinSamples")
        lockpick_escape = @("BudgetS", "MinMove")
        pc_dupes   = @("DupRadius", "ExpectPcs", "MinSamples", "StackFracMax")
    }
}

# Run ONE named oracle. Central dispatch keyed by the oracle ids used in the
# manifest's Gating/Advisory lists. $Bounds carries this scenario's threshold
# overrides for the ids listed in Get-OracleBoundSpec (splatted onto the call).
function Invoke-OneOracle {
    param([string]$Id, [string]$HostLog, [string]$JoinLog, [double]$Tolerance, $ExpectedSkewMs,
          [hashtable]$Bounds = @{})
    switch ($Id) {
        "crosscheck"    { return (Test-Crosscheck      -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "npc_track"     { return (Test-NpcTrack        -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "coop_presence" { return (Test-CoopPresence    -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "pose"          { return (Test-NpcPose         -HostFile $HostLog -JoinFile $JoinLog) }
        "pose_state"    { return (Test-NpcPoseState    -HostFile $HostLog -JoinFile $JoinLog) }
        "bed_pose"      { return (Test-BedPose         -HostFile $HostLog -JoinFile $JoinLog) }
        "bed_wake"      { return (Test-BedWake         -HostFile $HostLog -JoinFile $JoinLog) }
        "bed_lay"       { return (Test-BedLay          -HostFile $HostLog -JoinFile $JoinLog) }
        "bed_put"       { return (Test-FurnPut         -HostFile $HostLog -JoinFile $JoinLog -Kind 1) }
        "cage_put"      { return (Test-FurnPut         -HostFile $HostLog -JoinFile $JoinLog -Kind 2) }
        "chain_put"     { return (Test-FurnPut         -HostFile $HostLog -JoinFile $JoinLog -Kind 3) }
        "pole_put"      { return (Test-FurnPut         -HostFile $HostLog -JoinFile $JoinLog -Kind 4 -InKind 2) }
        "cage_peer"     { return (Test-CagePeer        -HostFile $HostLog -JoinFile $JoinLog) }
        "sneak_probe"   { return (Test-SneakProbe      -HostFile $HostLog) }
        "spawn_probe"   { return (Test-SpawnProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "shop_probe"    { return (Test-ShopProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "wallet_probe"  { return (Test-WalletProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "money_sync"    { return (Test-MoneySync       -HostFile $HostLog -JoinFile $JoinLog) }
        "vendor_trade"  { return (Test-VendorTrade     -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_probe" { return (Test-RecruitProbe    -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_sync"  { return (Test-RecruitSync     -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_ctl"   { return (Test-RecruitCtl      -HostFile $HostLog -JoinFile $JoinLog) }
        "faction_probe" { return (Test-FactionProbe    -HostFile $HostLog -JoinFile $JoinLog) }
        "faction_sync"  { return (Test-FactionSync     -HostFile $HostLog -JoinFile $JoinLog) }
        "time_probe"    { return (Test-TimeProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "time_sync"     { return (Test-TimeSync        -HostFile $HostLog -JoinFile $JoinLog) }
        "door_probe"    { return (Test-DoorProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "door_sync"     { return (Test-DoorSync        -HostFile $HostLog -JoinFile $JoinLog) }
        "deed_probe"    { return (Test-DeedProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "deed_sync"     { return (Test-DeedSync        -HostFile $HostLog -JoinFile $JoinLog) }
        "build_probe"   { return (Test-BuildProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "build_sync"    { return (Test-BuildSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "bdoor_probe"   { return (Test-BdoorProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "bdoor_sync"    { return (Test-BdoorSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "hunger_probe"  { return (Test-HungerProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "hunger_sync"   { return (Test-HungerSync      -HostFile $HostLog -JoinFile $JoinLog) }
        "latejoin_probe" { return (Test-LatejoinProbe  -HostFile $HostLog -JoinFile $JoinLog) }
        "latejoin_sync"  { return (Test-LatejoinSync   -HostFile $HostLog -JoinFile $JoinLog) }
        "save_probe"     { return (Test-SaveProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "load_probe"     { return (Test-LoadProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "save_sync"      { return (Test-SaveSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "save_resume"    { return (Test-SaveResume     -HostFile $HostLog -JoinFile $JoinLog) }
        "load_sync"      { return (Test-LoadSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "money_persist"  { return (Test-MoneyPersist   -HostFile $HostLog -JoinFile $JoinLog) }
        "connect_bootstrap" { return (Test-ConnectBootstrap -HostFile $HostLog -JoinFile $JoinLog) }
        "connect_stream"    { return (Test-ConnectStream    -HostFile $HostLog -JoinFile $JoinLog) }
        "prod_probe"     { return (Test-ProdProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "prod_sync"      { return (Test-ProdSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "research_probe" { return (Test-ResearchProbe  -HostFile $HostLog -JoinFile $JoinLog) }
        "research_sync"  { return (Test-ResearchSync   -HostFile $HostLog -JoinFile $JoinLog) }
        "store_probe"    { return (Test-StoreProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "store_sync"     { return (Test-StoreSync      -HostFile $HostLog -JoinFile $JoinLog) }
        "squad_probe"    { return (Test-SquadProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "squad_sync"     { return (Test-SquadSync      -HostFile $HostLog -JoinFile $JoinLog) }
        "spawn_sync"    { return (Test-SpawnSync       -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "spawn_far"     { return (Test-SpawnFarBind    -HostFile $HostLog -JoinFile $JoinLog) }
        "npc_census"    { return (Test-NpcCensus       -HostFile $HostLog -JoinFile $JoinLog) }
        "sneak_pose"    { return (Test-SneakPose       -HostFile $HostLog -JoinFile $JoinLog) }
        "sneak_detect"  { return (Test-SneakDetect     -HostFile $HostLog -JoinFile $JoinLog) }
        "body_state"    { return (Test-NpcBodyState    -HostFile $HostLog -JoinFile $JoinLog) }
        "crawl_move"    { return (Test-CrawlMove       -HostFile $HostLog -JoinFile $JoinLog) }
        "craft_order"   { return (Test-CraftOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "mine_pose"     { return (Test-MinePose        -JoinFile $JoinLog) }
        "mine_identity" { return (Test-MineIdentity    -JoinFile $JoinLog) }
        "mine_output"   { return (Test-MineOutput      -JoinFile $JoinLog) }
        "mine_clear"    { return (Test-MineClear       -JoinFile $JoinLog) }
        "down_order"    { return (Test-DownOrder       -HostFile $HostLog -JoinFile $JoinLog) }
        "death_order"   { return (Test-DeathOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "combat_probe"  { return (Test-CombatProbe     -HostFile $HostLog) }
        "combat_order"  { return (Test-CombatOrder     -HostFile $HostLog -JoinFile $JoinLog) }
        "combat_kill"   { return (Test-CombatKill      -HostFile $HostLog -JoinFile $JoinLog) }
        "damage_guard"  { return (Test-DamageGuard     -HostFile $HostLog -JoinFile $JoinLog) }
        "player_combat" { return (Test-PlayerCombat    -HostFile $HostLog -JoinFile $JoinLog) }
        "assault_town"  { return (Test-AssaultTown     -HostFile $HostLog -JoinFile $JoinLog) }
        "assault_travel" { return (Test-AssaultTravel  -HostFile $HostLog -JoinFile $JoinLog) }
        "assault_mint"  { return (Test-AssaultTravel  -HostFile $HostLog -JoinFile $JoinLog -GateName 'assault_mint' -RequireMint) }
        "mint_aggro"    { return (Test-MintAggro      -HostFile $HostLog -JoinFile $JoinLog) }
        "proxy_drift"   { return (Test-ProxyDrift     -JoinFile $JoinLog) }
        "pc_assault"    { return (Test-PcAssault       -HostFile $HostLog -JoinFile $JoinLog) }
        "player_ko"     { return (Test-PlayerKo        -HostFile $HostLog -JoinFile $JoinLog) }
        "medic_order"   { return (Test-MedicOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "limb_loss"     { return (Test-LimbLoss        -HostFile $HostLog -JoinFile $JoinLog) }
        "stats_sync"    { return (Test-StatsSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "carry_order"   { return (Test-CarryOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "npc_carry"     { return (Test-NpcCarry        -HostFile $HostLog -JoinFile $JoinLog) }
        "npc_vitals"    { return (Test-NpcVitals       -HostFile $HostLog -JoinFile $JoinLog) }
        "speed_sync"    { return (Test-SpeedSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "speed_probe"   { return (Test-SpeedProbe      -HostFile $HostLog) }
        "shackle_probe" { return (Test-ShackleProbe    -HostFile $HostLog -JoinFile $JoinLog) }
        "shackle_sync"  { return (Test-ShackleSync     -HostFile $HostLog -JoinFile $JoinLog) }
        "lockpick_escape" { return (Test-LockpickEscape -HostFile $HostLog -JoinFile $JoinLog @Bounds) }
        "pc_dupes"        { return (Test-PcDuplicates -HostFile $HostLog -JoinFile $JoinLog @Bounds) }
        "combat_crowd"  { return (Test-CombatCrowd     -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "combat_battle" { return (Test-CombatBattle    -HostFile $HostLog -JoinFile $JoinLog) }
        "combat_win"    { return (Test-CombatWin       -HostFile $HostLog -JoinFile $JoinLog) }
        "combat_snap_rate" { return (Test-CombatSnapRate -JoinFile $JoinLog) }
        "split_interest" { return (Test-SplitInterest  -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "inv_sync"      { return (Test-InventorySync   -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_bidir"     { return (Test-InventoryBidir  -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_overflow"  { return (Test-InventoryOverflow -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_dropfull"  { return (Test-InventoryDropFull -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_equip"     { return (Test-InventoryEquip  -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_reequip"   { return (Test-InventoryReequip -HostFile $HostLog -JoinFile $JoinLog) }
        "add_equip"     { return (Test-AddEquip        -HostFile $HostLog -JoinFile $JoinLog) }
        "trade_probe"   { return (Test-TradeProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "trade_peer"    { return (Test-TradePeer       -HostFile $HostLog -JoinFile $JoinLog) }
        "drop_probe"    { return (Test-DropProbe       -HostFile $HostLog) }
        "wi_sync"       { return (Test-WorldItemSync   -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "wi_join"       { return (Test-WorldItemSync   -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance -JoinAuthor -GateName "wi_join") }
        "wpn_relocate"  { return (Test-WpnRelocate     -HostFile $HostLog -JoinFile $JoinLog) }
        "weapon_drop"   { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog) }
        "armor_drop"    { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog -GateName "armor_drop") }
        "backpack_drop" { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog -GateName "backpack_drop") }
        "pickup_mirror" { return (Test-WorldPickupMirror -HostFile $HostLog -JoinFile $JoinLog) }
        "gear_repickup" { return (Test-GearRepickup    -HostFile $HostLog -JoinFile $JoinLog) }
        "gear_repickup_retry" { return (Test-GearRepickup -HostFile $HostLog -JoinFile $JoinLog `
                                            -GateName "gear_repickup_retry" -RequireRetry) }
        "gear_repickup_dedupe" { return (Test-GearRepickup -HostFile $HostLog -JoinFile $JoinLog `
                                            -GateName "gear_repickup_dedupe" -RequireDedupe) }
        "gear_repickup_recover" { return (Test-GearRepickup -HostFile $HostLog -JoinFile $JoinLog `
                                            -GateName "gear_repickup_recover" -RequireSiteRecovery) }
        "world_item_burst" { return (Test-WorldItemBurst -HostFile $HostLog -JoinFile $JoinLog) }
        "world_item_stale" { return (Test-WorldItemStale -HostFile $HostLog -JoinFile $JoinLog) }
        "nested_bag"    { return (Test-NestedBag       -HostFile $HostLog -JoinFile $JoinLog) }
        "dump_all"      { return (Test-DumpAll         -HostFile $HostLog -JoinFile $JoinLog) }
        "no_phantom_pickups" { return (Test-NoPhantomPickups -HostFile $HostLog -JoinFile $JoinLog) }
        "weapon_loot"   { return (Test-WeaponLoot      -HostFile $HostLog -JoinFile $JoinLog) }
        "rejoin_items"  { return (Test-RejoinItems     -HostFile $HostLog -JoinFile $JoinLog) }
        "smoothness"    { return (Test-Smoothness      -File $JoinLog @Bounds) }
        "anim_truth"    { return (Test-AnimTruth       -File $JoinLog) }
        "march"         { return (Test-MarchInPlace    -File $JoinLog @Bounds) }
        "snap_rate"     { return (Test-SnapRate        -File $JoinLog) }
        "snap_rate_squad" { return (Test-SnapRate      -File $JoinLog -SquadOnly -GateName "snap_rate_squad") }
        "suppress_churn" { return (Test-SuppressChurn  -File $JoinLog) }
        "rest_flap"     { return (Test-RestFlap        -File $JoinLog) }
        "existence_parity" { return (Test-ExistenceParity -File $JoinLog) }
        "follow_travel" { return (Test-FollowTravel    -HostFile $HostLog -JoinFile $JoinLog) }
        "travel_parity" { return (Test-TravelParity    -HostFile $HostLog -JoinFile $JoinLog) }
        "split_far"     { return (Test-SplitFar        -HostFile $HostLog -JoinFile $JoinLog) }
        "split_far2"    { return (Test-SplitFar2       -HostFile $HostLog -JoinFile $JoinLog) }
        "dual_drive"    { return (Test-DualDrive       -HostFile $HostLog -JoinFile $JoinLog @Bounds) }
        "run_apart"     { return (Test-RunApart        -HostFile $HostLog -JoinFile $JoinLog) }
        "town_arrive"   { return (Test-TownArrive      -HostFile $HostLog -JoinFile $JoinLog) }
        "town_pop_parity" { return (Test-TownPopParity -HostFile $HostLog -JoinFile $JoinLog) }
        "world_parity"  { return (Test-WorldParity     -HostFile $HostLog -JoinFile $JoinLog) }
        "cell_probe"    { return (Test-CellProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "camp_approach" { return (Test-CampApproach    -HostFile $HostLog -JoinFile $JoinLog) }
        "mint_dist"     { return (Test-MintDistance    -JoinFile $JoinLog) }
        "anti_zombie"   { return (Test-AntiZombie      -HostFile $HostLog -JoinFile $JoinLog) }
        "lifecycle"     { return (Test-Lifecycle       -JoinFile $JoinLog) }
        "clock_sync"    { return (Test-ClockSync       -HostFile $HostLog -JoinFile $JoinLog -ExpectedSkewMs $ExpectedSkewMs) }
        "panel_config"  { return (Test-PanelConfig     -File $HostLog) }
        default {
            Write-Host "  WARNING: unknown oracle id '$Id' (manifest error)"
            return (Add-GateResult -Name $Id -Status FAIL -Detail "unknown oracle id")
        }
    }
}

# Top-level: analyze one run's host/join logs and produce the overall verdict +
# verdict.json. Used by run_test.ps1 (live) and analyze_run.ps1 (collected logs).
#
# Verdict rule:
#   FAIL if any always-on gate (health/result/check_fail) failed,
#   or any GATING oracle failed,
#   or the scenario's PRIMARY gate is SKIP/missing (the no-signal guard).
#   ADVISORY oracle results are recorded but never gate.
function Invoke-RunAnalysis {
    param(
        [Parameter(Mandatory = $true)][string]$HostLog,
        [string]$JoinLog = "",
        [string]$Scenario = "",
        [double]$Tolerance = 3.0,
        [bool]$JoinExpected = $true,
        [string]$ManifestPath = "",
        [hashtable]$RunInfo = @{},
        $ExpectedSkewMs = $null,
        # True when the run went through the WAN relay proxy: applies the
        # scenario's deliberate WAN-regime adjustments (WanTolerance override +
        # WanDemote gating->advisory moves) declared in the manifest.
        [bool]$WanActive = $false,
        [string]$OutJson = ""
    )
    Reset-GateResults
    $manifest = Get-ScenarioManifest -Path $ManifestPath
    $entry = $null
    if ($Scenario -ne "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
        $entry = $manifest.Scenarios[$Scenario]
    }
    if ($WanActive -and $null -ne $entry) {
        if ($entry.ContainsKey("WanTolerance")) {
            Write-Host "  (WAN regime: tolerance $Tolerance -> $($entry.WanTolerance) per manifest)"
            $Tolerance = $entry.WanTolerance
        }
    }

    # 1. Log health (always).
    $cleanPattern = if ($Scenario -ne "") { "SCENARIO RESULT" } else { "test duration elapsed; exiting" }
    [void](Test-LogHealth -File $HostLog -Label "host" -Required $true -CleanPattern $cleanPattern)
    [void](Test-LogHealth -File $JoinLog -Label "join" -Required $JoinExpected -CleanPattern $cleanPattern)

    # 1a. Engine bookkeeping integrity (always). Reads the archived kenshi_info.log,
    # so it SKIPs on runs predating the archiving and on log-only re-judges - but it
    # always registers a result, because a scenario is allowed to gate on it and a
    # gate with no result at all reads as "missing" (a FAIL) rather than "no signal".
    $engineDir = ""
    if ($null -ne $RunInfo -and $RunInfo.ContainsKey("outDir")) { $engineDir = "$($RunInfo.outDir)" }
    [void](Test-EngineIntegrity -OutDir $engineDir)

    # 1b. Clock catch-up visibility (always, advisory). The join closes its
    # load skew by simming at up to 2x (protocol 25); any oracle that scores
    # motion while the slew is engaged is measuring the transient, not the
    # steady state (the smoothness oracle now excludes those frames itself -
    # slewSkip=). This FINDING makes the overlap visible on every run; the
    # time_sync scenario is where convergence is actually GATED.
    if ($JoinLog -ne "" -and (Test-Path $JoinLog)) {
        $slew = Get-SlewSummary -File $JoinLog
        if ($null -ne $slew) {
            $conv = if ($slew.converged) { "converged to 1x" } else { "STILL SLEWING at log end (slew=$($slew.lastSlew))" }
            Write-Host "  FINDING: join clock catch-up - peakOff=$($slew.peakOffGh)gh peakSlew=$($slew.peakSlew)x for ~$($slew.slewSecs)s; $conv"
        }
    }

    $gating = @(); $advisory = @(); $primary = ""
    if ($Scenario -ne "") {
        # 2. In-plugin CHECK lines + SCENARIO RESULT (always, for scenarios).
        [void](Test-NoCheckFail -HostFile $HostLog -JoinFile $JoinLog)
        [void](Test-ScenarioResultPass -File $HostLog -Label "host" -Required $true)
        [void](Test-ScenarioResultPass -File $JoinLog -Label "join" -Required $JoinExpected)

        # 3. Scenario oracles per the manifest.
        if ($null -ne $entry) {
            $gating   = @($entry.Gating)
            $advisory = @($entry.Advisory)
            $primary  = $entry.PrimaryGate
            if ($WanActive -and $entry.ContainsKey("WanDemote")) {
                foreach ($d in @($entry.WanDemote)) {
                    if ($gating -contains $d) {
                        Write-Host "  (WAN regime: gate '$d' demoted to advisory per manifest)"
                        $gating   = @($gating | Where-Object { $_ -ne $d })
                        $advisory += $d
                    }
                }
            }
            # Per-scenario bounds. Until now the only per-scenario lever on a
            # motion gate was WanDemote, which switches the gate OFF entirely -
            # so a scenario that legitimately runs 0.42 against a 0.40 ceiling
            # stops being judged at all instead of being judged at its own
            # number. Bounds keep the gate live at a scenario-appropriate
            # ceiling; WanBounds overlay it under the proxy.
            $bounds = @{}
            if ($entry.ContainsKey("Bounds")) { $bounds = $entry.Bounds }
            if ($WanActive -and $entry.ContainsKey("WanBounds")) {
                $merged = @{}
                foreach ($k in $bounds.Keys) { $merged[$k] = $bounds[$k] }
                foreach ($k in $entry.WanBounds.Keys) {
                    $o = @{}
                    if ($merged.ContainsKey($k)) { foreach ($p in $merged[$k].Keys) { $o[$p] = $merged[$k][$p] } }
                    foreach ($p in $entry.WanBounds[$k].Keys) { $o[$p] = $entry.WanBounds[$k][$p] }
                    $merged[$k] = $o
                }
                $bounds = $merged
            }
            $preRun = Get-PreRunGates
            foreach ($id in ($gating + $advisory)) {
                if ($preRun -contains $id) { continue }
                $b = @{}
                if ($bounds.ContainsKey($id)) {
                    $b = $bounds[$id]
                    $shown = ($b.Keys | Sort-Object | ForEach-Object { "$_=$($b[$_])" }) -join ' '
                    Write-Host "  (manifest bound for '$id': $shown)"
                }
                [void](Invoke-OneOracle -Id $id -HostLog $HostLog -JoinLog $JoinLog `
                          -Tolerance $Tolerance -ExpectedSkewMs $ExpectedSkewMs -Bounds $b)
            }
        } else {
            Write-Host "  WARNING: scenario '$Scenario' not in manifest; running generic cross-check"
            $gating = @("crosscheck"); $primary = "crosscheck"
            [void](Test-Crosscheck -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance)
        }
    }

    # engine_integrity runs on every scenario but is not yet trusted to judge one
    # (no baseline for Kenshi's own rate), so label it advisory rather than let
    # the summary imply it gates - unless a scenario opted into gating on it.
    if ($gating -notcontains "engine_integrity") { $advisory += "engine_integrity" }

    # 4. Compute the verdict.
    $gates = Get-GateResults
    $byName = @{}
    foreach ($g in $gates) { $byName[$g.gate] = $g }
    $reasons = @()
    $alwaysOn = @("health_host", "health_join", "check_fail", "result_host", "result_join")
    foreach ($n in $alwaysOn) {
        if ($byName.ContainsKey($n) -and $byName[$n].status -eq "FAIL") { $reasons += "$n FAIL" }
    }
    foreach ($n in $gating) {
        if (-not $byName.ContainsKey($n)) { $reasons += "$n missing"; continue }
        if ($byName[$n].status -eq "FAIL") { $reasons += "$n FAIL" }
    }
    if ($primary -ne "") {
        if (-not $byName.ContainsKey($primary)) {
            $reasons += "primary gate $primary missing (no signal)"
        } elseif ($byName[$primary].status -eq "SKIP") {
            $reasons += "primary gate $primary SKIP (no signal)"
        }
    }
    $pass = ($reasons.Count -eq 0)

    # 5. Summary table: every gate with its status (SKIP visible, advisory tagged).
    Write-Host ""
    Write-Host "== Gate summary =="
    foreach ($g in $gates) {
        $tag = ""
        if ($advisory -contains $g.gate) { $tag = " (advisory)" }
        elseif ($g.gate -eq $primary)    { $tag = " (primary)" }
        Write-Host ("  {0,-14} {1,-4}{2}{3}" -f $g.gate, $g.status, $tag,
                    $(if ($g.detail -ne "") { " - " + $g.detail } else { "" }))
    }
    if (-not $pass) { Write-Host ("  verdict reasons: " + ($reasons -join "; ")) }

    # 6. verdict.json for trending / offline consumption.
    $verdict = [pscustomobject]@{
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
        scenario  = $Scenario
        tolerance = $Tolerance
        pass      = $pass
        reasons   = $reasons
        primary   = $primary
        gating    = $gating
        advisory  = $advisory
        run       = $RunInfo
        gates     = $gates
    }
    if ($OutJson -ne "") {
        $verdict | ConvertTo-Json -Depth 6 | Set-Content -Path $OutJson -Encoding UTF8
        Write-Host "  verdict json: $OutJson"
    }
    return $verdict
}

Export-ModuleMember -Function @(
    "Reset-GateResults", "Add-GateResult", "Get-GateResults", "Merge-Status",
    "Get-PreRunGates",
    "Get-LogClockOffsetMs", "Get-ClockSyncStats", "Convert-StampToMs",
    "Get-ScenarioLines", "Get-ScenarioSeries", "Get-MarkerTimeMs", "Get-LogLastActivityMs", "Get-HandAliases",
    "Test-LogHealth", "Test-EngineIntegrity", "Test-NoCheckFail", "Test-ScenarioResultPass", "Test-ClockSync",
    "Test-Crosscheck", "Measure-NpcSync", "Test-NpcTrack", "Test-CoopPresence",
    "Test-NpcPose", "Test-NpcPoseState", "Test-NpcBodyState", "Test-BedPose", "Test-BedWake", "Test-BedLay",
    "Test-CraftOrder", "Test-DownOrder", "Test-DeathOrder",
    "Test-MinePose", "Test-MineIdentity", "Test-MineOutput", "Test-MineClear",
    "Test-CombatProbe", "Test-CombatOrder", "Test-CombatKill", "Test-DamageGuard",
    "Test-CombatSnapRate", "Test-CombatBattle", "Test-CombatWin", "Test-DeathParity",
    "Get-VitalsSeries", "Test-PlayerCombat", "Test-AssaultTown", "Test-PcAssault", "Get-CombatParity", "Test-PlayerKo", "Test-MedicOrder",
    "Test-MedicPose", "Test-LimbLoss", "Test-NpcVitals",
    "Get-CrawlSeries", "Get-CrawlProbeSeries", "Test-CrawlMove",
    "Get-StatsSeries", "Test-StatsSync",
    "Get-CarrySeries", "Test-CarryOrder", "Test-NpcCarry",
    "Get-FurnSeries", "Test-FurnPut", "Test-ChainPut", "Test-PolePut", "Test-CagePeer",
    "Test-SneakProbe",
    "Get-SpawnHands", "Test-SpawnProbe", "Test-SpawnSync", "Test-SpawnFarBind",
    "Test-NpcCensus",
    "Get-WalletSeries", "Get-PoolSeries", "Test-ShopProbe", "Test-WalletProbe",
    "Test-MoneySync", "Test-MoneyPersist", "Test-VendorTrade",
    "Test-RecruitProbe", "Test-RecruitSync", "Test-RecruitCtl",
    "Get-FacRelSeries", "Test-FactionProbe", "Test-FactionSync",
    "Get-GTimeSeries", "Test-TimeProbe", "Test-TimeSync", "Get-SlewSummary",
    "Get-SneakSeries", "Test-SneakPose", "Test-SneakDetect",
    "Get-ShackleSeries",
    "Test-SpeedSync",
    "Test-SpeedProbe",
    "Test-ShackleProbe",
    "Test-ShackleSync",
    "Get-EscapeSeries", "Get-EscapeFreedMs", "Get-EscapeMoveAfter",
    "Get-EscapeHeldAt", "Get-EscapeHeldEdges", "Test-LockpickEscape",
    "Test-PcDuplicates",
    "Test-CombatCrowd",
    "Test-SplitInterest",
    "Test-InventorySync", "Test-InventoryBidir", "Test-InventoryEquip",
    "Test-InventoryOverflow", "Test-InventoryDropFull",
    "Test-InventoryReequip", "Test-AddEquip", "Test-TradeProbe", "Test-TradePeer", "Test-DropProbe",
    "Test-WorldItemSync", "Test-RejoinItems", "Test-WpnRelocate", "Test-WeaponDrop",
    "Test-WorldPickupMirror", "Test-GearRepickup", "Test-NoPhantomPickups", "Test-WorldItemBurst",
    "Test-NestedBag", "Test-DumpAll",
    "Test-Smoothness", "Test-AnimTruth", "Test-MarchInPlace",
    "Test-SnapRate", "Test-SuppressChurn", "Test-RestFlap",
    "Test-ExistenceParity",
    "Get-WnpcRows", "Get-WorldRows", "Group-WnpcSamples", "Test-FollowTravel", "Test-TravelParity",
    "Test-SplitFar", "Test-SplitFar2", "Test-DualDrive", "Test-RunApart",
    "Get-TownArriveData", "Get-TownAuditRows", "Test-TownArrive", "Test-TownPopParity",
    "Test-WorldParity", "Test-CellProbe",
    "Test-CampApproach",
    "Test-MintDistance", "Test-AntiZombie", "Test-Lifecycle",
    "Get-PanelConnects", "Get-PanelIntents", "Test-PanelConfig",
    "Get-ScenarioManifest", "Get-OracleBoundSpec", "Invoke-OneOracle", "Invoke-RunAnalysis"
)
