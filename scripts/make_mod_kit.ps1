<#
.SYNOPSIS
  Package the PLAYER release: a single folder called "KenshiCoop" with the mod
  files inside, that a player copies straight into <Kenshi>\mods\. No install
  scripts, no launchers, no bundled save.

.DESCRIPTION
  Assembles dist\mod-kit\ as:
    KenshiCoop\                 <- the drop-in mod folder (copy this into mods\)
      KenshiCoop.dll              the plugin (protocol-version-matched; a mismatch
                                  is rejected at handshake by design)
      KenshiCoop.mod              mod-list entry so it shows in Kenshi's Mods menu
      RE_Kenshi.json              tells RE_Kenshi to load the plugin
      coop_config.json            only needed for LAN/direct-UDP; Steam play is
                                  configured entirely in-game (F2)
    README.txt                  <- plain copy-the-folder instructions (NOT copied
                                  into mods, so it never clutters the game folder)
  ...then zips it to dist\KenshiCoop-kit.zip (the release artifact the README
  and the GitHub release point at).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1

.EXAMPLE
  # Reuse the current build instead of recompiling.
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    # Where to find KenshiCoop.mod / RE_Kenshi.json if they aren't in dist\mods.
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    # The release gate's source of truth. A parameter so the headless suite can
    # point it at a synthetic file and prove the verdict flips BOTH ways; a gate
    # that can only ever say one thing is not a gate.
    [string]$BlockersFile = "",
    # Parse the blockers file, print the derived verdict as JSON and exit.
    # Builds nothing, packages nothing.
    [switch]$VerdictOnly
)

# ------------------------------------------------------- the release gate ----
# docs\RELEASE_BLOCKERS.md is TRACKED (docs\PHASE_*_GATE.md is gitignored) and
# is what decides whether an artifact out of this script may call itself
# shippable. The verdict is DERIVED from the file's open table - never written
# here as a literal - so closing the last row is what flips it, not an edit to
# this script.
function Get-ReleaseBlockers([string]$path) {
    $ids = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $path)) {
        return @{ found = $false; open = @($ids); note = "missing" }
    }
    $inOpen = $false
    foreach ($line in [System.IO.File]::ReadAllLines($path)) {
        if ($line -match '^\s*##\s') { $inOpen = ($line -match '(?i)^\s*##\s+open\s+blockers\s*$'); continue }
        if (-not $inOpen) { continue }
        if ($line -notmatch '^\s*\|') { continue }
        $cells = @($line.Trim().Trim('|') -split '\|' | ForEach-Object { $_.Trim() })
        if ($cells.Count -lt 2) { continue }
        if ($cells[0] -match '(?i)^-+$' -or $cells[0] -match '(?i)^:?-') { continue }
        if ($cells[0] -eq "ID") { continue }
        if ($cells[$cells.Count - 1] -match '(?i)^open$') { [void]$ids.Add($cells[0]) }
    }
    return @{ found = $true; open = @($ids); note = "parsed" }
}

if (-not $BlockersFile) { $BlockersFile = Join-Path $PSScriptRoot "..\docs\RELEASE_BLOCKERS.md" }
$gate = Get-ReleaseBlockers $BlockersFile
$releaseBlockers = @($gate.open)
# shippable is true ONLY when the open table is empty. A missing blockers file
# is NOT an empty one: it is an unknown, and an unknown may not be shipped.
$shippable = [bool]($gate.found -and $releaseBlockers.Count -eq 0)

if ($VerdictOnly) {
    @{
        blockersFile    = (Resolve-Path -LiteralPath $BlockersFile -ErrorAction SilentlyContinue | ForEach-Object { $_.Path })
        blockersFound   = [bool]$gate.found
        releaseBlockers = @($releaseBlockers)
        shippable       = $shippable
    } | ConvertTo-Json -Depth 5
    exit 0
}


$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipBuild) {
    # The PLAYER release ships the Release config: the shipped DLL excludes the
    # scenario harness (~12k lines) and does not define KENSHICOOP_HARNESS
    # (Phase 1 build separation). The test pipeline uses Harness instead.
    Write-Host "=== build plugin (Release / shipped, no scenario harness) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# Resolve the four mod files from the first place each exists.
function Resolve-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "$what not found (looked in: $($candidates -join '; '))"
}
$dll  = Resolve-First @(
    (Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"),
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.dll")
) "KenshiCoop.dll"

# Canonical shipped-DLL hash (Phase 1 provenance). Package from ONE DLL and
# assert the packaged copy is byte-identical to it, so the release artifact's
# SHA-256 is verifiable rather than a mutable file tracked under dist\.
$canonSha = (Get-FileHash -Algorithm SHA256 $dll).Hash
Write-Host "Canonical Release DLL SHA-256: $canonSha"
Write-Host "  source: $dll"
$json = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"),
    (Join-Path $HostDir  "mods\KenshiCoop\RE_Kenshi.json")
) "RE_Kenshi.json"
$mod  = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"),
    (Join-Path $HostDir  "mods\KenshiCoop\KenshiCoop.mod")
) "KenshiCoop.mod"

