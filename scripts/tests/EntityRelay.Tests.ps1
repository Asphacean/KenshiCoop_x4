<#
.SYNOPSIS
  Log-oracle wrapper for dist\nettest.exe (Phase 3, Plan 04 - PEER-02/03).

.DESCRIPTION
  Runs the headless multi-peer registry/relay test (src\nettest\main.cpp,
  linking the real src\plugin\net\NetLink.cpp production code path over real
  ENet UDP loopback) and asserts, from its captured stdout, that:
    (a) nettest itself exits 0 (every internal CHECK passed).
    (b) the Plan 01 Class A relay-count/no-echo oracle is present (a join-
        authored entity batch still reaches every other connected client
        exactly once, with no echo to the author) - the relay path this
        plan's disconnect-isolation guarantee builds on.
    (c) the two Plan 04 disconnect-isolation-at-N>=3 relay oracles are
        present: a SURVIVING client's authored batch still reaches the OTHER
        surviving client after a peer leaves, and no batch attributed to the
        departed owner ever reaches a survivor post-leave.

  Builds dist\nettest.exe first if it is missing or older than its sources,
  mirroring scripts\tests\NetRegistry.Tests.ps1/RoutingMatrix.Tests.ps1. The
  plan's own per-task/per-wave <verify> commands build explicitly first
  anyway (`cmd /c scripts\build_nettest.cmd && ...`), so this is a
  convenience fallback, not the only build path.

  Exit code = number of failed assertions (0 = PASS), matching the
  NetRegistry.Tests.ps1 / RoutingMatrix.Tests.ps1 / prototest convention so
  verify.ps1/regress.ps1 can sum them.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\tests\EntityRelay.Tests.ps1
#>
[CmdletBinding()]
param(
    [string]$NettestExe = "dist\nettest.exe",
    [string]$BuildScript = "scripts\build_nettest.cmd"
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                        # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                      # repo root

function Resolve-InputPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $repoRoot $Path)
}
$exePath   = Resolve-InputPath $NettestExe
$buildPath = Resolve-InputPath $BuildScript

# ---- tiny assert harness ------------------------------------------------------
# (verbatim shape from scripts\tests\NetRegistry.Tests.ps1)
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

# ---- build if missing or stale -------------------------------------------------
$mainCpp    = Join-Path $repoRoot "src\nettest\main.cpp"
$netLinkCpp = Join-Path $repoRoot "src\plugin\net\NetLink.cpp"
$needsBuild = -not (Test-Path $exePath)
if (-not $needsBuild) {
    $exeTime = (Get-Item $exePath).LastWriteTimeUtc
    foreach ($src in @($mainCpp, $netLinkCpp)) {
        if ((Test-Path $src) -and (Get-Item $src).LastWriteTimeUtc -gt $exeTime) {
            $needsBuild = $true
            break
        }
    }
}
if ($needsBuild) {
    Write-Host "== building dist\nettest.exe (missing or stale) =="
    if (-not (Test-Path $buildPath)) {
        Write-Host "  FAIL build script not found: $buildPath"
        exit 1
    }
    & cmd /c "`"$buildPath`""
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL nettest build failed (exit $LASTEXITCODE)"
        exit 1
    }
}

# ---- run nettest.exe and capture stdout ----------------------------------------
Write-Host "== running dist\nettest.exe =="
if (-not (Test-Path $exePath)) {
    Write-Host "  FAIL nettest.exe not found at $exePath after build attempt"
    exit 1
}
$output = & $exePath 2>&1
$exitCode = $LASTEXITCODE
$output | ForEach-Object { Write-Host "  | $_" }

# ---- (a) nettest itself reports every internal CHECK green ---------------------
Check "nettest.exe exited 0 (every internal CHECK passed)" ($exitCode -eq 0)

$outText = ($output -join "`n")

# ---- (b) Plan 01 Class A relay-count/no-echo oracle present --------------------
Check "nettest confirmed the Class A relay-count oracle (author's batch reached both other clients, no echo)" `
    ($outText -match '(?m)^\s*ok\s+relay: author''s batch reached both other clients, author got no echo\s*$')

# ---- (c) Plan 04 disconnect-isolation-at-N>=3 relay oracles --------------------
Check "nettest confirmed a surviving client's batch still reaches the other survivor after a peer left" `
    ($outText -match '(?m)^\s*ok\s+disconnect isolation \(relay\): survivor''s batch still reached the other survivor exactly once after a peer left\s*$')
Check "nettest confirmed no batch from the departed owner reached survivors post-leave" `
    ($outText -match '(?m)^\s*ok\s+disconnect isolation \(relay\): no batch from the departed owner reached survivors post-leave\s*$')

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("EntityRelay: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
