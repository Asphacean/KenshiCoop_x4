<#
.SYNOPSIS
  Config-driven N-instance (host + up to 3 joins) KenshiCoop test launcher for the
  Milestone A gate (POC-03). Generalizes scripts\run_test.ps1's proven 2-instance
  launch primitives (stale-kill, save restore, staggered load, per-instance env,
  window arrange/screenshot, self-exit kill-grace) to an arbitrary instance list
  read from a rig config file, WITHOUT modifying run_test.ps1 itself.

.DESCRIPTION
  Reads -RigConfig (default scripts\local.rig.json; see
  scripts\local.rig.example.json for the field reference). Element 0 of the
  config's "instances" array is the HOST; the remaining elements are JOINs,
  launched in array order.

  HARD REQUIREMENT: if the config declares fewer instances than
  -ExpectedInstances (default 4: host + 3 joins), or any configured
  installDir is missing kenshi_x64.exe, this script THROWS with the exact
  missing item. It never silently proceeds with fewer instances.

  PROCESS-LIFECYCLE ORCHESTRATION: each join may carry optional
  disconnectAtSec / reconnectAtSec fields (seconds measured from the HOST
  reaching "gameplay started" - the one stable anchor every instance can be
  measured against, regardless of launch stagger).
    - disconnectAtSec: this instance is force-stopped at that offset while
      the other instances keep running (the live "one client disconnects"
      DoD step).
    - reconnectAtSec: this instance's FIRST launch is deferred to that
      offset instead of the normal staggered start (the "replacement/rejoin
      join" DoD step - a fresh join launched later is an acceptable
      replacement connect per the Phase 4 CONTEXT).
  A join with neither field set launches normally in the staggered
  host-then-joins sequence every prior 2-instance run already relies on.

  This launcher's job ends at "N per-instance logs collected in one run
  dir" - it does NOT compute a pass/fail verdict (that is
  scripts\analyze_run4.ps1, plan 05's scope).

.EXAMPLE
  powershell -File scripts\run_test4.ps1 -RigConfig scripts\local.rig.json -Save squad1 -Seconds 90
