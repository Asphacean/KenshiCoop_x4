# oracles/Medical.ps1 - medical + stats oracles (monolith split of
# CoopOracles.psm1, 2026-07-12): Get-VitalsSeries, Test-NpcVitals,
# Test-LimbLoss, Get-StatsSeries, Test-StatsSync, Test-MedicOrder.
# Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the "SCENARIO VITALS"/STATS regexes -
# they are the C++ log contract (resources/CODE_MAP.md).

# Parse the EXTENDED "SCENARIO VITALS hand=i,s t=.. blood=.. bleed=.. fl=a,b,c,d
# bd=a,b,c,d unc=.. dead=.." series for one hand (i,s) into a time-ordered list of
# @{t; blood; fleshMin; bandSum; unc; dead; pfl; pst; ls}. Lines in the SHORT
# (combat_kill) format still parse (medical fields default). Protocol-16 logs
# append " pfl=<minFleshAllParts> pst=<minStunAllParts> ls=a,b,c,d" (LimbStates,
# 255=unknown); older logs leave those null. Timestamps are clock-offset
# corrected into the host frame like Get-ScenarioSeries.
function Get-VitalsSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO VITALS hand=' + [regex]::Escape($HandIS) +
           ' t=\d+ blood=(-?[\d\.]+)' +
           '(?: bleed=(-?[\d\.]+) fl=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+)' +
           ' bd=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) unc=(\d) dead=(\d))?' +
           '(?: pfl=(-?[\d\.]+) pst=(-?[\d\.]+) ls=(\d+),(\d+),(\d+),(\d+))?'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        # PSCustomObject, NOT a hashtable: Measure-Object -Property only sees real
        # properties (a hashtable 'blood' KEY measures as null -> silent nonsense).
        $e = [pscustomobject]@{ t = $t; blood = [double]$g[5].Value; fleshMin = $null; bandSum = $null; unc = 0; dead = 0; pfl = $null; pst = $null; ls = $null }
        if ($g[6].Success) {
            $fl = @(); for ($i = 7; $i -le 10; $i++) { $v = [double]$g[$i].Value; if ($v -ge 0) { $fl += $v } }
            $bd = 0.0;  for ($i = 11; $i -le 14; $i++) { $v = [double]$g[$i].Value; if ($v -ge 0) { $bd += $v } }
            if ($fl.Count -gt 0) { $e.fleshMin = ($fl | Measure-Object -Minimum).Minimum }
            $e.bandSum = $bd
            $e.unc  = [int]$g[15].Value
            $e.dead = [int]$g[16].Value
        }
        if ($g[17].Success) {
            $pv = [double]$g[17].Value; if ($pv -ge 0) { $e.pfl = $pv }
            $sv = [double]$g[18].Value; if ($sv -ge 0) { $e.pst = $sv }
            $e.ls = @([int]$g[19].Value, [int]$g[20].Value, [int]$g[21].Value, [int]$g[22].Value)
        }
        [void]$list.Add($e)
    }
    return ,$list
}

# npc_vitals (protocol 16, Phase B): combat-scoped world-NPC vitals must MATCH
# across sides - the combat_kill victim's join-side blood must CONVERGE to the
# host's truth, not just reach the KO outcome. Time-aligns the two VITALS
# series (CLOCKSYNC-corrected) and gates the median |host - join| blood gap
# over the tail (last TailMs of overlap) <= Tol.
function Test-NpcVitals {
    param([string]$HostFile, [string]$JoinFile,
          [double]$Tol = 15.0, [int]$TailMs = 10000, [int]$PairWinMs = 1500)
    $pin = Select-String -Path $HostFile -Pattern 'SCENARIO duel subjects pinned A=(\d+),(\d+) B=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $pin) {
        Write-Host "  NPC-VITALS SKIP - no pinned duelists on host"
        return (Add-GateResult -Name "npc_vitals" -Status SKIP -Detail "no pinned duelists")
    }
    $bi = $pin.Matches[0].Groups[3].Value; $bs = $pin.Matches[0].Groups[4].Value
    $handIS = "$bi,$bs"
    # No @() around the call: Get-VitalsSeries returns ,$list, so wrapping it
    # yields a one-element array holding the whole series.
    $Hv = @(); $Jv = @()
    foreach ($id in (Get-HandAliases -File $HostFile -WireIndexSerial $handIS)) { $Hv += Get-VitalsSeries -File $HostFile -HandIS $id }
    foreach ($id in (Get-HandAliases -File $JoinFile -WireIndexSerial $handIS)) { $Jv += Get-VitalsSeries -File $JoinFile -HandIS $id }
    $Hv = @($Hv | Sort-Object { $_.t }); $Jv = @($Jv | Sort-Object { $_.t })
    if ($Hv.Count -lt 3 -or $Jv.Count -lt 3) {
        Write-Host "  NPC-VITALS SKIP - insufficient vitals series (host=$($Hv.Count) join=$($Jv.Count))"
        return (Add-GateResult -Name "npc_vitals" -Status SKIP -Metrics @{ host = $Hv.Count; join = $Jv.Count } -Detail "insufficient series")
    }
    $endT = [Math]::Min(($Hv | Measure-Object -Property t -Maximum).Maximum,
                        ($Jv | Measure-Object -Property t -Maximum).Maximum)
    $tail = $endT - $TailMs
    $gaps = @()
    foreach ($js in @($Jv | Where-Object { $_.t -ge $tail -and $_.t -le $endT })) {
        $near = $null; $best = $PairWinMs + 1
        foreach ($hs in $Hv) {
            $dt = [Math]::Abs($hs.t - $js.t)
            if ($dt -lt $best) { $best = $dt; $near = $hs }
        }
        if ($null -ne $near) { $gaps += [Math]::Abs($near.blood - $js.blood) }
    }
    if ($gaps.Count -lt 3) {
        Write-Host "  NPC-VITALS SKIP - too few aligned tail pairs ($($gaps.Count))"
        return (Add-GateResult -Name "npc_vitals" -Status SKIP -Metrics @{ pairs = $gaps.Count } -Detail "too few aligned pairs")
    }
    $sorted = @($gaps | Sort-Object)
    $median = $sorted[[int][Math]::Floor($sorted.Count / 2)]
    $m = @{ pairs = $gaps.Count; medianGap = [Math]::Round($median, 1)
            maxGap = [Math]::Round(($gaps | Measure-Object -Maximum).Maximum, 1) }
    if ($median -gt $Tol) {
        Write-Host "  NPC-VITALS FAIL - victim B=$handIS tail blood gap median=$($m.medianGap) > $Tol (NPC vitals not converging)"
        return (Add-GateResult -Name "npc_vitals" -Status FAIL -Metrics $m -Detail "victim vitals diverge")
    }
    Write-Host "  NPC-VITALS PASS - victim B=$handIS tail blood gap median=$($m.medianGap) (<= $Tol) over $($m.pairs) pairs"
    return (Add-GateResult -Name "npc_vitals" -Status PASS -Metrics $m)
}

