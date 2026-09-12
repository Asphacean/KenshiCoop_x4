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

# coop_config.json (LAN/UDP only; Steam play needs no config). Written fresh so
# the release always ships a clean default.
@'
{
  // KenshiCoop config. For a normal Steam game you do NOT need to edit this file:
  // your friend's Steam ID is entered in-game (press F2, click "Copy my Steam ID"
  // to share yours, then "Paste friend's Steam ID" to enter theirs), and nothing
  // is written back to disk.
  //
  // This file only matters for a LAN / direct-UDP game: set "transport": "udp"
  // and put the host's address in "ip" (and "port" if you changed it). ip/port are
  // re-read each time you click Connect, so you can edit them without restarting.
  "transport": "steam",
  "ip": "127.0.0.1",
  "port": 27800,
  "autoConnect": false
}
'@ | Set-Content (Join-Path $modDir "coop_config.json") -Encoding UTF8

# Top-level README (sibling to the KenshiCoop folder, so it is NOT copied into
# the game). Plain "copy the folder" instructions - no install script.
$readmeText = @'
KenshiCoop x4 - 3-4 player co-op mod
====================================

This zip contains the "KenshiCoop" folder (that folder IS the mod), plus this
README and PROVENANCE.json. Supports 2, 3, or 4 players over direct UDP / LAN.
Everyone must run this same build.

INSTALL (every player)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Copy the "KenshiCoop" folder into your Kenshi mods folder:
       <Kenshi>\mods\
     so you end up with:
       <Kenshi>\mods\KenshiCoop\KenshiCoop.dll   (and the other files)
     The default Steam path is:
       C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

PREREQUISITES (every player)
----------------------------
  1. Kenshi 1.0.65 (Steam).
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. The host must be reachable over UDP by every joiner: same LAN, or the
     host's port forwarded / a VPN for internet play.

PLAY (LAN / direct UDP)
-----------------------
  1. Each JOINER edits <Kenshi>\mods\KenshiCoop\coop_config.json (Notepad):
       "transport": "udp"
       "ip":   the HOST's address   (e.g. "192.168.1.10")
       "port": the HOST's port      (default 27800)
     ip/port are re-read whenever you go ONLINE, so no restart after an edit.
     The host only needs "transport": "udp".
  2. HOST: load a save, or start a new game and pick a co-op start from the list
     that matches your player count (see GAME STARTS below). Press F2, set
     Transport: UDP and Role: HOST, then toggle Connection to ONLINE.
  3. EACH JOINER: press F2 (works at the MAIN MENU - no save needed), set
     Transport: UDP and Role: JOIN, then toggle Connection to ONLINE. The host
     streams its world to you on connect and you load right into it. Joiners
     connect to the host only, never to each other. (If you already have an
     identical copy of the host's save on disk it is used as-is instead of
     transferring.)
  4. The white status line and the TOP-LEFT banner show live connection/transfer
     state, at the main menu as well as in-game. Toggle Connection to OFFLINE to
     leave.

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
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (UDP): the joiners' ip/port must match the host, and the host
    must be reachable over UDP (LAN, or port forwarded / VPN for internet play).
    Look for connection lines in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": someone has a different build; everyone should use the
    same release.
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
@{
    dllSha256       = $canonSha
    protocolVersion = $proto
    builtUtc        = (Get-Date).ToUniversalTime().ToString("o")
    config          = "Release"
    blockersFile    = "docs/RELEASE_BLOCKERS.md"
    releaseBlockers = @($releaseBlockers)
    shippable       = $shippable
} | ConvertTo-Json | Set-Content (Join-Path $kitDir "PROVENANCE.json") -Encoding UTF8
Write-Host "Packaged DLL SHA-256 verified == canonical."

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
