<#
.SYNOPSIS
  Headless guard for Phase 12's N=3 simultaneous-join census-race reproducer
  (Phase 12 Plan 01, CENSUS-03 + success criterion 3).

.DESCRIPTION
  Needs NO live game launch. Three groups of checks:

  Group A (rig shape): the committed N=3 template
  (scripts\local.rig.n3.example.json) declares 3 instances with every join
  undeferred and no netsim environment key; the retired deferred-join variant
  (scripts\local.rig.n3.deferred-join.example.json) still exists and carries
  exactly one deferred join, so the retired shape stays available and stays
  distinguishable from standard coverage.

  Group B (anti-escape pins, success criterion 3): reads the LIVE VALUES
  (not just file bytes) of the census oracle's convergence/pairing/gap
  defaults (scripts\analyze_wnpc_diff4.ps1), the milestone_a_gate tolerance
  and gating list (read through the scenario manifest, scripts\scenarios.psd1),
  the join launch stagger default (scripts\run_test4.ps1), and the three
  netsim knob defaults (src\plugin\core\Config.cpp) - and asserts each still
  equals the value recorded when Phase 12 began. Two levers here are already
  PROVEN to mask the divergence this phase exists to reproduce (a deferred
  join, injected netsim delay); this group is what makes drifting either one
  a headless FAIL instead of a silent, undetected escape.

  Group C (command integrity): scripts\repro_census_n3.ps1 exists and its
  structural guard (Task 1) actually fires - invoking it against the
  deferred-join variant with -SkipBuild -SkipDeploy must refuse before
  launching anything. This suite never launches a real game (that proof
  lives in docs\PHASE_12_GATE.md, produced by a live run of the STANDARD
  rig - never exercised here).

  Exit code = number of failed checks (0 = PASS), matching
  MilestoneAGate.Tests.ps1's/EntityRelay.Tests.ps1's convention so
  verify.ps1/regress.ps1 can sum it.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\tests\CensusRepro.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                         # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                       # repo root

# ---- tiny assert harness (verbatim shape from MilestoneAGate.Tests.ps1) -------
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}
function CheckPinned {
    param([string]$Name, $Expected, $Found)
    if ($Expected -eq $Found) {
        $script:Pass++
        Write-Host "  ok   $Name (value=$Found)"
    } else {
        $script:Fail++
        Write-Host "  FAIL $Name - expected '$Expected', found '$Found'. Phase 12 forbids changing this value (success criterion 3)."
    }
}

Write-Host "== CensusRepro: N=3 rig shape + anti-escape pins + command integrity =="

# ---- Group A: rig shape (CENSUS-03) --------------------------------------------
Write-Host ""
Write-Host "-- Group A: rig shape --"
$standardPath = Join-Path $scriptsRoot "local.rig.n3.example.json"
$deferredPath = Join-Path $scriptsRoot "local.rig.n3.deferred-join.example.json"

Check "committed standard N=3 template exists" (Test-Path $standardPath)
if (Test-Path $standardPath) {
    $std = Get-Content -Raw -Path $standardPath | ConvertFrom-Json
    Check "standard template declares exactly 3 instances" ($std.instances.Count -eq 3)

    $deferredJoins = @($std.instances | Where-Object { $_.role -eq 'join' -and $null -ne $_.reconnectAtSec })
    Check "standard template: every join instance's deferred-launch property is null" ($deferredJoins.Count -eq 0)

    $netsimHit = $false
    foreach ($inst in $std.instances) {
        if ($null -ne $inst.env) {
            foreach ($p in $inst.env.PSObject.Properties) {
                if ($p.Name -like 'KENSHICOOP_NETSIM*') { $netsimHit = $true }
            }
        }
    }
    Check "standard template declares no netsim environment key" (-not $netsimHit)
}

Check "retired deferred-join variant file exists" (Test-Path $deferredPath)
if (Test-Path $deferredPath) {
    $def = Get-Content -Raw -Path $deferredPath | ConvertFrom-Json
    Check "deferred-join variant declares exactly 3 instances" ($def.instances.Count -eq 3)
    $deferredJoinsVariant = @($def.instances | Where-Object { $_.role -eq 'join' -and $null -ne $_.reconnectAtSec })
    Check "deferred-join variant: exactly one join carries a non-null deferred launch" ($deferredJoinsVariant.Count -eq 1)
}

# ---- Group B: anti-escape pins (success criterion 3) ---------------------------
Write-Host ""
Write-Host "-- Group B: anti-escape pins (values, not just file bytes) --"

# B1: census oracle convergence/pairing/gap defaults (analyze_wnpc_diff4.ps1
# param block).
$oraclePath = Join-Path $scriptsRoot "analyze_wnpc_diff4.ps1"
Check "scripts\analyze_wnpc_diff4.ps1 exists" (Test-Path $oraclePath)
if (Test-Path $oraclePath) {
    $oracleText = Get-Content -Raw -Path $oraclePath

    function Get-IntParamDefault {
        param([string]$Text, [string]$Name)
        $m = [regex]::Match($Text, '\[int\]\$' + [regex]::Escape($Name) + '\s*=\s*(\d+)')
        if (-not $m.Success) { return $null }
        return [int]$m.Groups[1].Value
    }

    $gapMs = Get-IntParamDefault -Text $oracleText -Name "GapMs"
    CheckPinned "analyze_wnpc_diff4.ps1 dump-gap default (GapMs)" 1000 $gapMs
    $pairTolMs = Get-IntParamDefault -Text $oracleText -Name "PairTolMs"
    CheckPinned "analyze_wnpc_diff4.ps1 pairing-window default (PairTolMs)" 4000 $pairTolMs
    $convWin = Get-IntParamDefault -Text $oracleText -Name "ConvergenceWindowSec"
    CheckPinned "analyze_wnpc_diff4.ps1 convergence-window default (ConvergenceWindowSec)" 150 $convWin
}

