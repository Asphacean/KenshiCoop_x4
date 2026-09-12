<#
.SYNOPSIS
  Offline, read-only timeline extractor for one N-way census-race run dir
  (Phase 12 Plan 02, CENSUS-02; the root-cause half).

.DESCRIPTION
  DESCRIPTION, NOT JUDGMENT. This script deliberately does NOT re-implement
  the census_convergence verdict - scripts\analyze_wnpc_diff4.ps1 owns that,
  its tolerances are pinned by scripts\tests\CensusRepro.Tests.ps1, and a
  second opinion about PASS/FAIL is exactly the "loosen the oracle" escape
  Phase 12 forbids. What this script adds is the DESCRIPTION the oracle does
  not print: when each instance arrived, what the host was doing at that
  moment, how the census cadence ran on each side, and - for every roster key
  that ends up one-sided - which side held it, what authority class it carried
  there, how far it sat from the observing side's own squad, and whether the
  two sides ever agreed about it before they stopped agreeing.

  It reuses the oracle's own vocabulary (paired / joinOnly / hostOnly /
  unresolved / departed, the SCENARIO WNPC row grammar, the 1000 ms dump-gap
  and 4000 ms pairing window) so the two outputs can be read side by side,
  and it reads the same archived logs with no writes of any kind.

  Sections printed, in order:
    1. ARRIVAL TIMELINE   - per-instance gameplay/scenario start, the host's
                            peer-connected events with their ids, the host's
                            save-push window around each connect, and the
                            wall-clock delta between the two joins' arrivals.
    2. CENSUS CADENCE     - the host's census send lines and each join's
                            census receive lines: count, time span, the
                            per-owner row counts they carry, and any
                            census-staleness edges logged.
    3. DIVERGENCE         - per host-join pair, built from the roster rows
                            both sides dump: the keys present on only one side
                            at the final comparable sample, when each was first
                            and last seen on each side, its authority class
                            history, its distance from the observing side's own
                            squad, and whether it ever agreed before regressing.
    4. SUMMARY            - one greppable final line.

  "divergedPairs" in the summary line names the pairs that still hold a
  one-sided key AT THE FINAL COMPARABLE PAIRED DUMP. That is a description of
  the tail state, not the oracle's verdict, and the two can legitimately
  differ (the oracle also exempts horizon-band, driven-age-out and departed
  keys). Read analyze_wnpc_diff4.ps1 for the verdict.

.PARAMETER RunDir
  The run directory to describe. Defaults to the content of
  tools\test-runs\last_census_repro.txt (written by scripts\repro_census_n3.ps1),
  so a verify command needs no path.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze_census_race.ps1

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze_census_race.ps1 -RunDir tools\test-runs\EXP1_schedule
#>
[CmdletBinding()]
param(
    [string]$RunDir = "",
    [int]$GapMs = 1000,          # same dump-gap the oracle clusters rows on
    [int]$PairTolMs = 4000,      # same host<->join dump pairing window
    [int]$TopN = 12,             # how many one-sided keys to name per pair
    [int]$ExpectedInstances = 0  # 0 = resolve from run_meta.json, else count join*.log
)

$ErrorActionPreference = "Stop"
$INV = [System.Globalization.CultureInfo]::InvariantCulture

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

# ---- Resolve the run dir ------------------------------------------------------
if ($RunDir -eq "") {
    $pointer = Join-Path $repoRoot "tools\test-runs\last_census_repro.txt"
    if (-not (Test-Path $pointer)) {
        throw "No -RunDir given and no pointer file at $pointer (run scripts\repro_census_n3.ps1 once, or pass -RunDir)."
    }
    $RunDir = (Get-Content -Raw -Path $pointer).Trim().Trim([char]0xFEFF)
}
if (-not (Test-Path $RunDir)) { throw "Run dir not found: $RunDir" }
$RunDir = (Resolve-Path $RunDir).Path

# ---- Resolve N ----------------------------------------------------------------
$resolvedN = 0
if ($ExpectedInstances -gt 0) {
    $resolvedN = $ExpectedInstances
} else {
    $metaPath = Join-Path $RunDir "run_meta.json"
    if (Test-Path $metaPath) {
        $meta = (Get-Content -Raw -Path $metaPath).TrimStart([char]0xFEFF) | ConvertFrom-Json
        if ($null -ne $meta.instanceCount -and [int]$meta.instanceCount -gt 0) { $resolvedN = [int]$meta.instanceCount }
    }
}
if ($resolvedN -le 0) {
    $resolvedN = 1 + @(Get-ChildItem -Path $RunDir -Filter "join*.log" -File -ErrorAction SilentlyContinue |
                       Where-Object { $_.Name -match '^join\d+\.log$' }).Count
}
if ($resolvedN -lt 2) { throw "Resolved N=$resolvedN is not a valid instance count (minimum 2: host + 1 join)." }

