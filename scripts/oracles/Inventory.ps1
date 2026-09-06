# oracles/Inventory.ps1 - inventory + world-item oracles (monolith split of
# CoopOracles.psm1, 2026-07-12): Test-InventorySync/Bidir/Equip/Reequip,
# Test-AddEquip, Test-TradeProbe, Test-TradePeer, Test-DropProbe,
# Test-WorldItemSync, Test-WpnRelocate, Test-WeaponDrop, Test-WeaponLoot,
# Test-InventoryOverflow, Test-InventoryDropFull (protocol 46 item-loss gates).
# Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the INV/TINV/DROP/GEAR marker regexes -
# they are the C++ log contract (resources/CODE_MAP.md).
# ---- Inventory / world-item oracles ------------------------------------------------

# inv_order (Phase 4a): content-snapshot replication; multiset (hash) gate.
function Test-InventorySync {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO INV (MEMBER|RECV) t=(\d+) count=(\d+) hash=(\d+)'
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{ count = [int]$matches[3]; hash = [uint32]$matches[4] }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 1) {
        Write-Host "  INV-SYNC FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "inv_sync" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $added = $false
    if (Test-Path $HostFile) { $added = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO INV ADD added=[1-9]' -Quiet) }
    $hFirst = $H[0]; $hLast = $H[$H.Count - 1]
    $jFirst = $J[0]; $jLast = $J[$J.Count - 1]
    $jChanged = $false
    foreach ($s in $J) { if ($s.hash -ne $jFirst.hash) { $jChanged = $true; break } }
    $hostChanged = ($hFirst.hash -ne $hLast.hash)
    $hashMatch   = ($hLast.hash -eq $jLast.hash)
    $joinNonEmpty= ($jLast.count -gt 0)
    $ok = ($added -and $hostChanged -and $hashMatch -and $joinNonEmpty)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  INV-SYNC $v - add=$added hostChanged=$hostChanged hashMatch=$hashMatch " +
                "joinNonEmpty=$joinNonEmpty (joinSawTransition=$jChanged advisory) " +
                "host=$($hFirst.count)->$($hLast.count) join=$($jFirst.count)->$($jLast.count) " +
                "finalHash host=$($hLast.hash) join=$($jLast.hash)")
    return (Add-GateResult -Name "inv_sync" -Status $v -Metrics @{
        added = $added; hostChanged = $hostChanged; hashMatch = $hashMatch
        joinNonEmpty = $joinNonEmpty; hostFinal = $hLast.count; joinFinal = $jLast.count })
}

# inv_bidir: per-rank convergence in BOTH directions (host authors r0, join r1).
function Test-InventoryBidir {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO INVB r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+)'
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; role = $matches[2]
                        count = [int]$matches[4]; hash = [uint32]$matches[5]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-BIDIR FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "inv_bidir" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $distinct = { param($rows) ($rows | ForEach-Object { $_.hash } | Select-Object -Unique).Count }
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 1 -or $obsRows.Count -lt 1) {
            Write-Host "  INV-BIDIR $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]
        $oLast = $obsRows[$obsRows.Count - 1]
        $aChanged = ((& $distinct $authorRows) -ge 2)         # author mutated live (add+remove)
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count)
        $r = $aChanged -and $converged
        Write-Host ("  INV-BIDIR $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorChanged=$aChanged converged=$converged" +
                    " authorFinal=(c$($aLast.count),h$($aLast.hash)) observerFinal=(c$($oLast.count),h$($oLast.hash))")
        return $r
    }
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    $hostSeq = (Select-String -Path $HostFile -Pattern 'SCENARIO INVB ADD r=0 n=[1-9]' -Quiet) -and `
               (Select-String -Path $HostFile -Pattern 'SCENARIO INVB REM r=0 n=[1-9]' -Quiet)
    $joinSeq = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVB ADD r=1 n=[1-9]' -Quiet) -and `
               (Select-String -Path $JoinFile -Pattern 'SCENARIO INVB REM r=1 n=[1-9]' -Quiet)
    $ok = $h2j -and $j2h -and $hostSeq -and $joinSeq
    Write-Host ("  INV-BIDIR " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostSeq=$hostSeq joinSeq=$joinSeq")
    return (Add-GateResult -Name "inv_bidir" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ h2j = $h2j; j2h = $j2h; hostSeq = $hostSeq; joinSeq = $joinSeq })
}

# Shared engine for inv_equip / inv_reequip (both parse SCENARIO INVE lines).
function Get-InveSeries {
    param([string]$File)
    $rx = 'SCENARIO INVE r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) eq=(\d+) hash=(\d+)'
    $arr = @()
    if (Test-Path $File) {
        foreach ($ln in Get-Content $File) {
            if ($ln -match $rx) {
                $arr += [pscustomobject]@{
                    rank = [int]$matches[1]; role = $matches[2]
                    count = [int]$matches[4]; eq = [int]$matches[5]; hash = [uint32]$matches[6]
                }
            }
        }
    }
    return ,$arr
}

# inv_equip: equipped-gear removal replicates per rank, both directions.
function Test-InventoryEquip {
    param([string]$HostFile, [string]$JoinFile)
    $H = Get-InveSeries -File $HostFile
    $J = Get-InveSeries -File $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-EQUIP FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "inv_equip" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $maxEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Maximum).Maximum }
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 1 -or $obsRows.Count -lt 1) {
            Write-Host "  INV-EQUIP $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]; $oLast = $obsRows[$obsRows.Count - 1]
        $aPeak = & $maxEq $authorRows
        $authorReduced = ($aPeak -ge 1) -and ($aLast.eq -lt $aPeak)   # a worn item was removed
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count) -and `
                     ($aLast.eq -eq $oLast.eq)
        $r = $authorReduced -and $converged
        Write-Host ("  INV-EQUIP $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorReduced=$authorReduced(peakEq$aPeak`->$($aLast.eq)) converged=$converged" +
                    " authorFinal=(c$($aLast.count),eq$($aLast.eq),h$($aLast.hash))" +
                    " observerFinal=(c$($oLast.count),eq$($oLast.eq),h$($oLast.hash))")
        return $r
    }
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    $hostSeq = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE UNEQUIP r=0 n=[1-9]' -Quiet)
    $joinSeq = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE UNEQUIP r=1 n=[1-9]' -Quiet)
    $ok = $h2j -and $j2h -and $hostSeq -and $joinSeq
    Write-Host ("  INV-EQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostSeq=$hostSeq joinSeq=$joinSeq")
    return (Add-GateResult -Name "inv_equip" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ h2j = $h2j; j2h = $j2h; hostSeq = $hostSeq; joinSeq = $joinSeq })
}