#>
[CmdletBinding()]
param(
    [string]$RigConfig = "",
    [string]$Save = "",
    [int]$Seconds = 0,
    [string]$Scenario = "",
    [string]$Setup = "",
    [string]$OutDir = "",
    [switch]$NoKill,
    [int]$JoinDelaySec = 8,
    [int]$StartTimeoutSec = 90,
    [int]$Frames = 5,
    [int]$FrameIntervalMs = 16,
    [int]$ShotLeadSec = 5,
    [switch]$NoArrange,
    [ValidateSet("widest", "primary")]
    [string]$ArrangeMonitor = "primary",
    [int]$ArrangeRepeatSec = 75,
    [int]$KillGraceSec = -1,
    # Fail loud (never silently run fewer) unless the config genuinely
    # declares fewer instances AND the caller explicitly lowers this.
    [int]$ExpectedInstances = 4,
    # Poll cadence for the disconnect/reconnect lifecycle monitor.
    [int]$LifecyclePollSec = 1
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopHarness.psm1") -Force
Import-Module (Join-Path $scriptDir "CoopOraclesN.psm1") -Force

# ---- Manifest entry resolution (07-04 gap fix): the N=4 launcher must apply
# the scenario's manifest DiagEnv exactly like run_test.ps1:357 does - before
# this fix it passed $null to Set-CoopDiagEnv, which CLEARS the managed keyset
# and never applies the manifest's DiagEnv. That was latent for
# milestone_a_gate/player_state_gate (neither declares DiagEnv) but broke
# item_conservation_gate outright: KENSHICOOP_INV_SYNC/KENSHICOOP_WORLD_SYNC
# stayed OFF, so the transfer/claim planes the gate exists to exercise never
# fired (run 20260902_214911_N4: 5 authored XFERs, 0 [xfer] COMMIT verdicts).
$manifestEntry = $null
if ($Scenario -ne "") {
    $manifest = Get-ScenarioManifest
    if ($manifest.Scenarios.ContainsKey($Scenario)) {
        $manifestEntry = $manifest.Scenarios[$Scenario]
    }
}

if ($RigConfig -eq "") { $RigConfig = Join-Path $scriptDir "local.rig.json" }
if (-not (Test-Path $RigConfig)) {
    throw "Rig config not found: $RigConfig (copy scripts\local.rig.example.json to scripts\local.rig.json and fill in real install paths)."
}
$cfg = Get-Content -Raw -Path $RigConfig | ConvertFrom-Json

if ($null -eq $cfg.instances -or $cfg.instances.Count -eq 0) {
    throw "Rig config '$RigConfig' declares no instances."
}
if ($cfg.instances.Count -lt $ExpectedInstances) {
    throw "Rig config '$RigConfig' declares $($cfg.instances.Count) instance(s); at least $ExpectedInstances required (host + $($ExpectedInstances - 1) join(s)). Refusing to silently run fewer - add the missing instance(s) or pass -ExpectedInstances explicitly."
}

# ---- HARD REQUIREMENT: every configured install must actually exist --------
for ($i = 0; $i -lt $cfg.instances.Count; $i++) {
    $inst = $cfg.instances[$i]
    if ([string]::IsNullOrEmpty($inst.installDir)) {
        throw "Rig config instance[$i] has no installDir."
    }
    $exe = Join-Path $inst.installDir "kenshi_x64.exe"
    if (-not (Test-Path $exe)) {
        throw "Rig config instance[$i] (installDir='$($inst.installDir)') is missing kenshi_x64.exe at '$exe'."
    }
}

$hostInst = $cfg.instances[0]
$joinInsts = @($cfg.instances[1..($cfg.instances.Count - 1)])

if ($Save -eq "") { $Save = $cfg.save }
if ($Save -eq "" -or $null -eq $Save) { throw "No -Save given and rig config has no default 'save'." }
# 08-04 gap fix: the rig config's "seconds" field is a per-machine fallback
# tuned for whichever gate first set it up (local.rig.json's own comment:
# "210 matches ... milestone_a_gate") - a LONGER-running scenario (e.g.
# world_state_gate's Seconds=280, HOST_DURATION_MS=240000) silently inherited
# the shorter rig default because this launcher never consulted the
# manifest's own Seconds/KillGraceSec, unlike run_test.ps1's proven
# precedent (:176-181, :537-540). A scenario-authored duration now wins over
# the rig fallback whenever the caller didn't explicitly pass -Seconds.
if ($Seconds -le 0) {
    if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey("Seconds")) {
        $Seconds = [int]$manifestEntry.Seconds
    } elseif ($cfg.seconds) {
        $Seconds = [int]$cfg.seconds
    } else {
        $Seconds = 90
    }
}
$hostPort = if ($hostInst.port) { [int]$hostInst.port } elseif ($cfg.port) { [int]$cfg.port } else { 27800 }
$hostIp   = if ($cfg.ip) { "$($cfg.ip)" } else { "127.0.0.1" }

