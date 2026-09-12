<#
.SYNOPSIS
  Measure a whole directory tree: one sha256 per file, plus a marker per empty
  directory, sorted by path with an ordinal comparer.

.DESCRIPTION
  THIS IS THE SINGLE IMPLEMENTATION OF THAT MEASUREMENT. Phase 16's criterion 4
  - "uninstall returns the install tree to a byte-identical pre-install state" -
  rests entirely on it, and two copies of a measurement can disagree, so plans
  16-02 and 16-03 consume this rather than reimplementing it. The POSIX front
  end in 16-02 must produce BYTE-IDENTICAL output, which is why the format is
  deliberately dull.

  OUTPUT FORMAT, one entry per line, LF-terminated including the last line:

      <64 lowercase hex sha256><space><path><LF>          for a file
      emptydir<space><path>/<LF>                          for an EMPTY directory

  Paths are relative to -Root and use forward slashes, with no leading "./".
  Empty directories are listed because criterion 4 compares the FILE SET as
  well as the bytes: an uninstall that leaves an empty mods\ behind has not
  returned the tree to its pre-install state, and a files-only walk cannot see
  that.

  Sorting is [StringComparer]::Ordinal on the path, NOT the culture collation -
  this machine runs uk-UA and a culture-aware sort would reorder the output
  between machines, which is exactly the kind of difference that makes a
  before/after comparison lie.

  With -OutFile the file is written UTF-8 WITHOUT a BOM. Windows PowerShell
  5.1's "-Encoding utf8" writes a BOM, and a BOM breaks a first-line match for
  every later grep - the trap docs\CROSS_MACHINE_RIG.md section 5e records.

.PARAMETER Root
  The directory to measure.

.PARAMETER Exclude
  Glob patterns matched (with -like) against each entry's RELATIVE path, e.g.
  "mods/KenshiCoop.backup/*" or "*.log". An excluded entry is omitted entirely.

.PARAMETER OutFile
  Write the measurement here instead of to standard output. Use this form when
  the bytes matter: standard output goes through the console encoding.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\hash_tree.ps1 -Root "F:\KenshiCoop-Clone3" -OutFile "F:\...\before.txt"

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\hash_tree.ps1 -Root "F:\KenshiCoop-Clone3" -Exclude "*.log" "RE_Kenshi_log.txt"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Root,
    [string[]]$Exclude = @(),
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Root)) {
    Write-Host "REFUSED: there is no directory at '$Root'."
    exit 3
}
$rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')

# Accept both an array and one ';'-separated string, for the same reason
# install_coop.ps1 does: under -File an array parameter cannot take more than
# one bare token.
$excludes = @()
foreach ($e in $Exclude) {
    foreach ($q in ("$e" -split ';')) {
        $q = $q.Trim()
        if ($q) { $excludes += $q }
    }
}

function Rel([string]$full) {
    $f = [System.IO.Path]::GetFullPath($full)
    if ($f.Length -le ($rootFull.Length + 1)) { return "" }
    return ($f.Substring($rootFull.Length + 1) -replace '\\', '/')
}

function Excluded([string]$rel) {
    foreach ($p in $excludes) { if ($rel -like $p) { return $true } }
    return $false
}

function Sha256([string]$path) {
    # .NET rather than Get-FileHash: Get-FileHash honours an inherited -WhatIf
    # and silently returns nothing, which would print a blank hash here and make
    # two different trees compare equal.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($path))
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

$entries = New-Object System.Collections.Generic.List[object]

foreach ($f in @(Get-ChildItem -LiteralPath $rootFull -Recurse -File -Force -ErrorAction SilentlyContinue)) {
    $rel = Rel $f.FullName
    if (-not $rel) { continue }
    if (Excluded $rel) { continue }
    [void]$entries.Add([pscustomobject]@{ key = $rel; line = ((Sha256 $f.FullName) + " " + $rel) })
}

foreach ($d in @(Get-ChildItem -LiteralPath $rootFull -Recurse -Directory -Force -ErrorAction SilentlyContinue)) {
    $rel = Rel $d.FullName
    if (-not $rel) { continue }
    if (Excluded ($rel + "/")) { continue }
    $kids = @(Get-ChildItem -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue)
    if ($kids.Count -ne 0) { continue }
    [void]$entries.Add([pscustomobject]@{ key = ($rel + "/"); line = ("emptydir " + $rel + "/") })
}

# Ordinal, not the uk-UA collation. Sort-Object is deliberately NOT used: it
# sorts with the current culture unless told otherwise, and the sort order is
# part of the output this measurement is compared byte-for-byte on.
$keys = New-Object System.Collections.Generic.List[string]
foreach ($e in $entries) { [void]$keys.Add($e.key) }
$byKey = @{}
foreach ($e in $entries) { $byKey[$e.key] = $e.line }
$ordinal = @($keys.ToArray())
[Array]::Sort($ordinal, [System.StringComparer]::Ordinal)

$sb = New-Object System.Text.StringBuilder
foreach ($k in $ordinal) { [void]$sb.Append($byKey[$k]); [void]$sb.Append("`n") }
$text = $sb.ToString()

if ($OutFile) {
    if (-not [System.IO.Path]::IsPathRooted($OutFile)) {
        Write-Host "REFUSED: -OutFile must be an ABSOLUTE path (got '$OutFile'). A relative path is swallowed silently and nothing is written."
        exit 3
    }
    $parent = Split-Path -Parent ([System.IO.Path]::GetFullPath($OutFile))
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($OutFile), $text, $enc)
} else {
    [Console]::Out.Write($text)
}
exit 0
