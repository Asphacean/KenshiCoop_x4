<#
.SYNOPSIS
  Gate wrapper for the world_state_gate N=4 oracle (Phase 8 plan 03,
  WORLD-01/02/03).

.DESCRIPTION
  Given -RunDir pointing at a real archived 4-log world_state_gate run
  (plan 04's job), calls scripts\analyze_run4.ps1 -Scenario world_state_gate
  against it and asserts RESULT: PASS - the live gate check.

  Given NO -RunDir (the normal case before a live gate run exists), this is a
  STRUCTURAL smoke check (mirrors InvConservationGate.Tests.ps1's own shape):
  (1) registration assertions - makeWorldStateScenario is chained in
  Scenario.cpp, declared in ScenarioSupport.h, ScenarioWorldState.cpp is
  listed in KenshiCoop.vcxproj, and world_state_gate is in scenarios.psd1
  with world_state as PrimaryGate + in NoSignalFails + carrying the
  KENSHICOOP_CELL_AUTH/KENSHICOOP_CELL_COLLAPSE DiagEnv; world_state is in
  Get-OracleRegistryN. (2) Oracle unit checks - synthetic 4-log fixtures
  (built from the SAME "SCENARIO WORLD <channel> ... t=<ms>"/
  "SCENARIO WORLD claimphase phase=<n> ..." line shapes ScenarioWorldState.cpp
  / CoopOraclesN.psm1's Test-WorldState actually emit/parse, wall-clock
  timestamped so the oracle's Convert-StampToMs/Get-LogClockOffsetMs window
  derivation exercises the real code path) proving the oracle:
    - PASSes a CLEAN fixture (every leg converges: fac final states agree,
      research known=1 on all 4 incl. the host, door/build/prod production
      crossing lines present, the rank2 writer's deed evidence lands owned=1
      with every non-writer's own '[deed] RECV ... ok=1' apply read-back
      present (08-05: deed is writer-scoped - the old every-rank pick raced
      the deed write itself under arming skew), contested-claim sequence
      0 -> 2 -> 2 (08-06 joins-move redesign: D1 = home cell, host party,
      owner=0; D2 = away cell after rank2+rank3 relocate, host NOT a party,
      lowest of {2,3} = 2; D3 = away cell after rank1 - the lowest playerId,
      the fresh-contest winner-to-be - arrives, continuity KEEPS owner=2),
      with each mover's own '[cell] CLAIM rank=<r> cell=<away>' arrival line
      present;
    - FAILs a DIVERGENCE fixture (one instance's final door state differs)
      naming the instance+channel;
    - FAILs a WRONG-WINNER fixture (D2's away cell resolves to owner=3
      instead of the locked lowest-playerId winner 2);
    - FAILs a CONTINUITY-REGRESSION fixture (D3's away owner flips to 1 when
      rank1 arrives - the fresh-contest-always/no-continuity model; the
      exact regression this scenario/oracle pair exists to catch);
    - FAILs a MOVER-ARRIVAL NO-SIGNAL fixture (join1 lacks its own
      '[cell] CLAIM rank=1 cell=<away>' line - D3's stays-2 verdict would be
      VACUOUS if the continuity contestant never arrived, so the gap FAILs
      by name);
    - FAILs a FLAPPING fixture (one instance's own claim series shows two
      different owners inside the same window, past the 5s settle
      tolerance);
    - FAILs a NO-SIGNAL fixture (one expected instance emits NO SCENARIO
      WORLD evidence for a channel while its peers do) - an evidentiary gap
      is a FAIL, never a silent pass;
    - FAILs a DEED-APPLY NO-SIGNAL fixture (08-05: one non-writer instance
      lacks its own '[deed] RECV ... ok=1' apply read-back for the writer's
      hand) naming the instance;
    - SKIPs a total no-signal fixture (NO instance emits any SCENARIO WORLD
      evidence at all) - the manifest's NoSignalFails then turns that SKIP
      into a verdict FAIL at the analyze_run4.ps1 level;
    - FAILs a ROUTING-REGRESSION fixture. NOTE (Claude's-discretion
      substitution, documented honestly): the plan's literal design asks for
      "a join log contains [fac] RECV with another JOIN's ownerId", but the
      production `[fac] RECV sid='%s' rel=%.1f was=%.1f ok=%d seq=%u` line
      (ReplicatorChannels.cpp) carries no ownerId field - a join legitimately
      logs the SAME "[fac] RECV" line when it receives the HOST's committed
      broadcast, so ownerId cannot be recovered from log text alone. This
      fixture instead exercises what Test-WorldState's routing check ACTUALLY
      asserts and CAN verify from available log grammar: the host's own log
      must show "[fac] RECV"/"[deed] RECV" (proof the join's intent reached
      the host under host-terminated PKT_FACTION/PKT_DEED routing) - omitting
      those lines from the host log FAILs Leg C. The stronger "no OTHER join
      ever sees a raw peer intent" property is exhaustively proven headless
      (nettest's host-terminated-intent + forged-owner legs, 08-01/08-02
      SUMMARY) - this leg is corroborating evidence from a live run, not the
      sole proof.

  Exit code = number of failed checks (0 = PASS), matching
  InvConservationGate.Tests.ps1's own convention so verify.ps1/regress.ps1
  can sum them.

.EXAMPLE
  # Self-test (no live run needed):
  powershell -ExecutionPolicy Bypass -File scripts\tests\WorldStateGate.Tests.ps1

.EXAMPLE
  # Judge a real archived 4-log run (plan 04):
  powershell -ExecutionPolicy Bypass -File scripts\tests\WorldStateGate.Tests.ps1 -RunDir tools\test-runs\20260905_120000_N4
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

# ---- tiny assert harness (verbatim shape from InvConservationGate.Tests.ps1) -
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
    # ---- Real 4-log world_state_gate run judgment (plan 04) -------------------
    Write-Host "== WorldStateGate: judging archived run dir =="
    Write-Host "  RunDir: $RunDir"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $RunDir -Scenario world_state_gate 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "  | $_" }
    $outText = ($output -join "`n")

    Check "analyze_run4.ps1 exited 0 against the archived run" ($exitCode -eq 0)
    Check "analyze_run4.ps1 reported RESULT: PASS" ($outText -match '(?m)^RESULT: PASS\s*$')
} else {
    # ---- Structural smoke check (no live run yet) ------------------------------
    Write-Host "== WorldStateGate: no -RunDir given; structural smoke check =="

    # 1. Scenario registration (chain/decl/vcxproj).
    $scenarioCpp = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\Scenario.cpp")
    $supportH    = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\ScenarioSupport.h")
    $vcxproj     = Get-Content -Raw (Join-Path $repoRoot "src\plugin\KenshiCoop.vcxproj")
    Check "makeWorldStateScenario chained in Scenario.cpp" ($scenarioCpp -match 'makeWorldStateScenario')
    Check "makeWorldStateScenario declared in ScenarioSupport.h" ($supportH -match 'makeWorldStateScenario')
    Check "ScenarioWorldState.cpp listed in KenshiCoop.vcxproj" ($vcxproj -match 'ScenarioWorldState\.cpp')

    # 2. Oracle registration.
    Check "Test-WorldState is exported" ($null -ne (Get-Command Test-WorldState -ErrorAction SilentlyContinue))
    $registry = Get-OracleRegistryN
    Check "world_state is in Get-OracleRegistryN" ($registry -contains 'world_state')

    # 3. Manifest entry.
    $manifest = Get-ScenarioManifest
    $entry = $manifest.Scenarios['world_state_gate']
    Check "scenarios.psd1 has a world_state_gate entry" ($null -ne $entry)
    if ($null -ne $entry) {
        Check "world_state_gate.PrimaryGate is world_state" ($entry.PrimaryGate -eq 'world_state')
        Check "world_state_gate.Gating includes world_state" (@($entry.Gating) -contains 'world_state')
        Check "world_state_gate.NoSignalFails includes world_state" (@($entry.NoSignalFails) -contains 'world_state')
        Check "world_state_gate.DiagEnv sets KENSHICOOP_CELL_AUTH=1" ("$($entry.DiagEnv['KENSHICOOP_CELL_AUTH'])" -eq '1')
        Check "world_state_gate.DiagEnv sets KENSHICOOP_CELL_COLLAPSE=0" ("$($entry.DiagEnv['KENSHICOOP_CELL_COLLAPSE'])" -eq '0')
    }

    # ---- Fixture builder -------------------------------------------------------
    # Wall-clock timestamped (Test-WorldState anchors every SCENARIO WORLD
    # pattern on "[HH:MM:SS.mmm]" via Convert-StampToMs, the Get-CellMap/
    # Test-SplitFar2 precedent) - unlike Test-InvConservation's bare-line
    # CONSERVE patterns, every line here needs a real stamp.
    #
    # Claimphase windows (host-authored, all times in the fixture's shared
    # 10:00:00-10:03:35 frame - no CLOCKSYNC lines, so every log's offset is 0
    # and timestamps are directly comparable; 08-06 windows open at
    # marker+30s):
    #   phase1 @ 10:00:10 -> D1 window = [10:00:40, 10:01:05] (25s, home)
    #   phase2 @ 10:01:10 -> D2 window = [10:01:40, 10:02:35] (55s, away+home)
    #   phase3 @ 10:02:40 -> D3 window = [10:03:10, 10:03:35] (25s, away+home)
    function New-WorldLog {
        param([string]$Path, [int]$Rank, [string[]]$ExtraLines = @())
        $isHost = if ($Rank -eq 0) { 1 } else { 0 }
        $lines = @(
            "[10:00:00.000] KenshiCoop: gameplay started",
            "[10:00:00.500] SCENARIO MAGATE start ownRank=$Rank host=$isHost"
        ) + $ExtraLines + @("[10:05:00.000] SCENARIO RESULT PASS")
        $lines | Set-Content -Path $Path -Encoding UTF8
    }

    # Channel evidence common to every instance (fac/research converge; claim
    # reports two SITES per the 08-06 joins-move redesign: the HOME cell
    # (5,7) stays owner=0 in all three windows, the AWAY cell (6,7) reads
    # owner=$D2Owner in D2 and $D3Owner in D3 - two samples per (window,
    # site) so the flap check sees stability by default). Door (08-04) is
    # placer-scoped like build/prod - see $hostBuildProdLines/
    # $join1CrossingLines below. Deed (08-05 gap closure) is WRITER-scoped:
    # only join2 (rank2) emits SCENARIO WORLD deed evidence, and every other
    # instance carries its own '[deed] RECV ... ok=1' apply line - see
    # $join2DeedLines/$deedRecvCrossLine below, not this per-instance block.
    function Get-BaseChannelLines {
        param([int]$D2Owner = 2, [int]$D3Owner = 2,
              [string[]]$ClaimExtra = @())
        $lines = @(
            "[10:00:15.000] SCENARIO WORLD fac sid='fac_a' rel=-75.0 t=15000",
            "[10:00:15.000] SCENARIO WORLD research sid='res_a' known=1 t=15000",
            "[10:00:16.000] SCENARIO WORLD claimcells home=5,7 away=6,7 ax=-48400 az=3000 t=16000",
            "[10:00:45.000] SCENARIO WORLD claim site=home cell=5,7 owner=0 t=45000",
            "[10:00:45.100] SCENARIO WORLD claim site=away cell=6,7 owner=0 t=45100",
            "[10:01:00.000] SCENARIO WORLD claim site=home cell=5,7 owner=0 t=60000",
            "[10:01:45.000] SCENARIO WORLD claim site=home cell=5,7 owner=0 t=105000",
            "[10:01:45.100] SCENARIO WORLD claim site=away cell=6,7 owner=$D2Owner t=105100",
            "[10:02:30.000] SCENARIO WORLD claim site=home cell=5,7 owner=0 t=150000",
            "[10:02:30.100] SCENARIO WORLD claim site=away cell=6,7 owner=$D2Owner t=150100",
            "[10:03:12.000] SCENARIO WORLD claim site=home cell=5,7 owner=0 t=192000",
            "[10:03:12.100] SCENARIO WORLD claim site=away cell=6,7 owner=$D3Owner t=192100",
            "[10:03:30.000] SCENARIO WORLD claim site=home cell=5,7 owner=0 t=210000",
            "[10:03:30.100] SCENARIO WORLD claim site=away cell=6,7 owner=$D3Owner t=210100"
        ) + $ClaimExtra
        return $lines
    }
    # Mover-arrival proofs (08-06): the production [cell] CLAIM line each
    # mover's claim publisher logs when its tab's cell changes to the away
    # cell - rank2/3 during D2, rank1 during D3.
    $moverClaimLines = @{
        join1 = "[10:02:58.000] [cell] CLAIM rank=1 cell=6,7 seq=2 dwell=3 pos=-48402,3002"
        join2 = "[10:01:28.000] [cell] CLAIM rank=2 cell=6,7 seq=2 dwell=3 pos=-48400,3000"
        join3 = "[10:01:29.000] [cell] CLAIM rank=3 cell=6,7 seq=2 dwell=3 pos=-48401,3001"
    }

    $hostPhaseLines = @(
        "[10:00:10.000] SCENARIO WORLD claimphase phase=1 ok=1 t=0",
        "[10:01:10.000] SCENARIO WORLD claimphase phase=2 ok=1 t=60000",
        "[10:02:40.000] SCENARIO WORLD claimphase phase=3 ok=1 t=150000"
    )
    # Door (08-04 live-gate fix): placer-scoped like build/prod - rank0
    # mints its own door-bearing shack, so only the host emits SCENARIO
    # WORLD door evidence, and the crossing proof is [bdoor] RECV (protocol
    # 28's translated-identity channel - a session-placed building's door
    # does NOT ride the generic [door] SEND/RECV channel; a live run
    # confirmed this) on a
    # non-placer log, mirroring [build]/[prod]'s own crossing lines below.
    $hostDoorLine = "[10:00:20.000] SCENARIO WORLD door hand=1.2.3.4.14 open=1 locked=0 t=20000"
    $join1DoorRecvLine = "[10:00:24.500] [bdoor] RECV key=1.2.3.4.14 idx=0 open=1 locked=0 was=0/0 ok=1 seq=1"
    $hostBuildProdLines = @(
        "[10:00:20.000] SCENARIO WORLD build key=1.2.3.4.11 prog=1.000 removed=0 t=20000",
        "[10:00:20.000] SCENARIO WORLD build key=1.2.3.4.12 prog=1.000 removed=1 t=20000",
        "[10:00:20.000] SCENARIO WORLD prod key=1.2.3.4.13 amt=2.500 t=20000"
    )
    $hostRoutingRecvLines = @(
        "[10:00:12.000] [fac] RECV sid='fac_a' rel=-75.0 was=0.0 ok=1 seq=1",
        "[10:00:12.500] [deed] RECV hand=1.2.3.4.10 owned=0->1 ok=1 seq=1"
    )
    # Deed (08-05 gap closure): writer-scoped. Only join2 (rank2) emits the
    # SCENARIO WORLD deed evidence; every NON-writer instance (host + the
    # other two joins) must carry its own '[deed] RECV hand=<writerHand>
    # owned=*->1 ok=1' apply read-back. The host's copy doubles as the Leg C
    # host-terminated routing proof (it already sits in
    # $hostRoutingRecvLines above, same hand).
    $join2DeedLines = @(
        "[10:00:15.000] SCENARIO WORLD deed hand=1.2.3.4.10 owned=1 t=15000",
        "[10:00:11.900] [deed] SEND hand=1.2.3.4.10 owned=1 owner='fac_pl' seq=1"
    )
    $deedRecvCrossLine = "[10:00:13.000] [deed] RECV hand=1.2.3.4.10 owned=0->1 ok=1 seq=1"
    $join1CrossingLines = @(
        "[10:00:22.000] [build] MINT key=1.2.3.4.11 sid='shack01' ui=0 rc=1",
        "[10:00:34.000] [build] STATE-RECV key=1.2.3.4.11 prog=1.000 complete=1",
        "[10:00:44.000] [build] REMOVE-RECV key=1.2.3.4.12 ok=1",
        "[10:00:24.000] [prod] RECV key=1.2.3.4.13 kind=1 pwr=0->0 out=0.000->2.500 in0=0.000->0.000 ok=1 seq=1"
    )

    # Build a full 4-log fixture.
    #   -D2Owner/-D3Owner: contested-claim leg owner value on ALL 4 logs
    #     (WRONG-WINNER / CONTINUITY-REGRESSION)
    #   -FlapLabel: one label gets a SECOND, DIFFERENT-owner D1 sample past
    #     the 5s settle tolerance (FLAPPING)
    #   -OmitChannelLabel/-OmitChannel: drop ONE channel's evidence for ONE
    #     label only (partial NO-SIGNAL)
    #   -OmitAllLabel: drop every SCENARIO WORLD line for one label (broader
    #     no-signal variant)
    #   -DropHostRoutingRecv: omit the host's own [fac]/[deed] RECV lines
    #     (ROUTING-REGRESSION - see the file header's documented substitution)
    #   -OmitHostDoor: drop the host's SCENARIO WORLD door evidence
    #     (placer evidence-gap NO-SIGNAL, 08-04)
    #   -OmitDoorRecv: drop join1's [bdoor] RECV crossing line (door-never-
    #     crossed NO-SIGNAL, 08-04)
    #   -OmitDeedRecvLabel: drop ONE non-writer instance's [deed] RECV apply
    #     line (deed-never-applied-there NO-SIGNAL, 08-05)
    #   -OmitMoverClaimLabel: drop ONE mover's production [cell] CLAIM
    #     arrival line (MOVER-ARRIVAL NO-SIGNAL, 08-06 - join1's makes D3's
    #     stays-2 verdict vacuous)
    function New-WorldFixture {
        param(
            [string]$Tmp,
            [int]$D2Owner = 2,
            [int]$D3Owner = 2,
            [string]$FlapLabel = "",
            [string]$OmitChannelLabel = "",
            [string]$OmitChannel = "",
            [string]$OmitAllLabel = "",
            [bool]$DropHostRoutingRecv = $false,
            [bool]$OmitHostDoor = $false,
            [bool]$OmitDoorRecv = $false,
            [string]$OmitDeedRecvLabel = "",
            [string]$OmitMoverClaimLabel = ""
        )
        $labels = @("host", "join1", "join2", "join3")
        $ranks = @{ host = 0; join1 = 1; join2 = 2; join3 = 3 }
        $extraByLabel = @{}
        foreach ($label in $labels) {
            $flapExtra = @()
            if ($label -eq $FlapLabel) {
                # A second D1 home sample, well past the 5s settle-tolerance
                # boundary (window start 10:00:40 + 5s = 10:00:45), reporting
                # a DIFFERENT owner than the other D1 samples.
                $flapExtra = @("[10:00:58.000] SCENARIO WORLD claim site=home cell=5,7 owner=3 t=58000")
            }
            $lines = @(Get-BaseChannelLines -D2Owner $D2Owner -D3Owner $D3Owner -ClaimExtra $flapExtra)
            if ($label -eq $OmitChannelLabel -and $OmitChannel -ne "") {
                $lines = @($lines | Where-Object { $_ -notmatch "SCENARIO WORLD $OmitChannel " })
            }
            if ($moverClaimLines.ContainsKey($label) -and $label -ne $OmitMoverClaimLabel) {
                $lines = @($lines) + @($moverClaimLines[$label])
            }
            $extraByLabel[$label] = $lines
        }
        $extraByLabel["host"] = @($extraByLabel["host"]) + @($hostPhaseLines) + @($hostBuildProdLines)
        if (-not $OmitHostDoor) { $extraByLabel["host"] = @($extraByLabel["host"]) + @($hostDoorLine) }
        if (-not $DropHostRoutingRecv) { $extraByLabel["host"] = @($extraByLabel["host"]) + @($hostRoutingRecvLines) }
        $extraByLabel["join1"] = @($extraByLabel["join1"]) + @($join1CrossingLines)
        if (-not $OmitDoorRecv) { $extraByLabel["join1"] = @($extraByLabel["join1"]) + @($join1DoorRecvLine) }
        # Deed (08-05): writer evidence on join2 (rank2); the non-writer JOINS
        # get their own [deed] RECV apply line (the host's rides
        # $hostRoutingRecvLines above). -OmitDeedRecvLabel drops one.
        $extraByLabel["join2"] = @($extraByLabel["join2"]) + @($join2DeedLines)
        foreach ($dl in @("join1", "join3")) {
            if ($dl -ne $OmitDeedRecvLabel) {
                $extraByLabel[$dl] = @($extraByLabel[$dl]) + @($deedRecvCrossLine)
            }
        }

        if ($OmitAllLabel -ne "") { $extraByLabel[$OmitAllLabel] = @() }

        $result = [ordered]@{}
        foreach ($label in $labels) {
            $path = Join-Path $Tmp "$label.log"
            New-WorldLog -Path $path -Rank $ranks[$label] -ExtraLines $extraByLabel[$label]
            $result[$label] = $path
        }
        return $result
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_worldstate_selftest_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # 4. CLEAN fixture -> PASS, sequence 0 -> 2 -> 2 (08-06 joins-move
        #    redesign: D1 home host-party=0, D2 away lowest-of-{2,3}=2, D3
        #    away continuity-keeps-2 despite rank1 arriving).
        $logsClean = New-WorldFixture -Tmp $tmp
        Reset-GateResults
        $stClean = Invoke-OneOracleN -Id 'world_state' -Logs $logsClean
        $gClean = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "CLEAN fixture PASSes" ($stClean -eq 'PASS')
        Check "CLEAN fixture reports the 0->2->2 claim sequence" ($gClean.metrics.claimSequence -eq '0->2->2')

        # 5. DIVERGENCE fixture (08-04: door is now placer-scoped like
        #    build/prod - see the file header's documented substitution):
        #    the host never emits SCENARIO WORLD door evidence at all ->
        #    Leg A FAIL (placer evidence gap), naming the gap.
        $logsDivergence = New-WorldFixture -Tmp $tmp -OmitHostDoor $true
        Reset-GateResults
        $stDivergence = Invoke-OneOracleN -Id 'world_state' -Logs $logsDivergence
        $gDivergence = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "DIVERGENCE fixture (host emits no door evidence) FAILs" ($stDivergence -eq 'FAIL')
        Check "DIVERGENCE finding names the placer evidence gap" ($gDivergence.detail -match 'placer' -and $gDivergence.detail -match 'door')

        # 6. WRONG-WINNER fixture: D2's away cell resolves to owner=3 instead
        #    of the locked lowest-playerId winner (2) -> Leg D FAIL.
        #    (D3Owner rides along at 3: the D3 window would otherwise "prove
        #    continuity" of a winner D2 already failed - one wrong reduce,
        #    two named findings, both about the same regression.)
        $logsWrongWinner = New-WorldFixture -Tmp $tmp -D2Owner 3 -D3Owner 3
        Reset-GateResults
        $stWrongWinner = Invoke-OneOracleN -Id 'world_state' -Logs $logsWrongWinner
        $gWrongWinner = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "WRONG-WINNER fixture (D2 away owner=3) FAILs" ($stWrongWinner -eq 'FAIL')
        Check "WRONG-WINNER finding names D2 and the mismatch" ($gWrongWinner.detail -match 'D2' -and $gWrongWinner.detail -match 'expects owner=2')

        # 7. CONTINUITY-REGRESSION fixture: D3's away owner flips to 1 when
        #    rank1 (the lowest playerId, the fresh-contest winner-to-be)
        #    arrives - the fresh-contest-always/no-continuity model, the
        #    exact regression this scenario/oracle pair exists to catch
        #    -> Leg D FAIL.
        $logsContinuityRegression = New-WorldFixture -Tmp $tmp -D3Owner 1
        Reset-GateResults
        $stContinuityRegression = Invoke-OneOracleN -Id 'world_state' -Logs $logsContinuityRegression
        $gContinuityRegression = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "CONTINUITY-REGRESSION fixture (D3 away owner flips to the arriving rank1) FAILs" ($stContinuityRegression -eq 'FAIL')
        Check "CONTINUITY-REGRESSION finding names D3 and the mismatch" ($gContinuityRegression.detail -match 'D3' -and $gContinuityRegression.detail -match 'expects owner=2')

        # 7b. MOVER-ARRIVAL NO-SIGNAL fixture (08-06): the away owner reads a
        #     clean 2 in D3, but join1's own '[cell] CLAIM rank=1 cell=6,7'
        #     arrival line is missing - the continuity contest never
        #     verifiably armed, so the stays-2 verdict would be vacuous ->
        #     Leg D FAIL naming join1.
        $logsNoArrival = New-WorldFixture -Tmp $tmp -OmitMoverClaimLabel "join1"
        Reset-GateResults
        $stNoArrival = Invoke-OneOracleN -Id 'world_state' -Logs $logsNoArrival
        $gNoArrival = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "MOVER-ARRIVAL NO-SIGNAL fixture (join1 never claims the away cell) FAILs" ($stNoArrival -eq 'FAIL')
        Check "MOVER-ARRIVAL finding names join1 and the unarmed continuity contest" ($gNoArrival.detail -match 'join1' -and $gNoArrival.detail -match 'continuity contest never armed')

        # 8. FLAPPING fixture: join2's OWN D1 claim series shows two DIFFERENT
        #    owners past the 5s settle tolerance -> Leg D FAIL.
        $logsFlapping = New-WorldFixture -Tmp $tmp -FlapLabel "join2"
        Reset-GateResults
        $stFlapping = Invoke-OneOracleN -Id 'world_state' -Logs $logsFlapping
        $gFlapping = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "FLAPPING fixture (D1 owner flaps past settle tolerance) FAILs" ($stFlapping -eq 'FAIL')
        Check "FLAPPING finding names D1 and the flapping instance" ($gFlapping.detail -match 'D1' -and $gFlapping.detail -match 'flapping' -and $gFlapping.detail -match 'join2')

        # 9. NO-SIGNAL fixture (08-04: door is placer-scoped now): the host
        #    DOES emit door evidence, but no non-placer log shows the
        #    [bdoor] RECV crossing line -> Leg A FAIL (never crossed), never
        #    a silent pass on placer-only evidence.
        $logsNoSignal = New-WorldFixture -Tmp $tmp -OmitDoorRecv $true
        Reset-GateResults
        $stNoSignal = Invoke-OneOracleN -Id 'world_state' -Logs $logsNoSignal
        $gNoSignal = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "NO-SIGNAL fixture (door never crosses) FAILs, not a silent pass" ($stNoSignal -eq 'FAIL')
        Check "NO-SIGNAL finding names the door crossing gap" ($gNoSignal.detail -match 'door' -and $gNoSignal.detail -match 'never crossed')

        # 10. Total no-signal: NO instance emits ANY SCENARIO WORLD evidence
        #     -> SKIP (the manifest's NoSignalFails - already proven
        #     registered in check 3 above - is what turns this into a
        #     verdict FAIL at the analyze_run4.ps1 level; the oracle itself
        #     reports the honest "nothing to judge" state, never a false
        #     PASS).
        New-WorldLog -Path (Join-Path $tmp "host.log")  -Rank 0
        New-WorldLog -Path (Join-Path $tmp "join1.log") -Rank 1
        New-WorldLog -Path (Join-Path $tmp "join2.log") -Rank 2
        New-WorldLog -Path (Join-Path $tmp "join3.log") -Rank 3
        $logsTotalNoSignal = [ordered]@{
            host  = (Join-Path $tmp "host.log")
            join1 = (Join-Path $tmp "join1.log")
            join2 = (Join-Path $tmp "join2.log")
            join3 = (Join-Path $tmp "join3.log")
        }
        Reset-GateResults
        $stTotalNoSignal = Invoke-OneOracleN -Id 'world_state' -Logs $logsTotalNoSignal
        Check "total no-signal (0 of 4 instances report) SKIPs (well-formed, no signal)" ($stTotalNoSignal -eq 'SKIP')

        # 11. ROUTING-REGRESSION fixture: the host's own log shows NO
        #     [fac]/[deed] RECV - the join(rank1/rank2)'s intent never
        #     reached the host under host-terminated routing -> Leg C FAIL.
        #     (See the file header for why this substitutes the plan's
        #     literal ownerId-based design, which the production log grammar
        #     cannot express.)
        $logsRoutingRegression = New-WorldFixture -Tmp $tmp -DropHostRoutingRecv $true
        Reset-GateResults
        $stRoutingRegression = Invoke-OneOracleN -Id 'world_state' -Logs $logsRoutingRegression
        $gRoutingRegression = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "ROUTING-REGRESSION fixture (host never RECVs the join intent) FAILs" ($stRoutingRegression -eq 'FAIL')
        Check "ROUTING-REGRESSION finding names the missing host-terminated intent" ($gRoutingRegression.detail -match 'never reached the host')

        # 12. DEED-APPLY NO-SIGNAL fixture (08-05: deed is writer-scoped): the
        #     rank2 writer emits its deed evidence, host/join1 carry their
        #     apply lines, but join3's own '[deed] RECV ... ok=1' apply
        #     read-back is missing -> Leg C FAIL naming join3, never a silent
        #     pass on writer-only evidence.
        $logsDeedNoSignal = New-WorldFixture -Tmp $tmp -OmitDeedRecvLabel "join3"
        Reset-GateResults
        $stDeedNoSignal = Invoke-OneOracleN -Id 'world_state' -Logs $logsDeedNoSignal
        $gDeedNoSignal = Get-GateResults | Where-Object { $_.gate -eq 'world_state' } | Select-Object -Last 1
        Check "DEED-APPLY NO-SIGNAL fixture (join3 never applies the writer's deed) FAILs" ($stDeedNoSignal -eq 'FAIL')
        Check "DEED-APPLY finding names join3 and the missing apply" ($gDeedNoSignal.detail -match 'join3' -and $gDeedNoSignal.detail -match 'never crossed/applied on join3')
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("WorldStateGate: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
