<#
.SYNOPSIS
  Gate wrapper for the item_conservation_gate N=4 oracle (Phase 7 plan 03).

.DESCRIPTION
  Given -RunDir pointing at a real archived 4-log item_conservation_gate run
  (plan 04's job), calls scripts\analyze_run4.ps1 -Scenario
  item_conservation_gate against it and asserts RESULT: PASS - the live gate
  check.

  Given NO -RunDir (the normal case before a live gate run exists), this is a
  STRUCTURAL smoke check (mirrors PlayerStateGate.Tests.ps1's own shape):
  (1) registration assertions - the scenario is chained in Scenario.cpp,
  declared in ScenarioSupport.h, listed in KenshiCoop.vcxproj, and
  item_conservation_gate is in scenarios.psd1 with inv_conservation as
  PrimaryGate and in NoSignalFails; inv_conservation is in
  Get-OracleRegistryN. (2) Oracle unit checks - synthetic 4-log fixtures
  (built from the SAME SCENARIO CONSERVE / [xfer] COMMIT / [wi] CLAIM-WIN
  line shapes ScenarioItemConservation.cpp / CoopOraclesN.psm1's
  Test-InvConservation actually emit/parse) proving the oracle:
    - PASSes a CLEAN fixture (every leg's before/after counts conserve, one
      commit per transfer, one winner per contention);
    - FAILs a DUP fixture (a rank's final total is higher than the ledger
      allows - a phantom gain with no matching evidence), naming the
      instance+rank+sid+delta;
    - FAILs a LOSS fixture (a rank's final total is lower than the ledger
      allows - a phantom loss), naming the instance+rank+sid+delta;
    - FAILs a conflicting-claim-winners fixture (two DIFFERENT winners
      logged for the same contention - "never both");
    - FAILs a duplicate-commit fixture (two COMMIT verdicts for the same
      authored transfer - "exactly one verdict, never both");
    - FAILs a partial no-signal fixture (one expected instance emits NO
      SCENARIO CONSERVE evidence while its peers do) - an evidentiary gap is
      a FAIL, never a silent pass;
    - SKIPs a total no-signal fixture (NO instance emits any CONSERVE/
      COMMIT/CLAIM-WIN evidence at all) - the manifest's NoSignalFails then
      turns that SKIP into a verdict FAIL at the analyze_run4.ps1 level;
    - PASSes a disconnect-after-commit fixture (a committed transfer's
      author disconnects AFTER the commit lands) - the commit "stands", a
      later disconnect never retroactively invalidates it.

  Exit code = number of failed checks (0 = PASS), matching
  PlayerStateGate.Tests.ps1's own convention so verify.ps1/regress.ps1 can
  sum them.

.EXAMPLE
  # Self-test (no live run needed):
  powershell -ExecutionPolicy Bypass -File scripts\tests\InvConservationGate.Tests.ps1

.EXAMPLE
  # Judge a real archived 4-log run (plan 04):
  powershell -ExecutionPolicy Bypass -File scripts\tests\InvConservationGate.Tests.ps1 -RunDir tools\test-runs\20260904_120000_N4
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

# ---- tiny assert harness (verbatim shape from PlayerStateGate.Tests.ps1) -----
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
    # ---- Real 4-log item_conservation_gate run judgment (plan 04) -------------
    Write-Host "== InvConservationGate: judging archived run dir =="
    Write-Host "  RunDir: $RunDir"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $RunDir -Scenario item_conservation_gate 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "  | $_" }
    $outText = ($output -join "`n")

    Check "analyze_run4.ps1 exited 0 against the archived run" ($exitCode -eq 0)
    Check "analyze_run4.ps1 reported RESULT: PASS" ($outText -match '(?m)^RESULT: PASS\s*$')
} else {
    # ---- Structural smoke check (no live run yet) ------------------------------
    Write-Host "== InvConservationGate: no -RunDir given; structural smoke check =="

    # 1. Scenario registration (chain/decl/vcxproj).
    $scenarioCpp   = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\Scenario.cpp")
    $supportH      = Get-Content -Raw (Join-Path $repoRoot "src\plugin\test\ScenarioSupport.h")
    $vcxproj       = Get-Content -Raw (Join-Path $repoRoot "src\plugin\KenshiCoop.vcxproj")
    Check "makeItemConservationScenario chained in Scenario.cpp" ($scenarioCpp -match 'makeItemConservationScenario')
    Check "makeItemConservationScenario declared in ScenarioSupport.h" ($supportH -match 'makeItemConservationScenario')
    Check "ScenarioItemConservation.cpp listed in KenshiCoop.vcxproj" ($vcxproj -match 'ScenarioItemConservation\.cpp')

    # 2. Oracle registration.
    Check "Test-InvConservation is exported" ($null -ne (Get-Command Test-InvConservation -ErrorAction SilentlyContinue))
    $registry = Get-OracleRegistryN
    Check "inv_conservation is in Get-OracleRegistryN" ($registry -contains 'inv_conservation')

    # 3. Manifest entry.
    $manifest = Get-ScenarioManifest
    $entry = $manifest.Scenarios['item_conservation_gate']
    Check "scenarios.psd1 has an item_conservation_gate entry" ($null -ne $entry)
    if ($null -ne $entry) {
        Check "item_conservation_gate.PrimaryGate is inv_conservation" ($entry.PrimaryGate -eq 'inv_conservation')
        Check "item_conservation_gate.Gating includes inv_conservation" (@($entry.Gating) -contains 'inv_conservation')
        Check "item_conservation_gate.NoSignalFails includes inv_conservation" (@($entry.NoSignalFails) -contains 'inv_conservation')
        Check "item_conservation_gate.DiagEnv forces KENSHICOOP_INV_SYNC=1" ("$($entry.DiagEnv['KENSHICOOP_INV_SYNC'])" -eq '1')
        Check "item_conservation_gate.DiagEnv forces KENSHICOOP_WORLD_SYNC=1" ("$($entry.DiagEnv['KENSHICOOP_WORLD_SYNC'])" -eq '1')
    }

    # ---- Fixture builder ------------------------------------------------------
    # Mirrors the item_conservation_gate script's own 6-leg design (baseline
    # SEED_QTY=6/rank): P2->P3 (1->2,qty2), P3->P4 (2->3,qty2),
    # P4->host (3->0,qty2), simultaneous SimA (1->0,qty1) + SimB (3->2,qty1),
    # then host (rank0) drops 1 unit and ranks 1/2 both attempt the pickup -
    # the baseline (correctly conserved) final per-rank totals are
    # rank0=8, rank1=4, rank2=7, rank3=5 (winner = rank1), ground=0,
    # global owner-of-record sum constant at 24.
    function New-InvLog {
        param([string]$Path, [int]$Rank, [string[]]$ExtraLines = @())
        $isHost = if ($Rank -eq 0) { 1 } else { 0 }
        $lines = @(
            "[10:00:00.000] KenshiCoop: gameplay started",
            "[10:00:00.500] SCENARIO MAGATE start ownRank=$Rank host=$isHost"
        ) + $ExtraLines + @("[10:00:20.000] SCENARIO RESULT PASS")
        $lines | Set-Content -Path $Path -Encoding UTF8
    }

    $cont0Lines = @(
        "SCENARIO CONSERVE ck=0 scope=cont rank=0 hand=1,2,3,4,5 sid='item_a' qty=6",
        "SCENARIO CONSERVE ck=0 scope=cont rank=1 hand=1,2,3,4,6 sid='item_a' qty=6",
        "SCENARIO CONSERVE ck=0 scope=cont rank=2 hand=1,2,3,4,7 sid='item_a' qty=6",
        "SCENARIO CONSERVE ck=0 scope=cont rank=3 hand=1,2,3,4,8 sid='item_a' qty=6",
        "SCENARIO CONSERVE ck=0 scope=ground sid='item_a' qty=0"
    )
    $xferP23  = "SCENARIO CONSERVE XFER src=1 dst=2 qty=2 moved=2 sid='item_a' t=20000"
    $xferP34  = "SCENARIO CONSERVE XFER src=2 dst=3 qty=2 moved=2 sid='item_a' t=36000"
    $xferP4H  = "SCENARIO CONSERVE XFER src=3 dst=0 qty=2 moved=2 sid='item_a' t=52000"
    $xferSimA = "SCENARIO CONSERVE XFER src=1 dst=0 qty=1 moved=1 sid='item_a' t=68000"
    $xferSimB = "SCENARIO CONSERVE XFER src=3 dst=2 qty=1 moved=1 sid='item_a' t=68000"
    $claim1   = "SCENARIO CONSERVE CLAIM rank=1 got=1 sid='item_a'"
    $claim2   = "SCENARIO CONSERVE CLAIM rank=2 got=1 sid='item_a'"
    $dropLine = "SCENARIO CONSERVE DROP rank=0 n=1 sid='item_a'"
    $winLine  = "[wi] CLAIM-WIN author=0 netId=42 winner=1 n=1"
    $baseCommitLines = @(
        "[xfer] COMMIT id=1 author=1 outcome=RELOCATE applied=2 sid='item_a' type=2 qty=2",
        "[xfer] COMMIT id=1 author=2 outcome=RELOCATE applied=2 sid='item_a' type=2 qty=2",
        "[xfer] COMMIT id=1 author=3 outcome=RELOCATE applied=2 sid='item_a' type=2 qty=2",
        "[xfer] COMMIT id=2 author=1 outcome=RELOCATE applied=1 sid='item_a' type=2 qty=1",
        "[xfer] COMMIT id=2 author=3 outcome=RELOCATE applied=1 sid='item_a' type=2 qty=1"
    )
    $baselineFinal = @{ 0 = 8; 1 = 4; 2 = 7; 3 = 5 }

    # Build a full 4-log fixture. $FinalOverrides mutates specific ranks' final
    # (ck=5) qty away from the conserved baseline (DUP/LOSS). $OmitLabel drops
    # ALL CONSERVE/XFER/DROP/CLAIM evidence for that one instance (partial
    # no-signal). $ExtraCommitLines/$ExtraWinLines/$ExtraHostLines let a case
    # inject a duplicate/conflicting verdict or a post-commit disconnect.
    function New-InvFixture {
        param(
            [string]$Tmp,
            [hashtable]$FinalOverrides = @{},
            [string]$OmitLabel = "",
            [string[]]$ExtraCommitLines = @(),
            [string[]]$ExtraWinLines = @(),
            [string[]]$ExtraHostLines = @()
        )
        $final = @{}
        foreach ($r in @(0, 1, 2, 3)) { $final[$r] = if ($FinalOverrides.ContainsKey($r)) { $FinalOverrides[$r] } else { $baselineFinal[$r] } }
        $contFinalLines = @(
            "SCENARIO CONSERVE ck=5 scope=cont rank=0 hand=1,2,3,4,5 sid='item_a' qty=$($final[0])",
            "SCENARIO CONSERVE ck=5 scope=cont rank=1 hand=1,2,3,4,6 sid='item_a' qty=$($final[1])",
            "SCENARIO CONSERVE ck=5 scope=cont rank=2 hand=1,2,3,4,7 sid='item_a' qty=$($final[2])",
            "SCENARIO CONSERVE ck=5 scope=cont rank=3 hand=1,2,3,4,8 sid='item_a' qty=$($final[3])",
            "SCENARIO CONSERVE ck=5 scope=ground sid='item_a' qty=0"
        )
        $commitLines = @($baseCommitLines) + @($ExtraCommitLines)
        $winLines = @($winLine) + @($ExtraWinLines)

        $hostExtra   = @($cont0Lines) + @($contFinalLines) + $commitLines + @($dropLine) + $winLines + @($ExtraHostLines)
        $join1Extra  = @($cont0Lines) + @($contFinalLines) + @($xferP23, $xferSimA, $claim1)
        $join2Extra  = @($cont0Lines) + @($contFinalLines) + @($xferP34, $claim2)
        $join3Extra  = @($cont0Lines) + @($contFinalLines) + @($xferP4H, $xferSimB)

        if ($OmitLabel -eq "host")  { $hostExtra  = @() }
        if ($OmitLabel -eq "join1") { $join1Extra = @() }
        if ($OmitLabel -eq "join2") { $join2Extra = @() }
        if ($OmitLabel -eq "join3") { $join3Extra = @() }

        New-InvLog -Path (Join-Path $Tmp "host.log")  -Rank 0 -ExtraLines $hostExtra
        New-InvLog -Path (Join-Path $Tmp "join1.log") -Rank 1 -ExtraLines $join1Extra
        New-InvLog -Path (Join-Path $Tmp "join2.log") -Rank 2 -ExtraLines $join2Extra
        New-InvLog -Path (Join-Path $Tmp "join3.log") -Rank 3 -ExtraLines $join3Extra

        return [ordered]@{
            host  = (Join-Path $Tmp "host.log")
            join1 = (Join-Path $Tmp "join1.log")
            join2 = (Join-Path $Tmp "join2.log")
            join3 = (Join-Path $Tmp "join3.log")
        }
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_invconserve_selftest_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # 4. CLEAN fixture -> PASS on all four legs.
        $logsClean = New-InvFixture -Tmp $tmp
        Reset-GateResults
        $stClean = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsClean
        $gClean = Get-GateResults | Where-Object { $_.gate -eq 'inv_conservation' } | Select-Object -Last 1
        Check "CLEAN fixture PASSes" ($stClean -eq 'PASS')
        Check "CLEAN fixture judges all 4 instances (Leg A)" ($gClean.metrics.legAChecked -eq 4 -and $gClean.metrics.legAConverged -eq 4)

        # 5. DUP fixture: rank1's final total is 1 HIGHER than the ledger allows
        #    (a phantom gain with no matching evidence) -> FAIL naming rank+sid.
        $logsDup = New-InvFixture -Tmp $tmp -FinalOverrides @{ 1 = ($baselineFinal[1] + 1) }
        Reset-GateResults
        $stDup = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsDup
        $gDup = Get-GateResults | Where-Object { $_.gate -eq 'inv_conservation' } | Select-Object -Last 1
        Check "DUP fixture (phantom gain) FAILs" ($stDup -eq 'FAIL')
        Check "DUP fixture names the offending rank+sid" ($gDup.detail -match "rank=1" -and $gDup.detail -match "sid='item_a'")

        # 6. LOSS fixture: rank3's final total is 1 LOWER than the ledger allows
        #    (a phantom loss with no consuming commit) -> FAIL naming rank+sid.
        $logsLoss = New-InvFixture -Tmp $tmp -FinalOverrides @{ 3 = ($baselineFinal[3] - 1) }
        Reset-GateResults
        $stLoss = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsLoss
        $gLoss = Get-GateResults | Where-Object { $_.gate -eq 'inv_conservation' } | Select-Object -Last 1
        Check "LOSS fixture (phantom loss) FAILs" ($stLoss -eq 'FAIL')
        Check "LOSS fixture names the offending rank+sid" ($gLoss.detail -match "rank=3" -and $gLoss.detail -match "sid='item_a'")

        # 7. Conflicting claim winners: TWO different winners logged for the
        #    SAME (author,netId) contention -> FAIL ("never both").
        $logsConflictWin = New-InvFixture -Tmp $tmp -ExtraWinLines @("[wi] CLAIM-WIN author=0 netId=42 winner=2 n=1")
        Reset-GateResults
        $stConflictWin = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsConflictWin
        $gConflictWin = Get-GateResults | Where-Object { $_.gate -eq 'inv_conservation' } | Select-Object -Last 1
        Check "conflicting CLAIM-WIN winners FAILs (never both)" ($stConflictWin -eq 'FAIL')
        Check "conflicting CLAIM-WIN finding names 'conflicting winners'" ($gConflictWin.detail -match 'conflicting winners')

        # 8. Duplicate commit: TWO [xfer] COMMIT lines for the SAME (author,id)
        #    authored transfer -> FAIL ("exactly one verdict, never both").
        $logsDupCommit = New-InvFixture -Tmp $tmp -ExtraCommitLines @("[xfer] COMMIT id=1 author=1 outcome=RELOCATE applied=2 sid='item_a' type=2 qty=2")
        Reset-GateResults
        $stDupCommit = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsDupCommit
        $gDupCommit = Get-GateResults | Where-Object { $_.gate -eq 'inv_conservation' } | Select-Object -Last 1
        Check "duplicate [xfer] COMMIT FAILs (never both)" ($stDupCommit -eq 'FAIL')
        Check "duplicate commit finding names 'duplicate verdict'" ($gDupCommit.detail -match 'duplicate verdict')

        # 9. Partial no-signal: join2 emits NO SCENARIO CONSERVE evidence at all
        #    while its 3 peers fully report -> FAIL (evidence gap), never a
        #    silent pass on the 3 instances that DID report.
        $logsPartialNoSignal = New-InvFixture -Tmp $tmp -OmitLabel "join2"
        Reset-GateResults
        $stPartialNoSignal = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsPartialNoSignal
        $gPartialNoSignal = Get-GateResults | Where-Object { $_.gate -eq 'inv_conservation' } | Select-Object -Last 1
        Check "partial no-signal (1 of 4 instances silent) FAILs, not a silent pass" ($stPartialNoSignal -eq 'FAIL')
        Check "partial no-signal finding names the missing instance" ($gPartialNoSignal.detail -match 'join2' -and $gPartialNoSignal.detail -match 'evidence gap')

        # 10. Total no-signal: NO instance emits ANY CONSERVE/COMMIT/CLAIM-WIN
        #     evidence -> SKIP (the manifest's NoSignalFails - already proven
        #     registered in check 3 above - is what turns this into a verdict
        #     FAIL at the analyze_run4.ps1 level; the oracle itself reports the
        #     honest "nothing to judge" state, never a false PASS).
        New-InvLog -Path (Join-Path $tmp "host.log")  -Rank 0
        New-InvLog -Path (Join-Path $tmp "join1.log") -Rank 1
        New-InvLog -Path (Join-Path $tmp "join2.log") -Rank 2
        New-InvLog -Path (Join-Path $tmp "join3.log") -Rank 3
        $logsTotalNoSignal = [ordered]@{
            host  = (Join-Path $tmp "host.log")
            join1 = (Join-Path $tmp "join1.log")
            join2 = (Join-Path $tmp "join2.log")
            join3 = (Join-Path $tmp "join3.log")
        }
        Reset-GateResults
        $stTotalNoSignal = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsTotalNoSignal
        Check "total no-signal (0 of 4 instances report) SKIPs (well-formed, no signal)" ($stTotalNoSignal -eq 'SKIP')

        # 11. Disconnect-after-commit: a committed transfer's author
        #     disconnects AFTER the commit lands - the commit STANDS; a later
        #     disconnect never retroactively invalidates an already-committed
        #     verdict (INV-04's "host committed -> transfer stands" rule) -
        #     this fixture must still PASS like the CLEAN baseline.
        $logsDisconnect = New-InvFixture -Tmp $tmp -ExtraHostLines @(
            "[10:00:15.000] SCENARIO MAGATE DISCONNECT peer=1 ownRank=0 t=90000",
            "[10:00:15.100] SCENARIO MAGATE SURVIVOR ownRank=0 ok=1"
        )
        Reset-GateResults
        $stDisconnect = Invoke-OneOracleN -Id 'inv_conservation' -Logs $logsDisconnect
        Check "disconnect-after-commit still PASSes (committed transfer stands)" ($stDisconnect -eq 'PASS')
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("InvConservationGate: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
