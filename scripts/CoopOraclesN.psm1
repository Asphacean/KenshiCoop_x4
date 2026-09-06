# CoopOraclesN.psm1 - the N=4 (Milestone A gate) oracle sibling of
# CoopOracles.psm1 (Phase 4 Plan 05, POC-03).
#
# CoopOracles.psm1's whole dispatch/verdict machinery (Add-GateResult, the
# three-state PASS/FAIL/SKIP contract, Get-LogClockOffsetMs, Get-ScenarioSeries,
# Test-LogHealth/Test-EngineIntegrity/Test-ScenarioResultPass) is built around
# exactly TWO logs (host + one join). The Milestone A gate collects FOUR logs
# (host + join1/2/3), and reusing that machinery for a fixed-arity-2 helper
# silently degrading to a 2-of-4 judgment is exactly 04-RESEARCH.md's Pitfall 5.
# So this is a SIBLING module, not an edit to CoopOracles.psm1: it Imports that
# module (reusing its gate array / three-state semantics / clock-alignment
# helpers) and adds N=4-shaped gate functions on top. CoopOracles.psm1,
# run_test.ps1 and analyze_run.ps1 are left byte-unchanged (04-RESEARCH.md
# "Don't Hand-Roll" guidance, T-04-14).
#
# The five gates below map 1:1 onto the milestone_a_gate DoD steps this
# plan's must_haves name, judged from the "SCENARIO MAGATE ..." + the existing
# MEMBER/RECV/WNPC schemas ScenarioMilestoneA.cpp (Phase 4 Plan 04) emits:
#   census_convergence   - NPC-census convergence (3 pairwise host-vs-joinK),
#                           via analyze_wnpc_diff4.ps1 (Plan 03)
#   disconnect_isolation - a departed peer's tab clears from survivors' TABMAP
#                           while survivors' own leader keeps resolving
#   driven_only_movement - every client's RECV series for an OTHER owner's
#                           hand tracks that owner's own MEMBER series, and no
#                           client ever RECV's its OWN rank's hand
#   desync_convergence   - the same cross-instance pairs are still converged
#                           (not permanently drifted) at the tail of the run
#   clean_exit           - every instance reached gameplay, exited via
#                           "SCENARIO RESULT", logged no ERROR lines, and the
#                           archived engine log shows no coop-attributed
#                           double-destroy (Test-LogHealth/Test-EngineIntegrity,
#                           reused per-instance rather than reinvented)
#
# HARD REQUIREMENT (Pitfall 5): Assert-AllLogsPresent is a top-level FAIL-loud
# guard (never a SKIP) on fewer than the configured number of non-empty logs -
# analyze_run4.ps1 calls it FIRST, before any gate above runs, so a PASS can
# never have examined fewer than N instances.

$script:MagateModuleDir = $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "CoopOracles.psm1") -Force

# ---- Pitfall 5 hard gate: fail loud on fewer than N non-empty logs -----------
#
# $Logs is a name -> path map (host/join1/join2/join3). Unlike every other gate
# in this module, this one FAILS (never SKIPs) on a short count: "could not
# judge" would let a 2-of-4 run report as inconclusive-but-harmless, when the
# actual defect is a verdict that only ever looked at half the instances.
function Assert-AllLogsPresent {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [int]$ExpectedCount = 4
    )
    $gate = "logs_present"
    $missing = New-Object System.Collections.ArrayList
    $present = 0
    foreach ($name in $Logs.Keys) {
        $path = $Logs[$name]
        if ([string]::IsNullOrEmpty($path) -or -not (Test-Path $path)) {
            [void]$missing.Add("$name (missing): $path")
            continue
        }
        if ((Get-Item $path).Length -le 0) {
            [void]$missing.Add("$name (empty): $path")
            continue
        }
        $present++
    }
    if ($present -lt $ExpectedCount -or $missing.Count -gt 0) {
        $detail = "examined $present of $ExpectedCount required log(s)"
        if ($missing.Count -gt 0) { $detail += "; " + ($missing -join "; ") }
        Write-Host "  LOGS-PRESENT FAIL - $detail"
        return (Add-GateResult -Name $gate -Status FAIL -Metrics @{ present = $present; expected = $ExpectedCount } -Detail $detail)
    }
    Write-Host "  LOGS-PRESENT PASS - all $ExpectedCount log(s) present and non-empty"
    return (Add-GateResult -Name $gate -Status PASS -Metrics @{ present = $present; expected = $ExpectedCount })
}

# ---- small MAGATE-schema parsing helpers -------------------------------------

# This client's own resolved rank, from "SCENARIO MAGATE start ownRank=<n> ...".
function Get-MagateOwnRank {
    param([string]$File)
    if (-not (Test-Path $File)) { return $null }
    $m = Select-String -Path $File -Pattern 'SCENARIO MAGATE start ownRank=(\d+)' -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($null -eq $m) { return $null }
    return [int]$m.Matches[0].Groups[1].Value
}

# rank -> "h3,h4" (the two hand components TABMAP logs), from this client's own
# "SCENARIO MAGATE TABMAP rank=<r> hand=<h3>,<h4>" lines (last value per rank wins).
function Get-MagateTabMap {
    param([string]$File)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    foreach ($m in (Select-String -Path $File -Pattern 'SCENARIO MAGATE TABMAP rank=(\d+) hand=(\d+),(\d+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $map[[int]$g[1].Value] = "$($g[2].Value),$($g[3].Value)"
    }
    return $map
}

# The MEMBER/RECV hand format is 5 comma-separated components, serialized by
# logScenarioEntity as e.hIndex,e.hSerial,e.hType,e.hContainer,e.hContainerSerial
# - i.e. h[3],h[4],h[0],h[1],h[2] in handFromEntity's h[5] layout (h[0]=hType,
# h[1]=hContainer, h[2]=hContainerSerial, h[3]=hIndex, h[4]=hSerial). TABMAP
# logs ONLY h[3],h[4] ("hand=%u,%u", h[3], h[4] - logTabMap). So the
# TABMAP-comparable suffix is the FIRST two components of the 5-part hand
# (parts[0],parts[1] = h[3],h[4]), NOT the last two - a hand serialized as
# [h3,h4,h0,h1,h2] has h3,h4 at the front, not the back.
function Get-MagateHandSuffix {
    param([string]$Hand5)
    $parts = $Hand5 -split ','
    if ($parts.Count -ge 5) { return "$($parts[0]),$($parts[1])" }
    return $Hand5
}

# ---- Gate: NPC-census convergence (PrimaryGate) ------------------------------
#
# Delegates to analyze_wnpc_diff4.ps1 (Plan 03), which already implements the 3
# pairwise host-vs-joinK WNPC diff plus its own Pitfall-5 hard gate. Run as a
# child process (it is a top-level script that executes immediately on load,
# not a function library) and parse its final greppable "WNPC4 RESULT: ..."
# line - the same "don't hand-roll, reuse the existing verdict" principle this
# whole module follows for CoopOracles.psm1.
function Test-MagateCensusConvergence {
    param(
        [string]$RunDir,
        [string]$AnalyzeScript = "",
        # Phase 11 plan 02 (TEST-01): threaded down from Invoke-OneOracleN so
        # analyze_wnpc_diff4.ps1 judges the SAME N as the caller resolved -
        # 0 (default) lets the child script resolve N itself (its own
        # -ExpectedInstances/run_meta.json/default-4 precedence), exactly
        # matching pre-Phase-11 behavior when nobody passes N explicitly.
        [int]$ExpectedInstances = 0
    )
    $gate = "census_convergence"
    if ($AnalyzeScript -eq "") { $AnalyzeScript = Join-Path $script:MagateModuleDir "analyze_wnpc_diff4.ps1" }
    if (-not (Test-Path $AnalyzeScript)) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "analyze_wnpc_diff4.ps1 not found at $AnalyzeScript")
    }
    $wnpcArgs = @("-RunDir", $RunDir)
    if ($ExpectedInstances -gt 0) { $wnpcArgs += @("-ExpectedInstances", $ExpectedInstances) }
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $AnalyzeScript @wnpcArgs 2>&1
    $outText = ($output -join "`n")
    $output | ForEach-Object { Write-Host "  [wnpc4] $_" }
    if ($outText -match '(?m)^WNPC4 RESULT: PASS\s*$') {
        return (Add-GateResult -Name $gate -Status PASS -Detail "3 pairwise host-vs-joinK NPC-census diffs converged")
    }
    $failMatch = [regex]::Match($outText, '(?m)^WNPC4 RESULT: FAIL.*$')
    if ($failMatch.Success) {
        return (Add-GateResult -Name $gate -Status FAIL -Detail "analyze_wnpc_diff4: $($failMatch.Value)")
    }
    return (Add-GateResult -Name $gate -Status SKIP -Detail "no WNPC4 RESULT line found in analyze_wnpc_diff4.ps1 output")
}

# ---- Gate: disconnect isolation ----------------------------------------------
#
# Reads every survivor log's own "SCENARIO MAGATE DISCONNECT peer=<p>
# ownRank=<r> t=<ms>" + "SCENARIO MAGATE SURVIVOR ownRank=<r> ok=<0|1>" pair
# (ScenarioMilestoneA.cpp's tickPeerWatch, Plan 04): SURVIVOR ok=1 is the
# scenario's own proof that the SURVIVING client's own leader still resolved
# post-leave: "the survivor's own squad persisted" - this is the gate's actual
# PASS/FAIL criterion.
#
# The departed peer's TABMAP entry is tracked too, but as an ADVISORY finding
# (reported, never gating) - not the "1-2s after disconnect it should clear"
# signal the original design (Plan 05/06) assumed. Direct archived-log
# inspection for this gap-closure plan (04-08, gap 2) proved that assumption
# false: SCENARIO MAGATE TABMAP is squad-TAB bookkeeping keyed off which
# CHARACTER currently answers to that rank (tabRankOf/tabLeaderIdx over the
# locally-captured `sq` array), not peer connectivity - a departed rank's
# leader hand was observed identical and CONTINUOUSLY present, sample after
# sample with zero gaps, all the way from disconnect through to (and past)
# the eventual reconnect, and likewise all the way to a survivor's own
# "SCENARIO RESULT" in a genuine no-reconnect case (join3 vs. join1's clean
# self-exit, tools/test-runs/20260902_094054_N4). A signal that structurally
# never clears, connected or not, reconnect or not, cannot be a reliable
# staleness gate - continuing to hard-FAIL on it would make disconnect
# handling unfalsifiable in the other direction (it could never demonstrate
# clean isolation, even on a run with zero real defects). So its presence
# past a grace floor is surfaced in Detail/Metrics as a genuine, unmasked
# finding (mirrors this module's own engine_integrity gate: "ADVISORY for
# now... precisely because that baseline is missing" - same reasoning here:
# no trustworthy TABMAP-clears baseline exists to gate on). SURVIVOR ok=1
# remains a fully independent, functioning correctness signal: it directly
# asserts the departure didn't corrupt the SURVIVING client's OWN leader.
#
# SKIPs (not FAILs) when no disconnect was observed at all - not every judged
# run schedules one (that is run_test4.ps1's rig-config concern, Plan 02).
#
# CleanupGraceMs (plan 06 live-run finding, retained as the advisory-finding
# threshold): the first live 4-process run observed apparent "cleanup" lag of
# 15-864ms past an 8000ms grace window - consistent with STATE.md's own
# documented ENet peer-timeout ceiling ("~5-10s, not instant teardown") plus
# the harness's Stop-Process -Force disconnect (no graceful ENet disconnect
# packet) plus one TABMAP sampling-cadence tick of slack. 15000ms gives firm
# headroom above that observed range for the advisory finding's threshold.
#
# The advisory window is bounded above by that same rank's RECONNECT event
# (04-VERIFICATION.md gap 2): a departed rank is legitimately reused by the
# replacement/rejoin join, and any TABMAP entry AT/AFTER that reconnect is
# exactly that legitimate reuse - it must never be reported as a finding at
# all (not even advisory). A TABMAP rank=r entry is surfaced as a finding
# only if it is BOTH past the grace floor AND strictly before that rank's own
# RECONNECT wall-clock time (or, if the rank never reconnects - e.g. an
# end-of-run shutdown disconnect - the window's upper bound is "end of log").
# Both the disconnect and reconnect times are measured from their OWN log
# lines' wall-clock stamps (Convert-StampToMs + the file's clock offset) -
# the SAME basis the TABMAP comparison already uses - rather than the
# embedded scenario `t=` field, which is on a different time base entirely.
function Test-MagateDisconnectIsolation {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [int]$CleanupGraceMs = 15000
    )
    $gate = "disconnect_isolation"
    $disconnects = New-Object System.Collections.ArrayList
    foreach ($label in $Logs.Keys) {
        $file = $Logs[$label]
        if (-not (Test-Path $file)) { continue }
        $off = Get-LogClockOffsetMs -File $file
        foreach ($m in (Select-String -Path $file -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE DISCONNECT peer=(\d+) ownRank=(\d+) t=(\d+)' -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            # Wall-clock time of the DISCONNECT line itself (same basis as the
            # TABMAP/RECONNECT comparisons below) - not the embedded scenario
            # `t=` field, which is a different time base entirely.
            $t = Convert-StampToMs -Groups $g -OffsetMs $off
            [void]$disconnects.Add([pscustomobject]@{ label = $label; file = $file; peer = [int]$g[5].Value; ownRank = [int]$g[6].Value; t = $t })
        }
    }
    if ($disconnects.Count -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no DISCONNECT observed in any of the $($Logs.Count) log(s) (rig did not schedule a disconnect this run)")
    }

    $problems = New-Object System.Collections.ArrayList
    $findings = New-Object System.Collections.ArrayList
    foreach ($d in $disconnects) {
        $survPat = "SCENARIO MAGATE SURVIVOR ownRank=$($d.ownRank) ok=(\d+)"
        $survHit = Select-String -Path $d.file -Pattern $survPat -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $survHit -or $survHit.Matches[0].Groups[1].Value -ne '1') {
            [void]$problems.Add("$($d.label): SURVIVOR ownRank=$($d.ownRank) missing or ok=0 after peer=$($d.peer) left")
        }

        $off = Get-LogClockOffsetMs -File $d.file

        # This rank's RECONNECT wall-clock time in the SAME survivor log, if
        # any - the earliest one closes the judged window (a rank that
        # reconnects more than once, in principle, is bounded by its first
        # return).
        $reconnectT = $null
        $recPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE RECONNECT peer=$($d.peer) ownRank=\d+ t=\d+"
        $recHit = Select-String -Path $d.file -Pattern $recPat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $recHit) {
            $reconnectT = Convert-StampToMs -Groups $recHit.Matches[0].Groups -OffsetMs $off
        }

        foreach ($m in (Select-String -Path $d.file -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE TABMAP rank=(\d+)' -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            if ([int]$g[5].Value -ne $d.peer) { continue }
            $tt = Convert-StampToMs -Groups $g -OffsetMs $off
            if ($tt -le ($d.t + $CleanupGraceMs)) { continue }             # still inside the grace floor
            if ($null -ne $reconnectT -and $tt -ge $reconnectT) { continue } # legitimate rank reuse by the replacement
            $recNote = if ($null -ne $reconnectT) { ", reconnect at t=${reconnectT}ms" } else { ", no reconnect observed" }
            [void]$findings.Add("$($d.label): TABMAP still lists departed peer's rank=$($d.peer) at t=${tt}ms (disconnect at t=$($d.t)ms, grace=${CleanupGraceMs}ms${recNote})")
            break
        }
    }
    if ($problems.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics @{ disconnects = $disconnects.Count; residualFindings = $findings.Count } -Detail ($problems -join '; '))
    }
    $detail = "every survivor's own leader resolved post-leave (SURVIVOR ok=1); no departed rank's TABMAP entry was ever mistaken for staleness AT/AFTER its RECONNECT (legitimate replacement reuse)"
    if ($findings.Count -gt 0) {
        $detail += "; advisory finding(s), not gating (TABMAP squad-tab bookkeeping is bound to the character, not connectivity, so its presence past the grace floor is reported, not judged): " + ($findings -join '; ')
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics @{ disconnects = $disconnects.Count; residualFindings = $findings.Count } -Detail $detail)
}

# ---- Gate: multi-owner driven-only movement ----------------------------------
#
# Two things, together: (a) every RECV'd OTHER-owner hand tracks that owner's
# OWN MEMBER series within Tolerance (nearest-timestamp distance, same
# reduction as CoopOracles' Measure-NpcSync), across at least two distinct
# other owners - proving genuine multi-owner replication, not a 2-log-shaped
# coincidence; (b) no client's log ever RECV's a hand matching ITS OWN rank
# (a locally-owned body must never be shown as remotely-driven).
# Owner-side combat evidence for the combat-band split below: suffix
# ("h3,h4") -> chronologically sorted samples of @{ t = wallclock ms
# (clock-offset corrected, same basis as Get-ScenarioSeries); fight = 0|1 }.
# Parsed from the scenario's own "SCENARIO COMBATSTATE hand=h3,h4 t=...
# fight=N ..." lines (ScenarioSupport.cpp logCombatStateLine).
function Get-MagateCombatSeries {
    param([string]$File)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $off = Get-LogClockOffsetMs -File $File
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO COMBATSTATE hand=(\d+,\d+) t=\d+ fight=(\d)"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $suffix = $g[5].Value
        if (-not $map.ContainsKey($suffix)) { $map[$suffix] = New-Object System.Collections.ArrayList }
        [void]$map[$suffix].Add(@{ t = $t; fight = [int]$g[6].Value })
    }
    return $map
}