$labels = New-Object System.Collections.ArrayList
[void]$labels.Add("host")
for ($i = 1; $i -lt $resolvedN; $i++) { [void]$labels.Add("join$i") }

$logPath = @{}
foreach ($l in $labels) {
    $p = Join-Path $RunDir "$l.log"
    if (-not (Test-Path $p)) { throw "Missing log for instance '$l': $p" }
    $logPath[$l] = $p
}

# ---- Helpers ------------------------------------------------------------------
function ConvertTo-Ms([string]$hh, [string]$mm, [string]$ss, [string]$mmm) {
    return ([int]$hh * 3600000) + ([int]$mm * 60000) + ([int]$ss * 1000) + [int]$mmm
}
function Format-Ms([Nullable[int]]$ms) {
    if ($null -eq $ms) { return "-" }
    $h = [Math]::Floor($ms / 3600000); $r = $ms % 3600000
    $m = [Math]::Floor($r / 60000);    $r = $r % 60000
    $s = [Math]::Floor($r / 1000);     $f = $r % 1000
    return [string]::Format($INV, "{0:00}:{1:00}:{2:00}.{3:000}", $h, $m, $s, $f)
}
function Format-Num($v, [int]$dec = 0) {
    if ($null -eq $v) { return "-" }
    return ([double]$v).ToString("F$dec", $INV)
}