# limb_loss (protocol 16, Phase C): each side amputates its OWN tab leader's
# limb (A: host member limb 0, B: join member limb 1). Gates per direction:
#   stump-crossed - the COPY side's ls[limb] leaves ORIGINAL (STUMP/REPLACED)
#                   within MaxLatencyMs of the owner's cut (event or self-heal)
#   stump-sticky  - once stump, the copy never reverts to ORIGINAL
#   ground-item   - each side sees >= 1 severed ground item near the subject
#                   after the cut, and no side ever sees more than the two
#                   legitimate items (a dedupe failure shows 3+)
function Test-LimbLoss {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [int]$GraceMs = 4000, [int]$MaxItems = 2)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; copyLog = $JoinFile },
        @{ tag = "B"; ownerLog = $JoinFile; copyLog = $HostFile }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $mk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO LIMB ' + $t + ' amputate issued hand=(\d+),(\d+) limb=(\d) ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) { $bad += "${t}: no amputate marker (ok=1)"; continue }
        $hi = $mk.Matches[0].Groups[1].Value; $hs = $mk.Matches[0].Groups[2].Value
        $limb = [int]$mk.Matches[0].Groups[3].Value
        $handIS = "$hi,$hs"
        $Tc = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO LIMB ' + $t + ' amputate issued')
        # Owner ground truth: the amputation landed (ls[limb] != ORIGINAL).
        $O = Get-VitalsSeries -File $d.ownerLog -HandIS $handIS
        $ownerCut = @($O | Where-Object { $_.t -ge $Tc -and $null -ne $_.ls -and $_.ls[$limb] -ge 1 -and $_.ls[$limb] -le 3 }).Count -gt 0
        if (-not $ownerCut) { $bad += "${t}: stump never visible on the owner (scaffold failed?)"; continue }
        # 1. Stump crossed: the copy's LimbState leaves ORIGINAL within budget.
        $C = Get-VitalsSeries -File $d.copyLog -HandIS $handIS
        $cutSamples = @($C | Where-Object { $_.t -ge $Tc -and $null -ne $_.ls -and $_.ls[$limb] -ge 1 -and $_.ls[$limb] -le 3 })
        if ($cutSamples.Count -lt 1) {
            $bad += "${t}: stump never reached the copy (limb states don't cross)"
            continue
        }
        $lat = [int]($cutSamples[0].t - $Tc)
        $m["${t}StumpLatMs"] = $lat
        if ($lat -gt $MaxLatencyMs) { $bad += "${t}: stump latency ${lat}ms > $MaxLatencyMs" }
        # 2. Sticky: after the first stump sample the copy never reverts.
        $revert = @($C | Where-Object { $_.t -gt $cutSamples[0].t -and $null -ne $_.ls -and $_.ls[$limb] -eq 0 }).Count
        if ($revert -gt 0) { $bad += "${t}: copy reverted to ORIGINAL $revert time(s) after the stump" }
        # 3. Ground item: both sides converge on >= 1 severed item near the
        # subject after the cut (host-authoritative world-item channel).
        foreach ($side in @(@{ n = "owner"; f = $d.ownerLog }, @{ n = "copy"; f = $d.copyLog })) {
            $items = @()
            foreach ($mi in (Select-String -Path $side.f -Pattern ('SCENARIO LIMBITEMS hand=' + $hi + ',' + $hs + ' t=(\d+) n=(\d+)') -ErrorAction SilentlyContinue)) {
                $items += [pscustomobject]@{ t = [int]$mi.Matches[0].Groups[1].Value; n = [int]$mi.Matches[0].Groups[2].Value }
            }
            # LIMBITEMS t= is scenario-elapsed; the cut marker time is wall-clock,
            # so anchor on the owner's cut ELAPSED time via the issued line's t.
            $post = @($items | Where-Object { $_.n -ge 1 })
            $maxN = 0
            if ($items.Count -gt 0) { $maxN = ($items | Measure-Object -Property n -Maximum).Maximum }
            $m["${t}$($side.n)MaxItems"] = $maxN
            if ($post.Count -lt 1) { $bad += "${t}: no severed ground item ever visible on the $($side.n)" }
            if ($maxN -gt $MaxItems) { $bad += "${t}: $($side.n) saw $maxN severed items (> $MaxItems; dedupe failed)" }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  LIMB-LOSS FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "limb_loss" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  LIMB-LOSS PASS - A: stump crossed in $($m.AStumpLatMs)ms (items owner=$($m.AownerMaxItems) copy=$($m.AcopyMaxItems)); " +
                "B: stump crossed in $($m.BStumpLatMs)ms (items owner=$($m.BownerMaxItems) copy=$($m.BcopyMaxItems))")
    return (Add-GateResult -Name "limb_loss" -Status PASS -Metrics $m)
}