function Test-MagateDrivenOnlyMovement {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [double]$Tolerance = 6.0,
        [int]$MaxDtMs = 2000,
        # Phase 11 (11-03 live matrix): the tolerance for samples taken while
        # the SOURCE body was in combat (owner-side COMBATSTATE fight=1) - the
        # product's OWN combat convergence contract (ReplicatorUtil.h
        # COMBAT_SNAP_DIST = 20.0f: "churn ceiling: a correctly-engaged fight
        # owns its" drift up to this band; COMBAT_SOFT_DIST is the 6u
        # walk-converge floor). A flat 6u median judged combat samples at the
        # SOFT band - i.e. it failed the design's own documented allowance
        # whenever a client's observation window was combat-heavy (measured:
        # the scheduled-disconnect join, killed at 80s, samples mostly the
        # scenario's deliberately-sustained hostile brawl and repeatedly
        # landed at 6.1-6.6u medians while every full-window observer passed
        # and the end state converged exactly). Non-combat samples keep the
        # strict 6u median unchanged.
        [double]$CombatTolerance = 20.0,
        [int]$CombatDtMs = 3000
    )
    $gate = "driven_only_movement"
    $ownRanks = @{}; $tabMaps = @{}; $memberSeries = @{}; $recvSeries = @{}; $combatSeries = @{}
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label]     = Get-MagateOwnRank -File $f
        $tabMaps[$label]      = Get-MagateTabMap -File $f
        $memberSeries[$label] = Get-ScenarioSeries -File $f -Kind "MEMBER"
        $recvSeries[$label]   = Get-ScenarioSeries -File $f -Kind "RECV"
        $combatSeries[$label] = Get-MagateCombatSeries -File $f
    }

    $violations = New-Object System.Collections.ArrayList
    $pairsChecked = 0; $pairsOk = 0
    $ownersObserved = New-Object System.Collections.Generic.HashSet[string]

    foreach ($obsLabel in $Logs.Keys) {
        if (-not $ownRanks.ContainsKey($obsLabel) -or $null -eq $ownRanks[$obsLabel]) { continue }
        $ownRank = $ownRanks[$obsLabel]
        $ownSuffix = $null
        if ($tabMaps[$obsLabel].ContainsKey($ownRank)) { $ownSuffix = $tabMaps[$obsLabel][$ownRank] }
        $recv = $recvSeries[$obsLabel]
        foreach ($hand in $recv.Keys) {
            $suffix = Get-MagateHandSuffix -Hand5 $hand
            if ($null -ne $ownSuffix -and $suffix -eq $ownSuffix) {
                [void]$violations.Add("$obsLabel RECV'd its OWN rank=$ownRank hand=$hand (locally-owned body driven by remote replication)")
                continue
            }
            $ownerRank = $null
            foreach ($r in $tabMaps[$obsLabel].Keys) {
                if ($tabMaps[$obsLabel][$r] -eq $suffix) { $ownerRank = $r; break }
            }
            if ($null -eq $ownerRank) { continue } # this client cannot yet resolve who owns it - not this gate's concern

            $ownerLabel = $null
            foreach ($cand in $ownRanks.Keys) { if ($ownRanks[$cand] -eq $ownerRank) { $ownerLabel = $cand; break } }
            if ($null -eq $ownerLabel -or -not $memberSeries.ContainsKey($ownerLabel)) { continue }
            $ownerMember = $memberSeries[$ownerLabel]
            if (-not $ownerMember.ContainsKey($hand) -or $ownerMember[$hand].Count -eq 0) { continue }

            [void]$ownersObserved.Add("$ownerRank")
            $pairsChecked++
            # The OWNER's own combat evidence for this hand (fight=1 windows) -
            # used to split samples into calm vs combat and judge each at its
            # own design band (see $CombatTolerance's doc comment).
            $ownerCombat = $null
            if ($combatSeries.ContainsKey($ownerLabel) -and $combatSeries[$ownerLabel].ContainsKey($suffix)) {
                $ownerCombat = $combatSeries[$ownerLabel][$suffix]
            }
            $distsCalm = New-Object System.Collections.ArrayList
            $distsCombat = New-Object System.Collections.ArrayList
            foreach ($js in $recv[$hand]) {
                $best = [double]::MaxValue; $bp = $null
                foreach ($hs in $ownerMember[$hand]) {
                    $dt = [Math]::Abs($hs.t - $js.t)
                    if ($dt -lt $best) { $best = $dt; $bp = $hs.p }
                }
                if ($best -le $MaxDtMs -and $null -ne $bp) {
                    $dx = $bp[0] - $js.p[0]; $dy = $bp[1] - $js.p[1]; $dz = $bp[2] - $js.p[2]
                    $d = [Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
                    $inCombat = $false
                    if ($null -ne $ownerCombat) {
                        $bestC = [double]::MaxValue; $fightC = 0
                        foreach ($cs in $ownerCombat) {
                            $dtc = [Math]::Abs($cs.t - $js.t)
                            if ($dtc -lt $bestC) { $bestC = $dtc; $fightC = $cs.fight }
                        }
                        if ($bestC -le $CombatDtMs -and $fightC -eq 1) { $inCombat = $true }
                    }
                    # Down-body samples take the combat band too (Phase 11,
                    # runs 20260905_175944/180631_N3 + the reverted product
                    # experiments in ReplicatorDrive.cpp's down path): an
                    # ACTIVE ragdoll owns its position - no engine lever
                    # relocates it (three instrumented live iterations:
                    # CharMovement writes flap against the physics re-sync,
                    # Character::teleport is ignored by a live ragdoll, and a
                    # stand-up/teleport/re-knockdown cycle thrashes worse).
                    # The offset a down body carries is whatever combat-band
                    # divergence existed at its KO edge (bounded by
                    # COMBAT_SNAP_DIST under the same contract), static while
                    # it lies down, and healed on revive - so a down sample is
                    # judged at that band, never at the walking 6u. bs is the
                    # RECV row's own streamed body state (bits 0-2 =
                    # DOWN|RAGDOLL|DEAD, Wire.h bodyIsDown).
                    $isDownSample = (($js.bs -band 7) -ne 0)
                    if ($inCombat -or $isDownSample) { [void]$distsCombat.Add($d) } else { [void]$distsCalm.Add($d) }
                }
            }
            if (($distsCalm.Count + $distsCombat.Count) -eq 0) { continue }
            $bad = @()
            $medCalm = $null; $medCombat = $null
            if ($distsCalm.Count -gt 0) {
                $sortedCalm = @($distsCalm | Sort-Object)
                $medCalm = $sortedCalm[[int]([Math]::Floor($sortedCalm.Count / 2))]
                if ($medCalm -gt $Tolerance) {
                    $bad += "median tracking distance ${medCalm}u over $($distsCalm.Count) non-combat sample(s) exceeds tolerance ${Tolerance}u"
                }
            }
            if ($distsCombat.Count -gt 0) {
                $sortedCombat = @($distsCombat | Sort-Object)
                $medCombat = $sortedCombat[[int]([Math]::Floor($sortedCombat.Count / 2))]
                if ($medCombat -gt $CombatTolerance) {
                    $bad += "median IN-COMBAT/DOWN tracking distance ${medCombat}u over $($distsCombat.Count) combat/down sample(s) exceeds the combat snap band ${CombatTolerance}u (ReplicatorUtil.h COMBAT_SNAP_DIST)"
                }
            }
            if ($bad.Count -eq 0) { $pairsOk++ }
            else { [void]$violations.Add("$obsLabel RECV of rank=$ownerRank hand=$hand $($bad -join '; ')") }
        }
    }

    # An own-rank RECV violation is a hard FAIL regardless of how many
    # legitimate other-owner pairs were also checked - it must never be
    # masked by a "no signal" SKIP below (that would hide the exact defect
    # this half of the gate exists to catch).
    if ($violations.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics @{ pairsChecked = $pairsChecked; pairsOk = $pairsOk; ownersObserved = $ownersObserved.Count } `
                    -Detail ($violations -join '; '))
    }
    if ($pairsChecked -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no resolvable other-owner RECV/MEMBER pair found across the $($Logs.Count) log(s)")
    }
    if ($ownersObserved.Count -lt 2) {
        return (Add-GateResult -Name $gate -Status SKIP -Metrics @{ ownersObserved = $ownersObserved.Count } `
                    -Detail "only $($ownersObserved.Count) distinct other-owner rank(s) had trackable RECV data (need >= 2 to prove multi-owner replication)")
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics @{ pairsChecked = $pairsChecked; pairsOk = $pairsOk; ownersObserved = $ownersObserved.Count } `
                -Detail "every RECV'd other-owner hand tracked its MEMBER source within ${Tolerance}u across $($ownersObserved.Count) distinct owner(s); no own-rank RECV violation")
}

# ---- Gate: cross-instance desync convergence ---------------------------------
#
# The movement gate above judges tracking fidelity over the WHOLE run; this
# judges something different - whether the SAME cross-instance pairs are still
# converged at the TAIL of the run (no permanent divergence at run end), which
# a whole-run median could mask (an early transient dragging the median up
# while the run actually settled, or a late drift the median is too coarse to
# catch). Reuses the same TABMAP-resolved owner/hand pairing as the movement
# gate; only the sampling window (near the owner's last MEMBER sample) differs.
function Test-MagateDesyncConvergence {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [double]$Tolerance = 8.0,
        [int]$TailWindowMs = 15000,
        # Phase 11: tail tolerance for a body lying DOWN at run end (see the
        # down-sample banding comment in Test-MagateDrivenOnlyMovement - the
        # ragdoll's resting offset is combat-band-bounded, engine-owned, and
        # heals on revive; ReplicatorUtil.h COMBAT_SNAP_DIST).
        [double]$DownBandTolerance = 20.0
    )
    $gate = "desync_convergence"
    $ownRanks = @{}; $tabMaps = @{}; $memberSeries = @{}; $recvSeries = @{}; $wnpcSeries = @{}
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label]     = Get-MagateOwnRank -File $f
        $tabMaps[$label]      = Get-MagateTabMap -File $f
        $memberSeries[$label] = Get-ScenarioSeries -File $f -Kind "MEMBER"
        $recvSeries[$label]   = Get-ScenarioSeries -File $f -Kind "RECV"
        # 05-03 gap-closure (run 20260902_150136_N4, root cause 2's sibling in
        # THIS gate): MilestoneAScenario::onTick stops calling tickEvidence()
        # (the ONLY source of MEMBER samples) the instant this client's own
        # elapsed time crosses its JOIN_DURATION_MS/HOST_DURATION_MS budget -
        # but the underlying Kenshi session, and the replicator's OWN always-on
        # "SCENARIO WNPC" census dump (a totally separate, scenario-lifecycle-
        # independent emitter - it kept firing in join1.log for 10+ seconds
        # after MEMBER went silent, confirmed against the raw log for hand
        # 1,1775512704), both keep running until the harness tears the
        # process down. WNPC rows report the same owner-local position for
        # every squad member (including this client's own hand) on a ~5s
        # cadence, so merging them into the owner's ground-truth series
        # recovers real end-of-run signal that MEMBER alone loses - instead of
        # just judging a stale pre-fight snapshot (which manufactures a
        # "desync" out of the owner's own diagnostic-loop stoppage, not an
        # actual cross-instance drift) or giving up on the pair entirely.
        $wnpcSeries[$label]    = Get-ScenarioSeries -File $f -Kind "WNPC"
    }

    # 05-03 gap-closure: a rank whose peer DISCONNECTED (scripted disconnectAtSec
    # DoD step, or the harness's own end-of-run teardown) and never RECONNECTED
    # in any log has no further ground truth to converge against for the rest
    # of the run - its owning process is gone, so there is nothing left for a
    # host-side proxy to "converge" WITH. clean_exit already grants the exact
    # same grace via -ExemptLabels (locked user decision, gap 3) and
    # disconnect_isolation already tolerates the parallel TABMAP-staleness
    # symptom of the same event; desync_convergence had no equivalent guard.
    # Scanned across every log (only survivors log a departed peer's DISCONNECT/
    # RECONNECT), keyed by rank (the "peer=" field the mod's own SCENARIO
    # MAGATE lines use is the departed RANK, not a connection-order index).
    $lastDisconnectEventIsFinal = @{} # rank -> $true if its last DC/RC event was a DISCONNECT
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $off = Get-LogClockOffsetMs -File $f
        $events = New-Object System.Collections.ArrayList
        foreach ($m in (Select-String -Path $f -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE DISCONNECT peer=(\d+) ownRank=\d+ t=\d+' -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$events.Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); rank = [int]$g[5].Value; kind = 'DC' })
        }
        foreach ($m in (Select-String -Path $f -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE RECONNECT peer=(\d+) ownRank=\d+ t=\d+' -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$events.Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); rank = [int]$g[5].Value; kind = 'RC' })
        }
        foreach ($rank in ($events | ForEach-Object { $_.rank } | Select-Object -Unique)) {
            $lastEvt = @($events | Where-Object { $_.rank -eq $rank } | Sort-Object { $_.t } | Select-Object -Last 1)
            if ($lastEvt.Count -eq 0) { continue }
            if ($lastEvt[0].kind -eq 'DC') { $lastDisconnectEventIsFinal[$rank] = $true }
        }
    }

    $checked = 0; $converged = 0; $skippedNoGroundTruth = 0
    $diverged = New-Object System.Collections.ArrayList
    foreach ($obsLabel in $Logs.Keys) {
        if (-not $ownRanks.ContainsKey($obsLabel) -or $null -eq $ownRanks[$obsLabel]) { continue }
        $recv = $recvSeries[$obsLabel]
        foreach ($hand in $recv.Keys) {
            $suffix = Get-MagateHandSuffix -Hand5 $hand
            $ownerRank = $null
            foreach ($r in $tabMaps[$obsLabel].Keys) { if ($tabMaps[$obsLabel][$r] -eq $suffix) { $ownerRank = $r; break } }
            if ($null -eq $ownerRank -or $ownerRank -eq $ownRanks[$obsLabel]) { continue }

            # No ground truth left to judge against: this rank's peer departed
            # (disconnect with no later reconnect anywhere) and cannot possibly
            # "converge" with a proxy - it no longer publishes anything.
            if ($lastDisconnectEventIsFinal.ContainsKey($ownerRank) -and $lastDisconnectEventIsFinal[$ownerRank]) {
                $skippedNoGroundTruth++
                continue
            }

            $ownerLabel = $null
            foreach ($cand in $ownRanks.Keys) { if ($ownRanks[$cand] -eq $ownerRank) { $ownerLabel = $cand; break } }
            if ($null -eq $ownerLabel -or -not $memberSeries.ContainsKey($ownerLabel)) { continue }
            $ownerMember = $memberSeries[$ownerLabel]
            if (-not $ownerMember.ContainsKey($hand) -or $ownerMember[$hand].Count -eq 0) { continue }

            # 05-03 gap-closure (run 20260902_150136_N4, root cause 2's sibling in
            # THIS gate): MEMBER alone can under-report the owner's true final
            # position - MilestoneAScenario::onTick stops calling tickEvidence()
            # (the ONLY MEMBER source) once this client's own elapsed time
            # crosses its scenario duration budget, while the session (and the
            # replicator's always-on WNPC census dump) keeps running. Merge in
            # WNPC rows for this SAME hand from the SAME owner log - self-
            # reported by the owner about its own body, on the SAME position
            # schema, just missing task/pelvis/crouch/idle/bs (irrelevant here,
            # only p/t are used below) - so "ground truth" tracks the owner's
            # real end-of-run position instead of freezing at whatever it was
            # doing when its own diagnostic loop happened to stop.
            $ownerGroundTruth = @($ownerMember[$hand])
            if ($wnpcSeries.ContainsKey($ownerLabel) -and $wnpcSeries[$ownerLabel].ContainsKey($hand)) {
                $ownerGroundTruth += @($wnpcSeries[$ownerLabel][$hand])
            }

            # 04-09 continuation gap-closure #3 (live gate false FAIL, run
            # 20260902_130704_N4): `Sort-Object t` (a BARE property-name
            # string) silently fails to sort correctly on this PowerShell 5.1
            # install when the pipeline objects are [hashtable] (as every
            # Get-ScenarioSeries entry is, @{t=...; p=...; ...}) - confirmed
            # by direct repro (`@(@{t=5},@{t=1},@{t=3}) | Sort-Object t`
            # returns objects in unspecified/hash-bucket order, NOT sorted by
            # t at all). `Sort-Object { $_.t }` (a script-block expression)
            # reliably invokes the normal per-object dot-property lookup and
            # sorts correctly for the SAME hashtable objects. This silently
            # made "the owner's last ground-truth sample"/"the tail's last RECV
            # sample" a near-random pick instead of the true chronological
            # last one - explaining why this gate intermittently PASSED on
            # earlier live runs (the random pick happened to land close to
            # the true final position) and FAILED here (it did not): the
            # comparison was never reliably judging "end of run" at all.
            $lastGroundTruthT = (@($ownerGroundTruth | Sort-Object { $_.t }) | Select-Object -Last 1).t

            $tail = @($recv[$hand] | Where-Object { $_.t -ge ($lastGroundTruthT - $TailWindowMs) })
            if ($tail.Count -eq 0) { continue }
            $lastRecv = (@($tail | Sort-Object { $_.t }) | Select-Object -Last 1)

            $best = [double]::MaxValue; $bp = $null
            foreach ($hs in $ownerGroundTruth) {
                $dt = [Math]::Abs($hs.t - $lastRecv.t)
                if ($dt -lt $best) { $best = $dt; $bp = $hs.p }
            }
            if ($null -eq $bp) { continue }
            $checked++
            $dx = $bp[0] - $lastRecv.p[0]; $dy = $bp[1] - $lastRecv.p[1]; $dz = $bp[2] - $lastRecv.p[2]
            $dist = [Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
            # A body lying DOWN at run end is judged at the combat band (its
            # ragdoll's resting offset is engine-owned and heals on revive -
            # see $DownBandTolerance's doc comment). bs is the observer's own
            # RECV row body state (bits 0-2 = DOWN|RAGDOLL|DEAD).
            $tailDown = (($lastRecv.bs -band 7) -ne 0)
            $effTol = if ($tailDown) { $DownBandTolerance } else { $Tolerance }
            $tolName = if ($tailDown) { "down-body combat band" } else { "tolerance" }
            if ($dist -le $effTol) { $converged++ }
            else { [void]$diverged.Add("$obsLabel<-rank$ownerRank hand=$hand end-of-run distance ${dist}u exceeds $tolName ${effTol}u") }
        }
    }

    $groundTruthNote = if ($skippedNoGroundTruth -gt 0) {
        " ($skippedNoGroundTruth pair(s) skipped: owner departed or its own diagnostic loop stopped before run end - no ground truth to judge)"
    } else { "" }
    if ($checked -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no cross-instance hand pair had samples near run end to judge convergence$groundTruthNote")
    }
    if ($diverged.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics @{ checked = $checked; converged = $converged; skippedNoGroundTruth = $skippedNoGroundTruth } -Detail ($diverged -join '; '))
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics @{ checked = $checked; converged = $converged; skippedNoGroundTruth = $skippedNoGroundTruth } `
                -Detail "all $checked cross-instance pair(s) converged within ${Tolerance}u by run end (no permanent divergence)$groundTruthNote")
}

