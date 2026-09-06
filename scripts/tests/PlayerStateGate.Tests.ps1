<#
.SYNOPSIS
  Gate wrapper for the player_state_gate N=4 oracle set (Phase 6 plan 02).

.DESCRIPTION
  Given -RunDir pointing at a real archived 4-log player_state_gate run
  (plan 03's job), calls scripts\analyze_run4.ps1 -Scenario player_state_gate
  against it and asserts RESULT: PASS - the live gate check.

  Given NO -RunDir (the normal case before a live gate run exists), this is a
  STRUCTURAL smoke check (mirrors MilestoneAGate.Tests.ps1's own self-test
  shape, 06-02-PLAN.md Task 3's own "structural smoke check if no fixture
  logs exist yet" allowance): it asserts the two new gate functions exist,
  are registered in Get-OracleRegistryN + dispatched by Invoke-OneOracleN,
  and each returns a well-formed PASS/FAIL/SKIP verdict object on tiny
  synthetic fixtures (a clean 4-log set with no player_state evidence at all
  - both gates must SKIP, never silently PASS with nothing judged; a set with
  matching SCENARIO RECRUIT/[recruit] EVT/REKEY-BIND evidence - Gate B must
  PASS; a set with a mismatched REKEY-BIND hand - Gate B must FAIL).

  Exit code = number of failed checks (0 = PASS), matching
  MilestoneAGate.Tests.ps1's own convention so verify.ps1/regress.ps1 can sum
  them.

.EXAMPLE
  # Self-test (no live run needed):
  powershell -ExecutionPolicy Bypass -File scripts\tests\PlayerStateGate.Tests.ps1

.EXAMPLE
  # Judge a real archived 4-log run (plan 03):
  powershell -ExecutionPolicy Bypass -File scripts\tests\PlayerStateGate.Tests.ps1 -RunDir tools\test-runs\20260903_120000_N4
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

# ---- tiny assert harness ------------------------------------------------------
# (verbatim shape from scripts\tests\MilestoneAGate.Tests.ps1)
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

function New-StubLog {
    param([string]$Path, [string[]]$ExtraLines = @())
    $lines = @(
        "[10:00:00.000] KenshiCoop: gameplay started",
        "[10:00:00.500] SCENARIO MAGATE start ownRank=0 host=1"
    ) + $ExtraLines + @("[10:00:20.000] SCENARIO RESULT PASS")
    $lines | Set-Content -Path $Path -Encoding UTF8
}

if ($RunDir -ne "") {
    # ---- Real 4-log player_state_gate run judgment (plan 03) ------------------
    Write-Host "== PlayerStateGate: judging archived run dir =="
    Write-Host "  RunDir: $RunDir"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $RunDir -Scenario player_state_gate 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "  | $_" }
    $outText = ($output -join "`n")

    Check "analyze_run4.ps1 exited 0 against the archived run" ($exitCode -eq 0)
    Check "analyze_run4.ps1 reported RESULT: PASS" ($outText -match '(?m)^RESULT: PASS\s*$')
} else {
    # ---- Structural smoke check (no live run yet) ------------------------------
    Write-Host "== PlayerStateGate: no -RunDir given; structural smoke check =="

    # 1. The gate functions exist and are exported.
    Check "Test-MagatePlayerStateMedical is exported" ($null -ne (Get-Command Test-MagatePlayerStateMedical -ErrorAction SilentlyContinue))
    Check "Test-MagatePlayerStateRecruit is exported"  ($null -ne (Get-Command Test-MagatePlayerStateRecruit  -ErrorAction SilentlyContinue))

    # 2. Both ids are registered + dispatchable.
    $registry = Get-OracleRegistryN
    Check "player_state_medical is in Get-OracleRegistryN" ($registry -contains 'player_state_medical')
    Check "player_state_recruit is in Get-OracleRegistryN" ($registry -contains 'player_state_recruit')

    # 3. The manifest carries a player_state_gate entry naming both ids.
    $manifest = Get-ScenarioManifest
    $entry = $manifest.Scenarios['player_state_gate']
    Check "scenarios.psd1 has a player_state_gate entry" ($null -ne $entry)
    if ($null -ne $entry) {
        Check "player_state_gate.PrimaryGate is player_state_medical" ($entry.PrimaryGate -eq 'player_state_medical')
        Check "player_state_gate.Gating includes both new ids" `
            (@($entry.Gating) -contains 'player_state_medical' -and @($entry.Gating) -contains 'player_state_recruit')
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_pstate_selftest_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # 4. No player_state evidence at all in any of the 4 logs -> both
        #    gates SKIP (no signal), never a false PASS.
        New-StubLog -Path (Join-Path $tmp "host.log")
        New-StubLog -Path (Join-Path $tmp "join1.log")
        New-StubLog -Path (Join-Path $tmp "join2.log")
        New-StubLog -Path (Join-Path $tmp "join3.log")
        $logs = [ordered]@{
            host  = (Join-Path $tmp "host.log")
            join1 = (Join-Path $tmp "join1.log")
            join2 = (Join-Path $tmp "join2.log")
            join3 = (Join-Path $tmp "join3.log")
        }

        Reset-GateResults
        $stMed = Invoke-OneOracleN -Id 'player_state_medical' -Logs $logs
        Check "player_state_medical SKIPs (well-formed, no signal) on evidence-free logs" ($stMed -eq 'SKIP')

        Reset-GateResults
        $stRec = Invoke-OneOracleN -Id 'player_state_recruit' -Logs $logs
        Check "player_state_recruit SKIPs (well-formed, no signal) on evidence-free logs" ($stRec -eq 'SKIP')

        # 5. A matching recruit fixture (author + 3 observers all consistent)
        #    -> Gate B PASSes. Evidence shape calibrated on live run
        #    20260902_174928_N4: observers must log the authenticated
        #    "[event] RECV ... ev=10 owner=<authorRank>" edge crossing AND a
        #    "[recruit] REKEY..." line (REKEY-BIND for a resolvable local
        #    body; REKEY-FALLBACK/REKEY ok=0 for a runtime force-REQ - the
        #    gate accepts any flavor). New-StubLog stamps every stub
        #    ownRank=0, so the author's rank identity resolves to 0.
        $afterHand = "3,4,0,55,1"          # t,c,cs,i,s (probeRecruit/[recruit] order)
        $afterMemKey = "55,1,3,4,0"        # i,s,t,c,cs (MEMBER/RECV key order)
        $beforeHand = "0,0,0,9,9"

        New-StubLog -Path (Join-Path $tmp "join3.log") -ExtraLines @(
            "[10:00:08.000] SCENARIO RECRUIT who=join leg=runtime res=1 before=$beforeHand after=$afterHand t=8000",
            "[10:00:08.050] [recruit] EVT send old=$beforeHand new=$afterHand",
            "[10:00:08.500] SCENARIO MEMBER hand=$afterMemKey pos=1.00,2.00,3.00 task=0 pelvis=1.0 crouch=0 idle=0 bs=0"
        )
        foreach ($obs in @('host', 'join1', 'join2')) {
            New-StubLog -Path (Join-Path $tmp "$obs.log") -ExtraLines @(
                "[10:00:08.590] [event] RECV id=1 ev=10 owner=0 hand=$beforeHand actor=55,1",
                "[10:00:08.600] [recruit] REKEY-BIND new=$afterHand playerSquad=1 wasSuppressed=0 tabKnown=1 rank=0 c=0x0",
                "[10:00:09.000] SCENARIO RECV hand=$afterMemKey pos=1.00,2.00,3.00 task=0 pelvis=1.0 crouch=0 idle=0 bs=0"
            )
        }
        Reset-GateResults
        $stRecOk = Invoke-OneOracleN -Id 'player_state_recruit' -Logs $logs
        Check "player_state_recruit PASSes on a consistent recruit fixture" ($stRecOk -eq 'PASS')

        # 6. An observer that never received/re-keyed the recruit edge
        #    -> Gate B FAILs (proves the guard actually fires, not merely exists).
        New-StubLog -Path (Join-Path $tmp "host.log") -ExtraLines @(
            "[10:00:09.000] SCENARIO RECV hand=$afterMemKey pos=1.00,2.00,3.00 task=0 pelvis=1.0 crouch=0 idle=0 bs=0"
        )
        Reset-GateResults
        $stRecBad = Invoke-OneOracleN -Id 'player_state_recruit' -Logs $logs
        Check "player_state_recruit FAILs when an observer never logs the RECV/re-key" ($stRecBad -eq 'FAIL')

        # 7. WR-03 minimum coverage: VITALS evidence for only ONE of the four
        #    owners (an evidence regression on the other three) -> the medical
        #    gate must SKIP (coverage shortfall), never green the run on the
        #    single remaining owner. Needs distinct ownRank per log (New-StubLog
        #    stamps every stub ownRank=0, which would collapse the expected-
        #    owner set to {0} and defeat the check).
        function New-RankedLog {
            param([string]$Path, [int]$Rank, [string[]]$ExtraLines = @())
            $isHost = 0
            if ($Rank -eq 0) { $isHost = 1 }
            $lines = @(
                "[10:00:00.000] KenshiCoop: gameplay started",
                "[10:00:00.500] SCENARIO MAGATE start ownRank=$Rank host=$isHost",
                "[10:00:00.600] SCENARIO MAGATE TABMAP rank=0 hand=10,100",
                "[10:00:00.600] SCENARIO MAGATE TABMAP rank=1 hand=11,101",
                "[10:00:00.600] SCENARIO MAGATE TABMAP rank=2 hand=12,102",
                "[10:00:00.600] SCENARIO MAGATE TABMAP rank=3 hand=13,103"
            ) + $ExtraLines + @("[10:00:20.000] SCENARIO RESULT PASS")
            $lines | Set-Content -Path $Path -Encoding UTF8
        }
        $rank0Vitals = @(
            "[10:00:05.000] SCENARIO VITALS hand=10,100 t=5000 blood=100",
            "[10:00:06.000] SCENARIO VITALS hand=10,100 t=6000 blood=100",
            "[10:00:07.000] SCENARIO VITALS hand=10,100 t=7000 blood=100"
        )
        New-RankedLog -Path (Join-Path $tmp "host.log") -Rank 0 -ExtraLines $rank0Vitals
        foreach ($i in 1..3) {
            New-RankedLog -Path (Join-Path $tmp "join$i.log") -Rank $i -ExtraLines $rank0Vitals
        }
        Reset-GateResults
        $stMedCov = Invoke-OneOracleN -Id 'player_state_medical' -Logs $logs
        Check "player_state_medical SKIPs (coverage) when only 1 of 4 owners has VITALS evidence" ($stMedCov -eq 'SKIP')
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }

    # ---- Phase 11 plan 02 (TEST-01): N=2/N=3 log-set fixtures ------------------
    # Test-MagatePlayerStateMedical/Recruit already iterate $Logs.Keys
    # generically (no hidden 4-only assumption) - the scenario-side fix is
    # what needed adaptation (ScenarioPlayerState.cpp's stealthRank()/
    # recruitRank(), Task 1). These fixtures prove the oracle judges a
    # SMALLER log dict correctly, with the author at the rank Task 1's
    # adaptive fallback actually resolves to at that N.
    function New-RankedLogN {
        param([string]$Path, [int]$Rank, [string[]]$ExtraLines = @())
        $isHost = if ($Rank -eq 0) { 1 } else { 0 }
        $lines = @(
            "[10:00:00.000] KenshiCoop: gameplay started",
            "[10:00:00.500] SCENARIO MAGATE start ownRank=$Rank host=$isHost"
        ) + $ExtraLines + @("[10:00:20.000] SCENARIO RESULT PASS")
        $lines | Set-Content -Path $Path -Encoding UTF8
    }
    $nAfterHand   = "3,4,0,55,1"
    $nAfterMemKey = "55,1,3,4,0"
    $nBeforeHand  = "0,0,0,9,9"

    foreach ($nSpec in @(
            @{ n = 2; authorRank = 1; labels = @("host", "join1") },
            @{ n = 3; authorRank = 2; labels = @("host", "join1", "join2") })) {
        $tmpN = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_pstate_n$($nSpec.n)_selftest_" + [System.Guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Force -Path $tmpN | Out-Null
        try {
            $authorLabel = "join$($nSpec.authorRank)"
            foreach ($lbl in $nSpec.labels) {
                if ($lbl -eq $authorLabel) {
                    New-RankedLogN -Path (Join-Path $tmpN "$lbl.log") -Rank $nSpec.authorRank -ExtraLines @(
                        "[10:00:08.000] SCENARIO RECRUIT who=join leg=runtime res=1 before=$nBeforeHand after=$nAfterHand t=8000",
                        "[10:00:08.050] [recruit] EVT send old=$nBeforeHand new=$nAfterHand",
                        "[10:00:08.500] SCENARIO MEMBER hand=$nAfterMemKey pos=1.00,2.00,3.00 task=0 pelvis=1.0 crouch=0 idle=0 bs=0"
                    )
                } else {
                    $rank = if ($lbl -eq "host") { 0 } else { [int]($lbl -replace 'join', '') }
                    New-RankedLogN -Path (Join-Path $tmpN "$lbl.log") -Rank $rank -ExtraLines @(
                        "[10:00:08.590] [event] RECV id=1 ev=10 owner=$($nSpec.authorRank) hand=$nBeforeHand actor=55,1",
                        "[10:00:08.600] [recruit] REKEY-BIND new=$nAfterHand playerSquad=1 wasSuppressed=0 tabKnown=1 rank=$($nSpec.authorRank) c=0x0",
                        "[10:00:09.000] SCENARIO RECV hand=$nAfterMemKey pos=1.00,2.00,3.00 task=0 pelvis=1.0 crouch=0 idle=0 bs=0"
                    )
                }
            }
            $logsN = [ordered]@{}
            foreach ($lbl in $nSpec.labels) { $logsN[$lbl] = (Join-Path $tmpN "$lbl.log") }
            Reset-GateResults
            $stN = Invoke-OneOracleN -Id 'player_state_recruit' -Logs $logsN
            Check "player_state_recruit PASSes on an N=$($nSpec.n) log set authored by rank $($nSpec.authorRank) (the adaptive recruitRank() fallback at this N)" ($stN -eq 'PASS')
        } finally {
            Remove-Item -Recurse -Force $tmpN -ErrorAction SilentlyContinue
        }
    }
}

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("PlayerStateGate: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