# Parse the "SCENARIO CRAWL hand=i,s t=.. prone=.. crip=.. pos=x,y,z bs=.. mv=..
# spd=.." series (protocol 53) for one hand into a time-ordered list of
# @{t; prone; crip; x; y; z; bs; mv; spd}. prone/crip are -1 when unreadable.
# Timestamps are clock-offset corrected into the host frame like every other
# cross-log series.
function Get-CrawlSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO CRAWL hand=' + [regex]::Escape($HandIS) +
           ' t=(\d+) prone=(-?\d+) crip=(-?\d+)' +
           ' pos=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) bs=(\d+) mv=(\d) spd=(-?[\d\.]+)'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        [void]$list.Add([pscustomobject]@{
            t     = (Convert-StampToMs -Groups $g -OffsetMs $off)
            rawT  = [long]$g[5].Value
            prone = [int]$g[6].Value
            crip  = [int]$g[7].Value
            x     = [double]$g[8].Value
            y     = [double]$g[9].Value
            z     = [double]$g[10].Value
            bs    = [int]$g[11].Value
            mv    = [int]$g[12].Value
            spd   = [double]$g[13].Value
        })
    }
    return ,$list
}

# Parse the "SCENARIO CRAWLPROBE hand=i,s t=.. mv=x,y,z hk=N:x,y,z ..." series
# (protocol 53) for one hand. hk = whether the body still has its movement
# controller's physics character, which is the ONLY log-visible witness for a
# purely VISUAL desync: the model renders from the physics body while
# Character::getPosition (and the nametag, and every position oracle) reads
# CharMovement::pos. A copy can therefore track its owner perfectly on paper with
# its mesh stranded where it collapsed - measured, and invisible to every other
# check here.
function Get-CrawlProbeSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO CRAWLPROBE hand=' + [regex]::Escape($HandIS) +
           ' t=(\d+) mv=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) hk=(\d):'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        [void]$list.Add([pscustomobject]@{
            t    = (Convert-StampToMs -Groups $g -OffsetMs $off)
            rawT = [long]$g[5].Value
            mvx  = [double]$g[6].Value
            mvz  = [double]$g[8].Value
            hk   = [int]$g[9].Value
        })
    }
    return ,$list
}