# B2: milestone_a_gate tolerance + gating list, read through the scenario
# manifest (not regex - the manifest is the single declarative source of
# truth CoopOracles.psm1's Get-ScenarioManifest already reads).
Import-Module (Join-Path $scriptsRoot "CoopOracles.psm1") -Force
$manifest = Get-ScenarioManifest
$mag = $manifest.Scenarios['milestone_a_gate']
Check "milestone_a_gate entry resolves through the scenario manifest" ($null -ne $mag)
if ($null -ne $mag) {
    CheckPinned "milestone_a_gate Tolerance default" ([double]6.0) ([double]$mag['Tolerance'])
    $expectedGating = @('census_convergence', 'disconnect_isolation', 'driven_only_movement', 'desync_convergence', 'clean_exit')
    $actualGating = @($mag['Gating'])
    $gatingSame = ($actualGating.Count -eq $expectedGating.Count)
    if ($gatingSame) {
        for ($i = 0; $i -lt $expectedGating.Count; $i++) {
            if ("$($actualGating[$i])" -ne $expectedGating[$i]) { $gatingSame = $false }
        }
    }
    if ($gatingSame) {
        $script:Pass++
        Write-Host "  ok   milestone_a_gate Gating list unchanged ($($actualGating -join ', '))"
    } else {
        $script:Fail++
        Write-Host "  FAIL milestone_a_gate Gating list changed - expected [$($expectedGating -join ', ')], found [$($actualGating -join ', ')]. Phase 12 forbids changing scripts\scenarios.psd1's milestone_a_gate entry."
    }
}

# B3: join launch stagger default (scripts\run_test4.ps1).
$runPath = Join-Path $scriptsRoot "run_test4.ps1"
Check "scripts\run_test4.ps1 exists" (Test-Path $runPath)
if (Test-Path $runPath) {
    $runText = Get-Content -Raw -Path $runPath
    $staggerM = [regex]::Match($runText, '\[int\]\$JoinDelaySec\s*=\s*(\d+)')
    $stagger = if ($staggerM.Success) { [int]$staggerM.Groups[1].Value } else { $null }
    CheckPinned "run_test4.ps1 join launch stagger default (JoinDelaySec)" 8 $stagger
}

# B4: netsim knob defaults (src\plugin\core\Config.cpp envOr lines) - the
# second known masking lever, at its SOURCE rather than a rig file.
$configPath = Join-Path $repoRoot "src\plugin\core\Config.cpp"
Check "src\plugin\core\Config.cpp exists" (Test-Path $configPath)
if (Test-Path $configPath) {
    $configText = Get-Content -Raw -Path $configPath

    function Get-EnvOrDefault {
        param([string]$Text, [string]$EnvName)
        $pattern = 'envOr\("' + [regex]::Escape($EnvName) + '",\s*"(\d+)"\)'
        $m = [regex]::Match($Text, $pattern)
        if (-not $m.Success) { return $null }
        return [int]$m.Groups[1].Value
    }

    $delayDefault  = Get-EnvOrDefault -Text $configText -EnvName "KENSHICOOP_NETSIM_DELAY_MS"
    CheckPinned "Config.cpp KENSHICOOP_NETSIM_DELAY_MS default" 0 $delayDefault
    $jitterDefault = Get-EnvOrDefault -Text $configText -EnvName "KENSHICOOP_NETSIM_JITTER_MS"
    CheckPinned "Config.cpp KENSHICOOP_NETSIM_JITTER_MS default" 0 $jitterDefault
    $lossDefault   = Get-EnvOrDefault -Text $configText -EnvName "KENSHICOOP_NETSIM_LOSS_PCT"
    CheckPinned "Config.cpp KENSHICOOP_NETSIM_LOSS_PCT default" 0 $lossDefault
}

# ---- Group C: command integrity -------------------------------------------------
Write-Host ""
Write-Host "-- Group C: command integrity --"
$reproScript = Join-Path $scriptsRoot "repro_census_n3.ps1"
Check "scripts\repro_census_n3.ps1 exists" (Test-Path $reproScript)

if ((Test-Path $reproScript) -and (Test-Path $deferredPath)) {
    # Task 1's structural guard must refuse a masked (deferred-join) rig
    # BEFORE launching anything - no build, no deploy, no game process. This
    # invokes the reproducer as a child process (never dot-sourced) so a
    # "throw" inside it cannot abort THIS suite; only the child's own exit
    # code and any wrapper-level exception are observed.
    $guardExit = 0
    $guardThrew = $false
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $reproScript -RigConfig $deferredPath -SkipBuild -SkipDeploy 2>&1 | Out-Null
        $guardExit = $LASTEXITCODE
    } catch {
        $guardThrew = $true
    }
    Check "repro_census_n3.ps1 refuses the deferred-join rig before launching anything (non-zero exit)" `
        ($guardThrew -or $guardExit -ne 0)
}

# By design, this suite never invokes repro_census_n3.ps1 with the STANDARD
# rig - that would attempt a real game launch, which a headless check must
# never do. The standard rig's own reproduction proof is a live run,
# recorded in docs\PHASE_12_GATE.md, not exercised by this file.

# ---- summary --------------------------------------------------------------------
Write-Host ""
Write-Host ("CensusRepro: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