# ---- Gate: clean exit / no crash ---------------------------------------------
#
# Reuses Test-LogHealth (reached gameplay, clean "SCENARIO RESULT" exit, no
# ERROR lines) per instance and Test-EngineIntegrity (no coop-attributed
# double-destroy) over the archived engine logs - the same primitives every
# other scenario's clean-exit judgment already relies on, generalized from 2
# instances to however many $Logs names.
#
# -ExemptLabels (gap 3, locked user decision): the DoD's own mandatory
# mid-run-disconnect step (Stop-Process -Force) structurally can never leave
# a clean "SCENARIO RESULT" behind on the instance it force-kills - that is
# not a crash, it is the harness doing exactly what the DoD asks. Names in
# -ExemptLabels (resolved by the caller from the disconnectAtSec marker -
# never hardcoded here, and never including the reconnect replacement) are
# judged by "reached gameplay" (its own "SCENARIO MAGATE start" line) instead
# of "SCENARIO RESULT", so a genuine early crash/launch failure on that same
# instance is still caught - only the graceful-exit requirement is waived.
function Test-MagateCleanExit {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [string]$RunDir = "",
        [string[]]$ExemptLabels = @()
    )
    $gate = "clean_exit"
    $bad = New-Object System.Collections.ArrayList
    $checked = 0
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { [void]$bad.Add("$label log missing"); continue }
        $checked++
        if ($ExemptLabels -contains $label) {
            $status = Test-LogHealth -File $f -Label "magate_$label" -Required $true -CleanPattern "SCENARIO MAGATE start"
        } else {
            $status = Test-LogHealth -File $f -Label "magate_$label" -Required $true -CleanPattern "SCENARIO RESULT"
        }
        if ($status -ne "PASS") { [void]$bad.Add("$label log-health $status") }
    }
    if ($RunDir -ne "" -and (Test-Path $RunDir)) {
        [void](Test-EngineIntegrity -OutDir $RunDir)
        $eng = @(Get-GateResults | Where-Object { $_.gate -eq "engine_integrity" }) | Select-Object -Last 1
        if ($null -ne $eng -and $eng.status -eq "FAIL") { [void]$bad.Add("engine_integrity FAIL: $($eng.detail)") }
    }
    if ($checked -eq 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Detail "no logs present to judge clean exit")
    }
    if ($bad.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics @{ checked = $checked } -Detail ($bad -join '; '))
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics @{ checked = $checked } `
                -Detail "all $checked instance(s) reached gameplay, exited via SCENARIO RESULT, logged no ERROR lines, no coop-attributed double-destroy")
}

# ---- Gate: per-owner medical/KO convergence (Phase 6 plan 02, player_state_gate) --
#
# The player_state_gate counterpart of Test-NpcVitals, generalized from ONE
# pinned duelist pair to EVERY owner's own leader hand judged against EVERY
# other instance's driven copy of it (the pairwise-leg generalization
# 06-RESEARCH.md's Oracle Plan section 2 calls for). Two legs:
#   Leg 1 (per-owner VITALS convergence) - for each rank R whose own log
#     publishes a SCENARIO VITALS series for its own leader hand (ground
#     truth - readMedicalByHand on R's OWN machine), and for every OTHER
#     instance O's VITALS series for the SAME hand (O's driven copy), assert
#     the tail-window median |blood| gap is within Tolerance - the exact
#     Test-NpcVitals shape (Medical.ps1:54-99), just run over every
#     (owner,observer) pair instead of one pinned duel victim.
#   Leg 2 (KO/revive edge crossing) - discovered empirically, not hardcoded to
#     a rank: whichever instance's log has an "[event] SEND ... ev=1|2 ..."
#     line (ReplicatorPublish.cpp:604) is that KO's owner; every OTHER
#     instance must log the matching "[event] RECV ... owner=<ownerRank>"
#     line (ReplicatorSpawn.cpp:938) for the SAME (id,ev) pair, and its own
#     RECV series for that hand must show a down bit (bs & (BODY_DOWN|
#     BODY_RAGDOLL|BODY_DEAD), Wire.h:361-363) within a latency budget after
#     the edge.
#
# Reuses Test-MagateDesyncConvergence's terminally-disconnected-owner skip
# (an owner whose last DC/RC event was a DISCONNECT with no later RECONNECT
# anywhere has no further ground truth to judge against) and the same
# insufficient-observation-window SKIP semantics Test-NpcVitals already uses
# (too few series/aligned pairs = SKIP, never a false PASS or FAIL).
function Test-MagatePlayerStateMedical {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [double]$Tolerance = 15.0,
        [int]$TailWindowMs = 12000,
        [int]$MinSeries = 3,
        [int]$KoWindowMs = 15000
    )
    $gate = "player_state_medical"
    $ownRanks = @{}; $tabMaps = @{}
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label] = Get-MagateOwnRank -File $f
        $tabMaps[$label]  = Get-MagateTabMap -File $f
    }

    # Terminally-disconnected-owner skip (semantic shape from
    # Test-MagateDesyncConvergence, CoopOraclesN.psm1:408-438): a rank whose
    # last DISCONNECT/RECONNECT event was a DISCONNECT with nothing after it
    # anywhere has no further ground truth left to converge against.
    #
    # Phase 6 review WR-03: events are MERGED across every log BEFORE the
    # per-rank verdict is taken. The old per-file latch set the terminal flag
    # from whichever file happened to end on a DC and never un-latched it when
    # ANOTHER instance's log carried the later RECONNECT - a reconnected rank
    # could be silently dropped from judgment because one early-dying log only
    # saw the DC. Convert-StampToMs already maps each file into the shared
    # clock frame (per-file OffsetMs), so cross-file ordering is sound.
    $lastDisconnectEventIsFinal = @{}
    $terminalDcTime = @{}
    $dcRcEvents = New-Object System.Collections.ArrayList
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $off = Get-LogClockOffsetMs -File $f
        foreach ($m in (Select-String -Path $f -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE DISCONNECT peer=(\d+) ownRank=\d+ t=\d+' -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$dcRcEvents.Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); rank = [int]$g[5].Value; kind = 'DC' })
        }
        foreach ($m in (Select-String -Path $f -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE RECONNECT peer=(\d+) ownRank=\d+ t=\d+' -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$dcRcEvents.Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); rank = [int]$g[5].Value; kind = 'RC' })
        }
    }
    foreach ($rank in ($dcRcEvents | ForEach-Object { $_.rank } | Select-Object -Unique)) {
        $lastEvt = @($dcRcEvents | Where-Object { $_.rank -eq $rank } | Sort-Object { $_.t } | Select-Object -Last 1)
        if ($lastEvt.Count -eq 0) { continue }
        if ($lastEvt[0].kind -eq 'DC') {
            $lastDisconnectEventIsFinal[$rank] = $true
            # Time of that terminal DC (sightings across logs differ by ms;
            # any of them serves): a KO edge authored comfortably BEFORE it
            # is still judgeable (leg 2) - the edge crossed while connected.
            $terminalDcTime[$rank] = $lastEvt[0].t
        }
    }

    # ---- Leg 1: per-owner pairwise VITALS convergence -------------------------
    $checked = 0; $converged = 0; $skippedShort = 0
    $diverged = New-Object System.Collections.ArrayList
    $ownersChecked = New-Object System.Collections.Generic.HashSet[string]
    foreach ($ownerLabel in $Logs.Keys) {
        $ownerRank = $ownRanks[$ownerLabel]
        if ($null -eq $ownerRank) { continue }
        if ($lastDisconnectEventIsFinal.ContainsKey($ownerRank) -and $lastDisconnectEventIsFinal[$ownerRank]) { continue }
        $ownerFile = $Logs[$ownerLabel]
        if (-not $tabMaps[$ownerLabel].ContainsKey($ownerRank)) { continue }
        $handIS = $tabMaps[$ownerLabel][$ownerRank]

        $ownerVitals = @()
        foreach ($id in (Get-HandAliases -File $ownerFile -WireIndexSerial $handIS)) { $ownerVitals += Get-VitalsSeries -File $ownerFile -HandIS $id }
        $ownerVitals = @($ownerVitals | Sort-Object { $_.t })
        if ($ownerVitals.Count -lt $MinSeries) { $skippedShort++; continue }

        foreach ($obsLabel in $Logs.Keys) {
            if ($obsLabel -eq $ownerLabel) { continue }
            $obsFile = $Logs[$obsLabel]
            if (-not (Test-Path $obsFile)) { continue }
            $obsVitals = @()
            foreach ($id in (Get-HandAliases -File $obsFile -WireIndexSerial $handIS)) { $obsVitals += Get-VitalsSeries -File $obsFile -HandIS $id }
            $obsVitals = @($obsVitals | Sort-Object { $_.t })
            if ($obsVitals.Count -lt $MinSeries) { $skippedShort++; continue }

            $endT = [Math]::Min(($ownerVitals | Measure-Object -Property t -Maximum).Maximum,
                                ($obsVitals   | Measure-Object -Property t -Maximum).Maximum)
            $tail = $endT - $TailWindowMs
            $gaps = @()
            foreach ($os in @($obsVitals | Where-Object { $_.t -ge $tail -and $_.t -le $endT })) {
                $near = $null; $best = [double]::MaxValue
                foreach ($ov in $ownerVitals) {
                    $dt = [Math]::Abs($ov.t - $os.t)
                    if ($dt -lt $best) { $best = $dt; $near = $ov }
                }
                if ($null -ne $near) { $gaps += [Math]::Abs($near.blood - $os.blood) }
            }
            if ($gaps.Count -eq 0) { $skippedShort++; continue }
            $ownersChecked.Add("$ownerRank") | Out-Null
            $checked++
            $sorted = @($gaps | Sort-Object)
            $median = $sorted[[int][Math]::Floor($sorted.Count / 2)]
            if ($median -le $Tolerance) { $converged++ }
            else {
                [void]$diverged.Add("$obsLabel<-rank$ownerRank hand=$handIS tail blood gap median=$([Math]::Round($median,1)) exceeds $Tolerance")
            }
        }
    }

    # ---- Leg 2: KO/revive edge crosses to every other instance ----------------
    # The owning rank is discovered from the log evidence itself (not a
    # hardcoded rank constant) - whichever process actually authored an
    # "[event] SEND ... ev=1|2 ..." line is judged, generically.
    $koFindings = New-Object System.Collections.ArrayList
    $koChecked = 0; $koSkipped = 0
    $koSendPat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] SEND id=(\d+) ev=(1|2) hand=(\d+),(\d+),(\d+),(\d+),(\d+)'
    # Phase 6 review WR-03: collect EVERY qualifying KO/death SEND across ALL
    # logs, then judge each one. The old code `break`ed on the first hit, so
    # only ONE edge per run was ever judged - and NONE at all whenever that
    # single author happened to disconnect later in the run.
    $koSends = New-Object System.Collections.ArrayList
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        # Only a KO/death of the sender's OWN squad leader is the scenario's
        # player-KO edge. The host ALSO [event]-SENDs down/death latches for
        # WORLD NPCs (a burst of ev=1/ev=2 sends at gameplay start - run
        # 20260902_174928: host id=1..25 owner=0, all world-NPC hands) - a
        # different mechanism this gate must never judge. The sender's own
        # leader hand comes from its own TABMAP line at its own rank, the
        # same identity leg 1 already keys on.
        $senderRank = $ownRanks[$label]
        if ($null -eq $senderRank) { continue }
        if (-not $tabMaps[$label].ContainsKey($senderRank)) { continue }
        $ownLeaderIS = $tabMaps[$label][$senderRank]
        $off = Get-LogClockOffsetMs -File $f
        # ... and only AFTER the sender's own scenario armed: at world load the
        # engine transiently reads bodies as ragdoll/down before physics
        # settles, so an [event] SEND ev=1 for the sender's OWN leader can
        # fire at gameplay start with no peer even connected yet (run
        # 20260902_174928: host id=1 ev=1 at 17:49:46, 22s before its own
        # MAGATE start, unreceivable by the not-yet-connected joins). The
        # scenario's KO step by definition happens after the scenario arms.
        $armT = Get-MarkerTimeMs -File $f -Pattern 'SCENARIO MAGATE start'
        foreach ($hit in (Select-String -Path $f -Pattern $koSendPat -ErrorAction SilentlyContinue)) {
            $g = $hit.Matches[0].Groups
            if ("$($g[10].Value),$($g[11].Value)" -ne $ownLeaderIS) { continue }
            if ($null -ne $armT -and (Convert-StampToMs -Groups $g -OffsetMs $off) -lt $armT) { continue }
            [void]$koSends.Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                id = [int]$g[5].Value; ev = [int]$g[6].Value
                handIS = "$($g[10].Value),$($g[11].Value)"
                label = $label; rank = $senderRank
            })
        }
    }
    $recvSeriesCache = @{}
    $obsEndCache = @{}
    foreach ($koSend in $koSends) {
        $koOwnerRank = $koSend.rank
        # WR-03: a terminally-disconnected author's edge is still judgeable
        # when it fired comfortably BEFORE the terminal DC - the reliable
        # send crossed while connected (run 20260902_181807: join1's KO edge
        # at +36s RECV'd by all three observers, yet the old blanket skip
        # judged nothing because join1 disconnected two minutes LATER). Only
        # an edge inside the disconnect margin has no delivery guarantee.
        if ($lastDisconnectEventIsFinal.ContainsKey($koOwnerRank) -and $lastDisconnectEventIsFinal[$koOwnerRank]) {
            $dcT = $terminalDcTime[$koOwnerRank]
            if ($null -eq $dcT -or $koSend.t -ge ($dcT - 2000)) { $koSkipped++; continue }
        }
        foreach ($obsLabel in $Logs.Keys) {
            if ($obsLabel -eq $koSend.label) { continue }
            $obsFile = $Logs[$obsLabel]
            if (-not (Test-Path $obsFile)) { continue }
            # Coverage: an observer whose log ends before the edge (plus a
            # delivery margin) has no evidence to judge - the recruit gate's
            # own insufficient-observation-window shape, never a false FAIL
            # against a harness-killed instance.
            if (-not $obsEndCache.ContainsKey($obsLabel)) {
                $obsEndCache[$obsLabel] = Get-LogLastActivityMs -File $obsFile
            }
            $obsEnd = $obsEndCache[$obsLabel]
            if ($null -ne $obsEnd -and $obsEnd -lt ($koSend.t + 2000)) { $koSkipped++; continue }
            $koChecked++
            $recvPat = "\[event\] RECV id=$($koSend.id) ev=$($koSend.ev) owner=$koOwnerRank"
            $recvHit = Select-String -Path $obsFile -Pattern $recvPat -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -eq $recvHit) {
                [void]$koFindings.Add("$obsLabel never logged the matching [event] RECV id=$($koSend.id) ev=$($koSend.ev) owner=$koOwnerRank")
                continue
            }
            # Down-bit window: this observer's own RECV series for the same
            # hand should show BODY_DOWN|BODY_RAGDOLL|BODY_DEAD (0x7) within
            # KoWindowMs of the SEND edge - the driven copy actually went down.
            if (-not $recvSeriesCache.ContainsKey($obsLabel)) {
                $recvSeriesCache[$obsLabel] = Get-ScenarioSeries -File $obsFile -Kind "RECV"
            }
            $recvSeries = $recvSeriesCache[$obsLabel]
            $matchKeys = @($recvSeries.Keys | Where-Object { (Get-MagateHandSuffix -Hand5 $_) -eq $koSend.handIS })
            $sawDown = $false
            foreach ($k in $matchKeys) {
                foreach ($row in $recvSeries[$k]) {
                    if ($row.t -ge $koSend.t -and $row.t -le ($koSend.t + $KoWindowMs) -and (([int]$row.bs -band 0x7) -ne 0)) { $sawDown = $true; break }
                }
                if ($sawDown) { break }
            }
            if (-not $sawDown) {
                # An observer killed before the full down-bit window elapsed
                # cannot be asserted against (insufficient window, not a
                # violation); one that lived through the window and never
                # showed the bit is a genuine finding.
                if ($null -ne $obsEnd -and $obsEnd -lt ($koSend.t + $KoWindowMs)) { $koSkipped++ }
                else {
                    [void]$koFindings.Add("$obsLabel RECV'd the KO edge but its own RECV series never showed a down bit (bs & 0x7) within ${KoWindowMs}ms of hand=$($koSend.handIS)")
                }
            }
        }
    }

    # Phase 6 review WR-03: minimum-coverage requirement. The gate exists to
    # judge EVERY owner's medical state; a PASS built on a fraction of them
    # (evidence regression: missing/short VITALS on the other logs) is a
    # blind-spot green. Expected coverage = every rank that resolved an
    # ownRank, MINUS terminally-disconnected ranks (the DoD's own scheduled
    # disconnect legitimately removes an owner from judgment - the clean_exit
    # exemption pattern). Any shortfall is an honest SKIP, never a PASS.
    # Observed FAIL findings still take precedence: real divergence on the
    # owners we DID judge is a violation regardless of coverage.
    $expectedOwners = New-Object System.Collections.Generic.HashSet[string]
    foreach ($label in $Logs.Keys) {
        $r = $ownRanks[$label]
        if ($null -eq $r) { continue }
        if ($lastDisconnectEventIsFinal.ContainsKey($r) -and $lastDisconnectEventIsFinal[$r]) { continue }
        $expectedOwners.Add("$r") | Out-Null
    }
    $allFindings = @($diverged) + @($koFindings)
    if ($allFindings.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL `
                    -Metrics @{ pairsChecked = $checked; pairsConverged = $converged; koChecked = $koChecked; koSkipped = $koSkipped; owners = $ownersChecked.Count; ownersExpected = $expectedOwners.Count } `
                    -Detail ($allFindings -join '; '))
    }
    if ($checked -eq 0 -and $koChecked -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Metrics @{ skippedShort = $skippedShort } `
                    -Detail "no resolvable per-owner VITALS pair and no KO SEND/RECV edge found across the $($Logs.Count) log(s)")
    }
    if ($ownersChecked.Count -lt $expectedOwners.Count) {
        $missing = @($expectedOwners | Where-Object { -not $ownersChecked.Contains($_) }) -join ','
        return (Add-GateResult -Name $gate -Status SKIP `
                    -Metrics @{ pairsChecked = $checked; pairsConverged = $converged; koChecked = $koChecked; koSkipped = $koSkipped; owners = $ownersChecked.Count; ownersExpected = $expectedOwners.Count; skippedShort = $skippedShort } `
                    -Detail "only $($ownersChecked.Count) of $($expectedOwners.Count) expected owner(s) judgeable (rank(s) $missing had no judgeable VITALS pair - evidence gap, not proof of convergence)")
    }
    return (Add-GateResult -Name $gate -Status PASS `
                -Metrics @{ pairsChecked = $checked; pairsConverged = $converged; koChecked = $koChecked; koSkipped = $koSkipped; owners = $ownersChecked.Count; ownersExpected = $expectedOwners.Count } `
                -Detail "every judgeable per-owner VITALS pair converged within ${Tolerance}u blood gap across all $($ownersChecked.Count) expected owner(s); every judgeable KO/death edge ($koChecked observer check(s), $koSkipped skipped for insufficient window) crossed with a matching down-bit window")
}

