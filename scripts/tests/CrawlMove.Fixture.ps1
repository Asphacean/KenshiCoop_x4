<#
.SYNOPSIS
  Zero-game fixture for the crawl_move oracle (protocol 53): proves Test-CrawlMove
  PASSES on a synthetic log pair where the prone posture and the crippled flag
  cross, and FAILS on the pre-fix shape where the owner crawls and the copy walks
  upright to the same place.

.DESCRIPTION
  The FAIL fixture is the point. A position-only gate passes the pre-fix bug (the
  driven copy is walked to the right coordinates, just standing up), so the
  fixture feeds exactly that log - matching positions, owner prone=2 crip=1, copy
  prone=0 crip=0 - and asserts the gate reports FAIL. A gate that cannot be shown
  to fail on the bug it exists for is not evidence.

  Exit code = number of failed assertions (0 = PASS), matching prototest and
  Contract.Tests so it can gate a commit.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot "scripts\CoopOracles.psm1") -Force

$script:failed = 0
function Check {
    param([string]$Name, [bool]$Ok)
    if ($Ok) { Write-Host "  ok   $Name" }
    else { Write-Host "  FAIL $Name"; $script:failed++ }
}

# Build one side's log. $Cut = emit the amputate marker for window $Tag.
# $Prone / $Crip = what THIS side reads for the subject. $Dx = position offset
# applied to this side's samples (0 = perfect tracking).
function New-CrawlLog {
    param([string]$Path, [string]$Tag, [bool]$Cut,
          [int]$Prone, [int]$Crip, [double]$Dx = 0.0, [int]$Mv = 1,
          [string]$Path2 = 'advance', [int]$Hk = 1)
    $lines = @(
        "[10:00:00.000] KenshiCoop: gameplay started",
        "[10:00:00.100] CLOCKSYNC offset=0 rtt=10"
    )
    # Pre-cut baseline: upright and healthy on both sides. Span is measured from
    # the cut onward, so the baseline always advances regardless of $Path2.
    for ($i = 0; $i -lt 6; $i++) {
        $ms = 1000 + $i * 500
        $lines += (New-CrawlSample -Ms $ms -Prone 0 -Crip 0 -Dx $Dx -Mv 0 -Path2 'advance')
        $lines += (New-CrawlProbe -Ms $ms -X (100.0 + ($ms / 500.0) + $Dx) -Hk 1)
    }
    if ($Cut) {
        $lines += "[10:00:04.000] SCENARIO CRAWL $Tag amputate issued hand=7,3 limb=2 ok=1"
    }
    # Post-cut: the state under test, moving, for 20 s.
    for ($i = 0; $i -lt 40; $i++) {
        $ms = 4500 + $i * 500
        $s = (New-CrawlSample -Ms $ms -Prone $Prone -Crip $Crip -Dx $Dx -Mv $Mv -Path2 $Path2)
        $lines += $s
        # Keep the probe's mv consistent with the sample's pos: both read the same
        # CharMovement::pos in the real logs.
        $px = [double]([regex]::Match($s, 'pos=(-?[\d\.]+),').Groups[1].Value)
        $lines += (New-CrawlProbe -Ms $ms -X $px -Hk $Hk)
    }
    $lines += "[10:00:30.000] SCENARIO RESULT PASS"
    $lines | Set-Content -Path $Path -Encoding UTF8
}

# $Path2 = the shape of this side's trajectory:
#   advance - x climbs 1 u per sample (a body that goes somewhere)
#   frozen  - x pinned at its first post-cut value (the copy that crawls in place)
#   osc     - x oscillates +/-10 u about that value, so the body MOVES yet never
#             ends up far from where it started (the real crawl_move motion)
function New-CrawlSample {
    param([int]$Ms, [int]$Prone, [int]$Crip, [double]$Dx, [int]$Mv,
          [string]$Path2 = 'advance')
    $sec = [int][Math]::Floor($Ms / 1000)
    $stamp = "10:00:{0:00}.{1:000}" -f $sec, ($Ms % 1000)
    $x0 = 109.0   # x of the first post-cut sample under 'advance'
    switch ($Path2) {
        'frozen' { $x = $x0 + $Dx }
        'osc' {
            $ph = ($Ms - 4500) % 8000
            $leg = $ph / 4000.0
            if ($leg -le 1.0) { $x = $x0 + 10.0 * $leg }
            else              { $x = $x0 + 10.0 * (2.0 - $leg) }
            $x += $Dx
        }
        default { $x = 100.0 + ($Ms / 500.0) + $Dx }
    }
    $bs = 8 + ($Prone -shl 9)   # BODY_CRAWL | prone field
    # LOCALE-SAFETY: this machine's culture (uk-UA) renders {0:F2}/-f as
    # comma-decimal ("109,00"), which never matches Get-CrawlSeries' dot-
    # decimal regex. InvariantCulture keeps the emitted line locale-stable.
    $xStr = $x.ToString('F2', [System.Globalization.CultureInfo]::InvariantCulture)
    return ("[$stamp] SCENARIO CRAWL hand=7,3 t=$Ms prone=$Prone crip=$Crip " +
            "pos=$xStr,5.00,50.00 bs=$bs mv=$Mv spd=1.20")
}

