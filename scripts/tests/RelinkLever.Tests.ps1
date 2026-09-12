<#
.SYNOPSIS
  Headless guard for the Phase 14 connect-relink lever and its judge
  (Phase 14 Plan 02, UI-06/UI-07). Needs NO live game launch.

.DESCRIPTION
  Two halves.

  GROUP A - source-shape pins (fast, no build). The lever's design rules are
  the kind that rot silently: the ScenarioContext adapter must be wired at
  EVERY construction site (the struct has no constructor, so a missed site is
  a wild pointer, not a compile error); the connect sequence must still exist
  exactly ONCE in the tree, so Phase 15 keeps a genuine before/after baseline
  on coopUiConnect; the scenario TU must name neither NetLink nor g_net; the
  lever must stay behind KENSHICOOP_HARNESS so Release never gains it; and
  Config.cpp/Config.h/src\netproto\Wire.h must stay byte-identical to the
  pre-phase commit, because describeConfig builds the "effective cfg" banner
  from Config and Phase 14 criterion 4 is that a run which never invokes the
  lever is indistinguishable from a pre-phase run.

  GROUP B - judge sensitivity. A judge that cannot FAIL is not evidence. This
  half builds a SYNTHETIC run directory (no game, no rig) that satisfies every
  check, asserts scripts\analyze_relink.ps1 returns PASS on it, then mutates
  ONE thing at a time and asserts it returns FAIL for each: the roster-reset
  line removed; slots=0 at the boundary; the post-boundary admitted ids
  differing from the discarded ones; a "slots full" rejection present; the
  RELINK thread id differing from the MAINTID. The good-fixture PASS is
  asserted in its own right, so a judge that always fails is caught too.

  Exit code = number of failed checks (0 = PASS), matching
  CensusRepro.Tests.ps1's / Contract.Tests.ps1's convention so
  verify.ps1 can sum it.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\tests\RelinkLever.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$INV         = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                         # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                       # repo root

# ---- tiny assert harness (verbatim shape from CensusRepro.Tests.ps1) ----------
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

Write-Host "== RelinkLever: connect-relink source shape + judge sensitivity =="

# =================================================================== Group A ==
Write-Host ""
Write-Host "-- Group A: source-shape pins --"

$pluginCpp   = Join-Path $repoRoot "src\plugin\Plugin.cpp"
$scenSession = Join-Path $repoRoot "src\plugin\test\ScenarioSession.cpp"
$netLinkCpp  = Join-Path $repoRoot "src\plugin\net\NetLink.cpp"
$netLinkH    = Join-Path $repoRoot "src\plugin\net\NetLink.h"
$configCpp   = Join-Path $repoRoot "src\plugin\core\Config.cpp"
$configH     = Join-Path $repoRoot "src\plugin\core\Config.h"

foreach ($f in @($pluginCpp, $scenSession, $netLinkCpp, $netLinkH, $configCpp, $configH)) {
    Check "source present: $(Split-Path -Leaf $f)" (Test-Path $f)
}

$plugin = Get-Content -Raw -Path $pluginCpp
$scen   = Get-Content -Raw -Path $scenSession
$net    = Get-Content -Raw -Path $netLinkCpp