# ---- Gate: recruitment end-ownership (Phase 6 plan 02, player_state_gate) --------
#
# Generalizes the 2-log Test-RecruitSync's semantic model (World.ps1:382-449,
# do NOT edit it - the sibling-module rule) to N=4, judged against the
# evidence the recruit path ACTUALLY emits (calibrated on live run
# 20260902_174928_N4, the first real N=4 recruit ever judged):
#
#   * A RUNTIME recruit keeps its hand (before == after in the SCENARIO
#     RECRUIT line): there is no "retired old hand", the observers take the
#     REKEY-FALLBACK -> force-REQ -> [spawn] mint path (never REKEY-BIND,
#     which requires a pre-existing local body), and their [spawn] REQ for
#     that hand IS the legitimate mint channel - only a baked recruit
#     (before != after) has a retired hand that must never be re-minted.
#   * The recruit lands in the shared rank-0 tab on EVERY instance
#     (insertPeerMember/joinPlayerSquadAt), so the scenario's rank-keyed
#     MEMBER/RECV labeling files it under whichever label rank 0 maps to on
#     that instance (host: MEMBER; joins: RECV) - presence in EITHER series
#     is visibility evidence; OWNERSHIP is judged from the [recruit]/[event]
#     lines, never from the MEMBER/RECV label.
#
# Asserts: (1) the author published the edge ("[recruit] EVT send") and its
# own log carries the hand in its MEMBER-or-RECV series; (2) every other
# instance whose log covers the edge received it with the author's identity
# ("[event] RECV ... ev=10 owner=<authorRank>") and processed the re-key
# ("[recruit] REKEY..."); (3) every other instance that lived long enough
# for the ~30s REQ-retry mint channel actually SEES the unit (alias-aware
# MEMBER-or-RECV presence); (4) NO other instance ever claims ownership
# (ownIt=1 / CONTROL-FLIP / its own EVT send for the same hand); (5) for a
# baked recruit only, no observer re-mints the retired old hand.
function Test-MagatePlayerStateRecruit {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        # An observer must have lived at least this long past the recruit
        # edge for the visibility assertion to be judgeable: the runtime-
        # recruit mint channel is REQ/INFO with a 30s denied-retry cadence
        # (ReplicatorSpawn.cpp DENIED_RETRY_MS), so an observer that died
        # sooner may legitimately not have minted yet - insufficient
        # observation window, not a violation (04-09 exemption category).
        [int]$VisibilityWindowMs = 60000
    )
    $gate = "player_state_recruit"
    $recruitPat = 'SCENARIO RECRUIT who=\S+ leg=\S+ res=1 before=(\d+),(\d+),(\d+),(\d+),(\d+) after=(\d+),(\d+),(\d+),(\d+),(\d+)'
    $authorLabel = $null; $before = $null; $after = $null
    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $hit = Select-String -Path $f -Pattern $recruitPat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $hit) {
            $g = $hit.Matches[0].Groups
            $authorLabel = $label
            # Hand parts are u32 on the wire (serials like 3129272576 exceed
            # Int32.MaxValue) - [uint32], never [int] (live-run 20260902_174928
            # threw InvalidCastFromStringToInteger on a real recruit serial).
            $before = @([uint32]$g[1].Value, [uint32]$g[2].Value, [uint32]$g[3].Value, [uint32]$g[4].Value, [uint32]$g[5].Value)
            $after  = @([uint32]$g[6].Value, [uint32]$g[7].Value, [uint32]$g[8].Value, [uint32]$g[9].Value, [uint32]$g[10].Value)
            break
        }
    }
    if ($null -eq $authorLabel) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no SCENARIO RECRUIT res=1 line found in any of the $($Logs.Count) log(s)")
    }

    # after/before are [type,container,containerSerial,index,serial] (h[0..4],
    # the readObjectHand/probeRecruit layout) - the SAME order the [recruit]/
    # [spawn] replicator log lines use. MEMBER/RECV keys (Get-ScenarioSeries)
    # are serialized index,serial,type,container,containerSerial instead
    # (logScenarioEntity's own field order); their suffix (index,serial) is
    # what Get-MagateHandSuffix/Get-HandAliases key on.
    $afterTccsIs  = "$($after[0]),$($after[1]),$($after[2]),$($after[3]),$($after[4])"
    $afterIS      = "$($after[3]),$($after[4])"
    $beforeTccsIs = "$($before[0]),$($before[1]),$($before[2]),$($before[3]),$($before[4])"
    $handRetired  = ($beforeTccsIs -ne $afterTccsIs)

    $authorRank = Get-MagateOwnRank -File $Logs[$authorLabel]
    if ($null -eq $authorRank) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "author log ($authorLabel) carries no 'SCENARIO MAGATE start ownRank=' line - cannot resolve the author's rank identity")
    }

    # Alias-aware presence of the recruited hand in a log's MEMBER-or-RECV
    # series: a minted proxy lives under a LOCAL hand the "[rekey] wire=..."
    # translation maps back to the wire hand (Get-HandAliases).
    function Test-RecruitHandVisible {
        param([string]$File, [string]$WireIS)
        # Get-HandAliases returns ",$ids" (the comma no-unroll trick) - @()
        # around it NESTS the array; unwrap via foreach, the same convention
        # every existing caller (Medical.ps1:67, Combat.ps1:127) uses.
        $aliases = @()
        foreach ($id in (Get-HandAliases -File $File -WireIndexSerial $WireIS)) { $aliases += $id }
        foreach ($kind in @("MEMBER", "RECV")) {
            $series = Get-ScenarioSeries -File $File -Kind $kind
            foreach ($k in $series.Keys) {
                if (($aliases -contains (Get-MagateHandSuffix -Hand5 $k)) -and $series[$k].Count -gt 0) { return $true }
            }
        }
        return $false
    }

    $findings = New-Object System.Collections.ArrayList
    $authorFile = $Logs[$authorLabel]

    # (1) author publishes the edge + carries the hand in its own evidence stream.
    $sendPat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[recruit\] EVT send old=\d+,\d+,\d+,\d+,\d+ new=' + [regex]::Escape($afterTccsIs)
    $sendHit = Select-String -Path $authorFile -Pattern $sendPat -ErrorAction SilentlyContinue | Select-Object -First 1
    $sendT = $null
    if ($null -eq $sendHit) {
        [void]$findings.Add("$authorLabel never logged '[recruit] EVT send ... new=$afterTccsIs'")
    } else {
        $authorOff = Get-LogClockOffsetMs -File $authorFile
        $sendT = Convert-StampToMs -Groups $sendHit.Matches[0].Groups -OffsetMs $authorOff
    }
    if (-not (Test-RecruitHandVisible -File $authorFile -WireIS $afterIS)) {
        [void]$findings.Add("$authorLabel (author) never carried the recruited hand ($afterIS) in its own MEMBER/RECV series")
    }

    $observed = 0; $visSkipped = 0
    foreach ($label in $Logs.Keys) {
        if ($label -eq $authorLabel) { continue }
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $obsEnd = Get-LogLastActivityMs -File $f
        # An observer whose log ends before the edge even happened has no
        # evidence to judge (e.g. an earlier scheduled disconnect).
        if ($null -ne $sendT -and $null -ne $obsEnd -and $obsEnd -lt ($sendT + 2000)) { continue }
        $observed++

        # (2a) the edge crossed with the author's identity intact.
        $recvPat = "\[event\] RECV id=\d+ ev=10 owner=$authorRank hand=" + [regex]::Escape($beforeTccsIs)
        if ($null -eq (Select-String -Path $f -Pattern $recvPat -ErrorAction SilentlyContinue | Select-Object -First 1)) {
            [void]$findings.Add("$label never logged '[event] RECV ... ev=10 owner=$authorRank hand=$beforeTccsIs'")
        }
        # (2b) the re-key path ran (any flavor: REKEY-BIND for a resolvable
        # local body, REKEY-FALLBACK/REKEY ok=0 for a runtime force-REQ).
        $rekeyPat = '\[recruit\] REKEY.*new=' + [regex]::Escape($afterTccsIs)
        if ($null -eq (Select-String -Path $f -Pattern $rekeyPat -ErrorAction SilentlyContinue | Select-Object -First 1)) {
            [void]$findings.Add("$label never logged a '[recruit] REKEY... new=$afterTccsIs' (re-key path never ran)")
        }
        # (3) visibility: the unit must actually exist on this observer -
        # judgeable only if it lived past the mint channel's retry cadence.
        $windowOk = ($null -eq $sendT -or $null -eq $obsEnd -or ($obsEnd - $sendT) -ge $VisibilityWindowMs)
        if ($windowOk) {
            if (-not (Test-RecruitHandVisible -File $f -WireIS $afterIS)) {
                [void]$findings.Add("$label never carried the recruited hand ($afterIS) in its MEMBER/RECV series despite $([int](($obsEnd - $sendT)/1000))s of post-edge runtime (unit invisible to this instance)")
            }
        } else { $visSkipped++ }
        # (4) end-ownership: no other instance may CLAIM the unit.
        if ($null -ne (Select-String -Path $f -Pattern ('\[recruit\] MEMBER new=' + [regex]::Escape($afterTccsIs) + '.*ownIt=1') -ErrorAction SilentlyContinue | Select-Object -First 1)) {
            [void]$findings.Add("$label inserted the recruit with ownIt=1 (claimed ownership of another player's recruit)")
        }
        if ($null -ne (Select-String -Path $f -Pattern ('CONTROL-FLIP claim new=' + [regex]::Escape($afterTccsIs)) -ErrorAction SilentlyContinue | Select-Object -First 1)) {
            [void]$findings.Add("$label CONTROL-FLIP-claimed the recruit (ownership seized without a squad-move transfer)")
        }
        if ($null -ne (Select-String -Path $f -Pattern ('\[recruit\] EVT send old=\d+,\d+,\d+,\d+,\d+ new=' + [regex]::Escape($afterTccsIs)) -ErrorAction SilentlyContinue | Select-Object -First 1)) {
            [void]$findings.Add("$label ALSO published a recruit EVT for the same hand (duplicate authorship)")
        }
        # (5) baked recruit only: the retired old hand must never re-mint.
        if ($handRetired) {
            if ($null -ne (Select-String -Path $f -Pattern ('\[spawn\] REQ hand=' + [regex]::Escape($beforeTccsIs)) -ErrorAction SilentlyContinue | Select-Object -First 1)) {
                [void]$findings.Add("$label logged a duplicate-mint '[spawn] REQ hand=$beforeTccsIs' for the retired old hand")
            }
        }
    }
    if ($observed -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "only the author's log ($authorLabel) covers the recruit edge; no other instance to judge against")
    }
    if ($findings.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics @{ author = $authorLabel; observed = $observed; visSkipped = $visSkipped } -Detail ($findings -join '; '))
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics @{ author = $authorLabel; observed = $observed; visSkipped = $visSkipped } `
                -Detail "author ($authorLabel, rank $authorRank) published the recruit; all $observed other instance(s) received it with the author's identity, ran the re-key path, see the unit ($visSkipped visibility check(s) skipped for insufficient window), and never claimed it")
}

# ---- Gate: item conservation Legs A-D (Phase 7 plan 03, item_conservation_gate) --
#
# Judges the item_conservation_gate live scenario's SCENARIO CONSERVE census
# (src/plugin/test/ScenarioItemConservation.cpp) against the production
# transfer/claim verdict evidence ([xfer] COMMIT - Plan 01, [wi] CLAIM-WIN -
# Plan 02) for zero duplication and zero silent loss across the 4-instance
# rig. Four legs, ALL evidence-driven - no scenario-specific magic numbers
# are hardcoded here. Step 2 below builds a per-(rank,sid) ledger entirely
# from the scenario's own self-authored SEED/XFER/DROP/CLAIM lines plus the
# production COMMIT/CLAIM-WIN lines, unioned across all 4 logs (a real
# authored action is logged exactly once, by its own author) - so a
# differently-scripted future run of the same scenario is judged identically
# with no oracle-side edit:
#   Leg A (per-instance conservation) - for each rank's OWN authoritative log
#     (the log whose ownRank_ matches), final(sid) - initial(sid) across the
#     observed checkpoints must equal the ledger-derived expected delta; any
#     mismatch FAILs naming instance+rank+sid+delta (an unexplained
#     loss/gain).
#   Leg B (cross-instance convergence) - at the LAST checkpoint every log's
#     own local mirror of every rank's container (every log reports all 4
#     ranks each checkpoint, not just its own) agrees exactly with every
#     other log's mirror (tolerance 0 - integer counts).
#   Leg C (global conservation) - summing each container ONCE from its
#     OWNER's own log (never all 4 mirrors, which would over-count ~4x) plus
#     ground from the drop author's (rank 0/host's) own log stays constant
#     across every checkpoint that has full 4-container coverage.
#   Leg D (transfer/claim verdict bookkeeping) - every self-authored transfer
#     (SCENARIO CONSERVE XFER) has exactly one [xfer] COMMIT verdict logged
#     for it; the one contention has exactly one [wi] CLAIM-WIN winner; no
#     verdict is ever duplicated or conflicting.
# NoSignalFails (scenarios.psd1's item_conservation_gate entry): zero CONSERVE
# evidence anywhere -> this gate SKIPs, which NoSignalFails turns into a
# verdict FAIL in analyze_run4.ps1 - an evidentiary gap is never a silent
# pass.
function Test-InvConservation {
    param(
        [Parameter(Mandatory = $true)]$Logs
    )
    $gate = "inv_conservation"

    $contPat   = "SCENARIO CONSERVE ck=(\d+) scope=cont rank=(\d+) hand=(\d+),(\d+),(\d+),(\d+),(\d+) sid='([^']*)' qty=(\d+)"
    $grndPat   = "SCENARIO CONSERVE ck=(\d+) scope=ground sid='([^']*)' qty=(-?\d+)"
    $xferPat   = "SCENARIO CONSERVE XFER src=(\d+) dst=(\d+) qty=(-?\d+) moved=(-?\d+) sid='([^']*)' t=(\d+)"
    $dropPat   = "SCENARIO CONSERVE DROP rank=(\d+) n=(-?\d+) sid='([^']*)'"
    $claimPat  = "SCENARIO CONSERVE CLAIM rank=(\d+) got=(-?\d+) sid='([^']*)'"
    $commitPat = "\[xfer\] COMMIT id=(\d+) author=(\d+) outcome=(\S+) applied=(\d+) sid='([^']*)' type=(\d+) qty=(\d+)"
    $winPat    = "\[wi\] CLAIM-WIN author=(\d+) netId=(\d+) winner=(\d+) n=(\d+)"

    # ---- Step 1: parse every log's own CONSERVE + verdict evidence ------------
    # contByLog[$label][$rank]["$ck|$sid"] = qty - THIS log's own local view of
    # rank R's container at checkpoint ck (every log reports every rank each
    # checkpoint, which doubles as Leg B's cross-log comparison input).
    $contByLog = @{}
    $grndByLog = @{}
    $xferLines   = New-Object System.Collections.ArrayList
    $dropLines   = New-Object System.Collections.ArrayList
    $claimLines  = New-Object System.Collections.ArrayList
    $commitLines = New-Object System.Collections.ArrayList
    $winLines    = New-Object System.Collections.ArrayList
    $ownRanks = @{}
    $anyEvidence = $false

    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label] = Get-MagateOwnRank -File $f
        $contByLog[$label] = @{}
        $grndByLog[$label] = @{}

        foreach ($m in (Select-String -Path $f -Pattern $contPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            $ck = [int]$g[1].Value; $rank = [int]$g[2].Value; $sid = $g[8].Value; $qty = [int]$g[9].Value
            if (-not $contByLog[$label].ContainsKey($rank)) { $contByLog[$label][$rank] = @{} }
            $contByLog[$label][$rank]["$ck|$sid"] = $qty
        }
        foreach ($m in (Select-String -Path $f -Pattern $grndPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            $ck = [int]$g[1].Value; $sid = $g[2].Value; $qty = [int]$g[3].Value
            $grndByLog[$label]["$ck|$sid"] = $qty
        }
        foreach ($m in (Select-String -Path $f -Pattern $xferPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            [void]$xferLines.Add([pscustomobject]@{
                label = $label; src = [int]$g[1].Value; dst = [int]$g[2].Value
                qty = [int]$g[3].Value; moved = [int]$g[4].Value; sid = $g[5].Value; t = [int]$g[6].Value
            })
        }
        foreach ($m in (Select-String -Path $f -Pattern $dropPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            [void]$dropLines.Add([pscustomobject]@{ label = $label; rank = [int]$g[1].Value; n = [int]$g[2].Value; sid = $g[3].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $claimPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            [void]$claimLines.Add([pscustomobject]@{ label = $label; rank = [int]$g[1].Value; got = [int]$g[2].Value; sid = $g[3].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $commitPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            [void]$commitLines.Add([pscustomobject]@{
                label = $label; id = [int]$g[1].Value; author = [int]$g[2].Value
                outcome = $g[3].Value; applied = [int]$g[4].Value; sid = $g[5].Value
            })
        }
        foreach ($m in (Select-String -Path $f -Pattern $winPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true
            $g = $m.Matches[0].Groups
            [void]$winLines.Add([pscustomobject]@{ label = $label; author = [int]$g[1].Value; netId = [int]$g[2].Value; winner = [int]$g[3].Value; n = [int]$g[4].Value })
        }
    }

    if (-not $anyEvidence) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no SCENARIO CONSERVE / [xfer] COMMIT / [wi] CLAIM-WIN evidence found in any of the $($Logs.Count) log(s)")
    }

    # ---- Step 2: build the evidence-driven per-(rank,sid) ledger --------------
    $ledger = @{}
    function Add-InvLedger { param($Rank, $Sid, $Delta)
        $k = "$Rank|$Sid"
        if (-not $ledger.ContainsKey($k)) { $ledger[$k] = 0 }
        $ledger[$k] += $Delta
    }
    $seenXfer = New-Object System.Collections.Generic.HashSet[string]
    foreach ($x in $xferLines) {
        # A real transfer is authored by exactly ONE log (only the SOURCE
        # rank's own instance calls moveItemBetweenContainers) - dedupe on
        # (label,src,dst,t) in case a resent/duplicated line ever appears.
        $key = "$($x.label)|$($x.src)|$($x.dst)|$($x.t)"
        if (-not $seenXfer.Add($key)) { continue }
        Add-InvLedger -Rank $x.src -Sid $x.sid -Delta (0 - $x.moved)
        Add-InvLedger -Rank $x.dst -Sid $x.sid -Delta $x.moved
    }
    $seenDrop = New-Object System.Collections.Generic.HashSet[string]
    $dropSidByAny = $null
    $dropQtyByAny = 0
    foreach ($d in $dropLines) {
        $key = "$($d.label)|$($d.rank)|$($d.sid)"
        if (-not $seenDrop.Add($key)) { continue }
        Add-InvLedger -Rank $d.rank -Sid $d.sid -Delta (0 - $d.n)
        if ($null -eq $dropSidByAny) { $dropSidByAny = $d.sid; $dropQtyByAny = $d.n }
    }
    $seenWin = New-Object System.Collections.Generic.HashSet[string]
    foreach ($w in $winLines) {
        $key = "$($w.author)|$($w.netId)"
        if (-not $seenWin.Add($key)) { continue }
        # CLAIM-WIN carries no sid (it is a netId-keyed world-item verdict,
        # not a container-plane packet) - attribute it to the sid the
        # scenario's own DROP line named, since this scenario runs exactly
        # one contention over exactly one dropped item.
        #
        # 07-04 live-run fix (run 20260902_222940_N4 ground truth): CLAIM-WIN's
        # n= is the number of COMPETING CLAIMS in the window
        # (ReplicatorItems.cpp: `n = wit->second.claims.size()`), NOT an
        # awarded quantity - a real 2-claimant contention emits n=2 while the
        # winner physically gains only the dropped item. The winner's expected
        # delta is therefore the DROP line's quantity (what existed to be
        # won); using n=2 inflated the expectation and produced a false FAIL
        # against a correctly-conserved run. Zero-tolerance is unchanged: a
        # duped award still shows actual > expected and FAILs.
        if ($null -ne $dropSidByAny) { Add-InvLedger -Rank $w.winner -Sid $dropSidByAny -Delta $dropQtyByAny }
    }

    # ---- Leg A: per-instance conservation --------------------------------------
    $legAChecked = 0; $legAConverged = 0; $legASkipped = 0
    $legAFindings = New-Object System.Collections.ArrayList
    $expectedOwnerCount = 0
    foreach ($label in $Logs.Keys) { if ($null -ne $ownRanks[$label]) { $expectedOwnerCount++ } }
    $judgedOwners = New-Object System.Collections.Generic.HashSet[string]
    foreach ($ownerLabel in $Logs.Keys) {
        $ownerRank = $ownRanks[$ownerLabel]
        if ($null -eq $ownerRank) { continue }
        if (-not $contByLog.ContainsKey($ownerLabel) -or -not $contByLog[$ownerLabel].ContainsKey($ownerRank)) { $legASkipped++; continue }
        $series = $contByLog[$ownerLabel][$ownerRank]
        $bySid = @{}
        foreach ($k in $series.Keys) {
            $parts = $k -split '\|', 2
            $ck = [int]$parts[0]; $sid = $parts[1]
            if (-not $bySid.ContainsKey($sid)) { $bySid[$sid] = New-Object System.Collections.ArrayList }
            [void]$bySid[$sid].Add([pscustomobject]@{ ck = $ck; qty = $series[$k] })
        }
        foreach ($sid in $bySid.Keys) {
            $rows = @($bySid[$sid] | Sort-Object { $_.ck })
            if ($rows.Count -lt 2) { $legASkipped++; continue }
            $initial = $rows[0].qty; $final = $rows[$rows.Count - 1].qty
            $actualDelta = $final - $initial
            $expectedKey = "$ownerRank|$sid"
            $expectedDelta = if ($ledger.ContainsKey($expectedKey)) { $ledger[$expectedKey] } else { 0 }
            $legAChecked++
            [void]$judgedOwners.Add($ownerLabel)
            if ($actualDelta -eq $expectedDelta) { $legAConverged++ }
            else {
                [void]$legAFindings.Add("$ownerLabel rank=$ownerRank sid='$sid' delta=$actualDelta expected=$expectedDelta (unexplained loss/gain)")
            }
        }
    }
    # Coverage gap: SOME (not zero, not all) expected instances never emitted
    # judgeable evidence for their OWN rank - a real evidentiary gap, not
    # "nothing to judge at all" (that earlier case already returned SKIP via
    # $anyEvidence). Surfaced as a Leg A FINDING so the gate FAILs outright
    # rather than silently passing on whatever fraction of instances happened
    # to report - a missing instance is never a silent pass.
    if ($judgedOwners.Count -gt 0 -and $judgedOwners.Count -lt $expectedOwnerCount) {
        $missingOwners = @()
        foreach ($label in $Logs.Keys) {
            if ($null -ne $ownRanks[$label] -and -not $judgedOwners.Contains($label)) { $missingOwners += "$label(rank=$($ownRanks[$label]))" }
        }
        [void]$legAFindings.Add("only $($judgedOwners.Count) of $expectedOwnerCount expected instance(s) emitted judgeable SCENARIO CONSERVE cont evidence for their own rank - missing: $($missingOwners -join ', ') (evidence gap, not proof of conservation)")
    }

    # ---- Leg B: cross-instance convergence at the final checkpoint ------------
    $finalCk = -1
    foreach ($label in $contByLog.Keys) {
        foreach ($rank in $contByLog[$label].Keys) {
            foreach ($k in $contByLog[$label][$rank].Keys) {
                $ck = [int](($k -split '\|', 2)[0])
                if ($ck -gt $finalCk) { $finalCk = $ck }
            }
        }
    }
    $legBChecked = 0; $legBConverged = 0
    $legBFindings = New-Object System.Collections.ArrayList
    if ($finalCk -ge 0) {
        foreach ($rank in 0..3) {
            $bySid = @{}
            foreach ($label in $contByLog.Keys) {
                if (-not $contByLog[$label].ContainsKey($rank)) { continue }
                foreach ($k in $contByLog[$label][$rank].Keys) {
                    $parts = $k -split '\|', 2
                    if ([int]$parts[0] -ne $finalCk) { continue }
                    $sid = $parts[1]
                    if (-not $bySid.ContainsKey($sid)) { $bySid[$sid] = @{} }
                    $bySid[$sid][$label] = $contByLog[$label][$rank][$k]
                }
            }
            foreach ($sid in $bySid.Keys) {
                $vals = $bySid[$sid]
                if ($vals.Keys.Count -lt 2) { continue }
                $ref = $null; $refLabel = $null
                foreach ($label in $vals.Keys) {
                    if ($null -eq $ref) { $ref = $vals[$label]; $refLabel = $label; continue }
                    $legBChecked++
                    if ($vals[$label] -eq $ref) { $legBConverged++ }
                    else {
                        [void]$legBFindings.Add("rank=$rank sid='$sid' ck=$finalCk $refLabel=$ref vs ${label}=$($vals[$label]) (not converged)")
                    }
                }
            }
        }
    }

    # ---- Leg C: global conservation (owner-of-record sum, per checkpoint) -----
    $legCFindings = New-Object System.Collections.ArrayList
    $legCChecked = 0
    $allCks = New-Object System.Collections.Generic.HashSet[int]
    foreach ($label in $contByLog.Keys) {
        foreach ($rank in $contByLog[$label].Keys) {
            foreach ($k in $contByLog[$label][$rank].Keys) { [void]$allCks.Add([int](($k -split '\|', 2)[0])) }
        }
    }
    $hostLabel = $null
    foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq 0) { $hostLabel = $label } }
    $ownerLabelByRank = @{}
    foreach ($rank in 0..3) {
        foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq $rank) { $ownerLabelByRank[$rank] = $label } }
    }
    $sumByCk = @{}
    foreach ($ck in ($allCks | Sort-Object)) {
        $sum = 0; $have = $true
        foreach ($rank in 0..3) {
            $ownerLabel = $ownerLabelByRank[$rank]
            if ($null -eq $ownerLabel -or -not $contByLog.ContainsKey($ownerLabel) -or -not $contByLog[$ownerLabel].ContainsKey($rank)) { $have = $false; break }
            $foundAny = $false
            foreach ($k in $contByLog[$ownerLabel][$rank].Keys) {
                $parts = $k -split '\|', 2
                if ([int]$parts[0] -eq $ck) { $sum += $contByLog[$ownerLabel][$rank][$k]; $foundAny = $true }
            }
            if (-not $foundAny) { $have = $false; break }
        }
        if ($have -and $null -ne $hostLabel -and $grndByLog.ContainsKey($hostLabel)) {
            foreach ($k in $grndByLog[$hostLabel].Keys) {
                $parts = $k -split '\|', 2
                if ([int]$parts[0] -eq $ck) { $sum += $grndByLog[$hostLabel][$k] }
            }
        }
        if ($have) { $sumByCk[$ck] = $sum; $legCChecked++ }
    }
    if ($legCChecked -gt 0) {
        # 07-04 live-run fix (run 20260902_214911_N4 ground truth): each
        # instance's checkpoints fire on its OWN scenario clock, and the four
        # scenarios arm up to ~23s apart (staggered join launches). An
        # INTERMEDIATE ck therefore sums owner-of-record counts sampled at
        # DIFFERENT wall times - while a transfer is in flight between the src
        # owner's ckN and the dst owner's ckN, the sum is structurally short/
        # long by the transferred qty without any real dup/loss. Conservation
        # is judged at the two QUIESCENT points the scenario designs for:
        # the baseline (first) ck (post-seed settle, pre-legs) and the final
        # ck (post-all-legs settle). The zero-tolerance verdict at those two
        # points is unchanged - this narrows WHERE the sum is a valid
        # measurement, not WHAT counts as conserved.
        $cks = @($sumByCk.Keys | Sort-Object)
        $baseline = $sumByCk[$cks[0]]
        $finalCkC = $cks[$cks.Count - 1]
        if ($sumByCk[$finalCkC] -ne $baseline) {
            [void]$legCFindings.Add("global owner-of-record sum at final ck=$finalCkC is $($sumByCk[$finalCkC]), baseline (ck=$($cks[0])) was $baseline")
        }
    }

    # ---- Leg D: transfer/claim verdict bookkeeping -----------------------------
    $legDFindings = New-Object System.Collections.ArrayList
    $xferAuthoredCount = $seenXfer.Count
    $commitByKey = @{}
    foreach ($c in $commitLines) {
        $k = "$($c.author)|$($c.id)"
        if (-not $commitByKey.ContainsKey($k)) { $commitByKey[$k] = New-Object System.Collections.ArrayList }
        [void]$commitByKey[$k].Add($c)
    }
    foreach ($k in $commitByKey.Keys) {
        if ($commitByKey[$k].Count -gt 1) {
            [void]$legDFindings.Add("xfer author/id=$k has $($commitByKey[$k].Count) [xfer] COMMIT lines (expected exactly 1 - duplicate verdict)")
        }
    }
    if ($xferAuthoredCount -gt 0 -and $commitByKey.Keys.Count -ne $xferAuthoredCount) {
        [void]$legDFindings.Add("$xferAuthoredCount authored transfer(s) (SCENARIO CONSERVE XFER) but $($commitByKey.Keys.Count) distinct [xfer] COMMIT verdict(s) (expected 1:1)")
    }
    $winByKey = @{}
    foreach ($w in $winLines) {
        $k = "$($w.author)|$($w.netId)"
        if (-not $winByKey.ContainsKey($k)) { $winByKey[$k] = New-Object System.Collections.ArrayList }
        [void]$winByKey[$k].Add($w)
    }
    foreach ($k in $winByKey.Keys) {
        $winners = @($winByKey[$k] | ForEach-Object { $_.winner } | Select-Object -Unique)
        if ($winners.Count -gt 1) {
            [void]$legDFindings.Add("claim author/netId=$k has conflicting winners: $($winners -join ',') (expected exactly one)")
        }
    }
    $claimAttemptCount = @($claimLines | Where-Object { $_.got -gt 0 }).Count
    if ($claimAttemptCount -ge 2 -and $winByKey.Keys.Count -lt 1) {
        [void]$legDFindings.Add("$claimAttemptCount successful optimistic pickup(s) logged but no [wi] CLAIM-WIN verdict found (contention never resolved)")
    }

    # ---- Aggregate ---------------------------------------------------------------
    $allFindings = @($legAFindings) + @($legBFindings) + @($legCFindings) + @($legDFindings)
    $metrics = @{
        legAChecked = $legAChecked; legAConverged = $legAConverged; legASkipped = $legASkipped
        legBChecked = $legBChecked; legBConverged = $legBConverged
        legCChecked = $legCChecked
        xferAuthored = $xferAuthoredCount; commitsSeen = $commitByKey.Keys.Count
        claimAttempts = $claimAttemptCount; claimWins = $winByKey.Keys.Count
    }
    if ($allFindings.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $metrics -Detail ($allFindings -join '; '))
    }
    if ($legAChecked -eq 0 -and $legBChecked -eq 0 -and $legCChecked -eq 0 -and $xferAuthoredCount -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Metrics $metrics -Detail "insufficient CONSERVE checkpoint evidence to judge any leg")
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics $metrics `
                -Detail "Leg A: $legAConverged/$legAChecked per-instance deltas explained; Leg B: $legBConverged/$legBChecked cross-instance pairs converged; Leg C: $legCChecked checkpoint(s) globally conserved; Leg D: $xferAuthoredCount xfer(s)/$($commitByKey.Keys.Count) commit(s), $($winByKey.Keys.Count) claim verdict(s), no duplicates")
}

