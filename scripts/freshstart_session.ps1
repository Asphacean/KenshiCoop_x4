<#
.SYNOPSIS
  Stage, ASSERT and launch the TRUE fresh-install co-op condition on two Kenshi
  clones: the Release DLL, no coop_config.json on disk at all, and not one
  KENSHICOOP_* variable in the child environment.

.DESCRIPTION
  Phase 15's criterion 1 reads "a player starts or joins a session entirely from
  the in-game panel, having never seen an environment variable or hand-edited a
  config file". manual_session.ps1 cannot test that: it sets KENSHICOOP_MODE,
  KENSHICOOP_TRANSPORT, KENSHICOOP_PORT, KENSHICOOP_STEAM_PEER and
  KENSHICOOP_AUTOCONNECT on both clients by design. Testing the claim with the
  config file merely "left untouched" does not test it either - Config.cpp reads
  coop_config.json next to the DLL on every loadConfig(), so a stale file is an
  invisible input.

  THE DELIVERABLE HERE IS THE ASSERTION, NOT THE LAUNCHER. A fresh-start script
  that launches correctly but cannot prove the environment was clean leaves
  criterion 1 resting on an intention. So this script:

    1. deploys the RELEASE DLL (what a player gets - not the Harness build),
    2. DELETES coop_config.json from each clone and ASSERTS it is absent, at
       both locations Config.cpp::configFilePath() can resolve (next to the DLL,
       and the clone root cwd fallback),
    3. removes every KENSHICOOP_* variable from this process's environment - the
       one the launched games inherit - and ASSERTS none remains, against a name
       list SCANNED OUT OF src/ rather than hand-maintained,
    4. launches both clones tiled to the Kenshi TITLE SCREEN, no auto-load, no
       auto-connect (both default to role=host until the player flips the panel,
       so both write KenshiCoop_host.log into their own directory),
    5. on exit copies both logs into an absolute -OutDir and runs
       check_panel_log.ps1 over them.

  KENSHICOOP_LOG is deliberately NOT set: unset, the plugin writes its default
  log name into the process working directory, which is both what a real player
  gets and (WINDOWS #15) the way to avoid a relative log path being silently
  swallowed.

  Clones only. The script refuses, by name, to touch a real Steam install.

.PARAMETER NoLaunch
  Stage and assert, write the preflight record, and stop. Use when the machine
  has nobody at the keyboard: the fresh-install CONDITION is then proven and on
  record even though the session itself was not driven.

.PARAMETER LaunchSeconds
  Unattended boot probe: launch, wait N seconds, close both clients, collect.
  This proves the fresh-install BOOT path (plugin loads, default log lands in
  the clone directory) and NOTHING about criteria 1-3, which need a human at
  the panel. 0 = attended (wait for Enter).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\freshstart_session.ps1 -WhatIf

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\freshstart_session.ps1 -OutDir F:\PrivatProjects\KenshiCoop\KenshiCoop\tools\test-runs\freshstart
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # Client A - the one the player will set to HOST in the panel.
    [string]$ClientADir = "F:\KenshiCoop-Clone4",
    # Client B - the one the player will set to JOIN in the panel.
    [string]$ClientBDir = "F:\KenshiCoop-Clone1",
    # ABSOLUTE output directory for the preflight record and the collected logs.
    # A relative path is refused: WINDOWS #15 (a relative log/out path is
    # swallowed silently and nothing is written).
    [string]$OutDir = "",
    # Build Release first. Off by default: the deploy asserts the deployed DLL
    # is byte-identical to the repo's Release build either way.
    [switch]$Build,
    # Stage + assert + record, then stop (no Kenshi launched).
    [switch]$NoLaunch,
    # Unattended boot probe length in seconds (see .PARAMETER LaunchSeconds).
    [int]$LaunchSeconds = 0,
    [switch]$NoTile,
    [int]$WindowW = 1720,
    [int]$WindowH = 1440,
    [ValidateSet("widest", "primary")]
    [string]$TileMonitor = "widest",
    [int]$TileRepeatSec = 75,
    [int]$StartTimeoutSec = 90
)

$ErrorActionPreference = "Stop"
$INV = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$isWhatIf  = [bool]$WhatIfPreference

