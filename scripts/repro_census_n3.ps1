<#
.SYNOPSIS
  Committed one-command N=3 simultaneous-join census-race reproducer
  (Phase 12 Plan 01, CENSUS-03; success criterion 1's "reproduce" half).

.DESCRIPTION
  Someone who was not here runs ONE command and gets `milestone_a_gate` at
  N=3 with BOTH joins connecting undeferred and no injected netsim delay,
  judged by the existing offline oracle (scripts\analyze_run4.ps1). This is
  the reproducer every later Phase 12 plan measures its evidence through
  (.planning/debug/knowledge-base.md entry n3-census-join-schedule-race).

  STRUCTURAL ANTI-ESCAPE GUARD (T-12-01). Two levers are already proven to
  MASK the divergence rather than fix it: deferring one join's launch
  (reconnectAtSec) and adding receive-side netsim delay to one join. This
  script REFUSES TO RUN a rig config carrying either lever, or a rig that
  does not declare exactly 3 instances - it is structurally incapable of
  measuring a masked configuration and reporting the result as a clean
  reproduction. Use scripts\local.rig.n3.deferred-join.example.json (a
  separately named file, never this script's default) if the deferred-join
  baseline is what you actually want to run.

  Behavior, in order: parse + guard the rig; build the Harness configuration
  (unless -SkipBuild); deploy it into every instance's installDir (unless
  -SkipDeploy); run scripts\run_test4.ps1 (milestone_a_gate, N=3); judge the
  run with scripts\analyze_run4.ps1; record the run dir + verdict into
  tools\test-runs\last_census_repro.txt (stable pointer) and
  tools\test-runs\census_repro_history.jsonl (append-only history, written
  on both a passing and a failing run). Exits 0 whenever the script itself
  completed - the gate verdict is DATA, read from the
  "CENSUS REPRO RESULT: PASS|FAIL" / "CENSUS REPRO SUMMARY" lines or from
  verdict.json, never from this script's own exit code.

.EXAMPLE
  # Standard reproduction attempt (builds + deploys + runs once):
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\repro_census_n3.ps1

.EXAMPLE
  # Re-run 3 times without rebuilding/redeploying (binary already on disk):
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\repro_census_n3.ps1 -SkipBuild -SkipDeploy -Runs 3
#>
[CmdletBinding()]
param(
    [string]$RigConfig = "",
    [int]$Seconds = 210,
    [int]$Runs = 1,
    [switch]$SkipBuild,
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if ($RigConfig -eq "") { $RigConfig = Join-Path $scriptDir "local.rig.n3.json" }
if (-not (Test-Path $RigConfig)) {
    throw "Rig config not found: $RigConfig (copy scripts\local.rig.n3.example.json to scripts\local.rig.n3.json and fill in real install paths)."
}
$cfg = Get-Content -Raw -Path $RigConfig | ConvertFrom-Json

# ---- Structural anti-escape guard (T-12-01) --------------------------------
# This is not a lint - it is the mechanism that keeps this command incapable
# of reporting a masked configuration as a clean reproduction. See the two
# masking levers recorded in knowledge-base.md entry n3-census-join-schedule-race.
if ($null -eq $cfg.instances -or $cfg.instances.Count -ne 3) {
    $n = if ($null -eq $cfg.instances) { 0 } else { $cfg.instances.Count }
    throw "Rig config '$RigConfig' declares $n instance(s); this reproducer requires exactly 3 (the standard simultaneous-join N=3 shape). Use scripts\run_test4.ps1 directly with -ExpectedInstances if a different instance count is genuinely intended."
}
for ($i = 0; $i -lt $cfg.instances.Count; $i++) {
    $inst  = $cfg.instances[$i]
    $label = if ($inst.role -eq 'host') { "instance[$i] (host, $($inst.logName))" } else { "instance[$i] (join, $($inst.logName))" }

    if ($null -ne $inst.reconnectAtSec -and "$($inst.reconnectAtSec)" -ne "") {
        throw "Rig config '$RigConfig' $label carries a deferred launch (reconnectAtSec=$($inst.reconnectAtSec)) - refusing to run. A deferred join is one of the two known levers that MASKS the simultaneous-join census divergence (knowledge-base.md entry n3-census-join-schedule-race); a PASS produced with it is not a reproduction attempt. Set reconnectAtSec to null in the rig, or deliberately run scripts\local.rig.n3.deferred-join.example.json outside this script if you specifically want the deferred-join baseline."
    }
    if ($null -ne $inst.env) {
        foreach ($prop in $inst.env.PSObject.Properties) {
            if ($prop.Name -like 'KENSHICOOP_NETSIM*') {
                throw "Rig config '$RigConfig' $label declares netsim env key '$($prop.Name)' - refusing to run. Injected netsim delay is the second known lever that MASKS the simultaneous-join census divergence (knowledge-base.md entry n3-census-join-schedule-race); a PASS produced with it is not a reproduction attempt. Remove the KENSHICOOP_NETSIM_* key(s) from the rig to measure the true reproducer."
            }
        }
    }
}

Write-Host "== KenshiCoop N=3 simultaneous-join census-race reproducer (CENSUS-03) =="
Write-Host "  rig config: $RigConfig"
Write-Host "  seconds:    $Seconds"
Write-Host "  runs:       $Runs"

# ---- Build ------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== build Harness ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Harness"
    if ($LASTEXITCODE -ne 0) { throw "build_plugin.cmd Harness failed, exit $LASTEXITCODE" }
} else {
    Write-Host "  (SkipBuild: reusing whatever Harness DLL is already built)"
}

# ---- Deploy -------------------------------------------------------------------
if (-not $SkipDeploy) {
    Write-Host ""
    Write-Host "=== deploy Harness into every rig instance ==="
    foreach ($inst in $cfg.instances) {
        Write-Host "  deploy -> $($inst.installDir)"
        & cmd.exe /c "`"$scriptDir\deploy.cmd`" `"$($inst.installDir)`" Harness"
        if ($LASTEXITCODE -ne 0) { throw "deploy.cmd failed for installDir '$($inst.installDir)', exit $LASTEXITCODE" }
    }
} else {
    Write-Host "  (SkipDeploy: reusing whatever DLL is already deployed on each instance)"
}

$headSha = "unknown"
try {
    $headSha = (& git.exe -C $repoRoot rev-parse --short HEAD 2>$null).Trim()
    if ([string]::IsNullOrEmpty($headSha)) { $headSha = "unknown" }
} catch { $headSha = "unknown" }

$lastPointerPath = Join-Path $repoRoot "tools\test-runs\last_census_repro.txt"
$historyPath     = Join-Path $repoRoot "tools\test-runs\census_repro_history.jsonl"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $lastPointerPath) | Out-Null

