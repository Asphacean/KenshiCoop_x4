<#
.SYNOPSIS
  N-way ((N-1) pairwise host-vs-joinK) NPC-census CONVERGENCE oracle for the
  Milestone A gate's N-process run (Phase 4 Plan 03/09, POC-02;
  N-parameterized Phase 11 Plan 02, TEST-01).

.DESCRIPTION
  Generalizes scripts\analyze_wnpc_diff.ps1's working pairwise host-vs-one-join
  WNPC roster diff to N=4 by invoking the SAME parsing/pairing/set-diff logic
  three times: host-vs-join1, host-vs-join2, host-vs-join3. The 2-log original
  is NOT edited in place (other callers depend on its exact 2-log contract,
  per 04-RESEARCH.md's "Don't Hand-Roll" guidance) - this script copies its
  Get-Rows/Get-Dumps parsing verbatim instead of dot-sourcing it, since the
  original is a top-level script (not a function library) that executes its
  own 2-log analysis immediately on load.

  CONVERGENCE, NOT INSTANTANEOUS SET-EQUALITY (04-09 gap-closure decision).
  Since the NPC census is host-broadcast and host-authoritative (PKT_NPC_CENSUS,
  Class B, docs\ROUTING_MATRIX.md row 39), every join's roster should
  EVENTUALLY match the host's - but a fresh join catching up on a host's
  already-populated census, or a rekeyed/proxy body's audit row settling onto
  its wire key, both produce transient single-dump mismatches that resolve on
  their own and are not the "permanent desync" the Milestone A DoD's "one host
  owns every NPC" language is actually about. A single-dump host-only-or-
  join-only body therefore counts as a real divergence for a pair ONLY if:
    (a) it never converges (is never seen matching on both sides) within a
        bounded convergence window - default 150s (-ConvergenceWindowSec),
        measured from the JOIN's own gameplay start ("SCENARIO MAGATE start"),
        not from when the mismatch itself began;
    (b) it DOES converge at least once, then permanently regresses (mismatches
        again with no further convergence observed for the rest of the run) -
        this is a real desync, not cold-start catch-up, regardless of window;
    (c) ZERO TOLERANCE at the tail: the FINAL comparable paired dump (both
        instances still eligible/alive) still differs at all, by either side -
        whatever converged earlier, the run must end agreeing.
  Transient cold-start lag that converges within the window is NOT a
  violation and is reported as a convergence-latency metric instead. This
  naturally absorbs known audit-only residuals (e.g. the ~2-dump/~10s
  rekey/dormant-re-arm duplicate row from 04-07-SUMMARY.md, commit 866ee8a) as
  long as they resolve before the run's final comparable dump.

  HARD REQUIREMENT (04-RESEARCH.md Pitfall 5 - fixed-arity 2-log oracle helpers
  silently degrading to partial judgment): all four logs (host + join1/2/3)
  must exist AND be non-empty before any verdict is computed. A missing or
  empty log FAILS LOUD (a "WNPC4 RESULT: FAIL reason=..." line + a thrown
  error) rather than silently judging on however many logs happen to be
  present - a "PASS" that only ever examined 2 of 4 instances is the exact
  failure mode this guard exists to prevent.

  Emits one final greppable verdict line ("WNPC4 RESULT: PASS" or
  "WNPC4 RESULT: FAIL ...") for plan 05's gate set to consume, following the
  same "RESULT: PASS/FAIL" convention scripts\analyze_run.ps1 already uses.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\analyze_wnpc_diff4.ps1 -RunDir tools\test-runs\20260902_120000_N4
#>
param(
    [Parameter(Mandatory = $true)][string]$RunDir,
    [int]$GapMs = 1000,      # timestamp gap that starts a new dump
    [int]$PairTolMs = 4000,  # host<->join dump pairing window
    [int]$TopN = 12,         # how many offenders to name per pair
    # Bounded cold-start convergence window (seconds, from the JOIN's own
    # "SCENARIO MAGATE start" gameplay-start line) a host-only-or-join-only
    # body has to converge in before it counts as a real divergence (rule a).
    [int]$ConvergenceWindowSec = 150,
    # Explicit log path overrides (default: <RunDir>\host.log / join1.log / join2.log / join3.log,
    # the exact names scripts\run_test4.ps1 + scripts\local.rig.example.json use).
    [string]$HostLog = "",
    [string]$Join1Log = "",
    [string]$Join2Log = "",
    [string]$Join3Log = "",
    # Rig config fallback for resolving which join(s) had a scheduled
    # disconnect (disconnectAtSec), used ONLY to scope the scheduled-
    # disconnect convergence exemption below. Precedence mirrors
    # scripts\analyze_run4.ps1's existing resolution: (i) run_meta.json
    # archived in $RunDir (what a live run_test4.ps1 session writes) takes
    # priority; (ii) -RigConfig's raw instances is the offline-re-judge
    # fallback for a run dir that predates run_meta.json; (iii) neither
    # present - no exemption, full strictness (matches pre-existing
    # behavior). Never required for a live-written run.
    [string]$RigConfig = "",
    # Phase 11 plan 02 (TEST-01): total instance count (host + joins). Same
    # resolution precedence as scripts\analyze_run4.ps1's own param: (i) this
    # param when > 0, (ii) run_meta.json's instanceCount, (iii) 4. At N=2 this
    # degenerates to the single host-vs-join1 pairwise diff (analyze_wnpc_
    # diff.ps1's own 2-log ancestor shape).
    [int]$ExpectedInstances = 0
)

$ErrorActionPreference = "Stop"

if ($HostLog -eq "")  { $HostLog  = Join-Path $RunDir "host.log" }

# ---- Resolve N (Phase 11 plan 02, TEST-01) -----------------------------------
$resolvedN = 4
$runMetaPathForN = Join-Path $RunDir "run_meta.json"
if ($ExpectedInstances -gt 0) {
    $resolvedN = $ExpectedInstances
} elseif (Test-Path $runMetaPathForN) {
    $rmForN = Get-Content -Raw -Path $runMetaPathForN | ConvertFrom-Json
    if ($null -ne $rmForN.instanceCount -and [int]$rmForN.instanceCount -gt 0) { $resolvedN = [int]$rmForN.instanceCount }
}
if ($resolvedN -lt 2) { throw "Resolved N=$resolvedN is not a valid instance count (minimum 2: host + 1 join)." }

# Explicit per-instance overrides (-Join1Log/-Join2Log/-Join3Log), honored up
# to whatever N actually needs - a join beyond N-1 is never added, not even
# when its override param was passed (mirrors analyze_run4.ps1's same rule).
$joinOverridesForN = @{ 1 = $Join1Log; 2 = $Join2Log; 3 = $Join3Log }
$joinLabels = New-Object System.Collections.ArrayList
$joinLogsByLabel = [ordered]@{}
for ($i = 1; $i -lt $resolvedN; $i++) {
    $override = if ($joinOverridesForN.ContainsKey($i)) { $joinOverridesForN[$i] } else { "" }
    if ($override -eq "") { $override = Join-Path $RunDir "join$i.log" }
    [void]$joinLabels.Add("join$i")
    $joinLogsByLabel["join$i"] = $override
}

# ---- Scheduled-disconnect exempt-set resolution (05-03 gap-closure --------
# continuation: a rig-scheduled force-kill of a join, via disconnectAtSec,
# truncates that join's own observation window at an arbitrary point in its
# lifetime - unlike a graceful end (own SCENARIO RESULT/last log line), which
# is already handled by the existing $MinResolutionWindowMsLocal "insufficient
# observation window" exemption below. That existing exemption only covers a
# mismatch caught with LESS than one dump cycle (~6s) of runway before a
# side's true end; a scheduled-disconnect kill can - and, per run
# 20260902_144339_N4, does - truncate a join's life tens of seconds before a
# host-census-radius-straddling wildlife pack's already-documented 13s-140s-
# variable cold-load lag (docs\MILESTONE_A_GATE.md) has a chance to resolve,
# well outside that 6s floor. A "never converges" verdict (rule a) for such a
# key is therefore not evidence of PERMANENT divergence - it is evidence the
# harness itself cut the join's life short before convergence could be proven
# either way. Precedence for resolving which join(s) are scheduled-disconnect
# mirrors scripts\analyze_run4.ps1's existing pattern exactly: (i) run_meta.json
# archived in $RunDir; (ii) -RigConfig's raw instances (offline re-judge
# fallback); (iii) neither - empty set, no exemption. Scope is deliberately
# narrow: only rule (a)'s "never converges" case is affected (folded into the
# SAME $unresolved bucket the window-based exemption already uses, so rule
# (c)'s zero-tolerance final-dump check - which already skips $unresolved keys
# - is correctly exempted too for exactly these keys, never more). Rule (b)
# (permanent regression AFTER a proven convergence) is untouched - that has
# positive evidence of disagreement, not an absence-of-evidence gap. A pair
# with NO scheduled-disconnect side, and any dump comparison between two
# genuinely still-alive sides, keeps full pre-existing strictness.
$scheduledDisconnectJoinLabels = @()
# $joinLogsByLabel is already the N-sized join1..join(N-1) map built above
# (Phase 11 plan 02, TEST-01) - not re-initialized here.
$runMetaPath = Join-Path $RunDir "run_meta.json"
if (Test-Path $runMetaPath) {
    $runMetaJson = Get-Content -Raw -Path $runMetaPath | ConvertFrom-Json
    $exemptLogNames = @($runMetaJson.scheduledDisconnect)
    foreach ($label in $joinLogsByLabel.Keys) {
        if ($exemptLogNames -contains (Split-Path -Leaf $joinLogsByLabel[$label])) { $scheduledDisconnectJoinLabels += $label }
    }
    Write-Host "scheduled-disconnect exempt set (from run_meta.json): $(if ($scheduledDisconnectJoinLabels.Count -gt 0) { $scheduledDisconnectJoinLabels -join ', ' } else { '(none)' })"
} elseif ($RigConfig -ne "" -and (Test-Path $RigConfig)) {
    $rig = Get-Content -Raw -Path $RigConfig | ConvertFrom-Json
    $exemptLogNames = New-Object System.Collections.ArrayList
    if ($null -ne $rig.instances) {
        foreach ($inst in $rig.instances) {
            if ($null -ne $inst.disconnectAtSec -and "$($inst.disconnectAtSec)" -ne "") {
                if ($inst.logName) { [void]$exemptLogNames.Add("$($inst.logName)") }
            }
        }
    }
    foreach ($label in $joinLogsByLabel.Keys) {
        if ($exemptLogNames -contains (Split-Path -Leaf $joinLogsByLabel[$label])) { $scheduledDisconnectJoinLabels += $label }
    }
    Write-Host "scheduled-disconnect exempt set (from -RigConfig '$RigConfig'): $(if ($scheduledDisconnectJoinLabels.Count -gt 0) { $scheduledDisconnectJoinLabels -join ', ' } else { '(none)' })"
} else {
    Write-Host "scheduled-disconnect exempt set: (none - no run_meta.json and no -RigConfig)"
}

# ---- Pitfall 5 hard gate: all N logs must exist AND be non-empty -------------
# before any pair is judged. Never skip a missing/empty log and judge on the
# ones that happen to be present - that produces a "PASS" that examined fewer
# than N instances, which is worse than no verdict at all. (Phase 11 plan 02,
# TEST-01: N-relative, not hardcoded to 4.)
$required = [ordered]@{ host = $HostLog }
foreach ($label in $joinLabels) { $required[$label] = $joinLogsByLabel[$label] }
$missing = New-Object System.Collections.ArrayList
foreach ($name in $required.Keys) {
    $path = $required[$name]
    $bad = $false
    if (-not (Test-Path $path)) {
        $bad = $true
        [void]$missing.Add("$name (missing): $path")
    } elseif ((Get-Item $path).Length -le 0) {
        $bad = $true
        [void]$missing.Add("$name (empty): $path")
    }
}
if ($missing.Count -gt 0) {
    Write-Host "WNPC4 RESULT: FAIL reason=missing-or-empty-log"
    foreach ($m in $missing) { Write-Host "  $m" }
    throw "analyze_wnpc_diff4: $($missing.Count) of $resolvedN required logs missing/empty - refusing to judge on a partial log set (04-RESEARCH.md Pitfall 5)"
}

# ---- Parsing (copied verbatim from scripts\analyze_wnpc_diff.ps1's Get-Rows/ --
# Get-Dumps - the 2-log original is NOT edited in place; other callers depend
# on its exact 2-log contract). $GapMs is a script param here exactly as it is
# a script param there.

function Get-Rows([string]$File) {
    $rows = New-Object System.Collections.ArrayList
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WNPC hand=([\d,]+) " +
           "pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) cls=(\w+) name='([^']*)'"
    foreach ($m in (Select-String -Path $File -Pattern $pat)) {
        $g = $m.Matches[0].Groups
        if ($g[9].Value -eq "pc") { continue }   # player squad is not world state
        $t = ([int]$g[1].Value * 3600000) + ([int]$g[2].Value * 60000) +
             ([int]$g[3].Value * 1000) + [int]$g[4].Value
        [void]$rows.Add([pscustomobject]@{
            t = $t; hand = $g[5].Value
            x = [double]$g[6].Value; y = [double]$g[7].Value; z = [double]$g[8].Value
            cls = $g[9].Value; name = $g[10].Value })
    }
    return $rows
}

# 04-07 gap-closure (root cause 2, oracle legitimacy), REVISED 04-09 continuation
# (live gate cluster #1: join3's reconnectAtSec replacement-join pack, run
# 20260902_122859_N4). The ORIGINAL version of this function used the wall-clock
# ms of a log's own FIRST "SCENARIO RESULT" line as its exclusion boundary, on
# the theory that a process "keeps emitting SCENARIO WNPC dumps for a few
# seconds AFTER its own SCENARIO RESULT line (Plugin.cpp's self-exit hold)" and
# that trailing window is not a legitimate comparison point because the side
# is "no longer being actively driven/replicated". That was TRUE for the
# specific 04-07 case it was built for (host already dead, join trailing 84s
# further because of an unclamped join-duration timing bug that 04-07 ALSO
# fixed on the producer side). It is FALSE in general: measured directly on
# join3.log in run 20260902_122859_N4, the self-exit hold is a period of
# ACTIVE, ongoing replication, not stale/frozen data - join3 logs continuing
# [life]/[snap]/[gate]/[stats] activity for ~4s after its own RESULT line, and
# its FINAL SCENARIO WNPC dump (t=+1020ms after RESULT) is the one dump where
# a host-vouched "Hungry bandit" pack (hand=*,41,3942993152) FULLY CONVERGES
# (all 6 members cls=drv) - a dump the old RESULT-boundary excluded, freezing
# rule (c)'s "final comparable dump" evaluation on the PRIOR (still-converging)
# dump and producing a false FAIL on an already-converged pack. Now that the
# 04-07 timing fix (KENSHICOOP_SCENARIO_JOIN_DURATION_SEC) prevents a join from
# legitimately outliving the host by tens of seconds, the RESULT-line boundary
# is no longer needed to guard against that failure mode and is actively too
# tight for the ordinary self-exit hold. Use the log's own LAST timestamped
# line (true process end) as the boundary - already the established,
# precedented fallback for a log that never reaches RESULT at all (a scheduled
# disconnectAtSec force-kill, e.g. join2) - applied universally, so a
# self-exit hold's genuine trailing convergence data is never discarded on
# either side of a pair, while a dump sampled after a process has actually
# stopped logging (true end) is still correctly excluded (same legitimacy
# principle as CoopOraclesN.psm1's disconnect_isolation gate excluding TABMAP
# findings at/after a departed rank's own RECONNECT - a structurally-expected
# state transition must never be judged as a defect). Returns $null only for a
# pathological log with no timestamped line at all, so the caller never
# excludes on a missing bound.
function Get-LogEndTimeMs([string]$File) {
    $anyPat = "^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]"
    $last = Select-String -Path $File -Pattern $anyPat -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $last) {
        $g = $last.Matches[0].Groups
        return ([int]$g[1].Value * 3600000) + ([int]$g[2].Value * 60000) + ([int]$g[3].Value * 1000) + [int]$g[4].Value
    }
    return $null # pathological: no timestamped line at all - caller must not exclude on a missing bound
}

