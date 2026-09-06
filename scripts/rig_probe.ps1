<#
.SYNOPSIS
  Cheap falsification probe for the 4-instance rig (RESEARCH Assumptions A1/A2),
  plus an A4 ownership-mapping helper reused by the Task 3 smoke run.

.DESCRIPTION
  Read-only against the primary Kenshi install: this script only ever LINKS FROM
  or LAUNCHES it. It never writes into or deletes from the primary install
  directory (T-04-01 guard, enforced by Assert-NotPrimary below).

  Two independent, non-fatal experiments (each prints one
  "RIGPROBE <ID>=<VERDICT> (...)" finding line and continues regardless of the
  other's outcome):

    (1) HARDLINK (A1) - creates ONE NTFS hardlink of a single large read-only
        asset file FROM -SourceDir into a scratch dir, confirms the link
        resolves and reads back byte-identical (SHA-256 compare), then deletes
        the scratch link. Answers "is same-volume hardlinking of Kenshi assets
        safe on this filesystem at all?"

    (2) STEAM-CLOSED LAUNCH (A2) - if Steam is currently running, emits UNKNOWN
        and instructs the operator to close Steam and re-run (this script never
        tries to close Steam itself). If Steam is not running AND RE_Kenshi is
        already installed in -SourceDir, launches -SourceDir's kenshi_x64.exe
        once with KENSHICOOP_TRANSPORT=udp and a short self-exit, and reports
        whether the plugin log reached "PLUGIN-STARTED" or the process died at
        the launcher. If RE_Kenshi is not installed, emits UNKNOWN (that is a
        separate, user_setup-gated precondition, not a Steam-state finding).

  A third mode, -OwnershipRunDir, skips both experiments and instead greps an
  existing run directory's logs for the PKT_OWN_RANKS / allOwnRanks_ /
  ownRank ownership-announcement lines, printing the observed rank->player
  mapping so the A4 (dynamic-tab vs baked-4-squad-start) decision is
  reproducible from a run's logs alone (used by Task 3).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rig_probe.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rig_probe.ps1 -OwnershipRunDir tools\test-runs\20260901_120000
#>
[CmdletBinding()]
param(
    [string]$SourceDir = "F:\SteamLibrary\steamapps\common\Kenshi",
    # Same-volume as SourceDir by default: NTFS hardlinks (Experiment 1) cannot
    # cross drive letters, and $env:TEMP is usually on a different drive than
    # the Kenshi install, so a cross-volume default would always FAIL the
    # hardlink experiment for a reason unrelated to hardlink safety itself.
    [string]$ScratchDir = (Join-Path (Split-Path -Path "F:\SteamLibrary\steamapps\common\Kenshi" -Qualifier) "kenshicoop-rigprobe"),
    [int]$LaunchTestSeconds = 20,
    [string]$OwnershipRunDir = ""
)

if (-not $PSBoundParameters.ContainsKey('ScratchDir') -and $PSBoundParameters.ContainsKey('SourceDir')) {
    # Re-derive the same-volume default from the actual -SourceDir the caller
    # passed, rather than the hard-coded default above.
    $ScratchDir = Join-Path (Split-Path -Path $SourceDir -Qualifier) "kenshicoop-rigprobe"
}

$ErrorActionPreference = "Stop"

function Write-RigProbe {
    param([string]$Id, [string]$Verdict, [string]$Detail = "")
    $line = "RIGPROBE $Id=$Verdict"
    if ($Detail -ne "") { $line += " ($Detail)" }
    Write-Host $line
}

function Assert-NotPrimary {
    # Hard guard (threat T-04-01): a write/link TARGET must never resolve to,
    # or overlap, the primary Kenshi install. This script only ever reads or
    # hardlinks FROM $Primary; $Target is always a scratch/dest path.
    param([string]$Target, [string]$Primary)
    $t = [System.IO.Path]::GetFullPath($Target).TrimEnd('\')
    $p = [System.IO.Path]::GetFullPath($Primary).TrimEnd('\')
    if ($t -ieq $p -or
        $t.StartsWith("$p\", [System.StringComparison]::OrdinalIgnoreCase) -or
        $p.StartsWith("$t\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "REFUSING: target '$Target' equals or overlaps the primary install '$Primary'."
    }
}

# ---- Experiment 1: HARDLINK (A1) --------------------------------------------
function Test-HardlinkSafety {
    param([string]$SourceDir, [string]$ScratchDir)

    if (-not (Test-Path (Join-Path $SourceDir "kenshi_x64.exe"))) {
        Write-RigProbe -Id "A1" -Verdict "UNKNOWN" -Detail "SourceDir '$SourceDir' has no kenshi_x64.exe"
        return
    }
    Assert-NotPrimary -Target $ScratchDir -Primary $SourceDir

    $dataDir = Join-Path $SourceDir "data"
    $asset = Get-ChildItem -Path $dataDir -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object Length -Descending | Select-Object -First 1
    if (-not $asset) {
        Write-RigProbe -Id "A1" -Verdict "UNKNOWN" -Detail "no asset file found under '$dataDir'"
        return
    }

    New-Item -ItemType Directory -Path $ScratchDir -Force | Out-Null
    $linkPath = Join-Path $ScratchDir ("hardlink_probe_" + $asset.Name)
    if (Test-Path $linkPath) { Remove-Item $linkPath -Force -ErrorAction SilentlyContinue }

    try {
        New-Item -ItemType HardLink -Path $linkPath -Target $asset.FullName -ErrorAction Stop | Out-Null
        $link = Get-Item $linkPath
        $sameSize = ($link.Length -eq $asset.Length)
        $srcHash = Get-FileHash -Path $asset.FullName -Algorithm SHA256 -ErrorAction Stop
        $linkHash = Get-FileHash -Path $linkPath -Algorithm SHA256 -ErrorAction Stop
        $identical = $sameSize -and ($srcHash.Hash -eq $linkHash.Hash)
        if ($identical) {
            Write-RigProbe -Id "A1" -Verdict "PASS" -Detail "hardlinked+read '$($asset.Name)' ($([math]::Round($asset.Length/1MB,1)) MB), bytes identical"
        } else {
            Write-RigProbe -Id "A1" -Verdict "FAIL" -Detail "hardlink of '$($asset.Name)' did not read back identically (sameSize=$sameSize)"
        }
    } catch {
        Write-RigProbe -Id "A1" -Verdict "FAIL" -Detail "hardlink creation/read threw: $($_.Exception.Message)"
    } finally {
        if (Test-Path $linkPath) { Remove-Item $linkPath -Force -ErrorAction SilentlyContinue }
    }
}

# ---- Experiment 2: STEAM-CLOSED LAUNCH (A2) ---------------------------------
function Test-SteamClosedLaunch {
    param([string]$SourceDir, [int]$TestSeconds, [string]$ScratchDir)

    $steam = @(Get-Process -Name steam -ErrorAction SilentlyContinue)
    if ($steam.Count -gt 0) {
        Write-RigProbe -Id "A2" -Verdict "UNKNOWN" -Detail "Steam is running (pid $($steam[0].Id)); close Steam fully and re-run this probe to test the UDP-mode launch without Steam"
        return
    }

    $exe = Join-Path $SourceDir "kenshi_x64.exe"
    if (-not (Test-Path $exe)) {
        Write-RigProbe -Id "A2" -Verdict "UNKNOWN" -Detail "'$exe' not found"
        return
    }

    $rek = (Test-Path (Join-Path $SourceDir "RE_Kenshi.dll")) -or
           (Test-Path (Join-Path $SourceDir "RE_Kenshi")) -or
           (Test-Path (Join-Path $SourceDir "dinput8.dll"))
    if (-not $rek) {
        Write-RigProbe -Id "A2" -Verdict "UNKNOWN" -Detail "RE_Kenshi not installed in '$SourceDir' - cannot confirm the co-op plugin loads; install RE_Kenshi per user_setup, then re-run this probe"
        return
    }

    Assert-NotPrimary -Target $ScratchDir -Primary $SourceDir
    New-Item -ItemType Directory -Path $ScratchDir -Force | Out-Null
    $logPath = Join-Path $ScratchDir "rigprobe_launch.log"
    if (Test-Path $logPath) { Remove-Item $logPath -Force -ErrorAction SilentlyContinue }

    $env:KENSHICOOP_TRANSPORT    = "udp"
    $env:KENSHICOOP_STEAM_PEER   = "0"
    $env:KENSHICOOP_TEST_SECONDS = "$TestSeconds"
    $env:KENSHICOOP_LOG          = $logPath

    Write-Host "  launching '$exe' (Steam closed, KENSHICOOP_TRANSPORT=udp, self-exit in ${TestSeconds}s) ..."
    $proc = Start-Process -FilePath $exe -WorkingDirectory $SourceDir -PassThru

    $deadline = (Get-Date).AddSeconds($TestSeconds + 30)
    $reachedPlugin = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $logPath) {
            $hit = Select-String -Path $logPath -Pattern "PLUGIN-STARTED" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { $reachedPlugin = $true; break }
        }
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }

    if (-not $proc.HasExited) {
        Start-Sleep -Seconds 3
        if (-not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }

    if ($reachedPlugin) {
        Write-RigProbe -Id "A2" -Verdict "PASS" -Detail "kenshi_x64.exe reached PLUGIN-STARTED under KENSHICOOP_TRANSPORT=udp with Steam fully closed"
    } else {
        Write-RigProbe -Id "A2" -Verdict "FAIL" -Detail "kenshi_x64.exe did not reach PLUGIN-STARTED before timeout/exit with Steam closed (see '$logPath')"
    }
}

# ---- A4 helper: ownership-mapping evidence from an existing run dir --------
function Get-RigProbeOwnershipMap {
    # Greps a run dir's log files for the PKT_OWN_RANKS ownership-announcement
    # lines and prints the observed rank->player mapping, so the A4 decision
    # (dynamic-tab creation vs a baked 4-squad start) is reproducible from a
    # run's logs alone.
    #
    # Pattern note: the canonical, currently-emitted grammar is literally
    # "player=<id> rank=<n>" (ReplicatorPublish.cpp's PKT_OWN_RANKS
    # log-oracle, one line per rank assignment - see the comment there: "the
    # exact grammar Phase 4's 4-process harness greps across all instances'
    # logs"). OWN_RANKS/allOwnRanks_/ownRank are kept as a fallback in case an
    # older or future build logs under a different name, but they do NOT
    # match this grammar and would silently report UNKNOWN against a real,
    # correctly-connected run (found empirically in 04-01 Task 3's rerun:
    # tools/test-runs/20260901_235551 had 6 "player=N rank=N" lines that the
    # old pattern missed entirely).
    param([string]$RunDir)

    if (-not (Test-Path $RunDir)) {
        Write-RigProbe -Id "A4" -Verdict "UNKNOWN" -Detail "run dir '$RunDir' not found"
        return
    }
    $logs = Get-ChildItem -Path $RunDir -Filter "*.log" -Recurse -ErrorAction SilentlyContinue
    if (-not $logs -or $logs.Count -eq 0) {
        Write-RigProbe -Id "A4" -Verdict "UNKNOWN" -Detail "no logs found under '$RunDir'"
        return
    }
    $found = @()
    foreach ($log in $logs) {
        $hits = Select-String -Path $log.FullName -Pattern "player=\d+\s+rank=\d+|OWN_RANKS|allOwnRanks_|ownRank" -ErrorAction SilentlyContinue
        foreach ($h in $hits) {
            $found += [pscustomobject]@{ File = $log.Name; Line = $h.LineNumber; Text = $h.Line.Trim() }
        }
    }
    if ($found.Count -eq 0) {
        Write-RigProbe -Id "A4" -Verdict "UNKNOWN" -Detail "no ownership-announcement lines found in $($logs.Count) log(s) under '$RunDir'"
        return
    }
    Write-Host "RIGPROBE A4 ownership lines (rank->player mapping evidence):"
    foreach ($f in $found) {
        Write-Host "  [$($f.File):$($f.Line)] $($f.Text)"
    }
}

# ---- Entry point -------------------------------------------------------------
if ($OwnershipRunDir -ne "") {
    Get-RigProbeOwnershipMap -RunDir $OwnershipRunDir
} else {
    Test-HardlinkSafety -SourceDir $SourceDir -ScratchDir $ScratchDir
    Test-SteamClosedLaunch -SourceDir $SourceDir -TestSeconds $LaunchTestSeconds -ScratchDir $ScratchDir
}