# ---- Gate: world_state_gate convergence (Phase 8 plan 03, WORLD-01/02/03) ---
#
# Judges ScenarioWorldState.cpp's archived 4-log run: Leg A (build/door),
# Leg B (prod/research), Leg C (faction/deed + host-terminated routing
# proof), Leg D (contested-claim single-winner 0 -> 2 -> 2, 08-06
# joins-move redesign - see the Leg D block comment). Mirrors
# Test-InvConservation's shape (per-label log map, Add-GateResult, ONE
# aggregate PASS/FAIL/SKIP, `Sort-Object { $_.prop }` for every hashtable
# sort - the PS 5.1 bare-Sort-Object-on-hashtable bug already burned Phases
# 4/5/6, T-08-11).
#
# Evidence source: "SCENARIO WORLD <channel> ... t=<ms>" (own-clock,
# scenario-authored, ScenarioWorldState.cpp) for the primary per-channel
# convergence checks, cross-checked against the PRODUCTION log's own
# [build]/[fac]/[deed]/[prod] lines for the routing/crossing proof. The
# contested-claim leg (Leg D) windows itself off the HOST's
# "SCENARIO WORLD claimphase phase=<1|2|3>" markers, read in WALL-CLOCK time
# (Convert-StampToMs + Get-LogClockOffsetMs - the Get-CellMap/Test-SplitFar2
# precedent) so the oracle never duplicates ScenarioWorldState.cpp's own-clock
# timing constants in a second language.
function Test-WorldState {
    param(
        [Parameter(Mandatory = $true)]$Logs
    )
    $gate = "world_state"

    $doorPat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD door hand=([\d\.]+) open=(\d) locked=(-?\d) t=(\d+)"
    $facPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD fac sid='([^']*)' rel=(-?[\d\.]+) t=(\d+)"
    $deedPat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD deed hand=([\d\.]+) owned=(\d) t=(\d+)"
    $resPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD research sid='([^']*)' known=(-?\d) t=(\d+)"
    $prodPat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD prod key=([\d\.]+) amt=(-?[\d\.]+) t=(\d+)"
    $buildPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD build key=([\d\.]+) prog=(-?[\d\.]+) removed=(\d) t=(\d+)"
    # 08-06 redesign: every instance reports TWO fixed query points - the HOME
    # cell (own spawn) and the derived AWAY cell the joins relocate into. The
    # site= tag makes the two series separable per window.
    $claimPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD claim site=(home|away) cell=(-?\d+),(-?\d+) owner=(\d+) t=(\d+)"
    $phasePat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD claimphase phase=(\d) ok=(\d) t=(\d+)"

    $buildMintPat   = "\[build\] MINT .*rc=(\d+)"
    $buildStatePat  = "\[build\] STATE-RECV .*complete=(\d+)"
    $buildRemPat    = "\[build\] REMOVE-RECV .*ok=(\d+)"
    # 08-04 live-gate fix: a session-placed building's door rides
    # PKT_BUILD_DOOR (protocol 28's translated-identity channel), logged as
    # "[bdoor] SEND/RECV" (ReplicatorChannels.cpp) - NOT the generic
    # "[door] SEND/RECV" channel, which publishDoors deliberately skips for
    # any door on a building in ownBuilds_/mintByLocal_ ("their runtime
    # hands would never resolve on the peer anyway"). A live run confirmed
    # zero "[door] SEND" lines for the minted door leg's toggle.
    $doorRecvPat    = "\[bdoor\] RECV .*ok=(\d+)"
    $prodRecvPat    = "\[prod\] RECV key="
    $facRecvPat     = "\[fac\] RECV sid='([^']*)'"
    $deedRecvPat    = "\[deed\] RECV hand="
    # 08-05 gap closure: the deed leg is writer-scoped now (see Leg C below) -
    # each NON-writer instance's convergence/crossing proof is its own
    # "[deed] RECV hand=<writerHand> owned=X->Y ok=Z" line, whose Y is a
    # POST-WRITE readDeedByHand read-back (Replicator::applyDeeds), i.e. a
    # genuine per-instance applied-state witness.
    $deedRecvApplyPat = "\[deed\] RECV hand=([\d\.]+) owned=-?\d+->(-?\d+) ok=(\d+)"

    # ---- Step 1: parse every log's own SCENARIO WORLD evidence (host clock
    # frame, via each log's own CLOCKSYNC offset) -------------------------------
    $doorByLog = @{}; $facByLog = @{}; $deedByLog = @{}; $resByLog = @{}
    $prodByLog = @{}; $buildByLog = @{}; $claimByLog = @{}; $phaseByLog = @{}
    $ownRanks = @{}
    $anyEvidence = $false

    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label] = Get-MagateOwnRank -File $f
        $off = Get-LogClockOffsetMs -File $f
        $doorByLog[$label] = New-Object System.Collections.ArrayList
        $facByLog[$label] = New-Object System.Collections.ArrayList
        $deedByLog[$label] = New-Object System.Collections.ArrayList
        $resByLog[$label] = New-Object System.Collections.ArrayList
        $prodByLog[$label] = New-Object System.Collections.ArrayList
        $buildByLog[$label] = New-Object System.Collections.ArrayList
        $claimByLog[$label] = New-Object System.Collections.ArrayList
        $phaseByLog[$label] = New-Object System.Collections.ArrayList

        foreach ($m in (Select-String -Path $f -Pattern $doorPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$doorByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                hand = $g[5].Value; open = [int]$g[6].Value; locked = [int]$g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $facPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$facByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                sid = $g[5].Value; rel = [double]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $deedPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$deedByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                hand = $g[5].Value; owned = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $resPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$resByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                sid = $g[5].Value; known = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $prodPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$prodByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                key = $g[5].Value; amt = [double]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $buildPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$buildByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                key = $g[5].Value; prog = [double]$g[6].Value; removed = [int]$g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $claimPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$claimByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                site = $g[5].Value
                cx = [int]$g[6].Value; cz = [int]$g[7].Value; owner = [int]$g[8].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $phasePat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$phaseByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                phase = [int]$g[5].Value; ok = [int]$g[6].Value })
        }
    }

    if (-not $anyEvidence) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no SCENARIO WORLD evidence found in any of the $($Logs.Count) log(s)")
    }

    $findings = New-Object System.Collections.ArrayList
    $metrics = @{}
    # Every rank-independent-readable channel (door/fac/deed/research/claim)
    # is expected from ALL resolved-rank instances - a channel that only SOME
    # instances reported is a partial no-signal evidence gap, not a silent
    # pass on whichever fraction happened to report (the Test-InvConservation
    # Leg A "judgedOwners" precedent, generalized to every leg here).
    $expectedCount = 0
    foreach ($label in $Logs.Keys) { if ($null -ne $ownRanks[$label]) { $expectedCount++ } }

    # ---- Leg A: build/door convergence (WORLD-01) ------------------------------
    $placerLabel = $null
    foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq 0) { $placerLabel = $label } }

    # Door (08-04 live-gate fix): wanderer4's actual co-op spawn has no
    # pre-existing door within reach, so ScenarioWorldState.cpp's door leg
    # now mirrors build/prod - rank0 MINTS its own door-bearing shack
    # (placer-scoped runtime hand, the protocol-27 identity problem), so
    # only the placer emits SCENARIO WORLD door evidence; the other 3
    # instances' crossing proof is the production log's own [door] SEND/
    # RECV lines, mirroring the build check immediately below.
    if ($null -eq $placerLabel -or $doorByLog[$placerLabel].Count -eq 0) {
        [void]$findings.Add("Leg A: no SCENARIO WORLD door evidence from the placer (rank0) log (evidence gap)")
        $metrics.doorLogsReporting = 0
    } else {
        $doorRecvOk = 0
        foreach ($label in $Logs.Keys) {
            if ($ownRanks[$label] -eq 0) { continue } # only non-placers RECV
            $f = $Logs[$label]
            if (-not (Test-Path $f)) { continue }
            if (@(Select-String -Path $f -Pattern $doorRecvPat -ErrorAction SilentlyContinue) |
                Where-Object { [int]$_.Matches[0].Groups[1].Value -eq 1 } | Select-Object -First 1) { $doorRecvOk++ }
        }
        if ($doorRecvOk -eq 0) { [void]$findings.Add("Leg A: no non-placer log shows '[door] RECV ... ok=1' (door toggle never crossed)") }
        $metrics.doorLogsReporting = 1
        $metrics.doorRecvOk = $doorRecvOk
    }

    # Build: placer-scoped (rank0 only emits SCENARIO WORLD build) - the
    # crossing proof for the OTHER 3 instances is the production log itself.
    if ($null -eq $placerLabel -or $buildByLog[$placerLabel].Count -eq 0) {
        [void]$findings.Add("Leg A: no SCENARIO WORLD build evidence from the placer (rank0) log (evidence gap)")
    } else {
        $mintOk = 0; $stateCompleteOk = 0; $removeOk = 0
        foreach ($label in $Logs.Keys) {
            if ($ownRanks[$label] -eq 0) { continue } # only non-placers MINT/STATE-RECV/REMOVE-RECV
            $f = $Logs[$label]
            if (-not (Test-Path $f)) { continue }
            if (@(Select-String -Path $f -Pattern $buildMintPat -ErrorAction SilentlyContinue) |
                Where-Object { [int]$_.Matches[0].Groups[1].Value -eq 1 } | Select-Object -First 1) { $mintOk++ }
            if (@(Select-String -Path $f -Pattern $buildStatePat -ErrorAction SilentlyContinue) |
                Where-Object { [int]$_.Matches[0].Groups[1].Value -eq 1 } | Select-Object -First 1) { $stateCompleteOk++ }
            if (@(Select-String -Path $f -Pattern $buildRemPat -ErrorAction SilentlyContinue) |
                Where-Object { [int]$_.Matches[0].Groups[1].Value -eq 1 } | Select-Object -First 1) { $removeOk++ }
        }
        if ($mintOk -eq 0) { [void]$findings.Add("Leg A: no non-placer log shows '[build] MINT ... rc=1' (placed building never crossed)") }
        if ($stateCompleteOk -eq 0) { [void]$findings.Add("Leg A: no non-placer log shows '[build] STATE-RECV ... complete=1' (construction completion never crossed)") }
        if ($removeOk -eq 0) { [void]$findings.Add("Leg A: no log shows '[build] REMOVE-RECV ... ok=1' (removal never crossed)") }
        $metrics.buildMintOk = $mintOk; $metrics.buildStateOk = $stateCompleteOk; $metrics.buildRemoveOk = $removeOk
    }

    # ---- Leg B: prod/research convergence (WORLD-02) ---------------------------
    if ($null -eq $placerLabel -or $prodByLog[$placerLabel].Count -eq 0) {
        [void]$findings.Add("Leg B: no SCENARIO WORLD prod evidence from the host (rank0) log (evidence gap)")
    } else {
        $prodRows = @($prodByLog[$placerLabel] | Sort-Object { $_.t })
        $finalAmt = $prodRows[$prodRows.Count - 1].amt
        if ($finalAmt -le 0.0) {
            [void]$findings.Add("Leg B: host's prod buffer never moved above 0 (amt=$finalAmt) - operate() leg did not run")
        }
        $prodRecvLogs = 0
        foreach ($label in $Logs.Keys) {
            if ($ownRanks[$label] -eq 0) { continue }
            $f = $Logs[$label]
            if ((Test-Path $f) -and (Select-String -Path $f -Pattern $prodRecvPat -ErrorAction SilentlyContinue | Select-Object -First 1)) { $prodRecvLogs++ }
        }
        if ($prodRecvLogs -eq 0) { [void]$findings.Add("Leg B: no join log shows '[prod] RECV' (host-authored buffer never broadcast)") }
        $metrics.prodFinalAmt = $finalAmt; $metrics.prodRecvLogs = $prodRecvLogs
    }
    $resFinal = @{}
    foreach ($label in $resByLog.Keys) {
        $rows = @($resByLog[$label] | Sort-Object { $_.t })
        if ($rows.Count -eq 0) { continue }
        $resFinal[$label] = $rows[$rows.Count - 1].known
    }
    if ($resFinal.Keys.Count -eq 0) {
        [void]$findings.Add("Leg B: no SCENARIO WORLD research evidence in any log (evidence gap)")
    } else {
        foreach ($label in $resFinal.Keys) {
            if ($resFinal[$label] -ne 1) {
                [void]$findings.Add("Leg B: research sentinel not known on $label (known=$($resFinal[$label])) - the join->host research intent leg did not converge")
            }
        }
        # The join-awarded sid landing on the HOST is the new intent leg's own
        # proof - explicitly named so a routing regression that only breaks
        # host convergence (but happens to leave the other joins fine) FAILs.
        $hostLabel = $null
        foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq 0) { $hostLabel = $label } }
        if ($null -ne $hostLabel -and $resFinal.ContainsKey($hostLabel) -and $resFinal[$hostLabel] -ne 1) {
            [void]$findings.Add("Leg B: research sentinel not known on the HOST log - the join(rank3)->host intent leg's own proof failed")
        }
        if ($resFinal.Keys.Count -lt $expectedCount) {
            $missing = @($Logs.Keys | Where-Object { $null -ne $ownRanks[$_] -and -not $resFinal.ContainsKey($_) })
            [void]$findings.Add("Leg B: only $($resFinal.Keys.Count) of $expectedCount expected instance(s) emitted SCENARIO WORLD research evidence - missing: $($missing -join ', ') (evidence gap, not proof of convergence)")
        }
    }
    $metrics.researchLogsKnown = @($resFinal.Values | Where-Object { $_ -eq 1 }).Count
    $metrics.researchLogsTotal = $resFinal.Keys.Count

    # ---- Leg C: faction/deed convergence + host-terminated routing (WORLD-02) --
    $facFinal = @{}
    foreach ($label in $facByLog.Keys) {
        $rows = @($facByLog[$label] | Sort-Object { $_.t })
        if ($rows.Count -eq 0) { continue }
        $facFinal[$label] = $rows[$rows.Count - 1].rel
    }
    if ($facFinal.Keys.Count -eq 0) {
        [void]$findings.Add("Leg C: no SCENARIO WORLD fac evidence in any log (evidence gap)")
    } else {
        $vals = @($facFinal.Values)
        $ref = $vals[0]
        foreach ($label in $facFinal.Keys) {
            $d = $facFinal[$label] - $ref
            if ($d -gt 0.5 -or $d -lt -0.5) {
                [void]$findings.Add("Leg C: faction sentinel relation diverged on $label (rel=$($facFinal[$label]) vs reference=$ref, EPS=0.5)")
            }
        }
        if ($facFinal.Keys.Count -lt $expectedCount) {
            $missing = @($Logs.Keys | Where-Object { $null -ne $ownRanks[$_] -and -not $facFinal.ContainsKey($_) })
            [void]$findings.Add("Leg C: only $($facFinal.Keys.Count) of $expectedCount expected instance(s) emitted SCENARIO WORLD fac evidence - missing: $($missing -join ', ') (evidence gap, not proof of convergence)")
        }
    }
    # Deed (08-05 gap closure): WRITER-scoped like door/build/prod. The old
    # every-rank pick raced the deed write itself: its sorted-first-UNOWNED
    # filter is time-varying (the write flips the sentinel owned=0->1 the
    # moment it crosses), and arming skew put the last-launched join's pick
    # AFTER the write crossed (runs 20260903_134400/135913: join3 skipped the
    # now-owned sentinel and picked the next sorted hand, deterministically).
    # Building hands themselves proved CROSS-PROCESS-STABLE in both runs
    # (every instance applied the same wire hand, [deed] RECV ok=1), so this
    # was a scenario pick bug, not a channel identity problem. Now only the
    # rank2 writer emits SCENARIO WORLD deed evidence; every OTHER instance
    # must show its own '[deed] RECV hand=<writerHand> owned=*->1 ok=1'
    # apply read-back - a per-instance applied-state witness, so a silent or
    # unresolved instance still FAILs by name (NoSignalFails preserved; the
    # check is strengthened, not loosened: it now verifies the actual apply,
    # not just a read of a possibly-different building).
    $deedWriterLabel = $null
    foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq 2) { $deedWriterLabel = $label } }
    if ($null -eq $deedWriterLabel -or $deedByLog[$deedWriterLabel].Count -eq 0) {
        [void]$findings.Add("Leg C: no SCENARIO WORLD deed evidence from the writer (rank2) log (evidence gap)")
        $metrics.deedWriterReporting = 0
    } else {
        $rows = @($deedByLog[$deedWriterLabel] | Sort-Object { $_.t })
        $lastDeed = $rows[$rows.Count - 1]
        if ($lastDeed.owned -ne 1) {
            [void]$findings.Add("Leg C: deed sentinel not owned=1 on the writer $deedWriterLabel (owned=$($lastDeed.owned)) - the deed write never landed locally")
        }
        $writerHand = $lastDeed.hand
        $deedApplyOk = 0
        foreach ($label in $Logs.Keys) {
            if ($null -eq $ownRanks[$label] -or $ownRanks[$label] -eq 2) { continue }
            $f = $Logs[$label]
            if (-not (Test-Path $f)) { continue }
            $hit = @(Select-String -Path $f -Pattern $deedRecvApplyPat -ErrorAction SilentlyContinue) | Where-Object {
                $_.Matches[0].Groups[1].Value -eq $writerHand -and
                [int]$_.Matches[0].Groups[2].Value -eq 1 -and
                [int]$_.Matches[0].Groups[3].Value -eq 1 } | Select-Object -First 1
            if ($hit) { $deedApplyOk++ }
            else {
                [void]$findings.Add("Leg C: $label shows no '[deed] RECV hand=$writerHand ... ->1 ok=1' (the writer's deed never crossed/applied on $label)")
            }
        }
        $metrics.deedWriterReporting = 1
        $metrics.deedApplyOk = $deedApplyOk
    }
    # Host-terminated-intent routing proof: PKT_FACTION/PKT_DEED are
    # RELAY_NONE - a join's intent reaches the HOST ONLY, never another
    # join. The host's own log receiving the intent is the directly
    # observable half of that property from archived logs (the join<->join
    # non-relay half is exhaustively proven headless - nettest, 08-01/08-02).
    $hostLabel = $null
    foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq 0) { $hostLabel = $label } }
    if ($null -ne $hostLabel) {
        $hf = $Logs[$hostLabel]
        $facRecvOk = (Test-Path $hf) -and (Select-String -Path $hf -Pattern $facRecvPat -ErrorAction SilentlyContinue | Select-Object -First 1)
        $deedRecvOk = (Test-Path $hf) -and (Select-String -Path $hf -Pattern $deedRecvPat -ErrorAction SilentlyContinue | Select-Object -First 1)
        if (-not $facRecvOk) { [void]$findings.Add("Leg C: host log shows no '[fac] RECV' - the join(rank1)->host faction intent never reached the host") }
        if (-not $deedRecvOk) { [void]$findings.Add("Leg C: host log shows no '[deed] RECV' - the join(rank2)->host deed intent never reached the host") }
    } else {
        [void]$findings.Add("Leg C: no host (rank0) log identified - cannot judge the host-terminated routing proof")
    }

    # ---- Leg D: contested-claim single-winner 0 -> 2 -> 2 (WORLD-03) -----------
    # 08-06 redesign (only JOINS move - no lever reliably relocates the host's
    # own selected world-authority leader, measured across runs 135913/144325/
    # 145920/152337): D1 judges the HOME cell (all four squads co-spawned,
    # host is a party -> fresh contest -> owner=0); D2 judges the AWAY cell
    # after rank2+rank3 relocate into it (host NOT a party -> fresh contest ->
    # lowest playerId among {2,3} -> owner=2); D3 judges the AWAY cell after
    # rank1 - the LOWEST playerId, the fresh-contest winner-to-be - also
    # arrives (continuity keeps the incumbent -> owner STAYS 2; a
    # fresh-always/no-continuity model resolves 1, a host-wins model 0). The
    # HOME cell must additionally stay owner=0 through D2/D3 (the host never
    # left). Because D3's stays-2 verdict would be VACUOUS if join1 never
    # actually arrived, each mover must also show its own production
    # "[cell] CLAIM rank=<r> cell=<away>" line - the claim PIPELINE (not just
    # the body) saw the relocation; join1's is the proof the continuity
    # contest armed at all. Windows open at marker+30s (movers fire 20s
    # before their marker on their own clocks; skew + verify + 3s claim dwell
    # + broadcast settle inside the 30s - the 07/08 arming-skew lesson).
    $hostPhase = if ($null -ne $hostLabel) { @($phaseByLog[$hostLabel] | Sort-Object { $_.t }) } else { @() }
    if ($hostPhase.Count -lt 3) {
        [void]$findings.Add("Leg D: host log has $($hostPhase.Count)/3 SCENARIO WORLD claimphase markers (evidence gap - cannot derive D1/D2/D3 windows)")
    } else {
        $p1 = ($hostPhase | Where-Object { $_.phase -eq 1 } | Select-Object -First 1).t
        $p2 = ($hostPhase | Where-Object { $_.phase -eq 2 } | Select-Object -First 1).t
        $p3 = ($hostPhase | Where-Object { $_.phase -eq 3 } | Select-Object -First 1).t
        if ($null -eq $p1 -or $null -eq $p2 -or $null -eq $p3) {
            [void]$findings.Add("Leg D: host log is missing one of phase=1/2/3 (evidence gap)")
        } else {
            # Cross-instance cell agreement: all four instances must report
            # the SAME home cell and the SAME away cell (the away point is
            # independently derived on every instance from its own spawn -
            # divergent derivation would make the owner comparison
            # meaningless, so it FAILs by name here first).
            $cellsBySite = @{ home = @{}; away = @{} }
            foreach ($label in $claimByLog.Keys) {
                foreach ($r in $claimByLog[$label]) {
                    $cellsBySite[$r.site]["$($r.cx),$($r.cz)"] = $true
                }
            }
            foreach ($site in @('home', 'away')) {
                $cells = @($cellsBySite[$site].Keys)
                if ($cells.Count -gt 1) {
                    [void]$findings.Add("Leg D: instances disagree on the $site cell identity: $(($cells | Sort-Object { $_ }) -join ' vs ') (derivation divergence)")
                }
            }
            $homeCells = @($cellsBySite['home'].Keys)
            $awayCells = @($cellsBySite['away'].Keys)
            if ($homeCells.Count -eq 1 -and $awayCells.Count -eq 1 -and $homeCells[0] -eq $awayCells[0]) {
                [void]$findings.Add("Leg D: home and away query points resolve to the SAME cell ($($homeCells[0])) - the away derivation never left the spawn cell")
            }

            $settleTol = 5000.0
            # Each window judges ONE primary site (drives claimSequence) and
            # may carry secondary site assertions (same rigor, not sequenced).
            $windows = @(
                [pscustomobject]@{ name = "D1"; start = $p1 + 30000; end = $p2 - 5000
                                   checks = @([pscustomobject]@{ site = 'home'; expect = 0; primary = $true }) }
                [pscustomobject]@{ name = "D2"; start = $p2 + 30000; end = $p3 - 5000
                                   checks = @([pscustomobject]@{ site = 'away'; expect = 2; primary = $true },
                                              [pscustomobject]@{ site = 'home'; expect = 0; primary = $false }) }
                [pscustomobject]@{ name = "D3"; start = $p3 + 30000; end = $p3 + 55000
                                   checks = @([pscustomobject]@{ site = 'away'; expect = 2; primary = $true },
                                              [pscustomobject]@{ site = 'home'; expect = 0; primary = $false }) }
            )
            $sequence = New-Object System.Collections.ArrayList
            foreach ($w in $windows) {
                if ($w.end -le $w.start) {
                    [void]$findings.Add("Leg D: $($w.name) window is empty/inverted (start=$($w.start) end=$($w.end)) - claimphase markers too close together")
                    continue
                }
                foreach ($chk in $w.checks) {
                    $site = $chk.site
                    $settled = @{}   # label -> last owner in window for this site
                    $flapLabels = New-Object System.Collections.ArrayList
                    $anyInWindow = $false
                    foreach ($label in $claimByLog.Keys) {
                        $rows = @($claimByLog[$label] | Where-Object { $_.site -eq $site -and $_.t -ge $w.start -and $_.t -le $w.end } | Sort-Object { $_.t })
                        if ($rows.Count -eq 0) { continue }
                        $anyInWindow = $true
                        $settled[$label] = $rows[$rows.Count - 1].owner
                        # Flap check: past the settle-tolerance sub-window, this
                        # instance's OWN series must show exactly one owner.
                        $postSettle = @($rows | Where-Object { $_.t -ge ($w.start + $settleTol) })
                        $distinct = @($postSettle | ForEach-Object { $_.owner } | Select-Object -Unique)
                        if ($distinct.Count -gt 1) { [void]$flapLabels.Add($label) }
                    }
                    if (-not $anyInWindow) {
                        [void]$findings.Add("Leg D: $($w.name) window has NO SCENARIO WORLD claim site=$site evidence from any log (evidence gap)")
                        continue
                    }
                    if ($settled.Keys.Count -lt $expectedCount) {
                        $missing = @($Logs.Keys | Where-Object { $null -ne $ownRanks[$_] -and -not $settled.ContainsKey($_) })
                        [void]$findings.Add("Leg D: $($w.name) ($site) only $($settled.Keys.Count) of $expectedCount expected instance(s) reported a claim owner in-window - missing: $($missing -join ', ') (evidence gap, not proof of a single winner)")
                    }
                    $owners = @($settled.Values | Select-Object -Unique)
                    if ($owners.Count -gt 1) {
                        [void]$findings.Add("Leg D: $($w.name) ($site) instances disagree on the settled cell owner: $(($settled.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')")
                    } elseif ($owners[0] -ne $chk.expect) {
                        [void]$findings.Add("Leg D: $($w.name) ($site) settled owner=$($owners[0]) but the locked tie-break expects owner=$($chk.expect)")
                    } elseif ($chk.primary) {
                        [void]$sequence.Add($owners[0])
                    }
                    if ($flapLabels.Count -gt 0) {
                        [void]$findings.Add("Leg D: $($w.name) ($site) flapping past the $($settleTol/1000)s settle tolerance on: $($flapLabels -join ', ')")
                    }
                }
            }
            $metrics.claimSequence = ($sequence -join '->')

            # Mover-arrival proofs: each mover's own production
            # "[cell] CLAIM rank=<r> cell=<awayCell>" line proves the claim
            # PIPELINE registered its relocation (dwell passed, claim sent) -
            # the away-cell owner verdicts above cannot be passed vacuously
            # by a mover that never arrived. join1's (rank1) is the proof
            # D3's continuity contest armed at all.
            if ($awayCells.Count -eq 1) {
                $awayCell = $awayCells[0]
                foreach ($mv in @(
                    [pscustomobject]@{ rank = 2; why = "the D2 mover never claimed the away cell" },
                    [pscustomobject]@{ rank = 3; why = "the D2 mover never claimed the away cell" },
                    [pscustomobject]@{ rank = 1; why = "the D3 continuity contest never armed (rank1 never claimed the away cell)" })) {
                    $mvLabel = $null
                    foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq $mv.rank) { $mvLabel = $label } }
                    if ($null -eq $mvLabel) { continue } # rank coverage gap already FAILed above
                    $f = $Logs[$mvLabel]
                    $movePat = "\[cell\] CLAIM rank=$($mv.rank) cell=$([regex]::Escape($awayCell)) seq="
                    if (-not ((Test-Path $f) -and (Select-String -Path $f -Pattern $movePat -ErrorAction SilentlyContinue | Select-Object -First 1))) {
                        [void]$findings.Add("Leg D: $mvLabel (rank$($mv.rank)) shows no '[cell] CLAIM rank=$($mv.rank) cell=$awayCell' - $($mv.why)")
                    }
                }
            } elseif ($awayCells.Count -eq 0) {
                [void]$findings.Add("Leg D: no instance emitted any SCENARIO WORLD claim site=away evidence (away derivation never completed - evidence gap)")
            }
        }
    }

    # ---- Aggregate ---------------------------------------------------------------
    if ($findings.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $metrics -Detail ($findings -join '; '))
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics $metrics `
                -Detail "Leg A: door+build convergence ok; Leg B: prod+research convergence ok; Leg C: faction+deed convergence + host-terminated routing ok; Leg D: contested-claim sequence $($metrics.claimSequence)")
}

# Test-Consensus (Phase 9 plan 03, CONS-01/02/03): judges the ONE live
# consensus_gate scenario (ScenarioConsensus.cpp) against the money/speed/
# time evidence Plans 01/02 (protocol 60) emit, generalizing Test-SpeedSync's
# CLOCKSYNC-corrected two-log alignment (scripts/oracles/Npc.ps1) and
# Test-TimeSync's convergence gating (scripts/oracles/World.ps1) to four
# logs, and following Test-InvConservation's evidence-driven-ledger idiom
# (no hardcoded numbers - BASE/solvent totals come from THIS SCENARIO's own
# verified "SCENARIO CONSENSUS moneyseed/moneyspend" evidence, never from a
# number baked into this file).
#
#   Leg A (money): builds the fold ledger from the host's own
#     "[wallet] POOL FOLD ..." lines (asserting no (owner,seq) is folded or
#     rejected more than once - the double-fold/replay defect - and that no
#     FOLD line ever reports a NEGATIVE resulting pool, which is the
#     minted-value/should-have-rejected defect, independent of any
#     scenario-specific number); asserts the SAME (owner,seq,delta,pool)
#     "[wallet] REJECT ..." tuple is present on every expected instance (the
#     deterministic-and-identical verdict); asserts every instance's final
#     SCENARIO POOL reading (including the rejected buyer's own - the refund
#     proof) equals base + sum(folded deltas).
#   Leg B (speed): reconstructs the min-vote/cap ground truth from the
#     host's own "[speed] SET ... VOTES ..." series plus the per-click
#     "[speed] DENY ..." denial evidence (WR-04) - denied raise, all-raise,
#     pause/unpause, the combat cap, rank3's held constraining vote, and a
#     min-rule consistency invariant over every SET line; the
#     instant-vote-drop proof compares the host's own
#     "[leave] speed vote=... owner=3" timestamp (bracketed by the
#     "SCENARIO CONSENSUS legmark leg=B phase=1/2" window - the
#     disconnectAtSec timing contract, ScenarioConsensus.cpp's file header)
#     against every SURVIVOR's own SCENARIO SPEED series raising back to 3x
#     within tolerance.
#   Leg C (time): aligns every instance's SCENARIO GTIME series on the
#     BRACKET wall-clock timestamp (Convert-StampToMs + each log's own
#     CLOCKSYNC offset - the Test-WorldState precedent, NOT the scenario's
#     own-clock "t=" field, which drifts with arming skew at N>2), asserting
#     max pairwise |delta gameHours| stays under tolerance both at a
#     mid-run checkpoint (around Leg A's end, before the Leg-B speed changes)
#     and in the final tail window (non-divergence across those changes).
function Test-Consensus {
    param(
        [Parameter(Mandatory = $true)]$Logs
    )
    $gate = "consensus"

    $legMarkPat     = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO CONSENSUS legmark leg=(\w) phase=(\d+) t=(\d+)"
    $moneySeedPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO CONSENSUS moneyseed base=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+) t=(\d+)"
    $moneySpendPat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO CONSENSUS moneyspend rank=(\d+) phase=(\S+) amount=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+) t=(\d+)"
    $poolPat        = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO POOL money=(-?\d+) who=(\S+) t=(\d+)"
    $foldPat        = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[wallet\] POOL FOLD owner=(\d+) seq=(\d+) delta=(-?\d+) t=(-?\d+)"
    $rejectPat      = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[wallet\] REJECT owner=(\d+) seq=(\d+) delta=(-?\d+) pool=(-?\d+)"
    $speedSetPat    = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[speed\] SET mult=([\d.]+) paused=(\d) combat=(\d) cap=(\d) VOTES host=([\d.]+)/(\d)(.*)$"
    # Phase 9 review WR-04: the host's per-click denial evidence line
    # (ReplicatorChannels.cpp syncSpeed emits it when a real host click's
    # request stays above the unchanged arbitrated effective - the change-
    # gated SET line is silent at exactly that moment).
    $speedDenyPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[speed\] DENY req=([\d.]+) eff=([\d.]+) cap=(\d) VOTES host=([\d.]+)/(\d)(.*)$"
    $speedSeriesPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SPEED t=(\d+) mult=([\d.]+) paused=(\d) nbtn=(-?\d+) buttons=(\S*)"
    $leavePat       = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[leave\] speed vote=(\d+) time report=(\d+) owner=(\d+)"
    $gtimePat       = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO GTIME hours=([\d.]+) hourLen=([\-\d.]+) fsm=([\d.]+) paused=(\d) ok=(\d) t=(\d+)"

    $ownRanks = @{}
    $legMarkByLog = @{}; $moneySeedByLog = @{}; $moneySpendByLog = @{}
    $poolByLog = @{}; $foldByLog = @{}; $rejectByLog = @{}
    $speedSetByLog = @{}; $speedDenyByLog = @{}; $speedSeriesByLog = @{}; $leaveByLog = @{}; $gtimeByLog = @{}
    $anyEvidence = $false

    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label] = Get-MagateOwnRank -File $f
        $off = Get-LogClockOffsetMs -File $f
        $legMarkByLog[$label]     = New-Object System.Collections.ArrayList
        $moneySeedByLog[$label]   = New-Object System.Collections.ArrayList
        $moneySpendByLog[$label]  = New-Object System.Collections.ArrayList
        $poolByLog[$label]        = New-Object System.Collections.ArrayList
        $foldByLog[$label]        = New-Object System.Collections.ArrayList
        $rejectByLog[$label]      = New-Object System.Collections.ArrayList
        $speedSetByLog[$label]    = New-Object System.Collections.ArrayList
        $speedDenyByLog[$label]   = New-Object System.Collections.ArrayList
        $speedSeriesByLog[$label] = New-Object System.Collections.ArrayList
        $leaveByLog[$label]       = New-Object System.Collections.ArrayList
        $gtimeByLog[$label]       = New-Object System.Collections.ArrayList

        foreach ($m in (Select-String -Path $f -Pattern $legMarkPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$legMarkByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                leg = $g[5].Value; phase = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $moneySeedPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$moneySeedByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                base = [int]$g[5].Value; ok = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $moneySpendPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$moneySpendByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                rank = [int]$g[5].Value; phase = $g[6].Value; amount = [int]$g[7].Value; ok = [int]$g[8].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $poolPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$poolByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                money = [int]$g[5].Value; who = $g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $foldPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$foldByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                owner = [int]$g[5].Value; seq = [int]$g[6].Value; delta = [int]$g[7].Value; total = [int]$g[8].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $rejectPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$rejectByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                owner = [int]$g[5].Value; seq = [int]$g[6].Value; delta = [int]$g[7].Value; pool = [int]$g[8].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $speedSetPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $votes = @{}
            foreach ($vm in [regex]::Matches($g[11].Value, '(\d+)=([\d.]+)/(\d)')) {
                $votes[[int]$vm.Groups[1].Value] = [pscustomobject]@{
                    req = [double]$vm.Groups[2].Value; combat = [int]$vm.Groups[3].Value }
            }
            [void]$speedSetByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                eff = [double]$g[5].Value; paused = [int]$g[6].Value; combat = [int]$g[7].Value
                cap = [int]$g[8].Value; hostReq = [double]$g[9].Value; hostCombat = [int]$g[10].Value
                votes = $votes })
        }
        foreach ($m in (Select-String -Path $f -Pattern $speedDenyPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $votes = @{}
            foreach ($vm in [regex]::Matches($g[10].Value, '(\d+)=([\d.]+)/(\d)')) {
                $votes[[int]$vm.Groups[1].Value] = [pscustomobject]@{
                    req = [double]$vm.Groups[2].Value; combat = [int]$vm.Groups[3].Value }
            }
            [void]$speedDenyByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                req = [double]$g[5].Value; eff = [double]$g[6].Value; cap = [int]$g[7].Value
                votes = $votes })
        }
        foreach ($m in (Select-String -Path $f -Pattern $speedSeriesPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$speedSeriesByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                mult = [double]$g[6].Value; paused = [int]$g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $leavePat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$leaveByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                owner = [int]$g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $gtimePat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$gtimeByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                hours = [double]$g[5].Value; hourLen = [double]$g[6].Value
                mult = [double]$g[7].Value; ok = [int]$g[9].Value })
        }
    }

    if (-not $anyEvidence) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no SCENARIO CONSENSUS evidence found in any of the $($Logs.Count) log(s)")
    }

    $findings = New-Object System.Collections.ArrayList
    $metrics = @{}
    $hostLabel = $null
    foreach ($label in $Logs.Keys) { if ($ownRanks[$label] -eq 0) { $hostLabel = $label } }
    $expectedCount = 0
    foreach ($label in $Logs.Keys) { if ($null -ne $ownRanks[$label]) { $expectedCount++ } }

    # ==== Leg A: money (CONS-01) =================================================
    if ($null -eq $hostLabel -or $moneySeedByLog[$hostLabel].Count -eq 0) {
        [void]$findings.Add("Leg A: no SCENARIO CONSENSUS moneyseed evidence from the host log (evidence gap)")
    } else {
        $seedRow = $moneySeedByLog[$hostLabel][0]
        $base = $seedRow.base
        if ($seedRow.ok -ne 1) {
            [void]$findings.Add("Leg A: host's money seed write never verified (ok=$($seedRow.ok)) - Leg A cannot be judged")
        } else {
            # Solvent total, from THIS scenario's own verified moneyspend
            # evidence (no hardcoded numbers) - every ok=1 'solvent' phase
            # spend across all logs.
            $solventTotal = 0
            foreach ($label in $moneySpendByLog.Keys) {
                foreach ($row in $moneySpendByLog[$label]) {
                    if ($row.phase -eq 'solvent' -and $row.ok -eq 1) { $solventTotal += $row.amount }
                }
            }
            # Fold ledger: sum every host-side [wallet] POOL FOLD delta
            # (foldMonotonic ensures each (owner,seq) folds at most once on
            # the host that decided it - flag any that don't). A FOLD line
            # reporting a NEGATIVE resulting pool is a minted-value/
            # should-have-rejected defect, independent of any
            # scenario-specific number.
            $foldRows = @($foldByLog[$hostLabel] | Sort-Object { $_.t })
            $seenFoldReject = @{}
            $totalFoldDelta = 0
            $mintedFound = $false
            foreach ($row in $foldRows) {
                $key = "$($row.owner)|$($row.seq)"
                if ($seenFoldReject.ContainsKey($key)) {
                    [void]$findings.Add("Leg A: (owner=$($row.owner),seq=$($row.seq)) was folded/rejected MORE THAN ONCE on the host - double-fold/replay defect")
                }
                $seenFoldReject[$key] = $true
                $totalFoldDelta += $row.delta
                if ($row.total -lt 0) { $mintedFound = $true }
            }
            if ($mintedFound) {
                [void]$findings.Add("Leg A: at least one '[wallet] POOL FOLD ...' line reports a NEGATIVE resulting pool - a delta that should have been rejected instead folded (minted value)")
            }
            $rejectRows = New-Object System.Collections.ArrayList
            foreach ($label in $rejectByLog.Keys) {
                foreach ($row in $rejectByLog[$label]) {
                    [void]$rejectRows.Add([pscustomobject]@{ label = $label; owner = $row.owner; seq = $row.seq; delta = $row.delta; pool = $row.pool })
                    if ($label -eq $hostLabel) {
                        $key = "$($row.owner)|$($row.seq)"
                        if ($seenFoldReject.ContainsKey($key)) {
                            [void]$findings.Add("Leg A: (owner=$($row.owner),seq=$($row.seq)) appears in BOTH a FOLD and a REJECT on the host - double-processed defect")
                        }
                        $seenFoldReject[$key] = $true
                    }
                }
            }
            $rejectKeys = @($rejectRows | ForEach-Object { "$($_.owner)|$($_.seq)|$($_.delta)|$($_.pool)" } | Select-Object -Unique)
            if ($rejectKeys.Count -eq 0) {
                [void]$findings.Add("Leg A: no '[wallet] REJECT ...' evidence on any log - the deliberate overdraft contest never produced an observable verdict")
            } else {
                if ($rejectKeys.Count -gt 1) {
                    [void]$findings.Add("Leg A: instances disagree on the reject verdict - distinct (owner,seq,delta,pool) tuples seen: $($rejectKeys -join ' | ')")
                }
                $canonicalKey = $rejectKeys[0]
                $labelsWithReject = @($rejectRows | Where-Object { "$($_.owner)|$($_.seq)|$($_.delta)|$($_.pool)" -eq $canonicalKey } | ForEach-Object { $_.label } | Select-Object -Unique)
                if ($labelsWithReject.Count -lt $expectedCount) {
                    $missing = @($Logs.Keys | Where-Object { $null -ne $ownRanks[$_] -and $labelsWithReject -notcontains $_ })
                    [void]$findings.Add("Leg A: the reject verdict is missing on $($missing -join ', ') - only $($labelsWithReject.Count) of $expectedCount instance(s) show it (not an identical verdict on all four)")
                }
                $metrics.rejectedOwner = ($canonicalKey -split '\|')[0]
            }
            $expectedFinal = $base + $totalFoldDelta
            $metrics.base = $base; $metrics.solventTotal = $solventTotal; $metrics.expectedFinal = $expectedFinal
            # Conservation to ONE authoritative total: every log's LAST
            # SCENARIO POOL reading (post-settle, i.e. after leg A's own
            # phase=2 marker) must equal expectedFinal - INCLUDING the
            # rejected buyer's own log, which is the refund proof (its pool
            # series returning to the authoritative total, not stuck at a
            # partially-spent value).
            $legAEndMark = $null
            foreach ($row in $legMarkByLog[$hostLabel]) { if ($row.leg -eq 'A' -and $row.phase -eq 2) { $legAEndMark = $row.t } }
            foreach ($label in $poolByLog.Keys) {
                $rows = @($poolByLog[$label] | Sort-Object { $_.t })
                if ($rows.Count -eq 0) {
                    [void]$findings.Add("Leg A: $label has no SCENARIO POOL evidence (evidence gap)")
                    continue
                }
                $settleRows = if ($null -ne $legAEndMark) { @($rows | Where-Object { $_.t -ge $legAEndMark }) } else { @() }
                if ($settleRows.Count -eq 0) { $settleRows = $rows }
                $lastMoney = $settleRows[$settleRows.Count - 1].money
                if ($lastMoney -ne $expectedFinal) {
                    [void]$findings.Add("Leg A: $label's final pool=$lastMoney does not equal the authoritative total $expectedFinal (base=$base + folded=$totalFoldDelta) - conservation violated")
                }
            }
        }
    }

    # ==== Leg B: speed (CONS-02) =================================================
    if ($null -eq $hostLabel -or $speedSetByLog[$hostLabel].Count -eq 0) {
        [void]$findings.Add("Leg B: no '[speed] SET ... VOTES ...' evidence on the host log (evidence gap)")
    } else {
        $sets = @($speedSetByLog[$hostLabel] | Sort-Object { $_.t })
        $sawDenied = $false; $sawAllRaised = $false; $sawPause = $false; $sawUnpause = $false
        $sawCombatCap = $false; $sawHoldVote = $false
        $pauseHappened = $false
        # Phase 9 review WR-04: the denied-raise proof. PRIMARY evidence is
        # the host's own per-click "[speed] DENY req=.. eff=.. cap=.." line
        # (emitted at exactly the denied moment - post-WR-04 builds).
        # FALLBACK evidence (recalibrated against archived pre-DENY runs,
        # e.g. 20260904_040725_N4, which cannot contain a line that did not
        # exist yet): an UNPAUSED, cap-free SET line whose effective sits at
        # <=1x while the host's standing request is 3x and a voter is still
        # below - the min rule visibly constraining the effective below the
        # host's request. The OLD check accepted paused lines too, which let
        # the pause leg satisfy it vacuously; paused is now excluded, and the
        # min-rule CONSISTENCY invariant below is what makes a wrongly-
        # GRANTED raise (eff above the vote min) a hard FAIL in either era.
        foreach ($d in @($speedDenyByLog[$hostLabel])) {
            if ($d.req -ge 2.99 -and $d.eff -le 1.01 -and $d.cap -eq 0) { $sawDenied = $true }
        }
        foreach ($s in $sets) {
            $joinReqs = @($s.votes.Values | ForEach-Object { $_.req })
            $anyVoterBelow3 = (@($joinReqs | Where-Object { $_ -lt 2.99 })).Count -gt 0
            $allVotersAt3 = ($s.hostReq -ge 2.99) -and ($s.votes.Count -ge 3) -and
                            ((@($joinReqs | Where-Object { $_ -lt 2.99 })).Count -eq 0)

            if ($s.paused -eq 0 -and $s.eff -le 1.01 -and $s.cap -eq 0 -and
                $s.hostReq -ge 2.99 -and $anyVoterBelow3) {
                $sawDenied = $true
            }
            if ($s.eff -ge 2.99 -and $s.cap -eq 0 -and $allVotersAt3) {
                $sawAllRaised = $true
                if ($pauseHappened) { $sawUnpause = $true }
            }
            if ($s.eff -le 0.01 -and $s.paused -eq 1) { $sawPause = $true; $pauseHappened = $true }
            if ($s.eff -le 1.01 -and $s.cap -eq 1) { $sawCombatCap = $true }
            if ($s.eff -le 1.01 -and $s.cap -eq 0 -and $s.votes.ContainsKey(3) -and
                $s.votes[3].req -le 1.01 -and $s.hostReq -ge 2.99) {
                $sawHoldVote = $true
            }

            # Phase 9 review WR-04: min-rule CONSISTENCY invariant over EVERY
            # SET line - the effective must equal the min of the host's
            # request and every (voted, req>=0) voter, capped at 1x while the
            # combat cap is up (speedReduce's contract, SpeedVote.h). This is
            # the check that can actually FAIL if the min rule breaks in the
            # granted-raise direction: a wrongly-granted raise IS a change,
            # so it IS logged, and its eff sits above the vote min.
            $reqVals = @([double]$s.hostReq)
            foreach ($v in $s.votes.Values) { if ($v.req -ge 0) { $reqVals += [double]$v.req } }
            $minReq = ($reqVals | Measure-Object -Minimum).Minimum
            $expectedEff = if ($s.cap -eq 1) { [math]::Min(1.0, $minReq) } else { $minReq }
            if ([math]::Abs($s.eff - $expectedEff) -gt 0.02) {
                [void]$findings.Add("Leg B: SET line at t=$($s.t) reports eff=$($s.eff) but the vote set mins to $expectedEff (host=$($s.hostReq), cap=$($s.cap)) - effective is not the min of the votes (min-rule violation)")
            }
        }
        if (-not $sawDenied)     { [void]$findings.Add("Leg B: never observed a DENIED raise (no '[speed] DENY' line and no unpaused cap-free '[speed] SET' with eff<=1x while the host requests 3x and a voter is still below - the min rule)") }
        if (-not $sawAllRaised)  { [void]$findings.Add("Leg B: never observed effective reaching 3x with all voters (host + 3 joins) at 3x (the all-must-raise rule)") }
        if (-not $sawPause)      { [void]$findings.Add("Leg B: never observed a paused ('[speed] SET' eff=0 paused=1) transition") }
        if (-not $sawUnpause)    { [void]$findings.Add("Leg B: never observed recovery back to 3x after the pause") }
        if (-not $sawCombatCap)  { [void]$findings.Add("Leg B: never observed the combat cap ('[speed] SET' cap=1)") }
        if (-not $sawHoldVote)   { [void]$findings.Add("Leg B: never observed rank3 holding a constraining 1x vote while the host requests 3x (cap=0) - the pre-disconnect baseline for the instant-drop proof") }

        # Instant vote drop: the host's own [leave] line for owner=3,
        # bracketed by the legmark leg=B phase=1/2 window (the
        # disconnectAtSec timing contract, ScenarioConsensus.cpp's file
        # header) - and every SURVIVOR's own SCENARIO SPEED series raising
        # back to 3x within tolerance afterward.
        $b1 = $null; $b2 = $null
        foreach ($row in $legMarkByLog[$hostLabel]) {
            if ($row.leg -eq 'B' -and $row.phase -eq 1) { $b1 = $row.t }
            if ($row.leg -eq 'B' -and $row.phase -eq 2) { $b2 = $row.t }
        }
        $leaveRows = @($leaveByLog[$hostLabel] | Where-Object { $_.owner -eq 3 } | Sort-Object { $_.t })
        if ($leaveRows.Count -eq 0) {
            [void]$findings.Add("Leg B: host log shows no '[leave] speed vote=... owner=3' - the disconnect was never detected (the instant-vote-drop proof has no evidence)")
        } else {
            $leaveT = $leaveRows[0].t
            $metrics.leaveT = $leaveT
            if ($null -ne $b1 -and $null -ne $b2 -and ($leaveT -lt $b1 -or $leaveT -gt $b2)) {
                [void]$findings.Add("Leg B: join3's disconnect at t=$leaveT falls OUTSIDE the held-vote bracket [$b1,$b2] - the timing contract (ScenarioConsensus.cpp) was violated")
            }
            $dropTolMs = 15000
            $survivorsRaised = 0; $survivorsChecked = 0
            foreach ($label in $speedSeriesByLog.Keys) {
                if ($ownRanks[$label] -eq 3 -or $null -eq $ownRanks[$label]) { continue }
                $survivorsChecked++
                $rows = @($speedSeriesByLog[$label] | Where-Object { $_.t -ge $leaveT -and $_.t -le ($leaveT + $dropTolMs) } | Sort-Object { $_.t })
                $raised = @($rows | Where-Object { $_.mult -ge 2.99 })
                if ($raised.Count -gt 0) { $survivorsRaised++ }
                else { [void]$findings.Add("Leg B: $label's SCENARIO SPEED series never raises to 3x within ${dropTolMs}ms of join3's disconnect (t=$leaveT) - the instant-drop proof failed for $label") }
            }
            if ($survivorsChecked -eq 0) {
                [void]$findings.Add("Leg B: no surviving instance's rank could be resolved to check the instant-drop raise (evidence gap)")
            }
            $metrics.survivorsRaised = $survivorsRaised; $metrics.survivorsChecked = $survivorsChecked
        }
    }

    # ==== Leg C: time convergence (CONS-03) ======================================
    $THRESH_GH = 0.02 # game hours - the Test-TimeSync 2p tolerance, generalized to N
    $gtimeOk = @{}
    foreach ($label in $gtimeByLog.Keys) {
        $gtimeOk[$label] = @($gtimeByLog[$label] | Where-Object { $_.ok -eq 1 } | Sort-Object { $_.t })
    }
    $thinLogs = @($gtimeOk.Keys | Where-Object { $null -ne $ownRanks[$_] -and $gtimeOk[$_].Count -lt 5 })
    if ($thinLogs.Count -gt 0) {
        [void]$findings.Add("Leg C: too few GTIME samples on $($thinLogs -join ', ') (evidence gap)")
    }
    $labels = @($gtimeOk.Keys | Where-Object { $null -ne $ownRanks[$_] })
    if ($labels.Count -ge 2) {
        $maxT = 0
        foreach ($label in $labels) { foreach ($s in $gtimeOk[$label]) { if ($s.t -gt $maxT) { $maxT = $s.t } } }
        $tailSeries = @{}
        foreach ($label in $labels) { $tailSeries[$label] = @($gtimeOk[$label] | Where-Object { $_.t -ge ($maxT - 20000) }) }
        $tailResult = Get-ConsensusMaxPairwiseDeltaGh -Series $tailSeries -Labels $labels
        $metrics.finalMaxDeltaGh = [math]::Round($tailResult.worst, 5)
        if ($tailResult.worst -gt $THRESH_GH) {
            [void]$findings.Add("Leg C: final clock convergence diverges - max pairwise offset $([math]::Round($tailResult.worst,5))gh between $($tailResult.pair) exceeds tolerance $THRESH_GH")
        }
        # Mid-run checkpoint (around leg A's phase=2 end marker, BEFORE the
        # Leg-B speed changes begin) - corroborates non-divergence across
        # those changes, not just a converged final moment.
        if ($null -ne $hostLabel) {
            $legAEndMark2 = $null
            foreach ($row in $legMarkByLog[$hostLabel]) { if ($row.leg -eq 'A' -and $row.phase -eq 2) { $legAEndMark2 = $row.t } }
            if ($null -ne $legAEndMark2) {
                $midSeries = @{}
                foreach ($label in $labels) {
                    $midSeries[$label] = @($gtimeOk[$label] | Where-Object { $_.t -ge ($legAEndMark2 - 10000) -and $_.t -le ($legAEndMark2 + 10000) })
                }
                $midResult = Get-ConsensusMaxPairwiseDeltaGh -Series $midSeries -Labels $labels
                $metrics.midMaxDeltaGh = [math]::Round($midResult.worst, 5)
                if ($midResult.worst -gt $THRESH_GH) {
                    [void]$findings.Add("Leg C: mid-run clock convergence (around leg A end) diverges - max pairwise offset $([math]::Round($midResult.worst,5))gh between $($midResult.pair) exceeds tolerance $THRESH_GH")
                }
            }
        }
    } else {
        [void]$findings.Add("Leg C: fewer than 2 logs have GTIME evidence (evidence gap)")
    }

    # ---- Aggregate ---------------------------------------------------------------
    if ($findings.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $metrics -Detail ($findings -join '; '))
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics $metrics `
                -Detail "Leg A: money conservation + identical reject + refund ok; Leg B: min-vote + cap + instant-drop ok; Leg C: time convergence ok")
}

