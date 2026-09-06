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

# ---- summary -------------------------------------------------------------------
Write-Host ""
Write-Host "================= VERIFY SUMMARY ================="
Write-Host ("  unit layer (prototest):   " + $(if ($unitOk) { "PASS" } else { "FAIL" }))
Write-Host ("  contract fixtures:        " + $(if ($fixOk)  { "PASS" } else { "FAIL" }))
Write-Host ("  oracle fixtures:          " + $(if ($oracleOk) { "PASS" } else { "FAIL" }))
Write-Host ("  routing matrix:           " + $(if ($routingOk) { "PASS" } else { "FAIL" }))
Write-Host ("OVERALL: " + $(if ($overall) { "PASS" } else { "FAIL" }))
if ($overall) { exit 0 } else { exit 1 }