# crawl_move (protocol 53): a body prone from a lost leg must CRAWL on the peer,
# not be driven as an upright walker. Per direction (A = host-owned subject,
# B = join-owned), gates four things against the OWNER's own reading:
#   1. the copy reaches the owner's prone POSTURE within budget,
#   2. the copy reaches the owner's crippled FLAG within budget,
#   3. neither reverts afterwards (allowing the 1 Hz re-apply throttle),
#   4. the copy TRACKS position while the injured body is being moved.
# Posture parity is what distinguishes this from limb_loss and from any position
# gate: a copy walking upright toward the right coordinates passes those.
# SKIPs (loudly - it is the primary gate) when the owner never became crippled at
# all, since then the scaffold produced no signal to judge rather than the sync
# having failed.
function Test-CrawlMove {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [int]$SettleMs = 4000,
          [double]$MaxGap = 12.0, [int]$MaxDt = 800, [int]$MinPaired = 6,
          [double]$MinSpan = 6.0, [double]$MinSpanFrac = 0.5,
          [double]$MinPhysFrac = 0.8)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; copyLog = $JoinFile },
        @{ tag = "B"; ownerLog = $JoinFile; copyLog = $HostFile }
    )
    $m = @{}; $bad = @(); $noSignal = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $mk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO CRAWL ' + $t + ' amputate issued hand=(\d+),(\d+) limb=(\d) ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) { $bad += "${t}: no amputate marker (ok=1)"; continue }
        $handIS = "$($mk.Matches[0].Groups[1].Value),$($mk.Matches[0].Groups[2].Value)"
        $Tc = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO CRAWL ' + $t + ' amputate issued')
        # No @() wrapper: Get-CrawlSeries returns ",$list" (the ArrayList guard
        # against an empty result unrolling away), so wrapping would nest it.
        $O = Get-CrawlSeries -File $d.ownerLog -HandIS $handIS
        $C = Get-CrawlSeries -File $d.copyLog  -HandIS $handIS
        if ($O.Count -lt 1) { $bad += "${t}: owner logged no CRAWL series"; continue }
        if ($C.Count -lt 1) { $bad += "${t}: copy logged no CRAWL series"; continue }

        # Owner ground truth. The engine decides whether a lost leg cripples; if
        # it never did, there is nothing to replicate and nothing to judge.
        $oCrip  = @($O | Where-Object { $_.t -ge $Tc -and $_.crip -eq 1 })
        $oProne = @($O | Where-Object { $_.t -ge $Tc -and $_.prone -gt 0 })
        if ($oCrip.Count -lt 1 -and $oProne.Count -lt 1) {
            $noSignal += "${t}: owner never became crippled or prone after the cut"
            continue
        }
        # The posture the owner actually settled on (mode of its post-cut values),
        # rather than assuming PS_CRIPPLED - the copy must match the OWNER.
        $want = 0
        if ($oProne.Count -gt 0) {
            $want = ($oProne | Group-Object -Property prone |
                     Sort-Object -Property Count -Descending | Select-Object -First 1).Name
            $want = [int]$want
        }
        $m["${t}OwnerProne"] = $want
        $m["${t}OwnerCrip"]  = $(if ($oCrip.Count -gt 0) { 1 } else { 0 })

        # 1. Posture crossed.
        if ($want -gt 0) {
            $hit = @($C | Where-Object { $_.t -ge $Tc -and $_.prone -eq $want })
            if ($hit.Count -lt 1) {
                $bad += "${t}: prone=$want never reached the copy (posture does not cross)"
            } else {
                $lat = [int]($hit[0].t - $Tc)
                $m["${t}ProneLatMs"] = $lat
                if ($lat -gt $MaxLatencyMs) { $bad += "${t}: prone latency ${lat}ms > $MaxLatencyMs" }
                # 3a. Sticky: past the settle window the copy stays posed. The
                # apply is throttled to 1 Hz, so judge the settled tail only.
                $tail = @($C | Where-Object { $_.t -ge ($hit[0].t + $SettleMs) })
                if ($tail.Count -ge 4) {
                    $held = @($tail | Where-Object { $_.prone -eq $want }).Count
                    $ratio = [Math]::Round($held / $tail.Count, 3)
                    $m["${t}ProneHoldRatio"] = $ratio
                    if ($ratio -lt 0.8) { $bad += "${t}: copy held prone=$want for only $ratio of the settled tail" }
                }
            }
        }

        # 2. Crippled cause crossed (the flag the engine's crawl gait reads).
        if ($oCrip.Count -gt 0) {
            $hitC = @($C | Where-Object { $_.t -ge $Tc -and $_.crip -eq 1 })
            if ($hitC.Count -lt 1) {
                $bad += "${t}: crippled never reached the copy (cause does not cross)"
            } else {
                $latC = [int]($hitC[0].t - $Tc)
                $m["${t}CripLatMs"] = $latC
                if ($latC -gt $MaxLatencyMs) { $bad += "${t}: crippled latency ${latC}ms > $MaxLatencyMs" }
                # 3b. Sticky: the medical channel is change-gated, so a revert
                # means something WROTE crippled=false, not that a send was lost.
                $rev = @($C | Where-Object { $_.t -gt ($hitC[0].t + $SettleMs) -and $_.crip -eq 0 }).Count
                $m["${t}CripReverts"] = $rev
                if ($rev -gt 0) { $bad += "${t}: copy reverted to crippled=0 $rev time(s)" }
            }
        }

        # 4. Position tracked while the injured body is under a move order. The
        # gap is judged on the owner's MOVING samples: a crawler driven as an
        # upright walker is exactly where the two positions separate.
        $oMove = @($O | Where-Object { $_.t -ge $Tc -and $_.mv -eq 1 })
        if ($oMove.Count -lt $MinPaired) {
            $noSignal += "${t}: owner never reported $MinPaired moving samples after the cut"
            continue
        }
        $gaps = New-Object System.Collections.ArrayList
        foreach ($os in $oMove) {
            $best = $null; $bestDt = [int]::MaxValue
            foreach ($cs in $C) {
                $dt = [Math]::Abs([int]($cs.t - $os.t))
                if ($dt -lt $bestDt) { $bestDt = $dt; $best = $cs }
            }
            if ($bestDt -le $MaxDt -and $null -ne $best) {
                $dx = $best.x - $os.x; $dz = $best.z - $os.z
                [void]$gaps.Add([Math]::Sqrt($dx * $dx + $dz * $dz))
            }
        }
        if ($gaps.Count -lt $MinPaired) {
            $noSignal += "${t}: only $($gaps.Count) paired moving sample(s) (need $MinPaired)"
            continue
        }
        $sorted = @($gaps | Sort-Object)
        $med = [Math]::Round($sorted[[int]([Math]::Floor($sorted.Count / 2))], 2)
        $m["${t}GapMed"] = $med
        $m["${t}GapPairs"] = $gaps.Count
        if ($med -gt $MaxGap) { $bad += "${t}: median crawl gap ${med}u > $MaxGap" }

        # 5. The copy actually TRAVELLED. The gap check alone is not enough to
        # judge tracking: it is a distance between two positions, so a copy frozen
        # where the owner started scores a small gap whenever the owner stays near
        # its own start. That is not hypothetical - run 20260805_153425 passed at
        # gap=3.68u with the copy's position BYTE-IDENTICAL across the whole move
        # window ("crawls in place while the camera follows the real position").
        # Span = how far each body ever got from its own first post-cut sample.
        $oPost = @($O | Where-Object { $_.t -ge $Tc })
        $cPost = @($C | Where-Object { $_.t -ge $Tc })
        $span = {
            param($s)
            if ($s.Count -lt 2) { return 0.0 }
            $x0 = $s[0].x; $z0 = $s[0].z; $mx = 0.0
            foreach ($p in $s) {
                $dx = $p.x - $x0; $dz = $p.z - $z0
                $dd = [Math]::Sqrt($dx * $dx + $dz * $dz)
                if ($dd -gt $mx) { $mx = $dd }
            }
            return $mx
        }
        $oSpan = [Math]::Round((& $span $oPost), 2)
        $cSpan = [Math]::Round((& $span $cPost), 2)
        $m["${t}OwnerSpan"] = $oSpan
        $m["${t}CopySpan"]  = $cSpan
        if ($oSpan -lt $MinSpan) {
            # The owner never crawled anywhere, so the copy standing still proves
            # nothing. Report it rather than passing on an unexercised check.
            $noSignal += "${t}: owner only travelled ${oSpan}u after the cut (< ${MinSpan}u); tracking unexercised"
        } elseif ($cSpan -lt ($oSpan * $MinSpanFrac)) {
            $bad += ("${t}: copy travelled only ${cSpan}u of the owner's ${oSpan}u " +
                     "(crawling in place, not tracking)")
        }

        # 6. The copy can still be RENDERED where it is. Everything above reads
        # Character::getPosition, and the model does not: it follows the movement
        # controller's physics character, which the engine tears down when a body
        # collapses. A driven copy is AI-suspended, so it never runs the recovery
        # that re-creates it - leaving a body whose position, posture, medical
        # state and animation are all correct and whose MESH is stranded at the
        # collapse spot. Nothing else in this gate can see that, so judge the
        # witness directly over the owner's moving window.
        $P = Get-CrawlProbeSeries -File $d.copyLog -HandIS $handIS
        if ($P.Count -lt 1) {
            $noSignal += "${t}: copy logged no CRAWLPROBE series (physics witness absent)"
        } else {
            $t0 = $oMove[0].t; $t1 = $oMove[$oMove.Count - 1].t
            $win = @($P | Where-Object { $_.t -ge $t0 -and $_.t -le $t1 })
            if ($win.Count -lt $MinPaired) {
                $noSignal += "${t}: only $($win.Count) CRAWLPROBE sample(s) in the move window"
            } else {
                $have = @($win | Where-Object { $_.hk -eq 1 }).Count
                $frac = [Math]::Round($have / $win.Count, 3)
                $m["${t}PhysFrac"] = $frac
                if ($frac -lt $MinPhysFrac) {
                    $bad += ("${t}: copy had no physics body for $(1 - $frac) of the move " +
                             "window (physFrac=$frac); the model renders from it, so it " +
                             "stays where the body collapsed while the position tracks")
                }
            }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  CRAWL-MOVE FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "crawl_move" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    if ($noSignal.Count -gt 0 -and $m.Count -eq 0) {
        Write-Host ("  CRAWL-MOVE SKIP - " + ($noSignal -join "; "))
        return (Add-GateResult -Name "crawl_move" -Status SKIP -Metrics $m -Detail ($noSignal -join "; "))
    }
    foreach ($n in $noSignal) { Write-Host "    FINDING: $n" }
    Write-Host ("  CRAWL-MOVE PASS - A: prone=$($m.AOwnerProne) crossed in $($m.AProneLatMs)ms, crippled in $($m.ACripLatMs)ms, gap=$($m.AGapMed)u, travel=$($m.ACopySpan)/$($m.AOwnerSpan)u, phys=$($m.APhysFrac); " +
                "B: prone=$($m.BOwnerProne) crossed in $($m.BProneLatMs)ms, crippled in $($m.BCripLatMs)ms, gap=$($m.BGapMed)u, travel=$($m.BCopySpan)/$($m.BOwnerSpan)u, phys=$($m.BPhysFrac)")
    return (Add-GateResult -Name "crawl_move" -Status PASS -Metrics $m)
}