if ($OutDir -eq "") {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\${stamp}_N$($cfg.instances.Count)"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "== KenshiCoop N-instance test run =="
Write-Host "  instances: $($cfg.instances.Count) (1 host + $($joinInsts.Count) join(s))"
Write-Host "  save:      $Save"
Write-Host "  seconds:   $Seconds"
Write-Host "  out dir:   $OutDir"

# ---- Determine each instance's real save root (A3 finding: a clone with
# "User save location=1" reads/writes its OWN install-local save\, never the
# shared %LOCALAPPDATA%\kenshi\save the primary Steam install normally uses).
function Get-CoopSaveRoot {
    param([string]$InstallDir)
    $cfgPath = Join-Path $InstallDir "settings.cfg"
    if (Test-Path -LiteralPath $cfgPath) {
        $text = Get-Content -LiteralPath $cfgPath -Raw -ErrorAction SilentlyContinue
        if ($text -match '(?im)^\s*User save location\s*=\s*1\s*$') {
            return (Join-Path $InstallDir "save")
        }
    }
    return (Join-Path $env:LOCALAPPDATA "kenshi\save")
}

# ---- Stale-process cleanup --------------------------------------------------
if (-not $NoKill) {
    $killed = Stop-CoopKenshi -SettleSec 2
    if ($killed -gt 0) { Write-Host "  killed $killed stale Kenshi process(es) before starting ..." }
}

# ---- Restore the pristine save fixture into EVERY instance's own save root -
# ALSO always restore the shared %LOCALAPPDATA%\kenshi\save copy: run
# 20260903_161831_N4 proved the clones actually LOAD (and connect-push
# re-bake) the AppData copy despite "User save location=1" in settings.cfg -
# the A3 install-local interpretation left the AppData copy unrestored, so a
# drift baked there (run 151044's garbage-spawn teleport) silently persisted
# across every later run's "pristine" restore. Restoring BOTH roots is
# idempotent and closes the gap regardless of which one a given
# Kenshi/RE_Kenshi build reads.
$allInsts = @($hostInst) + $joinInsts
$seenRoots = @{}
$appDataRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
$rootsToRestore = @($appDataRoot) + @($allInsts | ForEach-Object { Get-CoopSaveRoot -InstallDir $_.installDir })
foreach ($root in $rootsToRestore) {
    if ($seenRoots.ContainsKey($root)) { continue }
    $seenRoots[$root] = $true
    Write-Host "  restoring save '$Save' -> $root"
    & powershell -NoProfile -File (Join-Path $scriptDir "deploy_saves.ps1") -Save $Save -SaveRoot $root
    if ($LASTEXITCODE -ne 0) { throw "deploy_saves.ps1 failed for '$Save' -> '$root' ($LASTEXITCODE)" }
}

# ---- Consistent window size across all instances (best-effort; reuses the
# existing 2-arg primitive degenerately per install rather than forking it) -
foreach ($inst in $allInsts) {
    & powershell -NoProfile -File (Join-Path $scriptDir "set_video_mode.ps1") `
        -Width 1280 -Height 1024 -HostDir $inst.installDir -JoinDir $inst.installDir | Out-Null
}

function Set-Inst4Env {
    # -JoinDurationSec (04-07 gap-closure, root cause 2): only set for a JOIN
    # launched LATE relative to the host (reconnectAtSec DoD step) - the
    # caller has already computed the real host-relative remaining safe
    # window (see the deferred-launch site below). Every other launch (host,
    # normal staggered joins) omits it, which CLEARS any stale value left in
    # this process's environment from a prior instance in the same run -
    # $env: vars persist across Set-Inst4Env calls within one script process,
    # so an explicit clear is required, not just "don't set it".
    param($Inst, [bool]$IsHost, [string]$LogPath, [Nullable[int]]$JoinDurationSec = $null)
    $env:KENSHICOOP_MODE         = if ($IsHost) { "host" } else { "join" }
    $env:KENSHICOOP_TRANSPORT    = "udp"
    $env:KENSHICOOP_STEAM_PEER   = "0"
    $env:KENSHICOOP_PORT         = "$hostPort"
    $env:KENSHICOOP_IP           = $hostIp
    $env:KENSHICOOP_SAVE         = $Save
    $env:KENSHICOOP_TEST_SECONDS = "$Seconds"
    $env:KENSHICOOP_LOG          = $LogPath
    $env:KENSHICOOP_SCENARIO     = $Scenario
    $env:KENSHICOOP_SETUP        = if ($IsHost) { $Setup } else { "" }
    if ($null -ne $JoinDurationSec) {
        $env:KENSHICOOP_SCENARIO_JOIN_DURATION_SEC = "$JoinDurationSec"
    } else {
        Remove-Item Env:\KENSHICOOP_SCENARIO_JOIN_DURATION_SEC -ErrorAction SilentlyContinue
    }
    # Apply the scenario's manifest DiagEnv hermetically (clears the managed
    # keyset first; $manifestEntry may be $null for an unmanifested scenario,
    # which degrades to the old clear-only behavior).
    $appliedDiag = Set-CoopDiagEnv -Entry $manifestEntry
    if ($appliedDiag.Count -gt 0 -and -not $script:diagEnvLogged) {
        Write-Host ("  DiagEnv (manifest): " + (($appliedDiag.Keys | Sort-Object { $_ } | ForEach-Object { "$_=$($appliedDiag[$_])" }) -join " "))
        $script:diagEnvLogged = $true
    }
    if ($null -ne $Inst.env) {
        foreach ($k in $Inst.env.PSObject.Properties.Name) {
            Set-Item -Path "env:$k" -Value "$($Inst.env.$k)"
        }
    }
}

function Wait-ForLogLine {
    param([string]$File, [string]$Pattern, [int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $File) {
            $hit = Select-String -Path $File -Pattern $Pattern -SimpleMatch:$false -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Start-Inst4 {
    param([string]$Exe, [string]$WorkDir)
    $out = & powershell -NoProfile -File (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

function Take-Shot4 {
    param([int]$ProcId, [string]$Out, [string]$Label)
    try {
        & powershell -NoProfile -File (Join-Path $scriptDir "screenshot.ps1") -ProcessId $ProcId -Out $Out -Frames $Frames -IntervalMs $FrameIntervalMs
        Write-Host "  captured $Label ($Frames frame(s)) -> $Out"
    } catch {
        Write-Warning "screenshot ($Label) failed: $($_.Exception.Message)"
    }
}

function Test-Alive4 { param([int]$ProcId) return ($ProcId -ne 0 -and $null -ne (Get-Process -Id $ProcId -ErrorAction SilentlyContinue)) }

# ---- Track every instance's launch/lifecycle state --------------------------
# label -> @{ inst=; log=; png=; pid=0; launched=$false; disconnected=$false }
$tracked = New-Object System.Collections.ArrayList
function New-TrackedInst {
    param($Inst, [string]$Label, [bool]$IsHost)
    $log = Join-Path $OutDir $(if ($Inst.logName) { $Inst.logName } else { "$Label.log" })
    $png = Join-Path $OutDir "$Label.png"
    return [pscustomobject]@{
        Inst = $Inst; Label = $Label; IsHost = $IsHost; Log = $log; Png = $png
        Pid = 0; Launched = $false; Disconnected = $false
    }
}

$hostT = New-TrackedInst -Inst $hostInst -Label "host" -IsHost $true
[void]$tracked.Add($hostT)
for ($i = 0; $i -lt $joinInsts.Count; $i++) {
    [void]$tracked.Add((New-TrackedInst -Inst $joinInsts[$i] -Label "join$($i + 1)" -IsHost $false))
}

# ---- run_meta.json: archive which instance(s) had a scheduled disconnect vs
# reconnect (disconnectAtSec/reconnectAtSec), keyed by the ACTUAL logName each
# instance writes to - the archivable, config-derived marker analyze_run4.ps1
# resolves its scheduled-disconnect exempt set from (clean_exit/health_*/
# result_* never require "SCENARIO RESULT" on that instance, since
# Stop-Process -Force structurally can't leave one behind), so an OFFLINE
# re-judge of this run dir works without the (gitignored) rig config itself.
# Never a hardcoded instance name (04-08-PLAN.md gap 3, locked user decision).
$scheduledDisconnect = New-Object System.Collections.ArrayList
$scheduledReconnect  = New-Object System.Collections.ArrayList
foreach ($t in $tracked) {
    $logName = Split-Path -Leaf $t.Log
    if ($null -ne $t.Inst.disconnectAtSec -and "$($t.Inst.disconnectAtSec)" -ne "") { [void]$scheduledDisconnect.Add($logName) }
    if ($null -ne $t.Inst.reconnectAtSec -and "$($t.Inst.reconnectAtSec)" -ne "") { [void]$scheduledReconnect.Add($logName) }
}
$metaDiagEnv = @{}
if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey('DiagEnv')) {
    foreach ($k in $manifestEntry.DiagEnv.Keys) { $metaDiagEnv[$k] = "$($manifestEntry.DiagEnv[$k])" }
}
# Phase 11 plan 02 (TEST-01): instanceCount + logNames are the offline-
# re-judge bridge - analyze_run4.ps1/analyze_wnpc_diff4.ps1 must judge an
# archived N<4 run without the gitignored rig config, so N travels in
# run_meta.json, not only in the rig file. Additive keys only (extend-never-
# break: scheduledDisconnect/scheduledReconnect/diagEnv are untouched).
$metaLogNames = @($tracked | ForEach-Object { Split-Path -Leaf $_.Log })
$runMeta = [pscustomobject]@{
    scheduledDisconnect = @($scheduledDisconnect)
    scheduledReconnect  = @($scheduledReconnect)
    diagEnv             = $metaDiagEnv
    instanceCount       = $tracked.Count
    logNames            = $metaLogNames
}
$runMeta | ConvertTo-Json | Set-Content -Path (Join-Path $OutDir "run_meta.json") -Encoding UTF8
Write-Host "  run_meta.json: scheduledDisconnect=[$($scheduledDisconnect -join ', ')] scheduledReconnect=[$($scheduledReconnect -join ', ')] instanceCount=$($tracked.Count) logNames=[$($metaLogNames -join ', ')]"

# ---- Launch HOST first -------------------------------------------------------
Write-Host "Launching HOST ($($hostT.Label)) ..."
Set-Inst4Env -Inst $hostInst -IsHost $true -LogPath $hostT.Log
$hostT.Pid = Start-Inst4 -Exe (Join-Path $hostInst.installDir "kenshi_x64.exe") -WorkDir $hostInst.installDir
if ($hostT.Pid -eq 0) { throw "Host failed to get past the launcher." }
$hostT.Launched = $true

Write-Host "Waiting for HOST to reach gameplay before launching JOINs (timeout ${StartTimeoutSec}s) ..."
$hostGameplay = Wait-ForLogLine -File $hostT.Log -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec
if ($hostGameplay) { Write-Host "Host in gameplay." } else { Write-Warning "Host not in gameplay after ${StartTimeoutSec}s; continuing anyway." }
$hostGameplayEpoch = Get-Date

# ---- Launch each JOIN in order, staggered; joins with reconnectAtSec are
# deferred to the lifecycle monitor loop below instead of launched now. -----
foreach ($t in ($tracked | Where-Object { -not $_.IsHost })) {
    $hasReconnect = ($null -ne $t.Inst.reconnectAtSec) -and ("$($t.Inst.reconnectAtSec)" -ne "")
    if ($hasReconnect) {
        Write-Host "  $($t.Label): deferring first launch to reconnectAtSec=$($t.Inst.reconnectAtSec)s (lifecycle monitor)."
        continue
    }
    Start-Sleep -Seconds $JoinDelaySec
    Write-Host "Launching $($t.Label) ..."
    Set-Inst4Env -Inst $t.Inst -IsHost $false -LogPath $t.Log
    $t.Pid = Start-Inst4 -Exe (Join-Path $t.Inst.installDir "kenshi_x64.exe") -WorkDir $t.Inst.installDir
    if ($t.Pid -eq 0) { Write-Warning "$($t.Label) failed to get past the launcher; continuing without it." }
    else { $t.Launched = $true }
}

Write-Host ("PIDs: " + (($tracked | ForEach-Object { "$($_.Label)=$($_.Pid)" }) -join "  "))

# ---- Arrange host+join1 side by side (arrange_windows.ps1 is a genuine
# 2-window primitive; join2/join3 are left at Kenshi's default spawn position
# - screenshot.ps1 captures by PID via PrintWindow regardless of overlap, so
# tiling is a visual nicety here, not a correctness requirement). -----------
if (-not $NoArrange -and $hostT.Pid -ne 0) {
    $join1T = $tracked | Where-Object { $_.Label -eq "join1" } | Select-Object -First 1
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Write-Host "Arranging host/join1 windows side by side ($ArrangeMonitor monitor) ..."
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-File", "`"$arrangeScript`"",
        "-HostPid", "$($hostT.Pid)", "-JoinPid", "$(if ($join1T) { $join1T.Pid } else { 0 })",
        "-Monitor", $ArrangeMonitor, "-TimeoutSec", "90", "-RepeatSec", "$ArrangeRepeatSec"
    ) | Out-Null
}