# inv_reequip (up path): unequip to loose, hold, re-equip; observer must witness
# the dip-then-restore and converge.
function Test-InventoryReequip {
    param([string]$HostFile, [string]$JoinFile)
    $H = Get-InveSeries -File $HostFile
    $J = Get-InveSeries -File $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-REEQUIP FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "inv_reequip" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $maxEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Maximum).Maximum }
    $minEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Minimum).Minimum }
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 2 -or $obsRows.Count -lt 2) {
            Write-Host "  INV-REEQUIP $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]; $oLast = $obsRows[$obsRows.Count - 1]
        $aPeak = & $maxEq $authorRows; $aDip = & $minEq $authorRows
        $authorRestored = ($aPeak -ge 1) -and ($aDip -lt $aPeak) -and ($aLast.eq -eq $aPeak)
        $oPeak = & $maxEq $obsRows; $oDip = & $minEq $obsRows
        $observerSawCycle = ($oDip -lt $oPeak) -and ($oLast.eq -eq $oPeak)
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count) -and `
                     ($aLast.eq -eq $oLast.eq)
        $r = $authorRestored -and $converged -and $observerSawCycle
        Write-Host ("  INV-REEQUIP $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorRestored=$authorRestored(peak$aPeak dip$aDip ->$($aLast.eq))" +
                    " observerSawCycle=$observerSawCycle(peak$oPeak dip$oDip ->$($oLast.eq))" +
                    " converged=$converged" +
                    " authorFinal=(c$($aLast.count),eq$($aLast.eq),h$($aLast.hash))" +
                    " observerFinal=(c$($oLast.count),eq$($oLast.eq),h$($oLast.hash))")
        return $r
    }
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    $hostUn = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE UNEQUIP r=0 n=[1-9]' -Quiet)
    $hostRe = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE REEQUIP r=0 n=[1-9]' -Quiet)
    $joinUn = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE UNEQUIP r=1 n=[1-9]' -Quiet)
    $joinRe = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE REEQUIP r=1 n=[1-9]' -Quiet)
    $seq = $hostUn -and $hostRe -and $joinUn -and $joinRe
    $ok = $h2j -and $j2h -and $seq
    Write-Host ("  INV-REEQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostUn=$hostUn hostRe=$hostRe joinUn=$joinUn joinRe=$joinRe")
    return (Add-GateResult -Name "inv_reequip" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ h2j = $h2j; j2h = $j2h; seq = $seq })
}

# inv_addequip (d25 fix): LOCAL reconcile - a fabricated equip must become a durable
# create-loose-then-equip. Every client that produced a verdict must pass.
function Test-AddEquip {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'ADDEQ verdict pass=(\d+) baseWorn=(-?\d+) create=(-?\d+) equip=(-?\d+) persist=(-?\d+)'
    $eval = {
        param($file, $label)
        if (-not (Test-Path $file)) { return $null }
        $line = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $line) { return $null }
        $baseWorn = [int]$line.Matches[0].Groups[2].Value
        $create   = [int]$line.Matches[0].Groups[3].Value
        $equip    = [int]$line.Matches[0].Groups[4].Value
        $persist  = [int]$line.Matches[0].Groups[5].Value
        $ok = ($baseWorn -ge 1) -and ($equip -ge $baseWorn) -and ($persist -ge $baseWorn)
        Write-Host ("  ADD-EQUIP [$label] " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                    " - baseWorn=$baseWorn create=$create equip=$equip persist=$persist" +
                    $(if ($create -lt $baseWorn) { " (create<baseWorn => equip was correctly DEFERRED)" } else { "" }))
        return $ok
    }
    $h = & $eval $HostFile "host"
    $j = & $eval $JoinFile "join"
    if ($null -eq $h -and $null -eq $j) {
        Write-Host "  ADD-EQUIP FAIL - no ADDEQ verdict line on either client"
        return (Add-GateResult -Name "add_equip" -Status FAIL -Detail "no ADDEQ verdict on either client")
    }
    $ok = $true
    if ($null -ne $h) { $ok = $ok -and $h }
    if ($null -ne $j) { $ok = $ok -and $j }
    Write-Host ("  ADD-EQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "add_equip" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ host = $h; join = $j })
}

# trade_probe (protocol-36 BASELINE, evidence not a sync-quality gate): the host
# performed three real cross-owner drags (TAKE / GIVE / WTAKE, violating the
# single-writer inventory model the way a player's direct drag does) while both
# clients sampled both squad containers. PASS = the probe EXECUTED; the value is
# the printed conservation report (dupe / loss / weapon-vanish signatures).
function Test-TradeProbe {
    param([string]$HostFile, [string]$JoinFile)
    $rx = "SCENARIO TRDP r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+) probe=(-?\d+) wpn=(-?\d+)"
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; t = [int]$matches[3]
                        count = [int]$matches[4]; probe = [int]$matches[6]; wpn = [int]$matches[7]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  TRADE-PROBE FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "trade_probe" -Status FAIL `
                    -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    # The three cross-owner drags (host log). WTAKE n=-1 = no weapon was found to move.
    $moves = @{}
    foreach ($m in @("TAKE", "GIVE", "WTAKE")) {
        $l = Select-String -Path $HostFile -Pattern "SCENARIO TRDP $m n=(-?\d+) sid='([^']*)'" |
             Select-Object -Last 1
        if ($l -and $l.Line -match "n=(-?\d+) sid='([^']*)'") {
            $moves[$m] = @{ n = [int]$matches[1]; sid = $matches[2] }
        }
    }
    $seedH = Select-String -Path $HostFile -Pattern "SCENARIO TRDP SEED r=0 n=[1-9]" -Quiet
    $seedJ = Select-String -Path $JoinFile -Pattern "SCENARIO TRDP SEED r=1 n=[1-9]" -Quiet

    # Per client: first/final probe + weapon totals across both containers, and
    # per-rank finals (where the moved items LANDED). A cross-owner move conserves
    # the per-client total; the seeds add +5 globally (host +2, join +3, same sid).
    $summar = {
        param($S)
        $first = @{}; $final = @{}
        foreach ($r in 0, 1) {
            $rows = @($S | Where-Object { $_.rank -eq $r })
            if ($rows.Count -gt 0) {
                $first[$r] = $rows[0]
                $final[$r] = $rows[$rows.Count - 1]
            }
        }
        if (-not ($first.ContainsKey(0) -and $first.ContainsKey(1))) { return $null }
        [pscustomobject]@{
            probeFirst = $first[0].probe + $first[1].probe
            probeFinal = $final[0].probe + $final[1].probe
            probeR0 = $final[0].probe; probeR1 = $final[1].probe
            wpnFirst = $first[0].wpn + $first[1].wpn
            wpnFinal = $final[0].wpn + $final[1].wpn
            wpnR0 = $final[0].wpn; wpnR1 = $final[1].wpn
        }
    }
    $hs = & $summar $H
    $js = & $summar $J
    if (-not $hs -or -not $js) {
        Write-Host "  TRADE-PROBE FAIL - a client never sampled both containers"
        return (Add-GateResult -Name "trade_probe" -Status FAIL -Detail "missing rank series")
    }
    # PER-MOVE signatures (final totals alone can lie: a TAKE-dupe and a GIVE-wipe
    # cancel arithmetically). The drags fire at fixed scenario times (TAKE 16 s /
    # GIVE 26 s / WTAKE 36 s on the host clock; both scenario clocks arm at
    # peer-ready, so join timestamps align within ~a second) - read each rank's
    # value just before each drag and compare with the settled value before the
    # NEXT drag / at the end.
    $valAt = {
        param($S, $rank, $tMax)   # last sample of `rank` strictly before tMax (0 = final)
        $rows = @($S | Where-Object { $_.rank -eq $rank -and ($tMax -le 0 -or $_.t -lt $tMax) })
        if ($rows.Count -eq 0) { return $null }
        return $rows[$rows.Count - 1]
    }
    $TAKE_MS = 16000; $GIVE_MS = 26000; $WPN_MS = 36000
    # probe-item value per (client, rank) at each boundary
    $hR0pre = (& $valAt $H 0 $TAKE_MS).probe; $hR1pre = (& $valAt $H 1 $TAKE_MS).probe
    $jR1pre = (& $valAt $J 1 $TAKE_MS).probe
    $hR0mid = (& $valAt $H 0 $GIVE_MS).probe; $hR1mid = (& $valAt $H 1 $GIVE_MS).probe
    $jR1mid = (& $valAt $J 1 $GIVE_MS).probe
    $hR0end = $hs.probeR0; $hR1end = $hs.probeR1
    $jR0end = $js.probeR0; $jR1end = $js.probeR1
    # TAKE (r1 -> r0 by the host): did it land, did the REMOVAL ever reach the owner,
    # did the owner's snapshot re-add it on the host (the dupe)?
    $takeLanded     = ($hR0mid - $hR0pre) -ge 1
    $takePropagated = ($jR1mid - $jR1pre) -le -1
    $takeReAdded    = $takeLanded -and (-not $takePropagated) -and ($hR1mid -ge $hR1pre)
    $takeSig = if ($takeReAdded) { "DUPE(re-added by owner snapshot; removal never propagated)" }
               elseif ($takeLanded -and $takePropagated) { "CLEAN" }
               elseif (-not $takeLanded) { "NO-OP(item never landed)" }
               else { "PARTIAL" }
    # GIVE (r0 -> r1 by the host): it left r0; did it ever ARRIVE on the owner, or
    # did the owner's reconcile wipe it?
    $giveSent    = ($hR0end - $hR0mid) -le -1
    $giveArrived = ($jR1end - $jR1mid) -ge 1
    $giveSig = if ($giveSent -and -not $giveArrived) { "WIPED(owner reconcile destroyed the given item)" }
               elseif ($giveSent -and $giveArrived) { "CLEAN" }
               else { "NO-OP(item never left)" }
    # WTAKE (weapon r1 -> r0): weapons cannot be fabricated on a peer, so the failure
    # mode is cross-client STATE DIVERGENCE (each client renders different bags).
    $wpnDiverged = ($hs.wpnR0 -ne $js.wpnR0) -or ($hs.wpnR1 -ne $js.wpnR1)
    $wpnSig = if ($wpnDiverged) { "DIVERGED(host r0=$($hs.wpnR0)/r1=$($hs.wpnR1) vs join r0=$($js.wpnR0)/r1=$($js.wpnR1))" }
              else { "CLEAN" }
    $mv = { param($m) if ($moves.ContainsKey($m)) { "n=$($moves[$m].n) sid='$($moves[$m].sid)'" } else { "(missing)" } }
    Write-Host "  TRADE-PROBE moves: TAKE $(& $mv 'TAKE')  GIVE $(& $mv 'GIVE')  WTAKE $(& $mv 'WTAKE')  seeds host=$seedH join=$seedJ"
    Write-Host "  TRADE-PROBE probe totals: host $($hs.probeFirst)->$($hs.probeFinal) (r0=$hR0end r1=$hR1end)  join $($js.probeFirst)->$($js.probeFinal) (r0=$jR0end r1=$jR1end)"
    Write-Host "  TRADE-PROBE wpn   totals: host $($hs.wpnFirst)->$($hs.wpnFinal) (r0=$($hs.wpnR0) r1=$($hs.wpnR1))  join $($js.wpnFirst)->$($js.wpnFinal) (r0=$($js.wpnR0) r1=$($js.wpnR1))"
    Write-Host "  TRADE-PROBE TAKE : landed=$takeLanded removalPropagated=$takePropagated reAdded=$takeReAdded => $takeSig"
    Write-Host "  TRADE-PROBE GIVE : sent=$giveSent arrived=$giveArrived => $giveSig"
    Write-Host "  TRADE-PROBE WTAKE: => $wpnSig"
    Write-Host "  FINDING: trade_probe baseline - TAKE:$takeSig GIVE:$giveSig WEAPON:$wpnSig"
    # PASS = executed (all three drags fired, TAKE/GIVE actually moved something,
    # both clients sampled + seeded). The signatures above are the evidence.
    $executed = $moves.ContainsKey("TAKE") -and $moves["TAKE"].n -ge 1 -and `
                $moves.ContainsKey("GIVE") -and $moves["GIVE"].n -ge 1 -and `
                $moves.ContainsKey("WTAKE") -and $seedH -and $seedJ
    $v = if ($executed) { "PASS" } else { "FAIL" }
    Write-Host "  TRADE-PROBE $v - executed=$executed"
    return (Add-GateResult -Name "trade_probe" -Status $v -Metrics @{
        hostProbe = "$($hs.probeFirst)->$($hs.probeFinal)"; joinProbe = "$($js.probeFirst)->$($js.probeFinal)"
        hostWpn = "$($hs.wpnFirst)->$($hs.wpnFinal)"; joinWpn = "$($js.wpnFirst)->$($js.wpnFinal)"
        takeSig = $takeSig; giveSig = $giveSig; wpnSig = $wpnSig })
}