# One pass per log; every fact this script prints comes out of this parse.
function Read-InstanceLog([string]$File) {
    $o = [pscustomobject]@{
        rows          = (New-Object System.Collections.ArrayList)  # SCENARIO WNPC, cls != pc
        pcRows        = (New-Object System.Collections.ArrayList)  # SCENARIO WNPC, cls = pc
        censusSent    = (New-Object System.Collections.ArrayList)
        censusRecv    = (New-Object System.Collections.ArrayList)
        staleEdges    = (New-Object System.Collections.ArrayList)
        peerConnects  = (New-Object System.Collections.ArrayList)
        saveWindows   = (New-Object System.Collections.ArrayList)
        loadGo        = $null
        connectingMs  = $null
        gameplayMs    = $null
        magateMs      = $null
        ownRank       = $null
        firstMs       = $null
        lastMs        = $null
    }
    $reStamp   = '^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]'
    $reWnpc    = "SCENARIO WNPC hand=([\d,]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) cls=(\w+) name='([^']*)'"
    $reSent    = '\[census\] sent n=(\d+) radius=(\d+) mid=(\d+) fast=(\d+) anchors=(\d+)(.*?) enum=(\d+) notmine=(\d+) proxyrow=(\d+) attnR=(\d+)'
    $reRecv    = '\[census\] recv owner=(\d+) n=(\d+) kept=(\d+) offcell=(\d+) culls=(\d+).*?owners=(\d+)'
    $reConn    = '\[net\] peer connected id=(\d+) player=(\d+)'
    $reSave    = "\[save\] LOCAL-SAVE name='([^']*)'"
    $reQuiesce = "\[save\] QUIESCED kind=(\w+) name='([^']*)' files=(\d+) bytes=(\d+) waitMs=(\d+)"
    $reLoadGo  = "\[load\] GO id=(\d+) name='([^']*)' fp=([0-9a-f]+)"
    $reMagate  = 'SCENARIO MAGATE start ownRank=(\d+)'

    $pendingSaveMs = $null
    foreach ($line in [System.IO.File]::ReadLines($File)) {
        $ms = $null
        $m = [regex]::Match($line, $reStamp)
        if ($m.Success) {
            $ms = ConvertTo-Ms $m.Groups[1].Value $m.Groups[2].Value $m.Groups[3].Value $m.Groups[4].Value
            if ($null -eq $o.firstMs) { $o.firstMs = $ms }
            $o.lastMs = $ms
        } else { continue }

        if ($line.Contains("SCENARIO WNPC ")) {
            $w = [regex]::Match($line, $reWnpc)
            if ($w.Success) {
                $rec = [pscustomobject]@{
                    t = $ms; hand = $w.Groups[1].Value
                    x = [double]::Parse($w.Groups[2].Value, $INV)
                    y = [double]::Parse($w.Groups[3].Value, $INV)
                    z = [double]::Parse($w.Groups[4].Value, $INV)
                    cls = $w.Groups[5].Value; name = $w.Groups[6].Value }
                if ($rec.cls -eq "pc") { [void]$o.pcRows.Add($rec) } else { [void]$o.rows.Add($rec) }
            }
            continue
        }
        if ($line.Contains("[census] ")) {
            $s = [regex]::Match($line, $reSent)
            if ($s.Success) {
                [void]$o.censusSent.Add([pscustomobject]@{
                    t = $ms; n = [int]$s.Groups[1].Value; radius = [int]$s.Groups[2].Value
                    anchors = [int]$s.Groups[5].Value; anchorDetail = $s.Groups[6].Value.Trim()
                    enum = [int]$s.Groups[7].Value; notmine = [int]$s.Groups[8].Value
                    proxyrow = [int]$s.Groups[9].Value; attnR = [int]$s.Groups[10].Value })
                continue
            }
            $r = [regex]::Match($line, $reRecv)
            if ($r.Success) {
                [void]$o.censusRecv.Add([pscustomobject]@{
                    t = $ms; owner = [int]$r.Groups[1].Value; n = [int]$r.Groups[2].Value
                    kept = [int]$r.Groups[3].Value; offcell = [int]$r.Groups[4].Value
                    culls = [int]$r.Groups[5].Value; owners = [int]$r.Groups[6].Value })
                continue
            }
            if ($line.Contains("[census] STALE") -or $line.Contains("[census] fresh again")) {
                $kind = if ($line.Contains("STALE")) { "STALE" } else { "fresh-again" }
                [void]$o.staleEdges.Add([pscustomobject]@{ t = $ms; kind = $kind })
            }
            continue
        }
        if ($line.Contains("[net] peer connected")) {
            $c = [regex]::Match($line, $reConn)
            if ($c.Success) {
                [void]$o.peerConnects.Add([pscustomobject]@{
                    t = $ms; id = [int]$c.Groups[1].Value; player = [int]$c.Groups[2].Value })
            }
            continue
        }
        if ($line.Contains("[net] connecting")) { if ($null -eq $o.connectingMs) { $o.connectingMs = $ms }; continue }
        if ($line.Contains("[save] LOCAL-SAVE")) {
            if ([regex]::IsMatch($line, $reSave)) { $pendingSaveMs = $ms }
            continue
        }
        if ($line.Contains("[save] QUIESCED")) {
            $q = [regex]::Match($line, $reQuiesce)
            if ($q.Success) {
                [void]$o.saveWindows.Add([pscustomobject]@{
                    saveMs = $pendingSaveMs; quiesceMs = $ms
                    files = [int]$q.Groups[3].Value; bytes = [long]$q.Groups[4].Value
                    waitMs = [int]$q.Groups[5].Value })
                $pendingSaveMs = $null
            }
            continue
        }
        if ($line.Contains("[load] GO ")) {
            $g = [regex]::Match($line, $reLoadGo)
            if ($g.Success -and $null -eq $o.loadGo) {
                $o.loadGo = [pscustomobject]@{ t = $ms; id = [int]$g.Groups[1].Value; fp = $g.Groups[3].Value }
            }
            continue
        }
        if ($line.Contains("KenshiCoop: gameplay started")) { if ($null -eq $o.gameplayMs) { $o.gameplayMs = $ms }; continue }
        if ($line.Contains("SCENARIO MAGATE start")) {
            $g2 = [regex]::Match($line, $reMagate)
            if ($g2.Success -and $null -eq $o.magateMs) { $o.magateMs = $ms; $o.ownRank = [int]$g2.Groups[1].Value }
            continue
        }
    }
    return $o
}