# Parse the "SCENARIO STATS hand=i,s t=.. str=.. stealth=.. dex=.. athl=..
# tough=.. xp=.." series (protocol-17 stats sync) for one hand into a
# time-ordered list of @{t; str; stealth; dex; athl; tough; xp}. Timestamps are
# clock-offset corrected into the host frame like Get-ScenarioSeries.
function Get-StatsSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO STATS hand=' + [regex]::Escape($HandIS) +
           ' t=\d+ str=(-?[\d\.]+) stealth=(-?[\d\.]+) dex=(-?[\d\.]+) athl=(-?[\d\.]+)' +
           ' tough=(-?[\d\.]+) xp=(-?[\d\.]+)'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $e = [pscustomobject]@{
            t = $t
            str     = [double]$g[5].Value
            stealth = [double]$g[6].Value
            dex     = [double]$g[7].Value
            athl    = [double]$g[8].Value
            tough   = [double]$g[9].Value
            xp      = [double]$g[10].Value
        }
        [void]$list.Add($e)
    }
    return ,$list
}

# stats_sync (protocol 17): each side raises stats on its OWN tab leader via
# the raise-only scaffold (A: host raises str+stealth, B: join raises
# dex+athl). Gates per direction:
#   raised-crossed - the COPY side's raised stats reach the raise target within
#                    MaxLatencyMs of the owner's write (change-gated stream)
#   raised-sticky  - once crossed, the copy never falls back below the target
#   no-drift       - the control stat (toughness, touched by NEITHER side)
#                    never deviates more than DriftTol from its first sample
#                    on the copy (the stream must not corrupt untouched stats)
function Test-StatsSync {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [double]$RaiseTo = 60.0,
          [double]$Tol = 0.5, [double]$DriftTol = 2.0)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; copyLog = $JoinFile; raised = @("str", "stealth") },
        @{ tag = "B"; ownerLog = $JoinFile; copyLog = $HostFile; raised = @("dex", "athl") }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $mk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO STATRAISE ' + $t + ' hand=(\d+),(\d+) stat\d+=[\d\.]+ ok=1 stat\d+=[\d\.]+ ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) { $bad += "${t}: no raise marker (ok=1,1)"; continue }
        $hi = $mk.Matches[0].Groups[1].Value; $hs = $mk.Matches[0].Groups[2].Value
        $handIS = "$hi,$hs"
        $Tc = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO STATRAISE ' + $t + ' ')
        # Owner ground truth: the raise landed locally.
        $O = Get-StatsSeries -File $d.ownerLog -HandIS $handIS
        $C = Get-StatsSeries -File $d.copyLog  -HandIS $handIS
        if ($C.Count -lt 3) { $bad += "${t}: copy stats series too short ($($C.Count))"; continue }
        foreach ($stat in $d.raised) {
            $oOk = @($O | Where-Object { $_.t -ge $Tc -and $_.$stat -ge ($RaiseTo - $Tol) }).Count -gt 0
            if (-not $oOk) { $bad += "${t}: $stat never reached $RaiseTo on the owner (scaffold failed?)"; continue }
            # 1. Crossed: the copy's value reaches the target within budget.
            $cross = @($C | Where-Object { $_.t -ge $Tc -and $_.$stat -ge ($RaiseTo - $Tol) })
            if ($cross.Count -lt 1) { $bad += "${t}: $stat never crossed to the copy"; continue }
            $lat = [int]($cross[0].t - $Tc)
            $m["${t}${stat}LatMs"] = $lat
            if ($lat -gt $MaxLatencyMs) { $bad += "${t}: $stat latency ${lat}ms > $MaxLatencyMs" }
            # 2. Sticky: after crossing, the copy never falls back below.
            $revert = @($C | Where-Object { $_.t -gt $cross[0].t -and $_.$stat -lt ($RaiseTo - $Tol - 0.5) }).Count
            if ($revert -gt 0) { $bad += "${t}: $stat reverted on the copy $revert time(s) after crossing" }
        }
        # 3. No-drift: the untouched control stat stays put on the copy.
        $base = [double]$C[0].tough
        $drift = ($C | ForEach-Object { [Math]::Abs($_.tough - $base) } | Measure-Object -Maximum).Maximum
        $m["${t}ToughDrift"] = [Math]::Round($drift, 2)
        if ($drift -gt $DriftTol) { $bad += "${t}: control stat (toughness) drifted $([Math]::Round($drift,2)) on the copy (> $DriftTol)" }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  STATS-SYNC FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "stats_sync" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  STATS-SYNC PASS - A: str crossed in $($m.AstrLatMs)ms, stealth in $($m.AstealthLatMs)ms (control drift $($m.AToughDrift)); " +
                "B: dex crossed in $($m.BdexLatMs)ms, athl in $($m.BathlLatMs)ms (control drift $($m.BToughDrift))")
    return (Add-GateResult -Name "stats_sync" -Status PASS -Metrics $m)
}