# A1 - the adapter is wired at EVERY ScenarioContext construction site. Counted
# against the pickMintedProxy precedent rather than a hardcoded number, so
# adding a construction site without wiring the adapter FAILs here.
$precedentWired = [regex]::Matches($plugin, '\.pickMintedProxy\s*=').Count
$relinkWired    = [regex]::Matches($plugin, '\.relinkSession\s*=').Count
Check "relinkSession wired at every ScenarioContext site (relink=$relinkWired precedent=$precedentWired)" `
    ($relinkWired -ge 1 -and $relinkWired -eq $precedentWired)

# A2 - the connect sequence exists ONCE (D-02): the panel handler and startup.
# The adapter CALLS coopUiConnect; it does not restate its body.
$connectCallSites = [regex]::Matches($plugin, '(?m)^\s*startNetworking\(\);').Count
Check "bare startNetworking(); call sites still 2 (found $connectCallSites)" ($connectCallSites -eq 2)

# A3 - adapter discipline: the scenario TU reaches nothing but the function pointer.
Check "ScenarioSession.cpp names no NetLink type"  (-not ($scen -match 'NetLink'))
Check "ScenarioSession.cpp names no g_net global"  (-not ($scen -match 'g_net'))

# A4 - the lever is harness-only (Release must gain nothing).
$thunk = [regex]::Match($plugin, '(?s)#ifdef\s+KENSHICOOP_HARNESS.*?coopScenarioRelinkSession.*?#endif\s*//\s*KENSHICOOP_HARNESS')
Check "coopScenarioRelinkSession sits inside a KENSHICOOP_HARNESS guard" ($thunk.Success)
$maintid = [regex]::Match($plugin, '(?s)#ifdef\s+KENSHICOOP_HARNESS.*?\[coop-ui\]\s+MAINTID\s+tid=')
Check "the MAINTID game-thread id line is harness-guarded" ($maintid.Success)

# A5 - connect_relink has BOTH a C++ maker and a manifest entry.
Check "connect_relink has a C++ maker" ($scen -match 'name\s*==\s*"connect_relink"')
$manifestOk = $false
try {
    Import-Module (Join-Path $scriptsRoot "CoopOracles.psm1") -Force -ErrorAction Stop
    $manifest = Get-ScenarioManifest
    $manifestOk = $manifest.Scenarios.ContainsKey('connect_relink')
} catch { $manifestOk = $false }
Check "connect_relink has a scenarios.psd1 manifest entry" $manifestOk

# A6 - the session-boundary trace (this plan's own artifact).
Check "resetSessionRoster tagged at the launch call site" ($net -match 'resetSessionRoster\(\s*"launch"\s*\)')
Check "resetSessionRoster tagged at the stop call site"   ($net -match 'resetSessionRoster\(\s*"stop"\s*\)')
Check "the roster-reset trace line exists in NetLink.cpp" ($net -match 'roster-reset where=%s role=%s slots=%u ids=\{%s\} epochs=%u')
Check "the trace is gated on KENSHICOOP_NET_ROSTER_TRACE" ($net -match 'KENSHICOOP_NET_ROSTER_TRACE')
# The trace must NEVER reach Config: describeConfig prints the effective-cfg
# banner from Config, and a field there breaks criterion 4 by construction.
Check "ROSTER_TRACE did not leak into Config.cpp" (-not ((Get-Content -Raw -Path $configCpp) -match 'ROSTER_TRACE'))
Check "ROSTER_TRACE did not leak into Config.h"   (-not ((Get-Content -Raw -Path $configH)   -match 'ROSTER_TRACE'))

# A7 - the banner/wire pins, read from the recorded pre-phase baseline.
$baselinePath = Join-Path $repoRoot "tools\test-runs\phase14_baseline.json"
if (-not (Test-Path $baselinePath)) {
    Check "phase14_baseline.json present (pre-phase baseline for the pins)" $false
} else {
    $b = Get-Content -Raw -Path $baselinePath | ConvertFrom-Json
    # NOTE the path is src\netproto\Wire.h - plan 14-01 corrected the plan text's
    # bare netproto\Wire.h, which does not exist and made Select-String throw.
    $pins = @('src/plugin/core/Config.cpp', 'src/plugin/core/Config.h', 'src/netproto/Wire.h')
    $drift = @()
    try {
        $raw = & git.exe -C $repoRoot diff --name-only $b.baseCommit -- $pins[0] $pins[1] $pins[2] 2>$null
        $drift = @($raw | Where-Object { "$_".Trim() -ne "" })
    } catch { $drift = @('<git failed>') }
    Check "Config.cpp/Config.h/src\netproto\Wire.h byte-unchanged since $($b.baseCommit.Substring(0,7)) (drift=$($drift.Count))" `
        ($drift.Count -eq 0)
    $wire = Get-Content -Raw -Path (Join-Path $repoRoot "src\netproto\Wire.h")
    $pm = [regex]::Match($wire, 'PROTOCOL_VERSION\s*=\s*(\d+)')
    $proto = if ($pm.Success) { [int]::Parse($pm.Groups[1].Value, $INV) } else { -1 }
    Check "PROTOCOL_VERSION still $($b.protocolVersion) (found $proto)" ($proto -eq [int]$b.protocolVersion)
}