$passCount = 0
$failCount = 0

for ($runIdx = 1; $runIdx -le $Runs; $runIdx++) {
    $stamp     = Get-Date -Format "yyyyMMdd_HHmmss"
    $runDirRaw = Join-Path $repoRoot "tools\test-runs\${stamp}_N3_repro"

    Write-Host ""
    Write-Host "=== run $runIdx/$Runs -> $runDirRaw ==="
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "run_test4.ps1") `
        -RigConfig $RigConfig -Scenario milestone_a_gate -ExpectedInstances 3 -Seconds $Seconds -OutDir $runDirRaw
    if ($LASTEXITCODE -ne 0) { throw "run_test4.ps1 failed (exit $LASTEXITCODE) on run $runIdx - see console output above." }

    if (-not (Test-Path $runDirRaw)) { throw "run_test4.ps1 reported success but run dir '$runDirRaw' does not exist." }
    $runDir = (Resolve-Path -LiteralPath $runDirRaw).Path

    Write-Host ""
    Write-Host "=== judge run $runIdx/$Runs (analyze_run4.ps1) ==="
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "analyze_run4.ps1") `
        -RunDir $runDir -ExpectedInstances 3
    $analyzeExit = $LASTEXITCODE

    $verdictPath = Join-Path $runDir "verdict.json"
    if (-not (Test-Path $verdictPath)) {
        throw "analyze_run4.ps1 exited $analyzeExit and produced no verdict.json in '$runDir' - a hard-requirement failure (e.g. a missing/empty log), not a gate FAIL. Inspect the console output above."
    }
    $verdict = Get-Content -Raw -Path $verdictPath | ConvertFrom-Json
    $gate    = @($verdict.gates | Where-Object { $_.gate -eq 'census_convergence' }) | Select-Object -First 1
    $gateStatus = if ($null -ne $gate) { $gate.status } else { "MISSING" }

    # Stable pointer: always overwritten, on both a passing and a failing run.
    Set-Content -Path $lastPointerPath -Value $runDir -Encoding UTF8 -NoNewline

    # Append-only history: always appended, on both a passing and a failing run
    # - a history that only records wins is not evidence.
    $historyEntry = [pscustomobject]@{
        timestamp          = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        runDir              = $runDir
        pass                = [bool]$verdict.pass
        census_convergence  = $gateStatus
        headSha             = $headSha
    }
    Add-Content -Path $historyPath -Value ($historyEntry | ConvertTo-Json -Compress)

    if ($verdict.pass) { $passCount++ } else { $failCount++ }
    $resultTag = if ($verdict.pass) { "PASS" } else { "FAIL" }
    Write-Host ""
    Write-Host "CENSUS REPRO RESULT: $resultTag runDir=$runDir"
}

Write-Host ""
Write-Host "CENSUS REPRO SUMMARY runs=$Runs pass=$passCount fail=$failCount"
exit 0