# Cluster a flat row list into per-dump buckets on a timestamp gap (the
# oracle's Get-Dumps, same $GapMs semantics).
function Get-DumpBuckets($rows, [int]$GapMsLocal) {
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

# Whole-run PC anchor set, deduped to a 50 u grid - the SAME anchor definition
# analyze_wnpc_diff4.ps1's Get-PcAnchors uses for its horizon-band exemption, so
# the distances printed here are directly comparable to the d_min values the
# oracle reports. A contemporaneous (per-dump) distance would be a different
# measure and would silently disagree with the oracle's own numbers.
function Get-PcAnchorGrid($pcRows) {
    $seen = @{}; $anchors = New-Object System.Collections.ArrayList
    foreach ($r in $pcRows) {
        $key = "{0}:{1}" -f [Math]::Round($r.x / 50), [Math]::Round($r.z / 50)
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        [void]$anchors.Add([pscustomobject]@{ x = $r.x; z = $r.z })
    }
    return ,$anchors
}

function Get-MinDist([double]$x, [double]$z, $anchorRows) {
    $min = [double]::MaxValue
    foreach ($a in $anchorRows) {
        $dx = $x - $a.x; $dz = $z - $a.z
        $d = [Math]::Sqrt(($dx * $dx) + ($dz * $dz))
        if ($d -lt $min) { $min = $d }
    }
    if ($min -eq [double]::MaxValue) { return -1.0 }
    return $min
}

# ---- Parse every log ----------------------------------------------------------
$inst = [ordered]@{}
foreach ($l in $labels) { $inst[$l] = Read-InstanceLog $logPath[$l] }

Write-Host ("=== CENSUS RACE ANALYSIS ===")
Write-Host ("runDir: " + $RunDir)
Write-Host ("instances: " + $resolvedN + " (" + ($labels -join ", ") + ")")
Write-Host ""

# ---- 1. ARRIVAL TIMELINE ------------------------------------------------------
Write-Host "-- 1. ARRIVAL TIMELINE --"
Write-Host ("{0,-7} {1,-13} {2,-13} {3,-13} {4,-13} {5,-9} {6}" -f `
            "inst", "logStart", "connecting", "gameplay", "magateStart", "ownRank", "loadFp")
foreach ($l in $labels) {
    $o = $inst[$l]
    Write-Host ("{0,-7} {1,-13} {2,-13} {3,-13} {4,-13} {5,-9} {6}" -f `
        $l, (Format-Ms $o.firstMs), (Format-Ms $o.connectingMs), (Format-Ms $o.gameplayMs),
        (Format-Ms $o.magateMs),
        $(if ($null -eq $o.ownRank) { "-" } else { [string]$o.ownRank }),
        $(if ($null -eq $o.loadGo) { "-" } else { $o.loadGo.fp }))
}
Write-Host ""

$h = $inst["host"]
Write-Host "host peer-connected events:"
if ($h.peerConnects.Count -eq 0) { Write-Host "  (none logged)" }
foreach ($pc in $h.peerConnects) {
    $sinceGameplay = if ($null -ne $h.gameplayMs) { $pc.t - $h.gameplayMs } else { $null }
    $sinceMagate   = if ($null -ne $h.magateMs)   { $pc.t - $h.magateMs }   else { $null }
    $afterMagate = if ($null -ne $sinceMagate -and $sinceMagate -ge 0) { "AFTER host gameplay-start marker" } else { "before host gameplay-start marker" }
    Write-Host ("  id={0} player={1} at {2}  (+{3} ms after host gameplay; {4} ms vs host MAGATE start -> {5})" -f `
        $pc.id, $pc.player, (Format-Ms $pc.t),
        $(if ($null -eq $sinceGameplay) { "?" } else { [string]$sinceGameplay }),
        $(if ($null -eq $sinceMagate) { "?" } else { [string]$sinceMagate }),
        $afterMagate)
}
Write-Host ""
Write-Host "host save-push windows (LOCAL-SAVE -> QUIESCED, one per connect):"
if ($h.saveWindows.Count -eq 0) { Write-Host "  (none logged)" }
$wi = 0
foreach ($sw in $h.saveWindows) {
    $wi++
    $span = if ($null -ne $sw.saveMs) { $sw.quiesceMs - $sw.saveMs } else { $null }
    Write-Host ("  [{0}] save={1} quiesced={2} span={3} ms files={4} bytes={5}" -f `
        $wi, (Format-Ms $sw.saveMs), (Format-Ms $sw.quiesceMs),
        $(if ($null -eq $span) { "?" } else { [string]$span }), $sw.files, $sw.bytes)
}
Write-Host ""

