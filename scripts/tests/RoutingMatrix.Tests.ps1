<#
.SYNOPSIS
  Zero-game completeness check for the packet-routing matrix (ROUTE-01, Phase
  1). Runs in milliseconds with NO game launch and NO built DLL - it only
  reads src\netproto\Wire.h and the two Phase 1 docs, so it can gate every
  commit and every later relay-implementation phase.

.DESCRIPTION
  Asserts the routing-matrix contract:
    * completeness  - every PKT_*/EVT_* enum value declared in Wire.h appears
                      in docs\ROUTING_MATRIX.md's classification table
    * no-typo       - every PKT_*/EVT_* identifier named in the matrix doc is
                      a real Wire.h enum value (catches typos/renames)
    * no-duplicate  - no identifier appears in more than one matrix row
    * grep-sweep    - every file the MAIN_GOAL two-player terminology sweep
                      surfaces is reconciled (named as a finding or in the
                      "Reviewed" allowlist) in docs\TWO_PLAYER_ASSUMPTIONS.md

  Also includes a NEGATIVE fixture path (see the -MatrixPath param and the
  Task 1 verify wrapper in 01-01-PLAN.md) that proves the completeness check
  actually fires on a matrix missing a row - a green run on the real files
  means the guard works, not that it was skipped.

  The script extracts identifiers via Select-String/regex only. It never
  dot-sources, invokes, or expression-evaluates the parsed file contents
  (Wire.h, ROUTING_MATRIX.md, and TWO_PLAYER_ASSUMPTIONS.md are all read as
  untrusted text - see PLAN.md threat T-01-01).

  Exit code = number of failed assertions (0 = PASS), matching the
  Contract.Tests.ps1 / prototest convention so verify.ps1 can sum them.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\tests\RoutingMatrix.Tests.ps1

.EXAMPLE
  # Point at a fixture copy to prove the completeness guard fires.
  powershell -ExecutionPolicy Bypass -File scripts\tests\RoutingMatrix.Tests.ps1 -MatrixPath $env:TEMP\rm_matrix.md
#>
[CmdletBinding()]
param(
    [string]$WirePath      = "src\netproto\Wire.h",
    [string]$MatrixPath    = "docs\ROUTING_MATRIX.md",
    [string]$InventoryPath = "docs\TWO_PLAYER_ASSUMPTIONS.md"
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                        # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                      # repo root

# Resolve relative param defaults against the repo root so the script also
# works when invoked with a cwd other than the repo root (verify.ps1 always
# runs from the repo root, but the fixture wrapper in 01-01-PLAN.md Task 1
# passes an absolute -MatrixPath from $env:TEMP - that must pass through
# untouched).
function Resolve-InputPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $repoRoot $Path)
}
$wireFile      = Resolve-InputPath $WirePath
$matrixFile    = Resolve-InputPath $MatrixPath
$inventoryFile = Resolve-InputPath $InventoryPath

# ---- tiny assert harness ------------------------------------------------------
# (verbatim shape from scripts\tests\Contract.Tests.ps1:40-47)
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

# ---- extractors -----------------------------------------------------------

# Every PKT_*/EVT_* identifier declared in Wire.h's PacketType/EventType enums.
# \s* (not \s+) before '=' because PKT_WORLD_ITEM_REMOVE= 8 has no space.
function Get-WireEnumValues {
    param([string]$Path)
    $ids = New-Object System.Collections.Generic.HashSet[string]
    if (-not (Test-Path $Path)) { return $ids }
    foreach ($pattern in @('PKT_[A-Z_]+\s*=\s*\d+', 'EVT_[A-Z_]+\s*=\s*\d+')) {
        foreach ($m in (Select-String -Path $Path -Pattern $pattern)) {
            foreach ($mm in $m.Matches) {
                $id = ($mm.Value -split '\s*=\s*')[0].Trim()
                [void]$ids.Add($id)
            }
        }
    }
    return $ids
}

# Every PKT_*/EVT_* identifier occurrence in the matrix doc's table DATA rows
# (lines shaped like "| 1 | `PKT_HELLO` | ..."), with a per-identifier
# occurrence count so the caller can assert "exactly once". Restricted to
# table rows (not prose/legend/comment text elsewhere in the doc) so a
# narrative mention of an identifier (e.g. this file's own completeness-note
# prose, which names PKT_HELLO and EVT_NONE again outside the table) does not
# register as a second row.
function Get-MatrixDocIds {
    param([string]$Path)
    $counts = @{}
    if (-not (Test-Path $Path)) { return $counts }
    $rowPattern = '^\s*\|\s*\d+\s*\|\s*`(PKT_[A-Z_]+|EVT_[A-Z_]+)`'
    foreach ($m in (Select-String -Path $Path -Pattern $rowPattern)) {
        $id = $m.Matches[0].Groups[1].Value
        if ($counts.ContainsKey($id)) { $counts[$id]++ } else { $counts[$id] = 1 }
    }
    return $counts
}

# ---- 1. matrix completeness ---------------------------------------------------
Write-Host "== routing matrix completeness =="

