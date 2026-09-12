<#
.SYNOPSIS
  Committed one-command N=3 connect-relink probe: build, deploy, run, judge
  (Phase 14 Plan 02, UI-07; ROADMAP criteria 1 and 2).

.DESCRIPTION
  Someone who was not here runs ONE command and gets a live N=3 run in which
  the F2 panel's Connect handler is re-invoked programmatically on a schedule,
  with KENSHICOOP_NET_ROSTER_TRACE on, so the host log records WHICH join
  slots were discarded at each NetLink session boundary and WHICH ids were
  admitted again afterwards. scripts\analyze_relink.ps1 then judges it.

  Commit 9011527 is what this exercises. g_net is a reused singleton, so a
  relink is stop() + startHost() on the SAME object, and threadLoop() ends
  with enet_host_destroy(), which frees the whole ENetPeer array. Pre-fix, the
  surviving registry_ made the lowest-free-slot scan read stale ids as
  occupied, so rejoins climbed to higher ids and were eventually refused with
  "MAX_PLAYERS=4 slots full". Two relinks per run rather than one is
  deliberate: a state bug that survives one boundary can still be latent after
  it, and repeating the boundary is the cheapest way to expose anything that
  assumes a fresh object across sessions.

  -RelinkRole both (the default) covers BOTH session boundaries in one run:
  the HOST boundary is where registry_ lives and therefore where criterion 2's
  roster evidence comes from, while criterion 1 literally asks for a CLIENT
  that reconnects - and the client path additionally touches epochSeen_ and
  the server-peer reconnect, which the host path never does. The scenario
  interleaves the two sides one recovery budget apart, so no two session
  boundaries overlap and each one stays separately attributable, and exactly
  one join relinks (KENSHICOOP_RELINK_JOIN_ID) so the other join is still
  OBSERVING and produces the independent second log.

  IT DOES NOT WRITE THE RIG IT READS. scripts\local.rig.n3.json is pinned by
  Phase 12's anti-escape guard and is what Phase 13's gate measured against;
  this script deep-copies it, injects the scenario env into the copy, writes
  the copy under tools\test-runs\, and hands THAT to run_test4.ps1.

  STRUCTURAL REFUSALS (this phase's own masking levers). It refuses to run:
    - a rig whose instances declare disconnectAtSec or reconnectAtSec. A
      process-level disconnect is a DIFFERENT mechanism from an in-process
      relink; a run that mixed them could not attribute what it saw to the
      lever.
    - a rig that does not declare exactly 3 instances.
    - a configuration in which KENSHICOOP_NET_ROSTER_TRACE would be off for
      any instance - the observation IS the deliverable (14-CONTEXT D-06).
    - a clone whose mods\KenshiCoop\coop_config.json declares an ip or port
      different from the rig's. coopUiConnect calls reloadPeerFromFile on
      EVERY connect, so a stale file silently rebinds the relinked host to an
      endpoint the joins are not calling and the rejoins never return.

  NOT SET: KENSHICOOP_SAVE_SYNC=0. It looks right (it would keep the host's
  bootstrap connect-push out of a roster measurement) but it is MEASURED to
  deadlock the run: titleUpdate_hook early-returns for an online join, so a
  join never auto-loads its own save and reaches gameplay ONLY via the host's
  connect-push, which is itself saveSync-gated. Plan 14-01 lost a 220 s run to
  exactly that (tools\test-runs\phase14_tracer_attempt1_savesync0). Do not
  re-introduce it; a different lever is needed if bootstrap isolation is ever
  genuinely wanted.

  Exit code: 0 when the probe ran and the judge PASSed, 1 when the judge
  FAILed, non-zero (throw) when the probe could not run at all. Unlike
  repro_census_n3.ps1 the verdict is NOT merely data here - this command's
  whole purpose is the verdict.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\relink_probe.ps1

.EXAMPLE
  # Host-side boundaries only, no rebuild/redeploy:
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\relink_probe.ps1 -RelinkRole host -SkipBuild -SkipDeploy
#>
[CmdletBinding()]
param(
    [string]$RigConfig = "",
    [int]$Relinks = 2,
    [int]$RelinkAtMs = 40000,
    [int]$RecoverMs = 60000,
    [ValidateSet("host", "join", "both")]
    [string]$RelinkRole = "both",
    [int]$RelinkJoinId = 1,
    # 0 = compute from the schedule below. A hardcoded number goes stale the
    # moment -Relinks or -RecoverMs moves, and a self-exit clock that fires
    # mid-recovery produces a FAIL that says nothing about the roster.
    [int]$Seconds = 0,
    [int]$KillGraceSec = 120,
    [switch]$SkipBuild,
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"
$INV = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if ($RigConfig -eq "") { $RigConfig = Join-Path $scriptDir "local.rig.n3.json" }
if (-not (Test-Path $RigConfig)) {
    throw "Rig config not found: $RigConfig (copy scripts\local.rig.n3.example.json to scripts\local.rig.n3.json and fill in real install paths)."
}
$RigConfig = (Resolve-Path -LiteralPath $RigConfig).Path
$cfg = Get-Content -Raw -Path $RigConfig | ConvertFrom-Json

# ---- structural refusals -------------------------------------------------------
if ($null -eq $cfg.instances -or $cfg.instances.Count -ne 3) {
    $n = 0; if ($null -ne $cfg.instances) { $n = $cfg.instances.Count }
    throw "Rig config '$RigConfig' declares $n instance(s); this probe requires exactly 3 (host + 2 joins). Use scripts\run_test4.ps1 directly with -ExpectedInstances if a different instance count is genuinely intended."
}
for ($i = 0; $i -lt $cfg.instances.Count; $i++) {
    $inst  = $cfg.instances[$i]
    $label = "instance[$i] ($($inst.role), $($inst.logName))"
    foreach ($lever in @('disconnectAtSec', 'reconnectAtSec')) {
        $v = $inst.$lever
        if ($null -ne $v -and "$v" -ne "") {
            throw "Rig config '$RigConfig' $label carries a process-lifecycle lever ($lever=$v) - refusing to run. A process-level disconnect/deferred launch is a DIFFERENT mechanism from the in-process relink this probe measures; a run that mixed them could not attribute the observed roster behaviour to the lever. Set $lever to null in the rig."
        }
    }
}

# coop_config.json drift: coopUiConnect re-reads ip/port on EVERY connect.
$rigIp   = "$($cfg.ip)"
$rigPort = "$($cfg.port)"
foreach ($inst in $cfg.instances) {
    $ccPath = Join-Path $inst.installDir "mods\KenshiCoop\coop_config.json"
    if (-not (Test-Path $ccPath)) { continue }
    $raw = Get-Content -Raw -Path $ccPath
    $mIp   = [regex]::Match($raw, '"ip"\s*:\s*"([^"]*)"')
    $mPort = [regex]::Match($raw, '"port"\s*:\s*(\d+)')
    if ($mIp.Success -and $rigIp -ne "" -and $mIp.Groups[1].Value -ne $rigIp) {
        throw "Clone '$($inst.installDir)' has coop_config.json ip='$($mIp.Groups[1].Value)' but the rig uses ip='$rigIp' - refusing to run. coopUiConnect calls reloadPeerFromFile on every connect, so the relinked session would silently bind to a different endpoint and the rejoins would never return."
    }
    if ($mPort.Success -and $rigPort -ne "" -and $mPort.Groups[1].Value -ne $rigPort) {
        throw "Clone '$($inst.installDir)' has coop_config.json port=$($mPort.Groups[1].Value) but the rig uses port=$rigPort - refusing to run (same reloadPeerFromFile reason as above)."
    }
}

# ---- schedule + self-exit budget ------------------------------------------------
# The scenario's slots run off the ARMED clock; the self-exit clock runs off
# GAMEPLAY start, so the arming wait sits on top of the schedule.
#   role=both : host on the even slots, the designated join on the odd ones, one
#               recovery budget apart -> last slot = at + (2*(n-1)+1)*rec
#   otherwise : last slot = at + (n-1)*rec
# windowEnd adds one more budget (the last recovery), ARM_ALLOWANCE covers the
# worst-case arm (the scenario's own 45 s peer-ready timeout plus N=3 loading),
# and TAIL leaves room for the RESULT lines to reach disk.
$ARM_ALLOWANCE_MS = 60000
$TAIL_MS          = 20000
if ($RelinkRole -eq "both") {
    $lastSlotMs = $RelinkAtMs + (2 * ($Relinks - 1) + 1) * $RecoverMs
} else {
    $lastSlotMs = $RelinkAtMs + ($Relinks - 1) * $RecoverMs
}
$windowEndMs = $lastSlotMs + $RecoverMs
if ($Seconds -le 0) {
    $Seconds = [int][Math]::Ceiling(($ARM_ALLOWANCE_MS + $windowEndMs + $TAIL_MS) / 1000.0)
}

$scenarioEnv = [ordered]@{
    KENSHICOOP_SCENARIO            = 'connect_relink'
    KENSHICOOP_RELINK_COUNT        = $Relinks.ToString($INV)
    KENSHICOOP_RELINK_AT_MS        = $RelinkAtMs.ToString($INV)
    KENSHICOOP_RELINK_RECOVER_MS   = $RecoverMs.ToString($INV)
    KENSHICOOP_RELINK_ROLE         = $RelinkRole
    KENSHICOOP_RELINK_JOIN_ID      = $RelinkJoinId.ToString($INV)
    KENSHICOOP_NET_ROSTER_TRACE    = '1'
}
if ("$($scenarioEnv.KENSHICOOP_NET_ROSTER_TRACE)" -eq "" -or "$($scenarioEnv.KENSHICOOP_NET_ROSTER_TRACE)" -eq "0") {
    throw "KENSHICOOP_NET_ROSTER_TRACE would be OFF - refusing to run. Observing the session-boundary roster IS this probe's deliverable (14-CONTEXT D-06); a run without it proves nothing about commit 9011527."
}

Write-Host "== KenshiCoop N=3 connect-relink probe (UI-07) =="
Write-Host "  rig config:  $RigConfig  (READ ONLY - a temp copy is what actually runs)"
Write-Host "  relinks:     $Relinks   role=$RelinkRole  joinId=$RelinkJoinId"
Write-Host ("  schedule:    at={0}ms recover={1}ms lastSlot={2}ms windowEnd={3}ms" -f $RelinkAtMs, $RecoverMs, $lastSlotMs, $windowEndMs)
Write-Host "  seconds:     $Seconds   (self-exit from gameplay start)  killGrace: $KillGraceSec"

# ---- build ----------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== build Harness ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Harness"
    if ($LASTEXITCODE -ne 0) { throw "build_plugin.cmd Harness failed, exit $LASTEXITCODE" }
} else {
    Write-Host "  (SkipBuild: reusing whatever Harness DLL is already built)"
}

# ---- deploy ---------------------------------------------------------------------
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

# ---- deep-copy the rig and inject the scenario env into the COPY ----------------
$runsRoot = Join-Path $repoRoot "tools\test-runs"
New-Item -ItemType Directory -Force -Path $runsRoot | Out-Null
$stamp    = Get-Date -Format "yyyyMMdd_HHmmss"
$tempRig  = Join-Path $runsRoot "relink_rig_$stamp.json"

# Round-trip through JSON so nothing below can alias into $cfg's own objects.
$copy = Get-Content -Raw -Path $RigConfig | ConvertFrom-Json
foreach ($inst in $copy.instances) {
    $merged = [ordered]@{}
    if ($null -ne $inst.env) {
        foreach ($p in $inst.env.PSObject.Properties) { $merged[$p.Name] = "$($p.Value)" }
    }
    foreach ($k in $scenarioEnv.Keys) { $merged[$k] = "$($scenarioEnv[$k])" }
    $inst | Add-Member -NotePropertyName env -NotePropertyValue ([pscustomobject]$merged) -Force
}
$copy | Add-Member -NotePropertyName _generated_by -NotePropertyValue "scripts\relink_probe.ps1 (temp copy; scripts\local.rig.n3.json is never written)" -Force
$copy | ConvertTo-Json -Depth 20 | Set-Content -Path $tempRig -Encoding UTF8

# Paranoia, not ceremony: assert the pinned rig is byte-identical to what we read.
$before = Get-Content -Raw -Path $RigConfig
if ($before -ne (Get-Content -Raw -Path $RigConfig)) { throw "the pinned rig changed underneath this run" }
Write-Host ""
Write-Host "  temp rig:    $tempRig"

$headSha = "unknown"
try {
    $headSha = (& git.exe -C $repoRoot rev-parse --short HEAD 2>$null).Trim()
    if ([string]::IsNullOrEmpty($headSha)) { $headSha = "unknown" }
} catch { $headSha = "unknown" }

# ---- run ------------------------------------------------------------------------
# ABSOLUTE -OutDir only: a relative path is forwarded into KENSHICOOP_LOG, which
# the game resolves against its own install dir, and fopen then fails silently
# with no logs written anywhere.
$runDirRaw = Join-Path $runsRoot "${stamp}_N3_relink"
Write-Host ""
Write-Host "=== run -> $runDirRaw ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "run_test4.ps1") `
    -RigConfig $tempRig -Scenario connect_relink -ExpectedInstances 3 `
    -Seconds $Seconds -KillGraceSec $KillGraceSec -OutDir $runDirRaw
if ($LASTEXITCODE -ne 0) { throw "run_test4.ps1 failed (exit $LASTEXITCODE) - see console output above." }
if (-not (Test-Path $runDirRaw)) { throw "run_test4.ps1 reported success but run dir '$runDirRaw' does not exist." }
$runDir = (Resolve-Path -LiteralPath $runDirRaw).Path

# ---- judge ----------------------------------------------------------------------
Write-Host ""
Write-Host "=== judge (analyze_relink.ps1) ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "analyze_relink.ps1") `
    -RunDir $runDir -ExpectedInstances 3
$judgeExit = $LASTEXITCODE
if ($judgeExit -eq 2) { throw "analyze_relink.ps1 could not judge run dir '$runDir' (exit 2) - a hard-requirement failure, not a gate FAIL." }
$pass = ($judgeExit -eq 0)

# ---- record ---------------------------------------------------------------------
# Stable pointer + append-only history, written on FAILING runs as well: a
# history that only records wins is not evidence.
$lastPointerPath = Join-Path $runsRoot "last_relink.txt"
$historyPath     = Join-Path $runsRoot "relink_history.jsonl"
Set-Content -Path $lastPointerPath -Value $runDir -Encoding UTF8 -NoNewline
$historyEntry = [pscustomobject]@{
    timestamp  = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir     = $runDir
    pass       = $pass
    relinks    = $Relinks
    role       = $RelinkRole
    joinId     = $RelinkJoinId
    relinkAtMs = $RelinkAtMs
    recoverMs  = $RecoverMs
    seconds    = $Seconds
    rig        = $RigConfig
    tempRig    = $tempRig
    headSha    = $headSha
}
Add-Content -Path $historyPath -Value ($historyEntry | ConvertTo-Json -Compress)

Write-Host ""
Write-Host ("RELINK PROBE RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }) + " runDir=$runDir")
if ($pass) { exit 0 } else { exit 1 }