# Rebuild dist\mod-kit from scratch so no stale install script survives.
$kitDir  = Join-Path $repoRoot "dist\mod-kit"
$modDir  = Join-Path $kitDir "KenshiCoop"
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path $modDir | Out-Null

Write-Host "=== assembling KenshiCoop mod folder ==="
Copy-Item $dll  (Join-Path $modDir "KenshiCoop.dll")
Copy-Item $json (Join-Path $modDir "RE_Kenshi.json")
Copy-Item $mod  (Join-Path $modDir "KenshiCoop.mod")

# The INSTALLERS ship with the kit, beside the KenshiCoop folder - the layout
# both of them already resolve a payload from. This is what closes
# RELEASE_BLOCKERS row KIT-PROVENANCE: the only way to hand someone a playable
# KenshiCoop is now to hand them a kit this script built, which means a stamped
# PROVENANCE.json travels with it. dist\KenshiCoop-friend-kit.zip existed
# because the pipeline produced no installable artifact and a hand-made zip
# filled the gap; an artifact that carries its own installer removes the reason
# to make one by hand.
foreach ($inst in @("install_coop.ps1", "install_coop.sh")) {
    $src = Join-Path $scriptDir $inst
    if (-not (Test-Path -LiteralPath $src)) { throw "$inst not found at $src" }
    Copy-Item $src (Join-Path $kitDir $inst)
}
# install_coop.sh is run by a Linux/Steam Deck player; a CRLF shebang line is
# "bad interpreter: /bin/sh^M". Normalise it on the way into the kit.
$shPath = Join-Path $kitDir "install_coop.sh"
$shText = [System.IO.File]::ReadAllText($shPath) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($shPath, $shText, (New-Object System.Text.UTF8Encoding($false)))

# coop_config.json is deliberately NOT packaged. Since Phase 15 the F2 panel
# arms the endpoint itself, so a fresh install with no config file is a working
# state - and install_coop.ps1 already refuses to write one for the same
# reason. A shipped config is an INVISIBLE INPUT: it would sit on disk saying
# "transport": "steam" while the player sets UDP in the panel, which is a
# support case with no symptom to read. The kit ships the three payload files
# and nothing else.

# Top-level README (sibling to the KenshiCoop folder, so it is NOT copied into
# the game). Plain "copy the folder" instructions - no install script.
$readmeText = @'
KenshiCoop x4 - 3-4 player co-op mod
====================================

This zip contains the "KenshiCoop" folder (that folder IS the mod), an installer
for Windows and one for Linux/Steam Deck, this README, and PROVENANCE.json.
Supports 2, 3, or 4 players over direct UDP / LAN. Everyone must run this same
build: the protocol version is checked when you connect and a mismatch is
rejected, which from the game looks simply like "it will not connect".

PROVENANCE.json records which build this is - the DLL's SHA-256, the protocol
version, and a hash of every file in this zip. If you are unsure what someone
sent you, that file is how you check.

PREREQUISITES (every player)
----------------------------
  1. Kenshi 1.0.65+ (Steam).
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. The Microsoft Visual C++ 2010 x64 runtime (the plugin is built with it).
     On Windows it is usually already present. Under Proton / Steam Deck the
     installer checks for it and tells you what is missing.
  4. The host must be reachable over UDP by every joiner: same LAN, or the
     host's port forwarded / a VPN (Tailscale, Hamachi) for internet play.

INSTALL (every player)
----------------------
  Windows:
    1. Right-click the downloaded zip > Properties > Unblock (if shown), then
       extract it.
    2. In the extracted folder, run:
         powershell -ExecutionPolicy Bypass -File install_coop.ps1
       It finds your Kenshi installation, backs up anything it replaces
       (verifying the backup by hash first), and writes the mod. If it finds
       more than one install it stops and asks which, so pass:
         powershell -ExecutionPolicy Bypass -File install_coop.ps1 -KenshiDir "<path to Kenshi>"
    3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

  Linux / Steam Deck:
    1. Extract the zip, then in that folder run:
         sh ./install_coop.sh --kenshi-dir ~/.local/share/Steam/steamapps/common/Kenshi
       This script does NOT auto-detect: --kenshi-dir is always required, and
       must point at the folder holding kenshi_x64.exe. Same behaviour
       otherwise, plus a check for the VC++ 2010 runtime files Proton needs
       beside kenshi_x64.exe.
    2. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

  To see what is installed without changing anything:
      install_coop.ps1 -Info            /  install_coop.sh --kenshi-dir DIR --info
  To remove it:
      install_coop.ps1 -Uninstall       /  install_coop.sh --kenshi-dir DIR --uninstall
  (The uninstall is driven by the manifest written at install time and restores
  the backups it verified; a file you edited yourself is reported and left
  alone, not deleted.)