# The physics witness beside each sample. $Hk=0 = the body lost its physics
# character and never got it back, so the MODEL is stranded even though every
# position field below tracks perfectly.
function New-CrawlProbe {
    param([int]$Ms, [double]$X, [int]$Hk)
    $sec = [int][Math]::Floor($Ms / 1000)
    $stamp = "10:00:{0:00}.{1:000}" -f $sec, ($Ms % 1000)
    # LOCALE-SAFETY: same InvariantCulture requirement as New-CrawlSample above.
    $ic = [System.Globalization.CultureInfo]::InvariantCulture
    $xStr = $X.ToString('F2', $ic)
    $x10Str = ($X / 10.0).ToString('F2', $ic)
    return ("[$stamp] SCENARIO CRAWLPROBE hand=7,3 t=$Ms " +
            "mv=$xStr,5.00,50.00 hk=${Hk}:$x10Str,0.50,5.00 " +
            "mode=0 ao=0 tav=0.00,0.00,0.00 mot=1.00,0.00,1.00")
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_crawl_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    # ---- 1. FIXED shape: the copy reaches the owner's posture and cause. ----
    # Only window A is exercised (the host owns the subject); B's absent marker is
    # a FAIL by design, so the fixture drives A on both sides of the pair by
    # giving the join the B marker too, with its own subject in the same state.
    Write-Host "== crawl_move: posture + cause cross (expect PASS) =="
    $hOk = Join-Path $tmp "host_ok.log"; $jOk = Join-Path $tmp "join_ok.log"
    New-CrawlLog -Path $hOk -Tag "A" -Cut $true  -Prone 2 -Crip 1
    New-CrawlLog -Path $jOk -Tag "B" -Cut $true  -Prone 2 -Crip 1
    Reset-GateResults
    $st = Test-CrawlMove -HostFile $hOk -JoinFile $jOk
    Check "fixed shape PASSes" ($st -eq 'PASS')
    $g = Get-GateResults | Where-Object { $_.gate -eq 'crawl_move' } | Select-Object -First 1
    Check "records the owner posture" ($null -ne $g -and $g.metrics['AOwnerProne'] -eq 2)
    Check "records a crawl gap" ($null -ne $g -and $g.metrics.ContainsKey('AGapMed'))

    # ---- 2. PRE-FIX shape: same positions, copy upright and not crippled. ----
    # This is the bug a position gate cannot see.
    Write-Host "== crawl_move: copy walks upright to the right place (expect FAIL) =="
    $hBad = Join-Path $tmp "host_bad.log"; $jBad = Join-Path $tmp "join_bad.log"
    New-CrawlLog -Path $hBad -Tag "A" -Cut $true -Prone 2 -Crip 1
    New-CrawlLog -Path $jBad -Tag "B" -Cut $true -Prone 0 -Crip 0
    Reset-GateResults
    $st2 = Test-CrawlMove -HostFile $hBad -JoinFile $jBad
    Check "pre-fix shape FAILs" ($st2 -eq 'FAIL')
    $g2 = Get-GateResults | Where-Object { $_.gate -eq 'crawl_move' } | Select-Object -First 1
    Check "failure names the posture" ($null -ne $g2 -and $g2.detail -match 'posture does not cross')
    Check "failure names the cause" ($null -ne $g2 -and $g2.detail -match 'cause does not cross')

    # ---- 3. Position drift: posture crosses but the copy is left behind. ----
    Write-Host "== crawl_move: posture crosses, position drifts (expect FAIL) =="
    $hDrift = Join-Path $tmp "host_drift.log"; $jDrift = Join-Path $tmp "join_drift.log"
    New-CrawlLog -Path $hDrift -Tag "A" -Cut $true -Prone 2 -Crip 1
    New-CrawlLog -Path $jDrift -Tag "B" -Cut $true -Prone 2 -Crip 1 -Dx 40.0
    Reset-GateResults
    $st3 = Test-CrawlMove -HostFile $hDrift -JoinFile $jDrift
    Check "drifting copy FAILs" ($st3 -eq 'FAIL')
    $g3 = Get-GateResults | Where-Object { $_.gate -eq 'crawl_move' } | Select-Object -First 1
    Check "failure names the gap" ($null -ne $g3 -and $g3.detail -match 'median crawl gap')

    # ---- 5. Crawls in place: right posture, small gap, no travel. ----
    # The shape the gate MISSED (run 20260805_153425 passed at gap=3.68u with the
    # copy byte-frozen). The owner oscillates, so it never gets far from its own
    # start and a copy parked there scores a small gap - the gap check alone cannot
    # tell "tracking" from "not moving at all".
    Write-Host "== crawl_move: copy crawls in place, small gap (expect FAIL) =="
    $hFrz = Join-Path $tmp "host_frz.log"; $jFrz = Join-Path $tmp "join_frz.log"
    New-CrawlLog -Path $hFrz -Tag "A" -Cut $true -Prone 2 -Crip 1 -Path2 'osc'
    New-CrawlLog -Path $jFrz -Tag "B" -Cut $true -Prone 2 -Crip 1 -Path2 'frozen'
    Reset-GateResults
    $st5 = Test-CrawlMove -HostFile $hFrz -JoinFile $jFrz
    Check "frozen copy FAILs" ($st5 -eq 'FAIL')
    $g5 = Get-GateResults | Where-Object { $_.gate -eq 'crawl_move' } | Select-Object -First 1
    Check "failure names the travel" ($null -ne $g5 -and $g5.detail -match 'crawling in place')
    # The point of the fixture: the OLD checks all passed on this log.
    Check "posture still crossed"  ($null -ne $g5 -and $g5.detail -notmatch 'posture does not cross')
    Check "gap was within bounds"  ($null -ne $g5 -and $g5.detail -notmatch 'median crawl gap')

    # ---- 6. Frozen MODEL: every position field tracks, mesh left behind. ----
    # The shape no position check can see (run 20260805_154639): the copy's
    # getPosition follows the owner to ~0.2u and the nametag moves with it, while
    # the model stays at the collapse spot because the copy has no physics body.
    Write-Host "== crawl_move: position tracks but the model cannot render there (expect FAIL) =="
    $hPhys = Join-Path $tmp "host_phys.log"; $jPhys = Join-Path $tmp "join_phys.log"
    New-CrawlLog -Path $hPhys -Tag "A" -Cut $true -Prone 2 -Crip 1
    New-CrawlLog -Path $jPhys -Tag "B" -Cut $true -Prone 2 -Crip 1 -Hk 0
    Reset-GateResults
    $st6 = Test-CrawlMove -HostFile $hPhys -JoinFile $jPhys
    Check "physics-less copy FAILs" ($st6 -eq 'FAIL')
    $g6 = Get-GateResults | Where-Object { $_.gate -eq 'crawl_move' } | Select-Object -First 1
    Check "failure names the physics body" ($null -ne $g6 -and $g6.detail -match 'no physics body')
    # Everything a position gate can measure was perfect on this log.
    Check "posture crossed anyway"  ($null -ne $g6 -and $g6.detail -notmatch 'posture does not cross')
    Check "gap fine anyway"         ($null -ne $g6 -and $g6.detail -notmatch 'median crawl gap')
    Check "travel fine anyway"      ($null -ne $g6 -and $g6.detail -notmatch 'crawling in place')

    # ---- 4. No signal: the engine never crippled the subject. ----
    Write-Host "== crawl_move: owner never crippled (expect SKIP) =="
    $hNo = Join-Path $tmp "host_no.log"; $jNo = Join-Path $tmp "join_no.log"
    New-CrawlLog -Path $hNo -Tag "A" -Cut $true -Prone 0 -Crip 0
    New-CrawlLog -Path $jNo -Tag "B" -Cut $true -Prone 0 -Crip 0
    Reset-GateResults
    $st4 = Test-CrawlMove -HostFile $hNo -JoinFile $jNo
    Check "no-signal run SKIPs" ($st4 -eq 'SKIP')
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failed -eq 0) { Write-Host "crawl_move fixture: all checks passed - PASS" }
else { Write-Host "crawl_move fixture: $script:failed check(s) FAILED" }
exit $script:failed