# ---- Disconnect / reconnect lifecycle monitor + screenshot capture ---------
# Runs until $Seconds have elapsed since host gameplay (or all tracked
# instances have exited), applying disconnectAtSec / reconnectAtSec offsets
# measured from $hostGameplayEpoch.
$shotsTaken = @{}
$deadline = $hostGameplayEpoch.AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    $elapsed = ((Get-Date) - $hostGameplayEpoch).TotalSeconds

    foreach ($t in $tracked) {
        # Deferred first launch (reconnectAtSec).
        if (-not $t.Launched -and $null -ne $t.Inst.reconnectAtSec -and "$($t.Inst.reconnectAtSec)" -ne "") {
            if ($elapsed -ge [double]$t.Inst.reconnectAtSec) {
                # 04-07 gap-closure (root cause 2, timing): a join deferred to
                # reconnectAtSec arms its OWN scenario clock well after the
                # host armed its - if it then ran the scenario's full default
                # JOIN_DURATION_MS (ScenarioMilestoneA.cpp), it would self-exit
                # tens of seconds AFTER the host already exited, and its
                # census would go permanently stale against a dead host
                # (04-VERIFICATION gap 1's false "divergence"). $elapsed here
                # (seconds since $hostGameplayEpoch, this loop's own stable
                # anchor) is the real host-relative offset THIS join is
                # launching at, so compute its remaining safe window instead
                # of blindly trusting the compiled-in default:
                #   hostScenarioSelfExitSec: matches THIS scenario's own
                #     compiled-in host self-exit duration (e.g.
                #     ScenarioMilestoneA.cpp's HOST_DURATION_MS=150000 - the
                #     host's own scenario self-exits 150s after ITS onStart
                #     arms) - same cross-reference local.rig.json's own
                #     "seconds" comment already relies on. Phase 10 plan 03
                #     gap fix: this used to be hardcoded to 150, silently
                #     mismatching any scenario with a LONGER host duration
                #     (e.g. save_load_gate's 320s, ScenarioSaveLoad.cpp) and
                #     self-exiting the host mid-Leg-L before join3's
                #     deferred launch could even complete its bootstrap
                #     (research Pitfall 9). Now reads the manifest's own
                #     HostSelfExitSec (a new manifest key) when the resolved
                #     scenario entry declares one, falling back to 150 -
                #     preserving every existing scenario's behavior
                #     byte-for-byte when the key is absent.
                #   armMarginSec: this join still needs to reach gameplay,
                #     connect, and arm (onStart) AFTER $elapsed is sampled -
                #     generous margin for that load+connect+arm overhead.
                #   floor 20s: always leave a meaningful scenario window
                #     rather than starving the join to near-zero.
                $hostScenarioSelfExitSec =
                    if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey("HostSelfExitSec")) {
                        [int]$manifestEntry.HostSelfExitSec
                    } else { 150 }
                $armMarginSec            = 20
                $joinRemainingSec        = [int]([Math]::Max(20, $hostScenarioSelfExitSec - $elapsed - $armMarginSec))
                Write-Host "  [lifecycle] launching $($t.Label) now (reconnectAtSec=$($t.Inst.reconnectAtSec)s reached, join scenario duration capped to ${joinRemainingSec}s so it exits at/before the host) ..."
                Set-Inst4Env -Inst $t.Inst -IsHost $false -LogPath $t.Log -JoinDurationSec $joinRemainingSec
                $t.Pid = Start-Inst4 -Exe (Join-Path $t.Inst.installDir "kenshi_x64.exe") -WorkDir $t.Inst.installDir
                if ($t.Pid -eq 0) { Write-Warning "$($t.Label) reconnect-launch failed to get past the launcher." }
                else { $t.Launched = $true }
            }
        }
        # Scheduled disconnect.
        if ($t.Launched -and -not $t.Disconnected -and $null -ne $t.Inst.disconnectAtSec -and "$($t.Inst.disconnectAtSec)" -ne "") {
            if ($elapsed -ge [double]$t.Inst.disconnectAtSec -and (Test-Alive4 $t.Pid)) {
                Write-Host "  [lifecycle] disconnecting $($t.Label) now (disconnectAtSec=$($t.Inst.disconnectAtSec)s reached) ..."
                try { Stop-Process -Id $t.Pid -Force -ErrorAction SilentlyContinue } catch {}
                $t.Disconnected = $true
            }
        }
        # Best-effort screenshot once, shortly before the run's shot-lead window.
        if (-not $shotsTaken.ContainsKey($t.Label) -and $t.Launched -and (Test-Alive4 $t.Pid)) {
            if ($elapsed -ge [Math]::Max(0, $Seconds - $ShotLeadSec)) {
                Take-Shot4 -ProcId $t.Pid -Out $t.Png -Label $t.Label
                $shotsTaken[$t.Label] = $true
            }
        }
    }

    if (-not ($tracked | Where-Object { $_.Launched -and -not $_.Disconnected -and (Test-Alive4 $_.Pid) })) { break }
    Start-Sleep -Seconds $LifecyclePollSec
}

