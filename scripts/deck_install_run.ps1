<#
.SYNOPSIS
  Drive the POSIX installer against the REAL second target - a Steam Deck
  running Kenshi under Proton - and record every measurement as a file rather
  than a transcript. Phase 16 Plan 02, Task 3 (INST-02, INST-04).

.DESCRIPTION
  This is criterion 2's actual evidence. scripts\tests\InstallerPosix.Tests.ps1
  proves the shell script's LOGIC headlessly; only a run on a machine that
  actually starts Kenshi through Proton proves the ENVIRONMENT. Neither stands
  in for the other, and this script never claims otherwise.

  THIS IS THE DEVELOPER'S OWN MACHINE. The sequence below captures the Deck's
  state before it writes anything, verifies that docs\CROSS_MACHINE_RIG.md
  section 8's rollback is still valid before it touches anything, and halts
  rather than continuing on any failure.

  Nine steps, in order:

    1. Assert non-interactive ssh. Tailscale SSH has been observed answering
       with an interactive re-auth demand AT EXIT CODE 0, so the reply text is
       checked as well as the exit code.
    2. Kill any running game and wait - a loaded DLL is the usual reason a
       deploy silently no-ops (CROSS_MACHINE_RIG section 2).
    3. Capture the pre-state BEFORE any write: whole-tree hash, the installed
       DLL's hash and mtime, the full text of Plugins_x64.cfg and
       data/mods.cfg, and the presence of kenshi_x64_vanilla.exe and
       Plugins_x64_vanilla.cfg. Section 8's rollback depends on those two; if
       either is missing this halts instead of writing. The capture is copied
       back to -OutDir immediately.
    4. Copy install_coop.sh and a Release payload over, CRs stripped, +x, with
       sha256 compared on BOTH SIDES for every file. "I copied the file" is not
       verification (section 2). The payload carries no RE_Kenshi and no Kenshi
       binary - the licensing boundary - only this repo's own Release DLL and
       two repo-owned text files.
    5. Run install_coop.sh through a small per-run wrapper placed on the Deck,
       not on the ssh command line (section 3).
    6. Launch once through the Deck's own ~/rekit/launch_auto.sh from an
       ATTACHED ssh session - a detached launch dies with the session (section
       3) - read the build stamp and proto=v back out of the Deck's own coop
       log, assert both, then kill the game explicitly.
    7. Uninstall, hash the tree again, compare with step 3.
    8. Re-verify the two vanilla copies and restore whatever step 7 did not.
    9. Write tools/test-runs/phase16_deck_install.json, APPENDING to any
       existing history array rather than overwriting it (section 2 step 6).

  Steps 5-7 exceed a 600 s foreground tool ceiling; run this script itself in
  the background and process the JSON it writes.

.PARAMETER DeckHost
  user@host for ssh/scp. The tailnet address is a DEFAULT only, and
  tools\test-runs\ is gitignored, so no real address is committed.

.PARAMETER OutDir
  ABSOLUTE path for the run records. A relative path is refused.

.PARAMETER SkipLaunch
  Do steps 1-5 and 7-9 but not the launch. The record then says the launch was
  skipped, and criterion 2 is NOT reported as proved.

.PARAMETER WhatIf
  Print the nine-step plan and touch nothing, locally or remotely.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deck_install_run.ps1 -OutDir F:\...\tools\test-runs\phase16_deck
#>
[CmdletBinding()]
param(
    [string]$DeckHost = "deck@100.69.90.73",
    [string]$OutDir = "",
    [switch]$SkipLaunch,
    [switch]$WhatIf,
    [int]$LaunchTimeoutSec = 240
)

$ErrorActionPreference = "Stop"
$INV        = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot   = Split-Path -Parent $scriptDir
$UTF8NB     = New-Object System.Text.UTF8Encoding($false)

$RunId      = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss", $INV)
$RemoteRoot = '$HOME/rekit/phase16'
$KenshiRoot = '$HOME/.local/share/Steam/steamapps/common/Kenshi'

# Files the GAME itself rewrites on every launch. They are measured and
# reported, never silently dropped: the criterion-4 verdict is taken on the
# tree WITHOUT them, and the full diff is recorded so a reader can see exactly
# what moved and decide for themselves.
$VolatilePatterns = @(
    '*.log', 'RE_Kenshi_log.txt', 'RE_Kenshi.ini', 'RE_Kenshi.ini.bak',
    'FileIOLog.txt', '__tutorials.data', 'save/*', '_screens/*', 'RE_Kenshi/*'
)

function Say([string]$m) { Write-Host $m }
function Die([string]$m) {
    Write-Host ""
    Write-Host "HALTED: $m"
    if ($script:Record) { Save-Record "halted" $m }
    exit 4
}