# join arrival delta - the "simultaneous join" variable this phase is about.
$joinLabels = @($labels | Where-Object { $_ -ne "host" })
$arrivals = @()
foreach ($jl in $joinLabels) {
    $a = $inst[$jl].connectingMs
    if ($null -eq $a) { $a = $inst[$jl].firstMs }
    $arrivals += [pscustomobject]@{ label = $jl; t = $a }
}
$sortedArr = @($arrivals | Where-Object { $null -ne $_.t } | Sort-Object t)
$joinDeltaMs = -1
if ($sortedArr.Count -ge 2) {
    $joinDeltaMs = $sortedArr[$sortedArr.Count - 1].t - $sortedArr[0].t
    Write-Host ("join arrival order: " + (($sortedArr | ForEach-Object { $_.label + "@" + (Format-Ms $_.t) }) -join " -> "))
    Write-Host ("join arrival delta (first to last connect): " + $joinDeltaMs + " ms")
} else {
    Write-Host "join arrival delta: n/a (fewer than 2 joins)"
}
# Distinct save fingerprints each join actually loaded: identical fingerprints
# mean the joins started from the SAME world snapshot, distinct ones mean they
# did not, which is a property of the connect schedule, not of the network.
$fps = @($joinLabels | ForEach-Object { if ($null -ne $inst[$_].loadGo) { $inst[$_].loadGo.fp } } | Where-Object { $_ })
$distinctFp = @($fps | Sort-Object -Unique).Count
Write-Host ("join save fingerprints: " + ($fps -join ", ") + "  (distinct=" + $distinctFp + " of " + $fps.Count + ")")
Write-Host ""