# trade_peer (protocol 37 VALIDATION): the trade_probe drags rerun with the
# transfer-intent channel live. GATES that every baselined failure signature is
# closed: TAKE lands AND its removal reaches the owner (no dupe), GIVE arrives on
# the owner (no wipe), the traded WEAPON is conserved per client AND both clients
# agree on the final per-rank state (no divergence/vanish), and the channel
# actually carried it ([xfer] SEND on the dragger, [xfer] APPLY on the peer).
function Test-TradePeer {
    param([string]$HostFile, [string]$JoinFile)
    $rx = "SCENARIO TRDE r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+) probe=(-?\d+) wpn=(-?\d+) arm=(-?\d+) aq=(-?\d+) alv=(-?\d+)"
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; t = [int]$matches[3]
                        count = [int]$matches[4]; probe = [int]$matches[6]; wpn = [int]$matches[7]
                        # arm = tracked-armour count; aq = its wire quality bucket;
                        # alv = its getLevel() craft level (both -1 when absent here).
                        arm = [int]$matches[8]; aq = [int]$matches[9]; alv = [int]$matches[10]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  TRADE-PEER FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "trade_peer" -Status FAIL `
                    -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    # The four cross-owner drags must have MOVED something locally on the host.
    $moves = @{}
    foreach ($m in @("TAKE", "GIVE", "WTAKE", "ATAKE")) {
        $l = Select-String -Path $HostFile -Pattern "SCENARIO TRDE $m n=(-?\d+) sid='([^']*)'" |
             Select-Object -Last 1
        if ($l -and $l.Line -match "n=(-?\d+) sid='([^']*)'") {
            $moves[$m] = @{ n = [int]$matches[1]; sid = $matches[2] }
        }
    }
    $seedH = Select-String -Path $HostFile -Pattern "SCENARIO TRDE SEED r=0 n=[1-9]" -Quiet
    $seedJ = Select-String -Path $JoinFile -Pattern "SCENARIO TRDE SEED r=1 n=[1-9]" -Quiet
    # The channel evidence: the dragger authored intents, the peer applied them.
    # Host-committed two-phase wire (Phase 7, protocol 58): the receiver logs
    # "[xfer] APPLY-COMMIT id=..." (not the pre-Phase-7 "APPLY id=" this regex
    # used to match), and the host's audit-only ACK echo is
    # "[xfer] COMMIT-ACK id=.. from=.. applied=.. verdict=<XferCommitOutcome>"
    # (a numeric outcome code, not the old word-based verdict=accept/reject/
    # partial + waitedMs=.. fraction format - that pre-host-arbitration shape
    # no longer exists on the wire). Updated to match ReplicatorItems.cpp's
    # current log strings verbatim (was silently reading applies=0/acked=0
    # against a channel that was actually working - trade_peer FAILed on
    # channel evidence alone while every downstream signal was CLEAN).
    $sends   = @(Select-String -Path $HostFile -Pattern "\[xfer\] SEND id=").Count
    $applies = @(Select-String -Path $JoinFile -Pattern "\[xfer\] APPLY-COMMIT id=").Count
    # XFER_COMMIT_REJECT = 2 (Wire.h). These drags are all legal cross-owner
    # moves of an item the source really holds, so the only correct outcome is
    # RELOCATE(0) or FABRICATE(1); a REJECT(2) here means the host could not
    # find what the author swore it had moved.
    $acks     = @(Select-String -Path $HostFile -Pattern "\[xfer\] COMMIT-ACK id=")
    $ackIds   = @{}
    $rejects  = 0; $partials = 0; $maxWait = 0; $ackMissing = 0
    foreach ($a in $acks) {
        if ($a.Line -match "\[xfer\] COMMIT-ACK id=(\d+) from=\d+ applied=(\d+) verdict=(\d+)") {
            $ackIds[[int]$matches[1]] = $true
            if ([int]$matches[3] -eq 2) { $rejects++ } # XFER_COMMIT_REJECT
        }
    }
    $ackOk = ($ackIds.Count -ge $sends) -and ($rejects -eq 0)

    $summar = {
        param($S)
        $first = @{}; $final = @{}
        foreach ($r in 0, 1) {
            $rows = @($S | Where-Object { $_.rank -eq $r })
            if ($rows.Count -gt 0) {
                $first[$r] = $rows[0]
                $final[$r] = $rows[$rows.Count - 1]
            }
        }
        if (-not ($first.ContainsKey(0) -and $first.ContainsKey(1))) { return $null }
        $armPre   = @($S | Where-Object { $_.rank -eq 1 -and $_.alv -ge 0 })
        $armPost  = @($S | Where-Object { $_.rank -eq 0 -and $_.alv -ge 0 })
        $armR0Rows = @($S | Where-Object { $_.rank -eq 0 -and $_.arm -gt 0 })
        $armR1Rows = @($S | Where-Object { $_.rank -eq 1 -and $_.arm -gt 0 })
        [pscustomobject]@{
            probeFirst = $first[0].probe + $first[1].probe
            probeFinal = $final[0].probe + $final[1].probe
            probeR0 = $final[0].probe; probeR1 = $final[1].probe
            wpnFirst = $first[0].wpn + $first[1].wpn
            wpnFinal = $final[0].wpn + $final[1].wpn
            wpnR0 = $final[0].wpn; wpnR1 = $final[1].wpn
            # The armour is MINTED mid-run, so its baseline is the first sample in which it
            # exists, not the series' first row (which predates the mint and would score
            # the mint itself as a duplication). Peaks are per rank, which is enough to
            # catch a dupe without pairing the two ranks' samples by timestamp.
            armFinal = $final[0].arm + $final[1].arm
            armR0 = $final[0].arm; armR1 = $final[1].arm
            armPeakR0 = $(if ($armR0Rows.Count -gt 0) { ($armR0Rows | Measure-Object -Property arm -Maximum).Maximum } else { 0 })
            armPeakR1 = $(if ($armR1Rows.Count -gt 0) { ($armR1Rows | Measure-Object -Property arm -Maximum).Maximum } else { 0 })
            armEverSeen = ($armR0Rows.Count + $armR1Rows.Count) -gt 0
            # The tracked armour's grade BEFORE the drag is read from rank 1 (where it
            # starts) and AFTER from rank 0 (where it lands). alv = -1 means "this
            # container does not hold the piece", so those samples say nothing about its
            # grade and are excluded rather than counted as a demotion.
            armPreLvl  = $(if ($armPre.Count  -gt 0) { $armPre[0].alv }  else { -1 })
            armPreQ    = $(if ($armPre.Count  -gt 0) { $armPre[0].aq }   else { -1 })
            armPostLvl = $(if ($armPost.Count -gt 0) { $armPost[$armPost.Count - 1].alv } else { -1 })
            armPostQ   = $(if ($armPost.Count -gt 0) { $armPost[$armPost.Count - 1].aq }  else { -1 })
        }
    }
    $hs = & $summar $H
    $js = & $summar $J
    if (-not $hs -or -not $js) {
        Write-Host "  TRADE-PEER FAIL - a client never sampled both containers"
        return (Add-GateResult -Name "trade_peer" -Status FAIL -Detail "missing rank series")
    }
    # Same per-move boundaries as the baseline oracle (drag times are shared).
    $valAt = {
        param($S, $rank, $tMax)   # last sample of `rank` strictly before tMax (0 = final)
        $rows = @($S | Where-Object { $_.rank -eq $rank -and ($tMax -le 0 -or $_.t -lt $tMax) })
        if ($rows.Count -eq 0) { return $null }
        return $rows[$rows.Count - 1]
    }
    $TAKE_MS = 16000; $GIVE_MS = 26000
    $hR0pre = (& $valAt $H 0 $TAKE_MS).probe; $hR1pre = (& $valAt $H 1 $TAKE_MS).probe
    $jR1pre = (& $valAt $J 1 $TAKE_MS).probe
    $hR0mid = (& $valAt $H 0 $GIVE_MS).probe; $hR1mid = (& $valAt $H 1 $GIVE_MS).probe
    $jR1mid = (& $valAt $J 1 $GIVE_MS).probe
    $hR0end = $hs.probeR0; $hR1end = $hs.probeR1
    $jR0end = $js.probeR0; $jR1end = $js.probeR1
    # TAKE (r1 -> r0): landed on the dragger AND the removal reached the owner AND
    # no re-add on the dragger's r1 (the dupe window).
    $takeLanded     = ($hR0mid - $hR0pre) -ge 1
    $takePropagated = ($jR1mid - $jR1pre) -le -1
    $takeNoDupe     = ($hR1mid - $hR1pre) -le -1   # dragger's r1 stayed down (no re-add)
    $takeOk  = $takeLanded -and $takePropagated -and $takeNoDupe
    $takeSig = if ($takeOk) { "CLEAN" }
               elseif (-not $takeLanded) { "NO-OP(item never landed)" }
               elseif (-not $takePropagated) { "DUPE(removal never reached the owner)" }
               else { "DUPE(re-added on the dragger)" }
    # GIVE (r0 -> r1): left the dragger's r0 AND arrived on the owner (no wipe).
    $giveSent    = ($hR0end - $hR0mid) -le -1
    $giveArrived = ($jR1end - $jR1mid) -ge 1
    $giveKeptH   = ($hR1end - $hR1mid) -ge 1       # the dragger still renders it in r1
    $giveOk  = $giveSent -and $giveArrived -and $giveKeptH
    $giveSig = if ($giveOk) { "CLEAN" }
               elseif (-not $giveSent) { "NO-OP(item never left)" }
               elseif (-not $giveArrived) { "WIPED(never arrived on the owner)" }
               else { "WIPED(reconciled away on the dragger)" }
    # WTAKE: conservation per client + cross-client agreement + it actually moved.
    $wpnConsH  = $hs.wpnFinal -eq $hs.wpnFirst
    $wpnConsJ  = $js.wpnFinal -eq $js.wpnFirst
    $wpnAgree  = ($hs.wpnR0 -eq $js.wpnR0) -and ($hs.wpnR1 -eq $js.wpnR1)
    $wpnMoved  = $hs.wpnR0 -ge 1
    $wpnOk  = $wpnConsH -and $wpnConsJ -and $wpnAgree -and $wpnMoved
    $wpnSig = if ($wpnOk) { "CLEAN" }
              elseif (-not ($wpnConsH -and $wpnConsJ)) { "VANISH(host $($hs.wpnFirst)->$($hs.wpnFinal) join $($js.wpnFirst)->$($js.wpnFinal))" }
              elseif (-not $wpnAgree) { "DIVERGED(host r0=$($hs.wpnR0)/r1=$($hs.wpnR1) vs join r0=$($js.wpnR0)/r1=$($js.wpnR1))" }
              else { "NO-MOVE(weapon never landed in r0)" }
    # ATAKE: exactly ONE reference piece is minted, so at REST each client must hold it once,
    # in rank 0. Judged on the SETTLED state - the same bar as the WTAKE gate beside it, and
    # deliberately so: the receiver transiently over-counts EVERY leg of this scenario for
    # ~14 s after a drag (the real item is relocated while a copy is fabricated from the
    # owner's still-stale snapshot, which reconcile then removes). The weapon leg peaks at 3
    # and the probe leg at 4 in exactly the same window, so that transient is pre-existing
    # and unrelated to grades; gating on peaks here would fail this scenario for a different
    # defect. It is surfaced as a FINDING below rather than silently dropped.
    $armConsH = ($hs.armFinal -eq 1)
    $armConsJ = ($js.armFinal -eq 1)
    $armAgree = ($hs.armR0 -eq $js.armR0) -and ($hs.armR1 -eq $js.armR1)
    $armMoved = ($hs.armR0 -ge 1) -and ($hs.armR1 -eq 0)
    $armSeen  = $hs.armEverSeen -and $js.armEverSeen
    $armOk  = $armSeen -and $armConsH -and $armConsJ -and $armAgree -and $armMoved
    $armSig = if ($armOk) { "CLEAN" }
              elseif (-not $armSeen) { "ABSENT(the seeded armour never reached both clients)" }
              elseif (-not ($armConsH -and $armConsJ)) { "NOT-CONSERVED(host final=$($hs.armFinal) join final=$($js.armFinal) - expected exactly 1 each)" }
              elseif (-not $armAgree) { "DIVERGED(host r0=$($hs.armR0)/r1=$($hs.armR1) vs join r0=$($js.armR0)/r1=$($js.armR1))" }
              else { "NO-MOVE(armour never landed in r0)" }
    $armPeak = [Math]::Max([Math]::Max($hs.armPeakR0, $hs.armPeakR1),
                           [Math]::Max($js.armPeakR0, $js.armPeakR1))
    # THE GRADE GATE. Kenshi's named grades (Prototype 5 ... Masterwork 95) are points on a
    # 1-100 craft level, so the number that must survive is alv (getLevel()), NOT aq (the
    # Item::quality bucket the wire carries and the fabricate path writes back - it agrees
    # across clients by construction, which is why weapon_loot's qual= assertion never
    # caught a demoted item).
    #
    # The REFERENCE is the level the join's own mint actually produced, read from its seed
    # line rather than hardcoded here - that keeps the expected value owned by the scenario
    # and, more importantly, states the assumption the whole fix rests on as its own gate:
    # if the engine ignored levelOverride, mintHonoured fails and the run says so instead of
    # blaming the sync.
    $LVL_TOL = 2      # levels; getLevel() is integral, so this only absorbs rounding
    $Q_TOL   = 5      # buckets; the tolerance Test-WeaponLoot already uses for quality
    $seedRx = "SCENARIO TRDE arm seeded ok=(-?\d+) sid='([^']*)' wantLvl=(-?\d+) gotLvl=(-?\d+) gotQ=(-?\d+)"
    $seedLine = Select-String -Path $JoinFile -Pattern $seedRx | Select-Object -Last 1
    $seedOk = $false; $wantLvl = -1; $refLvl = -1; $refQ = -1
    if ($seedLine -and ($seedLine.Line -match $seedRx)) {
        $seedOk  = ([int]$matches[1] -eq 1)
        $wantLvl = [int]$matches[3]
        $refLvl  = [int]$matches[4]
        $refQ    = [int]$matches[5]
    }
    $mintHonoured = $seedOk -and ($refLvl -ge 0) -and ([Math]::Abs($refLvl - $wantLvl) -le $LVL_TOL)
    $gradeKnown = $mintHonoured -and ($hs.armPostLvl -ge 0) -and ($js.armPostLvl -ge 0)
    $gradeAgree = $gradeKnown -and ([Math]::Abs($hs.armPostLvl - $js.armPostLvl) -le $LVL_TOL)
    # Against the reference on BOTH sides: the join must not lose the grade of the piece it
    # minted, and the host's copy - which it could only get by fabricating from the wire -
    # must carry the same grade.
    $gradeKept  = $gradeKnown -and ([Math]::Abs($js.armPostLvl - $refLvl) -le $LVL_TOL) -and
                                   ([Math]::Abs($hs.armPostLvl - $refLvl) -le $LVL_TOL)
    $qAgree     = $gradeKnown -and ([Math]::Abs($hs.armPostQ - $js.armPostQ) -le $Q_TOL)
    $gradeOk    = $gradeAgree -and $gradeKept -and $qAgree
    $gradeSig = if ($gradeOk) { "CLEAN" }
                elseif (-not $seedOk) { "NO-SEED(the join never minted the reference armour)" }
                elseif (-not $mintHonoured) { "MINT-IGNORED(asked for lvl=$wantLvl, engine produced $refLvl - levelOverride is not the craft level)" }
                elseif (-not $gradeKnown) { "UNREAD(the piece was never readable in r0 on both clients)" }
                elseif (-not $gradeKept) { "DEMOTED(reference alv=$refLvl -> host $($hs.armPostLvl) join $($js.armPostLvl))" }
                elseif (-not $gradeAgree) { "DIVERGED(host alv=$($hs.armPostLvl) vs join alv=$($js.armPostLvl))" }
                else { "QUALITY-DIVERGED(host aq=$($hs.armPostQ) vs join aq=$($js.armPostQ))" }
    # Probe-item conservation: cross-owner moves conserve; the seeds add +5 globally.
    $consH = $hs.probeFinal -eq ($hs.probeFirst + 5)
    $consJ = $js.probeFinal -eq ($js.probeFirst + 5)
    $probeAgree = ($hR0end -eq $jR0end) -and ($hR1end -eq $jR1end)
    $mv = { param($m) if ($moves.ContainsKey($m)) { "n=$($moves[$m].n) sid='$($moves[$m].sid)'" } else { "(missing)" } }
    Write-Host "  TRADE-PEER moves: TAKE $(& $mv 'TAKE')  GIVE $(& $mv 'GIVE')  WTAKE $(& $mv 'WTAKE')  ATAKE $(& $mv 'ATAKE')  seeds host=$seedH join=$seedJ  xfer sent=$sends applied=$applies acked=$($ackIds.Count) (reject=$rejects partial=$partials missing=$ackMissing slowestMs=$maxWait)"
    Write-Host "  TRADE-PEER probe totals: host $($hs.probeFirst)->$($hs.probeFinal) (r0=$hR0end r1=$hR1end)  join $($js.probeFirst)->$($js.probeFinal) (r0=$jR0end r1=$jR1end)  conserve host=$consH join=$consJ agree=$probeAgree"
    Write-Host "  TRADE-PEER wpn   totals: host $($hs.wpnFirst)->$($hs.wpnFinal) (r0=$($hs.wpnR0) r1=$($hs.wpnR1))  join $($js.wpnFirst)->$($js.wpnFinal) (r0=$($js.wpnR0) r1=$($js.wpnR1))"
    Write-Host "  TRADE-PEER arm   totals: host final=$($hs.armFinal) (r0=$($hs.armR0) r1=$($hs.armR1) peak r0/r1=$($hs.armPeakR0)/$($hs.armPeakR1))  join final=$($js.armFinal) (r0=$($js.armR0) r1=$($js.armR1) peak r0/r1=$($js.armPeakR0)/$($js.armPeakR1))"
    Write-Host "  TRADE-PEER arm   grade: minted lvl=$refLvl (want $wantLvl) q=$refQ  pre-drag host alv=$($hs.armPreLvl) join alv=$($js.armPreLvl)  post-drag host alv=$($hs.armPostLvl) aq=$($hs.armPostQ)  join alv=$($js.armPostLvl) aq=$($js.armPostQ)"
    Write-Host "  TRADE-PEER TAKE : landed=$takeLanded removalPropagated=$takePropagated noDupe=$takeNoDupe => $takeSig"
    Write-Host "  TRADE-PEER GIVE : sent=$giveSent arrived=$giveArrived keptOnDragger=$giveKeptH => $giveSig"
    Write-Host "  TRADE-PEER WTAKE: conserveH=$wpnConsH conserveJ=$wpnConsJ agree=$wpnAgree moved=$wpnMoved => $wpnSig"
    Write-Host "  TRADE-PEER ATAKE: conserveH=$armConsH conserveJ=$armConsJ agree=$armAgree moved=$armMoved => $armSig"
    if ($armPeak -gt 1) {
        Write-Host "  FINDING: the traded armour was transiently DOUBLED on a client (peak=$armPeak) before reconcile settled it to 1 - the same post-drag transient the weapon and probe legs show, so not grade-related; advisory here, gated on the settled state"
    }
    Write-Host "  TRADE-PEER GRADE: seeded=$seedOk mintHonoured=$mintHonoured known=$gradeKnown agree=$gradeAgree kept=$gradeKept qualAgree=$qAgree => $gradeSig"
    if (-not $gradeKnown) {
        Write-Host "    NOTE: the grade gate needs the join's minted Masterwork armour to reach rank 0 on both clients; without it a demotion cannot be observed"
    }
    $executed = $moves.ContainsKey("TAKE") -and $moves["TAKE"].n -ge 1 -and `
                $moves.ContainsKey("GIVE") -and $moves["GIVE"].n -ge 1 -and `
                $moves.ContainsKey("WTAKE") -and $moves["WTAKE"].n -ge 1 -and `
                $moves.ContainsKey("ATAKE") -and $moves["ATAKE"].n -ge 1 -and `
                $seedH -and $seedJ
    $channel = ($sends -ge 4) -and ($applies -ge 4) -and $ackOk
    $ok = $executed -and $channel -and $takeOk -and $giveOk -and $wpnOk -and `
          $armOk -and $gradeOk -and $consH -and $consJ -and $probeAgree
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  TRADE-PEER $v - executed=$executed channel=$channel (ack=$ackOk) take=$takeOk give=$giveOk wpn=$wpnOk arm=$armOk grade=$gradeOk conserve=$($consH -and $consJ) agree=$probeAgree"
    return (Add-GateResult -Name "trade_peer" -Status $v -Metrics @{
        hostProbe = "$($hs.probeFirst)->$($hs.probeFinal)"; joinProbe = "$($js.probeFirst)->$($js.probeFinal)"
        hostWpn = "$($hs.wpnFirst)->$($hs.wpnFinal)"; joinWpn = "$($js.wpnFirst)->$($js.wpnFinal)"
        hostArm = "final=$($hs.armFinal) peak=$($hs.armPeakR0)/$($hs.armPeakR1)"
        joinArm = "final=$($js.armFinal) peak=$($js.armPeakR0)/$($js.armPeakR1)"
        armRefLvl = $refLvl; armHostLvl = $hs.armPostLvl; armJoinLvl = $js.armPostLvl
        armRefQ = $refQ; armHostQ = $hs.armPostQ; armJoinQ = $js.armPostQ
        sends = $sends; applies = $applies; acked = $ackIds.Count
        ackReject = $rejects; ackPartial = $partials; ackMissing = $ackMissing
        ackSlowestMs = $maxWait
        takeSig = $takeSig; giveSig = $giveSig; wpnSig = $wpnSig
        armSig = $armSig; gradeSig = $gradeSig })
}