# The JOIN's own gameplay-start wall-clock ms (Get-Rows' H:M:S.mmm basis),
# from its first "SCENARIO MAGATE start ownRank=..." line - the anchor the
# bounded convergence window (rule a) is measured from, NOT from when any
# particular mismatch happens to begin. Falls back to the log's first
# timestamped line if the MAGATE marker is absent (older archived runs that
# predate the MAGATE schema), and to $null only if the log has no timestamped
# lines at all (pathological; caller must not apply a window in that case).
function Get-LogStartTimeMs([string]$File) {
    $startPat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO MAGATE start"
    $hit = Select-String -Path $File -Pattern $startPat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $hit) {
        $g = $hit.Matches[0].Groups
        return ([int]$g[1].Value * 3600000) + ([int]$g[2].Value * 60000) + ([int]$g[3].Value * 1000) + [int]$g[4].Value
    }
    $anyPat = "^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]"
    $first = Select-String -Path $File -Pattern $anyPat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $first) {
        $g = $first.Matches[0].Groups
        return ([int]$g[1].Value * 3600000) + ([int]$g[2].Value * 60000) + ([int]$g[3].Value * 1000) + [int]$g[4].Value
    }
    return $null
}

# Phase 11 (11-03 live matrix): the same WNPC grammar's cls=pc rows - each
# side's OWN squad bodies, which Get-Rows deliberately filters from the judged
# world state - reused here ONLY as distance anchors for the horizon-band
# exclusion in Get-PairwiseDiff. Deduped to a 50u grid (the squads sit in one
# town for a whole gate run; the raw row count is dumps x squad size).
function Get-PcAnchors([string]$File) {
    $seen = @{}; $anchors = New-Object System.Collections.ArrayList
    $pat = "SCENARIO WNPC hand=[\d,]+ pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) cls=pc"
    foreach ($m in (Select-String -Path $File -Pattern $pat)) {
        $g = $m.Matches[0].Groups
        $x = [double]$g[1].Value; $z = [double]$g[3].Value
        $key = "{0}:{1}" -f [Math]::Round($x / 50), [Math]::Round($z / 50)
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        [void]$anchors.Add([pscustomobject]@{ x = $x; z = $z })
    }
    return ,$anchors
}