# ---- 2. CENSUS CADENCE --------------------------------------------------------
Write-Host "-- 2. CENSUS CADENCE --"
foreach ($l in $labels) {
    $o = $inst[$l]
    $sent = $o.censusSent; $recv = $o.censusRecv
    $sentSpan = if ($sent.Count -ge 2) { $sent[$sent.Count - 1].t - $sent[0].t } else { 0 }
    $recvSpan = if ($recv.Count -ge 2) { $recv[$recv.Count - 1].t - $recv[0].t } else { 0 }
    Write-Host ("{0,-7} sent={1,-3} span={2,-7} recv={3,-3} span={4,-7} staleEdges={5}" -f `
        $l, $sent.Count, $sentSpan, $recv.Count, $recvSpan, $o.staleEdges.Count)
    if ($sent.Count -gt 0) {
        $nMin = ($sent | Measure-Object -Property n -Minimum).Minimum
        $nMax = ($sent | Measure-Object -Property n -Maximum).Maximum
        $eMax = ($sent | Measure-Object -Property enum -Maximum).Maximum
        $nmMax = ($sent | Measure-Object -Property notmine -Maximum).Maximum
        $anchorsMax = ($sent | Measure-Object -Property anchors -Maximum).Maximum
        Write-Host ("        published rows n=[{0}..{1}] enumMax={2} notmineMax={3} anchorsMax={4}" -f `
            $nMin, $nMax, $eMax, $nmMax, $anchorsMax)
        Write-Host ("        first={0} last={1}" -f (Format-Ms $sent[0].t), (Format-Ms $sent[$sent.Count - 1].t))
    }
    if ($recv.Count -gt 0) {
        $byOwner = $recv | Group-Object owner
        foreach ($g in $byOwner) {
            $kMin = ($g.Group | Measure-Object -Property kept -Minimum).Minimum
            $kMax = ($g.Group | Measure-Object -Property kept -Maximum).Maximum
            $cMax = ($g.Group | Measure-Object -Property culls -Maximum).Maximum
            Write-Host ("        recv owner={0} samples={1} kept=[{2}..{3}] cullsMax={4} first={5} last={6}" -f `
                $g.Name, $g.Count, $kMin, $kMax, $cMax,
                (Format-Ms ($g.Group | Select-Object -First 1).t),
                (Format-Ms ($g.Group | Select-Object -Last 1).t))
        }
    }
    foreach ($se in $o.staleEdges) { Write-Host ("        stale edge {0} at {1}" -f $se.kind, (Format-Ms $se.t)) }
}
Write-Host ""

# ---- 3. DIVERGENCE DESCRIPTION ------------------------------------------------
Write-Host "-- 3. DIVERGENCE DESCRIPTION (per host-join pair) --"
# Radii this run actually used, read off the host's own census lines rather
# than assumed (censusR is the publish radius before the 1.25 publish margin).
$censusR = 2000.0; $attnR = 1000.0
if ($h.censusSent.Count -gt 0) {
    $censusR = [double]$h.censusSent[0].radius
    $attnR   = [double]$h.censusSent[0].attnR
}
$edgeBand  = $censusR * 0.8    # analyze_wnpc_diff4.ps1's horizon-band edge
$censusPub = $censusR * 1.25   # publishNpcCensus' actual enumeration radius

$hostDumps = Get-DumpBuckets $h.rows $GapMs
$hAnchorGrid = Get-PcAnchorGrid $h.pcRows
$divergedPairs = New-Object System.Collections.ArrayList
$totalJoinOnlyKeys = 0
$totalHostOnlyKeys = 0

foreach ($jl in $joinLabels) {
    $j = $inst[$jl]
    $joinDumps = Get-DumpBuckets $j.rows $GapMs
    $jAnchorGrid = Get-PcAnchorGrid $j.pcRows

    # Same nearest-neighbour dump pairing the oracle uses, bounded by each
    # side's own last timestamped line (true process end).
    $hEligible = @($hostDumps | Where-Object { $null -eq $h.lastMs -or $_.t -lt $h.lastMs })
    $jEligible = @($joinDumps | Where-Object { $null -eq $j.lastMs -or $_.t -lt $j.lastMs })
    $pairedSeq = New-Object System.Collections.ArrayList
    foreach ($hd in $hEligible) {
        $jd = $jEligible | Sort-Object { [Math]::Abs($_.t - $hd.t) } | Select-Object -First 1
        if ($null -eq $jd -or [Math]::Abs($jd.t - $hd.t) -gt $PairTolMs) { continue }
        [void]$pairedSeq.Add([pscustomobject]@{ t = $hd.t; hd = $hd; jd = $jd })
    }

    Write-Host ("== host vs {0} ==" -f $jl)
    Write-Host ("  paired dumps: {0}  (host dumps={1}, {2} dumps={3})" -f $pairedSeq.Count, $hostDumps.Count, $jl, $joinDumps.Count)
    if ($pairedSeq.Count -eq 0) { Write-Host "  (no comparable dumps)"; Write-Host ""; continue }

    # Per-key observation record across the paired sequence.
    $keyRec = @{}
    foreach ($p in $pairedSeq) {
        $hSet = @{}; foreach ($r in $p.hd.rows) { $hSet[$r.hand] = $r }
        $jSet = @{}; foreach ($r in $p.jd.rows) { $jSet[$r.hand] = $r }
        $allKeys = @($hSet.Keys) + @($jSet.Keys) | Sort-Object -Unique
        foreach ($k in $allKeys) {
            $onH = $hSet.ContainsKey($k); $onJ = $jSet.ContainsKey($k)
            # Mirror analyze_wnpc_diff4.ps1's own one-sided rule EXACTLY: a
            # cls=hid row is the observing side's suppression machinery AGREEING
            # with the other side's absence, so a hid-only-on-one-side key is not
            # one-sided presence at all and the oracle does not track it. A hid
            # row present on BOTH sides still counts as a match there, and does
            # here. Diverging from this would make the two outputs incomparable.
            $status = $null
            if ($onH -and $onJ) { $status = "match" }
            elseif ($onJ -and "$($jSet[$k].cls)" -ne "hid") { $status = "join_only" }
            elseif ($onH -and "$($hSet[$k].cls)" -ne "hid") { $status = "host_only" }
            if ($null -eq $status) { continue }
            if (-not $keyRec.ContainsKey($k)) {
                $keyRec[$k] = [pscustomobject]@{
                    name = ""; nMatch = 0; nJoinOnly = 0; nHostOnly = 0
                    firstHostT = $null; lastHostT = $null; firstJoinT = $null; lastJoinT = $null
                    lastStatus = ""; lastT = 0; clsSeen = (New-Object System.Collections.ArrayList)
                    minDistOwn = [double]::MaxValue }
            }
            $rec = $keyRec[$k]
            if ($onH) {
                if ($null -eq $rec.firstHostT) { $rec.firstHostT = $p.hd.t }
                $rec.lastHostT = $p.hd.t
                if ($rec.name -eq "") { $rec.name = $hSet[$k].name }
            }
            if ($onJ) {
                if ($null -eq $rec.firstJoinT) { $rec.firstJoinT = $p.jd.t }
                $rec.lastJoinT = $p.jd.t
                if ($rec.name -eq "") { $rec.name = $jSet[$k].name }
                $c = $jSet[$k].cls
                if (-not $rec.clsSeen.Contains($c)) { [void]$rec.clsSeen.Add($c) }
            }
            switch ($status) {
                "match"     { $rec.nMatch++ }
                "join_only" { $rec.nJoinOnly++ }
                "host_only" { $rec.nHostOnly++ }
            }
            $rec.lastStatus = $status; $rec.lastT = $p.t

            # Distance from the OBSERVING side's own squad: a one-sided body's
            # distance is what decides whether either side was ever entitled to
            # speak for it (attention radius on the plugin side, edge band on
            # the oracle side), so it is printed rather than assumed.
            if ($status -ne "match") {
                $src = if ($onJ) { $jSet[$k] } else { $hSet[$k] }
                $anchorGrid = if ($onJ) { $jAnchorGrid } else { $hAnchorGrid }
                $d = Get-MinDist $src.x $src.z $anchorGrid
                if ($d -ge 0 -and $d -lt $rec.minDistOwn) { $rec.minDistOwn = $d }
            }
        }
    }

    $finalT = $pairedSeq[$pairedSeq.Count - 1].t
    $oneSidedAtEnd = @($keyRec.Keys | Where-Object { $keyRec[$_].lastT -eq $finalT -and $keyRec[$_].lastStatus -ne "match" })
    $joinOnlyKeys = @($keyRec.Keys | Where-Object { $keyRec[$_].nJoinOnly -gt 0 })
    $hostOnlyKeys = @($keyRec.Keys | Where-Object { $keyRec[$_].nHostOnly -gt 0 })
    $joinOnlyRows = ($keyRec.Keys | ForEach-Object { $keyRec[$_].nJoinOnly } | Measure-Object -Sum).Sum
    $hostOnlyRows = ($keyRec.Keys | ForEach-Object { $keyRec[$_].nHostOnly } | Measure-Object -Sum).Sum
    $totalJoinOnlyKeys += $joinOnlyKeys.Count
    $totalHostOnlyKeys += $hostOnlyKeys.Count
    if ($oneSidedAtEnd.Count -gt 0) { [void]$divergedPairs.Add($jl) }

    Write-Host ("  joinOnly rows={0} over {1} key(s); hostOnly rows={2} over {3} key(s)" -f `
        $joinOnlyRows, $joinOnlyKeys.Count, $hostOnlyRows, $hostOnlyKeys.Count)
    Write-Host ("  final comparable paired dump t={0}; one-sided keys still there: {1}" -f (Format-Ms $finalT), $oneSidedAtEnd.Count)

    # Regression description: agreed at least once, then stopped agreeing.
    $regressed = @($keyRec.Keys | Where-Object { $keyRec[$_].nMatch -gt 0 -and $keyRec[$_].lastStatus -ne "match" })
    $neverAgreed = @($keyRec.Keys | Where-Object { $keyRec[$_].nMatch -eq 0 -and ($keyRec[$_].nJoinOnly + $keyRec[$_].nHostOnly) -gt 0 })
    Write-Host ("  one-sided keys that NEVER agreed: {0}; keys that agreed then regressed: {1}" -f $neverAgreed.Count, $regressed.Count)

    # The one-sided authority-class histogram on the join side. This is the
    # field that says WHICH join-side rule left the body standing.
    $clsHist = @{}
    foreach ($k in $joinOnlyKeys) {
        foreach ($c in $keyRec[$k].clsSeen) {
            if (-not $clsHist.ContainsKey($c)) { $clsHist[$c] = 0 }
            $clsHist[$c]++
        }
    }
    if ($clsHist.Keys.Count -gt 0) {
        $parts = @($clsHist.Keys | Sort-Object | ForEach-Object { "$_=" + $clsHist[$_] })
        Write-Host ("  join-side authority classes over joinOnly keys: " + ($parts -join " "))
    }

    # Distance bands. These three radii are not decoration - they are the three
    # different reaches the two sides actually use, and a one-sided key's band
    # says which rule left it standing:
    #   <= attnR            the join's attention gate covers it (it gets culled)
    #   attnR .. edgeBand   attention does not cover it, the oracle still judges it
    #   > edgeBand          the oracle's horizon-band exemption applies
    # censusPub is how far the host's existence claim actually reaches.
    $bandLine = "  distance bands (min distance to the OBSERVING side's own deduped PC anchors, oracle semantics):"
    Write-Host $bandLine
    Write-Host ("    attnR={0}u (join attention gate)  edgeBand={1}u (oracle horizon exemption)  censusPub={2}u (host existence claim)" -f `
        (Format-Num $attnR 0), (Format-Num $edgeBand 0), (Format-Num $censusPub 0))
    $bIn = 0; $bMid = 0; $bOut = 0; $bUnk = 0
    foreach ($k in $oneSidedAtEnd) {
        $d = $keyRec[$k].minDistOwn
        if ($d -eq [double]::MaxValue) { $bUnk++ }
        elseif ($d -le $attnR) { $bIn++ }
        elseif ($d -le $edgeBand) { $bMid++ }
        else { $bOut++ }
    }
    Write-Host ("    one-sided-at-end keys by band: <=attnR={0}  attnR..edgeBand={1}  >edgeBand={2}  unknown={3}" -f `
        $bIn, $bMid, $bOut, $bUnk)

    # The SHAPE the oracle actually judges, reconstructed from its own three
    # documented exemptions rather than from a second opinion about the verdict:
    #   - a key whose one-sided rows were ONLY ever cls=drv is inside the
    #     driven-extrapolation age-out exemption;
    #   - a key that NEVER agreed and sits wholly beyond edgeBand is inside the
    #     horizon-band exemption;
    #   - everything else that is still one-sided at the final comparable dump
    #     is what the oracle reports as a rule (a)/(b)/(c) divergence.
    # Printing the shape, not a verdict: analyze_wnpc_diff4.ps1 stays the judge.
    $judged = New-Object System.Collections.ArrayList
    foreach ($k in $oneSidedAtEnd) {
        $rec = $keyRec[$k]
        $drvOnly = ($rec.clsSeen.Count -gt 0) -and -not (@($rec.clsSeen | Where-Object { $_ -ne "drv" }).Count -gt 0)
        if ($drvOnly) { continue }
        $beyondBand = ($rec.minDistOwn -ne [double]::MaxValue) -and ($rec.minDistOwn -gt $edgeBand)
        if ($rec.nMatch -eq 0 -and $beyondBand) { continue }
        [void]$judged.Add($k)
    }
    Write-Host ("    one-sided-at-end keys OUTSIDE both oracle exemptions (drv-only age-out / never-paired horizon band): {0}" -f $judged.Count)
    foreach ($k in $judged) {
        $rec = $keyRec[$k]
        $dTxt = if ($rec.minDistOwn -eq [double]::MaxValue) { "?" } else { (Format-Num $rec.minDistOwn 0) + "u" }
        $shape = if ($rec.nMatch -gt 0) { "agreed-then-regressed" } else { "never-agreed" }
        Write-Host ("      {0} name='{1}' cls=[{2}] d={3} match={4} -> {5}" -f `
            $k, $rec.name, ($rec.clsSeen -join ","), $dTxt, $rec.nMatch, $shape)
    }

    $named = @($oneSidedAtEnd)
    if ($named.Count -eq 0) { $named = @($neverAgreed) }
    $named = @($named | Sort-Object { -($keyRec[$_].nJoinOnly + $keyRec[$_].nHostOnly) } | Select-Object -First $TopN)
    if ($named.Count -gt 0) {
        Write-Host "  one-sided keys (most-persistent first):"
        foreach ($k in $named) {
            $rec = $keyRec[$k]
            $dTxt = if ($rec.minDistOwn -eq [double]::MaxValue) { "?" } else { (Format-Num $rec.minDistOwn 0) + "u" }
            Write-Host ("    {0,-34} name='{1}' side={2} cls=[{3}] joinOnly={4} hostOnly={5} match={6} hostSeen=[{7}..{8}] joinSeen=[{9}..{10}] dOwnSquadMin={11}" -f `
                $k, $rec.name, $(if ($rec.nJoinOnly -ge $rec.nHostOnly) { "join" } else { "host" }),
                ($rec.clsSeen -join ","), $rec.nJoinOnly, $rec.nHostOnly, $rec.nMatch,
                (Format-Ms $rec.firstHostT), (Format-Ms $rec.lastHostT),
                (Format-Ms $rec.firstJoinT), (Format-Ms $rec.lastJoinT), $dTxt)
        }
    }
    Write-Host ""
}

# ---- 4. SUMMARY ---------------------------------------------------------------
$dpTxt = if ($divergedPairs.Count -eq 0) { "none" } else { ($divergedPairs -join ",") }
Write-Host ("CENSUS-RACE SUMMARY runDir={0} joinDeltaMs={1} divergedPairs={2} keysJoinOnly={3} keysHostOnly={4}" -f `
            $RunDir, $joinDeltaMs, $dpTxt, $totalJoinOnlyKeys, $totalHostOnlyKeys)