# drop_probe (W0 diagnostic): assert the probe EXECUTED and surface the evidence.
function Test-DropProbe {
    param([string]$HostFile)
    if (-not (Test-Path $HostFile)) {
        Write-Host "  DROP-PROBE FAIL - no host log"
        return (Add-GateResult -Name "drop_probe" -Status FAIL -Detail "no host log")
    }
    $rx = 'SCENARIO DROP RESULT dropped=(-?\d+) before=(-?\d+) after=(-?\d+) enumerated=(\d+)'
    $line = Select-String -Path $HostFile -Pattern $rx | Select-Object -Last 1
    if (-not $line) {
        Write-Host "  DROP-PROBE FAIL - no 'SCENARIO DROP RESULT' evidence line"
        return (Add-GateResult -Name "drop_probe" -Status FAIL -Detail "no RESULT line")
    }
    $null = ($line.Line -match $rx)
    $dropped    = [int]$matches[1]
    $before     = [int]$matches[2]
    $after      = [int]$matches[3]
    $enumerated = [int]$matches[4]
    $seeded = Select-String -Path $HostFile -Pattern 'SCENARIO DROP SEEDED added=\d+ sid=' | Select-Object -Last 1
    $ok = ($dropped -gt 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  DROP-PROBE $v - dropped=$dropped before=$before after=$after enumerated=$enumerated")
    if ($seeded) { Write-Host ("  DROP-PROBE   " + $seeded.Line.Trim()) }
    $afterScan = $false
    foreach ($ln in Get-Content $HostFile) {
        if ($ln -match 'SCENARIO DROP AFTER-scan:') { $afterScan = $true; continue }
        if ($afterScan) {
            if ($ln -match 'WORLDITEM (scan|  )') { Write-Host ("  DROP-PROBE   " + ($ln -replace '^.*WORLDITEM', 'WORLDITEM').Trim()) }
            elseif ($ln -match 'SCENARIO DROP RESULT') { break }
        }
    }
    return (Add-GateResult -Name "drop_probe" -Status $v -Metrics @{ dropped = $dropped; before = $before; after = $after; enumerated = $enumerated })
}

# world_item_sync (W1): drop streams to the join (proxy spawn), despawn culls it.
function Test-WorldItemSync {
    # -JoinAuthor: the world_item_join variant (W1 bidir) - the JOIN drops/despawns
    # and the HOST must spawn/cull the proxy. Same series logic, roles swapped;
    # $GateName labels the verdict (wi_sync / wi_join).
    param([string]$HostFile, [string]$JoinFile, [double]$Tol = 3.0,
          [switch]$JoinAuthor, [string]$GateName = "wi_sync")
    $rx = 'SCENARIO WI (HOST|JOIN) t=(\d+) n=(\d+) pos=(-?[0-9.]+),(-?[0-9.]+),(-?[0-9.]+) hash=(\d+)'
    $series = {
        param($file, $role)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx -and $matches[1] -eq $role) {
                    $arr += [pscustomobject]@{
                        n = [int]$matches[3]; x = [double]$matches[4]; y = [double]$matches[5]
                        z = [double]$matches[6]; hash = [uint32]$matches[7]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile "HOST"
    $J = & $series $JoinFile "JOIN"
    $label = if ($JoinAuthor) { "WI-JOIN" } else { "WI-SYNC" }
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  $label FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name $GateName -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $authorFile = if ($JoinAuthor) { $JoinFile } else { $HostFile }
    $drop    = [bool](Select-String -Path $authorFile -Pattern 'SCENARIO WI DROP .*dropped=[1-9]' -Quiet)
    $despawn = [bool](Select-String -Path $authorFile -Pattern 'SCENARIO WI DESPAWN destroyed=[1-9]' -Quiet)
    $hostPresent = $H | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    $joinPresent = $J | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    # The OBSERVER side seeing the item proves the proxy spawned (the author's own
    # sighting is just its real local item).
    $observerSpawned = if ($JoinAuthor) { $hostPresent -ne $null } else { $joinPresent -ne $null }
    $authorSaw       = if ($JoinAuthor) { $joinPresent -ne $null } else { $hostPresent -ne $null }
    $posMatch = $false; $hashMatch = $false; $dxz = -1.0
    if ($hostPresent -and $joinPresent) {
        $dx = $hostPresent.x - $joinPresent.x; $dz = $hostPresent.z - $joinPresent.z
        $dxz = [math]::Sqrt($dx * $dx + $dz * $dz)
        $posMatch  = ($dxz -le $Tol)
        $hashMatch = ($hostPresent.hash -eq $joinPresent.hash)
    }
    $hostCulled = ($hostPresent -ne $null) -and ($H[$H.Count - 1].n -eq 0)
    $joinCulled = ($joinPresent -ne $null) -and ($J[$J.Count - 1].n -eq 0)
    $ok = $drop -and $despawn -and $authorSaw -and $observerSpawned -and $posMatch -and $hashMatch -and $joinCulled -and $hostCulled
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  $label $v - drop=$drop despawn=$despawn observerSpawned=$observerSpawned " +
                "posMatch=$posMatch (dXZ=$([math]::Round($dxz,2))u tol=$Tol) hashMatch=$hashMatch " +
                "hostCulled=$hostCulled joinCulled=$joinCulled")
    if ($hostPresent -and $joinPresent) {
        Write-Host ("  $label   host item pos=($($hostPresent.x),$($hostPresent.y),$($hostPresent.z)) hash=$($hostPresent.hash)")
        Write-Host ("  $label   join item pos=($($joinPresent.x),$($joinPresent.y),$($joinPresent.z)) hash=$($joinPresent.hash)")
    }
    return (Add-GateResult -Name $GateName -Status $v -Metrics @{
        drop = $drop; despawn = $despawn; observerSpawned = $observerSpawned
        posMatch = $posMatch; dxz = [math]::Round($dxz, 2); hashMatch = $hashMatch
        hostCulled = $hostCulled; joinCulled = $joinCulled })
}

# rejoin_items (Phase 3 item-dup fix): a reload must not duplicate save-native
# ground items. The HOST drops K items (both clients reach n0+K), coordinated-
# saves so the drops bake into the shared save, then loads it mid-session. The
# first-scan baseline must record the now-native drops as never-emit; WITHOUT it
# the host re-streams them and the join layers a duplicate proxy per reload. The
# gate: POST-reload ground-item count must NOT grow past the PRE-reload count on
# EITHER side (equal is the fixed behavior), a reload edge actually happened, and
# the host authored the drop + coordinated save + mid-session load.
function Test-RejoinItems {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    $parse = {
        param($file)
        $o = [pscustomobject]@{ base = -1; pre = -1; post = -1; postSeen = $false
                                swapDone = $false; reload = $false; verdictPass = $null }
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match 'SCENARIO RI BASELINE n=(-?\d+)') { $o.base = [int]$matches[1] }
                if ($ln -match 'SCENARIO RI PRERELOAD n=(-?\d+)') { $o.pre = [int]$matches[1] }
                if ($ln -match 'SCENARIO RI POSTRELOAD n=(-?\d+) pre=(-?\d+)') {
                    $o.post = [int]$matches[1]
                    if ($o.pre -lt 0) { $o.pre = [int]$matches[2] }
                    $o.postSeen = $true
                }
                if ($ln -match 'SCENARIO RI SWAPDONE')     { $o.swapDone = $true }
                if ($ln -match '\[load\] WORLD-RELOAD')    { $o.reload = $true }
                if ($ln -match 'SCENARIO RI verdict .*pass=(\d+)') { $o.verdictPass = ([int]$matches[1] -eq 1) }
            }
        }
        return $o
    }
    $H = & $parse $HostFile
    $J = & $parse $JoinFile

    # Host authoring legs.
    $drop = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO RI DROP .*dropped=[1-9]' -Quiet -ErrorAction SilentlyContinue)
    $save = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO RI SAVE .*ok=1' -Quiet -ErrorAction SilentlyContinue)
    $load = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO RI LOAD .*ok=1' -Quiet -ErrorAction SilentlyContinue)
    if (-not $drop) { $why += "host never dropped test items (no 'SCENARIO RI DROP dropped>=1')" }
    if (-not $save) { $why += "host coordinated save failed (no 'SCENARIO RI SAVE ok=1')" }
    if (-not $load) { $why += "host mid-session load failed (no 'SCENARIO RI LOAD ok=1')" }

    # The drops must have actually entered the world+save (host grew over its own
    # baseline), otherwise the test never exercised the item-dup path.
    if ($H.base -ge 0 -and $H.pre -ge 0 -and $H.pre -le $H.base) {
        $why += "host drops never registered (baseline=$($H.base) >= preReload=$($H.pre) - nothing to duplicate)"
    }

    # A reload edge must have happened on BOTH sides (the coordinated load must
    # drive the join, not just the host).
    if (-not ($H.reload -or $H.swapDone)) { $why += "host saw no reload edge (no WORLD-RELOAD / RI SWAPDONE)" }
    if (-not ($J.reload -or $J.swapDone)) { $why += "join saw no reload edge (coordinated load did not drive the join)" }

    if (-not $H.postSeen) { $why += "host missing POSTRELOAD census" }
    if (-not $J.postSeen) { $why += "join missing POSTRELOAD census" }

    # THE gate (cross-client parity): both clients loaded the byte-identical save,
    # so once co-located after the swap they must converge to the SAME native
    # count. A join that mints a proxy on top of each save-native (the reload dup
    # bug) shows a count that EXCEEDS the host's authoritative native count. So a
    # join-post GREATER than host-post = duplication. (join-pre is NOT a valid
    # baseline: the drops land near the host leader, often outside the join's 60u
    # interest sphere pre-reload, so the join legitimately reaches the full count
    # only after the reload co-locates it with the saved items.)
    if ($H.postSeen -and $J.postSeen -and $H.post -ge 0 -and $J.post -gt $H.post) {
        $why += "join DUPLICATED items on reload (join post=$($J.post) EXCEEDS host post=$($H.post) - a proxy minted on top of each save-native)"
    }
    # Host must not balloon against its own pre-reload count either.
    if ($H.postSeen -and $H.pre -ge 0 -and $H.post -gt $H.pre) {
        $why += "host DUPLICATED items on reload (pre=$($H.pre) -> post=$($H.post))"
    }

    Write-Host ("    FINDING: host base={0} pre={1} post={2} reload={3} | join pre={4} post={5} reload={6} | parity(join<=host)={7} | drop={8} save={9} load={10}" -f `
        $H.base, $H.pre, $H.post, ($H.reload -or $H.swapDone), $J.pre, $J.post, ($J.reload -or $J.swapDone), `
        ($J.post -le $H.post), $drop, $save, $load)

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  REJOIN-ITEMS $v - $detail"
    return (Add-GateResult -Name "rejoin_items" -Status $v -Metrics @{
        hostBase = $H.base; hostPre = $H.pre; hostPost = $H.post
        joinPre = $J.pre; joinPost = $J.post
        drop = $drop; save = $save; load = $load } -Detail $detail)
}