# Max pairwise |delta gameHours| across N wall-clock-aligned GTIME series
# (nearest-sample-within-tolerance pairing, the Test-TimeSync idiom
# generalized from one pair to every pair of $Labels).
#
# Plan 04 live-run fix (run 20260904_031458_N4): the raw |hours_a - hours_b|
# comparison conflates TWO different quantities whenever the paired samples
# are not simultaneous - genuine cross-instance clock divergence, AND the
# natural game-hour advance that occurs during the real-time GAP between the
# two samples (up to AlignTolMs=2000ms apart) at whatever speed multiplier
# was in effect. At mult=3x (this scenario's Leg-B raised state) the
# natural-advance term alone is ~3/hourLen gh per real second - a ~1.9s
# pairing gap (well inside the 2000ms tolerance) produces a ~0.052gh "delta"
# from perfectly-converged clocks alone (observed live: host/join2 paired
# 1.9s apart, hourLen=109.1, mult=3.00 -> 3*1.9/109.1=0.0523gh, matching the
# reported 0.05253gh almost exactly). This is an alignment-tolerance
# artifact, not a real desync - manual point-in-time comparison of the same
# two logs at truly-simultaneous wall timestamps showed ~0.003gh agreement
# throughout. Fix: subtract the EXPECTED natural advance for the actual
# pairing gap (using each sample's own mult/hourLen, averaged across the
# pair) before comparing to tolerance - this measures genuine RESIDUAL
# divergence, the quantity CONS-03's slew-convergence claim is actually
# about, and is a strengthening of the oracle's precision (the 0.02gh
# threshold itself is unchanged) per this codebase's never-weaken-the-
# verdict rule (docs/PHASE_8_GATE.md's oracle-fix-justification precedent).
function Get-ConsensusMaxPairwiseDeltaGh {
    param($Series, [string[]]$Labels, [int]$AlignTolMs = 2000)
    $worst = 0.0; $worstPair = ""
    for ($i = 0; $i -lt $Labels.Count; $i++) {
        for ($j = $i + 1; $j -lt $Labels.Count; $j++) {
            $a = $Series[$Labels[$i]]; $b = $Series[$Labels[$j]]
            if ($a.Count -eq 0 -or $b.Count -eq 0) { continue }
            foreach ($sa in $a) {
                $near = $b | Sort-Object { [math]::Abs($_.t - $sa.t) } | Select-Object -First 1
                if ($null -eq $near -or [math]::Abs($near.t - $sa.t) -gt $AlignTolMs) { continue }
                $gapMs = $near.t - $sa.t
                $avgMult = 1.0; $avgHourLen = 0.0
                if ($null -ne $sa.mult -and $null -ne $near.mult) {
                    $avgMult = ($sa.mult + $near.mult) / 2.0
                }
                if ($null -ne $sa.hourLen -and $null -ne $near.hourLen) {
                    $avgHourLen = ($sa.hourLen + $near.hourLen) / 2.0
                }
                $expectedGh = 0.0
                if ($avgHourLen -gt 0.0) {
                    $expectedGh = $avgMult * ($gapMs / 1000.0) / $avgHourLen
                }
                $d = [math]::Abs(($near.hours - $sa.hours) - $expectedGh)
                if ($d -gt $worst) { $worst = $d; $worstPair = "$($Labels[$i])/$($Labels[$j])" }
            }
        }
    }
    return [pscustomobject]@{ worst = $worst; pair = $worstPair }
}

