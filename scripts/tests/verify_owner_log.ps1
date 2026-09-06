<#
.SYNOPSIS
  Phase 5 Plan 03 Task 1 (TEST-03 diagnosability): asserts that ReplicatorChannels.cpp's
  apply-side (received-packet) combat/treatment log lines carry the sender's ownerId.

.DESCRIPTION
  applyCombatHits' three received-hit log lines and applyTreatments' received-treatment
  log line must each print an `owner=%u` token fed by `p.ownerId`, so a hit/treatment can
  be attributed to a specific player at N>2 (a hitId/treatId alone is ambiguous once more
  than one non-host sender exists). This is a cheap, greppable source-text check - it does
  not run the plugin - so it can gate a commit without a live rig.

  Selects non-comment lines in ReplicatorChannels.cpp containing both `coop::logLine`-style
  `_snprintf` format strings and the `owner=%u` token, and fails if fewer than 3 are found
  (the plan's own acceptance floor - the 4th, applyTreatments' TREAT RECV line, is also
  expected but the floor matches the plan's literal `<fails_when>` wording).

  Exit code = number of failed assertions (0 = PASS), matching prototest/Contract.Tests
  so it can gate a commit.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$target = Join-Path $repoRoot "src\plugin\sync\ReplicatorChannels.cpp"

if (-not (Test-Path $target)) {
    Write-Host "FAIL: $target not found"
    exit 1
}

$lines = Get-Content -Path $target
$ownerLines = @()
foreach ($line in $lines) {
    $trimmed = $line.Trim()
    if ($trimmed.StartsWith('//')) { continue }        # skip comment-only lines
    if ($trimmed -notmatch 'owner=%u') { continue }     # must carry the appended field
    if ($trimmed -notmatch '"\[(med|combat)\]') { continue }  # apply-side format string
    $ownerLines += $line
}

$count = $ownerLines.Count
Write-Host "Apply-side [med]/[combat] log lines carrying owner=%u : $count"
foreach ($l in $ownerLines) { Write-Host "  $($l.Trim())" }

if ($count -lt 3) {
    Write-Host "FAIL: expected at least 3 non-comment apply-side log lines with owner=%u, found $count"
    exit 1
}

Write-Host "PASS"
exit 0
