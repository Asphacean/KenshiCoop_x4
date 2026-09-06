<#
.SYNOPSIS
  Offline N-instance oracle runner: judge a collected host+joins run dir with
  the SAME verdict rule a live run_test4.ps1 session would use (Phase 4 Plan
  05, POC-03; N-parameterized Phase 11 Plan 02, TEST-01).

.DESCRIPTION
  This is scripts\analyze_run.ps1's N-instance counterpart, generalized the
  same way scripts\analyze_wnpc_diff4.ps1 (Plan 03) generalized its 2-log
  ancestor: a new sibling script, not an edit - analyze_run.ps1 stays
  byte-unchanged so the existing 2-log matrix is never put at risk
  (04-RESEARCH.md "Don't Hand-Roll", T-04-14).

  N (host + joins) is resolved from -ExpectedInstances, else run_meta.json's
  instanceCount, else 4 (see the param's own doc comment) - so this script
  judges N=2/N=3/N=4 runs through the exact same generic path.

  HARD REQUIREMENT (04-RESEARCH.md Pitfall 5): scripts\CoopOraclesN.psm1's
  Assert-AllLogsPresent runs FIRST, before any other gate. A run missing or
  emptying any of the resolved N required logs FAILS LOUD immediately - it is
  never silently judged on however many happen to be present, and no PASS
  verdict from this script can have examined fewer than N instances.

  Resolves the -Scenario manifest entry (scripts\scenarios.psd1, default
  milestone_a_gate) for its Gating/Advisory/PrimaryGate lists and Tolerance,
  dispatches each id through CoopOraclesN's Invoke-OneOracleN, applies the
  same always-on health/result/check-fail gates and no-signal PrimaryGate
  rule scripts\CoopOracles.psm1's Invoke-RunAnalysis uses for the 2-log case,
  and writes a verdict.json into the run dir in the same shape (timestamp/
  scenario/tolerance/pass/reasons/primary/gating/advisory/run/gates) for
  cross-run trending. This dispatch is entirely manifest-driven (no gate id
  is hardcoded here), so `-Scenario item_conservation_gate` (Phase 7 plan 03)
  and `-Scenario world_state_gate` (Phase 8 plan 03, Test-WorldState) are
  judged through the exact same generic path as milestone_a_gate - no
  per-scenario code in this script.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\analyze_run4.ps1 -RunDir tools\test-runs\20260902_120000_N4

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\analyze_run4.ps1 -RunDir tools\test-runs\20260903_100000_N4 -Scenario world_state_gate
#>
[CmdletBinding()]
param(
    # A run dir containing host.log/join1.log/join2.log/join3.log (run_test4.ps1's
    # naming convention) ...
    [string]$RunDir = "",
    # ... or explicit per-instance log path overrides.
    [string]$HostLog = "",
    [string]$Join1Log = "",
    [string]$Join2Log = "",
    [string]$Join3Log = "",
    [string]$Scenario = "milestone_a_gate",
    [double]$Tolerance = 0,
    # Where to write verdict.json (defaults beside the host log, same filename
    # the 2-log judge uses, so cross-run trending tools find either shape the
    # same way - the two never collide because run_test4.ps1 always gives the
    # N=4 gate its own dedicated run dir).
    [string]$OutJson = "",
    # Rig config fallback for resolving the scheduled-disconnect exempt set
    # when the run dir predates run_meta.json (offline re-judge of an older
    # archived run). Never required - a run with a live-written run_meta.json
    # never needs it (04-08-PLAN.md gap 3).
    [string]$RigConfig = "",
    # Phase 11 plan 02 (TEST-01): total instance count (host + joins) this run
    # declares. 0 (default/unset) means "resolve it" - precedence is (i) this
    # param when > 0, (ii) run_meta.json's instanceCount (the offline-re-judge
    # bridge: an archived N<4 run has no gitignored rig config to read N from),
    # (iii) 4 (today's only-ever-tested shape, so an older archived run with
    # neither this param nor an instanceCount field still judges as N=4 -
    # byte-identical to this script's pre-Phase-11 behavior).
    [int]$ExpectedInstances = 0
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Import-Module (Join-Path $scriptDir "CoopOraclesN.psm1") -Force

if ($HostLog -eq ""  -and $RunDir -ne "") { $HostLog  = Join-Path $RunDir "host.log" }
if ($HostLog -eq "") { throw "No -RunDir and no -HostLog given: nothing to judge." }

# ---- Resolve N (Phase 11 plan 02, TEST-01) -----------------------------------
$resolvedN = 4
if ($ExpectedInstances -gt 0) {
    $resolvedN = $ExpectedInstances
} elseif ($RunDir -ne "" -and (Test-Path (Join-Path $RunDir "run_meta.json"))) {
    $rm = Get-Content -Raw -Path (Join-Path $RunDir "run_meta.json") | ConvertFrom-Json
    if ($null -ne $rm.instanceCount -and [int]$rm.instanceCount -gt 0) { $resolvedN = [int]$rm.instanceCount }
}
if ($resolvedN -lt 2) { throw "Resolved N=$resolvedN is not a valid instance count (minimum 2: host + 1 join)." }

# ---- Build the N-sized log set: host + join1..join(N-1) ----------------------
# Explicit per-instance overrides (-Join1Log/-Join2Log/-Join3Log) are honored
# up to whatever N actually needs; a join beyond N-1 is never added to $logs,
# not even when its override param was passed (Pitfall-5 fail-loud stays
# scoped to exactly the N declared logs, never more, never fewer).
$logs = [ordered]@{ host = $HostLog }
$joinOverrides = @{ 1 = $Join1Log; 2 = $Join2Log; 3 = $Join3Log }
for ($i = 1; $i -lt $resolvedN; $i++) {
    $override = if ($joinOverrides.ContainsKey($i)) { $joinOverrides[$i] } else { "" }
    if ($override -eq "" -and $RunDir -ne "") { $override = Join-Path $RunDir "join$i.log" }
    $logs["join$i"] = $override
}

if ($OutJson -eq "") { $OutJson = Join-Path (Split-Path -Parent $HostLog) "verdict.json" }

Write-Host "== analyze_run4: $Scenario (N=$resolvedN) =="
foreach ($name in $logs.Keys) { Write-Host "  ${name}: $($logs[$name])" }
Write-Host ""

Reset-GateResults

# ---- Pitfall 5 hard gate: FIRST, before anything else is judged --------------
# Fail-loud is preserved exactly: ANY of the $resolvedN declared logs missing
# or empty still fails immediately - "honest, not vacuous-pass" (must_haves).
$logsPresentStatus = Assert-AllLogsPresent -Logs $logs -ExpectedCount $resolvedN
if ($logsPresentStatus -ne "PASS") {
    $gate = @(Get-GateResults) | Where-Object { $_.gate -eq "logs_present" } | Select-Object -Last 1
    $reason = "logs_present FAIL: $($gate.detail)"
    Write-Host ""
    Write-Host "== Gate summary =="
    Write-Host ("  {0,-22} {1,-4} - {2}" -f "logs_present", $logsPresentStatus, $gate.detail)
    Write-Host "  verdict reasons: $reason"
    $verdict = [pscustomobject]@{
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
        scenario  = $Scenario
        tolerance = $Tolerance
        pass      = $false
        reasons   = @($reason)
        primary   = ""
        gating    = @()
        advisory  = @()
        run       = @{ offline = $true; runDir = $RunDir; logs = $logs }
        gates     = @(Get-GateResults)
    }
    $verdict | ConvertTo-Json -Depth 6 | Set-Content -Path $OutJson -Encoding UTF8
    Write-Host "  verdict json: $OutJson"
    Write-Host ""
    Write-Host "RESULT: FAIL"
    exit 1
}

# ---- Clock alignment visibility (host is the reference clock; offset 0) -----
foreach ($name in ($logs.Keys | Where-Object { $_ -ne "host" })) {
    $off = Get-LogClockOffsetMs -File $logs[$name]
    Write-Host "  $name clock offset applied: ${off}ms (from CLOCKSYNC; 0 = none logged)"
}
Write-Host ""

# ---- Scheduled-disconnect exempt-set resolution (gap 3, locked user decision:
# keep Stop-Process -Force; exempt via the disconnectAtSec marker ONLY - never
# a hardcoded instance name, and never the reconnect replacement). Precedence:
#   (i)   run_meta.json archived in the run dir (what a live run_test4.ps1
#         session writes - the offline-re-judgeable, config-derived marker);
#   (ii)  -RigConfig's raw instances (offline re-judge of an OLDER archived
#         run that predates run_meta.json, e.g. this plan's own target run);
#   (iii) empty (no exemption) - clean_exit/health_*/result_* judge every
#         instance at full strictness, same as before this gap-closure plan.
$exemptLabels = @()
$runMetaPath = if ($RunDir -ne "") { Join-Path $RunDir "run_meta.json" } else { "" }
if ($runMetaPath -ne "" -and (Test-Path $runMetaPath)) {
    $runMetaJson = Get-Content -Raw -Path $runMetaPath | ConvertFrom-Json
    $exemptLogNames = @($runMetaJson.scheduledDisconnect)
    foreach ($name in $logs.Keys) {
        if ($exemptLogNames -contains (Split-Path -Leaf $logs[$name])) { $exemptLabels += $name }
    }
    Write-Host "  exempt set (scheduled-disconnect, from run_meta.json): $(if ($exemptLabels.Count -gt 0) { $exemptLabels -join ', ' } else { '(none)' })"
} elseif ($RigConfig -ne "" -and (Test-Path $RigConfig)) {
    $rig = Get-Content -Raw -Path $RigConfig | ConvertFrom-Json
    $exemptLogNames = New-Object System.Collections.ArrayList
    if ($null -ne $rig.instances) {
        foreach ($inst in $rig.instances) {
            if ($null -ne $inst.disconnectAtSec -and "$($inst.disconnectAtSec)" -ne "") {
                if ($inst.logName) { [void]$exemptLogNames.Add("$($inst.logName)") }
            }
        }
    }
    foreach ($name in $logs.Keys) {
        if ($exemptLogNames -contains (Split-Path -Leaf $logs[$name])) { $exemptLabels += $name }
    }
    Write-Host "  exempt set (scheduled-disconnect, from -RigConfig '$RigConfig'): $(if ($exemptLabels.Count -gt 0) { $exemptLabels -join ', ' } else { '(none)' })"
} else {
    Write-Host "  exempt set (scheduled-disconnect): (none - no run_meta.json and no -RigConfig)"
}
Write-Host ""

# ---- Manifest lookup (Gating/Advisory/PrimaryGate/Tolerance) -----------------
$manifest = Get-ScenarioManifest
$entry = $null
if ($manifest.Scenarios.ContainsKey($Scenario)) { $entry = $manifest.Scenarios[$Scenario] }

if ($Tolerance -eq 0) {
    $Tolerance = 6.0
    if ($null -ne $entry -and $entry.ContainsKey("Tolerance")) { $Tolerance = $entry.Tolerance }
}

$defaultGating = @("census_convergence", "disconnect_isolation", "driven_only_movement", "desync_convergence", "clean_exit")
$gating = @(); $advisory = @(); $primary = "census_convergence"; $noSignalFails = @()
if ($null -ne $entry) {
    $gating   = @($entry.Gating)
    $advisory = @($entry.Advisory)
    $primary  = $entry.PrimaryGate
    if ($entry.ContainsKey("NoSignalFails")) { $noSignalFails = @($entry.NoSignalFails) }
} else {
    Write-Host "  WARNING: scenario '$Scenario' not in manifest; using the default N=4 gate set"
    $gating = $defaultGating
}

# ---- 1. Log health + scenario-result (always, per instance) -----------------
# An exempt (scheduled-disconnect) instance never reaches "SCENARIO RESULT" by
# construction (Stop-Process -Force is a hard kill, not a graceful scenario
# exit) - health_<label>/result_<label> require only that it REACHED GAMEPLAY
# (its own "SCENARIO MAGATE start" line), so a real early crash/launch
# failure on the exempt instance is still caught, never hidden (gap 3).
foreach ($name in $logs.Keys) {
    if ($exemptLabels -contains $name) {
        [void](Test-LogHealth -File $logs[$name] -Label $name -Required $true -CleanPattern "SCENARIO MAGATE start")
    } else {
        [void](Test-LogHealth -File $logs[$name] -Label $name -Required $true -CleanPattern "SCENARIO RESULT")
    }
}
foreach ($name in $logs.Keys) {
    if ($exemptLabels -contains $name) {
        $gate = "result_$name"
        $reached = (Test-Path $logs[$name]) -and `
            ($null -ne (Select-String -Path $logs[$name] -Pattern 'SCENARIO MAGATE start' -ErrorAction SilentlyContinue | Select-Object -First 1))
        if ($reached) {
            Write-Host "  $name SCENARIO RESULT exempt (scheduled-disconnect, disconnectAtSec) - reached gameplay, harness force-killed"
            [void](Add-GateResult -Name $gate -Status PASS -Detail "scheduled-disconnect exempt (disconnectAtSec) - reached gameplay, harness force-killed")
        } else {
            Write-Host "  $name SCENARIO RESULT exempt but never reached gameplay - not hidden, treated as a real failure"
            [void](Add-GateResult -Name $gate -Status FAIL -Detail "scheduled-disconnect exempt (disconnectAtSec) but never reached gameplay (no SCENARIO MAGATE start) - real early crash/launch failure, not hidden by the exemption")
        }
    } else {
        [void](Test-ScenarioResultPass -File $logs[$name] -Label $name -Required $true)
    }
}

# ---- 1a. In-plugin CHECK-line failures across all 4 logs (always) -----------
$checkFailCount = 0
foreach ($name in $logs.Keys) {
    $fails = @(Select-String -Path $logs[$name] -Pattern "CHECK \S+ FAIL" -ErrorAction SilentlyContinue)
    foreach ($f in $fails) { Write-Host "  [$name] $($f.Line.Trim())" }
    $checkFailCount += $fails.Count
}
[void](Add-GateResult -Name "check_fail" -Status $(if ($checkFailCount -eq 0) { "PASS" } else { "FAIL" }) `
            -Metrics @{ failLines = $checkFailCount })

# ---- 2. The N-instance gate set (Gating + Advisory) --------------------------
foreach ($id in ($gating + $advisory)) {
    [void](Invoke-OneOracleN -Id $id -Logs $logs -RunDir $RunDir -Tolerance $Tolerance -ExemptLabels $exemptLabels -ExpectedInstances $resolvedN)
}

# ---- 3. Verdict rule (mirrors CoopOracles.psm1's Invoke-RunAnalysis) --------
$gates = @(Get-GateResults)
$byName = @{}
foreach ($g in $gates) { $byName[$g.gate] = $g }
$reasons = @()
$alwaysOn = @("logs_present", "check_fail") + @($logs.Keys | ForEach-Object { "health_$_" }) + @($logs.Keys | ForEach-Object { "result_$_" })
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
# NoSignalFails: same no-signal guard as PrimaryGate above, extended to any
# other gate the manifest names - a SKIP on driven_only_movement/
# desync_convergence is an evidentiary gap (no positive judgment of the DoD
# behavior), not a pass, so it must contribute a verdict reason too (gap 4).
foreach ($n in $noSignalFails) {
    if (-not $byName.ContainsKey($n)) {
        $reasons += "no-signal-fails gate $n missing (no signal)"
    } elseif ($byName[$n].status -eq "SKIP") {
        $reasons += "no-signal-fails gate $n SKIP (no signal)"
    }
}
$pass = ($reasons.Count -eq 0)

# ---- 4. Summary table ---------------------------------------------------------
Write-Host ""
Write-Host "== Gate summary =="
foreach ($g in $gates) {
    $tag = ""
    if ($advisory -contains $g.gate) { $tag = " (advisory)" }
    elseif ($g.gate -eq $primary)    { $tag = " (primary)" }
    Write-Host ("  {0,-22} {1,-4}{2}{3}" -f $g.gate, $g.status, $tag,
                $(if ($g.detail -ne "") { " - " + $g.detail } else { "" }))
}
if (-not $pass) { Write-Host ("  verdict reasons: " + ($reasons -join "; ")) }

# ---- 5. verdict.json -----------------------------------------------------------
$verdict = [pscustomobject]@{
    timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    scenario  = $Scenario
    tolerance = $Tolerance
    pass      = $pass
    reasons   = $reasons
    primary   = $primary
    gating    = $gating
    advisory  = $advisory
    run       = @{ offline = $true; runDir = $RunDir; logs = $logs }
    gates     = $gates
}
$verdict | ConvertTo-Json -Depth 6 | Set-Content -Path $OutJson -Encoding UTF8
Write-Host "  verdict json: $OutJson"

Write-Host ""
Write-Host ("RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }))
if ($pass) { exit 0 } else { exit 1 }