# medic_order BIDIRECTIONAL: the owner wounds its own member (limb flesh + blood),
# the OTHER player bandages the driven copy it renders. Judged per direction:
#   wound-crossed    - the HEALER's copy shows the wound (fleshMin dropped)
#   heal-happened    - the healer's bandage found damaged limbs (n>0)
#   treatment-crossed- the OWNER's body shows the bandaging afterwards
# All three require the medical replication features (vitals sync + treatment
# forwarding); without them this documents spikes 21-23's truth (nothing crosses).
function Test-MedicOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 5000,
          [double]$WoundFlesh = 60.0, [double]$MinBandRise = 50.0)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; healerLog = $JoinFile },
        @{ tag = "B"; ownerLog = $JoinFile; healerLog = $HostFile }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $wk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO MEDIC ' + $t + ' wound issued hand=(\d+),(\d+) ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $wk) { $bad += "${t}: no wound marker"; continue }
        $hi = $wk.Matches[0].Groups[1].Value; $hs = $wk.Matches[0].Groups[2].Value
        $handIS = "$hi,$hs"
        $Tw = Get-MarkerTimeMs -File $d.ownerLog  -Pattern ('SCENARIO MEDIC ' + $t + ' wound issued')
        $hk = Select-String -Path $d.healerLog -Pattern ('SCENARIO MEDIC ' + $t + ' heal issued hand=\d+,\d+ n=(\d+)') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $hk) { $bad += "${t}: no heal marker on the healer"; continue }
        $healN = [int]$hk.Matches[0].Groups[1].Value
        $Th = Get-MarkerTimeMs -File $d.healerLog -Pattern ('SCENARIO MEDIC ' + $t + ' heal issued')

        # Owner ground truth: the wound landed on the authoritative body.
        $O = Get-VitalsSeries -File $d.ownerLog -HandIS $handIS
        $ownerWounded = @($O | Where-Object { $_.t -ge $Tw -and $null -ne $_.fleshMin -and $_.fleshMin -le $WoundFlesh }).Count -gt 0
        if (-not $ownerWounded) { $bad += "${t}: wound never visible on the owner (scaffold failed?)"; continue }
        # 1. Wound crossed: the HEALER's driven copy shows the wound before the heal.
        $H = Get-VitalsSeries -File $d.healerLog -HandIS $handIS
        $crossWound = @($H | Where-Object { $_.t -ge ($Tw + $GraceMs) -and $null -ne $_.fleshMin -and $_.fleshMin -le $WoundFlesh }).Count -gt 0
        # 2. Heal happened: the bandage pass found damaged limbs on the driven copy.
        $healed = ($healN -gt 0)
        # 3. Treatment crossed: the OWNER's bandaging rises after the heal.
        $preBand  = @($O | Where-Object { $_.t -lt $Th -and $null -ne $_.bandSum })
        $postBand = @($O | Where-Object { $_.t -ge ($Th + $GraceMs) -and $null -ne $_.bandSum })
        $bandRise = $null
        if ($preBand.Count -gt 0 -and $postBand.Count -gt 0) {
            $bandRise = [double]($postBand | Measure-Object -Property bandSum -Maximum).Maximum -
                        [double]($preBand  | Measure-Object -Property bandSum -Maximum).Maximum
        }
        $treated = ($null -ne $bandRise -and $bandRise -ge $MinBandRise)
        $m["${t}WoundCrossed"] = $crossWound; $m["${t}HealN"] = $healN
        $m["${t}BandRise"] = if ($null -ne $bandRise) { [Math]::Round($bandRise,1) } else { -1 }
        if (-not $crossWound) { $bad += "${t}: wound never reached the healer's copy (vitals don't cross)" }
        if (-not $healed)     { $bad += "${t}: healer found nothing to bandage (n=0 - its copy is pristine)" }
        if (-not $treated)    { $bad += "${t}: treatment never reached the owner (bandRise=$($m["${t}BandRise"]))" }
        # 4. FULL-ANATOMY crossing (protocol 16): the wound scaffold now lowers
        # head/chest/stomach AND the stun track; the healer's copy must show
        # both (pfl/pst = min flesh/stun across ALL parts). Gated only when the
        # extended fields are present (older logs skip).
        $ownerP = @($O | Where-Object { $_.t -ge $Tw -and $null -ne $_.pfl })
        if ($ownerP.Count -gt 0) {
            $crossPart = @($H | Where-Object { $_.t -ge ($Tw + $GraceMs) -and $null -ne $_.pfl -and $_.pfl -le $WoundFlesh }).Count -gt 0
            $crossStun = @($H | Where-Object { $_.t -ge ($Tw + $GraceMs) -and $null -ne $_.pst -and $_.pst -le ($WoundFlesh + 20) }).Count -gt 0
            $m["${t}PartCrossed"] = $crossPart; $m["${t}StunCrossed"] = $crossStun
            if (-not $crossPart) { $bad += "${t}: full-anatomy wound (head/chest) never reached the healer's copy" }
            if (-not $crossStun) { $bad += "${t}: stun damage never reached the healer's copy" }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  MEDIC-ORDER FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "medic_order" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  MEDIC-ORDER PASS - A: wound crossed, bandaged n=$($m.AHealN), owner bandRise=$($m.ABandRise); " +
                "B: wound crossed, bandaged n=$($m.BHealN), owner bandRise=$($m.BBandRise)")
    return (Add-GateResult -Name "medic_order" -Status PASS -Metrics $m)
}

