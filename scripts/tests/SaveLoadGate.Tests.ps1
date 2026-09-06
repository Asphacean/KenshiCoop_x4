<#
.SYNOPSIS
  Gate wrapper for the save_load_gate N=4 oracle (Phase 10 plan 03,
  SAVE-01..04).

.DESCRIPTION
  Given -RunDir pointing at a real archived 4-log save_load_gate run (plan
  04's job), calls scripts\analyze_run4.ps1 -Scenario save_load_gate against
  it and asserts RESULT: PASS - the live gate check.

  Given NO -RunDir (the normal case before a live gate run exists), this is a
  STRUCTURAL smoke check (ConsensusGate.Tests.ps1/WorldStateGate.Tests.ps1's
  own shape): (1) registration assertions - makeSaveLoadScenario is chained
  in Scenario.cpp, declared in ScenarioSupport.h, ScenarioSaveLoad.cpp is
  listed in KenshiCoop.vcxproj, and save_load_gate is in scenarios.psd1 with
  save_load as PrimaryGate + in NoSignalFails + a HostSelfExitSec key;
  save_load is in Get-OracleRegistryN. (2) Oracle unit checks - synthetic
  4-log fixtures (built from the SAME "SCENARIO SAVELOAD ..." / production
  "[boot]/[save]/[coord]/[load] ..." line shapes ScenarioSaveLoad.cpp /
  CoopOraclesN.psm1's Test-SaveLoad actually emit/parse, wall-clock
  timestamped so the oracle's Convert-StampToMs/Get-LogClockOffsetMs
  alignment exercises the real code path) proving the oracle:
    - PASSes an all-agree fixture (Leg L: targeted bootstrap + zero-reset
      survivors + continuous heartbeat + join3 convergence with no rank-3-
      driver movement before connect; Leg S: three distinct committed
      owners + no drop; Leg R: matched REJECT/REJECTED (both ids) + an
      accepted try=2 retry; Leg H: one broadcast GO + three per-client
      loaded ACKs + three WORLD-RELOADs);
    - FAILs a BLOCKED-FOREVER fixture (one client, owner=3, never reaches
      committed or dropped in Leg S - the bounded-outcome violation);
    - FAILs a ONE-ACK-IMPLIES-ALL fixture (only ONE owner ever reaches
      committed - the last-of-N-wins collapse);
    - FAILs a DOUBLE-TRANSITION fixture (a second [save] XFER-BEGIN opens
      mid-Leg-S with no [coord] REJECT explaining the overlap);
    - FAILs a RELOAD-STORM fixture (an established join shows
      [load] WORLD-RELOAD inside the late-join window - a global reset);
    - FAILs a JOINER-DIVERGED fixture (join3's bootstrap shows neither a
      MATCH load nor a NACK+XFER-COMMIT - it never converged);
    - FAILs a HEARTBEAT-GAP fixture (a survivor's census heartbeat goes
      silent for longer than the tolerance inside the join window, with NO
      reset lines - must fail, not pass-by-absence);
    - SKIPs a MISSING-LEG-MARKER fixture (SAVELOAD evidence exists but no
      leg=<L|S|R|H> marker at all - no window is judgeable) - the
      manifest's NoSignalFails then turns that SKIP into a verdict FAIL at
      the analyze_run4.ps1 level;
    - SKIPs a TOTAL no-signal fixture (NO instance emits any SCENARIO
      SAVELOAD evidence at all).

  Exit code = number of failed checks (0 = PASS), matching
  ConsensusGate.Tests.ps1/WorldStateGate.Tests.ps1's own convention so
  verify.ps1/regress.ps1 can sum them.

.EXAMPLE
  # Self-test (no live run needed):
  powershell -ExecutionPolicy Bypass -File scripts\tests\SaveLoadGate.Tests.ps1

.EXAMPLE
  # Judge a real archived 4-log run (plan 04):
  powershell -ExecutionPolicy Bypass -File scripts\tests\SaveLoadGate.Tests.ps1 -RunDir tools\test-runs\20260906_120000_N4
#>
[CmdletBinding()]
param(
    [string]$RunDir = ""
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                        # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                      # repo root

Import-Module (Join-Path $scriptsRoot "CoopOraclesN.psm1") -Force

# ---- tiny assert harness (verbatim shape from ConsensusGate.Tests.ps1) ------
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

$analyzeScript = Join-Path $scriptsRoot "analyze_run4.ps1"
if (-not (Test-Path $analyzeScript)) {
    Write-Host "  FAIL analyze_run4.ps1 not found at $analyzeScript"
    exit 1
}

if ($RunDir -ne "") {
    # ---- Real 4-log save_load_gate run judgment (plan 04) ---------------------
    Write-Host "== SaveLoadGate: judging archived run dir =="
    Write-Host "  RunDir: $RunDir"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $RunDir -Scenario save_load_gate 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "  | $_" }
    $outText = ($output -join "`n")

    Check "analyze_run4.ps1 exited 0 against the archived run" ($exitCode -eq 0)
    Check "analyze_run4.ps1 reported RESULT: PASS" ($outText -match '(?m)^RESULT: PASS\s*$')
} else {
    # ---- Structural smoke check (no live run yet) ------------------------------
    Write-Host "== SaveLoadGate: no -RunDir given; structural smoke check =="

    # 1. Scenario registration (chain/decl/vcxproj).
    $scenarioCpp = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\Scenario.cpp")
    $supportH    = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\ScenarioSupport.h")
    $vcxproj     = Get-Content -Raw (Join-Path $repoRoot "src\plugin\KenshiCoop.vcxproj")
    Check "makeSaveLoadScenario chained in Scenario.cpp" ($scenarioCpp -match 'makeSaveLoadScenario')
    Check "makeSaveLoadScenario declared in ScenarioSupport.h" ($supportH -match 'makeSaveLoadScenario')
    Check "ScenarioSaveLoad.cpp listed in KenshiCoop.vcxproj" ($vcxproj -match 'ScenarioSaveLoad\.cpp')

    # 2. Oracle registration.
    Check "Test-SaveLoad is exported" ($null -ne (Get-Command Test-SaveLoad -ErrorAction SilentlyContinue))
    $registry = Get-OracleRegistryN
    Check "save_load is in Get-OracleRegistryN" ($registry -contains 'save_load')

    # 3. Manifest entry.
    $manifest = Get-ScenarioManifest
    $entry = $manifest.Scenarios['save_load_gate']
    Check "scenarios.psd1 has a save_load_gate entry" ($null -ne $entry)
    if ($null -ne $entry) {
        Check "save_load_gate.PrimaryGate is save_load" ($entry.PrimaryGate -eq 'save_load')
        Check "save_load_gate.Gating includes save_load" (@($entry.Gating) -contains 'save_load')
        Check "save_load_gate.NoSignalFails includes save_load" (@($entry.NoSignalFails) -contains 'save_load')
        Check "save_load_gate.Tier is none (N=4-only)" ($entry.Tier -eq 'none')
        Check "save_load_gate declares HostSelfExitSec" ($entry.ContainsKey('HostSelfExitSec'))
    }

    # 4. PROTOCOL_VERSION guard - this plan (Phase 10 Plan 03) adds no wire
    # change; PROTOCOL_VERSION stays at Plan 01's locked value (61).
    $wireH = Get-Content -Raw (Join-Path $repoRoot "src\netproto\Wire.h")
    Check "PROTOCOL_VERSION is still 61 (no wire change in Plan 03)" ($wireH -match 'PROTOCOL_VERSION = 61;')

    # 5. run_test4.ps1 reads the deferred-launch cap from the manifest.
    $rigPs1 = Get-Content -Raw (Join-Path $scriptsRoot "run_test4.ps1")
    Check "run_test4.ps1's deferred-launch cap reads HostSelfExitSec" ($rigPs1 -match 'HostSelfExitSec')

    # ---- Fixture builder -------------------------------------------------------
    # Wall-clock timestamped (Test-SaveLoad anchors every pattern on
    # "[HH:MM:SS.mmm]" via Convert-StampToMs, the Test-Consensus/Test-
    # WorldState precedent) - one shared frame across all four logs (offsets
    # in whole/fractional SECONDS from a fixed midnight-safe base), no
    # CLOCKSYNC lines so every log's offset is 0 and timestamps are directly
    # comparable. join3's own first line lands LATE in this shared frame
    # (t=30s) - a genuine late-join, not a t=0 arm like the three
    # establisheds.
    $T0 = Get-Date -Date "2026-01-01 10:00:00.000"
    function Stamp {
        param([double]$OffsetSec)
        return ($T0.AddSeconds($OffsetSec)).ToString('HH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture)
    }

    function New-SaveLoadLog {
        param([string]$Path, [int]$Rank, [double]$StartSec, [string[]]$Lines, [double]$EndSec)
        $isHost = if ($Rank -eq 0) { 1 } else { 0 }
        $out = New-Object System.Collections.ArrayList
        [void]$out.Add("[$(Stamp $StartSec)] KenshiCoop: gameplay started")
        [void]$out.Add("[$(Stamp $StartSec)] SCENARIO MAGATE start ownRank=$Rank host=$isHost")
        [void]$out.AddRange($Lines)
        [void]$out.Add("[$(Stamp $EndSec)] SCENARIO RESULT PASS")
        $out | Set-Content -Path $Path -Encoding UTF8
    }

    # Continuous census heartbeat (host/join1/join2 across the join window,
    # 3s cadence - well inside the oracle's 5000ms gap tolerance). r3pos is
    # STATIC pre-connect (nobody driving rank 3's squad yet) unless
    # -Jitter3seenAfter forces a jump at/after a given offset (used by no
    # fixture here, kept for completeness/clarity of the static-by-default
    # contract) - the HEARTBEAT-GAP fixture instead THINS this series via
    # -DropRange.
    function Get-CensusLines {
        param([int]$Rank, [double]$FromSec, [double]$ToSec, [double]$StepSec = 3.0,
              [double[]]$DropRange = @(), [double]$MoveR3AfterSec = -1)
        $lines = New-Object System.Collections.ArrayList
        $t = $FromSec
        while ($t -le $ToSec) {
            $skip = ($DropRange.Count -eq 2 -and $t -gt $DropRange[0] -and $t -lt $DropRange[1])
            if (-not $skip) {
                $r3x = 500.0
                if ($MoveR3AfterSec -ge 0 -and $t -ge $MoveR3AfterSec) { $r3x = 500.0 + ($t - $MoveR3AfterSec) * 3.0 }
                $r3xStr = $r3x.ToString('F1', [System.Globalization.CultureInfo]::InvariantCulture)
                [void]$lines.Add("[$(Stamp $t)] SCENARIO SAVELOAD census rank=$Rank haveOwn=1 pos=100.0,0.0,100.0 " +
                    "r3seen=1 r3pos=$r3xStr,0.0,500.0 t=$([int]($t * 1000))")
            }
            $t += $StepSec
        }
        return @($lines)
    }

    # Build a full 4-log fixture. Each switch injects ONE defect (see each
    # locked FAIL fixture's own doc line above) from an otherwise all-agree
    # baseline; PASS is every switch left off.
    function New-SaveLoadFixture {
        param(
            [string]$Tmp,
            [bool]$BlockedForeverOwner3 = $false,
            [bool]$OneAckOnly = $false,
            [bool]$DoubleTransition = $false,
            [bool]$ReloadStormJoin1 = $false,
            [bool]$JoinerDiverged = $false,
            [bool]$HeartbeatGapJoin2 = $false,
            [bool]$MissingLegMarkers = $false,
            [bool]$TotalNoSignal = $false,
            [bool]$LcDroppedSettle = $false
        )
        $labels = @("host", "join1", "join2", "join3")
        $ranks = @{ host = 0; join1 = 1; join2 = 2; join3 = 3 }
        $linesByLabel = @{ host = New-Object System.Collections.ArrayList
                            join1 = New-Object System.Collections.ArrayList
                            join2 = New-Object System.Collections.ArrayList
                            join3 = New-Object System.Collections.ArrayList }

        if (-not $TotalNoSignal) {
            # IN-02 fixture (Phase 10 REVIEW; Phase 11 Plan 01): an
            # LC_DROPPED-settle boot-kind transition - a [boot] GO->join with
            # a matching [load] DROP and NO RESYNC at all (dest=98, distinct
            # from join3's dest=3 below, so it never touches the Leg L
            # assertions). Pre-fix, the settle loop matched ONLY the RESYNC
            # marker, so this transition would still read "active" until its
            # 60s placeholder (t=0.2s + 60000ms ~= 60.2s) - well past join3's
            # own boot push at t=30s, producing a FALSE double-transition
            # finding with no [coord] REJECT to explain the "overlap" (two
            # entirely unrelated boot pushes to different dests). Post-fix,
            # the DROP at t=0.8s settles it almost immediately, so no
            # overlap is ever computed and the fixture stays PASS.
            if ($LcDroppedSettle) {
                [void]$linesByLabel.host.Add("[$(Stamp 0.2)] [boot] GO->join id=90 dest=98 name='lcdrop_test' fp=deadbeef (push on connect)")
                [void]$linesByLabel.host.Add("[$(Stamp 0.8)] [load] DROP owner=98 loadId=90 retries=1")
            }

            # ---- Leg L (t=0..120s host clock; wall-clock offsets identical) ----
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 0.5)] SCENARIO SAVELOAD leg=L phase=begin t=0")
            }

            # Establisheds' pre-connect heartbeat (static r3pos - nobody driving
            # rank 3 yet).
            foreach ($lbl in @("host", "join1", "join2")) {
                $dropRange = if ($HeartbeatGapJoin2 -and $lbl -eq "join2") { @(10.0, 100.0) } else { @() }
                [void]$linesByLabel[$lbl].AddRange(@(Get-CensusLines -Rank $ranks[$lbl] -FromSec 1 -ToSec 29 -DropRange $dropRange))
            }

            # join3 connects at t=30s (a genuine late launch - not present
            # before this point in the shared wall-clock frame).
            [void]$linesByLabel.host.Add("[$(Stamp 30.0)] [boot] GO->join id=1 dest=3 name='svloadS_boot' fp=deadbeef (push on connect)")
            [void]$linesByLabel.join3.Add("[$(Stamp 31.0)] SCENARIO SAVELOAD joined rank=3 t=1000")
            if (-not $JoinerDiverged) {
                [void]$linesByLabel.join3.Add("[$(Stamp 32.0)] [load] GO id=1 name='svloadS_boot' fp=deadbeef MATCH -> loading")
            }
            [void]$linesByLabel.host.Add("[$(Stamp 33.0)] [boot] RESYNC dest=3")
            [void]$linesByLabel.host.Add("[$(Stamp 34.0)] [load] CLIENT owner=3 loadId=1 state=loaded")

            if ($ReloadStormJoin1) {
                [void]$linesByLabel.join1.Add("[$(Stamp 50.0)] [load] WORLD-RELOAD swapMs=400 hookTicksDuringSwap=3")
            }

            # Post-connect heartbeat continues to legSBeginT=125s.
            foreach ($lbl in @("host", "join1", "join2")) {
                # HEARTBEAT-GAP: keep a couple of samples at the edges (>=2
                # total, so this exercises the max-gap check itself rather
                # than the separate too-few-samples guard) and drop the
                # entire middle, producing one gap far past the tolerance.
                $dropRange = if ($HeartbeatGapJoin2 -and $lbl -eq "join2") { @(40.0, 120.0) } else { @() }
                [void]$linesByLabel[$lbl].AddRange(@(Get-CensusLines -Rank $ranks[$lbl] -FromSec 35 -ToSec 124 -DropRange $dropRange))
            }

            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 120.0)] SCENARIO SAVELOAD leg=L phase=end t=120000")
            }

            # ---- Leg S (t=125..145s) --------------------------------------------
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 125.0)] SCENARIO SAVELOAD leg=S phase=begin t=125000")
            }
            [void]$linesByLabel.host.Add("[$(Stamp 130.0)] SCENARIO SAVELOAD savehost name='svloadS' ok=1 try=1 t=130000")
            [void]$linesByLabel.host.Add("[$(Stamp 132.0)] [save] XFER-BEGIN id=10 name='svloadS' files=5 bytes=1000 dest=4294967295")
            if ($DoubleTransition) {
                # A ROGUE second transition opens mid-Leg-S, before id=10
                # settles (its committed rows land at t=140-142) - no
                # [coord] REJECT anywhere near it, so no overlap is explained.
                [void]$linesByLabel.host.Add("[$(Stamp 138.0)] [save] XFER-BEGIN id=99 name='rogue' files=1 bytes=10 dest=4294967295")
            }
            # ONE-ACK-IMPLIES-ALL: only owner=1 ever reaches committed (owner=1
            # always lands - it is the one ACK the last-of-N-wins regression
            # would mistake for group commit).
            [void]$linesByLabel.host.Add("[$(Stamp 140.0)] [save] CLIENT owner=1 xferId=10 state=committed")
            if (-not $OneAckOnly) {
                [void]$linesByLabel.host.Add("[$(Stamp 141.0)] [save] CLIENT owner=2 xferId=10 state=committed")
            }
            # BLOCKED-FOREVER: owner=3 never reaches committed OR dropped -
            # the host neither settles nor gives up on it.
            if (-not $OneAckOnly -and -not $BlockedForeverOwner3) {
                [void]$linesByLabel.host.Add("[$(Stamp 142.0)] [save] CLIENT owner=3 xferId=10 state=committed")
            }
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 145.0)] SCENARIO SAVELOAD leg=S phase=end t=145000")
            }

            # ---- Leg R (t=150..180s) --------------------------------------------
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 150.0)] SCENARIO SAVELOAD leg=R phase=begin t=150000")
            }
            [void]$linesByLabel.join1.Add("[$(Stamp 152.0)] SCENARIO SAVELOAD reqsave rank=1 name='svloadR1' ok=1 try=1 t=152000")
            [void]$linesByLabel.join2.Add("[$(Stamp 152.0)] SCENARIO SAVELOAD reqsave rank=2 name='svloadR2' ok=1 try=1 t=152000")
            [void]$linesByLabel.host.Add("[$(Stamp 153.0)] [coord] REJECT requester=2 reqId=1 reason=busy active=1/1 kind=save")
            [void]$linesByLabel.join2.Add("[$(Stamp 154.0)] [coord] REJECTED reqId=1 kind=save active=1/1")
            [void]$linesByLabel.host.Add("[$(Stamp 155.0)] [save] XFER-BEGIN id=11 name='svloadR1' files=5 bytes=1000 dest=4294967295")
            [void]$linesByLabel.host.Add("[$(Stamp 163.0)] [save] CLIENT owner=1 xferId=11 state=committed")
            [void]$linesByLabel.host.Add("[$(Stamp 164.0)] [save] CLIENT owner=2 xferId=11 state=committed")
            [void]$linesByLabel.host.Add("[$(Stamp 165.0)] [save] CLIENT owner=3 xferId=11 state=committed")
            [void]$linesByLabel.join2.Add("[$(Stamp 175.0)] SCENARIO SAVELOAD reqsave rank=2 name='svloadR2' ok=1 try=2 t=175000")
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 180.0)] SCENARIO SAVELOAD leg=R phase=end t=180000")
            }

            # ---- Leg H (t=185..210s) --------------------------------------------
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 185.0)] SCENARIO SAVELOAD leg=H phase=begin t=185000")
            }
            [void]$linesByLabel.host.Add("[$(Stamp 187.0)] SCENARIO SAVELOAD loadhost name='svloadS' ok=1 t=187000")
            [void]$linesByLabel.host.Add("[$(Stamp 188.0)] [load] GO->join id=600 name='svloadS' fp=cafebabe")
            [void]$linesByLabel.host.Add("[$(Stamp 200.0)] [load] CLIENT owner=1 loadId=600 state=loaded")
            [void]$linesByLabel.host.Add("[$(Stamp 201.0)] [load] CLIENT owner=2 loadId=600 state=loaded")
            [void]$linesByLabel.host.Add("[$(Stamp 202.0)] [load] CLIENT owner=3 loadId=600 state=loaded")
            [void]$linesByLabel.join1.Add("[$(Stamp 199.0)] [load] WORLD-RELOAD swapMs=500 hookTicksDuringSwap=3")
            [void]$linesByLabel.join2.Add("[$(Stamp 199.5)] [load] WORLD-RELOAD swapMs=500 hookTicksDuringSwap=3")
            [void]$linesByLabel.join3.Add("[$(Stamp 200.0)] [load] WORLD-RELOAD swapMs=500 hookTicksDuringSwap=3")
            if (-not $MissingLegMarkers) {
                [void]$linesByLabel.host.Add("[$(Stamp 210.0)] SCENARIO SAVELOAD leg=H phase=end t=210000")
            }
        }

        $result = [ordered]@{}
        foreach ($label in $labels) {
            $path = Join-Path $Tmp "$label.log"
            $startSec = if ($label -eq "join3") { 30.0 } else { 0.0 }
            New-SaveLoadLog -Path $path -Rank $ranks[$label] -StartSec $startSec -Lines $linesByLabel[$label] -EndSec 215.0
            $result[$label] = $path
        }
        return $result
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_saveload_selftest_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # run_meta.json (scheduledReconnect naming join3.log) - Test-
        # SaveLoad's bonus RunDir cross-check exercised via -RunDir $tmp.
        $runMeta = [pscustomobject]@{ scheduledDisconnect = @(); scheduledReconnect = @("join3.log", "n2_join1.log", "n3_join2.log") }
        $runMeta | ConvertTo-Json | Set-Content -Path (Join-Path $tmp "run_meta.json") -Encoding UTF8

        # 6. all-agree fixture -> PASS.
        $logsClean = New-SaveLoadFixture -Tmp $tmp
        Reset-GateResults
        $stClean = Invoke-OneOracleN -Id 'save_load' -Logs $logsClean -RunDir $tmp
        $gClean = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "all-agree fixture PASSes" ($stClean -eq 'PASS')
        if ($stClean -ne 'PASS') { Write-Host "      detail: $($gClean.detail)" }
        Check "all-agree fixture reports 3 committed Leg S owners" ($gClean.metrics.legSCommittedOwners -eq 3)

        # 7. BLOCKED-FOREVER fixture.
        $logsBlocked = New-SaveLoadFixture -Tmp $tmp -BlockedForeverOwner3 $true
        Reset-GateResults
        $stBlocked = Invoke-OneOracleN -Id 'save_load' -Logs $logsBlocked -RunDir $tmp
        $gBlocked = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "BLOCKED-FOREVER fixture (owner=3 never committed/dropped) FAILs" ($stBlocked -eq 'FAIL')
        Check "BLOCKED-FOREVER finding cites the distinct-owner count" ($gBlocked.detail -match 'distinct owner')

        # 8. ONE-ACK-IMPLIES-ALL fixture.
        $logsOneAck = New-SaveLoadFixture -Tmp $tmp -OneAckOnly $true
        Reset-GateResults
        $stOneAck = Invoke-OneOracleN -Id 'save_load' -Logs $logsOneAck -RunDir $tmp
        $gOneAck = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "ONE-ACK-IMPLIES-ALL fixture (only owner=1 committed) FAILs" ($stOneAck -eq 'FAIL')
        Check "ONE-ACK-IMPLIES-ALL fixture reports exactly 1 committed owner" ($gOneAck.metrics.legSCommittedOwners -eq 1)

        # 9. DOUBLE-TRANSITION fixture.
        $logsDouble = New-SaveLoadFixture -Tmp $tmp -DoubleTransition $true
        Reset-GateResults
        $stDouble = Invoke-OneOracleN -Id 'save_load' -Logs $logsDouble -RunDir $tmp
        $gDouble = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "DOUBLE-TRANSITION fixture (an unexplained overlapping XFER-BEGIN) FAILs" ($stDouble -eq 'FAIL')
        Check "DOUBLE-TRANSITION finding names the serialization defect" ($gDouble.detail -match 'double-transition')

        # 10. RELOAD-STORM fixture.
        $logsReload = New-SaveLoadFixture -Tmp $tmp -ReloadStormJoin1 $true
        Reset-GateResults
        $stReload = Invoke-OneOracleN -Id 'save_load' -Logs $logsReload -RunDir $tmp
        $gReload = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "RELOAD-STORM fixture (join1 WORLD-RELOAD inside the join window) FAILs" ($stReload -eq 'FAIL')
        Check "RELOAD-STORM finding names the global-reset regression" ($gReload.detail -match 'reloaded \(global reset\)')

        # 11. JOINER-DIVERGED fixture.
        $logsDiverged = New-SaveLoadFixture -Tmp $tmp -JoinerDiverged $true
        Reset-GateResults
        $stDiverged = Invoke-OneOracleN -Id 'save_load' -Logs $logsDiverged -RunDir $tmp
        $gDiverged = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "JOINER-DIVERGED fixture (no MATCH, no NACK+COMMIT) FAILs" ($stDiverged -eq 'FAIL')
        Check "JOINER-DIVERGED finding names the never-converged defect" ($gDiverged.detail -match 'never converged')

        # 12. HEARTBEAT-GAP fixture (the truncated-log guard).
        $logsGap = New-SaveLoadFixture -Tmp $tmp -HeartbeatGapJoin2 $true
        Reset-GateResults
        $stGap = Invoke-OneOracleN -Id 'save_load' -Logs $logsGap -RunDir $tmp
        $gGap = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "HEARTBEAT-GAP fixture (join2's census goes silent) FAILs, not pass-by-absence" ($stGap -eq 'FAIL')
        Check "HEARTBEAT-GAP finding names the silent-log gap" ($gGap.detail -match 'may have gone silent')

        # 13. MISSING-LEG-MARKER fixture -> SKIP (NoSignalFails escalates to
        #     FAIL at the analyze_run4.ps1 level - already proven registered
        #     in check 3 above).
        $logsNoMark = New-SaveLoadFixture -Tmp $tmp -MissingLegMarkers $true
        Reset-GateResults
        $stNoMark = Invoke-OneOracleN -Id 'save_load' -Logs $logsNoMark -RunDir $tmp
        Check "MISSING-LEG-MARKER fixture (evidence present, no leg= marker) SKIPs" ($stNoMark -eq 'SKIP')

        # 14. TOTAL no-signal: NO instance emits ANY SCENARIO SAVELOAD
        #     evidence -> SKIP.
        $logsTotalNoSignal = New-SaveLoadFixture -Tmp $tmp -TotalNoSignal $true
        Reset-GateResults
        $stTotalNoSignal = Invoke-OneOracleN -Id 'save_load' -Logs $logsTotalNoSignal -RunDir $tmp
        Check "total no-signal (0 of 4 instances report) SKIPs (well-formed, no signal)" ($stTotalNoSignal -eq 'SKIP')

        # 15. IN-02 fixture: an LC_DROPPED-settle boot-kind transition (a
        #     [boot] GO->join with a matching [load] DROP and no RESYNC)
        #     must settle at the DROP time, not the 60s placeholder - proven
        #     by the ABSENCE of a false double-transition finding against
        #     join3's own unrelated boot push 30s later in the same fixture.
        $logsLcDropped = New-SaveLoadFixture -Tmp $tmp -LcDroppedSettle $true
        Reset-GateResults
        $stLcDropped = Invoke-OneOracleN -Id 'save_load' -Logs $logsLcDropped -RunDir $tmp
        $gLcDropped = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "LC_DROPPED-settle fixture (DROP, no RESYNC) still PASSes" ($stLcDropped -eq 'PASS')
        if ($stLcDropped -ne 'PASS') { Write-Host "      detail: $($gLcDropped.detail)" }
        Check "LC_DROPPED-settle fixture reports no false double-transition finding" ($null -eq $gLcDropped.detail -or $gLcDropped.detail -notmatch 'double-transition')

        # ---- Phase 11 plan 02 (TEST-01): N=2/N=3 offline fixtures ------------
        # Prove honest SKIP semantics BEFORE any live spend: at N=2 Leg R's
        # rank-vs-rank contest is structurally impossible (one join) and the
        # scenario's own legskip marker must produce an honest SKIP (never a
        # silent/vacuous PASS, never a FAIL); at N=3 there are two
        # established joins and Leg R runs exactly like N=4's rank-1/rank-2
        # contest. The LAST declared instance is the deferred/late one,
        # mirroring the committed local.rig.n2/n3.example.json shape.
        function New-SaveLoadFixtureN {
            param([int]$N, [string]$Tmp, [bool]$ForceLegRSkip = $false)
            $labels = if ($N -eq 2) { @("host", "join1") } else { @("host", "join1", "join2") }
            $ranks  = @{ host = 0; join1 = 1; join2 = 2 }
            $lateLabel = $labels[$labels.Count - 1]
            $lateRank  = $ranks[$lateLabel]
            $linesByLabel = @{}
            foreach ($l in $labels) { $linesByLabel[$l] = New-Object System.Collections.ArrayList }

            # ---- Leg L (t=0..40s): targeted bootstrap of the late instance ----
            [void]$linesByLabel.host.Add("[$(Stamp 0.5)] SCENARIO SAVELOAD leg=L phase=begin t=0")
            foreach ($lbl in ($labels | Where-Object { $_ -ne $lateLabel })) {
                [void]$linesByLabel[$lbl].AddRange(@(Get-CensusLines -Rank $ranks[$lbl] -FromSec 1 -ToSec 9))
            }
            [void]$linesByLabel.host.Add("[$(Stamp 10.0)] [boot] GO->join id=1 dest=$lateRank name='svloadS_boot' fp=deadbeef (push on connect)")
            [void]$linesByLabel[$lateLabel].Add("[$(Stamp 11.0)] SCENARIO SAVELOAD joined rank=$lateRank t=1000")
            [void]$linesByLabel[$lateLabel].Add("[$(Stamp 12.0)] [load] GO id=1 name='svloadS_boot' fp=deadbeef MATCH -> loading")
            [void]$linesByLabel.host.Add("[$(Stamp 13.0)] [boot] RESYNC dest=$lateRank")
            [void]$linesByLabel.host.Add("[$(Stamp 14.0)] [load] CLIENT owner=$lateRank loadId=1 state=loaded")
            foreach ($lbl in $labels) {
                [void]$linesByLabel[$lbl].AddRange(@(Get-CensusLines -Rank $ranks[$lbl] -FromSec 15 -ToSec 39))
            }
            [void]$linesByLabel.host.Add("[$(Stamp 40.0)] SCENARIO SAVELOAD leg=L phase=end t=40000")

            # ---- Leg S (t=45..60s): every connected join commits --------------
            [void]$linesByLabel.host.Add("[$(Stamp 45.0)] SCENARIO SAVELOAD leg=S phase=begin t=45000")
            [void]$linesByLabel.host.Add("[$(Stamp 50.0)] SCENARIO SAVELOAD savehost name='svloadS' ok=1 try=1 t=50000")
            [void]$linesByLabel.host.Add("[$(Stamp 51.0)] [save] XFER-BEGIN id=10 name='svloadS' files=5 bytes=1000 dest=4294967295")
            $t = 55.0
            foreach ($lbl in ($labels | Where-Object { $_ -ne "host" })) {
                [void]$linesByLabel.host.Add("[$(Stamp $t)] [save] CLIENT owner=$($ranks[$lbl]) xferId=10 state=committed")
                $t += 1.0
            }
            [void]$linesByLabel.host.Add("[$(Stamp 60.0)] SCENARIO SAVELOAD leg=S phase=end t=60000")

            # ---- Leg R (t=65..95s): legskip at N<3, real contest at N>=3 ------
            [void]$linesByLabel.host.Add("[$(Stamp 65.0)] SCENARIO SAVELOAD leg=R phase=begin t=65000")
            if (($N -lt 3) -or $ForceLegRSkip) {
                [void]$linesByLabel.join1.Add("[$(Stamp 66.0)] SCENARIO SAVELOAD legskip leg=R n=$N")
            }
            if ($N -ge 3) {
                [void]$linesByLabel.join1.Add("[$(Stamp 66.5)] SCENARIO SAVELOAD reqsave rank=1 name='svloadR1' ok=1 try=1 t=66500")
                [void]$linesByLabel.join2.Add("[$(Stamp 66.5)] SCENARIO SAVELOAD reqsave rank=2 name='svloadR2' ok=1 try=1 t=66500")
                [void]$linesByLabel.host.Add("[$(Stamp 67.0)] [coord] REJECT requester=2 reqId=1 reason=busy active=1/1 kind=save")
                [void]$linesByLabel.join2.Add("[$(Stamp 68.0)] [coord] REJECTED reqId=1 kind=save active=1/1")
                [void]$linesByLabel.host.Add("[$(Stamp 69.0)] [save] XFER-BEGIN id=11 name='svloadR1' files=5 bytes=1000 dest=4294967295")
                [void]$linesByLabel.host.Add("[$(Stamp 75.0)] [save] CLIENT owner=1 xferId=11 state=committed")
                [void]$linesByLabel.host.Add("[$(Stamp 76.0)] [save] CLIENT owner=2 xferId=11 state=committed")
                [void]$linesByLabel.join2.Add("[$(Stamp 85.0)] SCENARIO SAVELOAD reqsave rank=2 name='svloadR2' ok=1 try=2 t=85000")
            }
            [void]$linesByLabel.host.Add("[$(Stamp 95.0)] SCENARIO SAVELOAD leg=R phase=end t=95000")

            # ---- Leg H (t=100..130s): every connected join's load ACK ---------
            [void]$linesByLabel.host.Add("[$(Stamp 100.0)] SCENARIO SAVELOAD leg=H phase=begin t=100000")
            [void]$linesByLabel.host.Add("[$(Stamp 102.0)] SCENARIO SAVELOAD loadhost name='svloadS' ok=1 t=102000")
            [void]$linesByLabel.host.Add("[$(Stamp 103.0)] [load] GO->join id=600 name='svloadS' fp=cafebabe")
            $t = 110.0
            foreach ($lbl in ($labels | Where-Object { $_ -ne "host" })) {
                [void]$linesByLabel.host.Add("[$(Stamp $t)] [load] CLIENT owner=$($ranks[$lbl]) loadId=600 state=loaded")
                [void]$linesByLabel[$lbl].Add("[$(Stamp ($t - 0.5))] [load] WORLD-RELOAD swapMs=500 hookTicksDuringSwap=3")
                $t += 1.0
            }
            [void]$linesByLabel.host.Add("[$(Stamp 130.0)] SCENARIO SAVELOAD leg=H phase=end t=130000")

            $result = [ordered]@{}
            foreach ($label in $labels) {
                $path = Join-Path $Tmp "n${N}_$label.log"
                $startSec = if ($label -eq $lateLabel) { 10.0 } else { 0.0 }
                New-SaveLoadLog -Path $path -Rank $ranks[$label] -StartSec $startSec -Lines $linesByLabel[$label] -EndSec 135.0
                $result[$label] = $path
            }
            return $result
        }

        # 16. N=2 fixture: Leg R is structurally impossible (one join) -
        #     honest, named SKIP (metrics.legRSkippedN), never a silent pass.
        $logsN2 = New-SaveLoadFixtureN -N 2 -Tmp $tmp
        Reset-GateResults
        $stN2 = Invoke-OneOracleN -Id 'save_load' -Logs $logsN2 -RunDir $tmp
        $gN2 = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "N=2 fixture (legskip leg=R) PASSes overall" ($stN2 -eq 'PASS')
        if ($stN2 -ne 'PASS') { Write-Host "      detail: $($gN2.detail)" }
        Check "N=2 fixture reports the honest Leg R legskip (n=2)" ($gN2.metrics.legRSkippedN -eq 2)

        # 17. N=3 fixture: two established joins - Leg R runs the SAME
        #     reject/rejected/retry contest N=4 exercises, no skip.
        $logsN3 = New-SaveLoadFixtureN -N 3 -Tmp $tmp
        Reset-GateResults
        $stN3 = Invoke-OneOracleN -Id 'save_load' -Logs $logsN3 -RunDir $tmp
        $gN3 = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "N=3 fixture (real Leg R contest) PASSes overall" ($stN3 -eq 'PASS')
        if ($stN3 -ne 'PASS') { Write-Host "      detail: $($gN3.detail)" }
        Check "N=3 fixture's Leg R actually ran (legRLoser metric present, not skipped)" ($gN3.metrics.legRLoser -eq 2)

        # 18. Negative control: a legskip marker at N=3 is NOT honored (never
        #     a silent pass on a marker that fired when it should not have -
        #     "skip ONLY when the marker is present AND N<3").
        $logsN3BadSkip = New-SaveLoadFixtureN -N 3 -Tmp $tmp -ForceLegRSkip $true
        Reset-GateResults
        $stN3BadSkip = Invoke-OneOracleN -Id 'save_load' -Logs $logsN3BadSkip -RunDir $tmp
        $gN3BadSkip = Get-GateResults | Where-Object { $_.gate -eq 'save_load' } | Select-Object -Last 1
        Check "N=3 fixture with an erroneous legskip marker FAILs (never a silent pass)" ($stN3BadSkip -eq 'FAIL')
        Check "erroneous-legskip finding names the N>=3 mismatch" ($gN3BadSkip.detail -match 'should have run, not skipped')
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("SaveLoadGate: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