function Refuse([string]$reason) {
    Write-Host ""
    Write-Host "REFUSED: $reason"
    exit 3
}
function NowUtc() { (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", $INV) }
function Sha256([string]$path) {
    # .NET rather than Get-FileHash: Get-FileHash honours an inherited
    # -WhatIf and returns nothing, which would make the -WhatIf plan print a
    # blank hash for the very DLL it is about to deploy.
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($path))
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}
function NormDir([string]$p) {
    if (-not $p) { return "" }
    return ($p.TrimEnd('\', '/')).ToLowerInvariant()
}

Write-Host "=== freshstart_session: the fresh-install condition, asserted ==="
$modeText = if ($isWhatIf) { "WhatIf (nothing is changed or launched)" }
            elseif ($NoLaunch) { "stage + assert only (-NoLaunch)" }
            elseif ($LaunchSeconds -gt 0) { "unattended boot probe, $LaunchSeconds s" }
            else { "attended live session" }
Write-Host ("  mode: {0}" -f $modeText)

# ---------------------------------------------------------------- guard rails
# Never a real game install. Both known real installs on this project's machines
# are named explicitly so the refusal says WHICH one was aimed at.
$protectedInstalls = @(
    "F:\SteamLibrary\steamapps\common\Kenshi",
    "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$clients = @()
foreach ($pair in @(, @("A", $ClientADir)) + @(, @("B", $ClientBDir))) {
    $label = $pair[0]
    $dir   = $pair[1]
    if (-not $dir) { Refuse "client $label has no install directory." }
    if (-not (Test-Path -LiteralPath $dir)) { Refuse "client $label install directory does not exist: $dir" }
    $exe = Join-Path $dir "kenshi_x64.exe"
    if (-not (Test-Path -LiteralPath $exe)) { Refuse "client $label is not a Kenshi install (no kenshi_x64.exe): $dir" }
    foreach ($prot in $protectedInstalls) {
        if ((NormDir $dir) -eq (NormDir $prot)) {
            Refuse "client $label points at the REAL Kenshi install ($prot). This script deletes files and deploys builds - clones only."
        }
    }
    $clients += [pscustomobject]@{
        label       = $label
        dir         = $dir
        exe         = $exe
        modDir      = (Join-Path $dir "mods\KenshiCoop")
        configPaths = @(
            (Join-Path $dir "mods\KenshiCoop\coop_config.json"),
            (Join-Path $dir "coop_config.json")
        )
        logName     = "KenshiCoop_host.log"
        dllSha256   = ""
        dllBytes    = 0
        dllUtc      = ""
        configDeleted = @()
        configAbsent  = $false
    }
}
if ((NormDir $ClientADir) -eq (NormDir $ClientBDir)) {
    Refuse "client A and client B are the same directory ($ClientADir) - two clients need two installs."
}

$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq "Kenshi_x64" -or $_.Name -eq "kenshi_x64" })
if ($running.Count -gt 0 -and -not $isWhatIf) {
    $ids = ($running | ForEach-Object { "$($_.Id)" }) -join ", "
    Refuse "a Kenshi process is already running (PID $ids) - it locks KenshiCoop.dll and the Release deploy would fail. Close all Kenshi windows and retry."
}

# OutDir must be absolute (WINDOWS #15).
if (-not $OutDir) {
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss", $INV)
    $OutDir = Join-Path $repoRoot ("tools\test-runs\freshstart_" + $stamp)
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    Refuse "-OutDir must be an ABSOLUTE path (got '$OutDir'). A relative path is swallowed silently and no evidence is written."
}

$releaseDll = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"

# --------------------------------------------------------- 1. Release deploy
Write-Host ""
Write-Host "=== 1. deploy the RELEASE DLL (the build a player gets) ==="
if ($Build) {
    Write-Host "  build: scripts\build_plugin.cmd Release"
    if (-not $isWhatIf) {
        & cmd /c "`"$scriptDir\build_plugin.cmd`" Release"
        if ($LASTEXITCODE -ne 0) { Refuse "build_plugin.cmd Release failed ($LASTEXITCODE)." }
    }
}
if (-not (Test-Path -LiteralPath $releaseDll)) {
    Refuse "no Release DLL at $releaseDll - build it first (scripts\build_plugin.cmd Release) or pass -Build."
}
$releaseSha = Sha256 $releaseDll
Write-Host ("  source: {0}" -f $releaseDll)
Write-Host ("  sha256: {0}" -f $releaseSha)
foreach ($c in $clients) {
    Write-Host ("  client {0}: deploy Release -> {1}" -f $c.label, $c.modDir)
    if (-not $isWhatIf) {
        & cmd /c "`"$scriptDir\deploy.cmd`" `"$($c.dir)`" Release" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Refuse "deploy.cmd failed for client $($c.label) ($LASTEXITCODE) - is a Kenshi process holding the DLL?"
        }
        $deployed = Join-Path $c.modDir "KenshiCoop.dll"
        $sha = Sha256 $deployed
        if ($sha -ne $releaseSha) {
            Refuse "client $($c.label) holds a DLL that is not the repo's Release build (deployed $sha, expected $releaseSha)."
        }
        $info = Get-Item -LiteralPath $deployed
        $c.dllSha256 = $sha
        $c.dllBytes  = $info.Length
        $c.dllUtc    = $info.LastWriteTimeUtc.ToString("yyyy-MM-ddTHH:mm:ssZ", $INV)
        Write-Host ("    ok: deployed DLL sha256 matches the Release build ({0} bytes)" -f $info.Length)
    }
}

# ------------------------------------------- 2. delete + ASSERT coop_config.json
Write-Host ""
Write-Host "=== 2. coop_config.json: delete, then ASSERT ABSENT ==="
foreach ($c in $clients) {
    $deleted = @()
    $present = @()
    foreach ($p in $c.configPaths) {
        if (Test-Path -LiteralPath $p) {
            Write-Host ("  client {0}: DELETE {1}" -f $c.label, $p)
            if (-not $isWhatIf) {
                # try/catch, not just -ErrorAction: a delete that cannot succeed
                # must fall through to the ASSERT below and be refused there,
                # never abort the script with a raw provider exception.
                try { Remove-Item -LiteralPath $p -Force -Confirm:$false -ErrorAction SilentlyContinue } catch { }
                if (-not (Test-Path -LiteralPath $p)) { $deleted += $p }
            }
        } else {
            Write-Host ("  client {0}: already absent  {1}" -f $c.label, $p)
        }
        if (-not $isWhatIf -and (Test-Path -LiteralPath $p)) { $present += $p }
    }
    if (-not $isWhatIf) {
        if ($present.Count -gt 0) {
            Refuse "client $($c.label) still holds a coop_config.json that could not be removed: $($present -join ', '). Criterion 1 cannot be tested with an invisible config input on disk."
        }
        Write-Host ("  client {0}: ASSERTED ABSENT at {1} path(s)" -f $c.label, $c.configPaths.Count)
        $c.configDeleted = $deleted
        $c.configAbsent  = $true
    }
}

# ---------------------------------------- 3. scrub + ASSERT the KENSHICOOP_* env
# The name list is SCANNED OUT OF src/ so it cannot rot behind the plugin: every
# KENSHICOOP_* literal the plugin can read is checked, not a hand-kept subset.
Write-Host ""
Write-Host "=== 3. KENSHICOOP_* environment: scrub, then ASSERT NONE REMAINS ==="
$scanned = New-Object System.Collections.Generic.HashSet[string]
Get-ChildItem -Path (Join-Path $repoRoot "src") -Recurse -Include *.cpp, *.h -ErrorAction SilentlyContinue |
    Select-String -Pattern 'KENSHICOOP_[A-Z0-9_]+' -AllMatches -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Matches } | ForEach-Object { [void]$scanned.Add($_.Value) }