# wpn_relocate (conservation spike): LOCAL single-client bag->ground->bag move of a
# real weapon. Gated on the host; the join result is advisory cross-client evidence.
function Test-WpnRelocate {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'RELOC verdict pass=(\d+) invBase=(-?\d+) drop\(inv=(-?\d+) grnd=(-?\d+) held=(-?\d+)\) pick\(inv=(-?\d+) grnd=(-?\d+) persist=(-?\d+)\)'
    $eval = {
        param($file, $label)
        if (-not (Test-Path $file)) { return $null }
        $line = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $line) { return $null }
        $g = $line.Matches[0].Groups
        $pass = [int]$g[1].Value; $invBase = [int]$g[2].Value
        $dInv = [int]$g[3].Value; $dGrnd = [int]$g[4].Value; $held = [int]$g[5].Value
        $pInv = [int]$g[6].Value; $pGrnd = [int]$g[7].Value; $persist = [int]$g[8].Value
        $dropOk   = ($invBase -ge 1) -and ($dInv -le $invBase - 1) -and ($dGrnd -ge 1)
        $dropHeld = ($held -ge 1)
        $pickOk   = ($pInv -ge $invBase) -and ($pGrnd -lt $dGrnd)
        $pickHeld = ($persist -ge $invBase)
        $ok = ($pass -eq 1) -and $dropOk -and $dropHeld -and $pickOk -and $pickHeld
        Write-Host ("  WPN-RELOCATE [$label] " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                    " - invBase=$invBase drop(inv=$dInv grnd=$dGrnd held=$held) pick(inv=$pInv grnd=$pGrnd persist=$persist)")
        if ($ok) { Write-Host "    conservation OK: weapon moved bag->ground->bag as a REAL object (no createItem)" }
        return $ok
    }
    $h = & $eval $HostFile "host"
    $j = & $eval $JoinFile "join"
    if ($null -eq $h) {
        Write-Host "  WPN-RELOCATE FAIL - no RELOC verdict line on the host (authoritative)"
        return (Add-GateResult -Name "wpn_relocate" -Status FAIL -Detail "no host RELOC verdict")
    }
    if ($null -ne $j) {
        Write-Host ("  WPN-RELOCATE [join] advisory cross-client result: " + $(if ($j) { "consistent" } else { "perturbed (host reconcile timing)" }))
    }
    Write-Host ("  WPN-RELOCATE " + $(if ($h) { "PASS" } else { "FAIL" }) + " (gated on host)")
    return (Add-GateResult -Name "wpn_relocate" -Status $(if ($h) { "PASS" } else { "FAIL" }) `
                -Metrics @{ host = $h; joinAdvisory = $j })
}

# world_weapon_drop / world_armor_drop (W2): host drops gear; join relocates its own
# copy to the ground. Both scenario variants emit the same WDROP log contract, so one
# oracle serves both - $GateName picks the verdict label (weapon_drop / armor_drop).
function Test-WeaponDrop {
    param([string]$HostFile, [string]$JoinFile, [string]$GateName = "weapon_drop")
    $tag = $GateName.ToUpper().Replace("_", "-")   # WEAPON-DROP / ARMOR-DROP
    $rxHost = 'WDROP verdict role=host pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invAfter=(-?\d+) grndAfter=(-?\d+)'
    $rxJoin = 'WDROP verdict role=join pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invMin=(-?\d+) grndMax=(-?\d+) relocated=(\d+)'
    $hostOk = $false
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invAfter = [int]$g[4].Value; $grnd = [int]$g[5].Value
            $hostOk = ($invBase -ge 1) -and ($invAfter -le $invBase - 1) -and ($grnd -ge 1)
            Write-Host ("  $tag [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - dropped gear to ground (invBase=$invBase invAfter=$invAfter ground=$grnd)")
        } else { Write-Host "  $tag [host] FAIL - no host WDROP verdict" }
    }
    $joinOk = $false
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invMin = [int]$g[4].Value; $grndMax = [int]$g[5].Value
            $joinOk = ($invBase -ge 1) -and ($invMin -le $invBase - 1) -and ($grndMax -ge 1)
            Write-Host ("  $tag [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - relocated own copy to ground (invBase=$invBase invMin=$invMin grndMax=$grndMax)")
            if (-not $joinOk -and $invMin -le $invBase - 1 -and $grndMax -lt 1) {
                Write-Host "    NOTE: weapon LEFT the bag but never appeared on the ground => destroyed by inv-reconcile, not conserved"
            }
        } else { Write-Host "  $tag [join] FAIL - no join WDROP verdict" }
    }
    $authored = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] DROP id=' -Quiet)
    $applied  = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[wd\] APPLY id=\d+ .* moved=1' -Quiet)
    Write-Host ("  $tag trace: host authored DROP=$authored, join APPLY moved=1=$applied")
    $ok = $hostOk -and $joinOk
    Write-Host ("  $tag " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name $GateName -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; authored = $authored; applied = $applied })
}

# world_pickup_mirror (protocol 47): the W1 CLAIM half. The host drops a non-gear item and
# the join consumes the resulting proxy; the host's OWN ground copy must then be destroyed.
# grndPeak>=1 proves the drop landed at all (so grndFinal==0 cannot pass vacuously), and
# grndFinal==0 is the fix: before the claim existed the author kept its copy forever and the
# item was duplicated - the "join picked it up but the host still sees it there" report.
function Test-WorldPickupMirror {
    param([string]$HostFile, [string]$JoinFile)
    $rxHost = 'WPMIRROR verdict role=host pass=(\d+) sid=''([^'']*)'' dropped=(-?\d+) grndPeak=(\d+) grndFinal=(-?\d+)'
    $rxJoin = 'WPMIRROR verdict role=join pass=(\d+) sid=''([^'']*)'' pickedUp=(-?\d+)'
    $hostOk = $false; $peak = 0; $final = -1; $dropped = 0
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $dropped = [int]$g[3].Value; $peak = [int]$g[4].Value; $final = [int]$g[5].Value
            $hostOk = ($dropped -ge 1) -and ($peak -ge 1) -and ($final -eq 0)
            Write-Host ("  WPICKUP-MIRROR [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - ground copy retired (dropped=$dropped peak=$peak final=$final)")
            if (-not $hostOk -and $peak -ge 1 -and $final -gt 0) {
                Write-Host "    NOTE: the item stayed on the author's ground after the peer took it => the CLAIM never landed (duplicate)"
            }
        } else { Write-Host "  WPICKUP-MIRROR [host] FAIL - no host WPMIRROR verdict" }
    } else { Write-Host "  WPICKUP-MIRROR [host] FAIL - no log" }
    $joinOk = $false; $got = 0
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $got = [int]$g[3].Value
            $joinOk = ($got -ge 1)
            Write-Host ("  WPICKUP-MIRROR [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - consumed the proxy (pickedUp=$got)")
        } else { Write-Host "  WPICKUP-MIRROR [join] FAIL - no join WPMIRROR verdict" }
    } else { Write-Host "  WPICKUP-MIRROR [join] FAIL - no log" }
    $claimed = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[wi\] CLAIM author=' -Quiet)
    $applied = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wi\] CLAIM-APPLY netId=\d+ .* destroyed=1' -Quiet)
    Write-Host ("  WPICKUP-MIRROR trace: join authored CLAIM=$claimed, host CLAIM-APPLY destroyed=1=$applied")
    $ok = $hostOk -and $joinOk
    Write-Host ("  WPICKUP-MIRROR " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "pickup_mirror" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; dropped = $dropped;
                            grndPeak = $peak; grndFinal = $final; pickedUp = $got;
                            claimed = $claimed; applied = $applied })
}

# world_item_stale: the join's proxy is freed by the ENGINE (KENSHICOOP_WI_TEST_STALE
# injects it at the exact moment a zone teardown would) and the cull about to run on
# it must notice through the proxy's HAND rather than by touching the object.
#   staleCull>=1   - the injection actually met the cull. Without it the run proves
#                    nothing: the publish sweep clears dead proxies every tick, so a
#                    scenario that frees one seconds earlier finds nothing left to
#                    mishandle - which is exactly how an earlier version of this test
#                    passed on the PRE-FIX build.
#   live=1 absent  - a cull reporting the freed object as live means the hand check
#                    did not happen, and GameWorld::destroy ran on it a second time
#   no [wi] CLAIM  - a claim here came from reading isInInventory out of freed heap;
#                    the host answers one by destroying its REAL item, which is this
#                    bug's quiet form (the item disappears from both worlds)
#   no CLAIM-APPLY destroyed=1 - the host end of that same phantom
# Pair this with engine_integrity: a second destroy that DOES reach the engine shows
# up there as Kenshi's own "alredy has destroy reason" line naming our coop-* reason.
function Test-WorldItemStale {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO WIS verdict role=(\w+) pass=(\d+) peak=(-?\d+) dropped=(-?\d+) despawned=(-?\d+)'
    $read = {
        param($file)
        $r = [pscustomobject]@{ found = $false; peak = 0; dropped = 0; despawned = 0 }
        if (-not (Test-Path $file)) { return $r }
        $ln = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $ln) { return $r }
        $g = $ln.Matches[0].Groups
        $r.found = $true; $r.peak = [int]$g[3].Value; $r.dropped = [int]$g[4].Value
        $r.despawned = [int]$g[5].Value
        return $r
    }
    $h = & $read $HostFile
    $j = & $read $JoinFile
    $inject = 0; $stale = 0; $live = 0
    if (Test-Path $JoinFile) {
        $inject = @(Select-String -Path $JoinFile -Pattern '\[wi\] TEST-STALE ' -ErrorAction SilentlyContinue).Count
        $stale  = @(Select-String -Path $JoinFile -Pattern '\[wi\] CULL .* live=0' -ErrorAction SilentlyContinue).Count
        $live   = @(Select-String -Path $JoinFile -Pattern '\[wi\] CULL .* live=1' -ErrorAction SilentlyContinue).Count
    }
    $staged = $h.found -and $j.found -and ($h.dropped -ge 1) -and ($h.despawned -ge 1) `
              -and ($j.peak -ge 1) -and ($inject -ge 1)
    if (-not $staged) {
        Write-Host "  WI-STALE SKIP - staging incomplete (host dropped=$($h.dropped) despawned=$($h.despawned); join proxyPeak=$($j.peak) injected=$inject)"
        Write-Host "    No proxy was freed under a cull, so the assertions would pass vacuously."
        return (Add-GateResult -Name "world_item_stale" -Status SKIP `
                    -Metrics @{ hostDropped = $h.dropped; hostDespawned = $h.despawned
                                joinPeak = $j.peak; injected = $inject } `
                    -Detail "staging incomplete")
    }
    $claim   = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[wi\] CLAIM author=' -Quiet)
    $applied = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wi\] CLAIM-APPLY netId=\d+ .* destroyed=1' -Quiet)
    $ok = ($stale -ge 1) -and ($live -eq 0) -and (-not $claim) -and (-not $applied)
    Write-Host ("  WI-STALE " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - injected=$inject staleCull=$stale liveCull=$live phantomClaim=$claim hostDestroyedReal=$applied")
    if ($stale -lt 1) {
        Write-Host "    NOTE: the injection fired but no cull reported live=0 - the cull never re-resolved the hand"
    }
    if ($live -gt 0) {
        Write-Host "    NOTE: a cull called the freed proxy live - it destroyed an object the engine had already destroyed"
    }
    if ($claim) {
        Write-Host "    NOTE: the join claimed a pickup for a freed proxy - a read off freed memory"
    }
    if ($applied) {
        Write-Host "    NOTE: the host destroyed its REAL item answering that claim - the item is gone from both worlds"
    }
    return (Add-GateResult -Name "world_item_stale" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ injected = $inject; staleCull = $stale; liveCull = $live
                            phantomClaim = $claim; hostDestroyedReal = $applied
                            joinPeak = $j.peak; hostDropped = $h.dropped
                            hostDespawned = $h.despawned })
}

