<#
.SYNOPSIS
  Offline judge for a connect_relink run: did the NetLink session roster
  actually RESET across the reconnect? (Phase 14 Plan 02, UI-07, ROADMAP
  criterion 2; 14-CONTEXT decisions D-04 and D-06.)

.DESCRIPTION
  Reads an ALREADY-ARCHIVED run directory and nothing else. It never launches
  a game, never writes into the run dir, and never needs a rig - so it can be
  pointed at a synthetic fixture directory, which is exactly what
  scripts\tests\RelinkLever.Tests.ps1 does to prove this judge can FAIL.

  Like scripts\analyze_census_race.ps1 (and unlike the registered gate
  oracles), this is NOT a registered oracle: it is deliberately absent from
  CoopOracles.psm1's registry and from Get-OracleBoundSpec, so adding it
  changes no existing gate verdict anywhere.

  WHAT IT JUDGES. Commit 9011527 clears the host join-slot roster at every
  NetLink session boundary, because g_net is a REUSED singleton: a relink is
  stop() + startHost() on the SAME object, and threadLoop() ends with
  enet_host_destroy(), which frees the whole ENetPeer array. Pre-fix, the
  surviving slot table made the lowest-free-slot scan read stale ids as
  OCCUPIED - rejoins would climb to higher ids and eventually be refused with
  a truthful-looking "MAX_PLAYERS=4 slots full". The discriminating evidence
  is therefore not "the run was green": it is the boundary line naming WHICH
  slots were discarded, followed by the SAME ids being admitted again.

  Checks (host log unless stated):
    a  every "SCENARIO relink issued" is followed by a "[coop-ui] connect:"
       line in the shape the panel_config oracle accepts - the panel handler
       really ran, rather than the adapter merely being called.
    b  every "[coop-ui] RELINK src=scenario tid=N" equals that instance's
       "[coop-ui] MAINTID tid=N" - the relink was issued on the GAME thread
       (D-04). A mismatch is a FAIL, not a warning.
    c  each host relink carries a "[net] roster-reset where=stop role=host"
       boundary, and at least one of them has slots >= 1 (a reset of an
       already-empty roster is not the observation). When an instance issued a
       JOIN-side relink, that instance's log must carry the matching
       role=client stop/launch boundary pair.
    d  the ids admitted DURING A BOUNDARY'S OWN RECOVERY equal the ids
       discarded at that boundary. This is the sharp discriminator described
       above. The window is closed by the relinking side's own
       "SCENARIO relink post" line, not by the next boundary: a client relink
       landing later in the same span admits ids that have nothing to do with
       this boundary, and folding those in would blame the boundary for
       somebody else's reconnect.
    e  zero "slots full" rejections anywhere in the host log.
    h  the host never holds MORE live slots than there are clients. A client
       that tears its own transport down (stop() -> enet_host_destroy()) sends
       no clean ENet disconnect, so the host keeps the old peer until ENet
       times it out - meanwhile the reconnect takes the NEXT free id and the
       host counts one client twice. That leak is what eventually produces a
       real "MAX_PLAYERS slots full", so it is judged here explicitly instead
       of being left to surface as a confusing failure of check d.
    f  every OBSERVING instance witnessed a roster drop AND return; every
       RELINKING instance recovered its pre-relink id set (ok=1).
    g  "SCENARIO RESULT PASS" in every instance log.

  EXIT CODES - note these are the OPPOSITE convention to
  scripts\repro_census_n3.ps1, where the verdict is DATA and the exit code
  only reports whether the script ran. Here the exit code IS the verdict,
  because this script is invoked as a gate by relink_probe.ps1 and by the
  headless guard:
    0  RELINK RESULT: PASS
    1  RELINK RESULT: FAIL  (the run was judged and did not meet the checks)
    2  the script could not run at all (no run dir, no pointer, unreadable or
       missing logs) - a hard-requirement failure, NOT a gate FAIL.

  All numeric parsing/formatting goes through InvariantCulture: the dev
  machine's locale is uk-UA and a decimal-comma reparse of a log timestamp or
  an id list is a silent wrong answer.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze_relink.ps1

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze_relink.ps1 -RunDir tools\test-runs\20260912_101500_N3_relink
#>
[CmdletBinding()]
param(
    [string]$RunDir = "",
    [int]$ExpectedInstances = 3,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$INV = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

function Die2 { param([string]$Msg) Write-Host "RELINK ERROR: $Msg"; exit 2 }
function ToInt { param([string]$S) return [int]::Parse($S, $INV) }
function IdSet {
    param([string]$Csv)
    $s = @()
    if ($null -ne $Csv -and $Csv.Trim() -ne "") {
        foreach ($p in $Csv.Split(',')) { if ($p.Trim() -ne "") { $s += (ToInt $p.Trim()) } }
    }
    return ,(@($s) | Sort-Object -Unique)
}
function SetEq {
    param($A, $B)
    $a = @($A); $b = @($B)
    if ($a.Count -ne $b.Count) { return $false }
    for ($i = 0; $i -lt $a.Count; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}
function CsvOf { param($S) if (@($S).Count -eq 0) { return "" } else { return (@($S) -join ',') } }

# ---- resolve the run dir -------------------------------------------------------
if ($RunDir -eq "") {
    $pointer = Join-Path $repoRoot "tools\test-runs\last_relink.txt"
    if (-not (Test-Path $pointer)) {
        Die2 "no -RunDir given and no pointer file at '$pointer' (run scripts\relink_probe.ps1 first)."
    }
    $RunDir = (Get-Content -Raw -Path $pointer).Trim()
}
if ($RunDir -eq "" -or -not (Test-Path $RunDir)) { Die2 "run dir not found: '$RunDir'" }
$RunDir = (Resolve-Path -LiteralPath $RunDir).Path

# ---- collect instance logs -----------------------------------------------------
$logFiles = @(Get-ChildItem -Path $RunDir -Filter '*.log' -File |
              Where-Object { $_.Name -notmatch 'engine' } |
              Where-Object { $_.Name -eq 'host.log' -or $_.Name -like 'join*.log' } |
              Sort-Object Name)
if ($logFiles.Count -lt $ExpectedInstances) {
    Die2 ("run dir '{0}' has {1} instance log(s); expected {2}." -f $RunDir, $logFiles.Count, $ExpectedInstances)
}
$hostLog = @($logFiles | Where-Object { $_.Name -eq 'host.log' }) | Select-Object -First 1
if ($null -eq $hostLog) { Die2 "no host.log in '$RunDir'" }

# ---- parse ---------------------------------------------------------------------
$RX = @{
    Stamp    = '^\[(\d\d:\d\d:\d\d\.\d{3})\]'
    Issued   = 'SCENARIO relink issued t=(\d+) n=(\d+)'
    Relink   = '\[coop-ui\] RELINK src=scenario tid=(\d+)'
    MainTid  = '\[coop-ui\] MAINTID tid=(\d+)'
    # Same shape the panel_config oracle (scripts\oracles\Panel.ps1) accepts.
    Connect  = '\[coop-ui\] connect: role=(HOST|JOIN) transport=(steam|udp) peer=(\d+) ownRanks=\{([\d,]*)\} src=(env|role)'
    Roster   = '\[net\] roster-reset where=(launch|stop) role=(host|client) slots=(\d+) ids=\{([\d,]*)\} epochs=(\d+)'
    PeerConn = '\[net\] peer connected id=(\d+) player=(\d+)'
    PeerDisc = '\[net\] peer disconnected id=(\d+)'
    Full     = 'slots full'
    Start    = 'SCENARIO relink start host=(\d+) localId=(\d+) side=(relink|observe) role=(\w+)'
    Post     = 'SCENARIO relink post t=(\d+) peers=(\d+) ids=\{([\d,]*)\} recoveredMs=(\d+) ok=(\d+)'
    Observed = 'SCENARIO relink observed t=(\d+) drop=(\d+) cycles=(\d+)/(\d+) peak=(\d+)'
    Sample   = 'SCENARIO relink (peer|prearm) t=(\d+) peers=(\d+) ids=\{([\d,]*)\}'
    Result   = 'SCENARIO RESULT (PASS|FAIL)'
}

function Parse-InstanceLog {
    param([System.IO.FileInfo]$File)
    $inst = [ordered]@{
        name = $File.Name; path = $File.FullName
        events = @(); mainTids = @(); relinkTids = @()
        issuedIdx = @(); connectIdx = @()
        boundaries = @(); admits = @(); departs = @(); slotsFull = 0
        side = ""; result = ""; posts = @(); postIdx = @(); observed = $null; samples = @()
    }
    $i = 0
    foreach ($line in [System.IO.File]::ReadLines($File.FullName)) {
        $i++
        $ts = ""
        $m = [regex]::Match($line, $RX.Stamp)
        if ($m.Success) { $ts = $m.Groups[1].Value }
        $add = { param($kind, $text) $inst.events += [pscustomobject]@{ idx = $i; ts = $ts; kind = $kind; text = $text } }

        $m = [regex]::Match($line, $RX.Start)
        if ($m.Success) {
            $inst.side = $m.Groups[3].Value
            & $add 'start' ("side={0} role={1} localId={2}" -f $m.Groups[3].Value, $m.Groups[4].Value, $m.Groups[2].Value)
            continue
        }
        $m = [regex]::Match($line, $RX.Issued)
        if ($m.Success) { $inst.issuedIdx += $i; & $add 'issued' ("t={0} n={1}" -f $m.Groups[1].Value, $m.Groups[2].Value); continue }
        $m = [regex]::Match($line, $RX.Relink)
        if ($m.Success) { $inst.relinkTids += (ToInt $m.Groups[1].Value); & $add 'RELINK' ("tid=" + $m.Groups[1].Value); continue }
        $m = [regex]::Match($line, $RX.MainTid)
        if ($m.Success) { $inst.mainTids += (ToInt $m.Groups[1].Value); & $add 'MAINTID' ("tid=" + $m.Groups[1].Value); continue }
        $m = [regex]::Match($line, $RX.Connect)
        if ($m.Success) { $inst.connectIdx += $i; & $add 'connect' ("role={0} transport={1} src={2}" -f $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[5].Value); continue }
        $m = [regex]::Match($line, $RX.Roster)
        if ($m.Success) {
            $b = [pscustomobject]@{
                idx = $i; ts = $ts
                where = $m.Groups[1].Value; role = $m.Groups[2].Value
                slots = (ToInt $m.Groups[3].Value); ids = (IdSet $m.Groups[4].Value)
                epochs = (ToInt $m.Groups[5].Value)
            }
            $inst.boundaries += $b
            & $add 'roster-reset' ("where={0} role={1} slots={2} ids={{{3}}} epochs={4}" -f $b.where, $b.role, $b.slots, (CsvOf $b.ids), $b.epochs)
            continue
        }
        $m = [regex]::Match($line, $RX.PeerConn)
        if ($m.Success) {
            $inst.admits += [pscustomobject]@{ idx = $i; ts = $ts; id = (ToInt $m.Groups[1].Value) }
            & $add 'peer-connected' ("id=" + $m.Groups[1].Value)
            continue
        }
        $m = [regex]::Match($line, $RX.PeerDisc)
        if ($m.Success) {
            $inst.departs += [pscustomobject]@{ idx = $i; ts = $ts; id = (ToInt $m.Groups[1].Value) }
            & $add 'peer-disconnected' ("id=" + $m.Groups[1].Value)
            continue
        }
        $m = [regex]::Match($line, $RX.Post)
        if ($m.Success) {
            $inst.postIdx += $i
            $inst.posts += [pscustomobject]@{ ok = (ToInt $m.Groups[5].Value); ids = (IdSet $m.Groups[3].Value); recoveredMs = (ToInt $m.Groups[4].Value) }
            & $add 'relink-post' ("ids={{{0}}} recoveredMs={1} ok={2}" -f $m.Groups[3].Value, $m.Groups[4].Value, $m.Groups[5].Value)
            continue
        }
        $m = [regex]::Match($line, $RX.Observed)
        if ($m.Success) {
            $inst.observed = [pscustomobject]@{ drop = (ToInt $m.Groups[2].Value); cycles = (ToInt $m.Groups[3].Value); want = (ToInt $m.Groups[4].Value) }
            & $add 'relink-observed' ("drop={0} cycles={1}/{2}" -f $m.Groups[2].Value, $m.Groups[3].Value, $m.Groups[4].Value)
            continue
        }
        $m = [regex]::Match($line, $RX.Sample)
        if ($m.Success) {
            $inst.samples += [pscustomobject]@{ peers = (ToInt $m.Groups[3].Value) }
            & $add 'roster-sample' ("{0} peers={1} ids={{{2}}}" -f $m.Groups[1].Value, $m.Groups[3].Value, $m.Groups[4].Value)
            continue
        }
        $m = [regex]::Match($line, $RX.Result)
        if ($m.Success) { $inst.result = $m.Groups[1].Value; & $add 'RESULT' $m.Groups[1].Value; continue }
        if ($line -match $RX.Full) { $inst.slotsFull++; & $add 'slots-full' $line.Trim(); continue }
    }
    return [pscustomobject]$inst
}

$instances = @()
foreach ($f in $logFiles) { $instances += (Parse-InstanceLog -File $f) }
$H = @($instances | Where-Object { $_.name -eq 'host.log' })[0]

# ---- timeline ------------------------------------------------------------------
if (-not $Quiet) {
    Write-Host "== relink timeline ($RunDir) =="
    foreach ($inst in $instances) {
        Write-Host ""
        Write-Host ("-- {0} (side={1}) --" -f $inst.name, $(if ($inst.side -eq "") { "?" } else { $inst.side }))
        if (@($inst.events).Count -eq 0) { Write-Host "   (no relink-relevant events)" }
        foreach ($e in $inst.events) { Write-Host ("   [{0}] {1,-16} {2}" -f $e.ts, $e.kind, $e.text) }
    }
    Write-Host ""
}

# ---- checks --------------------------------------------------------------------
$script:fails = @()
$notes = @()
function Judge {
    param([string]$Name, [bool]$Cond, [string]$Detail)
    if ($Cond) { Write-Host ("  ok   {0} - {1}" -f $Name, $Detail) }
    else       { Write-Host ("  FAIL {0} - {1}" -f $Name, $Detail); $script:fails += $Name }
}

# (a) every issued relink produced a real panel connect line, after it.
foreach ($inst in $instances) {
    if (@($inst.issuedIdx).Count -eq 0) { continue }
    $unmatched = 0
    foreach ($ix in $inst.issuedIdx) {
        if (@($inst.connectIdx | Where-Object { $_ -gt $ix }).Count -lt 1) { $unmatched++ }
    }
    Judge ("a/panel-handler-ran[" + $inst.name + "]") ($unmatched -eq 0) `
        ("{0} issued relink(s), {1} '[coop-ui] connect:' line(s), {2} unmatched" -f @($inst.issuedIdx).Count, @($inst.connectIdx).Count, $unmatched)
}
$totalIssued = 0
foreach ($inst in $instances) { $totalIssued += @($inst.issuedIdx).Count }
Judge "a/at-least-one-relink-issued" ($totalIssued -ge 1) ("issued={0} across all instances" -f $totalIssued)

# (b) the relink ran on the GAME thread: RELINK tid == that instance's MAINTID.
$relinkers = @($instances | Where-Object { @($_.relinkTids).Count -gt 0 })
Judge "b/relink-thread-observed" (@($relinkers).Count -ge 1) ("{0} instance(s) emitted a RELINK tid line" -f @($relinkers).Count)
foreach ($inst in $relinkers) {
    $mt = @($inst.mainTids) | Select-Object -First 1
    if ($null -eq $mt) {
        Judge ("b/game-thread-equality[" + $inst.name + "]") $false "no '[coop-ui] MAINTID tid=' line to compare against (pre-14-02 binary?)"
        continue
    }
    $bad = @($inst.relinkTids | Where-Object { $_ -ne $mt })
    Judge ("b/game-thread-equality[" + $inst.name + "]") (@($bad).Count -eq 0) `
        ("MAINTID={0}; RELINK tids={{{1}}}; mismatches={2}" -f $mt, (CsvOf $inst.relinkTids), @($bad).Count)
}

# (c) the host session boundary was observed, and it was not already empty.
$hostStops = @($H.boundaries | Where-Object { $_.where -eq 'stop' -and $_.role -eq 'host' })
$hostIssued = @($H.issuedIdx).Count
Judge "c/host-stop-boundaries" ((@($hostStops).Count -ge $hostIssued) -and (@($hostStops).Count -ge 1)) `
    ("{0} 'where=stop role=host' boundary line(s) for {1} host relink(s)" -f @($hostStops).Count, $hostIssued)
$nonEmpty = @($hostStops | Where-Object { $_.slots -ge 1 })
Judge "c/host-roster-was-not-empty" (@($nonEmpty).Count -ge 1) `
    ("{0}/{1} host boundaries discarded a NON-EMPTY roster (slots>=1)" -f @($nonEmpty).Count, @($hostStops).Count)

# (c, client half) an instance that issued a relink while running as a client must
# show the client-side boundary pair - a different session boundary (epochSeen_,
# server-peer reconnect) from the host path.
$clientRelinkers = @($instances | Where-Object { $_.name -ne 'host.log' -and @($_.issuedIdx).Count -gt 0 })
if (@($clientRelinkers).Count -gt 0) {
    foreach ($inst in $clientRelinkers) {
        $cs = @($inst.boundaries | Where-Object { $_.where -eq 'stop'   -and $_.role -eq 'client' })
        $cl = @($inst.boundaries | Where-Object { $_.where -eq 'launch' -and $_.role -eq 'client' })
        Judge ("c/client-boundary-pair[" + $inst.name + "]") ((@($cs).Count -ge 1) -and (@($cl).Count -ge 1)) `
            ("stop={0} launch={1} client-side boundaries" -f @($cs).Count, @($cl).Count)
    }
} else {
    $notes += "no client-side relink in this run - KENSHICOOP_RELINK_ROLE=join|both is UNVERIFIED by it"
    Write-Host "  note c/client-boundary-pair - no join issued a relink in this run (role=host); not asserted"
}

# (d) THE DISCRIMINATOR: the ids re-admitted after a boundary equal the ids it discarded.
for ($i = 0; $i -lt @($hostStops).Count; $i++) {
    $b = $hostStops[$i]
    # Close the window at THIS boundary's own recovery: the relinking side's
    # next "SCENARIO relink post" line, capped by the next boundary and EOF.
    # Anything admitted after that belongs to some other reconnect.
    $next = [int]::MaxValue
    if ($i + 1 -lt @($hostStops).Count) { $next = $hostStops[$i + 1].idx }
    $post = @($H.postIdx | Where-Object { $_ -gt $b.idx } | Sort-Object | Select-Object -First 1)
    if (@($post).Count -ge 1 -and $post[0] -lt $next) { $next = $post[0] }
    if ($b.slots -eq 0) {
        Write-Host ("  note d/readmit[boundary {0}] - discarded nothing; nothing to re-admit" -f ($i + 1))
        continue
    }
    $after = @($H.admits | Where-Object { $_.idx -gt $b.idx -and $_.idx -lt $next } | ForEach-Object { $_.id })
    $A = @(@($after) | Sort-Object -Unique)
    Judge ("d/readmit-same-ids[boundary " + ($i + 1) + "]") (SetEq $A $b.ids) `
        ("discarded={{{0}}} re-admitted={{{1}}} (pre-fix these would CLIMB, then exhaust MAX_PLAYERS)" -f (CsvOf $b.ids), (CsvOf $A))
}

# (e) no rejection anywhere on the host.
Judge "e/no-slots-full-rejection" ($H.slotsFull -eq 0) ("{0} 'slots full' line(s) in host.log" -f $H.slotsFull)

# (h) slot-leak detector. Replay the host's registry from its own log - a
# roster-reset clears it, a connect adds, a disconnect removes - and assert it
# never holds more live slots than there are clients. A client relink tears the
# transport down WITHOUT enet_peer_disconnect, so the host keeps the old peer
# until ENet times it out while the reconnect already took the next free id;
# for that window the host counts one client twice, and repeated relinks walk
# the table up to a genuine "MAX_PLAYERS slots full".
$slotCap  = [Math]::Max(1, $ExpectedInstances - 1)
$live     = @{}
$peak     = 0
$peakAt   = ""
$peakIds  = ""
$replay   = @()
foreach ($b in $H.boundaries) { $replay += [pscustomobject]@{ idx = $b.idx; ts = $b.ts; kind = 'reset'; id = 0 } }
foreach ($a in $H.admits)     { $replay += [pscustomobject]@{ idx = $a.idx; ts = $a.ts; kind = 'add';   id = $a.id } }
foreach ($x in $H.departs)    { $replay += [pscustomobject]@{ idx = $x.idx; ts = $x.ts; kind = 'del';   id = $x.id } }
foreach ($e in (@($replay) | Sort-Object idx)) {
    if     ($e.kind -eq 'reset') { $live = @{} }
    elseif ($e.kind -eq 'add')   { $live[$e.id] = $true }
    elseif ($e.kind -eq 'del')   { $live.Remove($e.id) | Out-Null }
    if ($live.Count -gt $peak) {
        $peak    = $live.Count
        $peakAt  = $e.ts
        $peakIds = CsvOf (@($live.Keys) | Sort-Object)
    }
}
Judge "h/no-phantom-double-slot" ($peak -le $slotCap) `
    ("host held at most {0} live slot(s) (cap {1} for {2} instances); peak ids={{{3}}} at [{4}]" -f $peak, $slotCap, $ExpectedInstances, $peakIds, $peakAt)

# (f) each instance produced the evidence its own side is responsible for.
foreach ($inst in $instances) {
    if ($inst.side -eq 'relink') {
        $ok = @($inst.posts | Where-Object { $_.ok -eq 1 })
        Judge ("f/relinker-recovered[" + $inst.name + "]") ((@($inst.posts).Count -ge 1) -and (@($ok).Count -eq @($inst.posts).Count)) `
            ("{0}/{1} relink post line(s) with ok=1" -f @($ok).Count, @($inst.posts).Count)
    } elseif ($inst.side -eq 'observe') {
        $o = $inst.observed
        $detail = "no 'SCENARIO relink observed' line"
        if ($null -ne $o) { $detail = ("drop={0} cycles={1}/{2}" -f $o.drop, $o.cycles, $o.want) }
        Judge ("f/observer-saw-drop-and-return[" + $inst.name + "]") (($null -ne $o) -and ($o.drop -eq 1) -and ($o.cycles -ge 1)) $detail
    } else {
        Judge ("f/side-declared[" + $inst.name + "]") $false "no 'SCENARIO relink start ... side=' line - the scenario never armed here"
    }
}

# (g) every instance finished its own scenario cleanly.
foreach ($inst in $instances) {
    $detail = "no 'SCENARIO RESULT' line"
    if ($inst.result -ne "") { $detail = "SCENARIO RESULT " + $inst.result }
    Judge ("g/scenario-result[" + $inst.name + "]") ($inst.result -eq 'PASS') $detail
}

# ---- verdict --------------------------------------------------------------------
$pass = (@($script:fails).Count -eq 0)
Write-Host ""
foreach ($n in $notes) { Write-Host ("RELINK NOTE: " + $n) }
Write-Host ("RELINK SUMMARY: instances={0} relinksIssued={1} hostBoundaries={2} nonEmptyBoundaries={3} slotsFull={4} failedChecks={5} runDir={6}" -f `
    @($instances).Count, $totalIssued, @($hostStops).Count, @($nonEmpty).Count, $H.slotsFull, @($script:fails).Count, $RunDir)
if (-not $pass) { Write-Host ("RELINK FAILED CHECKS: " + (@($script:fails) -join ', ')) }
Write-Host ("RELINK RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }))
if ($pass) { exit 0 } else { exit 1 }