PLAY (LAN / direct UDP)
-----------------------
  The connection is set up entirely in-game. There is no config file to edit.

  1. HOST: load a save, or start a new game and pick a co-op start from the list
     that matches your player count (see GAME STARTS below). Press F2, set
     Transport: UDP and Role: HOST, then toggle Connection to ONLINE. Tell the
     other players your IP address and port (default 27800).
  2. EACH JOINER: press F2 (works at the MAIN MENU - no save needed), set
     Transport: UDP and Role: JOIN, paste the host's address into the peer
     address field, then toggle Connection to ONLINE. The host streams its world
     to you on connect and you load right into it. Joiners connect to the host
     only, never to each other. (If you already have an identical copy of the
     host's save on disk it is used as-is instead of transferring.)
  3. The white status line and the TOP-LEFT banner show live connection/transfer
     state, at the main menu as well as in-game. Toggle Connection to OFFLINE to
     leave; the others keep playing.

GAME STARTS (one squad tab per player)
--------------------------------------
  The host runs squad 1; joiners take squads 2, 3, 4. Everyone's squad is
  visible and synced on every screen but answers only to its owner. New Game ->
  pick the bundled start that matches your player count, each pre-splits
  wanderers into separate squads so nobody has to split tabs by hand:
    * "Multiplayer (Wanderer x4)"  - four squads, for 3-4 players.
    * "Multiplayer (Wanderer x2)"  - two squads, for two players.
    * "Multiplayer+ (Wanderer x2)" - the x2 start with 500,000 cats (shared
                                      wallet) and both characters at 50 in every
                                      stat, to skip the early grind.
  With fewer players than squads, the unused squads just sit idle. You can also
  load any existing save and split units into extra squad tabs in-game.

STEAM (two players only)
------------------------
  The original two-player Steam P2P path still works but is NOT extended to 3-4
  players. For two players you may instead leave Transport on STEAM and swap
  Steam IDs in-game (F2 -> "Copy my Steam ID" / "Paste friend's Steam ID").

SAVING
------
  Any save any player makes during a session becomes one shared save on every
  machine, streamed automatically. To resume, the host loads it and goes online;
  the others reconnect from the main menu.

UNINSTALL
---------
  Run the installer again with -Uninstall (Windows) or --uninstall (Linux). It
  reverses exactly what it recorded at install time. Removing
  <Kenshi>\mods\KenshiCoop by hand also works.

KNOWN LIMITATIONS (worth knowing before you start)
--------------------------------------------------
  * Take a save sized to your group. A squad tab beyond the number of connected
    players is owned by NOBODY: every client can move it, and its state is not
    synced between you. Use the start that matches your player count.
  * A knocked-out body's resting position can drift apart between clients while
    it lies there. It is static while down and corrects when the character gets
    back up.
  * Three and four players over UDP is the new capability and it is a hobby
    project. Expect rough edges; the full, current list of known defects is in
    .planning/WINDOWS.md in the repository.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (UDP): the address each joiner pasted into the F2 panel must
    be the host's, and the host must be reachable over UDP (LAN, or port
    forwarded / VPN for internet play). Look for connection lines in
    <Kenshi>\KenshiCoop_*.log.
  * It just will not connect, with no other symptom: check the log for
    "protocol mismatch". Someone has a different build. Every player must
    install the SAME zip - compare dllSha256 in PROVENANCE.json.
'@

# A player who reconnects and is told 'slots full' deserves to have read
# about it first. The list is the SAME one PROVENANCE.json carries.
#
# The reconnect paragraph is DERIVED from that list, not unconditional. It
# describes exactly one blocker - WINDOWS-19's host-slot leak - and printing it
# while that row is closed hands a player a workaround ("wait ten seconds") for
# a defect their build does not have, which is its own kind of wrong. Keying it
# to the blocker means the README can no longer drift from the gate.
$reconnectBlockers = @($releaseBlockers | Where-Object { $_ -match '^WINDOWS-(19|22)$' })
if ($releaseBlockers.Count -gt 0) {
    $known = "`r`n`r`nKNOWN ISSUES (this build is NOT a finished release)`r`n"
    $known += "---------------------------------------------------`r`n"
    if ($reconnectBlockers.Count -gt 0) {
        $known += "  * RECONNECTING can use up a player slot. If you disconnect and come`r`n"
        $known += "    straight back, the host may still be holding your old slot for a few`r`n"
        $known += "    seconds, so you return under a different player id - and with 3 or 4`r`n"
        $known += "    players a second quick reconnect can be told 'slots full'. Wait about`r`n"
        $known += "    ten seconds before reconnecting, or have the host restart the session.`r`n"
    }
    $known += "  * Open release blockers in this build: " + ($releaseBlockers -join ", ") + "`r`n"
    $known += "    See docs/RELEASE_BLOCKERS.md in the repository for what each one means.`r`n"
    $readmeText += $known
}
# UTF-8 WITHOUT a BOM, the convention the rest of this project writes with.
[System.IO.File]::WriteAllText((Join-Path $kitDir "README.txt"), $readmeText,
    (New-Object System.Text.UTF8Encoding($false)))