# Anything never shot (exited early or run ended before its shot window) gets
# a best-effort final attempt.
foreach ($t in $tracked) {
    if (-not $shotsTaken.ContainsKey($t.Label) -and (Test-Alive4 $t.Pid)) {
        Take-Shot4 -ProcId $t.Pid -Out $t.Png -Label $t.Label
        $shotsTaken[$t.Label] = $true
    }
}

# ---- Wait for self-exit, with a hard-timeout kill so unattended runs never
# hang (mirrors run_test.ps1's per-PID kill-grace loop, generalized to N). --
# 08-04 gap fix (same class as the Seconds fix above): a scenario's own
# manifest KillGraceSec (e.g. world_state_gate's 250s, needed so a
# JOIN_DURATION_MS=225000 join can self-exit cleanly instead of being
# hard-killed mid-scenario) now raises the default floor, mirroring
# run_test.ps1's manifestEntry.KillGraceSec precedent (:537-538).
$killGrace = if ($KillGraceSec -ge 0) { $KillGraceSec } else { $ShotLeadSec + 60 }
if ($KillGraceSec -lt 0 -and $null -ne $manifestEntry -and $manifestEntry.ContainsKey("KillGraceSec")) {
    $killGrace = [Math]::Max($killGrace, [int]$manifestEntry.KillGraceSec)
}
$killDeadline = (Get-Date).AddSeconds($killGrace)
foreach ($t in $tracked) {
    if ($t.Pid -eq 0 -or $t.Disconnected) { continue }
    $remain = [int]([Math]::Max(1, ($killDeadline - (Get-Date)).TotalSeconds))
    try { Wait-Process -Id $t.Pid -Timeout $remain -ErrorAction Stop }
    catch {
        if (Test-Alive4 $t.Pid) {
            Write-Warning "$($t.Label) PID $($t.Pid) did not self-exit; killing."
            try { Stop-Process -Id $t.Pid -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
}

# ---- Archive each instance's own engine log next to ours (best-effort; a
# missing file must never fail a run - mirrors run_test.ps1). ----------------
foreach ($t in $tracked) {
    $src = Join-Path $t.Inst.installDir "kenshi_info.log"
    if (-not (Test-Path $src)) { continue }
    try { Copy-Item $src (Join-Path $OutDir "$($t.Label)_engine.log") -Force -ErrorAction Stop }
    catch { Write-Warning "could not archive $($t.Label) engine log: $($_.Exception.Message)" }
}

Write-Host ""
Write-Host "== Results =="
foreach ($t in $tracked) {
    Write-Host ("  {0}: log={1} png={2} disconnected={3}" -f $t.Label, $t.Log, $t.Png, $t.Disconnected)
}
Write-Host ""
Write-Host "run_test4.ps1 collected $($tracked.Count) per-instance log(s) in $OutDir."
Write-Host "(No pass/fail verdict is computed here - see scripts\analyze_run4.ps1.)"
