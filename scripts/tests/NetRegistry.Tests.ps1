<#
.SYNOPSIS
  Log-oracle wrapper for dist\nettest.exe (Phase 2, Plan 03 - NET-05/PEER-01).

.DESCRIPTION
  Runs the headless multi-peer registry test (src\nettest\main.cpp, linking
  the real src\plugin\net\NetLink.cpp production code path over real ENet UDP
  loopback) and asserts, from its captured stdout, the three facts the phase's
  VALIDATION.md Wave 0 requirement calls for:
    (a) nettest itself exits 0 (every internal CHECK passed - roster,
        disconnect isolation, and reconnect slot reuse included).
    (b) 3 distinct player ids were assigned to the 3 simultaneous clients.
    (c) the disconnect/reconnect cycle reused the freed slot (id=2) without
        renumbering either surviving client.

  Builds dist\nettest.exe first if it is missing or older than its sources,
  mirroring how RoutingMatrix.Tests.ps1/Contract.Tests.ps1 are meant to be
  runnable standalone. The plan's own per-task/per-wave <verify> commands
  build explicitly first anyway (`cmd /c scripts\build_nettest.cmd && ...`),
  so this is a convenience fallback, not the only build path.

  Exit code = number of failed assertions (0 = PASS), matching the
  Contract.Tests.ps1 / RoutingMatrix.Tests.ps1 / prototest convention so
  verify.ps1/regress.ps1 can sum them.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\tests\NetRegistry.Tests.ps1
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
# (verbatim shape from scripts\tests\Contract.Tests.ps1:40-47)
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

# ---- (b) 3 distinct assigned player ids appear ---------------------------------
Check "nettest observed 3 distinct assigned PlayerIds" `
    ($outText -match '(?m)^\s*ok\s+3 distinct PlayerIds assigned\s*$')
Check "nettest confirmed all assigned PlayerIds within \{1,2,3\}" `
    ($outText -match '(?m)^\s*ok\s+all assigned PlayerIds within \{1,2,3\}\s*$')

# ---- (c) disconnect/reconnect cycle: freed id reused, no other id renumbered ---
Check "nettest confirmed disconnect isolation (surviving clients kept their ids)" `
    ($outText -match '(?m)^\s*ok\s+disconnect isolation: surviving clients kept their own PlayerIds\s*$')
Check "nettest confirmed the freed slot (id=2) was reclaimed on reconnect, not renumbered" `
    ($outText -match '(?m)^\s*ok\s+reconnect: freed slot id=2 reclaimed \(lowest-free-slot reuse, no renumber\)\s*$')
Check "nettest confirmed surviving clients were NOT renumbered after the reconnect" `
    ($outText -match '(?m)^\s*ok\s+reconnect: surviving clients were NOT renumbered\s*$')

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("NetRegistry: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