# A8 - the judge and the runner exist, and the judge is NOT a registered oracle.
$judge  = Join-Path $scriptsRoot "analyze_relink.ps1"
$runner = Join-Path $scriptsRoot "relink_probe.ps1"
Check "scripts\analyze_relink.ps1 exists" (Test-Path $judge)
Check "scripts\relink_probe.ps1 exists"   (Test-Path $runner)
$oracles = Join-Path $scriptsRoot "CoopOracles.psm1"
if (Test-Path $oracles) {
    Check "analyze_relink is NOT registered as a gate oracle (no existing verdict moves)" `
        (-not ((Get-Content -Raw -Path $oracles) -match 'analyze_relink'))
}
# A9 - the runner must not carry the measured-dead KENSHICOOP_SAVE_SYNC=0 lever,
# and must not write the pinned rig.
# Strip the comment-based help first: the doc block NAMES the lever in order to
# explain why it is not set, and matching that text would be a false positive.
$probe = (Get-Content -Raw -Path $runner) -replace '(?s)^\s*<#.*?#>', ''
Check "relink_probe.ps1 does not set KENSHICOOP_SAVE_SYNC (14-01 measured it deadlocks the join)" `
    (-not ($probe -match 'KENSHICOOP_SAVE_SYNC'))
Check "relink_probe.ps1 passes a TEMP rig to run_test4.ps1, never the pinned one" `
    ($probe -match '-RigConfig\s+\$tempRig')

# =================================================================== Group B ==
Write-Host ""
Write-Host "-- Group B: judge sensitivity (synthetic fixtures, no game) --"

$fixRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_relink_fix_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))

function New-GoodFixture {
    param([string]$Dir)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null

    # host: 2 relinks, each discarding {1,2} and re-admitting {1,2}.
    $hostLines = @(
        '[10:00:00.000] [HOST] INFO: [coop-ui] MAINTID tid=1234',
        '[10:00:01.000] [HOST] INFO: [net] roster-reset where=launch role=host slots=0 ids={} epochs=0',
        '[10:00:01.100] [HOST] INFO: [net] hosting',
        '[10:00:10.000] [HOST] INFO: [net] peer connected id=1 player=1 (proto v61)',
        '[10:00:11.000] [HOST] INFO: [net] peer connected id=2 player=2 (proto v61)',
        '[10:00:20.000] [HOST] INFO: SCENARIO relink start host=1 localId=0 side=relink role=both atMs=40000 recoverMs=60000 count=2 adapter=1',
        '[10:01:00.000] [HOST] INFO: SCENARIO relink pre t=40000 peers=2 ids={1,2}',
        '[10:01:00.001] [HOST] INFO: SCENARIO relink issued t=40000 n=1',
        '[10:01:00.002] [HOST] INFO: [coop-ui] RELINK src=scenario tid=1234',
        '[10:01:00.010] [HOST] INFO: [coop-ui] connect: role=HOST transport=udp peer=0 ownRanks={0} src=role',
        '[10:01:00.050] [HOST] INFO: [net] roster-reset where=stop role=host slots=2 ids={1,2} epochs=2',
        '[10:01:00.070] [HOST] INFO: [net] roster-reset where=launch role=host slots=0 ids={} epochs=0',
        '[10:01:00.080] [HOST] INFO: [net] hosting',
        '[10:01:06.000] [HOST] INFO: [net] peer connected id=1 player=1 (proto v61)',
        '[10:01:07.000] [HOST] INFO: [net] peer connected id=2 player=2 (proto v61)',
        '[10:01:07.100] [HOST] INFO: SCENARIO relink post t=47100 peers=2 ids={1,2} recoveredMs=7100 ok=1',
        '[10:03:00.000] [HOST] INFO: SCENARIO relink pre t=160000 peers=2 ids={1,2}',
        '[10:03:00.001] [HOST] INFO: SCENARIO relink issued t=160000 n=2',
        '[10:03:00.002] [HOST] INFO: [coop-ui] RELINK src=scenario tid=1234',
        '[10:03:00.010] [HOST] INFO: [coop-ui] connect: role=HOST transport=udp peer=0 ownRanks={0} src=role',
        '[10:03:00.050] [HOST] INFO: [net] roster-reset where=stop role=host slots=2 ids={1,2} epochs=2',
        '[10:03:00.070] [HOST] INFO: [net] roster-reset where=launch role=host slots=0 ids={} epochs=0',
        '[10:03:05.000] [HOST] INFO: [net] peer connected id=1 player=1 (proto v61)',
        '[10:03:05.500] [HOST] INFO: [net] peer connected id=2 player=2 (proto v61)',
        '[10:03:05.600] [HOST] INFO: SCENARIO relink post t=165600 peers=2 ids={1,2} recoveredMs=5600 ok=1',
        '[10:03:05.700] [HOST] INFO: SCENARIO RESULT PASS'
    )
    Set-Content -Path (Join-Path $Dir 'host.log') -Value $hostLines -Encoding UTF8

    # join1: the designated CLIENT relinker - the other session boundary.
    $join1 = @(
        '[10:00:05.000] [JOIN] INFO: [coop-ui] MAINTID tid=2222',
        '[10:00:05.100] [JOIN] INFO: [net] roster-reset where=launch role=client slots=0 ids={} epochs=0',
        '[10:00:20.000] [JOIN] INFO: SCENARIO relink start host=0 localId=1 side=relink role=both atMs=40000 recoverMs=60000 count=2 adapter=1',
        '[10:02:00.000] [JOIN] INFO: SCENARIO relink pre t=100000 peers=2 ids={0,2}',
        '[10:02:00.001] [JOIN] INFO: SCENARIO relink issued t=100000 n=1',
        '[10:02:00.002] [JOIN] INFO: [coop-ui] RELINK src=scenario tid=2222',
        '[10:02:00.010] [JOIN] INFO: [coop-ui] connect: role=JOIN transport=udp peer=0 ownRanks={1} src=role',
        '[10:02:00.050] [JOIN] INFO: [net] roster-reset where=stop role=client slots=0 ids={} epochs=1',
        '[10:02:00.070] [JOIN] INFO: [net] roster-reset where=launch role=client slots=0 ids={} epochs=0',
        '[10:02:06.000] [JOIN] INFO: SCENARIO relink post t=106000 peers=2 ids={0,2} recoveredMs=6000 ok=1',
        '[10:04:00.000] [JOIN] INFO: SCENARIO relink pre t=220000 peers=2 ids={0,2}',
        '[10:04:00.001] [JOIN] INFO: SCENARIO relink issued t=220000 n=2',
        '[10:04:00.002] [JOIN] INFO: [coop-ui] RELINK src=scenario tid=2222',
        '[10:04:00.010] [JOIN] INFO: [coop-ui] connect: role=JOIN transport=udp peer=0 ownRanks={1} src=role',
        '[10:04:00.050] [JOIN] INFO: [net] roster-reset where=stop role=client slots=0 ids={} epochs=1',
        '[10:04:00.070] [JOIN] INFO: [net] roster-reset where=launch role=client slots=0 ids={} epochs=0',
        '[10:04:05.000] [JOIN] INFO: SCENARIO relink post t=225000 peers=2 ids={0,2} recoveredMs=5000 ok=1',
        '[10:04:05.100] [JOIN] INFO: SCENARIO RESULT PASS'
    )
    Set-Content -Path (Join-Path $Dir 'join1.log') -Value $join1 -Encoding UTF8

    # join2: the OBSERVER - the independent second log.
    $join2 = @(
        '[10:00:06.000] [JOIN] INFO: [coop-ui] MAINTID tid=3333',
        '[10:00:06.100] [JOIN] INFO: [net] roster-reset where=launch role=client slots=0 ids={} epochs=0',
        '[10:00:20.000] [JOIN] INFO: SCENARIO relink prearm t=300 peers=2 ids={0,1}',
        '[10:00:21.000] [JOIN] INFO: SCENARIO relink start host=0 localId=2 side=observe role=both atMs=40000 recoverMs=60000 count=2 adapter=1',
        '[10:01:00.100] [JOIN] INFO: SCENARIO relink peer t=40100 peers=0 ids={}',
        '[10:01:07.500] [JOIN] INFO: SCENARIO relink peer t=47500 peers=2 ids={0,1}',
        '[10:02:00.100] [JOIN] INFO: SCENARIO relink peer t=100100 peers=1 ids={0}',
        '[10:02:06.500] [JOIN] INFO: SCENARIO relink peer t=106500 peers=2 ids={0,1}',
        '[10:02:06.600] [JOIN] INFO: SCENARIO relink observed t=106600 drop=1 cycles=2/2 peak=2 via=cycles',
        '[10:02:06.700] [JOIN] INFO: SCENARIO RESULT PASS'
    )
    Set-Content -Path (Join-Path $Dir 'join2.log') -Value $join2 -Encoding UTF8
}

function Invoke-Judge {
    param([string]$Dir)
    & powershell -NoProfile -ExecutionPolicy Bypass -File $judge -RunDir $Dir -ExpectedInstances 3 -Quiet 2>&1 | Out-Null
    return $LASTEXITCODE
}

function New-MutatedFixture {
    param([string]$Name, [scriptblock]$Mutate)
    $dir = Join-Path $fixRoot $Name
    New-GoodFixture -Dir $dir
    $hp    = Join-Path $dir 'host.log'
    $lines = @(Get-Content -Path $hp)
    $lines = @(& $Mutate $lines)
    Set-Content -Path $hp -Value $lines -Encoding UTF8
    return $dir
}

try {
    $good = Join-Path $fixRoot 'good'
    New-GoodFixture -Dir $good
    $goodExit = Invoke-Judge -Dir $good
    # Asserted in its own right: a judge that always FAILs is not evidence either.
    Check "judge PASSes the good fixture (exit 0, got $goodExit)" ($goodExit -eq 0)

    # M1 - the boundary trace removed entirely.
    $d = New-MutatedFixture 'm1_no_roster_reset' { param($L) @($L | Where-Object { $_ -notmatch 'roster-reset' }) }
    $e = Invoke-Judge -Dir $d
    Check "judge FAILs when the roster-reset line is missing (exit 1, got $e)" ($e -eq 1)

    # M2 - the boundary reset an ALREADY-EMPTY roster: green-looking, no evidence.
    $d = New-MutatedFixture 'm2_slots_zero' { param($L) @($L | ForEach-Object { $_ -replace 'where=stop role=host slots=2 ids=\{1,2\}', 'where=stop role=host slots=0 ids={}' }) }
    $e = Invoke-Judge -Dir $d
    Check "judge FAILs when a boundary discarded an empty roster (exit 1, got $e)" ($e -eq 1)

    # M3 - THE DISCRIMINATOR: the rejoin climbed to a higher id (the pre-fix shape).
    $d = New-MutatedFixture 'm3_ids_climbed' { param($L) @($L | ForEach-Object { if ($_ -match '^\[10:01:06\.000\]') { $_ -replace 'id=1 player=1', 'id=3 player=3' } else { $_ } }) }
    $e = Invoke-Judge -Dir $d
    Check "judge FAILs when the re-admitted ids differ from the discarded ones (exit 1, got $e)" ($e -eq 1)

    # M4 - the terminal pre-fix symptom.
    $d = New-MutatedFixture 'm4_slots_full' { param($L) $L + '[10:03:06.000] [HOST] ERROR: [net] peer rejected: MAX_PLAYERS=4 slots full' }
    $e = Invoke-Judge -Dir $d
    Check "judge FAILs when a 'slots full' rejection is present (exit 1, got $e)" ($e -eq 1)

    # M5 - the relink was NOT issued on the game thread (D-04).
    $d = New-MutatedFixture 'm5_wrong_tid' { param($L) @($L | ForEach-Object { $_ -replace 'RELINK src=scenario tid=1234', 'RELINK src=scenario tid=9999' }) }
    $e = Invoke-Judge -Dir $d
    Check "judge FAILs when the RELINK tid differs from the MAINTID (exit 1, got $e)" ($e -eq 1)

    # M6 - the slot leak measured live on 2026-09-12: a client relink whose old
    # peer has not timed out yet, so the host holds BOTH the stale id and the
    # newly assigned one. Placed after the boundary's own recovery post line, so
    # only check h can see it - which is the point of h existing separately.
    $d = New-MutatedFixture 'm6_phantom_slot' { param($L)
        $out = @()
        foreach ($ln in $L) {
            $out += $ln
            if ($ln -match '^\[10:01:07\.100\]') {
                $out += '[10:02:30.000] [HOST] INFO: [net] peer connected id=3 player=3 (proto v61)'
            }
        }
        @($out)
    }
    $e = Invoke-Judge -Dir $d
    Check "judge FAILs when the host holds a phantom extra slot (exit 1, got $e)" ($e -eq 1)

    # A judge pointed at nothing must report "could not run" (2), not a verdict.
    $empty = Join-Path $fixRoot 'empty'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $e = Invoke-Judge -Dir $empty
    Check "judge exits 2 (could not run) on a run dir with no logs (got $e)" ($e -eq 2)
} finally {
    if (Test-Path $fixRoot) { Remove-Item -Recurse -Force -Path $fixRoot -ErrorAction SilentlyContinue }
}

# This suite NEVER launches a game and never invokes relink_probe.ps1 against a
# real rig - the live proof is a run of scripts\relink_probe.ps1, recorded in
# the plan's SUMMARY, not exercised here.

# ---- summary ------------------------------------------------------------------
Write-Host ""
Write-Host ("RelinkLever: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
