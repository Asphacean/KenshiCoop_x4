<#
.SYNOPSIS
  Gate wrapper for the consensus_gate N=4 oracle (Phase 9 plan 03,
  CONS-01/02/03).

.DESCRIPTION
  Given -RunDir pointing at a real archived 4-log consensus_gate run (plan
  04's job), calls scripts\analyze_run4.ps1 -Scenario consensus_gate against
  it and asserts RESULT: PASS - the live gate check.

  Given NO -RunDir (the normal case before a live gate run exists), this is a
  STRUCTURAL smoke check (mirrors WorldStateGate.Tests.ps1/
  InvConservationGate.Tests.ps1's own shape): (1) registration assertions -
  makeConsensusScenario is chained in Scenario.cpp, declared in
  ScenarioSupport.h, ScenarioConsensus.cpp is listed in KenshiCoop.vcxproj,
  and consensus_gate is in scenarios.psd1 with consensus as PrimaryGate + in
  NoSignalFails; consensus is in Get-OracleRegistryN. (2) Oracle unit checks -
  synthetic 4-log fixtures (built from the SAME "SCENARIO CONSENSUS ..." /
  production "[wallet]"/"[speed]"/"[leave]"/"SCENARIO POOL/SPEED/GTIME" line
  shapes ScenarioConsensus.cpp / CoopOraclesN.psm1's Test-Consensus actually
  emit/parse, wall-clock timestamped so the oracle's Convert-StampToMs/
  Get-LogClockOffsetMs alignment exercises the real code path) proving the
  oracle:
    - PASSes a CLEAN fixture (Leg A: money conserves to one authoritative
      total, the overdraft reject verdict is identical on all four instances
      and the rejected buyer's own pool series shows the refund; Leg B: the
      denied-raise / all-raise / pause-unpause / combat-cap / held-vote
      sequence is observed and the survivors' SCENARIO SPEED series raises
      to 3x shortly after the host's own disconnect detection; Leg C: the
      four GTIME series stay converged at both the mid-run checkpoint and
      the final tail);
    - FAILs a ONE-INSTANCE-MISSING-REJECT fixture (one of the four logs
      lacks the REJECT verdict the other three carry);
    - FAILs a MINTED-VALUE fixture (both overdraft deltas fold - the second
      fold reports a NEGATIVE resulting pool, an overdraft that folded when
      it should have rejected);
    - FAILs a DOUBLE-FOLD fixture (the same (owner,seq) is folded twice on
      the host);
    - FAILs a REPLAY fixture (a REJECTED (owner,seq) is later also folded -
      a committed verdict re-awarded);
    - FAILs a WRONG-MIN fixture (the SET line reports effective=1x even
      though every voter's VOTES entry reads 3x - effective is not the true
      min);
    - FAILs a DENIED-RAISE-GRANTED fixture (WR-04: the host's 3x click is
      wrongly granted - a SET line reports effective=3x while every voter
      still holds 1x - the min-rule consistency invariant catches the
      direction the old vacuous denied-raise check never could);
    - FAILs a VOTE-NOT-DROPPED fixture (no survivor's SCENARIO SPEED series
      ever raises back to 3x after the host's own disconnect detection -
      the stale vote never actually gets dropped);
    - FAILs a CLOCK-DIVERGES fixture (one instance's GTIME series drifts
      more than the tolerance from the others in the final tail window);
    - SKIPs a total no-signal fixture (NO instance emits any SCENARIO
      CONSENSUS evidence at all) - the manifest's NoSignalFails then turns
      that SKIP into a verdict FAIL at the analyze_run4.ps1 level.

  Exit code = number of failed checks (0 = PASS), matching
  WorldStateGate.Tests.ps1/InvConservationGate.Tests.ps1's own convention so
  verify.ps1/regress.ps1 can sum them.

.EXAMPLE
  # Self-test (no live run needed):
  powershell -ExecutionPolicy Bypass -File scripts\tests\ConsensusGate.Tests.ps1

.EXAMPLE
  # Judge a real archived 4-log run (plan 04):
  powershell -ExecutionPolicy Bypass -File scripts\tests\ConsensusGate.Tests.ps1 -RunDir tools\test-runs\20260906_120000_N4
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

# ---- tiny assert harness (verbatim shape from WorldStateGate.Tests.ps1) -----
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
    # ---- Real 4-log consensus_gate run judgment (plan 04) ---------------------
    Write-Host "== ConsensusGate: judging archived run dir =="
    Write-Host "  RunDir: $RunDir"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $RunDir -Scenario consensus_gate 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "  | $_" }
    $outText = ($output -join "`n")

    Check "analyze_run4.ps1 exited 0 against the archived run" ($exitCode -eq 0)
    Check "analyze_run4.ps1 reported RESULT: PASS" ($outText -match '(?m)^RESULT: PASS\s*$')
} else {
    # ---- Structural smoke check (no live run yet) ------------------------------
    Write-Host "== ConsensusGate: no -RunDir given; structural smoke check =="

    # 1. Scenario registration (chain/decl/vcxproj).
    $scenarioCpp = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\Scenario.cpp")
    $supportH    = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\ScenarioSupport.h")
    $vcxproj     = Get-Content -Raw (Join-Path $repoRoot "src\plugin\KenshiCoop.vcxproj")
    Check "makeConsensusScenario chained in Scenario.cpp" ($scenarioCpp -match 'makeConsensusScenario')
    Check "makeConsensusScenario declared in ScenarioSupport.h" ($supportH -match 'makeConsensusScenario')
    Check "ScenarioConsensus.cpp listed in KenshiCoop.vcxproj" ($vcxproj -match 'ScenarioConsensus\.cpp')

    # 2. Oracle registration.
    Check "Test-Consensus is exported" ($null -ne (Get-Command Test-Consensus -ErrorAction SilentlyContinue))
    $registry = Get-OracleRegistryN
    Check "consensus is in Get-OracleRegistryN" ($registry -contains 'consensus')

    # 3. Manifest entry.
    $manifest = Get-ScenarioManifest
    $entry = $manifest.Scenarios['consensus_gate']
    Check "scenarios.psd1 has a consensus_gate entry" ($null -ne $entry)
    if ($null -ne $entry) {
        Check "consensus_gate.PrimaryGate is consensus" ($entry.PrimaryGate -eq 'consensus')
        Check "consensus_gate.Gating includes consensus" (@($entry.Gating) -contains 'consensus')
        Check "consensus_gate.NoSignalFails includes consensus" (@($entry.NoSignalFails) -contains 'consensus')
        Check "consensus_gate.Tier is none (N=4-only)" ($entry.Tier -eq 'none')
    }

    # 4. PROTOCOL_VERSION guard - this plan (Phase 9 Plan 03) itself adds no
    # wire change. Phase 10 Plan 01 (SAVE-01/02/03) legitimately bumped
    # PROTOCOL_VERSION 60 -> 61 (the phase's own one-time locked wire change,
    # PKT_LOAD_ACK/PKT_COORD_REJECT) - this guard is updated to match that
    # CURRENT value rather than asserting a stale pre-Phase-10 constant, so
    # it keeps testing "no wire drift since the last deliberate bump" instead
    # of permanently failing on a bump this test predates.
    $wireH = Get-Content -Raw (Join-Path $repoRoot "src\netproto\Wire.h")
    Check "PROTOCOL_VERSION is still 61 (no wire change in Plan 03/Phase 10 Plan 02)" ($wireH -match 'PROTOCOL_VERSION = 61;')

    # ---- Fixture builder -------------------------------------------------------
    # Wall-clock timestamped (Test-Consensus anchors every pattern on
    # "[HH:MM:SS.mmm]" via Convert-StampToMs, the Test-WorldState precedent) -
    # a single shared 10:00:00-based frame across all four logs, no CLOCKSYNC
    # lines so every offset is 0 and timestamps are directly comparable.
    #
    # Leg A ledger (matches ScenarioConsensus.cpp's own design constants,
    # documented there - NOT read by the oracle, which derives everything
    # from these lines): BASE=10000; solvent spends rank1=150/rank2=200/
    # rank3=250 (sum 600, pool->9400); overdraft contest rank2=5200 (folds -
    # solvent against 9400) / rank3=5100 (rejected - overdraws the reduced
    # 4200 pool). expectedFinal = 10000-600-5200 = 4200.
    function New-ConsensusLog {
        param([string]$Path, [int]$Rank, [string[]]$ExtraLines = @())
        $isHost = if ($Rank -eq 0) { 1 } else { 0 }
        $lines = @(
            "[10:00:00.000] KenshiCoop: gameplay started",
            "[10:00:00.500] SCENARIO MAGATE start ownRank=$Rank host=$isHost"
        ) + $ExtraLines + @("[10:05:00.000] SCENARIO RESULT PASS")
        $lines | Set-Content -Path $Path -Encoding UTF8
    }

    # Host-only Leg A/B evidence: legmarks, moneyseed, the fold ledger, the
    # speed SET/VOTES sequence, and the [leave] disconnect line.
    #   -SecondFoldOwner3/-SecondFoldTotal: MINTED-VALUE - fold BOTH overdraft
    #     deltas instead of rejecting the second (negative resulting total).
    #   -DoubleFoldSolvent1: DOUBLE-FOLD - repeat rank1's solvent fold line.
    #   -ReplayReject3: REPLAY - also fold the (owner=3,seq=2) delta that was
    #     already rejected.
    #   -WrongMinAllRaised: WRONG-MIN - the "all raised" SET line reports
    #     eff=1.00 despite every voter reading 3.00.
    #   -OmitPostLeaveRecompute: VOTE-NOT-DROPPED - never emit the post-leave
    #     SET line that raises effective back to 3x.
    #   -GrantDeniedRaise: DENIED-RAISE-GRANTED (WR-04) - instead of the
    #     "[speed] DENY" line, the host's 3x click is wrongly GRANTED while
    #     every voter still reads 1.00 (a changed effective, so production
    #     WOULD log this SET line if the min rule broke this way) - the
    #     min-rule consistency invariant must FAIL it.
    function Get-HostLegLines {
        param(
            [bool]$SecondFoldOwner3 = $false,
            [bool]$DoubleFoldSolvent1 = $false,
            [bool]$ReplayReject3 = $false,
            [bool]$WrongMinAllRaised = $false,
            [bool]$OmitPostLeaveRecompute = $false,
            [bool]$GrantDeniedRaise = $false
        )
        $lines = New-Object System.Collections.ArrayList
        [void]$lines.Add("[10:00:00.500] SCENARIO CONSENSUS legmark leg=A phase=1 t=0")
        [void]$lines.Add("[10:00:08.000] SCENARIO CONSENSUS moneyseed base=10000 ok=1 before=8000 after=10000 t=8000")
        # Fold ledger: three solvent folds (owner=1/2/3 seq=1), then the
        # overdraft contest (owner=2 seq=2 folds; owner=3 seq=2 rejected -
        # unless a fixture variant corrupts this).
        [void]$lines.Add("[10:00:22.100] [wallet] POOL FOLD owner=1 seq=1 delta=-150 t=9850")
        if ($DoubleFoldSolvent1) {
            [void]$lines.Add("[10:00:22.150] [wallet] POOL FOLD owner=1 seq=1 delta=-150 t=9700")
        }
        [void]$lines.Add("[10:00:22.600] [wallet] POOL FOLD owner=2 seq=1 delta=-200 t=9650")
        [void]$lines.Add("[10:00:23.100] [wallet] POOL FOLD owner=3 seq=1 delta=-250 t=9400")
        [void]$lines.Add("[10:00:55.600] [wallet] POOL FOLD owner=2 seq=2 delta=-5200 t=4200")
        if ($SecondFoldOwner3) {
            # MINTED-VALUE: the second overdraft delta ALSO folds (should
            # have rejected) - the resulting total goes negative.
            [void]$lines.Add("[10:00:55.700] [wallet] POOL FOLD owner=3 seq=2 delta=-5100 t=-900")
        } else {
            [void]$lines.Add("[10:00:55.700] [wallet] REJECT owner=3 seq=2 delta=-5100 pool=4200")
            if ($ReplayReject3) {
                # REPLAY: the already-rejected (owner=3,seq=2) is later ALSO
                # folded - a committed verdict re-awarded.
                [void]$lines.Add("[10:00:58.000] [wallet] POOL FOLD owner=3 seq=2 delta=-5100 t=-900")
            }
        }
        [void]$lines.Add("[10:01:25.000] SCENARIO CONSENSUS legmark leg=A phase=2 t=85000")

        # Leg B: the min-vote/cap/held-vote sequence. The denied raise is the
        # per-click "[speed] DENY" evidence line (WR-04): production's SET log
        # is change-gated, so a denied click (effective UNCHANGED at 1x) never
        # produces a SET line at that moment - the old fixture's baked
        # "denied" SET line was a shape production cannot emit.
        if ($GrantDeniedRaise) {
            [void]$lines.Add("[10:01:40.000] [speed] SET mult=3.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=1.00/0 2=1.00/0 3=1.00/0")
        } else {
            [void]$lines.Add("[10:01:40.000] [speed] DENY req=3.00 eff=1.00 cap=0 VOTES host=3.00/0 1=1.00/0 2=1.00/0 3=1.00/0")
        }
        if ($WrongMinAllRaised) {
            [void]$lines.Add("[10:01:50.000] [speed] SET mult=1.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=3.00/0 2=3.00/0 3=3.00/0")
        } else {
            [void]$lines.Add("[10:01:50.000] [speed] SET mult=3.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=3.00/0 2=3.00/0 3=3.00/0")
        }
        [void]$lines.Add("[10:02:00.000] [speed] SET mult=0.00 paused=1 combat=0 cap=0 VOTES host=3.00/0 1=0.00/0 2=3.00/0 3=3.00/0")
        if ($WrongMinAllRaised) {
            # WRONG-MIN also corrupts the post-pause recovery line - it is
            # the SAME "all voters at 3x" checkpoint as the earlier one, so
            # both must read the wrong effective for the defect to be
            # observable (a single corrupted line would still let this one
            # satisfy the all-raised check honestly).
            [void]$lines.Add("[10:02:05.000] [speed] SET mult=1.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=3.00/0 2=3.00/0 3=3.00/0")
        } else {
            [void]$lines.Add("[10:02:05.000] [speed] SET mult=3.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=3.00/0 2=3.00/0 3=3.00/0")
        }
        [void]$lines.Add("[10:02:15.000] [speed] SET mult=1.00 paused=0 combat=1 cap=1 VOTES host=3.00/1 1=3.00/0 2=3.00/0 3=3.00/0")
        [void]$lines.Add("[10:02:30.000] SCENARIO CONSENSUS legmark leg=B phase=1 t=150000")
        [void]$lines.Add("[10:02:35.000] [speed] SET mult=1.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=3.00/0 2=3.00/0 3=1.00/0")
        [void]$lines.Add("[10:03:00.000] [leave] speed vote=1 time report=1 owner=3")
        if (-not $OmitPostLeaveRecompute) {
            [void]$lines.Add("[10:03:00.500] [speed] SET mult=3.00 paused=0 combat=0 cap=0 VOTES host=3.00/0 1=3.00/0 2=3.00/0")
        }
        [void]$lines.Add("[10:03:40.000] SCENARIO CONSENSUS legmark leg=B phase=2 t=220000")
        return @($lines)
    }

    # Every-instance SCENARIO CONSENSUS moneyspend evidence (joins 1-3 only).
    $join1MoneyLines = @(
        "[10:00:22.000] SCENARIO CONSENSUS moneyspend rank=1 phase=solvent amount=150 ok=1 before=10000 after=9850 t=22000"
    )
    $join2MoneyLines = @(
        "[10:00:22.500] SCENARIO CONSENSUS moneyspend rank=2 phase=solvent amount=200 ok=1 before=9850 after=9650 t=22500",
        "[10:00:55.000] SCENARIO CONSENSUS moneyspend rank=2 phase=overdraft amount=5200 ok=1 before=9400 after=4200 t=55000"
    )
    $join3MoneyLines = @(
        "[10:00:23.000] SCENARIO CONSENSUS moneyspend rank=3 phase=solvent amount=250 ok=1 before=9650 after=9400 t=23000",
        "[10:00:55.500] SCENARIO CONSENSUS moneyspend rank=3 phase=overdraft amount=5100 ok=0 before=4200 after=4200 t=55500"
    )

    # Every-instance SCENARIO POOL series - final reading (post leg A settle,
    # t>=10:01:25) equals the authoritative total (4200), UNLESS a fixture
    # deliberately diverges one instance's own view (-DivergePoolLabel).
    function Get-PoolLines {
        param([bool]$Diverge = $false)
        $final = if ($Diverge) { 9000 } else { 4200 }
        return @(
            "[10:00:09.000] SCENARIO POOL money=10000 who=x t=9000",
            "[10:00:30.000] SCENARIO POOL money=9400 who=x t=30000",
            "[10:01:30.000] SCENARIO POOL money=$final who=x t=90000"
        )
    }
    # ONE-INSTANCE-MISSING-REJECT: that instance never carries the reject
    # line while its peers do.
    function Get-RejectLine { return "[10:00:55.750] [wallet] REJECT owner=3 seq=2 delta=-5100 pool=4200" }

    # Every-instance SCENARIO SPEED series - survivors (not rank3) raise to
    # 3x shortly after the [leave] line (10:03:00.000); rank3's own series
    # just stops (it disconnected).
    function Get-SpeedSeriesLines {
        param([int]$Rank, [bool]$Raise = $true)
        if ($Rank -eq 3) {
            return @(
                "[10:02:36.000] SCENARIO SPEED t=155000 mult=1.00 paused=0 nbtn=1 buttons=1x"
            )
        }
        $lines = @(
            "[10:01:41.000] SCENARIO SPEED t=100000 mult=1.00 paused=0 nbtn=1 buttons=1x",
            "[10:01:51.000] SCENARIO SPEED t=110000 mult=3.00 paused=0 nbtn=1 buttons=3x",
            "[10:02:36.000] SCENARIO SPEED t=155000 mult=1.00 paused=0 nbtn=1 buttons=1x"
        )
        if ($Raise) {
            $lines += "[10:03:05.000] SCENARIO SPEED t=185000 mult=3.00 paused=0 nbtn=1 buttons=3x"
        }
        return $lines
    }

    # Every-instance SCENARIO GTIME series - converged (delta 0) at both the
    # mid checkpoint (near 10:01:25) and the final tail, UNLESS
    # -DivergeGtime makes ONE instance's series drift past tolerance.
    function Get-GtimeLines {
        param([bool]$Diverge = $false)
        $lines = New-Object System.Collections.ArrayList
        $times = @("10:00:10.000", "10:01:00.000", "10:01:25.000", "10:02:00.000",
                   "10:03:00.000", "10:04:00.000", "10:04:50.000")
        $hoursBase = @(10.100, 10.600, 10.850, 11.500, 12.500, 13.500, 14.400)
        for ($i = 0; $i -lt $times.Count; $i++) {
            $h = $hoursBase[$i]
            if ($Diverge -and $i -ge ($times.Count - 2)) { $h += 0.05 } # only the final tail diverges
            $hStr = $h.ToString('F5', [System.Globalization.CultureInfo]::InvariantCulture)
            [void]$lines.Add("[$($times[$i])] SCENARIO GTIME hours=$hStr hourLen=109.0 fsm=1.00 paused=0 ok=1 t=$($i * 30000)")
        }
        return @($lines)
    }

    # Build a full 4-log fixture. Parameters select which corruption (if any)
    # to inject - see each helper's own doc comment above for the exact
    # regression each represents.
    function New-ConsensusFixture {
        param(
            [string]$Tmp,
            [bool]$SecondFoldOwner3 = $false,
            [bool]$DoubleFoldSolvent1 = $false,
            [bool]$ReplayReject3 = $false,
            [bool]$WrongMinAllRaised = $false,
            [bool]$OmitPostLeaveRecompute = $false,
            [bool]$GrantDeniedRaise = $false,
            [string]$OmitRejectLabel = "",
            [string]$DivergePoolLabel = "",
            [bool]$SuppressAllRaises = $false,
            [bool]$DivergeGtimeJoin3 = $false,
            [bool]$TotalNoSignal = $false
        )
        $labels = @("host", "join1", "join2", "join3")
        $ranks = @{ host = 0; join1 = 1; join2 = 2; join3 = 3 }
        $moneyByLabel = @{ host = @(); join1 = $join1MoneyLines; join2 = $join2MoneyLines; join3 = $join3MoneyLines }
        $extraByLabel = @{}

        foreach ($label in $labels) {
            $lines = New-Object System.Collections.ArrayList
            if ($TotalNoSignal) {
                $extraByLabel[$label] = @()
                continue
            }
            if ($label -eq "host") {
                [void]$lines.AddRange(@(Get-HostLegLines -SecondFoldOwner3:$SecondFoldOwner3 `
                    -DoubleFoldSolvent1:$DoubleFoldSolvent1 -ReplayReject3:$ReplayReject3 `
                    -WrongMinAllRaised:$WrongMinAllRaised -OmitPostLeaveRecompute:$OmitPostLeaveRecompute `
                    -GrantDeniedRaise:$GrantDeniedRaise))
            } else {
                [void]$lines.AddRange(@($moneyByLabel[$label]))
            }
            # host's own REJECT line is already part of Get-HostLegLines'
            # own ledger sequence (correct chronological placement) - only
            # the three joins need it added here.
            if ($label -ne "host" -and -not $SecondFoldOwner3 -and $label -ne $OmitRejectLabel) {
                [void]$lines.Add((Get-RejectLine))
            }
            $diverge = ($label -eq $DivergePoolLabel)
            [void]$lines.AddRange(@(Get-PoolLines -Diverge:$diverge))
            $raise = -not $SuppressAllRaises
            [void]$lines.AddRange(@(Get-SpeedSeriesLines -Rank $ranks[$label] -Raise:$raise))
            $divergeGtime = ($DivergeGtimeJoin3 -and $label -eq "join3")
            [void]$lines.AddRange(@(Get-GtimeLines -Diverge:$divergeGtime))
            $extraByLabel[$label] = @($lines)
        }

        $result = [ordered]@{}
        foreach ($label in $labels) {
            $path = Join-Path $Tmp "$label.log"
            New-ConsensusLog -Path $path -Rank $ranks[$label] -ExtraLines $extraByLabel[$label]
            $result[$label] = $path
        }
        return $result
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_consensus_selftest_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # 5. CLEAN fixture -> PASS.
        $logsClean = New-ConsensusFixture -Tmp $tmp
        Reset-GateResults
        $stClean = Invoke-OneOracleN -Id 'consensus' -Logs $logsClean
        $gClean = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "CLEAN fixture PASSes" ($stClean -eq 'PASS')
        Check "CLEAN fixture reports the authoritative final total (4200)" ($gClean.metrics.expectedFinal -eq 4200)

        # 6. ONE-INSTANCE-MISSING-REJECT fixture.
        $logsMissingReject = New-ConsensusFixture -Tmp $tmp -OmitRejectLabel "join2"
        Reset-GateResults
        $stMissingReject = Invoke-OneOracleN -Id 'consensus' -Logs $logsMissingReject
        $gMissingReject = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "ONE-INSTANCE-MISSING-REJECT fixture (join2 lacks the reject) FAILs" ($stMissingReject -eq 'FAIL')
        Check "ONE-INSTANCE-MISSING-REJECT finding names join2" ($gMissingReject.detail -match 'join2')

        # 7. MINTED-VALUE fixture.
        $logsMinted = New-ConsensusFixture -Tmp $tmp -SecondFoldOwner3 $true
        Reset-GateResults
        $stMinted = Invoke-OneOracleN -Id 'consensus' -Logs $logsMinted
        $gMinted = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "MINTED-VALUE fixture (both overdraft deltas fold) FAILs" ($stMinted -eq 'FAIL')
        Check "MINTED-VALUE finding names the negative-total defect" ($gMinted.detail -match 'minted value')

        # 8. DOUBLE-FOLD fixture.
        $logsDoubleFold = New-ConsensusFixture -Tmp $tmp -DoubleFoldSolvent1 $true
        Reset-GateResults
        $stDoubleFold = Invoke-OneOracleN -Id 'consensus' -Logs $logsDoubleFold
        $gDoubleFold = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "DOUBLE-FOLD fixture (owner=1,seq=1 folded twice) FAILs" ($stDoubleFold -eq 'FAIL')
        Check "DOUBLE-FOLD finding names the double-fold/replay defect" ($gDoubleFold.detail -match 'double-fold/replay')

        # 9. REPLAY fixture.
        $logsReplay = New-ConsensusFixture -Tmp $tmp -ReplayReject3 $true
        Reset-GateResults
        $stReplay = Invoke-OneOracleN -Id 'consensus' -Logs $logsReplay
        $gReplay = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "REPLAY fixture (a rejected seq is later also folded) FAILs" ($stReplay -eq 'FAIL')
        Check "REPLAY finding names the double-processed defect" ($gReplay.detail -match 'double-processed')

        # 10. WRONG-MIN fixture.
        $logsWrongMin = New-ConsensusFixture -Tmp $tmp -WrongMinAllRaised $true
        Reset-GateResults
        $stWrongMin = Invoke-OneOracleN -Id 'consensus' -Logs $logsWrongMin
        $gWrongMin = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "WRONG-MIN fixture (eff=1x despite all voters at 3x) FAILs" ($stWrongMin -eq 'FAIL')
        Check "WRONG-MIN finding names the all-must-raise rule" ($gWrongMin.detail -match 'all-must-raise')

        # 10b. DENIED-RAISE-GRANTED fixture (WR-04): the host's 3x click is
        #      wrongly GRANTED while every voter still reads 1.00 - a CHANGED
        #      effective, so production WOULD log this SET line if the min
        #      rule broke in the granted direction. The min-rule consistency
        #      invariant is what must catch it (the old denied-raise check
        #      could never fail this way - the vacuity WR-04 closes).
        $logsGranted = New-ConsensusFixture -Tmp $tmp -GrantDeniedRaise $true
        Reset-GateResults
        $stGranted = Invoke-OneOracleN -Id 'consensus' -Logs $logsGranted
        $gGranted = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "DENIED-RAISE-GRANTED fixture (eff=3x while voters hold 1x) FAILs" ($stGranted -eq 'FAIL')
        Check "DENIED-RAISE-GRANTED finding names the min-rule violation" ($gGranted.detail -match 'min-rule violation')

        # 11. VOTE-NOT-DROPPED fixture.
        $logsVoteNotDropped = New-ConsensusFixture -Tmp $tmp -OmitPostLeaveRecompute $true -SuppressAllRaises $true
        Reset-GateResults
        $stVoteNotDropped = Invoke-OneOracleN -Id 'consensus' -Logs $logsVoteNotDropped
        $gVoteNotDropped = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "VOTE-NOT-DROPPED fixture (no survivor ever raises after the leave) FAILs" ($stVoteNotDropped -eq 'FAIL')
        Check "VOTE-NOT-DROPPED finding names the instant-drop proof" ($gVoteNotDropped.detail -match 'instant-drop proof failed')

        # 12. CLOCK-DIVERGES fixture.
        $logsClockDiverges = New-ConsensusFixture -Tmp $tmp -DivergeGtimeJoin3 $true
        Reset-GateResults
        $stClockDiverges = Invoke-OneOracleN -Id 'consensus' -Logs $logsClockDiverges
        $gClockDiverges = Get-GateResults | Where-Object { $_.gate -eq 'consensus' } | Select-Object -Last 1
        Check "CLOCK-DIVERGES fixture (join3's final tail drifts past tolerance) FAILs" ($stClockDiverges -eq 'FAIL')
        Check "CLOCK-DIVERGES finding names the convergence defect" ($gClockDiverges.detail -match 'clock convergence diverges')

        # 13. Total no-signal: NO instance emits ANY SCENARIO CONSENSUS
        #     evidence -> SKIP (the manifest's NoSignalFails - already proven
        #     registered in check 3 above - is what turns this into a
        #     verdict FAIL at the analyze_run4.ps1 level).
        $logsTotalNoSignal = New-ConsensusFixture -Tmp $tmp -TotalNoSignal $true
        Reset-GateResults
        $stTotalNoSignal = Invoke-OneOracleN -Id 'consensus' -Logs $logsTotalNoSignal
        Check "total no-signal (0 of 4 instances report) SKIPs (well-formed, no signal)" ($stTotalNoSignal -eq 'SKIP')
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("ConsensusGate: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