# The effective census radius this run ACTUALLY used, parsed from the log's own
# effective-cfg line ("... censusR=2000 ..."). Falls back to the compiled-in
# default (2000) for an archived log that predates the cfg line.
function Get-CensusRadius([string]$File) {
    $hit = Select-String -Path $File -Pattern "censusR=(\d+)" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $hit) { return [double]$hit.Matches[0].Groups[1].Value }
    return 2000.0
}

function Get-MinAnchorDist([double]$x, [double]$z, $anchors) {
    $min = [double]::MaxValue
    foreach ($a in $anchors) {
        $dx = $x - $a.x; $dz = $z - $a.z
        $d = [Math]::Sqrt($dx * $dx + $dz * $dz)
        if ($d -lt $min) { $min = $d }
    }
    return $min
}

# Cluster a flat row list into per-dump buckets on a timestamp gap.
function Get-Dumps($rows, [int]$GapMsLocal) {
    $dumps = New-Object System.Collections.ArrayList
    $cur = $null; $last = -1
    foreach ($r in ($rows | Sort-Object t)) {
        if ($null -eq $cur -or ($r.t - $last) -gt $GapMsLocal) {
            if ($null -ne $cur) { [void]$dumps.Add($cur) }
            $cur = [pscustomobject]@{ t = $r.t; rows = (New-Object System.Collections.ArrayList) }
        }
        [void]$cur.rows.Add($r); $last = $r.t
    }
    if ($null -ne $cur) { [void]$dumps.Add($cur) }
    return $dumps
}

