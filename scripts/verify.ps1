<#
.SYNOPSIS
  Zero-game verification gate: the fast, game-independent safety net every
  commit and every refactor phase must keep green. Wraps the C++ unit layer
  (prototest - wire contract, content hash, interpolation buffer, and the other
  pure units) and the PowerShell harness contract fixtures (manifest schema,
  scenario drift, oracle registry, verdict rule) into a single PASS/FAIL.

  Requires NO Kenshi launch and NO KenshiCoop.dll - only dist\prototest.exe (a
  ~30 KB standalone) and the scripts. Run it before the two-client regression
  matrix (scripts\regress.ps1), which needs the game.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1

.EXAMPLE
  # Reuse an already-built prototest (skip the compile step).
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    # Reuse the existing dist\prototest.exe instead of rebuilding it.
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

$overall = $true

# ---- 1. C++ unit layer (prototest: wire/hash/interp + the pure units) ----------
Write-Host "############################################################"
Write-Host "# verify: C++ unit layer (prototest)"
Write-Host "############################################################"
$prototest = Join-Path $repoRoot "dist\prototest.exe"
if (-not $SkipBuild) {
    Write-Host "=== build prototest ==="
    & cmd.exe /c "`"$scriptDir\build_prototest.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "verify: FAIL (prototest build failed, exit $LASTEXITCODE)"; exit 1 }
}
if (Test-Path $prototest) {
    & $prototest
    $unitOk = ($LASTEXITCODE -eq 0)
    Write-Host ("UNIT LAYER: " + $(if ($unitOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $unitOk) { $overall = $false }
} else {
    Write-Host "UNIT LAYER: FAIL - dist\prototest.exe not found (run without -SkipBuild)"
    $overall = $false
}

# ---- 2. PowerShell contract / drift fixtures (zero game) -----------------------
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: harness contract fixtures"
Write-Host "############################################################"
$fixtures = Join-Path $scriptDir "tests\Contract.Tests.ps1"
if (Test-Path $fixtures) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $fixtures
    $fixOk = ($LASTEXITCODE -eq 0)
    Write-Host ("CONTRACT FIXTURES: " + $(if ($fixOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $fixOk) { $overall = $false }
} else {
    Write-Host "CONTRACT FIXTURES: FAIL - scripts\tests\Contract.Tests.ps1 not found"
    $overall = $false
}

# ---- 3. Per-oracle fixtures on synthetic logs (zero game) ----------------------
# An oracle judged only by its own green runs is unfalsifiable: these feed each
# gate a log pair shaped like the BUG it exists for and require a FAIL, so a
# passing regression run means the gate looked rather than that it cannot look.
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: oracle fixtures (synthetic logs)"
Write-Host "############################################################"
$oracleOk = $true
foreach ($f in @("tests\CrawlMove.Fixture.ps1")) {
    $p = Join-Path $scriptDir $f
    if (-not (Test-Path $p)) {
        Write-Host "ORACLE FIXTURE: FAIL - scripts\$f not found"
        $oracleOk = $false
        continue
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $p
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("ORACLE FIXTURE ${f}: FAIL (exit $LASTEXITCODE)")
        $oracleOk = $false
    }
}
Write-Host ("ORACLE FIXTURES: " + $(if ($oracleOk) { "PASS" } else { "FAIL" }))
if (-not $oracleOk) { $overall = $false }

# ---- 4. Routing matrix completeness (zero game) ---------------------------------
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: routing matrix completeness"
Write-Host "############################################################"
$routing = Join-Path $scriptDir "tests\RoutingMatrix.Tests.ps1"
if (Test-Path $routing) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $routing
    $routingOk = ($LASTEXITCODE -eq 0)
    Write-Host ("ROUTING MATRIX: " + $(if ($routingOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $routingOk) { $overall = $false }
} else {
    Write-Host "ROUTING MATRIX: FAIL - scripts\tests\RoutingMatrix.Tests.ps1 not found"
    $overall = $false
}

# ---- 5. Census-race reproducer rig shape + anti-escape pins (zero game) --------
# Phase 12 plan 01 (CENSUS-03): the N=3 simultaneous-join reproducer's rig
# shape and the three success-criterion-3 anti-escape pins (oracle tolerance,
# rig stagger, netsim knob defaults) must stay in this fast sweep - a drift on
# any of them is exactly what the two known masking levers exploit.
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: census-race reproducer guard"
Write-Host "############################################################"
$censusRepro = Join-Path $scriptDir "tests\CensusRepro.Tests.ps1"
if (Test-Path $censusRepro) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $censusRepro
    $censusReproOk = ($LASTEXITCODE -eq 0)
    Write-Host ("CENSUS REPRO GUARD: " + $(if ($censusReproOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $censusReproOk) { $overall = $false }
} else {
    Write-Host "CENSUS REPRO GUARD: FAIL - scripts\tests\CensusRepro.Tests.ps1 not found"
    $overall = $false
}

# ---- 6. Connect-relink lever shape + judge sensitivity (zero game) -------------
# Phase 14 plan 02 (UI-06/UI-07): the harness-drivable connect lever's source
# shape (adapter wired at every ScenarioContext site, connect sequence still
# single, lever harness-only, Config/wire pins) plus the five mutation fixtures
# that prove scripts\analyze_relink.ps1 can FAIL. A judge that cannot fail is
# not evidence, so its sensitivity belongs in the fast sweep, not in a run log.
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: connect-relink lever guard"
Write-Host "############################################################"
$relinkLever = Join-Path $scriptDir "tests\RelinkLever.Tests.ps1"
if (Test-Path $relinkLever) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $relinkLever
    $relinkLeverOk = ($LASTEXITCODE -eq 0)
    Write-Host ("RELINK LEVER GUARD: " + $(if ($relinkLeverOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $relinkLeverOk) { $overall = $false }
} else {
    Write-Host "RELINK LEVER GUARD: FAIL - scripts\tests\RelinkLever.Tests.ps1 not found"
    $overall = $false
}

# ---- 7. Player installer: round trip, backups, prerequisites (zero game) -------
# Phase 16 plan 01 (INST-01/03/04/05): the installer writes into someone else's
# game directory, so its reversal is proved by whole-tree hash rather than by an
# exit code, and its backups are proved to be content-addressed. Enumerated by
# GLOB rather than by filename so plans 16-02 and 16-03 add coverage without
# editing this file again.
Write-Host ""
Write-Host "############################################################"
Write-Host "# verify: player installer guard"
Write-Host "############################################################"
$installerTests = @(Get-ChildItem -Path (Join-Path $scriptDir "tests") -Filter "Installer*.Tests.ps1" -File -ErrorAction SilentlyContinue | Sort-Object Name)
$installerOk = $true
if ($installerTests.Count -eq 0) {
    # An empty glob is a FAIL, not a silent skip: a suite that was renamed or
    # deleted would otherwise turn this guard green by disappearing.
    Write-Host "INSTALLER GUARD: FAIL - no scripts\tests\Installer*.Tests.ps1 matched the glob"
    $installerOk = $false
} else {
    foreach ($f in $installerTests) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $f.FullName
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("INSTALLER SUITE $($f.Name): FAIL (exit $LASTEXITCODE)")
            $installerOk = $false
        }
    }
}
Write-Host ("INSTALLER GUARD: " + $(if ($installerOk) { "PASS" } else { "FAIL" }))
if (-not $installerOk) { $overall = $false }

# ---- summary -------------------------------------------------------------------
Write-Host ""
Write-Host "================= VERIFY SUMMARY ================="
Write-Host ("  unit layer (prototest):   " + $(if ($unitOk) { "PASS" } else { "FAIL" }))
Write-Host ("  contract fixtures:        " + $(if ($fixOk)  { "PASS" } else { "FAIL" }))
Write-Host ("  oracle fixtures:          " + $(if ($oracleOk) { "PASS" } else { "FAIL" }))
Write-Host ("  routing matrix:           " + $(if ($routingOk) { "PASS" } else { "FAIL" }))
Write-Host ("  census repro guard:       " + $(if ($censusReproOk) { "PASS" } else { "FAIL" }))
Write-Host ("  relink lever guard:       " + $(if ($relinkLeverOk) { "PASS" } else { "FAIL" }))
Write-Host ("  installer guard:          " + $(if ($installerOk) { "PASS" } else { "FAIL" }))
Write-Host ("OVERALL: " + $(if ($overall) { "PASS" } else { "FAIL" }))
if ($overall) { exit 0 } else { exit 1 }