$wireIds   = Get-WireEnumValues -Path $wireFile
$docCounts = Get-MatrixDocIds -Path $matrixFile
$docIds    = New-Object System.Collections.Generic.HashSet[string]
foreach ($k in $docCounts.Keys) { [void]$docIds.Add($k) }

Check "parsed Wire.h enum values" ($wireIds.Count -gt 0)

$missingFromDoc = @()
foreach ($id in $wireIds) {
    if (-not $docIds.Contains($id)) { $missingFromDoc += $id }
}
if ($missingFromDoc.Count -gt 0) {
    Write-Host ("      Wire.h identifiers missing from matrix: " + ($missingFromDoc -join ', '))
}
Check "every Wire.h enum value appears in the matrix" ($missingFromDoc.Count -eq 0)

$notRealIds = @()
foreach ($id in $docIds) {
    if (-not $wireIds.Contains($id)) { $notRealIds += $id }
}
if ($notRealIds.Count -gt 0) {
    Write-Host ("      matrix identifiers not in Wire.h: " + ($notRealIds -join ', '))
}
Check "every matrix identifier is a real Wire.h identifier" ($notRealIds.Count -eq 0)

$duplicates = @()
foreach ($id in $docCounts.Keys) {
    if ($docCounts[$id] -gt 1) { $duplicates += "$id (x$($docCounts[$id]))" }
}
if ($duplicates.Count -gt 0) {
    Write-Host ("      duplicate matrix rows: " + ($duplicates -join ', '))
}
Check "no identifier appears more than once in the matrix" ($duplicates.Count -eq 0)

# ---- 2. grep-sweep cross-reference (two-player-assumption inventory) ---------
# Locked decision (CONTEXT.md): automated grep sweep for the MAIN_GOAL
# terminology list, reconciled against docs\TWO_PLAYER_ASSUMPTIONS.md. Bucket
# by FILE (not raw line) per RESEARCH.md Pitfall 3 - a flat per-line check
# against high-noise terms like "seq"/"peer"/"ack" is unusable (hundreds of
# hits, most already-fine per-sender-monotonic comments).
Write-Host "== two-player-assumption grep-sweep cross-reference =="

$terms = @('peer', 'join', 'serverPeer', 'peerPresent', 'ownerId',
           'OWNER_ID_ALL', 'ack', 'seq', 'broadcast')
$srcDir = Join-Path $repoRoot "src"

$hitFiles = New-Object System.Collections.Generic.HashSet[string]
$geeErrors = @()
if (Test-Path $srcDir) {
    # -ErrorVariable (works alongside -ErrorAction SilentlyContinue on
    # PowerShell 5.1) captures any file the enumeration couldn't stat/read
    # (locked file, long-path issue, permission glitch, etc.) instead of
    # silently dropping it from $candidates - the whole point of this sweep
    # is completeness, so a file invisible to the tool must be surfaced, not
    # indistinguishable from "0 hits, file is clean".
    $candidates = Get-ChildItem -Path $srcDir -Recurse -Include *.h, *.cpp -ErrorVariable geeErrors -ErrorAction SilentlyContinue
    if ($geeErrors.Count -gt 0) {
        $geeMessages = ($geeErrors | ForEach-Object { $_.ToString() } | Select-Object -Unique -First 5) -join '; '
        Write-Host ("      WARNING: " + $geeErrors.Count + " file(s) could not be enumerated under src\: " + $geeMessages)
    }
    foreach ($f in $candidates) {
        foreach ($term in $terms) {
            if (Select-String -Path $f.FullName -Pattern $term -SimpleMatch -Quiet) {
                $rel = $f.FullName.Substring($repoRoot.Length).TrimStart('\', '/')
                [void]$hitFiles.Add(($rel -replace '\\', '/'))
                break
            }
        }
    }
}
Check "grep sweep found candidate files" ($hitFiles.Count -gt 0)
Check "no file-enumeration errors when scanning src\ for the two-player terminology sweep" ($geeErrors.Count -eq 0)

$inventoryText = ""
if (Test-Path $inventoryFile) { $inventoryText = Get-Content -Path $inventoryFile -Raw }

$unreconciled = @()
foreach ($f in $hitFiles) {
    $base = Split-Path -Leaf $f
    # Reconciled if the doc names either the repo-relative path or just the
    # bare filename (findings/allowlist entries commonly cite "NetLink.cpp"
    # without the full src/plugin/net/ prefix).
    $named = ($inventoryText.IndexOf($f, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) -or
             ($inventoryText.IndexOf($base, [System.StringComparison]::OrdinalIgnoreCase) -ge 0)
    if (-not $named) { $unreconciled += $f }
}
if ($unreconciled.Count -gt 0) {
    Write-Host ("      unreconciled terminology-hit files: " + ($unreconciled -join ', '))
}
Check "every terminology-hit file is referenced or allowlisted in the inventory" ($unreconciled.Count -eq 0)

# ---- summary --------------------------------------------------------------
Write-Host ""
Write-Host ("routing matrix completeness: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