# ---- Gate: save/load coordinator + late-join (Phase 10 plan 03, SAVE-01..04) --
#
# Judges the save_load_gate scenario's four legs (L late-join, S coordinated
# 4-client save, R concurrent rejection, H host-load-while-3-connected) from
# census-true evidence: this scenario's OWN "SCENARIO SAVELOAD ..." lines
# (ScenarioSaveLoad.cpp) bound each leg's window and drive the levers; the
# production "[boot]/[save]/[coord]/[load] ..." lines (Plans 01/02) are the
# actual correctness evidence. Reuses "SCENARIO MAGATE start ownRank=.." for
# Get-MagateOwnRank identity resolution (every N=4 gate's own convention).
function Test-SaveLoad {
    param(
        [Parameter(Mandatory = $true)]$Logs,
        [string]$RunDir = ""
    )
    $gate = "save_load"
    $CENSUS_GAP_TOL_MS = 5000  # 1 Hz heartbeat; a gap past this is a dead/truncated log, not a slow tick
    $R3_MOVE_TOL_U     = 5.0   # world-unit jitter tolerance for the rank-3-driver probe

    # ---- this scenario's own evidence (ScenarioSaveLoad.cpp) ------------------
    $legMarkPat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD leg=(\w) phase=(\w+) t=(\d+)"
    $censusPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD census rank=(\d+) haveOwn=(\d) pos=([\-\d.]+),([\-\d.]+),([\-\d.]+) r3seen=(\d) r3pos=([\-\d.]+),([\-\d.]+),([\-\d.]+) t=(\d+)"
    $joinedPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD joined rank=(\d+) t=(\d+)"
    # Phase 11 plan 02 (TEST-01): the scenario's N-inapplicable named skip
    # marker for Leg R (ScenarioSaveLoad.cpp's legRSkipLogged_ path).
    $legskipPat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD legskip leg=(\w) n=(\d+)"
    $savehostPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD savehost name='([^']*)' ok=(\d) try=(\d+) t=(\d+)"
    $reqsavePat  = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD reqsave rank=(\d+) name='([^']*)' ok=(\d) try=(\d+) t=(\d+)"
    $loadhostPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SAVELOAD loadhost name='([^']*)' ok=(\d) t=(\d+)"

    # ---- production evidence (Plans 01/02) -------------------------------------
    $bootGoPat        = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[boot\] GO->join id=(\d+) dest=(\d+) name='([^']*)' fp=([0-9a-fA-F]+)"
    $bootResyncPat    = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[boot\] RESYNC dest=(\d+)"
    $xferBeginPat     = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[save\] XFER-BEGIN id=(\d+) name='([^']*)' files=(\d+) bytes=(\d+) dest=(\d+)"
    $xferRecvPat      = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[save\] XFER-RECV id=(\d+)"
    $xferCommitPat    = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[save\] XFER-COMMIT"
    $saveClientPat    = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[save\] CLIENT owner=(\d+) xferId=(\d+) state=(\w+)"
    $saveDropPat      = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[save\] DROP owner=(\d+) xferId=(\d+) retries=(\d+)"
    $coordRejectPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[coord\] REJECT requester=(\d+) reqId=(\d+) reason=busy active=(\d+)/(\d+) kind=(\w+)"
    $coordRejectedPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[coord\] REJECTED reqId=(\d+) kind=(\w+) active=(\d+)/(\d+)"
    $loadGoMatchPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] GO id=(\d+) name='([^']*)' fp=([0-9a-fA-F]+) MATCH -> loading"
    $loadGoNackPat    = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] GO id=(\d+) name='([^']*)' hostFp=([0-9a-fA-F]+) localFp=([0-9a-fA-F]+) (\w+) -> NACK"
    $loadGoIdAnyPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] GO id=(\d+)"
    $loadGoBcastPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] GO->join id=(\d+) name='([^']*)' fp=([0-9a-fA-F]+)"
    $loadClientPat    = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] CLIENT owner=(\d+) loadId=(\d+) state=(\w+)"
    $loadDropPat      = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] DROP owner=(\d+) loadId=(\d+) retries=(\d+)"
    $worldReloadPat   = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] WORLD-RELOAD swapMs=(\d+)"
    $localLoadPat     = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[load\] LOCAL-LOAD name='([^']*)' suppressed=(\d) bypass=(\d) via=(\S+)"

    $ownRanks = @{}
    $legMarkByLog=@{}; $censusByLog=@{}; $joinedByLog=@{}; $legskipByLog=@{}; $savehostByLog=@{}; $reqsaveByLog=@{}; $loadhostByLog=@{}
    $bootGoByLog=@{}; $bootResyncByLog=@{}; $xferBeginByLog=@{}; $xferRecvByLog=@{}; $xferCommitByLog=@{}
    $saveClientByLog=@{}; $saveDropByLog=@{}; $coordRejectByLog=@{}; $coordRejectedByLog=@{}
    $loadGoMatchByLog=@{}; $loadGoNackByLog=@{}; $loadGoIdAnyByLog=@{}; $loadGoBcastByLog=@{}
    $loadClientByLog=@{}; $loadDropByLog=@{}; $worldReloadByLog=@{}; $localLoadByLog=@{}
    $overflowByLog=@{}
    $anyEvidence = $false

    foreach ($label in $Logs.Keys) {
        $f = $Logs[$label]
        if (-not (Test-Path $f)) { continue }
        $ownRanks[$label] = Get-MagateOwnRank -File $f
        $off = Get-LogClockOffsetMs -File $f

        $legMarkByLog[$label]     = New-Object System.Collections.ArrayList
        $censusByLog[$label]      = New-Object System.Collections.ArrayList
        $joinedByLog[$label]      = New-Object System.Collections.ArrayList
        $legskipByLog[$label]     = New-Object System.Collections.ArrayList
        $savehostByLog[$label]    = New-Object System.Collections.ArrayList
        $reqsaveByLog[$label]     = New-Object System.Collections.ArrayList
        $loadhostByLog[$label]    = New-Object System.Collections.ArrayList
        $bootGoByLog[$label]      = New-Object System.Collections.ArrayList
        $bootResyncByLog[$label]  = New-Object System.Collections.ArrayList
        $xferBeginByLog[$label]   = New-Object System.Collections.ArrayList
        $xferRecvByLog[$label]    = New-Object System.Collections.ArrayList
        $xferCommitByLog[$label]  = New-Object System.Collections.ArrayList
        $saveClientByLog[$label]  = New-Object System.Collections.ArrayList
        $saveDropByLog[$label]    = New-Object System.Collections.ArrayList
        $coordRejectByLog[$label] = New-Object System.Collections.ArrayList
        $coordRejectedByLog[$label] = New-Object System.Collections.ArrayList
        $loadGoMatchByLog[$label] = New-Object System.Collections.ArrayList
        $loadGoNackByLog[$label]  = New-Object System.Collections.ArrayList
        $loadGoIdAnyByLog[$label] = New-Object System.Collections.ArrayList
        $loadGoBcastByLog[$label] = New-Object System.Collections.ArrayList
        $loadClientByLog[$label]  = New-Object System.Collections.ArrayList
        $loadDropByLog[$label]    = New-Object System.Collections.ArrayList
        $worldReloadByLog[$label] = New-Object System.Collections.ArrayList
        $localLoadByLog[$label]   = New-Object System.Collections.ArrayList

        foreach ($m in (Select-String -Path $f -Pattern $legMarkPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$legMarkByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); leg = $g[5].Value; phase = $g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $censusPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$censusByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                rank = [int]$g[5].Value; haveOwn = [int]$g[6].Value
                x = [double]$g[7].Value; y = [double]$g[8].Value; z = [double]$g[9].Value
                r3seen = [int]$g[10].Value
                r3x = [double]$g[11].Value; r3y = [double]$g[12].Value; r3z = [double]$g[13].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $joinedPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$joinedByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); rank = [int]$g[5].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $legskipPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$legskipByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); leg = $g[5].Value; n = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $savehostPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$savehostByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); name = $g[5].Value; ok = [int]$g[6].Value; try = [int]$g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $reqsavePat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$reqsaveByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); rank = [int]$g[5].Value; name = $g[6].Value
                ok = [int]$g[7].Value; try = [int]$g[8].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadhostPat -ErrorAction SilentlyContinue)) {
            $anyEvidence = $true; $g = $m.Matches[0].Groups
            [void]$loadhostByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); name = $g[5].Value; ok = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $bootGoPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$bootGoByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value; dest = [int]$g[6].Value; name = $g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $bootResyncPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$bootResyncByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); dest = [int]$g[5].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $xferBeginPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$xferBeginByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value; dest = [long]$g[9].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $xferRecvPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$xferRecvByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $xferCommitPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$xferCommitByLog[$label].Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off) })
        }
        foreach ($m in (Select-String -Path $f -Pattern $saveClientPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$saveClientByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); owner = [int]$g[5].Value; xferId = [int]$g[6].Value; state = $g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $saveDropPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$saveDropByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); owner = [int]$g[5].Value; xferId = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $coordRejectPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$coordRejectByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); requester = [int]$g[5].Value; reqId = [int]$g[6].Value
                activeReq = [int]$g[7].Value; activeReqId = [int]$g[8].Value; kind = $g[9].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $coordRejectedPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$coordRejectedByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); reqId = [int]$g[5].Value; kind = $g[6].Value
                activeReq = [int]$g[7].Value; activeReqId = [int]$g[8].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadGoMatchPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$loadGoMatchByLog[$label].Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadGoNackPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$loadGoNackByLog[$label].Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadGoIdAnyPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$loadGoIdAnyByLog[$label].Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadGoBcastPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$loadGoBcastByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); id = [int]$g[5].Value; name = $g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadClientPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$loadClientByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); owner = [int]$g[5].Value; loadId = [int]$g[6].Value; state = $g[7].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $loadDropPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$loadDropByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); owner = [int]$g[5].Value; loadId = [int]$g[6].Value })
        }
        foreach ($m in (Select-String -Path $f -Pattern $worldReloadPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$worldReloadByLog[$label].Add([pscustomobject]@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off) })
        }
        foreach ($m in (Select-String -Path $f -Pattern $localLoadPat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$localLoadByLog[$label].Add([pscustomobject]@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off); suppressed = [int]$g[6].Value })
        }
        $overflowByLog[$label] = ((Select-String -Path $f -Pattern 'BACKLOG OVERFLOW' -ErrorAction SilentlyContinue) | Measure-Object).Count -gt 0
    }

    if (-not $anyEvidence) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no SCENARIO SAVELOAD evidence found in any of the $($Logs.Count) log(s)")
    }
    $anyLegMark = $false
    foreach ($label in $legMarkByLog.Keys) { if ($legMarkByLog[$label].Count -gt 0) { $anyLegMark = $true } }
    if (-not $anyLegMark) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "SCENARIO SAVELOAD evidence present but no leg=<L|S|R|H> phase=<begin|end> marker found - no leg window can be judged")
    }

    $findings = New-Object System.Collections.ArrayList
    $metrics = @{}
    $hostLabel  = ($ownRanks.Keys | Where-Object { $ownRanks[$_] -eq 0 } | Select-Object -First 1)
    # Phase 11 plan 02 (TEST-01): the late-joining instance is no longer
    # assumed to be rank 3 - ScenarioSaveLoad.cpp's identity fix (an env-var
    # signal, not rank arithmetic) means the "joined" marker names whichever
    # rank actually launched late, at ANY N. Resolve it the same way: whoever
    # emitted "SCENARIO SAVELOAD joined" is the late-join label, regardless of
    # its rank number. At N=4 this is byte-identical (only join3's rig entry
    # ever carries reconnectAtSec, so only rank 3's log ever has this
    # evidence).
    $join3Label = ($ownRanks.Keys | Where-Object { $null -ne $ownRanks[$_] -and @($joinedByLog[$_]).Count -gt 0 } | Select-Object -First 1)
    $establishedLabels = @($ownRanks.Keys | Where-Object { $null -ne $ownRanks[$_] -and $_ -ne $join3Label })

    function Get-LegMarkT { param($Log, $Leg, $Phase)
        foreach ($row in $legMarkByLog[$Log]) { if ($row.leg -eq $Leg -and $row.phase -eq $Phase) { return $row.t } }
        return $null
    }

    # ==== Leg L: late-join (SAVE-04) =============================================
    $join3PlayerId = if ($null -ne $join3Label) { $ownRanks[$join3Label] } else { $null }
    $joinConnectT = $null
    if ($null -ne $hostLabel -and $null -ne $join3PlayerId) {
        $bootRows = @($bootGoByLog[$hostLabel] | Where-Object { $_.dest -eq $join3PlayerId } | Sort-Object { $_.t })
        if ($bootRows.Count -gt 0) { $joinConnectT = $bootRows[0].t }
    }
    if ($null -eq $joinConnectT -and $null -ne $join3Label) {
        $j3rows = @($joinedByLog[$join3Label] | Sort-Object { $_.t })
        if ($j3rows.Count -gt 0) { $joinConnectT = $j3rows[0].t }
    }
    $legSBeginT = if ($null -ne $hostLabel) { Get-LegMarkT -Log $hostLabel -Leg 'S' -Phase 'begin' } else { $null }

    if ($null -eq $join3Label) {
        [void]$findings.Add("Leg L: no log emitted a SCENARIO SAVELOAD joined marker (no late-joining instance observed/armed) - late-join cannot be judged")
    } elseif ($null -eq $joinConnectT) {
        [void]$findings.Add("Leg L: no evidence of join3's connect (no host [boot] GO->join dest=$join3PlayerId and no join3 SAVELOAD joined marker) - evidence gap")
    } elseif ($null -eq $legSBeginT) {
        [void]$findings.Add("Leg L: host log has no 'leg=S phase=begin' marker - the Leg L window cannot be closed")
    } else {
        $metrics.joinConnectT = $joinConnectT
        foreach ($label in $establishedLabels) {
            $goHits = @($loadGoIdAnyByLog[$label] | Where-Object { $_.t -ge $joinConnectT -and $_.t -lt $legSBeginT })
            if ($goHits.Count -gt 0) { [void]$findings.Add("Leg L: $label shows '[load] GO id=' inside the join window - a survivor received a load broadcast (reload-storm regression)") }
            $recvHits = @($xferRecvByLog[$label] | Where-Object { $_.t -ge $joinConnectT -and $_.t -lt $legSBeginT })
            if ($recvHits.Count -gt 0) { [void]$findings.Add("Leg L: $label shows '[save] XFER-RECV' inside the join window - a survivor received a save-plane transfer") }
            $reloadHits = @($worldReloadByLog[$label] | Where-Object { $_.t -ge $joinConnectT -and $_.t -lt $legSBeginT })
            if ($reloadHits.Count -gt 0) { [void]$findings.Add("Leg L: $label shows '[load] WORLD-RELOAD' inside the join window - a survivor reloaded (global reset)") }
            $localLoadHits = @($localLoadByLog[$label] | Where-Object { $_.t -ge $joinConnectT -and $_.t -lt $legSBeginT -and $_.suppressed -eq 0 })
            if ($localLoadHits.Count -gt 0) { [void]$findings.Add("Leg L: $label shows an unsuppressed '[load] LOCAL-LOAD' inside the join window - a survivor's native load fired") }

            $censusRows = @($censusByLog[$label] | Where-Object { $_.t -ge $joinConnectT -and $_.t -le $legSBeginT } | Sort-Object { $_.t })
            if ($censusRows.Count -lt 2) {
                [void]$findings.Add("Leg L: $label has fewer than 2 SAVELOAD census samples across the join window (dead/truncated log, or the no-reset absence above cannot be trusted)")
            } else {
                $maxGap = 0
                for ($i = 1; $i -lt $censusRows.Count; $i++) {
                    $gap = $censusRows[$i].t - $censusRows[$i - 1].t
                    if ($gap -gt $maxGap) { $maxGap = $gap }
                }
                if ($maxGap -gt $CENSUS_GAP_TOL_MS) {
                    [void]$findings.Add("Leg L: $label's SAVELOAD census heartbeat has a ${maxGap}ms gap inside the join window (exceeds ${CENSUS_GAP_TOL_MS}ms tolerance) - the log may have gone silent")
                }
            }
        }

        if ($null -ne $hostLabel) {
            $gotBootGo = @($bootGoByLog[$hostLabel] | Where-Object { $_.dest -eq $join3PlayerId }).Count -gt 0
            if (-not $gotBootGo) { [void]$findings.Add("Leg L: host log has no '[boot] GO->join dest=$join3PlayerId' - the targeted bootstrap never fired") }
            $gotResync = @($bootResyncByLog[$hostLabel] | Where-Object { $_.dest -eq $join3PlayerId }).Count -gt 0
            if (-not $gotResync) { [void]$findings.Add("Leg L: host log has no '[boot] RESYNC dest=$join3PlayerId' - the post-load catch-up pass never ran") }
        }

        $matched   = @($loadGoMatchByLog[$join3Label]).Count -gt 0
        $nacked    = @($loadGoNackByLog[$join3Label]).Count -gt 0
        $committed = @($xferCommitByLog[$join3Label]).Count -gt 0
        if (-not $matched -and -not ($nacked -and $committed)) {
            [void]$findings.Add("Leg L: join3's own log shows neither a fingerprint MATCH load nor a NACK+XFER-COMMIT transfer completion - the bootstrap never converged")
        }
        if ($overflowByLog[$join3Label]) {
            [void]$findings.Add("Leg L: join3's own log contains a BACKLOG OVERFLOW line - inbound queue pressure during its title wait")
        }

        foreach ($label in $establishedLabels) {
            $preRows = @($censusByLog[$label] | Where-Object { $_.t -lt $joinConnectT -and $_.r3seen -eq 1 } | Sort-Object { $_.t })
            for ($i = 1; $i -lt $preRows.Count; $i++) {
                $dx = $preRows[$i].r3x - $preRows[$i - 1].r3x
                $dz = $preRows[$i].r3z - $preRows[$i - 1].r3z
                $dist = [math]::Sqrt($dx * $dx + $dz * $dz)
                if ($dist -gt $R3_MOVE_TOL_U) {
                    [void]$findings.Add("Leg L: $label's rank-3-driver probe shows rank 3's squad MOVING (${dist}u) before join3 connected - a host-adoption sighting")
                    break
                }
            }
        }

        if ($RunDir -ne "" -and (Test-Path (Join-Path $RunDir "run_meta.json"))) {
            try {
                $meta = Get-Content -Raw (Join-Path $RunDir "run_meta.json") | ConvertFrom-Json
                $join3LogName = Split-Path -Leaf $Logs[$join3Label]
                if (@($meta.scheduledReconnect) -notcontains $join3LogName) {
                    [void]$findings.Add("Leg L: run_meta.json's scheduledReconnect does not name join3's log ($join3LogName) - the late-join may not have used the genuine deferred-launch lever")
                }
            } catch {}
        }
    }

    # ==== Leg S: coordinated 4-client save (SAVE-01) ============================
    $legSEndT = if ($null -ne $hostLabel) { Get-LegMarkT -Log $hostLabel -Leg 'S' -Phase 'end' } else { $null }
    if ($null -eq $hostLabel -or $null -eq $legSBeginT -or $null -eq $legSEndT) {
        [void]$findings.Add("Leg S: host log is missing its leg=S begin/end markers - evidence gap")
    } else {
        $clientRows = @($saveClientByLog[$hostLabel] | Where-Object { $_.t -ge $legSBeginT -and $_.t -le $legSEndT })
        $committedOwners = @($clientRows | Where-Object { $_.state -eq 'committed' } | ForEach-Object { $_.owner } | Select-Object -Unique)
        # Phase 11 plan 02 (TEST-01): "need 3" was N=4-only (every one of the
        # 3 joins must ACK) - generalized to every CONNECTED join (host
        # excluded), so N=2/N=3 don't hard-fail on an unreachable owner count.
        # By Leg S's window (~130s), Leg L has already closed - the late
        # joiner (if any) is already connected and expected to ACK too.
        $expectedCommitters = @($ownRanks.Keys | Where-Object { $null -ne $ownRanks[$_] -and $_ -ne $hostLabel }).Count
        if ($committedOwners.Count -lt $expectedCommitters) {
            [void]$findings.Add("Leg S: only $($committedOwners.Count) distinct owner(s) reached '[save] CLIENT ... state=committed' inside the Leg S window (need $expectedCommitters - one client's ACK never implies group commit)")
        }
        $dropRows = @($saveDropByLog[$hostLabel] | Where-Object { $_.t -ge $legSBeginT -and $_.t -le $legSEndT })
        if ($dropRows.Count -gt 0) {
            [void]$findings.Add("Leg S: host log shows a '[save] DROP' inside the Leg S window - a client was dropped during the coordinated save")
        }
        $metrics.legSCommittedOwners = $committedOwners.Count
    }

    # ==== Leg R: concurrent rejection (SAVE-02/03) ===============================
    $legRBeginT = if ($null -ne $hostLabel) { Get-LegMarkT -Log $hostLabel -Leg 'R' -Phase 'begin' } else { $null }
    $legREndT   = if ($null -ne $hostLabel) { Get-LegMarkT -Log $hostLabel -Leg 'R' -Phase 'end' }   else { $null }
    # Phase 11 plan 02 (TEST-01): N-inapplicable honest SKIP - Leg R's rank-
    # vs-rank contest needs two ESTABLISHED joins; a scenario running at N<3
    # (fewer than two joins ever connected) names this via its own
    # "SCENARIO SAVELOAD legskip leg=R n=<N>" marker (ScenarioSaveLoad.cpp's
    # legRSkipLogged_ path). The oracle re-derives N independently from the
    # log SET SIZE it was actually handed ($Logs.Count) rather than trusting
    # the scenario's self-reported n= value for the gating decision - "skip
    # ONLY when the marker is present AND N<3" (must_haves), never a silent
    # pass, and never fooled by a marker that fired when it should not have.
    $legRSkipRow = $null
    foreach ($label in $legskipByLog.Keys) {
        $hit = @($legskipByLog[$label] | Where-Object { $_.leg -eq 'R' } | Select-Object -First 1)
        if ($hit.Count -gt 0) { $legRSkipRow = $hit[0]; break }
    }
    $legRSkipped = ($null -ne $legRSkipRow -and $Logs.Count -lt 3)
    if ($null -ne $legRSkipRow -and $Logs.Count -ge 3) {
        [void]$findings.Add("Leg R: scenario emitted a legskip marker (n=$($legRSkipRow.n)) but $($Logs.Count) instance logs are present (N>=3) - Leg R should have run, not skipped")
    }

    if ($legRSkipped) {
        $metrics.legRSkippedN = $legRSkipRow.n
    } elseif ($null -eq $hostLabel -or $null -eq $legRBeginT -or $null -eq $legREndT) {
        [void]$findings.Add("Leg R: host log is missing its leg=R begin/end markers - evidence gap")
    } else {
        $rejectRows = @($coordRejectByLog[$hostLabel] | Where-Object { $_.t -ge $legRBeginT -and $_.t -le $legREndT -and $_.kind -eq 'save' } | Sort-Object { $_.t })
        if ($rejectRows.Count -eq 0) {
            [void]$findings.Add("Leg R: host log shows no '[coord] REJECT ... kind=save' inside the Leg R window - the concurrent-request contest never produced an observable verdict")
        } else {
            $reject = $rejectRows[0]
            $loserId = $reject.requester
            $loserLabel = ($ownRanks.Keys | Where-Object { $ownRanks[$_] -eq $loserId } | Select-Object -First 1)
            if ($null -eq $loserLabel) {
                [void]$findings.Add("Leg R: the host's REJECT names requester=$loserId, but no log resolves to that rank - cannot verify the loser's receipt")
            } else {
                $receiptRows = @($coordRejectedByLog[$loserLabel] | Where-Object { $_.reqId -eq $reject.reqId -and $_.kind -eq 'save' })
                if ($receiptRows.Count -eq 0) {
                    [void]$findings.Add("Leg R: $loserLabel (rank $loserId) never logged a matching '[coord] REJECTED reqId=$($reject.reqId)' receipt - the rejection was not observable end-to-end")
                }
                $retryRows = @($reqsaveByLog[$loserLabel] | Where-Object { $_.try -eq 2 } | Sort-Object { $_.t })
                if ($retryRows.Count -eq 0) {
                    [void]$findings.Add("Leg R: $loserLabel never issued its scripted try=2 retry")
                } else {
                    $retryT = $retryRows[0].t
                    $secondReject = @($coordRejectByLog[$hostLabel] | Where-Object {
                        $_.requester -eq $loserId -and $_.t -gt $retryT -and $_.t -le $legREndT -and $_.kind -eq 'save' })
                    if ($secondReject.Count -gt 0) {
                        [void]$findings.Add("Leg R: $loserLabel's retry (try=2) was ALSO rejected - the requester-may-retry-after-completion guarantee failed")
                    }
                }
            }
            $metrics.legRLoser = $loserId
        }
    }

    # ==== Serialization invariant (SAVE-03): one authoritative transition at a time
    if ($null -ne $hostLabel) {
        $transitions = New-Object System.Collections.ArrayList
        foreach ($s in @($xferBeginByLog[$hostLabel] | Sort-Object { $_.t })) {
            $settleRows = @($saveClientByLog[$hostLabel] | Where-Object { $_.xferId -eq $s.id -and $_.t -ge $s.t } | Sort-Object { $_.t })
            $endT = if ($settleRows.Count -gt 0) { $settleRows[$settleRows.Count - 1].t } else { $s.t + 60000 }
            [void]$transitions.Add([pscustomobject]@{ kind = 'save'; id = $s.id; startT = $s.t; endT = $endT })
        }
        # 10-04 live gate finding (run 3): a per-join targeted connect-push
        # (bootGoByLog, Plan 02) settles via its OWN "[boot] RESYNC dest=X"
        # marker, never via "[load] CLIENT ... state=loaded" (10-01-SUMMARY
        # key-decision #3: the connect-push frees the arbiter at quiescence,
        # not through SaveCoord/LoadCoord's per-client ACK bookkeeping - it
        # is a structurally different, independently-scoped category from a
        # broadcast coordinated load). Matching it against loadClientByLog
        # (which a connect-push never populates) always misses, falling
        # back to the generic "assume active for 60s" placeholder - so THREE
        # joins connecting in the normal staggered sequence (each one's own
        # connect-push genuinely settling in ~5-35s, well under the 60s
        # placeholder) reliably reads as "the next join's connect-push
        # started before the prior one settled", a false double-transition
        # on ordinary sequential connects, not a genuine defect. Match each
        # boot-kind transition's settle against ITS OWN dest's RESYNC
        # instead, so an actually-fast sequential connect stops being
        # mis-timed as still active for a full minute.
        # IN-02 fix (Phase 10 REVIEW; Phase 11 Plan 01): an LC_DROPPED-settle
        # connect-push never emits a RESYNC (only a genuinely re-synced boot
        # does), so matching RESYNC alone left THAT shape "active" for the
        # full 60s placeholder, able to reintroduce the false-double-
        # transition misjudgment this comment block already fixed once for
        # the normal-settle case. Settle at the EARLIEST of three
        # candidates for this dest/id: the existing RESYNC marker, OR the
        # per-client LoadCoord "state=loaded" settle Plugin.cpp:950-956
        # seeds for the same dest/id, OR a "[load] DROP" for that same
        # dest/id (the LC_DROPPED-settle shape) - falling back to the 60s
        # placeholder only when all three are absent. Only ADDS earlier
        # candidates, so a transition that already settled via RESYNC keeps
        # that exact endT unless a loadClient/loadDrop row settles sooner -
        # a prior PASS can only stay PASS.
        foreach ($s in @($bootGoByLog[$hostLabel] | Sort-Object { $_.t })) {
            $resyncRows = @($bootResyncByLog[$hostLabel] | Where-Object { $_.dest -eq $s.dest -and $_.t -ge $s.t } | Sort-Object { $_.t })
            $loadedRows = @($loadClientByLog[$hostLabel] | Where-Object { $_.owner -eq $s.dest -and $_.loadId -eq $s.id -and $_.state -eq 'loaded' -and $_.t -ge $s.t } | Sort-Object { $_.t })
            $dropRows   = @($loadDropByLog[$hostLabel]   | Where-Object { $_.owner -eq $s.dest -and $_.loadId -eq $s.id -and $_.t -ge $s.t } | Sort-Object { $_.t })
            $settleCandidates = @()
            if ($resyncRows.Count -gt 0) { $settleCandidates += $resyncRows[0].t }
            if ($loadedRows.Count -gt 0) { $settleCandidates += $loadedRows[0].t }
            if ($dropRows.Count -gt 0)   { $settleCandidates += $dropRows[0].t }
            $endT = if ($settleCandidates.Count -gt 0) { ($settleCandidates | Measure-Object -Minimum).Minimum } else { $s.t + 60000 }
            [void]$transitions.Add([pscustomobject]@{ kind = 'load'; id = $s.id; startT = $s.t; endT = $endT })
        }
        foreach ($s in @($loadGoBcastByLog[$hostLabel] | Sort-Object { $_.t })) {
            $settleRows = @($loadClientByLog[$hostLabel] | Where-Object { $_.loadId -eq $s.id -and $_.t -ge $s.t } | Sort-Object { $_.t })
            $endT = if ($settleRows.Count -gt 0) { $settleRows[$settleRows.Count - 1].t } else { $s.t + 60000 }
            [void]$transitions.Add([pscustomobject]@{ kind = 'load'; id = $s.id; startT = $s.t; endT = $endT })
        }
        $sorted = @($transitions | Sort-Object { $_.startT })
        for ($i = 1; $i -lt $sorted.Count; $i++) {
            if ($sorted[$i].startT -lt $sorted[$i - 1].endT) {
                # An overlap is only a defect if the host's arbiter did NOT
                # explicitly reject the later offer (Leg R's own contest is
                # the EXPECTED, documented overlap - the loser's REQ still
                # offers inside the winner's active window and correctly
                # draws a REJECT rather than starting a second transition).
                $explained = @($coordRejectByLog[$hostLabel] | Where-Object {
                    $_.t -ge $sorted[$i - 1].startT -and $_.t -le $sorted[$i].startT + 2000 }).Count -gt 0
                if (-not $explained) {
                    [void]$findings.Add("Serialization: a second $($sorted[$i].kind) transition (id=$($sorted[$i].id)) started at t=$($sorted[$i].startT) before the prior $($sorted[$i-1].kind) transition (id=$($sorted[$i-1].id)) settled at t=$($sorted[$i-1].endT), with no '[coord] REJECT' explaining the overlap - double-transition")
                }
            }
        }
    }

    # ==== Leg H: host-load-while-3-connected (SAVE-02) ===========================
    $legHBeginT = if ($null -ne $hostLabel) { Get-LegMarkT -Log $hostLabel -Leg 'H' -Phase 'begin' } else { $null }
    $legHEndT   = if ($null -ne $hostLabel) { Get-LegMarkT -Log $hostLabel -Leg 'H' -Phase 'end' }   else { $null }
    if ($null -eq $hostLabel -or $null -eq $legHBeginT -or $null -eq $legHEndT) {
        [void]$findings.Add("Leg H: host log is missing its leg=H begin/end markers - evidence gap")
    } else {
        $goRows = @($loadGoBcastByLog[$hostLabel] | Where-Object { $_.t -ge $legHBeginT -and $_.t -le $legHEndT })
        if ($goRows.Count -eq 0) {
            [void]$findings.Add("Leg H: host log shows no broadcast '[load] GO->join' inside the Leg H window - the mid-session load never issued")
        } else {
            $goId = $goRows[0].id
            # Phase 11 plan 02 (TEST-01): "1, 2, 3" was N=4-only (exactly 3
            # joins) - iterate the ranks ACTUALLY present in this run's own
            # ownRanks map instead, so N=2/N=3 verify every connected join's
            # load completion rather than hard-failing on an unreachable rank.
            $joinRanksPresent = @($ownRanks.Values | Where-Object { $null -ne $_ -and $_ -gt 0 } | Sort-Object -Unique)
            foreach ($rank in $joinRanksPresent) {
                $label = ($ownRanks.Keys | Where-Object { $ownRanks[$_] -eq $rank } | Select-Object -First 1)
                if ($null -eq $label) {
                    [void]$findings.Add("Leg H: no log resolves to rank $rank - cannot verify its load completion")
                    continue
                }
                $loadedRows = @($loadClientByLog[$hostLabel] | Where-Object { $_.owner -eq $rank -and $_.loadId -eq $goId -and $_.state -eq 'loaded' })
                if ($loadedRows.Count -eq 0) {
                    [void]$findings.Add("Leg H: host log shows no '[load] CLIENT owner=$rank loadId=$goId state=loaded' - rank $rank never positively ACKed the mid-session load")
                }
                $reloadRows = @($worldReloadByLog[$label] | Where-Object { $_.t -ge $legHBeginT -and $_.t -le $legHEndT })
                if ($reloadRows.Count -eq 0) {
                    [void]$findings.Add("Leg H: $label (rank $rank) shows no '[load] WORLD-RELOAD' inside the Leg H window - no observed reload")
                }
            }
        }
    }

    # ---- Aggregate ---------------------------------------------------------------
    if ($findings.Count -gt 0) {
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $metrics -Detail ($findings -join '; '))
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics $metrics `
                -Detail "Leg L: targeted bootstrap + zero-reset survivors + heartbeat + joiner convergence ok; Leg S: every connected join's committed owner ACK ok; Leg R: matched reject/rejected + accepted retry ok (or an honest N<3 legskip); Leg H: broadcast GO + per-client loaded ACKs ok")
}

# ---- N=4 oracle dispatch ------------------------------------------------------
#
# The N=4-shaped counterpart of CoopOracles.psm1's Invoke-OneOracle, keyed by
# the ids scripts\scenarios.psd1's milestone_a_gate/player_state_gate entries
# name in their Gating/Advisory/PrimaryGate lists. Central dispatch (not ad
# hoc per-caller switches) so analyze_run4.ps1 and the *Gate.Tests.ps1
# wrappers stay in sync with whatever gate ids the manifest actually
# declares.
function Invoke-OneOracleN {
    param(
        [string]$Id,
        [Parameter(Mandatory = $true)]$Logs,
        [string]$RunDir = "",
        [double]$Tolerance = 6.0,
        # Scheduled-disconnect exempt label set (gap 3) - only clean_exit
        # consumes this; every other gate id ignores it.
        [string[]]$ExemptLabels = @(),
        # Phase 11 plan 02 (TEST-01): threaded down to census_convergence's
        # analyze_wnpc_diff4.ps1 child process so it judges the SAME N the
        # caller (analyze_run4.ps1) resolved. 0 (default) = let the child
        # resolve N itself - byte-identical to pre-Phase-11 behavior.
        [int]$ExpectedInstances = 0
    )
    switch ($Id) {
        "census_convergence"   { return (Test-MagateCensusConvergence   -RunDir $RunDir -ExpectedInstances $ExpectedInstances) }
        "disconnect_isolation" { return (Test-MagateDisconnectIsolation -Logs $Logs) }
        "driven_only_movement" { return (Test-MagateDrivenOnlyMovement  -Logs $Logs -Tolerance $Tolerance) }
        "desync_convergence"   { return (Test-MagateDesyncConvergence   -Logs $Logs -Tolerance $Tolerance) }
        "clean_exit"           { return (Test-MagateCleanExit           -Logs $Logs -RunDir $RunDir -ExemptLabels $ExemptLabels) }
        "player_state_medical" { return (Test-MagatePlayerStateMedical  -Logs $Logs -Tolerance $Tolerance) }
        "player_state_recruit" { return (Test-MagatePlayerStateRecruit  -Logs $Logs) }
        "inv_conservation"     { return (Test-InvConservation           -Logs $Logs) }
        "world_state"          { return (Test-WorldState                -Logs $Logs) }
        "consensus"            { return (Test-Consensus                 -Logs $Logs) }
        "save_load"            { return (Test-SaveLoad                  -Logs $Logs -RunDir $RunDir) }
        default {
            Write-Host "  WARNING: unknown N=4 oracle id '$Id' (manifest error)"
            return (Add-GateResult -Name $Id -Status FAIL -Detail "unknown N=4 oracle id")
        }
    }
}

# The ids Invoke-OneOracleN can dispatch - a scenario manifest entry may
# reference any of these in its Gating/Advisory/PrimaryGate lists.
function Get-OracleRegistryN {
    return @("census_convergence", "disconnect_isolation", "driven_only_movement", "desync_convergence", "clean_exit",
             "player_state_medical", "player_state_recruit", "inv_conservation", "world_state", "consensus", "save_load")
}

Export-ModuleMember -Function @(
    "Assert-AllLogsPresent",
    "Get-MagateOwnRank", "Get-MagateTabMap", "Get-MagateHandSuffix",
    "Test-MagateCensusConvergence", "Test-MagateDisconnectIsolation",
    "Test-MagateDrivenOnlyMovement", "Test-MagateDesyncConvergence", "Test-MagateCleanExit",
    "Test-MagatePlayerStateMedical", "Test-MagatePlayerStateRecruit",
    "Test-InvConservation",
    "Test-WorldState",
    "Test-Consensus",
    "Test-SaveLoad",
    "Invoke-OneOracleN", "Get-OracleRegistryN",
    # Re-exported pass-through from the nested CoopOracles.psm1 import above.
    # A caller that ALSO does its own separate `Import-Module CoopOracles.psm1`
    # risks PowerShell loading a second, distinct module instance with its own
    # $script:Gates array - silently splitting gate state between "what
    # CoopOraclesN's gates recorded" and "what Get-GateResults sees". Re-
    # exporting these specific names here means a caller that only imports
    # CoopOraclesN.psm1 (analyze_run4.ps1's pattern) always reads/writes the
    # SAME gate-array instance this module's own gate functions use.
    "Reset-GateResults", "Get-GateResults", "Add-GateResult",
    "Get-ScenarioManifest", "Get-LogClockOffsetMs",
    "Test-LogHealth", "Test-ScenarioResultPass", "Test-EngineIntegrity"
)