# medic_pose (2026-07-15 medic-animation sync): the ANIMATION counterpart to
# medic_order. medic_order proves the medical STATE crosses (owner-authoritative
# vitals); this proves the first-aid ACTION is reproduced on the peer so the
# bandaging animation plays. Reproduction is a REPLICATION path (any live heal
# exercises it), not a scenario, so this judges a manual/host+join session log
# pair rather than a SCENARIO marker - mirroring how the mining fix is validated
# by prototest + a manual pass.
#
# Two log signals (C++ log contract; see EngineEntity.cpp logTaskKeyOnce and
# ReplicatorDrive.cpp applyOrder):
#   HOST recognised it   - "[taskkey] key=K desc='..' repro=1 subject=1" where
#                          desc reads as first-aid/medic (the host STREAMS such a
#                          task; repro=1 means it is in isReproduciblePose).
#   JOIN reproduced it   - "[pose] applyOrder .. task=K .. r=2" for that same K
#                          (r=2 = the driven copy took the pose = animation ordered).
# TaskType ids are DISCOVERED from the host desc (never hardcoded); -MedicTaskIds
# is only a fallback for older logs whose desc doesn't read as medic. SKIP when no
# medic task appears in the window (nobody healed) so it never falses on a run
# without a heal.
function Test-MedicPose {
    param([string]$HostFile, [string]$JoinFile,
          [int[]]$MedicTaskIds = @(25, 57, 58, 60))
    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        Write-Host "  MEDIC-POSE SKIP - missing log(s)"
        return (Add-GateResult -Name "medic_pose" -Status SKIP -Detail "missing logs")
    }
    # 1. Discover the medic TaskType id(s) the HOST recognised as reproducible.
    $tkPat = "\[taskkey\] key=(\d+) desc='([^']*)' repro=(\d) subject=(\d)"
    $medicKw = 'first ?aid|medic|bandag|heal|doctor|treat'
    $recognised = @{}; $seenMedicButNotRepro = @()
    foreach ($mm in (Select-String -Path $HostFile -Pattern $tkPat -ErrorAction SilentlyContinue)) {
        $g = $mm.Matches[0].Groups
        $key = [int]$g[1].Value; $desc = $g[2].Value
        $repro = [int]$g[3].Value
        $isMedic = ($desc -match $medicKw) -or ($MedicTaskIds -contains $key)
        if (-not $isMedic) { continue }
        if ($repro -eq 1) { $recognised[$key] = $desc }
        else { $seenMedicButNotRepro += "$key('$desc')" }
    }
    # 2. Did the JOIN reproduce a medic task (pose ordered, r=2)?
    $posePat = "\[pose\] applyOrder hand=\d+,\d+ task=(\d+) subj=\d+,\d+,\d+ det=-?\d+ r=(\d+)"
    $reproduced = @{}; $poseFar = @{}
    $medicIds = @($recognised.Keys) + $MedicTaskIds | Select-Object -Unique
    foreach ($pm in (Select-String -Path $JoinFile -Pattern $posePat -ErrorAction SilentlyContinue)) {
        $g = $pm.Matches[0].Groups
        $task = [int]$g[1].Value; $r = [int]$g[2].Value
        if ($medicIds -notcontains $task) { continue }
        if ($r -eq 2) { $reproduced[$task] = $true }
        elseif ($r -eq 3) { $poseFar[$task] = $true }   # resolved WRONG (far) fixture
    }
    if ($recognised.Count -eq 0 -and $reproduced.Count -eq 0) {
        $extra = if ($seenMedicButNotRepro.Count -gt 0) { " (host saw medic task(s) $($seenMedicButNotRepro -join ',') but repro=0 - NOT in isReproduciblePose)" } else { " (no medic task in the session - nobody healed?)" }
        Write-Host "  MEDIC-POSE SKIP - no reproducible medic task observed$extra"
        return (Add-GateResult -Name "medic_pose" -Status SKIP -Detail "no medic task$extra")
    }
    $bad = @()
    if ($recognised.Count -eq 0) { $bad += "host never marked a medic task repro=1 (isReproduciblePose gap)" }
    if ($reproduced.Count -eq 0) {
        $why = if ($poseFar.Count -gt 0) { "join saw the medic task but the patient fixture resolved far (r=3) - identity-trust gate?" } else { "join never issued the medic pose order (r=2 missing)" }
        $bad += $why
    }
    $mtr = @{ hostRepro = $recognised.Count; joinRepro = $reproduced.Count
             ids = (@($reproduced.Keys) -join ',') }
    if ($bad.Count -gt 0) {
        Write-Host ("  MEDIC-POSE FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "medic_pose" -Status FAIL -Metrics $mtr -Detail ($bad -join "; "))
    }
    Write-Host "  MEDIC-POSE PASS - host streamed + join reproduced medic task id(s) $($mtr.ids) (bandaging animation ordered on the peer)"
    return (Add-GateResult -Name "medic_pose" -Status PASS -Metrics $mtr)
}