# inv_regear (protocol 47): the ONE-INSTANCE invariant across a full W2 round trip. Both
# halves must hold, because either alone is satisfied by a broken outcome:
#   host grndFinal=0  - our ground copy was retired ... but on its own, losing the item does that
#   host bagR1>=1     - our copy of the peer's bag holds it ... but on its own, so does a duplicate
# grndPeak>=1 keeps the whole thing honest by proving the item was ever on the ground.
# The trace also reports REHOME-DUPE-RISK, which is the author declining to destroy a ground
# copy it could not verify - safe (never loses the item) but still a visible duplicate, so it
# is surfaced rather than swallowed.
function Test-GearRepickup {
    param([string]$HostFile, [string]$JoinFile,
          [string]$GateName = "gear_repickup",
          # inv_regear_refuse only: the run injected a refusal, so converging is not enough -
          # the retry/verify path must be visible in the log, or the gate would also pass on a
          # build that went back to re-homing once and giving up.
          [switch]$RequireRetry,
          # inv_regear_refuse_all only: every attempt was refused, so convergence must have
          # come from the verify-then-destroy branch specifically (REHOME-DEDUPE).
          [switch]$RequireDedupe,
          # inv_regear_forget only: the author threw its ground track away, so the pickup
          # arrived named but unmatchable. Converging can then ONLY have come from the
          # site-anchored recovery, and the gate says so - otherwise it would also pass on the
          # build that answered "untracked" and left the item lying on the ground.
          [switch]$RequireSiteRecovery)
    $rxHost = 'WGRP verdict role=host pass=(\d+) sid=''([^'']*)'' dropped=(-?\d+) grndPeak=(-?\d+) grndFinal=(-?\d+) bagR1=(-?\d+) bagBase=(-?\d+)'
    $rxJoin = 'WGRP verdict role=join pass=(\d+) sid=''([^'']*)'' pickedUp=(-?\d+) tries=(\d+) bagR1=(-?\d+) bagBase=(-?\d+)'
    $hostOk = $false; $dropped = 0; $peak = 0; $final = -1; $bag = -1; $base = 0
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $dropped = [int]$g[3].Value; $peak = [int]$g[4].Value
            $final = [int]$g[5].Value; $bag = [int]$g[6].Value; $base = [int]$g[7].Value
            $hostOk = ($dropped -ge 1) -and ($peak -ge 1) -and ($final -eq 0) -and ($bag -eq $base + 1)
            Write-Host ("  GEAR-REPICKUP [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - one instance after the peer's pickup (dropped=$dropped peak=$peak ground=$final bagR1=$bag base=$base)")
            if ($final -gt 0 -and $bag -gt $base) {
                Write-Host "    NOTE: DUPLICATE - the item is in the peer's bag AND still on our ground (the manual-session symptom)"
            } elseif ($final -eq 0 -and $bag -le $base -and $peak -ge 1) {
                Write-Host "    NOTE: LOST - our ground copy is gone and the bag did not gain it either"
            } elseif ($bag -gt $base + 1) {
                Write-Host "    NOTE: DUPLICATE inside the bag - it gained $($bag - $base) copies for one pickup"
            }
        } else { Write-Host "  GEAR-REPICKUP [host] FAIL - no host WGRP verdict" }
    } else { Write-Host "  GEAR-REPICKUP [host] FAIL - no log" }
    $joinOk = $false; $got = 0; $tries = 0; $jbag = -1; $jbase = 0
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $got = [int]$g[3].Value; $tries = [int]$g[4].Value
            $jbag = [int]$g[5].Value; $jbase = [int]$g[6].Value
            $joinOk = ($got -ge 1) -and ($jbag -eq $jbase + 1)
            Write-Host ("  GEAR-REPICKUP [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - picked the relocated gear up (got=$got tries=$tries bagR1=$jbag base=$jbase)")
        } else { Write-Host "  GEAR-REPICKUP [join] FAIL - no join WGRP verdict" }
    } else { Write-Host "  GEAR-REPICKUP [join] FAIL - no log" }
    $rehomed  = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] PICKUP-APPLY .* moved=1' -Quiet)
    $retried  = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] REHOME-(RETRY-OK|DEDUPE)' -Quiet)
    $risk     = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] REHOME-DUPE-RISK' -Quiet)
    $injected = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] REHOME-REFUSE-INJECTED' -Quiet)
    Write-Host ("  GEAR-REPICKUP trace: host re-homed on first try=$rehomed, needed retry/dedupe=$retried, unresolved duplicate=$risk, refusal injected=$injected")
    $deduped = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] REHOME-DEDUPE .* destroyed=1' -Quiet)
    $ok = $hostOk -and $joinOk
    if ($RequireRetry -or $RequireDedupe) {
        if (-not $injected) {
            Write-Host "  GEAR-REPICKUP FAIL - the refusal lever never fired, so the recovery path was never under test"
            $ok = $false
        } elseif (-not $retried) {
            Write-Host "  GEAR-REPICKUP FAIL - a re-home was refused but nothing retried or deduped it (single-shot re-home = permanent duplicate)"
            $ok = $false
        }
    }
    if ($RequireSiteRecovery) {
        $forgot    = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] TRACK-FORGET-INJECTED' -Quiet)
        $recovered = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] PICKUP-APPLY .* moved=1 why=recovered-by-site' -Quiet)
        $untracked = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] PICKUP-APPLY .* moved=0 why=untracked' -Quiet)
        Write-Host "  GEAR-REPICKUP site recovery: track forgotten=$forgot, recovered-by-site=$recovered, gave up as untracked=$untracked"
        if (-not $forgot) {
            Write-Host "  GEAR-REPICKUP FAIL - the forget lever never fired, so the recovery was never under test"
            $ok = $false
        } elseif (-not $recovered) {
            Write-Host "  GEAR-REPICKUP FAIL - a named pickup could not be matched and nothing recovered it (this is the duplicate the player reported)"
            $ok = $false
        }
    }
    if ($RequireDedupe) {
        Write-Host "  GEAR-REPICKUP dedupe branch: REHOME-DEDUPE destroyed=1 = $deduped"
        if (-not $deduped) {
            Write-Host "  GEAR-REPICKUP FAIL - with every re-home refused, the ground copy can only be retired by verify-then-destroy, and that branch never ran"
            $ok = $false
        }
    }
    Write-Host ("  GEAR-REPICKUP " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name $GateName -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; dropped = $dropped;
                            grndPeak = $peak; grndFinal = $final; hostBagR1 = $bag;
                            hostBagBase = $base; pickedUp = $got; tries = $tries;
                            joinBagR1 = $jbag; joinBagBase = $jbase;
                            rehomedFirstTry = $rehomed; retried = $retried; dupeRisk = $risk;
                            refusalInjected = $injected; deduped = $deduped })
}

# world_item_burst: a burst of non-gear drops landing in ONE publish pass must ALL reach the
# peer promptly. The per-tick send batch is finite, and the entries that did not fit used to be
# stamped as sent anyway, so they went out only when the 5 s safety resend happened to pick them
# up. The gate reads the SPREAD between the peer seeing the first item of the burst and seeing
# them all, which needs no shared clock: a few ticks means the batch was retried, seconds means
# the tail was booked as sent and quietly withheld. The author's own side must show the whole
# burst on its ground too, or a "converged" spread would just mean nothing was ever dropped.
# inv_nested_bag (protocol 48): an item the host places INSIDE a worn backpack must show up
# inside the JOIN's copy of that bag. A worn container owns its own private Inventory, and no
# section of the wearer's inventory refers to it, so a bagged item used to appear in no snapshot
# whatsoever - it existed for its author alone, and the bag travelled to the peer empty. The
# gate is about PLACE, not presence: landing the item loose on the character would pass a naive
# "does the peer have it" check while leaving the bag empty for the next relocation, so the
# scenario counts strictly INSIDE the container. It is also a DELTA (inBag - base), because the
# probe is a common template the save's bag may already hold - an absolute count would pass on
# the fixture's own contents with nothing having crossed the wire.
function Test-NestedBag {
    param([string]$HostFile, [string]$JoinFile)
    $rx = "NEST verdict role=(host|join) pass=(\d+) sid='([^']*)' bags=(-?\d+) minted=(-?\d+) want='([^']*)' base='([^']*)' inBags='([^']*)' delta='([^']*)' total=(-?\d+)"
    $read = {
        param($file, $role)
        $r = [pscustomobject]@{ found = $false; pass = $false; sid = ''; bags = -1; minted = -1
                                want = ''; base = ''; inBags = ''; delta = ''; total = 0 }
        if (-not (Test-Path $file)) { return $r }
        $m = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue |
             Where-Object { $_.Matches[0].Groups[1].Value -eq $role } | Select-Object -Last 1
        if ($null -eq $m) { return $r }
        $g = $m.Matches[0].Groups
        $r.found = $true; $r.pass = ([int]$g[2].Value -eq 1); $r.sid = $g[3].Value
        $r.bags = [int]$g[4].Value; $r.minted = [int]$g[5].Value
        $r.want = $g[6].Value; $r.base = $g[7].Value; $r.inBags = $g[8].Value
        $r.delta = $g[9].Value; $r.total = [int]$g[10].Value
        return $r
    }
    $h = & $read $HostFile 'host'
    $j = & $read $JoinFile 'join'
    foreach ($side in @(@('host', $h), @('join', $j))) {
        $role = $side[0]; $s = $side[1]
        if (-not $s.found) { Write-Host "  NESTED-BAG [$role] FAIL - no NEST verdict"; continue }
        Write-Host ("  NESTED-BAG [$role] " + $(if ($s.pass) { "PASS" } else { "FAIL" }) +
                    " - bags=$($s.bags) per-bag gain of '$($s.sid)' was '$($s.delta)', wanted '$($s.want)' (base '$($s.base)' -> '$($s.inBags)')")
        if ($s.bags -eq 1) {
            Write-Host "    NOTE: one container on the carrier ($role minted=$($s.minted)); Kenshi refuses a spare bag inside the worn bag, so the same-template collapse is not reachable here and only the placement is under test"
        } elseif ($s.delta -eq '0,0') {
            Write-Host "    NOTE: nothing reached a bag on this side - a worn container's contents are described by the snapshot only if the capture walks into it"
        } elseif ($s.delta -ne $s.want) {
            Write-Host "    NOTE: the gain landed in the wrong distribution - both parents' contents collapsing into one bag looks exactly like this"
        }
    }
    if ($h.found -and $j.found -and $h.inBags -ne $j.inBags) {
        Write-Host "    NOTE: host holds '$($h.inBags)' but the join holds '$($j.inBags)' (bag order may differ between clients; the per-bag gain is what must match)"
    }
    $skip = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[recon\] NESTED-SKIP' -Quiet)
    Write-Host "  NESTED-BAG trace: join skipped a nested apply for a missing container=$skip"
    $ok = $h.found -and $j.found -and $h.pass -and $j.pass

    # CHURN: the TOP-LEVEL reconcile must never judge a bagged item to be a surplus of the
    # CHARACTER's own inventory. The count delta alone cannot see this - destroying an item and
    # recreating it satisfies the delta, which is how this gate first passed over the bug. The
    # signature is the reconcile's own group line: it wants zero of the probe at the top level
    # while its local read reports some, which only happens if the read walked into the bag.
    # That is a removal aimed at a foreign Inventory, once per apply tick.
    $grpRx = "\[recon\] grp type=\d+ sid='([^']*)' desire\(eq=(-?\d+),loose=(-?\d+)\) cur\(eq=(-?\d+),loose=(-?\d+)\)"
    $grpLines = @()
    if (Test-Path $JoinFile) {
        $grpLines = @(Select-String -Path $JoinFile -Pattern $grpRx -ErrorAction SilentlyContinue)
    }
    if ($grpLines.Count -eq 0) {
        Write-Host "  NESTED-BAG FAIL - no '[recon] grp' lines in the join log, so the churn check could not run (this scenario's manifest sets KENSHICOOP_INV_DUMP=1; without it the gate is blind to a destroy/recreate loop)"
        $ok = $false
        $churn = -1
    } else {
        $churn = 0
        foreach ($m in $grpLines) {
            $g = $m.Matches[0].Groups
            if ($g[1].Value -ne $j.sid) { continue }
            $desired = [int]$g[2].Value + [int]$g[3].Value
            $current = [int]$g[4].Value + [int]$g[5].Value
            if ($desired -eq 0 -and $current -gt 0) { $churn++ }
        }
        Write-Host "  NESTED-BAG trace: top-level passes judging the bagged probe a surplus=$churn"
        if ($churn -gt 0) {
            Write-Host "    NOTE: the top-level reconcile counted the bag's contents as the character's own and ran a removal against the wrong Inventory - the delta still converges because the nested pass puts them back, so only this check sees it"
            $ok = $false
        }
    }
    Write-Host ("  NESTED-BAG " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "nested_bag" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $h.pass; joinOk = $j.pass; want = $j.want
                            bags = $j.bags; hostDelta = $h.delta; joinDelta = $j.delta
                            hostInBags = $h.inBags; joinInBags = $j.inBags
                            joinBase = $j.base; joinTotal = $j.total
                            nestedSkip = $skip; topLevelChurn = $churn })
}