# One pairwise host-vs-join CONVERGENCE pass (04-09 gap-closure): reduces the
# same per-dump set-diffs the 2-log original computes, but judges divergence
# on the per-key TIMELINE across the paired-dump sequence, not on whether any
# single dump ever shows a mismatch. See the script-level .DESCRIPTION for the
# (a)/(b)/(c) rule definitions.
function Get-PairwiseDiff($hDumps, $jDumps, [int]$PairTolMsLocal, [int]$TopNLocal, [string]$JoinLabel,
                           [Nullable[int]]$HostEndMs = $null, [Nullable[int]]$JoinEndMs = $null,
                           [Nullable[int]]$JoinStartMs = $null, [int]$ConvergenceWindowMsLocal = 150000,
                           [bool]$JoinIsScheduledDisconnect = $false,
                           $HostPcAnchors = $null, $JoinPcAnchors = $null,
                           [double]$HorizonBandU = 0) {
    $paired = 0; $withJoinOnly = 0; $withHostOnly = 0
    $joinOnlyTotal = 0; $hostOnlyTotal = 0
    $offenders = @{}   # join-only: hand -> @{ n; name; cls; x; z }  (informational, unchanged shape)
    $hostOnly  = @{}   # host-only, same shape
    $excludedHostDumps = 0; $excludedJoinDumps = 0

    # 04-07 gap-closure: never pair a dump sampled at/after EITHER side's own
    # end-of-scenario (RESULT, or last line if RESULT never logged) - see
    # Get-LogEndTimeMs. A $null bound (pathological empty log) excludes
    # nothing, matching the pre-existing no-bound behavior.
    $hDumpsEligible = @($hDumps | Where-Object { $null -eq $HostEndMs -or $_.t -lt $HostEndMs })
    $excludedHostDumps = $hDumps.Count - $hDumpsEligible.Count
    $jDumpsEligible = @($jDumps | Where-Object { $null -eq $JoinEndMs -or $_.t -lt $JoinEndMs })
    $excludedJoinDumps = $jDumps.Count - $jDumpsEligible.Count

    # Build the ordered paired-dump sequence once (same nearest-neighbor
    # pairing as before) so both the legacy per-dump aggregates AND the new
    # per-key timeline are derived from the identical set of judged pairs.
    $pairedSeq = New-Object System.Collections.ArrayList
    foreach ($hd in $hDumpsEligible) {
        $jd = $jDumpsEligible | Sort-Object { [Math]::Abs($_.t - $hd.t) } | Select-Object -First 1
        if ($null -eq $jd -or [Math]::Abs($jd.t - $hd.t) -gt $PairTolMsLocal) { continue }
        [void]$pairedSeq.Add([pscustomobject]@{ t = $hd.t; hd = $hd; jd = $jd })
    }
    $paired = $pairedSeq.Count

    # timeline: hand-key -> ArrayList of @{ t; status } (status: match / host_only / join_only),
    # in chronological order, one entry per paired dump the key appears in
    # (host set OR join set - a key absent from both at a given dump is simply
    # not tracked for that dump, not treated as any status).
    $timeline = @{}

    foreach ($p in $pairedSeq) {
        $hSet = @{}; foreach ($r in $p.hd.rows) { $hSet[$r.hand] = $r }
        $jSet = @{}; foreach ($r in $p.jd.rows) { $jSet[$r.hand] = $r }

        # Phase 11 (11-03 live matrix, run 20260905_120057_N4 'Ninja Guard'):
        # a cls=hid row is a SUPPRESSED local body - the observing side's own
        # cull/suppression machinery actively hiding it (a census wide-cull
        # logs `[life] ... to=CULLED reason=cull-wide` and the audit dump
        # keeps reporting the suppressed corpse as cls=hid). When the OTHER
        # side does not list the hand at all, a hid row is the suppression
        # AGREEING with that absence - the convergence this oracle exists to
        # check has already happened at the life level - so it must not count
        # as one-sided presence. When the other side DOES list the hand, the
        # pair still counts as a match (the body exists on both sides;
        # suppression is a rendering veto, not non-existence), so a wrongly-
        # culled-but-host-listed body is never hidden by this rule.
        $jOnly = @($jSet.Keys | Where-Object { -not $hSet.ContainsKey($_) -and "$($jSet[$_].cls)" -ne "hid" })
        $hOnly = @($hSet.Keys | Where-Object { -not $jSet.ContainsKey($_) -and "$($hSet[$_].cls)" -ne "hid" })
        $common = @($hSet.Keys | Where-Object { $jSet.ContainsKey($_) })
        if ($jOnly.Count -gt 0) { $withJoinOnly++; $joinOnlyTotal += $jOnly.Count }
        if ($hOnly.Count -gt 0) { $withHostOnly++; $hostOnlyTotal += $hOnly.Count }

        # Phase 11 review WR-03: the horizon-band and drv-tail exemptions below
        # claim WHOLE-HISTORY properties ("every observation sits beyond the
        # band", "join_only ... whose rows read cls=drv" for the whole tail),
        # but the offender records used to retain only the FIRST/LAST position
        # and the LAST-seen cls. Track the per-key extremes AS observations
        # accumulate instead: minAnchorDist (the closest any observation ever
        # came to the observing side's own squad anchors) and clsEverNonDrv
        # (did ANY one-sided observation read a cls other than drv), so both
        # exemptions can gate on the history they document.
        foreach ($k in $jOnly) {
            $r = $jSet[$k]
            if (-not $offenders.ContainsKey($k)) {
                $offenders[$k] = @{ n = 0; name = $r.name; cls = $r.cls; x = $r.x; z = $r.z; lastX = $r.x; lastZ = $r.z
                                    minAnchorDist = [double]::MaxValue; clsEverNonDrv = $false }
            }
            $offenders[$k].n++
            $offenders[$k].cls = $r.cls
            $offenders[$k].lastX = $r.x; $offenders[$k].lastZ = $r.z
            if ("$($r.cls)" -ne "drv") { $offenders[$k].clsEverNonDrv = $true }
            if ($null -ne $JoinPcAnchors -and @($JoinPcAnchors).Count -gt 0) {
                $d = Get-MinAnchorDist $r.x $r.z $JoinPcAnchors
                if ($d -lt $offenders[$k].minAnchorDist) { $offenders[$k].minAnchorDist = $d }
            }
            if (-not $timeline.ContainsKey($k)) { $timeline[$k] = New-Object System.Collections.ArrayList }
            [void]$timeline[$k].Add([pscustomobject]@{ t = $p.t; status = "join_only" })
        }
        foreach ($k in $hOnly) {
            $r = $hSet[$k]
            if (-not $hostOnly.ContainsKey($k)) {
                $hostOnly[$k] = @{ n = 0; name = $r.name; cls = $r.cls; x = $r.x; z = $r.z; lastX = $r.x; lastZ = $r.z
                                   minAnchorDist = [double]::MaxValue }
            }
            $hostOnly[$k].n++
            $hostOnly[$k].lastX = $r.x; $hostOnly[$k].lastZ = $r.z
            if ($null -ne $HostPcAnchors -and @($HostPcAnchors).Count -gt 0) {
                $d = Get-MinAnchorDist $r.x $r.z $HostPcAnchors
                if ($d -lt $hostOnly[$k].minAnchorDist) { $hostOnly[$k].minAnchorDist = $d }
            }
            if (-not $timeline.ContainsKey($k)) { $timeline[$k] = New-Object System.Collections.ArrayList }
            [void]$timeline[$k].Add([pscustomobject]@{ t = $p.t; status = "host_only" })
        }
        foreach ($k in $common) {
            if (-not $timeline.ContainsKey($k)) { $timeline[$k] = New-Object System.Collections.ArrayList }
            [void]$timeline[$k].Add([pscustomobject]@{ t = $p.t; status = "match" })
        }
    }

    # ---- Convergence-rule evaluation (a)/(b), per key -------------------------
    $violations = New-Object System.Collections.ArrayList   # @{ key; rule; detail }
    $latenciesMs = New-Object System.Collections.ArrayList  # convergence latency, ms - metrics only
    $departed = New-Object System.Collections.ArrayList     # keys that vanished from BOTH sides before run end - not a violation
    $deadlineMs = if ($null -ne $JoinStartMs) { $JoinStartMs + $ConvergenceWindowMsLocal } else { $null }

    # 04-09 continuation gap-closure (live gate cluster #2: Goat/Garru wildlife
    # cluster, run 20260902_122859_N4). $lastPairedT is the timestamp of the
    # LAST paired dump this pair actually judged. A per-key timeline entry only
    # exists for a dump where the key appeared in EITHER side's row set (see the
    # timeline-build loop above) - so if a key's last timeline entry is EARLIER
    # than $lastPairedT, there was at least one MORE paired dump after that
    # point where the key appeared on NEITHER side: the body left both host's
    # AND the join's own enumeration entirely (measured: a host-census-radius-
    # bounded NPC - e.g. a Goat/Garru wandering out to ~2500u - drops out of the
    # host's own census/audit-dump the instant it exits censusRadius_*1.25, while
    # the join's `targets_` drive entry (30s TARGET_STALE_MS grace,
    # ReplicatorDrive.cpp ageOutStaleTargets) keeps reporting it as cls=drv for
    # up to 30s longer via pure client-side extrapolation, until it too ages out
    # and stops appearing - after which NEITHER side ever mentions the hand
    # again). Once both sides have stopped enumerating a body, nobody is
    # rendering it and nobody can observe two disagreeing copies of it - the
    # DoD's "no permanent desync" is about a state BOTH players could witness
    # disagree, not about the two engines briefly disagreeing on when to stop
    # caring about a body neither is looking at. Treat this as "departed", not
    # a rule (a)/(b) violation. This does NOT touch rule (c): a key that is
    # still present in the run's true final comparable dump on only one side
    # is unaffected (its last timeline entry equals $lastPairedT, so it is
    # never classified "departed") and stays zero-tolerance as before.
    $lastPairedT = if ($pairedSeq.Count -gt 0) { $pairedSeq[$pairedSeq.Count - 1].t } else { $null }

    # 04-09 continuation gap-closure #2 (live gate cluster: a brand-new NPC that
    # enters range seconds before a short-lived reconnectAtSec join's own true
    # end, run 20260902_130704_N4 - hand=6,3814209024,1,15,3735595776 "Escaped
    # Servant"). SCENARIO WNPC audit dumps fire on a ~5s cadence
    # (auditRows_/notePlatoons in ReplicatorAuthority.cpp/ReplicatorPublish.cpp),
    # far coarser than the sub-second resolve pipeline it is meant to observe.
    # Measured directly: join3's own `[life] ... from=UNKNOWN to=HI reason=drive`
    # line proves it bound and started DRIVING this hand ~1s after the host
    # first census-vouched it - genuine, fast, correct convergence - but join3's
    # true end (its own last timestamped line) landed only ~2.8s later, before
    # the NEXT scheduled ~5s WNPC dump could ever fire to record that outcome.
    # The oracle only parses SCENARIO WNPC rows (Get-Rows), not the finer-grained
    # [life] signal, so it saw zero dumps confirming the bind and flagged a
    # "never converges" + final-dump violation for an NPC that, by the higher-
    # frequency evidence, almost certainly HAD converged - we just structurally
    # never got another dump to prove it either way. $MinResolutionWindowMs is a
    # DELIBERATELY CONSERVATIVE floor (a bit over one dump cycle) that exempts
    # ONLY a key whose first-ever mismatch appears with less than one dump's
    # worth of runway left before the shorter-lived side's own true end - i.e.
    # cases where there was no structural possibility of a confirming dump, not
    # cases that simply ran out of luck within a generous window. This does NOT
    # touch rule (b) (a regression has POSITIVE evidence of disagreement, not an
    # absence-of-evidence gap) and only suppresses rule (c) for the SAME key
    # (tracked via $unresolved below), preserving zero-tolerance for every
    # violation that had a real chance to be observed.
    $MinResolutionWindowMsLocal = 6000
    # Phase 11 (11-03 live matrix, run 20260905_112447_N4 Garru herd): a body
    # that exits the host's census/publish horizon leaves the join holding a
    # cls=drv driven-extrapolation TAIL that the join structurally CANNOT stop
    # reporting faster than its own targets_ age-out grace (TARGET_STALE_MS =
    # 30000 ms, ReplicatorDrive.cpp ageOutStaleTargets) plus one ~5 s WNPC dump
    # cycle - the EXACT mechanism the 04-09 'departed' comment already
    # documents ("keeps reporting it as cls=drv for up to 30s longer via pure
    # client-side extrapolation, until it too ages out"). When the shorter
    # side's own end lands INSIDE that 35 s window (measured live: the join's
    # scenario ended 29 s after the herd crossed the horizon, one second
    # before its own age-out would have fired), the aging-out had no
    # structural possibility of being observed - the same class of
    # absence-of-evidence gap as $MinResolutionWindowMsLocal, with the
    # mechanism's own measured constant. Applies ONLY to keys that are
    # join_only for their WHOLE mismatch history AND whose rows read cls=drv
    # (a pure extrapolation tail); a host_only key, a paired divergence, or a
    # non-driven join ghost keeps the strict 6 s floor.
    $DrvTailWindowMsLocal = 35000
    $shortestEndMs = $null
    foreach ($cand in @($HostEndMs, $JoinEndMs)) {
        if ($null -eq $cand) { continue }
        if ($null -eq $shortestEndMs -or $cand -lt $shortestEndMs) { $shortestEndMs = $cand }
    }
    $unresolved = New-Object System.Collections.ArrayList   # keys exempted for insufficient observation window
    $unresolvedReasons = @{}   # key -> reason string, for verbose reporting only

    # 05-03 gap-closure continuation: this pair's observation window was cut
    # short by a rig-scheduled force-kill (disconnectAtSec) of the join side,
    # not by either side's own natural end. See the script-level exempt-set
    # resolution block above for the full reasoning. The equality check below
    # (not just "is this join exempt") matters: it confirms the join's own
    # forced-kill end is ACTUALLY the bound that limited this pair's window
    # ($shortestEndMs) - if the host somehow ended first for an unrelated
    # reason, that is a real, unrelated timing issue and must stay fully
    # judged, not silently hidden behind the join's scheduled-disconnect flag.
    $scheduledDisconnectTruncated = $JoinIsScheduledDisconnect -and $null -ne $JoinEndMs -and
                                     $null -ne $shortestEndMs -and $shortestEndMs -eq $JoinEndMs

    foreach ($k in $timeline.Keys) {
        $seq = @($timeline[$k] | Sort-Object t)
        $firstMismatch = $seq | Where-Object { $_.status -ne "match" } | Select-Object -First 1
        if ($null -eq $firstMismatch) { continue }   # always matched - not interesting

        # Phase 11 (11-03 live matrix) horizon-band exclusion: a key that was
        # NEVER paired (one-sided for its whole observed history) and whose
        # every observation sits beyond the census EDGE band (censusRadius *
        # 0.8, mirroring ReplicatorAuthority.cpp's own EDGE_BAND diagnostic
        # constant) from the observing side's OWN squad anchors lives in the
        # documented enumeration-uncertainty band: the census existence
        # contract deliberately publishes 25% wider than the join culls
        # (ReplicatorPublish.cpp's margin comment - "an unstreamed far NPC is
        # locally simulated on BOTH sides, so its two positions legitimately
        # diverge"), and zone-loading discretization means the two sides'
        # enumeration sets legitimately differ out there (the Phase 4
        # 04-07-SUMMARY pattern-(2) known limitation: the SAME static 'Thief
        # Boss' camp at -51767,4731 flagged there resurfaced one-sidedly in
        # run 20260905_112042_N4 the moment longer join observation windows
        # existed). A key that was EVER paired keeps full judgment at ANY
        # distance - a real relay/census defect pairs first, then diverges,
        # and always happens where players actually are. Excluded keys are
        # REPORTED via the unresolved list (never silently dropped).
        $allStatuses = @($seq | ForEach-Object { $_.status } | Sort-Object -Unique)
        if ($HorizonBandU -gt 0 -and $allStatuses.Count -eq 1 -and $allStatuses[0] -ne "match") {
            $rec = $null; $anch = $null
            if ($allStatuses[0] -eq "join_only" -and $offenders.ContainsKey($k)) {
                $rec = $offenders[$k]; $anch = $JoinPcAnchors
            } elseif ($allStatuses[0] -eq "host_only" -and $hostOnly.ContainsKey($k)) {
                $rec = $hostOnly[$k]; $anch = $HostPcAnchors
            }
            # Phase 11 review WR-03: gate on the WHOLE-HISTORY minimum anchor
            # distance (accumulated per observation in the collection loop),
            # not just the first/last positions - a never-paired ghost that
            # wanders INSIDE the band mid-history (near players, exactly where
            # a real census/relay defect lives) but starts and ends far away
            # must stay fully judged.
            if ($null -ne $rec -and $null -ne $anch -and @($anch).Count -gt 0 -and
                $rec.minAnchorDist -lt [double]::MaxValue) {
                if ($rec.minAnchorDist -gt $HorizonBandU) {
                    [void]$unresolved.Add($k)
                    $unresolvedReasons[$k] = ("horizon-band one-sided body (never paired; every observation d_min={0:N0}u beyond the {1:N0}u census edge band - enumeration-uncertainty zone, no existence contract)" -f $rec.minAnchorDist, $HorizonBandU)
                    continue
                }
            }
        }

        $firstConv = $seq | Where-Object { $_.status -eq "match" -and $_.t -ge $firstMismatch.t } | Select-Object -First 1
        if ($null -eq $firstConv) {
            $lastEntry = $seq[$seq.Count - 1]
            if ($null -ne $lastPairedT -and $lastEntry.t -lt $lastPairedT) {
                [void]$departed.Add($k)   # vanished from both sides before run end - not rule (a)
                continue
            }
            # Effective resolution window for THIS key: the strict one-dump-cycle
            # floor, widened to the driven-extrapolation age-out constant when
            # the key is a pure join-side cls=drv tail (see $DrvTailWindowMsLocal).
            $resWinMs = $MinResolutionWindowMsLocal
            $mismStatuses = @($seq | Where-Object { $_.status -ne "match" } |
                              ForEach-Object { $_.status } | Sort-Object -Unique)
            # Phase 11 review WR-03: the wide window requires cls=drv across the
            # key's WHOLE one-sided history (clsEverNonDrv, accumulated per
            # observation), not just the last-seen cls - a ghost that was
            # cls=wld for most of its history and flipped to drv at the end is
            # not a pure extrapolation tail and keeps the strict 6 s floor.
            if ($mismStatuses.Count -eq 1 -and $mismStatuses[0] -eq "join_only" -and
                $offenders.ContainsKey($k) -and -not $offenders[$k].clsEverNonDrv) {
                $resWinMs = $DrvTailWindowMsLocal
            }
            if ($null -ne $shortestEndMs -and ($shortestEndMs - $firstMismatch.t) -lt $resWinMs) {
                [void]$unresolved.Add($k)   # no dump cycle had time to confirm either way - not rule (a)/(c)
                $unresolvedReasons[$k] = if ($resWinMs -eq $DrvTailWindowMsLocal) {
                    "driven-extrapolation age-out window (targets_ 30s grace + dump cycle > remaining runway)"
                } else { "insufficient observation window" }
                continue
            }
            if ($scheduledDisconnectTruncated) {
                # A rig-scheduled force-kill of $JoinLabel truncated this pair's
                # window before the mismatch could be proven permanent either
                # way - not rule (a)/(c) (05-03 gap-closure continuation).
                [void]$unresolved.Add($k)
                $unresolvedReasons[$k] = "scheduled-disconnect truncation ($JoinLabel force-killed at t=${JoinEndMs}ms)"
                continue
            }
            # Rule (a): never converges at all within the observed data.
            [void]$violations.Add(@{ key = $k; rule = "a"
                detail = "$($firstMismatch.status) since t=$($firstMismatch.t)ms, never converges (no subsequent match observed)" })
            continue
        }

        $latencyMs = $firstConv.t - $firstMismatch.t
        if ($null -ne $deadlineMs -and $firstConv.t -gt $deadlineMs) {
            # Rule (a): converged too late - outside the bounded cold-start window.
            [void]$violations.Add(@{ key = $k; rule = "a"
                detail = "converged at t=$($firstConv.t)ms, after window deadline $($deadlineMs)ms (onset t=$($firstMismatch.t)ms, JoinStart=$JoinStartMs, window=${ConvergenceWindowMsLocal}ms)" })
        } else {
            [void]$latenciesMs.Add($latencyMs)
        }

        # Rule (b): after converging, does it permanently regress (mismatch
        # again with no further match for the rest of the observed sequence)?
        $afterConv = @($seq | Where-Object { $_.t -gt $firstConv.t })
        if ($afterConv.Count -gt 0 -and $afterConv[$afterConv.Count - 1].status -ne "match") {
            $lastEntry = $afterConv[$afterConv.Count - 1]
            if ($null -ne $lastPairedT -and $lastEntry.t -lt $lastPairedT) {
                [void]$departed.Add($k)   # vanished from both sides before run end - not rule (b)
                continue
            }
            $regressionOnset = $null
            for ($i = $afterConv.Count - 1; $i -ge 0; $i--) {
                if ($afterConv[$i].status -eq "match") { break }
                $regressionOnset = $afterConv[$i]
            }
            if ($null -ne $regressionOnset) {
                # Same driven-extrapolation age-out exemption as the rule (a)
                # branch above, applied to the REGRESSION onset: a converged key
                # whose terminal regression is a pure join_only cls=drv tail
                # with less than the 35 s age-out runway left is structurally
                # unobservable, not a permanent regression.
                $regStatuses = @($afterConv | Where-Object { $_.status -ne "match" } |
                                 ForEach-Object { $_.status } | Sort-Object -Unique)
                # Phase 11 review WR-03: same whole-history clsEverNonDrv gate
                # as the rule (a) branch above (last-seen cls could mislabel a
                # mostly-wld ghost as a pure drv tail).
                $regIsDrvTail = ($regStatuses.Count -eq 1 -and $regStatuses[0] -eq "join_only" -and
                                 $offenders.ContainsKey($k) -and -not $offenders[$k].clsEverNonDrv)
                if ($regIsDrvTail -and $null -ne $shortestEndMs -and
                    ($shortestEndMs - $regressionOnset.t) -lt $DrvTailWindowMsLocal) {
                    [void]$unresolved.Add($k)
                    $unresolvedReasons[$k] = "driven-extrapolation age-out window (targets_ 30s grace + dump cycle > remaining runway after regression onset)"
                } else {
                    [void]$violations.Add(@{ key = $k; rule = "b"
                        detail = "converged at t=$($firstConv.t)ms then permanently regressed to $($regressionOnset.status) from t=$($regressionOnset.t)ms (no further match observed)" })
                }
            }
        }
    }

    # ---- Rule (c): zero tolerance at the final comparable dump -----------------
    if ($pairedSeq.Count -gt 0) {
        $lastPair = $pairedSeq[$pairedSeq.Count - 1]
        $hSetF = @{}; foreach ($r in $lastPair.hd.rows) { $hSetF[$r.hand] = $r }
        $jSetF = @{}; foreach ($r in $lastPair.jd.rows) { $jSetF[$r.hand] = $r }
        # Same cls=hid suppression-agreement rule as the per-dump loop above.
        $finalMismatchKeys = @($hSetF.Keys | Where-Object { -not $jSetF.ContainsKey($_) -and "$($hSetF[$_].cls)" -ne "hid" }) +
                             @($jSetF.Keys | Where-Object { -not $hSetF.ContainsKey($_) -and "$($jSetF[$_].cls)" -ne "hid" })
        foreach ($k in $finalMismatchKeys) {
            if ($unresolved -contains $k) { continue }   # same insufficient-window exemption as rule (a)
            if (-not @($violations | Where-Object { $_.key -eq $k -and $_.rule -eq "c" })) {
                [void]$violations.Add(@{ key = $k; rule = "c"; detail = "differs at the final comparable dump t=$($lastPair.t)ms" })
            }
        }
    }

    Write-Host ""
    Write-Host "== host vs $JoinLabel =="
    Write-Host "paired dumps: $paired"
    Write-Host "  dumps where $JoinLabel holds a hand the host does not: $withJoinOnly ($joinOnlyTotal row(s) total)"
    Write-Host "  dumps where the HOST holds a hand $JoinLabel does not: $withHostOnly ($hostOnlyTotal row(s) total)"
    if ($excludedHostDumps -gt 0 -or $excludedJoinDumps -gt 0) {
        Write-Host "  excluded (at/after own SCENARIO RESULT or exit): host=$excludedHostDumps dump(s) $JoinLabel=$excludedJoinDumps dump(s) - never judged, not counted above"
    }

    if ($offenders.Count -gt 0) {
        Write-Host ""
        Write-Host "$JoinLabel-only bodies, by how many dumps they persisted:"
        $ranked = $offenders.GetEnumerator() | Sort-Object { -$_.Value.n }
        foreach ($e in ($ranked | Select-Object -First $TopNLocal)) {
            Write-Host ("  {0,3} dump(s)  cls={1,-5} {2,-28} at {3,9:N0},{4,9:N0}  hand={5}" -f `
                        $e.Value.n, $e.Value.cls, "'$($e.Value.name)'", $e.Value.x, $e.Value.z, $e.Key)
        }
        if ($offenders.Count -gt $TopNLocal) {
            Write-Host "  ... and $($offenders.Count - $TopNLocal) more"
        }
        Write-Host ""
        Write-Host "distinct $JoinLabel-only bodies: $($offenders.Count)"
    }

    if ($hostOnly.Count -gt 0) {
        Write-Host ""
        Write-Host "host-only bodies (vs $JoinLabel), by how many dumps they persisted:"
        $rankedH = $hostOnly.GetEnumerator() | Sort-Object { -$_.Value.n }
        foreach ($e in ($rankedH | Select-Object -First $TopNLocal)) {
            Write-Host ("  {0,3} dump(s)  {1,-28} at {2,9:N0},{3,9:N0}  hand={4}" -f `
                        $e.Value.n, "'$($e.Value.name)'", $e.Value.x, $e.Value.z, $e.Key)
        }
        if ($hostOnly.Count -gt $TopNLocal) { Write-Host "  ... and $($hostOnly.Count - $TopNLocal) more" }
        Write-Host ""
        Write-Host "distinct host-only bodies (vs $JoinLabel): $($hostOnly.Count)"
    }

    Write-Host ""
    Write-Host "convergence: $($latenciesMs.Count) key(s) converged within the ${ConvergenceWindowSec}s window (cold-start lag, not a violation)"
    if ($latenciesMs.Count -gt 0) {
        $sorted = @($latenciesMs | Sort-Object)
        $avg = [Math]::Round((($sorted | Measure-Object -Sum).Sum / $sorted.Count), 0)
        Write-Host "  latency ms: min=$($sorted[0]) max=$($sorted[$sorted.Count-1]) avg=$avg"
    }
    if ($violations.Count -gt 0) {
        Write-Host "  $($violations.Count) CONVERGENCE VIOLATION(s) vs $($JoinLabel):"
        foreach ($v in ($violations | Select-Object -First $TopNLocal)) {
            Write-Host ("    rule={0}  hand={1}  {2}" -f $v.rule, $v.key, $v.detail)
        }
        if ($violations.Count -gt $TopNLocal) { Write-Host "    ... and $($violations.Count - $TopNLocal) more" }
    }
    if ($departed.Count -gt 0) {
        Write-Host "  $($departed.Count) key(s) departed BOTH sides' enumeration before the run's final comparable dump (mutual disappearance, not a violation)"
        foreach ($dk in ($departed | Select-Object -First $TopNLocal)) {
            Write-Host "    hand=$dk"
        }
        if ($departed.Count -gt $TopNLocal) { Write-Host "    ... and $($departed.Count - $TopNLocal) more" }
    }
    if ($unresolved.Count -gt 0) {
        Write-Host "  $($unresolved.Count) key(s) had no dump cycle with time to confirm convergence before run end (insufficient observation window, not a violation)"
        foreach ($uk in ($unresolved | Select-Object -First $TopNLocal)) {
            $reason = if ($unresolvedReasons.ContainsKey($uk)) { $unresolvedReasons[$uk] } else { "insufficient observation window" }
            Write-Host "    hand=$uk ($reason)"
        }
        if ($unresolved.Count -gt $TopNLocal) { Write-Host "    ... and $($unresolved.Count - $TopNLocal) more" }
    }

    return [pscustomobject]@{
        label          = $JoinLabel
        paired         = $paired
        joinOnlyTotal  = $joinOnlyTotal
        hostOnlyTotal  = $hostOnlyTotal
        distinctJoinOnly = $offenders.Count
        distinctHostOnly = $hostOnly.Count
        departedKeys   = @($departed).Count
        unresolvedKeys = @($unresolved).Count
        excludedHostDumps = $excludedHostDumps
        excludedJoinDumps = $excludedJoinDumps
        violations     = @($violations)
        convergenceLatenciesMs = @($latenciesMs)
        converged      = ($violations.Count -eq 0)
    }
}

