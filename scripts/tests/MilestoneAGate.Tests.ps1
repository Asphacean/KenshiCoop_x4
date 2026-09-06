<#
.SYNOPSIS
  Gate wrapper for the Milestone A N=4 oracle set (Phase 4 Plan 05, POC-03).

.DESCRIPTION
  Given -RunDir pointing at a real archived 4-log DoD run (plan 06's job), calls
  scripts\analyze_run4.ps1 against it and asserts RESULT: PASS - the live gate
  check.

  Given NO -RunDir (the normal case before a live gate run exists), this
  self-tests the 04-RESEARCH.md Pitfall 5 fail-loud guard instead: it builds a
  throwaway temp dir containing only 2 of the 4 expected logs (host.log +
  join1.log; join2.log/join3.log deliberately absent) and asserts BOTH
  analyze_run4.ps1 (the end-to-end script) and CoopOraclesN's
  Assert-AllLogsPresent (the guard function itself) FAIL LOUD on it - proving
  the guard actually fires rather than merely existing in source. A negative
  control (all 4 logs present) proves the guard does not also fire on a
  legitimately-complete set, so the FAIL above is evidence of the guard
  working, not a scenario the guard rejects unconditionally.

  This makes the wrapper automatable (part of the regular test suite,
  verify.ps1/regress.ps1 can sum its exit code) before any live N=4 run has
  ever happened - exactly the plan 05 must_have: "the oracle FAILS LOUD if
  fewer than the configured number of logs are found".

  Exit code = number of failed checks (0 = PASS), matching EntityRelay.Tests.ps1
  / NetRegistry.Tests.ps1 / prototest's convention so verify.ps1/regress.ps1 can
  sum them.

.EXAMPLE
  # Self-test the fail-loud guard (no live run needed):
  powershell -ExecutionPolicy Bypass -File scripts\tests\MilestoneAGate.Tests.ps1

.EXAMPLE
  # Judge a real archived 4-log run (plan 06):
  powershell -ExecutionPolicy Bypass -File scripts\tests\MilestoneAGate.Tests.ps1 -RunDir tools\test-runs\20260902_120000_N4
#>
[CmdletBinding()]
param(
    [string]$RunDir = ""
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                        # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                      # repo root

# ---- tiny assert harness ------------------------------------------------------
# (verbatim shape from scripts\tests\EntityRelay.Tests.ps1)
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
    param([string]$Path)
    @(
        "[10:00:00.000] KenshiCoop: gameplay started",
        "[10:00:01.000] SCENARIO RESULT PASS"
    ) | Set-Content -Path $Path -Encoding UTF8
}

if ($RunDir -ne "") {
    # ---- Real 4-log DoD run judgment (plan 06) --------------------------------
    Write-Host "== MilestoneAGate: judging archived run dir =="
    Write-Host "  RunDir: $RunDir"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $RunDir 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "  | $_" }
    $outText = ($output -join "`n")

    Check "analyze_run4.ps1 exited 0 against the archived run" ($exitCode -eq 0)
    Check "analyze_run4.ps1 reported RESULT: PASS" ($outText -match '(?m)^RESULT: PASS\s*$')
} else {
    # ---- Self-test: prove the Pitfall-5 fail-loud under-count guard fires -----
    Write-Host "== MilestoneAGate: no -RunDir given; self-testing the fail-loud under-count guard =="
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_magate_selftest_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        # Only 2 of the 4 required logs. Each has a clean "SCENARIO RESULT PASS" -
        # a defect-free verdict rule with no under-count guard would otherwise
        # have no OTHER reason to fail these two, which is exactly why this
        # proves the guard (not some unrelated log-health defect) is what fires.
        New-StubLog -Path (Join-Path $tmp "host.log")
        New-StubLog -Path (Join-Path $tmp "join1.log")

        Write-Host "-- analyze_run4.ps1 against a 2-of-4 log dir (join2.log/join3.log absent) --"
        $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $tmp 2>&1
        $exitCode = $LASTEXITCODE
        $output | ForEach-Object { Write-Host "  | $_" }
        $outText = ($output -join "`n")

        Check "analyze_run4.ps1 FAILS LOUD (nonzero exit) on a 2-of-4 log dir" ($exitCode -ne 0)
        Check "analyze_run4.ps1 reports RESULT: FAIL on a 2-of-4 log dir" ($outText -match '(?m)^RESULT: FAIL\s*$')
        Check "the failure is attributed to the logs_present guard, not some other gate" ($outText -match 'logs_present')
        Check "no verdict.json falsely claims pass on the 2-of-4 dir" `
            (-not (Test-Path (Join-Path $tmp "verdict.json")) -or `
             ((Get-Content (Join-Path $tmp "verdict.json") -Raw | ConvertFrom-Json).pass -eq $false))

        # Belt-and-suspenders: exercise Assert-AllLogsPresent directly (the guard
        # FUNCTION, not just the script wrapping it) so a future caller of the
        # function alone (bypassing analyze_run4.ps1) is covered too.
        Write-Host "-- Assert-AllLogsPresent directly --"
        Import-Module (Join-Path $scriptsRoot "CoopOraclesN.psm1") -Force
        Reset-GateResults
        $logs = [ordered]@{
            host  = (Join-Path $tmp "host.log")
            join1 = (Join-Path $tmp "join1.log")
            join2 = (Join-Path $tmp "join2.log")
            join3 = (Join-Path $tmp "join3.log")
        }
        $guardStatus = Assert-AllLogsPresent -Logs $logs -ExpectedCount 4
        Check "Assert-AllLogsPresent returns FAIL (never SKIP) on a 2-of-4 log set" ($guardStatus -eq "FAIL")

        # NEGATIVE control: once all 4 logs are genuinely present and non-empty,
        # the guard must NOT fire - otherwise the FAILs above would prove
        # nothing (an unconditionally-failing guard is not a guard).
        New-StubLog -Path (Join-Path $tmp "join2.log")
        New-StubLog -Path (Join-Path $tmp "join3.log")
        Reset-GateResults
        $guardStatus2 = Assert-AllLogsPresent -Logs $logs -ExpectedCount 4
        Check "Assert-AllLogsPresent PASSES once all 4 logs are present and non-empty (negative control)" ($guardStatus2 -eq "PASS")

        Write-Host "-- analyze_run4.ps1 against the now-complete 4-of-4 log dir --"
        $output2 = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $tmp 2>&1
        $exitCode2 = $LASTEXITCODE
        $output2 | ForEach-Object { Write-Host "  | $_" }
        $outText2 = ($output2 -join "`n")
        Check "analyze_run4.ps1 does not fail-loud once all 4 logs are present (negative control)" `
            ($outText2 -notmatch 'logs_present\s+FAIL')

        # ---- Phase 11 plan 02 (TEST-01): N=2/N=3 -ExpectedInstances fixtures --
        # Same Pitfall-5 fail-loud proof, generalized: the guard must fire on
        # an under-count log set AT WHATEVER N was declared (not hardcoded to
        # 4), and must NOT fire once that SAME N's log set is complete -
        # honest, not vacuous-pass, at every N.
        foreach ($n in @(2, 3)) {
            Write-Host "-- N=${n}: analyze_run4.ps1 -ExpectedInstances $n against an under-count log dir --"
            $tmpN = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_magate_selftest_n${n}_" + [System.Guid]::NewGuid().ToString("N"))
            New-Item -ItemType Directory -Force -Path $tmpN | Out-Null
            try {
                # Only the host log - every N>=2 declares at least 1 join, so
                # this is always an under-count set regardless of N.
                New-StubLog -Path (Join-Path $tmpN "host.log")

                $outputN = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $tmpN -ExpectedInstances $n 2>&1
                $exitCodeN = $LASTEXITCODE
                $outputN | ForEach-Object { Write-Host "  | $_" }
                $outTextN = ($outputN -join "`n")
                Check "N=${n}: analyze_run4.ps1 FAILS LOUD (nonzero exit) on an under-count log dir" ($exitCodeN -ne 0)
                Check "N=${n}: analyze_run4.ps1 reports RESULT: FAIL on an under-count log dir" ($outTextN -match '(?m)^RESULT: FAIL\s*$')
                Check "N=${n}: the failure is attributed to the logs_present guard" ($outTextN -match 'logs_present')

                # Complete the log set (host + join1..join(n-1)) - negative
                # control: the guard must not ALSO fire on a genuinely
                # complete N-sized set (an unconditionally-failing guard
                # proves nothing).
                for ($i = 1; $i -lt $n; $i++) { New-StubLog -Path (Join-Path $tmpN "join$i.log") }
                $outputN2 = & powershell -NoProfile -ExecutionPolicy Bypass -File $analyzeScript -RunDir $tmpN -ExpectedInstances $n 2>&1
                $outTextN2 = ($outputN2 -join "`n")
                Check "N=${n}: analyze_run4.ps1 does not fail-loud once the N-sized log set is complete (negative control)" `
                    ($outTextN2 -notmatch 'logs_present\s+FAIL')
                Check "N=${n}: analyze_run4.ps1 judged exactly N=$n (banner reports N=$n)" ($outTextN2 -match "\(N=$n\)")
            } finally {
                Remove-Item -Recurse -Force $tmpN -ErrorAction SilentlyContinue
            }
        }
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("MilestoneAGate: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