$fromParent = @(Get-ChildItem Env: | Where-Object { $_.Name -like "KENSHICOOP_*" } | ForEach-Object { $_.Name })
foreach ($n in $fromParent) { [void]$scanned.Add($n) }
$checked = @($scanned | Sort-Object)
Write-Host ("  names checked: {0} (scanned from src/, plus {1} inherited from this shell)" -f $checked.Count, $fromParent.Count)
if ($fromParent.Count -gt 0) {
    Write-Host ("  inherited and being removed: {0}" -f ($fromParent -join ", "))
}
if (-not $isWhatIf) {
    foreach ($n in $checked) { Remove-Item -LiteralPath ("Env:\" + $n) -ErrorAction SilentlyContinue }
    $remaining = @(Get-ChildItem Env: | Where-Object { $_.Name -like "KENSHICOOP_*" } | ForEach-Object { $_.Name })
    foreach ($n in $checked) { if (Test-Path ("Env:\" + $n)) { $remaining += $n } }
    $remaining = @($remaining | Sort-Object -Unique)
    if ($remaining.Count -gt 0) {
        Refuse "KENSHICOOP_* variables survived the scrub and would be inherited by the game: $($remaining -join ', ')"
    }
    Write-Host "  ASSERTED: no KENSHICOOP_* variable remains in the environment the clients inherit."
} else {
    Write-Host "  would remove every name above, then assert none remains."
}

# ------------------------------------------------------------- preflight record
$modeTag = if ($isWhatIf) { "whatif" }
           elseif ($NoLaunch) { "stage-only" }
           elseif ($LaunchSeconds -gt 0) { "boot-probe" }
           else { "attended" }
$headSha = ""
try { $headSha = (& git -C $repoRoot rev-parse HEAD 2>$null) } catch { $headSha = "" }

$preflight = [ordered]@{
    generatedUtc = (NowUtc)
    script       = "scripts/freshstart_session.ps1"
    mode         = $modeTag
    repoHeadSha  = "$headSha"
    releaseDll   = [ordered]@{ path = $releaseDll; sha256 = $releaseSha }
    clients      = @($clients | ForEach-Object {
        [ordered]@{
            label                = $_.label
            dir                  = $_.dir
            expectedLogName      = $_.logName
            dllSha256            = $_.dllSha256
            dllBytes             = $_.dllBytes
            dllLastWriteUtc      = $_.dllUtc
            configPathsChecked   = $_.configPaths
            configDeleted        = $_.configDeleted
            configAbsentAsserted = $_.configAbsent
        }
    })
    envScrub     = [ordered]@{
        namesCheckedCount     = $checked.Count
        namesChecked          = $checked
        inheritedAndRemoved   = $fromParent
        noneRemainingAsserted = (-not $isWhatIf)
    }
    notes        = @(
        "KENSHICOOP_LOG is intentionally unset: the plugin then writes its default log name into each client's own working directory (WINDOWS #15).",
        "With nothing set, Config.cpp defaults apply: role=host, transport=udp, autoConnect=0, save empty - so BOTH clients boot to the title screen and BOTH write KenshiCoop_host.log."
    )
}

if ($isWhatIf) {
    Write-Host ""
    Write-Host "=== WhatIf plan complete - nothing was changed, nothing was launched ==="
    Write-Host ("  would write the preflight record to: {0}" -f (Join-Path $OutDir "preflight.json"))
    foreach ($c in $clients) {
        Write-Host ("  would launch client {0}: {1} (workdir {2})" -f $c.label, $c.exe, $c.dir)
    }
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$preflight | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutDir "preflight.json") -Encoding UTF8
Write-Host ""
Write-Host ("  preflight record: {0}" -f (Join-Path $OutDir "preflight.json"))

if ($NoLaunch) {
    Write-Host ""
    Write-Host "=== -NoLaunch: the fresh-install CONDITION is staged and asserted; no session was driven ==="
    Write-Host "    Criteria 1-3 need a human at the F2 panel. Re-run without -NoLaunch to drive them."
    exit 0
}

# -------------------------------------------------------------- 4. launch both
if (-not $NoTile) {
    Write-Host ""
    Write-Host ("=== window layout: {0}x{1} x2, {2} monitor ===" -f $WindowW, $WindowH, $TileMonitor)
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
        -Width $WindowW -Height $WindowH -HostDir $ClientADir -JoinDir $ClientBDir
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

Write-Host ""
Write-Host "=== 4. launch both clients to the TITLE SCREEN (no env, no config) ==="
$pids = @{}
foreach ($c in $clients) {
    Write-Host ("Launching client {0} from {1} ..." -f $c.label, $c.dir)
    $p = Start-PastLauncher -Exe $c.exe -WorkDir $c.dir
    if ($p -eq 0) { Write-Warning "client $($c.label) failed to get past the launcher." }
    $pids[$c.label] = $p
    Start-Sleep -Seconds 4
}

if (-not $NoTile -and $pids["A"] -ne 0) {
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$arrangeScript`"",
        "-HostPid", "$($pids['A'])", "-JoinPid", "$($pids['B'])",
        "-Monitor", $TileMonitor, "-TimeoutSec", "90", "-RepeatSec", "$TileRepeatSec",
        "-ClientW", "$WindowW", "-ClientH", "$WindowH"
    ) | Out-Null
}

Write-Host ""
Write-Host "== both clients live: client A PID=$($pids['A']) (left), client B PID=$($pids['B']) (right) =="
Write-Host "   Client A dir: $ClientADir   log -> $ClientADir\KenshiCoop_host.log"
Write-Host "   Client B dir: $ClientBDir   log -> $ClientBDir\KenshiCoop_host.log"
Write-Host ""
Write-Host "   DO NOT set any environment variable and DO NOT create a coop_config.json."
Write-Host "   Everything below is done from the F2 panel alone."
Write-Host ""
Write-Host "   1) GLYPH CHECK first: press F2 on either client. Are all panel captions"
Write-Host "      readable text (no boxes, blanks or mojibake)? Record what you see."
Write-Host "   2) CRITERION 1: client A -> Role HOST, Transport UDP. Client B -> Role JOIN,"
Write-Host "      Transport UDP; copy 127.0.0.1:27800 to the clipboard, press"
Write-Host "      'Paste server address', confirm the 'On Connect' row shows it."
Write-Host "      Load a save on each, then set Connection ONLINE on both."
Write-Host "      Confirm a shared session: each client sees the other's squad."
Write-Host "   3) CRITERION 2: before connecting, confirm the panel names the armed role,"
Write-Host "      transport and peer address. Watch the Session row through all three"
Write-Host "      states (OFFLINE -> WAITING -> CONNECTED), at the title screen AND in game."
Write-Host "   4) CRITERION 3: on the JOIN set Connection OFFLINE, then ONLINE again,"
Write-Host "      without restarting Kenshi. Confirm the armed address survived (no"
Write-Host "      re-paste) and the session comes back."
Write-Host "      WINDOWS #19 lives here: a client relink leaks a host slot (the host holds"
Write-Host "      the old peer ~5.4 s while the reconnect takes the next free id). If the"
Write-Host "      reconnect is slow, lands on a different id, or fails, check the host log"
Write-Host "      for 'peer connected id=' and 'slots full' and RECORD IT - do not retune"
Write-Host "      the run until it passes."
Write-Host "   5) BAD PASTE: put junk on the clipboard, press 'Paste server address', and"
Write-Host "      confirm the row says so and the armed address is unchanged."
Write-Host ""

if ($LaunchSeconds -gt 0) {
    Write-Host ("   UNATTENDED boot probe: waiting {0}s, then closing both clients." -f $LaunchSeconds)
    Write-Host "   This proves the fresh-install BOOT path only. Criteria 1-3 need a human."
    Start-Sleep -Seconds $LaunchSeconds
    foreach ($k in @("A", "B")) {
        if ($pids[$k] -ne 0) { Stop-Process -Id $pids[$k] -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Seconds 3
} else {
    Read-Host "   Press Enter once the session is finished (close both Kenshi windows first)"
}

# ------------------------------------------------------- 5. collect + judge
Write-Host ""
Write-Host "=== 5. collect the logs and judge them ==="
$collected = @()
foreach ($c in $clients) {
    foreach ($n in @("KenshiCoop_host.log", "KenshiCoop_join.log", "kenshi_info.log")) {
        $src = Join-Path $c.dir $n
        if (Test-Path -LiteralPath $src) {
            $dst = Join-Path $OutDir ("client{0}_{1}" -f $c.label, $n)
            Copy-Item -LiteralPath $src -Destination $dst -Force
            Write-Host ("  {0} -> {1}" -f $src, $dst)
            if ($n -like "KenshiCoop_*") { $collected += $dst }
        }
    }
}

$panelExit = -1
$panelOut  = @()
if ($collected.Count -gt 0) {
    Write-Host ""
    # -Command with an explicit array literal, NOT -File: powershell.exe -File
    # flattens "-Log a b" into positional arguments and check_panel_log.ps1 then
    # refuses the second log with "a positional parameter cannot be found".
    $quoted = ($collected | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join ","
    $cmd = "& '" + (Join-Path $scriptDir "check_panel_log.ps1") + "' -Log @(" + $quoted + "); exit `$LASTEXITCODE"
    $panelOut = & powershell -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1
    $panelExit = $LASTEXITCODE
    $panelOut | ForEach-Object { Write-Host "  $_" }
} else {
    Write-Host "  no plugin logs were produced - nothing to judge."
}

$verdict = switch ($panelExit) {
    0       { "PASS" }
    1       { "FAIL" }
    2       { "SKIP (no panel connects found)" }
    default { "not run" }
}
$result = [ordered]@{
    preflight   = $preflight
    logs        = @($collected)
    panelCheck  = [ordered]@{
        exitCode = $panelExit
        verdict  = $verdict
        output   = @($panelOut | ForEach-Object { "$_" })
    }
    finishedUtc = (NowUtc)
}
$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $OutDir "result.json") -Encoding UTF8
Write-Host ""
Write-Host ("  result: {0}" -f (Join-Path $OutDir "result.json"))
# Propagate the panel oracle's own verdict rather than always exiting 0: a SKIP
# (exit 2, "no panel connects found") is what an UNDRIVEN run produces, and it
# must not be reported to a caller as a green run.
if ($collected.Count -gt 0) { exit $panelExit }
exit 0