# ---- Run the three pairwise diffs ---------------------------------------------

$ConvergenceWindowMs = $ConvergenceWindowSec * 1000

$hRows  = Get-Rows $HostLog
$hDumps = @(Get-Dumps $hRows $GapMs)
$hostEndMs = Get-LogEndTimeMs $HostLog
Write-Host "host: $($hRows.Count) rows in $($hDumps.Count) dumps (own end (last log line) at t=$hostEndMs ms)"

# Phase 11 horizon-band exclusion inputs: each side's own squad anchors (its
# cls=pc rows) + the run's effective census radius, banded at the same 0.8
# fraction ReplicatorAuthority.cpp's EDGE_BAND uses.
$hostPcAnchors = Get-PcAnchors $HostLog
$horizonBandU  = 0.8 * (Get-CensusRadius $HostLog)

$pairs = @()
foreach ($p in @($joinLabels | ForEach-Object { @{ label = $_; log = $joinLogsByLabel[$_] } })) {
    $jRows  = Get-Rows $p.log
    $jDumps = @(Get-Dumps $jRows $GapMs)
    $joinEndMs = Get-LogEndTimeMs $p.log
    $joinStartMs = Get-LogStartTimeMs $p.log
    $joinPcAnchors = Get-PcAnchors $p.log
    $joinIsScheduledDisconnect = $scheduledDisconnectJoinLabels -contains $p.label
    Write-Host "$($p.label): $($jRows.Count) rows in $($jDumps.Count) dumps (own end (last log line) at t=$joinEndMs ms, own gameplay start at t=$joinStartMs ms$(if ($joinIsScheduledDisconnect) { ', scheduled-disconnect' } else { '' }))"
    $pairs += Get-PairwiseDiff $hDumps $jDumps $PairTolMs $TopN $p.label $hostEndMs $joinEndMs $joinStartMs $ConvergenceWindowMs $joinIsScheduledDisconnect `
                               $hostPcAnchors $joinPcAnchors $horizonBandU
}

# ---- Verdict --------------------------------------------------------------
# Host-authoritative census means every join's roster should EVENTUALLY
# CONVERGE to match the host's (see script .DESCRIPTION for the (a)/(b)/(c)
# convergence rules Get-PairwiseDiff judges each pair against); (N-1) pairwise-
# converges-with-host implies all N agree transitively (04-RESEARCH.md Open
# Question 3's chosen approach - the fastest correct N-instance implementation,
# not a full N-way set-equality check). At N=2 this degenerates to exactly the
# single host-vs-join1 pairwise diff (Phase 11 plan 02, TEST-01).
$divergent = @($pairs | Where-Object { -not $_.converged })

Write-Host ""
Write-Host "== summary =="
foreach ($p in $pairs) {
    $verdict = if ($p.converged) { "CONVERGED" } else { "DIVERGED" }
    $latCount = @($p.convergenceLatenciesMs).Count
    Write-Host ("  host vs {0}: {1} (paired={2} joinOnly={3} hostOnly={4} excludedHost={5} excludedJoin={6} violations={7} convergedKeys={8} departed={9} unresolved={10})" -f `
                $p.label, $verdict, $p.paired, $p.joinOnlyTotal, $p.hostOnlyTotal, $p.excludedHostDumps, $p.excludedJoinDumps, `
                @($p.violations).Count, $latCount, $p.departedKeys, $p.unresolvedKeys)
}
Write-Host ""

if ($divergent.Count -gt 0) {
    $names = ($divergent | ForEach-Object { $_.label }) -join ","
    Write-Host "WNPC4 RESULT: FAIL reason=census-divergence pairs=$names"
    exit 1
} else {
    Write-Host "WNPC4 RESULT: PASS"
    exit 0
}