# Provenance: assert the PACKAGED DLL is byte-identical to the canonical build,
# then record the hash next to the kit so the release artifact is verifiable.
$packagedDll = Join-Path $modDir "KenshiCoop.dll"
$packagedSha = (Get-FileHash -Algorithm SHA256 $packagedDll).Hash
if ($packagedSha -ne $canonSha) {
    throw "packaged DLL hash ($packagedSha) != canonical Release DLL hash ($canonSha)"
}
$protoLine = Select-String -Path (Join-Path $repoRoot "src\netproto\Wire.h") `
    -Pattern 'PROTOCOL_VERSION\s*=\s*(\d+)' | Select-Object -First 1
$proto = if ($protoLine) { $protoLine.Matches[0].Groups[1].Value } else { "?" }
# Every packaged file, hashed. The DLL hash alone identifies the build but says
# nothing about the installer that writes it or the config it drops, and those
# are files a recipient runs. A manifest lets anyone re-hash what they received
# and compare, which is the whole point of shipping provenance rather than a
# version string.
$fileManifest = [ordered]@{}
foreach ($f in @(Get-ChildItem -Recurse -File -LiteralPath $kitDir | Sort-Object FullName)) {
    $rel = $f.FullName.Substring($kitDir.Length + 1).Replace("\", "/")
    $fileManifest[$rel] = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash
}
@{
    dllSha256       = $canonSha
    protocolVersion = $proto
    builtUtc        = (Get-Date).ToUniversalTime().ToString("o")
    config          = "Release"
    blockersFile    = "docs/RELEASE_BLOCKERS.md"
    releaseBlockers = @($releaseBlockers)
    shippable       = $shippable
    producedBy      = "scripts/make_mod_kit.ps1"
    repository      = "https://github.com/Asphacean/KenshiCoop_x4"
    files           = $fileManifest
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $kitDir "PROVENANCE.json") -Encoding UTF8
Write-Host "Packaged DLL SHA-256 verified == canonical."
Write-Host ("Hashed $($fileManifest.Count) packaged files into PROVENANCE.json.")

# The hand-assembled friend kit is retired. It carried no PROVENANCE.json, so a
# recipient could not tell which build they had - the support case the kit
# pipeline exists to remove (RELEASE_BLOCKERS row KIT-PROVENANCE). Deleting it
# here means it cannot quietly reappear beside a stamped kit and get sent to
# someone by mistake.
foreach ($stale in @(
    (Join-Path $repoRoot "dist\KenshiCoop-friend-kit.zip"),
    (Join-Path $repoRoot "dist\friend-kit")
)) {
    if (Test-Path -LiteralPath $stale) {
        Remove-Item -LiteralPath $stale -Recurse -Force
        Write-Host ("Removed un-stamped artifact: " + $stale)
    }
}

# Zip: the archive contains the KenshiCoop\ folder + README.txt + PROVENANCE.json.
$zip = Join-Path $repoRoot "dist\KenshiCoop-kit.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $kitDir "*") -DestinationPath $zip

Write-Host ""
Write-Host "Mod folder: $modDir"
Write-Host "Kit zipped: $zip"
Get-ChildItem -Recurse $kitDir | ForEach-Object {
    Write-Host ("  " + $_.FullName.Substring($kitDir.Length + 1))
}

Write-Host ""
if ($shippable) {
    Write-Host "RELEASE GATE: shippable=true - no open blocker in $BlockersFile."
} else {
    Write-Host "RELEASE GATE: shippable=false - this kit is for TESTING, not for publication."
    if (-not $gate.found) {
        Write-Host "  the blockers file was NOT FOUND at $BlockersFile; an unknown gate is not an open door."
    }
    foreach ($b in $releaseBlockers) { Write-Host ("  open blocker: " + $b) }
    if ($reconnectBlockers.Count -gt 0) {
        Write-Host "  README.txt carries the reconnect symptom in player language."
    } else {
        Write-Host "  README.txt omits the reconnect symptom: WINDOWS-19/-22 are closed."
    }
}