function Test-DumpAll {
    param([string]$HostFile, [string]$JoinFile)
    $rxH = "WDMP verdict role=host pass=(\d+) kit=(-?\d+) want=(-?\d+) dropped=(-?\d+) grndPeak=(-?\d+) grndFinal=(-?\d+) heldR1=(-?\d+) conserved=(\d+) dist='([^']*)'"
    $rxJ = "WDMP verdict role=join pass=(\d+) kit=(-?\d+) want=(-?\d+) pickedUp=(-?\d+) tries=(\d+) reDropped=(-?\d+) grndFinal=(-?\d+) held=(-?\d+) conserved=(\d+) dist='([^']*)'"
    $h = [pscustomobject]@{ found = $false; pass = $false; kit = 0; want = -1; dropped = -1
                            peak = -1; final = -1; held = -1; conserved = $false; dist = '' }
    $j = [pscustomobject]@{ found = $false; pass = $false; kit = 0; want = -1; got = -1; tries = 0
                            reDropped = -1; final = -1; held = -1; conserved = $false; dist = '' }
    if (Test-Path $HostFile) {
        $m = Select-String -Path $HostFile -Pattern $rxH -ErrorAction SilentlyContinue |
             Select-Object -Last 1
        if ($null -ne $m) {
            $g = $m.Matches[0].Groups
            $h.found = $true; $h.pass = ([int]$g[1].Value -eq 1); $h.kit = [int]$g[2].Value
            $h.want = [int]$g[3].Value; $h.dropped = [int]$g[4].Value; $h.peak = [int]$g[5].Value
            $h.final = [int]$g[6].Value; $h.held = [int]$g[7].Value
            $h.conserved = ([int]$g[8].Value -eq 1); $h.dist = $g[9].Value
        }
    }
    if (Test-Path $JoinFile) {
        $m = Select-String -Path $JoinFile -Pattern $rxJ -ErrorAction SilentlyContinue |
             Select-Object -Last 1
        if ($null -ne $m) {
            $g = $m.Matches[0].Groups
            $j.found = $true; $j.pass = ([int]$g[1].Value -eq 1); $j.kit = [int]$g[2].Value
            $j.want = [int]$g[3].Value; $j.got = [int]$g[4].Value; $j.tries = [int]$g[5].Value
            $j.reDropped = [int]$g[6].Value; $j.final = [int]$g[7].Value; $j.held = [int]$g[8].Value
            $j.conserved = ([int]$g[9].Value -eq 1); $j.dist = $g[10].Value
        }
    }
    $ok = $true
    # dist is one 'ground+bag/dumped' triple per template: the conservation ledger. Every instance
    # the host dumped must exist exactly ONCE on each side, on the ground or in the picker's bag.
    if (-not $h.found) { Write-Host "  DUMP-ALL [host] FAIL - no WDMP verdict"; $ok = $false }
    else {
        Write-Host ("  DUMP-ALL [host] " + $(if ($h.pass) { "PASS" } else { "FAIL" }) +
                    " - dumped $($h.dropped)/$($h.want) in a burst, ground peaked at $($h.peak); ledger ground+bag/dumped = '$($h.dist)'")
        if (-not $h.pass) { $ok = $false }
    }
    if (-not $j.found) { Write-Host "  DUMP-ALL [join] FAIL - no WDMP verdict"; $ok = $false }
    else {
        Write-Host ("  DUMP-ALL [join] " + $(if ($j.pass) { "PASS" } else { "FAIL" }) +
                    " - picked up $($j.got)/$($j.want) in $($j.tries) tries; ledger ground+bag/dumped = '$($j.dist)'")
        if (-not $j.pass) { $ok = $false }
    }
    foreach ($side in @(@('host', $h), @('join', $j))) {
        $role = $side[0]; $s = $side[1]
        if ($s.found -and -not $s.conserved) {
            Write-Host "    NOTE: [$role] the ledger does not balance ('$($s.dist)'). A template summing ABOVE what was dumped is the duplicate - the peer holds it and our ground copy was never retired, or a mirror fabricated a second one. Summing BELOW it is the item that never arrived."
        }
    }
    if ($h.found -and $h.want -lt 3) {
        Write-Host "    NOTE: only $($h.want) instance(s) were dumped, so this run was a trickle rather than the burst the scenario is for - the extra same-template copies could not be minted (a weapon needs its manufacturer GameData). The conservation ledger still applies, but the burst-specific paths are not under test."
    }

    # ---- Log-level gates: the states a converged count can still hide -------------------
    # A count taken at the verdict can be reached through any number of duplications and
    # corrections in between, so the signatures that MEAN a duplicate are gated directly.
    $sig = {
        param($file, $pattern)
        if (-not (Test-Path $file)) { return 0 }
        return @(Select-String -Path $file -Pattern $pattern -ErrorAction SilentlyContinue).Count
    }
    # FABRICATION. A mirror that cannot find its own copy mints one from the intent's provenance.
    # That is only safe when the item is genuinely absent, and after the nested reach landed it
    # no longer is: the ordinary cause was an item stowed in the worn backpack by a bulk pickup,
    # invisible to a top-level-only search, so the heal left the real item in the bag AND a minted
    # copy on the ground. Reported-not-failed is how this hid through several green runs.
    $healed = (& $sig $HostFile '\[wd\] APPLY-HEALED') + (& $sig $JoinFile '\[wd\] APPLY-HEALED')
    # A pickup that ran out of retries: the peer holds the item and we could not reach our copy.
    $gaveUp = (& $sig $HostFile '\[wd\] PICKUP-GAVEUP') + (& $sig $JoinFile '\[wd\] PICKUP-GAVEUP')
    # The old one-shot verdict. It should not appear at all now that an unsatisfied identified
    # pickup is parked and retried instead of concluded.
    $untracked = (& $sig $HostFile '\[wd\] PICKUP-APPLY .* why=untracked') +
                 (& $sig $JoinFile '\[wd\] PICKUP-APPLY .* why=untracked')
    # A track retired before WD_DEAD_HOLD_MS: the tick-denominated budget retired tracks ~25 ms
    # after their own drop, and the streak duration is now in the line so the gate can see it.
    $prunes = @()
    foreach ($f in @($HostFile, $JoinFile)) {
        if (Test-Path $f) {
            $prunes += @(Select-String -Path $f -ErrorAction SilentlyContinue `
                -Pattern "\[wd\] ground-prune .* \((\d+) consecutive reads over (\d+)ms, everLive=(\d+)")
        }
    }
    $fastPrune = 0
    foreach ($m in $prunes) {
        $g = $m.Matches[0].Groups
        if ([int]$g[3].Value -eq 1 -and [int]$g[2].Value -lt 3000) { $fastPrune++ }
    }
    Write-Host "  DUMP-ALL trace: fabrications=$healed, pickups given up=$gaveUp, one-shot untracked verdicts=$untracked, tracks retired early=$fastPrune (of $($prunes.Count) prunes)"
    if ($healed -gt 0) {
        Write-Host "    NOTE: a drop mirror fabricated an item instead of relocating its own copy - if that copy existed anywhere (a carried container, most likely) the result is a duplicate"
        $ok = $false
    }
    if ($gaveUp -gt 0) {
        Write-Host "    NOTE: an identified pickup exhausted its retries, so the peer holds an item whose local copy is still wherever it was"
        $ok = $false
    }
    if ($untracked -gt 0) {
        Write-Host "    NOTE: a pickup was answered 'untracked' and dropped - it should be parked and retried, because our ground copy is otherwise left there for the session"
        $ok = $false
    }
    if ($fastPrune -gt 0) {
        Write-Host "    NOTE: a track that HAD read live was retired after less than WD_DEAD_HOLD_MS of dead reads - the read budget is denominated in engine ticks and the loop runs at ~100-125 Hz, so this is the ~25 ms window that lost tracks 29 ms after their own drop"
        $ok = $false
    }
    # The nested reach, when it fires, is the difference between conserving the object and minting
    # a duplicate. Not required (the grid may not have overflowed), but recorded either way.
    $nested = (& $sig $HostFile '\[wd\] RELOCATE-NESTED') + (& $sig $JoinFile '\[wd\] RELOCATE-NESTED')
    Write-Host "  DUMP-ALL trace: relocations out of a carried container=$nested"

    Write-Host ("  DUMP-ALL " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "dump_all" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $h.pass; joinOk = $j.pass; kit = $h.kit; want = $h.want
                            dropped = $h.dropped; grndPeak = $h.peak; grndFinal = $h.final
                            pickedUp = $j.got; joinHeld = $j.held
                            hostConserved = $h.conserved; joinConserved = $j.conserved
                            hostDist = $h.dist; joinDist = $j.dist
                            reDropped = $j.reDropped; healed = $healed; gaveUp = $gaveUp
                            untracked = $untracked; fastPrune = $fastPrune
                            prunes = $prunes.Count; relocateNested = $nested })
}

function Test-WorldItemBurst {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'WIB verdict role=(host|join) pass=(\d+) n=(\d+) dropped=(-?\d+) peak=(-?\d+) firstMs=(\d+) allMs=(\d+) spreadMs=(-?\d+) limitMs=(\d+)'
    $read = {
        param($file, $role)
        $r = [pscustomobject]@{ found = $false; pass = $false; n = 0; dropped = -1; peak = -1
                                spread = -1; limit = 0 }
        if (-not (Test-Path $file)) { return $r }
        $m = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue |
             Where-Object { $_.Matches[0].Groups[1].Value -eq $role } | Select-Object -Last 1
        if ($null -eq $m) { return $r }
        $g = $m.Matches[0].Groups
        $r.found = $true; $r.pass = ([int]$g[2].Value -eq 1); $r.n = [int]$g[3].Value
        $r.dropped = [int]$g[4].Value; $r.peak = [int]$g[5].Value
        $r.spread = [int]$g[8].Value; $r.limit = [int]$g[9].Value
        return $r
    }
    $h = & $read $HostFile 'host'
    $j = & $read $JoinFile 'join'
    if (-not $h.found) { Write-Host "  WORLD-ITEM-BURST [host] FAIL - no WIB verdict" }
    else {
        Write-Host ("  WORLD-ITEM-BURST [host] " + $(if ($h.pass) { "PASS" } else { "FAIL" }) +
                    " - author dropped the whole burst (n=$($h.n) dropped=$($h.dropped) groundPeak=$($h.peak))")
    }
    if (-not $j.found) { Write-Host "  WORLD-ITEM-BURST [join] FAIL - no WIB verdict" }
    else {
        Write-Host ("  WORLD-ITEM-BURST [join] " + $(if ($j.pass) { "PASS" } else { "FAIL" }) +
                    " - peer saw all $($j.n) (groundPeak=$($j.peak) spread=$($j.spread)ms limit=$($j.limit)ms)")
        if ($j.peak -lt $j.n) {
            Write-Host "    NOTE: the peer never saw the whole burst - part of it was never streamed at all"
        } elseif ($j.spread -gt $j.limit) {
            Write-Host "    NOTE: the tail of the burst arrived $($j.spread)ms late - the overflow was booked as sent and waited for the safety resend"
        }
    }
    $defer = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wi\] SEND-DEFER' -Quiet)
    Write-Host "  WORLD-ITEM-BURST trace: batch overflowed (SEND-DEFER)=$defer"
    $ok = $h.found -and $j.found -and $h.pass -and $j.pass
    if (-not $defer) {
        Write-Host "  WORLD-ITEM-BURST FAIL - the batch never overflowed, so the deferral path was never under test"
        $ok = $false
    }
    Write-Host ("  WORLD-ITEM-BURST " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "world_item_burst" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $h.pass; joinOk = $j.pass; burstN = $j.n
                            hostDropped = $h.dropped; hostPeak = $h.peak; joinPeak = $j.peak
                            joinSpreadMs = $j.spread; limitMs = $j.limit; overflowed = $defer })
}

# no_phantom_pickups (protocol 47): every W2 PICKUP intent must NAME the instance it is about
# (ref=<dropOwner>/<dropId>, the identity both clients took from the DROP). ref=0/0 is the
# phantom signature, and it was mass-produced two ways: the gear census read an increase it
# had no evidence for (a container's worn items missing from one read and back in the next -
# or, worse, an uninitialised `seeded` flag making it skip its baseline entirely and read the
# whole worn kit as arrivals), and the receiver answered such an intent by re-homing an
# arbitrary same-sid ground item. Both ends now refuse to guess, so the count must be zero.
# Checked on authored intents AND on applies, since either end alone could reintroduce it.
function Test-NoPhantomPickups {
    param([string]$HostFile, [string]$JoinFile)
    $side = {
        param($file, $role)
        $r = [pscustomobject]@{ ok = $true; authored = 0; phantomAuthored = 0
                                applied = 0; phantomApplied = 0; seeded = 0; suppressed = 0 }
        if (-not (Test-Path $file)) {
            Write-Host "  NO-PHANTOM-PICKUPS [$role] FAIL - no log"
            $r.ok = $false; return $r
        }
        foreach ($ln in (Get-Content -Path $file -ErrorAction SilentlyContinue)) {
            if ($ln -match '\[wd\] PICKUP id=') {
                $r.authored++
                if ($ln -match 'ref=0/0') { $r.phantomAuthored++ }
            } elseif ($ln -match '\[wd\] PICKUP-APPLY ') {
                $r.applied++
                if ($ln -match 'ref=0/0') { $r.phantomApplied++ }
            }
            if ($ln -match '\[wd\] census-seed') { $r.seeded++ }
            if ($ln -match '\[wd\] increase-(unexplained|untracked)') { $r.suppressed++ }
        }
        $r.ok = ($r.phantomAuthored -eq 0) -and ($r.phantomApplied -eq 0)
        Write-Host ("  NO-PHANTOM-PICKUPS [$role] " + $(if ($r.ok) { "PASS" } else { "FAIL" }) +
                    " - identity-less authored=$($r.phantomAuthored)/$($r.authored) applied=$($r.phantomApplied)/$($r.applied)" +
                    " (containers seeded=$($r.seeded), unprovable increases suppressed=$($r.suppressed))")
        if ($r.phantomAuthored -gt 0) {
            Write-Host "    NOTE: the census authored $($r.phantomAuthored) PICKUP intent(s) naming no instance - it read an increase it had no arriving object for"
        }
        if ($r.phantomApplied -gt 0) {
            Write-Host "    NOTE: $($r.phantomApplied) PICKUP-APPLY had no drop identity - such an intent can only be answered by guessing which ground item to re-home"
        }
        return $r
    }
    $h = & $side $HostFile 'host'
    $j = & $side $JoinFile 'join'
    $ok = $h.ok -and $j.ok
    Write-Host ("  NO-PHANTOM-PICKUPS " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "no_phantom_pickups" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostAuthored = $h.authored; joinAuthored = $j.authored;
                            hostPhantomAuthored = $h.phantomAuthored; joinPhantomAuthored = $j.phantomAuthored;
                            hostApplied = $h.applied; joinApplied = $j.applied;
                            hostPhantomApplied = $h.phantomApplied; joinPhantomApplied = $j.phantomApplied;
                            hostSeeded = $h.seeded; joinSeeded = $j.seeded;
                            hostSuppressed = $h.suppressed; joinSuppressed = $j.suppressed })
}

# inv_overflow (protocol 46): the container-overflow item-loss class. Both clients run the
# same local checks against a container they OWN, so BOTH verdicts must pass. Gates:
#   gearOk     - a capture that cannot fit everything spends its budget on EQUIPPED entries
#                first (worn kit is never what gets cut)
#   truncOk    - the overflow is reported on the wire (INV_FLAG_TRUNCATED)
#   additiveOk - reconciling against a truncated list destroys NOTHING
# Also asserts no APPLY-TRUNCATED reconcile ever reported a shrinking container.
function Test-InventoryOverflow {
    param([string]$HostFile, [string]$JoinFile)
    $rxV = 'SCENARIO INVOF verdict pass=(\d+) gearOk=(\d+) truncOk=(\d+) additiveOk=(\d+) full=(\d+) eqFull=(\d+) small=(\d+) eqSmall=(\d+) before=(\d+) after=(\d+)'
    $side = {
        param($file, $role)
        $r = [pscustomobject]@{ ok = $false; gear = $false; trunc = $false; additive = $false
                                full = 0; before = 0; after = 0 }
        if (-not (Test-Path $file)) { Write-Host "  INV-OVERFLOW [$role] FAIL - no log"; return $r }
        $ln = Select-String -Path $file -Pattern $rxV -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $ln) { Write-Host "  INV-OVERFLOW [$role] FAIL - no INVOF verdict"; return $r }
        $g = $ln.Matches[0].Groups
        $r.gear     = ([int]$g[2].Value -eq 1)
        $r.trunc    = ([int]$g[3].Value -eq 1)
        $r.additive = ([int]$g[4].Value -eq 1)
        $r.full     = [int]$g[5].Value
        $r.before   = [int]$g[9].Value
        $r.after    = [int]$g[10].Value
        $r.ok = $r.gear -and $r.trunc -and $r.additive -and ($r.after -ge $r.before)
        Write-Host ("  INV-OVERFLOW [$role] " + $(if ($r.ok) { "PASS" } else { "FAIL" }) +
                    " - gearFirst=$($r.gear) truncFlagged=$($r.trunc) additiveOnly=$($r.additive)" +
                    " (full=$($r.full) before=$($r.before) after=$($r.after))")
        if (-not $r.additive -and $r.after -lt $r.before) {
            Write-Host "    NOTE: reconcile against a TRUNCATED snapshot destroyed $($r.before - $r.after) entries - the backpack item-loss bug"
        }
        return $r
    }
    $H = & $side $HostFile "host"
    $J = & $side $JoinFile "join"
    # Any truncated snapshot that actually crossed the wire must have been applied
    # additively; surface the counts so a cap that is too low is visible in the run.
    $sentTrunc = 0; $appliedTrunc = 0
    foreach ($f in @($HostFile, $JoinFile)) {
        if (Test-Path $f) {
            $sentTrunc    += @(Select-String -Path $f -Pattern '\[inv\] SEND-TRUNCATED' -ErrorAction SilentlyContinue).Count
            $appliedTrunc += @(Select-String -Path $f -Pattern '\[inv\] APPLY-TRUNCATED' -ErrorAction SilentlyContinue).Count
        }
    }
    Write-Host "  INV-OVERFLOW trace: wire truncations sent=$sentTrunc applied=$appliedTrunc (0/0 expected at cap 64)"
    $ok = $H.ok -and $J.ok
    Write-Host ("  INV-OVERFLOW " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "inv_overflow" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $H.ok; joinOk = $J.ok
                            gearFirst = ($H.gear -and $J.gear)
                            truncFlagged = ($H.trunc -and $J.trunc)
                            additiveOnly = ($H.additive -and $J.additive)
                            sentTruncations = $sentTrunc; appliedTruncations = $appliedTrunc })
}

# inv_dropfull (protocol 46): the W2 drop-vs-snapshot race. The host drops EQUIPPED gear
# while continuously churning the same container's loose contents, maximizing the chance a
# bag snapshot overtakes the drop intent. The join must still conserve the gear (bag loses
# it, ground gains it). Gates both verdicts plus the decisive wire evidence: the peer must
# log an APPLY with moved>0 and must NEVER log APPLY-LOST (the silent item loss).
# APPLY-HEALED now FAILS the gate rather than merely being reported. A heal means the mirror
# could not find its own copy and minted one from the intent's provenance - safe only if the item
# is genuinely absent, which it usually was not: the reach the mirror lacked was into carried
# containers, where a bulk pickup stows the tail of a burst. The heal then leaves the real item in
# the bag and a fabricated duplicate on the ground, and tolerating the signature is exactly how
# that survived several green runs.
function Test-InventoryDropFull {
    param([string]$HostFile, [string]$JoinFile)
    $rxHost = 'SCENARIO INVDF verdict role=host pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invAfter=(-?\d+) grndAfter=(-?\d+) churns=(\d+)'
    $rxJoin = 'SCENARIO INVDF verdict role=join pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invMin=(-?\d+) grndMax=(-?\d+) conserved=(\d+)'
    $hostOk = $false; $churns = 0
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invAfter = [int]$g[4].Value; $churns = [int]$g[6].Value
            $hostOk = ($invBase -ge 1) -and ($invAfter -le $invBase - 1) -and ($churns -gt 0)
            Write-Host ("  INV-DROPFULL [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - dropped gear mid-churn (invBase=$invBase invAfter=$invAfter churns=$churns)")
        } else { Write-Host "  INV-DROPFULL [host] FAIL - no host INVDF verdict" }
    } else { Write-Host "  INV-DROPFULL [host] FAIL - no log" }
    $joinOk = $false
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invMin = [int]$g[4].Value; $grndMax = [int]$g[5].Value
            $joinOk = ($invBase -ge 1) -and ($invMin -le $invBase - 1) -and ($grndMax -ge 1)
            Write-Host ("  INV-DROPFULL [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - conserved own copy (invBase=$invBase invMin=$invMin grndMax=$grndMax)")
            if (-not $joinOk -and $invMin -le $invBase - 1 -and $grndMax -lt 1) {
                Write-Host "    NOTE: gear LEFT the bag but never reached the ground => the snapshot beat the drop intent and the reconcile destroyed it"
            }
        } else { Write-Host "  INV-DROPFULL [join] FAIL - no join INVDF verdict" }
    } else { Write-Host "  INV-DROPFULL [join] FAIL - no log" }
    $authored = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] DROP id=' -Quiet)
    $moved    = $false; $lost = $false; $healed = 0
    if (Test-Path $JoinFile) {
        $moved  = [bool](Select-String -Path $JoinFile -Pattern '\[wd\] APPLY id=\d+ .* moved=[1-9]' -Quiet)
        $lost   = [bool](Select-String -Path $JoinFile -Pattern '\[wd\] APPLY-LOST' -Quiet)
        $healed = @(Select-String -Path $JoinFile -Pattern '\[wd\] APPLY-HEALED' -ErrorAction SilentlyContinue).Count
    }
    Write-Host "  INV-DROPFULL trace: host authored DROP=$authored, join moved>0=$moved, APPLY-LOST=$lost, healed=$healed"
    if ($healed -gt 0) {
        Write-Host "    NOTE: $healed drop(s) needed the fabrication backstop, which mints an item the mirror could not find. That is only safe when the item is genuinely absent, and it usually was not: an item stowed in a worn container by a bulk pickup is invisible to a top-level-only search, so the heal leaves the real one in the bag AND a minted copy on the ground. Reported-not-failed here is how that duplicate survived several green runs."
    }
    $ok = $hostOk -and $joinOk -and $authored -and $moved -and (-not $lost) -and ($healed -eq 0)
    Write-Host ("  INV-DROPFULL " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "inv_dropfull" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; authored = $authored
                            moved = $moved; lost = $lost; healed = $healed; churns = $churns })
}

# weapon_loot: the host's owned leader acquires a NOVEL weapon (a sid in no shared-save
# inventory); the join must fabricate exactly one copy onto its driven leader via the
# inventory snapshot channel + the spike-451 weapon CREATE. Gates: both verdicts pass
# (arrived, persisted, max count never exceeded 1 - the zero-dupe requirement), the
# sids MATCH across clients, and the quality buckets agree within tolerance.
function Test-WeaponLoot {
    param([string]$HostFile, [string]$JoinFile)
    $rxHost = 'WLOOT verdict role=host pass=(\d+) sid=''([^'']*)'' added=(-?\d+) final=(-?\d+) max=(-?\d+) qual=(-?\d+)'
    $rxJoin = 'WLOOT verdict role=join pass=(\d+) sid=''([^'']*)'' final=(-?\d+) max=(-?\d+) qual=(-?\d+)'
    $hostOk = $false; $hostSid = ""; $hostQual = -1
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $hostSid = $g[2].Value; $hostQual = [int]$g[6].Value
            $added = [int]$g[3].Value; $final = [int]$g[4].Value; $max = [int]$g[5].Value
            $hostOk = ($added -eq 1) -and ($final -eq 1) -and ($max -eq 1)
            Write-Host ("  WEAPON-LOOT [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - acquired novel weapon sid='$hostSid' (added=$added final=$final max=$max qual=$hostQual)")
        } else { Write-Host "  WEAPON-LOOT [host] FAIL - no host WLOOT verdict" }
    }
    $joinOk = $false; $joinSid = ""; $joinQual = -1
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $joinSid = $g[2].Value; $joinQual = [int]$g[5].Value
            $final = [int]$g[3].Value; $max = [int]$g[4].Value
            $joinOk = ($final -eq 1) -and ($max -eq 1)
            Write-Host ("  WEAPON-LOOT [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - peer copy fabricated sid='$joinSid' (final=$final max=$max qual=$joinQual)")
            if (-not $joinOk -and $final -eq 0) {
                Write-Host "    NOTE: weapon never appeared on the join => acquisition did not propagate (snapshot or CREATE failed)"
            }
            if (-not $joinOk -and $max -gt 1) {
                Write-Host "    NOTE: transient count $max > 1 => fabrication raced the conservation channel into a dupe"
            }
        } else { Write-Host "  WEAPON-LOOT [join] FAIL - no join WLOOT verdict" }
    }
    $sidMatch = ($hostSid -ne "") -and ($hostSid -eq $joinSid)
    if (-not $sidMatch) { Write-Host "  WEAPON-LOOT sid MISMATCH host='$hostSid' join='$joinSid'" }
    $qualOk = ($hostQual -ge 0) -and ($joinQual -ge 0) -and ([Math]::Abs($hostQual - $joinQual) -le 5)
    Write-Host ("  WEAPON-LOOT quality host=$hostQual join=$joinQual match=" + $(if ($qualOk) { "yes" } else { "NO" }))
    $ok = $hostOk -and $joinOk -and $sidMatch -and $qualOk
    Write-Host ("  WEAPON-LOOT " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "weapon_loot" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; sid = $hostSid;
                            sidMatch = $sidMatch; hostQual = $hostQual; joinQual = $joinQual })
}