function WriteTextNoBom([string]$path, [string]$text) {
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    # -Encoding utf8 in Windows PowerShell 5.1 writes a BOM, and a BOM breaks a
    # first-line match for every later check (CROSS_MACHINE_RIG section 5e).
    [System.IO.File]::WriteAllText($path, $text, $UTF8NB)
}

function Sha256File([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($path))
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Invoke-Ssh {
    param([string]$Command, [int]$TimeoutSec = 120, [switch]$AllowFail)
    $sshArgv = @("-o", "BatchMode=yes", "-o", ("ConnectTimeout=" + $TimeoutSec.ToString($INV)),
              "-o", "ServerAliveInterval=20", $DeckHost, $Command)
    # ssh writes ordinary notices to stderr; with $ErrorActionPreference=Stop a
    # native command's stderr can become a TERMINATING error, which would skip
    # both the halt message and the run record.
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { $out = (& ssh @sshArgv 2>&1 | Out-String) } finally { $ErrorActionPreference = $prev }
    $code = $LASTEXITCODE
    # Tailscale SSH has answered with an interactive re-auth demand AT EXIT
    # CODE 0 while doing nothing. The reply text is part of the verdict.
    if ($out -match 'login\.tailscale\.com' -or $out -match 'failed to fetch next SSH action') {
        Die ("ssh to $DeckHost is not non-interactive: Tailscale SSH demanded an interactive check. Verbatim:`n" + $out.Trim())
    }
    if ((-not $AllowFail) -and $code -ne 0) {
        Die ("ssh command failed (exit $code): $Command`n" + $out.Trim())
    }
    return @{ text = $out; exit = $code }
}

# scp does NOT expand $HOME in a remote path - ssh does. Every remote path
# handed to scp must therefore be literal, which is why step 1 resolves the
# Deck's own $HOME once and every later path is built from it.
#
# And the LOCAL path goes to an MSYS scp.exe, whose runtime rewrites arguments
# that look like paths and eats backslashes on the way - "F:\a\b\c.sh" arrived
# as "F:PrivatProjects...\\c.sh". Local paths are converted to MSYS form
# ("/f/a/b/c.sh"), which has no backslashes to lose and no colon to be mistaken
# for a host:path separator.
function ToScpLocal([string]$p) {
    $full = [System.IO.Path]::GetFullPath($p)
    if ($full -match '^([A-Za-z]):[\\/](.*)$') {
        return "/" + $Matches[1].ToLowerInvariant() + "/" + ($Matches[2] -replace '\\', '/')
    }
    return ($full -replace '\\', '/')
}

function Invoke-Scp {
    param([string]$Local, [string]$RemotePath)
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { $out = (& scp -o BatchMode=yes -o ConnectTimeout=30 (ToScpLocal $Local) ($DeckHost + ":" + $RemotePath) 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { Die ("scp of '$Local' to '$RemotePath' failed: " + $out.Trim()) }
}

function Invoke-ScpBack {
    param([string]$RemotePath, [string]$Local)
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { $out = (& scp -o BatchMode=yes -o ConnectTimeout=30 ($DeckHost + ":" + $RemotePath) (ToScpLocal $Local) 2>&1 | Out-String) }
    finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { Die ("scp back of '$RemotePath' failed: " + $out.Trim()) }
}

function Push-Script {
    # Author locally, strip CRs, chmod +x, then run THAT one path. A carriage
    # return on the shebang line makes the Deck report "bad interpreter"
    # (CROSS_MACHINE_RIG section 3).
    param([string]$Name, [string]$Body)
    $local = Join-Path $OutDir $Name
    WriteTextNoBom $local ($Body -replace "`r`n", "`n")
    Invoke-Scp $local ($RemoteRoot + "/" + $Name)
    Invoke-Ssh ("sed -i 's/\r$//' " + $RemoteRoot + "/" + $Name + " && chmod +x " + $RemoteRoot + "/" + $Name) | Out-Null
    return ($RemoteRoot + "/" + $Name)
}

$script:Record = $null
function Save-Record([string]$outcome, [string]$note) {
    if (-not $script:Record) { return }
    $script:Record.outcome = $outcome
    $script:Record.note    = $note
    $script:Record.finishedUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", $INV)

    $histPath = Join-Path $repoRoot "tools\test-runs\phase16_deck_install.json"
    $history = @()
    if (Test-Path -LiteralPath $histPath) {
        try {
            $existing = (Get-Content -Raw $histPath) | ConvertFrom-Json
            if ($existing.runs) { $history = @($existing.runs) }
        } catch { $history = @() }
    }
    $history += $script:Record
    $doc = [ordered]@{
        schemaVersion = 1
        description   = "Phase 16 plan 02 Task 3: install_coop.sh driven against the real Proton target. Appended, never overwritten: a record that holds only the newest measurement cannot answer what the Deck was running when an earlier run was judged."
        runs          = @($history)
    }
    WriteTextNoBom $histPath ($doc | ConvertTo-Json -Depth 20)
    WriteTextNoBom (Join-Path $OutDir ("run-" + $RunId + ".json")) ($script:Record | ConvertTo-Json -Depth 20)
    Say ("  record: " + $histPath)
}

# ============================================================== the plan =====
$PlanText = @"
deck_install_run.ps1 - nine steps against $DeckHost

  1. assert non-interactive ssh (text checked as well as exit code), record
     the SteamOS and Proton versions
  2. kill any running kenshi_x64.exe and wait
  3. CAPTURE THE PRE-STATE BEFORE ANY WRITE: whole-tree hash of $KenshiRoot,
     the installed DLL's sha256 and mtime, Plugins_x64.cfg and data/mods.cfg
     verbatim, and kenshi_x64_vanilla.exe / Plugins_x64_vanilla.cfg presence.
     HALT if either vanilla copy is missing - section 8's rollback would no
     longer be valid. Copy the capture back to -OutDir immediately.
  4. copy scripts/install_coop.sh and a Release payload over (CRs stripped,
     +x), sha256 compared on BOTH sides, halting on any inequality. Payload =
     src\plugin\x64\Release\KenshiCoop.dll + the repo's RE_Kenshi.json and
     KenshiCoop.mod. No RE_Kenshi or Kenshi binary is carried.
  5. run install_coop.sh through a per-run wrapper ON the Deck
  6. launch once via ~/rekit/launch_auto.sh from an ATTACHED ssh session, read
     'KenshiCoop: build <stamp>' and 'proto=v' back from the Deck's own log,
     assert proto == PROTOCOL_VERSION and the stamp matches the installed DLL,
     then kill the game
  7. install_coop.sh --uninstall, hash the tree again, compare with step 3
  8. re-verify the two vanilla copies; restore anything step 7 did not
  9. write tools/test-runs/phase16_deck_install.json, APPENDING to history

  volatile files, measured and reported but excluded from the verdict:
    $($VolatilePatterns -join ', ')
"@

# =============================================================== dispatch ====
if ($OutDir -and -not [System.IO.Path]::IsPathRooted($OutDir)) {
    Write-Host "REFUSED: -OutDir must be an ABSOLUTE path (got '$OutDir'). A relative path is swallowed silently and nothing is written there."
    exit 3
}
# IsPathRooted is not enough. "F:foo" is DRIVE-RELATIVE: .NET calls it rooted
# and resolves it against F:'s current directory, so it lands somewhere that
# looks plausible and is not where anyone meant. This run met that case for
# real - a caller stripped the backslashes out of the argument and the whole
# record tree was written into the repository root instead of tools\test-runs\.
if ($OutDir -match '^[A-Za-z]:[^\\/]') {
    Write-Host "REFUSED: -OutDir '$OutDir' is DRIVE-RELATIVE, not absolute. '$($OutDir.Substring(0,2))' without a separator after it resolves against that drive's current directory, so the records would be written somewhere other than where you asked. Pass a full path with separators, e.g. F:\path\to\out."
    exit 3
}
if (-not $OutDir) {
    Write-Host "REFUSED: -OutDir is required and must be an ABSOLUTE path."
    exit 3
}

Say "=== deck_install_run: Phase 16 plan 02, the live Proton target ==="
Say $PlanText

if ($WhatIf) {
    Say ""
    Say "WHATIF: nothing above was done. No ssh command was sent, no file was copied, and the Deck was not touched."
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$payloadDll = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"
$payloadJson = Join-Path $repoRoot "dist\mod-kit\KenshiCoop\RE_Kenshi.json"
$payloadMod  = Join-Path $repoRoot "dist\mod-kit\KenshiCoop\KenshiCoop.mod"
foreach ($p in @($payloadDll, $payloadJson, $payloadMod)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "REFUSED: the payload file '$p' is not there."; exit 3 }
}
# The licensing boundary, asserted rather than assumed.
if ($payloadDll -match 'vendor' -or $payloadJson -match 'vendor' -or $payloadMod -match 'vendor') {
    Write-Host "REFUSED: a payload file came from scripts\vendor\, which is excluded as a packaging source."
    exit 3
}

$protoExpected = ""
$wire = Join-Path $repoRoot "src\netproto\Wire.h"
if (Test-Path $wire) {
    $m = [regex]::Match((Get-Content -Raw $wire), 'PROTOCOL_VERSION\s*=\s*(\d+)')
    if ($m.Success) { $protoExpected = $m.Groups[1].Value }
}

$script:Record = [ordered]@{
    runId          = $RunId
    startedUtc     = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", $INV)
    finishedUtc    = ""
    outcome        = "in-progress"
    note           = ""
    deckHost       = $DeckHost
    repoHeadSha    = ""
    protocolExpected = $protoExpected
    skipLaunch     = [bool]$SkipLaunch
    volatilePatterns = @($VolatilePatterns)
    steps          = [ordered]@{}
}
try { $script:Record.repoHeadSha = (& git -C $repoRoot rev-parse HEAD).Trim() } catch { }

# ------------------------------------------------------------------- step 1
Say ""
Say "-- step 1: the Deck is reachable non-interactively --"
$probe = Invoke-Ssh "echo ok" 20
if ($probe.text.Trim() -ne "ok") {
    Die ("the ssh probe did not print a clean 'ok'. Verbatim:`n" + $probe.text.Trim())
}
Say "  ssh: ok (no auth line, no 'failed to fetch next SSH action')"
# `$HOME` must reach the DECK's shell, so it is backtick-escaped: "\$HOME" in a
# PowerShell double-quoted string is a literal backslash followed by THIS
# machine's $HOME, which is how this probe first asked the Deck about
# C:\Users\... . The trailing `; true` keeps a no-match `ls` from halting a
# step that is only gathering version strings.
$vers = Invoke-Ssh "uname -sr; cat /etc/os-release 2>/dev/null | grep -E '^(NAME|VERSION)=' ; ls -1d $KenshiRoot/../../compatdata/233860/pfx 2>/dev/null; ls -1d `$HOME/.local/share/Steam/steamapps/common/Proton* `$HOME/.steam/steam/steamapps/common/Proton* 2>/dev/null; true" 30
$script:Record.steps.step1 = [ordered]@{ ssh = "ok"; versions = $vers.text.Trim() }
Say ("  " + (($vers.text.Trim() -split "`n") -join "`n  "))

# Resolve the Deck's own $HOME ONCE and rebuild every remote path from it: scp
# hands its remote path to the server verbatim, so a "$HOME/..." remote path
# fails with "No such file or directory" while the identical ssh command works.
$deckHome = (Invoke-Ssh 'printf %s "$HOME"' 30).text.Trim()
if (-not $deckHome.StartsWith("/")) { Die "could not resolve the Deck's `$HOME (got '$deckHome')." }
$RemoteRoot = $deckHome + "/rekit/phase16"
$KenshiRoot = $deckHome + "/.local/share/Steam/steamapps/common/Kenshi"
$script:Record.steps.step1.deckHome   = $deckHome
$script:Record.steps.step1.kenshiRoot = $KenshiRoot
Say ("  deck home: $deckHome")

# ------------------------------------------------------------------- step 2
Say ""
Say "-- step 2: no game may be holding a DLL open --"
$kill = Invoke-Ssh "pkill -f 'kenshi_x64[.]exe' >/dev/null 2>&1; sleep 4; pgrep -af 'kenshi_x64[.]exe' || echo '(none running)'" 60 -AllowFail
Say ("  " + $kill.text.Trim())
$script:Record.steps.step2 = [ordered]@{ afterKill = $kill.text.Trim() }
if ($kill.text -notmatch '\(none running\)') { Die "a kenshi_x64.exe is still running on the Deck after pkill; a loaded DLL cannot be replaced." }

# ------------------------------------------------------------------- step 3
Say ""
Say "-- step 3: the pre-state, captured BEFORE anything is written --"
Invoke-Ssh ("mkdir -p " + $RemoteRoot) 30 | Out-Null
Invoke-Scp (Join-Path $repoRoot "scripts\install_coop.sh") ($RemoteRoot + "/install_coop.sh")
Invoke-Ssh ("sed -i 's/\r$//' " + $RemoteRoot + "/install_coop.sh && chmod +x " + $RemoteRoot + "/install_coop.sh") 30 | Out-Null

$localShSha = Sha256File (Join-Path $repoRoot "scripts\install_coop.sh")
$remoteShSha = (Invoke-Ssh ("sha256sum " + $RemoteRoot + "/install_coop.sh | cut -d' ' -f1") 30).text.Trim()
# The CR strip is deliberate, so the two hashes are compared AFTER it: what
# must match is the content the Deck will execute, which is the LF form the
# repo already stores (.gitattributes pins *.sh to eol=lf).
Say ("  install_coop.sh  local  $localShSha")
Say ("  install_coop.sh  remote $remoteShSha")
if ($localShSha -ne $remoteShSha) { Die "install_coop.sh differs between the two machines after the copy (local $localShSha, remote $remoteShSha)." }

$excl = ($VolatilePatterns | ForEach-Object { "--exclude `"$_`"" }) -join " "
$preCmd = @"
set -u
K=$KenshiRoot
R=$RemoteRoot
mkdir -p "`$R"
bash "`$R/install_coop.sh" --hash-tree "`$K" > "`$R/pre.full.txt"
bash "`$R/install_coop.sh" --hash-tree "`$K" $excl > "`$R/pre.filtered.txt"
echo "--- dll ---"
if [ -f "`$K/mods/KenshiCoop/KenshiCoop.dll" ]; then
  sha256sum "`$K/mods/KenshiCoop/KenshiCoop.dll"
  date -u -r "`$K/mods/KenshiCoop/KenshiCoop.dll" +"%Y-%m-%dT%H:%M:%SZ"
else
  echo "(absent)"
fi
echo "--- vanilla ---"
[ -f "`$K/kenshi_x64_vanilla.exe" ]  && echo "kenshi_x64_vanilla.exe PRESENT `$(sha256sum "`$K/kenshi_x64_vanilla.exe" | cut -d' ' -f1)"  || echo "kenshi_x64_vanilla.exe MISSING"
[ -f "`$K/Plugins_x64_vanilla.cfg" ] && echo "Plugins_x64_vanilla.cfg PRESENT `$(sha256sum "`$K/Plugins_x64_vanilla.cfg" | cut -d' ' -f1)" || echo "Plugins_x64_vanilla.cfg MISSING"
echo "--- plugins cfg ---"
cat "`$K/Plugins_x64.cfg"
echo "--- mods cfg ---"
cat "`$K/data/mods.cfg"
echo "--- manifest ---"
[ -f "`$K/mods/KenshiCoop.backup/INSTALL-MANIFEST.json" ] && echo "(a manifest is already there)" || echo "(no manifest)"
echo "--- entries ---"
wc -l < "`$R/pre.full.txt"
wc -l < "`$R/pre.filtered.txt"
"@
$preScript = Push-Script ("pre_" + $RunId + ".sh") $preCmd
$pre = Invoke-Ssh ("bash " + $preScript) 600
Say ($pre.text.Trim())

if ($pre.text -notmatch 'kenshi_x64_vanilla\.exe PRESENT') {
    Die "kenshi_x64_vanilla.exe is NOT on the Deck. docs\CROSS_MACHINE_RIG.md section 8's rollback is no longer valid, so nothing will be written."
}
if ($pre.text -notmatch 'Plugins_x64_vanilla\.cfg PRESENT') {
    Die "Plugins_x64_vanilla.cfg is NOT on the Deck. Section 8's rollback is no longer valid, so nothing will be written."
}
Say "  section 8's rollback is intact: both vanilla copies are present."

# Bring the capture home BEFORE any write.
Invoke-ScpBack ($RemoteRoot + "/pre.full.txt")     (Join-Path $OutDir ("pre.full." + $RunId + ".txt"))
Invoke-ScpBack ($RemoteRoot + "/pre.filtered.txt") (Join-Path $OutDir ("pre.filtered." + $RunId + ".txt"))
WriteTextNoBom (Join-Path $OutDir ("prestate." + $RunId + ".txt")) $pre.text

$preFullSha     = Sha256File (Join-Path $OutDir ("pre.full." + $RunId + ".txt"))
$preFilteredSha = Sha256File (Join-Path $OutDir ("pre.filtered." + $RunId + ".txt"))
$script:Record.steps.step3 = [ordered]@{
    rollbackValid      = $true
    preStateText       = $pre.text.Trim()
    preFullSha256      = $preFullSha
    preFilteredSha256  = $preFilteredSha
    installerShaLocal  = $localShSha
    installerShaRemote = $remoteShSha
}
Say ("  pre-state captured: full $preFullSha / filtered $preFilteredSha")

# ------------------------------------------------------------------- step 4
Say ""
Say "-- step 4: the payload, verified on both sides --"
$payload = @(
    @{ name = "KenshiCoop.dll"; local = $payloadDll },
    @{ name = "RE_Kenshi.json"; local = $payloadJson },
    @{ name = "KenshiCoop.mod"; local = $payloadMod }
)
Invoke-Ssh ("rm -rf " + $RemoteRoot + "/payload && mkdir -p " + $RemoteRoot + "/payload") 30 | Out-Null
$payloadRec = @()
foreach ($f in $payload) {
    Invoke-Scp $f.local ($RemoteRoot + "/payload/" + $f.name)
    $lsha = Sha256File $f.local
    $rsha = (Invoke-Ssh ("sha256sum " + $RemoteRoot + "/payload/" + $f.name + " | cut -d' ' -f1") 30).text.Trim()
    Say ("  {0,-16} local {1}" -f $f.name, $lsha)
    Say ("  {0,-16} remote {1}" -f "", $rsha)
    if ($lsha -ne $rsha) { Die ("the copy of " + $f.name + " does not match on the two machines (local $lsha, remote $rsha).") }
    $payloadRec += [ordered]@{ name = $f.name; source = $f.local; sha256Local = $lsha; sha256Remote = $rsha }
}
$script:Record.steps.step4 = [ordered]@{ payload = @($payloadRec); vendorUsed = $false }

# ------------------------------------------------------------------- step 5
Say ""
Say "-- step 5: install_coop.sh, run through a per-run wrapper on the Deck --"
$insCmd = @"
set -u
K=$KenshiRoot
R=$RemoteRoot
bash "`$R/install_coop.sh" --kenshi-dir "`$K" --source "`$R/payload" --out-dir "`$R/record" --force
rc=`$?
echo "INSTALL-EXIT=`$rc"
exit `$rc
"@
$insScript = Push-Script ("install_" + $RunId + ".sh") $insCmd
$ins = Invoke-Ssh ("bash " + $insScript) 900 -AllowFail
Say ($ins.text.Trim())
WriteTextNoBom (Join-Path $OutDir ("install." + $RunId + ".txt")) $ins.text
if ($ins.exit -ne 0 -or $ins.text -notmatch 'INSTALL: COMPLETE') {
    $script:Record.steps.step5 = [ordered]@{ exit = $ins.exit; text = $ins.text.Trim() }
    Die "install_coop.sh did not report INSTALL: COMPLETE on the Deck."
}
$manifest = (Invoke-Ssh ("cat " + $KenshiRoot + "/mods/KenshiCoop.backup/INSTALL-MANIFEST.json") 60).text
WriteTextNoBom (Join-Path $OutDir ("manifest." + $RunId + ".json")) $manifest
$script:Record.steps.step5 = [ordered]@{ exit = 0; text = $ins.text.Trim(); manifest = $manifest.Trim() }

$installedDllSha = (Invoke-Ssh ("sha256sum " + $KenshiRoot + "/mods/KenshiCoop/KenshiCoop.dll | cut -d' ' -f1") 60).text.Trim()
$localDllSha = Sha256File $payloadDll
Say ("  installed DLL sha256 on the Deck: $installedDllSha")
if ($installedDllSha -ne $localDllSha) { Die "the DLL now installed on the Deck is not the one this run copied ($installedDllSha vs $localDllSha)." }

# ------------------------------------------------------------------- step 6
Say ""
if ($SkipLaunch) {
    Say "-- step 6: SKIPPED (-SkipLaunch). Criterion 2 is NOT proved by this run. --"
    $script:Record.steps.step6 = [ordered]@{ launched = $false; reason = "-SkipLaunch was given; the record must not be read as proving that Kenshi starts under Proton." }
} else {
    Say "-- step 6: launch once through the Deck's own launch_auto.sh, ATTACHED --"
    $logRemote = $RemoteRoot + "/coop_" + $RunId + ".log"
    $wrapper = @"
#!/usr/bin/env bash
# Per-run wrapper, ON the Deck. A single ssh 'A=1 B=2 ... launch_auto.sh' has to
# survive two levels of shell quoting and is where a silently-dropped variable
# hides (CROSS_MACHINE_RIG section 3). This file is also the record of exactly
# what the environment was.
export KENSHICOOP_MODE=host
export KENSHICOOP_TRANSPORT=udp
export KENSHICOOP_STEAM_PEER=0
export KENSHICOOP_CELL_AUTH=0
export KENSHICOOP_LOG="$logRemote"
# No KENSHICOOP_SCENARIO and no KENSHICOOP_SAVE: this run proves the plugin
# LOADS under Proton, which happens at the title screen. It is not a gate run.
# No KENSHICOOP_NETSIM_* key - one of Phase 12's three forbidden escapes.
rm -f "`$KENSHICOOP_LOG"
exec "`$HOME/rekit/launch_auto.sh"
"@
    $wrapScript = Push-Script ("launch_" + $RunId + ".sh") $wrapper

    # ATTACHED: the ssh session stays open for the whole launch. The LOCAL
    # command is backgrounded instead, which is what section 3 prescribes.
    $sshArgs = @("-o", "BatchMode=yes", "-o", "ServerAliveInterval=20", $DeckHost, ("bash " + $wrapScript))
    $launchOut = Join-Path $OutDir ("launch." + $RunId + ".txt")
    $proc = Start-Process -FilePath "ssh" -ArgumentList $sshArgs -NoNewWindow -PassThru `
                          -RedirectStandardOutput $launchOut -RedirectStandardError ($launchOut + ".err")

    $deadline = (Get-Date).AddSeconds($LaunchTimeoutSec)
    $banner = ""
    $roleLine = ""
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 10
        $peek = Invoke-Ssh ("grep -m1 'KenshiCoop: build' " + $logRemote + " 2>/dev/null; grep -m1 'proto=v' " + $logRemote + " 2>/dev/null; true") 60 -AllowFail
        $t = $peek.text
        if ($t -match '(KenshiCoop: build [^\r\n]+)') { $banner = $Matches[1].Trim() }
        if ($t -match '([^\r\n]*proto=v[^\r\n]*)')    { $roleLine = $Matches[1].Trim() }
        if ($banner -and $roleLine) { break }
        Say ("  waiting for the load banner ... " + ([int]($deadline - (Get-Date)).TotalSeconds).ToString($INV) + "s left")
    }

    # Kill the game explicitly: a leftover instance holds the port and the DLL.
    Invoke-Ssh "pkill -f 'kenshi_x64[.]exe' >/dev/null 2>&1; sleep 5; pgrep -af 'kenshi_x64[.]exe' || echo '(none running)'" 90 -AllowFail | Out-Null
    try { if ($proc -and -not $proc.HasExited) { $proc.Kill() } } catch { }

    Invoke-ScpBack $logRemote (Join-Path $OutDir ("coop." + $RunId + ".log"))
    $protoSeen = ""
    if ($roleLine -match 'proto=v(\d+)') { $protoSeen = $Matches[1] }

    $stampOk = $false
    if ($banner -match 'KenshiCoop: build\s+(.+)$') {
        # The build stamp the DLL itself carries, read straight out of the file
        # that was just installed rather than out of the repo.
        $stamp = $Matches[1].Trim()
        $dllBytes = [System.IO.File]::ReadAllBytes($payloadDll)
        $ascii = [System.Text.Encoding]::ASCII.GetString($dllBytes)
        $parts = $stamp -split '\s+'
        $stampOk = $true
        foreach ($p in $parts) { if ($p -and ($ascii.IndexOf($p) -lt 0)) { $stampOk = $false } }
    }

    $script:Record.steps.step6 = [ordered]@{
        launched      = $true
        wrapper       = ($wrapper -replace "`r`n", "`n")
        bannerLine    = $banner
        roleLine      = $roleLine
        protocolSeen  = $protoSeen
        protocolExpected = $protoExpected
        protocolMatches  = ($protoSeen -ne "" -and $protoSeen -eq $protoExpected)
        buildStampFoundInInstalledDll = $stampOk
    }
    Say ("  banner : " + $(if ($banner) { $banner } else { "(NOT SEEN)" }))
    Say ("  role   : " + $(if ($roleLine) { $roleLine } else { "(NOT SEEN)" }))
    Say ("  proto  : seen '$protoSeen', expected '$protoExpected'")

    if (-not $banner) {
        Die "the Deck's coop log never showed 'KenshiCoop: build'. The game did not reach the point where the plugin loads, so criterion 2 is NOT proved by this run."
    }
    if ($protoSeen -ne $protoExpected) {
        Die "the Deck reported proto=v$protoSeen and src/netproto/Wire.h says $protoExpected."
    }
    if (-not $stampOk) {
        Die "the build stamp in the banner ('$banner') is not present in the DLL this run installed."
    }
    Say "  criterion 2's evidence: the plugin loaded through the Ogre plugin list and the game started."
}

# ------------------------------------------------------------------- step 7
Say ""
Say "-- step 7: uninstall, then measure the tree against step 3 --"
$unCmd = @"
set -u
K=$KenshiRoot
R=$RemoteRoot
bash "`$R/install_coop.sh" --kenshi-dir "`$K" --uninstall --out-dir "`$R/record"
rc=`$?
echo "UNINSTALL-EXIT=`$rc"
bash "`$R/install_coop.sh" --hash-tree "`$K" > "`$R/post.full.txt"
bash "`$R/install_coop.sh" --hash-tree "`$K" $excl > "`$R/post.filtered.txt"
echo "--- filtered diff (empty means byte-identical) ---"
diff "`$R/pre.filtered.txt" "`$R/post.filtered.txt" || true
echo "--- full diff (volatile files included) ---"
diff "`$R/pre.full.txt" "`$R/post.full.txt" || true
echo "--- end ---"
exit `$rc
"@
$unScript = Push-Script ("uninstall_" + $RunId + ".sh") $unCmd
$un = Invoke-Ssh ("bash " + $unScript) 900 -AllowFail
Say ($un.text.Trim())
WriteTextNoBom (Join-Path $OutDir ("uninstall." + $RunId + ".txt")) $un.text

Invoke-ScpBack ($RemoteRoot + "/post.full.txt")     (Join-Path $OutDir ("post.full." + $RunId + ".txt"))
Invoke-ScpBack ($RemoteRoot + "/post.filtered.txt") (Join-Path $OutDir ("post.filtered." + $RunId + ".txt"))

$postFullSha     = Sha256File (Join-Path $OutDir ("post.full." + $RunId + ".txt"))
$postFilteredSha = Sha256File (Join-Path $OutDir ("post.filtered." + $RunId + ".txt"))

$filteredIdentical = ($preFilteredSha -eq $postFilteredSha)
$fullIdentical     = ($preFullSha -eq $postFullSha)

$filteredDiff = ""
if ($un.text -match '(?s)--- filtered diff \(empty means byte-identical\) ---(.*?)--- full diff') { $filteredDiff = $Matches[1].Trim() }
$fullDiff = ""
if ($un.text -match '(?s)--- full diff \(volatile files included\) ---(.*?)--- end ---') { $fullDiff = $Matches[1].Trim() }

$script:Record.steps.step7 = [ordered]@{
    uninstallExit      = $un.exit
    preFilteredSha256  = $preFilteredSha
    postFilteredSha256 = $postFilteredSha
    filteredIdentical  = $filteredIdentical
    preFullSha256      = $preFullSha
    postFullSha256     = $postFullSha
    fullIdentical      = $fullIdentical
    filteredDiff       = $filteredDiff
    fullDiff           = $fullDiff
}
Say ("  filtered capture identical: $filteredIdentical  ($preFilteredSha -> $postFilteredSha)")
Say ("  full capture identical:     $fullIdentical")
if ($un.exit -ne 0) { Die "install_coop.sh --uninstall exited $($un.exit) on the Deck." }
if (-not $filteredIdentical) {
    Die ("the post-uninstall tree is NOT byte-identical to the pre-state. Diff recorded verbatim:`n" + $filteredDiff)
}

# ------------------------------------------------------------------- step 8
Say ""
Say "-- step 8: the Deck is left as it was found --"
$restCmd = @"
set -u
K=$KenshiRoot
echo "--- vanilla ---"
[ -f "`$K/kenshi_x64_vanilla.exe" ]  && echo "kenshi_x64_vanilla.exe PRESENT `$(sha256sum "`$K/kenshi_x64_vanilla.exe" | cut -d' ' -f1)"  || echo "kenshi_x64_vanilla.exe MISSING"
[ -f "`$K/Plugins_x64_vanilla.cfg" ] && echo "Plugins_x64_vanilla.cfg PRESENT `$(sha256sum "`$K/Plugins_x64_vanilla.cfg" | cut -d' ' -f1)" || echo "Plugins_x64_vanilla.cfg MISSING"
echo "--- installed dll now ---"
[ -f "`$K/mods/KenshiCoop/KenshiCoop.dll" ] && sha256sum "`$K/mods/KenshiCoop/KenshiCoop.dll" || echo "(absent)"
echo "--- plugins cfg ---"
cat "`$K/Plugins_x64.cfg"
echo "--- mods cfg ---"
cat "`$K/data/mods.cfg"
echo "--- leftovers from this run ---"
ls -la "`$K/mods/KenshiCoop.backup" 2>/dev/null || echo "(no backup directory - the uninstall removed the backups it owned)"
echo "--- running ---"
pgrep -af 'kenshi_x64[.]exe' || echo "(none running)"
"@
$restScript = Push-Script ("restore_" + $RunId + ".sh") $restCmd
$rest = Invoke-Ssh ("bash " + $restScript) 300
Say ($rest.text.Trim())
WriteTextNoBom (Join-Path $OutDir ("poststate." + $RunId + ".txt")) $rest.text

$rollbackStillValid = ($rest.text -match 'kenshi_x64_vanilla\.exe PRESENT' -and $rest.text -match 'Plugins_x64_vanilla\.cfg PRESENT')
$script:Record.steps.step8 = [ordered]@{
    postStateText      = $rest.text.Trim()
    rollbackStillValid = $rollbackStillValid
}
if (-not $rollbackStillValid) { Die "one of the two vanilla copies section 8's rollback depends on is gone after this run." }
Say "  section 8's rollback is still valid."

# Remove this run's own scratch, leaving the Deck's rekit as it was.
Invoke-Ssh ("rm -rf " + $RemoteRoot) 60 -AllowFail | Out-Null

# ------------------------------------------------------------------- step 9
Say ""
Say "-- step 9: the record --"
$launchOk = $true
if (-not $SkipLaunch) { $launchOk = [bool]$script:Record.steps.step6.protocolMatches }
$verdict = if ($SkipLaunch) { "install-and-uninstall-proved-launch-skipped" } else { "passed" }
Save-Record $verdict "Criterion 4 was measured on the real second target by whole-tree hash. The launch is criterion 2's evidence; where it was skipped, criterion 2 is UNPROVEN and must be reported as such rather than inferred."

Say ""
Say "DECK RUN: $verdict"
Say ("  pre-state  filtered sha256 : " + $preFilteredSha)
Say ("  post-uninstall     sha256 : " + $postFilteredSha)
Say ("  byte-identical            : " + $filteredIdentical)
if (-not $SkipLaunch) {
    Say ("  banner                    : " + $script:Record.steps.step6.bannerLine)
    Say ("  protocol                  : v" + $script:Record.steps.step6.protocolSeen)
}
Say ("  section 8 rollback valid  : " + $rollbackStillValid)
Say ""
Say "This run proves the Proton install path. It does not authorise a release:"
Say "WINDOWS #19 and #22 are open and UI-01..UI-04 are unobserved."
exit 0
