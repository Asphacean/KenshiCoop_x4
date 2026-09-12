<#
.SYNOPSIS
  Headless guard for the fresh-install runner, the Release payload and the
  auto-detect path (Phase 16 Plan 03, INST-01/04/05).

.DESCRIPTION
  Picked up by verify.ps1's scripts\tests\Installer*.Tests.ps1 glob, so
  verify.ps1 is not edited again.

  GROUP R - the auto-detect regression a REAL PLAYER found. install_coop.ps1's
  Find-KenshiInstalls ends with `return @($cands)`, and the `@()` is
  load-bearing: PowerShell UNWRAPS a single-element array on return, so with
  exactly ONE Kenshi install found $found became a bare STRING. A string has
  .Count = 1, so both the zero-candidate and the multi-candidate guard passed,
  and $found[0] then yielded its first CHARACTER - "C" - which GetFullPath
  resolved against the current directory. The player saw
  "auto-detected install: C:\steam\steamapps\common\Kenshi\C". Fixed in
  9b7665a. The 94-check suite stayed green through all of it because it does
  not cover auto-detection at all: this rig has several Kenshi clones so
  auto-detect always took the ">1" branch, and the tests pass -KenshiDir
  explicitly. Exactly one install is what every real player has and is the only
  case that was never exercised.

  Group R drives the SHIPPED text of Resolve-KenshiDir - install_coop.ps1 is
  read, cut at its dispatch marker and dot-sourced - with only the candidate
  PRODUCER stubbed, so the consumer under test is the real one. R6 re-applies
  the defect and requires R2 to flip.

  GROUP A - freshinstall_check.ps1's own refusals. A refusal path that has
  never fired is a refusal path that does not work.

  GROUP B - Release is what ships. The payload the installer resolves by
  default must BE the Release build, the Release build must carry the F2
  panel's own strings and must NOT carry the harness-only scenario
  identifiers, and the Harness build must carry them so the check is
  discriminating rather than merely quiet.

  GROUP C - mutation sensitivity. One mutation at a time, each required to
  turn a Group A/B/R verdict red.

  Exit code = number of failed checks (0 = PASS).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\tests\InstallerRelease.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$INV         = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptsRoot = Split-Path -Parent $scriptDir
$repoRoot    = Split-Path -Parent $scriptsRoot

$installer  = Join-Path $scriptsRoot "install_coop.ps1"
$fresh      = Join-Path $scriptsRoot "freshinstall_check.ps1"
$releaseDll = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"
$harnessDll = Join-Path $repoRoot "src\plugin\x64\Harness\KenshiCoop.dll"

$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

$UTF8NB = New-Object System.Text.UTF8Encoding($false)
function WriteText([string]$path, [string]$text) {
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($path, $text, $UTF8NB)
}
function Sha256File([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return "" }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLowerInvariant()
}
# Does a binary contain this ASCII literal? Read as Latin-1 so every byte maps
# to exactly one char and a UTF-16 string literal in the binary still matches
# when searched for with its bytes interleaved - both encodings are tried.
function BinaryHas([string]$path, [string]$needle) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text  = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    if ($text.Contains($needle)) { return $true }
    $wide = ($needle.ToCharArray() | ForEach-Object { "$_`0" }) -join ""
    return $text.Contains($wide)
}

$fixRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_relfix_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $fixRoot | Out-Null

Write-Host "== InstallerRelease: auto-detect regression, fresh-install refusals, Release payload =="

Check "scripts\install_coop.ps1 present"       (Test-Path $installer)
Check "scripts\freshinstall_check.ps1 present" (Test-Path $fresh)
if (-not (Test-Path $installer) -or -not (Test-Path $fresh)) {
    Write-Host ""
    Write-Host ("InstallerRelease: {0}/{1} checks passed - FAIL" -f $script:Pass, ($script:Pass + $script:Fail))
    exit $script:Fail
}

# ======================================================================= R ===
# The auto-detect regression a real player found.
Write-Host ""
Write-Host "-- GROUP R: single-install auto-detect (the path no test covered) --"

$DispatchMarker = "# ================================================================ dispatch ===="
$instText = [System.IO.File]::ReadAllText($installer)
$cut = $instText.IndexOf($DispatchMarker)
Check "R1 install_coop.ps1 carries the dispatch marker Group R cuts at" ($cut -gt 0)

$autoRoot = Join-Path $fixRoot "autodetect"
New-Item -ItemType Directory -Force -Path $autoRoot | Out-Null

# A synthetic install whose path starts with a drive letter, so the defect's
# signature (the FIRST CHARACTER of the path) is unmistakable.
$oneInstall = Join-Path $autoRoot "kenshi_one"
$twoInstall = Join-Path $autoRoot "kenshi_two"
foreach ($d in @($oneInstall, $twoInstall)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    WriteText (Join-Path $d "kenshi_x64.exe") "not a real exe"
}

$driver = Join-Path $autoRoot "drive_resolve.ps1"
WriteText $driver @'
param([string]$Prefix, [string]$C1 = "NONE", [string]$C2 = "NONE")
$ErrorActionPreference = "Stop"
# "NONE" means "no candidate": under powershell -File an empty string argument
# is dropped and a bare "-" is parsed as a parameter name, so a word sentinel is
# the only way to drive the zero-candidate case.
if ($C1 -eq "NONE") { $C1 = "" }
if ($C2 -eq "NONE") { $C2 = "" }
# Dot-source the SHIPPED text of install_coop.ps1 up to its dispatch line, so
# Resolve-KenshiDir under test is the real one, not a copy.
. $Prefix
# Only the candidate PRODUCER is stubbed. It returns an honest array; what is
# being tested is how the CONSUMER handles a one-element result.
function Find-KenshiInstalls() {
    $l = New-Object System.Collections.Generic.List[string]
    if ($C1) { [void]$l.Add($C1) }
    if ($C2) { [void]$l.Add($C2) }
    return @($l)
}
$r = Resolve-KenshiDir ""
Write-Host ("RESOLVED=" + $r)
exit 0
'@

function Run-Resolve([string]$prefixPath, [string]$c1, [string]$c2) {
    # Function-scoped: a child that writes to stderr must not abort the suite.
    $ErrorActionPreference = "Continue"
    if (-not $c1) { $c1 = "NONE" }
    if (-not $c2) { $c2 = "NONE" }
    $out = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $driver -Prefix $prefixPath -C1 $c1 -C2 $c2 2>&1 | ForEach-Object { "$_" })
    return @{ exit = $LASTEXITCODE; text = ($out -join "`n") }
}

$prefixGood = Join-Path $autoRoot "installer_prefix.ps1"
if ($cut -gt 0) { WriteText $prefixGood $instText.Substring(0, $cut) }

$r2 = Run-Resolve $prefixGood $oneInstall ""
Check "R2 EXACTLY ONE install auto-detected: resolves to that install, exit 0" `
    ($r2.exit -eq 0 -and $r2.text.Contains("RESOLVED=" + $oneInstall))
Check "R2b the resolved path is not the defect's first-character artifact" `
    (-not ($r2.text -match 'RESOLVED=.*\\[A-Za-z]$'))
Check "R2c the player-facing line names the install it auto-detected" `
    ($r2.text.Contains("auto-detected install: " + $oneInstall))

$r4 = Run-Resolve $prefixGood "" ""
Check "R4 ZERO installs: refuses and tells the player to pass -KenshiDir" `
    ($r4.exit -ne 0 -and $r4.text.Contains("-KenshiDir") -and $r4.text -match "no Kenshi installation was found")

$r5 = Run-Resolve $prefixGood $oneInstall $twoInstall
Check "R5 TWO installs: refuses and names BOTH of them" `
    ($r5.exit -ne 0 -and $r5.text.Contains($oneInstall) -and $r5.text.Contains($twoInstall))

# R6 - the mutation: put the shipped defect back and require R2 to flip.
$prefixBad = Join-Path $autoRoot "installer_prefix_mutated.ps1"
$mutText = $instText.Substring(0, [Math]::Max($cut, 0)).Replace('$found = @(Find-KenshiInstalls)', '$found = Find-KenshiInstalls')
WriteText $prefixBad $mutText
Check "R6a the mutation actually applied (the @() was removed)" `
    ($cut -gt 0 -and $mutText.Contains('$found = Find-KenshiInstalls') -and -not $mutText.Contains('$found = @(Find-KenshiInstalls)'))
$r6 = Run-Resolve $prefixBad $oneInstall ""
Check "R6b with the @() reverted, the single-install case NO LONGER resolves correctly" `
    (-not ($r6.exit -eq 0 -and $r6.text.Contains("RESOLVED=" + $oneInstall)))
Check "R6c the mutated run reproduces the player's symptom (a one-character tail)" `
    ($r6.text -match '\\[A-Za-z]''? is not a Kenshi installation' -or $r6.text -match 'RESOLVED=.*\\[A-Za-z]$')

# ======================================================================= A ===
Write-Host ""
Write-Host "-- GROUP A: freshinstall_check.ps1's refusals --"

$outOk = Join-Path $fixRoot "out"
New-Item -ItemType Directory -Force -Path $outOk | Out-Null

function Run-Fresh([string[]]$ExtraArgs) {
    $ErrorActionPreference = "Continue"
    $all = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $fresh) + $ExtraArgs
    $out = @(& powershell @all 2>&1 | ForEach-Object { "$_" })
    return @{ exit = $LASTEXITCODE; text = ($out -join "`n") }
}
function RefusedLine([string]$text) {
    # Assert on the REFUSED line alone, so an [ok] elsewhere in the transcript
    # cannot satisfy a refusal check (16-02's rule).
    $m = [regex]::Match($text, '(?m)^REFUSED: (.+)$')
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

$prot = "F:\SteamLibrary\steamapps\common\Kenshi"
$a1 = Run-Fresh @("-KenshiDir", $prot, "-OutDir", $outOk, "-NoLaunch")
$a1r = RefusedLine $a1.text
Check "A1 a PROTECTED Steam install is refused, by name, before any delete" `
    ($a1.exit -eq 2 -and $a1r.Contains($prot) -and $a1r.Contains("PROTECTED"))

$noExe = Join-Path $fixRoot "no_exe"
New-Item -ItemType Directory -Force -Path $noExe | Out-Null
$a2 = Run-Fresh @("-KenshiDir", $noExe, "-OutDir", $outOk, "-NoLaunch")
$a2r = RefusedLine $a2.text
Check "A2 a directory with no kenshi_x64.exe is refused, naming the directory" `
    ($a2.exit -eq 2 -and $a2r.Contains($noExe) -and $a2r.Contains("kenshi_x64.exe"))

# A clean fixture the later checks reuse.
function New-KenshiFixture([string]$name) {
    $d = Join-Path $fixRoot $name
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    WriteText (Join-Path $d "kenshi_x64.exe") "not a real exe"
    return $d
}

$relFix = New-KenshiFixture "relout"
$a3 = Run-Fresh @("-KenshiDir", $relFix, "-OutDir", "tools\test-runs\relative", "-NoLaunch")
$a3r = RefusedLine $a3.text
Check "A3 a RELATIVE -OutDir is refused (a swallowed path writes no evidence)" `
    ($a3.exit -eq 2 -and $a3r.Contains("RELATIVE"))

$a4 = Run-Fresh @("-KenshiDir", $relFix, "-OutDir", "F:tools", "-NoLaunch")
$a4r = RefusedLine $a4.text
Check "A4 a DRIVE-RELATIVE -OutDir ('F:tools') is refused by name" `
    ($a4.exit -eq 2 -and $a4r.Contains("DRIVE-RELATIVE"))

# A5 - a Kenshi process running OUT OF THE TARGET. A copy of cmd.exe named
# kenshi_x64.exe gives a process whose name and image path are both the target's.
$runFix = New-KenshiFixture "running"
$runExe = Join-Path $runFix "kenshi_x64.exe"
Copy-Item (Join-Path $env:SystemRoot "System32\cmd.exe") $runExe -Force
$proc = Start-Process -FilePath $runExe -ArgumentList "/c", "ping -n 40 127.0.0.1 >nul" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 900
$a5 = Run-Fresh @("-KenshiDir", $runFix, "-OutDir", $outOk, "-NoLaunch")
$a5r = RefusedLine $a5.text
Check "A5 a Kenshi RUNNING out of the target is refused (a loaded DLL cannot be replaced)" `
    ($a5.exit -eq 2 -and $a5r.Contains("RUNNING") -and $a5r.Contains($runFix))
try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
Start-Sleep -Milliseconds 300

# A6 - coop_config.json that cannot be removed, at the cwd-fallback path.
$cfgFix = New-KenshiFixture "lockedcfg"
$cfgPath = Join-Path $cfgFix "coop_config.json"
WriteText $cfgPath "{}"
$lock = [System.IO.File]::Open($cfgPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $a6 = Run-Fresh @("-KenshiDir", $cfgFix, "-OutDir", $outOk, "-NoLaunch")
} finally { $lock.Dispose() }
$a6r = RefusedLine $a6.text
Check "A6 a coop_config.json that cannot be removed is refused, naming the path" `
    ($a6.exit -eq 2 -and $a6r.Contains("coop_config.json") -and $a6r.Contains($cfgPath))
Check "A6b the refusal says why a stale config matters (an invisible input)" `
    ($a6r.Contains("INVISIBLE INPUT"))

# A7 - a KENSHICOOP_* variable that survives the scrub.
$envFix = New-KenshiFixture "envsurvivor"
$a7 = Run-Fresh @("-KenshiDir", $envFix, "-OutDir", $outOk, "-NoLaunch", "-InjectEnvSurvivor", "KENSHICOOP_TEST_SURVIVOR")
$a7r = RefusedLine $a7.text
Check "A7 a KENSHICOOP_* variable surviving the scrub is refused, naming it" `
    ($a7.exit -eq 2 -and $a7r.Contains("KENSHICOOP_TEST_SURVIVOR") -and $a7r.Contains("INHERIT"))

# A8 - everything clean: the guards pass and the preflight record is written.
$cleanFix = New-KenshiFixture "clean"
$cleanOut = Join-Path $fixRoot "clean_out"
$a8 = Run-Fresh @("-KenshiDir", $cleanFix, "-OutDir", $cleanOut, "-NoLaunch")
$pf = Join-Path $cleanOut "preflight.json"
Check "A8 a clean target gets past every guard and a preflight record is written" (Test-Path $pf)
$pfj = $null
if (Test-Path $pf) { $pfj = Get-Content -Raw $pf | ConvertFrom-Json }
Check "A8b the record names BOTH coop_config.json paths Config.cpp can resolve" `
    ($pfj -ne $null -and
     "$($pfj.configPaths.besideDll)" -eq (Join-Path $cleanFix "mods\KenshiCoop\coop_config.json") -and
     "$($pfj.configPaths.cwdFallback)" -eq (Join-Path $cleanFix "coop_config.json"))
Check "A8c the record names the mod directory and the backup directory as well" `
    ($pfj -ne $null -and @($pfj.freshPathsChecked).Count -eq 4)
Check "A8d the KENSHICOOP_* set was SCANNED out of src\, not hand-written (>= 100 names)" `
    ($pfj -ne $null -and [int]$pfj.envNameCount -ge 100)
Check "A8e a run that could not finish exits NON-ZERO (it must not read as green)" `
    ($a8.exit -ne 0)

# A9 - -WhatIf prints the whole plan and changes nothing.
$whatFix = New-KenshiFixture "whatif"
WriteText (Join-Path $whatFix "coop_config.json") "{}"
$a9 = Run-Fresh @("-KenshiDir", $whatFix, "-OutDir", $outOk, "-WhatIf")
Check "A9 -WhatIf exits 0 and prints the plan" `
    ($a9.exit -eq 0 -and $a9.text.Contains("WOULD delete then assert absent") -and $a9.text.Contains("nothing changed"))
Check "A9b -WhatIf changed nothing: the config file is still there" `
    (Test-Path -LiteralPath (Join-Path $whatFix "coop_config.json"))

# ======================================================================= B ===
Write-Host ""
Write-Host "-- GROUP B: Release is what ships --"

Check "B1 the canonical Release DLL exists" (Test-Path $releaseDll)

# What the installer resolves BY DEFAULT, taken from its own -WhatIf transcript
# (the artifact), not from a regex over its source.
$payFix = New-KenshiFixture "payload"
$ErrorActionPreference = "Continue"
$payOut = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -KenshiDir $payFix -WhatIf 2>&1 | ForEach-Object { "$_" })
$ErrorActionPreference = "Stop"
$payText = ($payOut -join "`n")
$payMatch = [regex]::Match($payText, '(?m)^\s*payload:\s*(.+?)\s*$')
$payDir = ""
if ($payMatch.Success) { $payDir = $payMatch.Groups[1].Value }
Check "B2 install_coop.ps1 reports the payload it would deploy" ($payDir -ne "")
$payDll = ""
if ($payDir -and (Test-Path -LiteralPath $payDir)) { $payDll = Join-Path $payDir "KenshiCoop.dll" }
Check "B3 the default payload's DLL is byte-identical to the canonical Release build" `
    ($payDll -ne "" -and (Sha256File $payDll) -ne "" -and (Sha256File $payDll) -eq (Sha256File $releaseDll))

Check "B4 the Release build carries the F2 panel's own strings" `
    ((BinaryHas $releaseDll "Copy my Steam ID") -and (BinaryHas $releaseDll "Paste friend"))
Check "B5 the Release build carries NO harness-only scenario identifier 'connect_relink'" `
    (-not (BinaryHas $releaseDll "connect_relink"))
Check "B6 the Release build carries NO harness-only scenario identifier 'MAINTID'" `
    (-not (BinaryHas $releaseDll "MAINTID"))
if (Test-Path $harnessDll) {
    Check "B7 the Harness build DOES carry 'connect_relink' (so B5 is discriminating, not quiet)" `
        (BinaryHas $harnessDll "connect_relink")
    Check "B8 the Harness build DOES carry 'MAINTID' (so B6 is discriminating, not quiet)" `
        (BinaryHas $harnessDll "MAINTID")
    Check "B9 Release and Harness are different binaries" `
        ((Sha256File $harnessDll) -ne (Sha256File $releaseDll))
} else {
    Check "B7 the Harness build is present so B5/B6 can be shown to discriminate" $false
}

# ======================================================================= C ===
Write-Host ""
Write-Host "-- GROUP C: mutation sensitivity (a green run must mean the test LOOKED) --"

$freshText = [System.IO.File]::ReadAllText($fresh)
# A mutant MUST live in scripts\ : freshinstall_check.ps1 derives $repoRoot from
# its own location, and a copy in the temp directory would refuse on the empty
# src\ scan long before reaching the behaviour under test - which would make
# every Group C check pass for the wrong reason.
$mutDir  = $scriptsRoot
$script:MutantFiles = @()

function Run-Mutant([string]$name, [string]$text, [string[]]$ExtraArgs) {
    $ErrorActionPreference = "Continue"
    $p = Join-Path $mutDir ("_mutant_" + $name + ".ps1")
    $script:MutantFiles += $p
    WriteText $p $text
    $all = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $p) + $ExtraArgs
    $out = @(& powershell @all 2>&1 | ForEach-Object { "$_" })
    return @{ exit = $LASTEXITCODE; text = ($out -join "`n") }
}

# C1 - drop the cwd-fallback config path from the both-paths assertion.
$c1text = $freshText.Replace('$freshPaths = @($modDir, $backupDir, $cfgBesideDll, $cfgCwd)',
                             '$freshPaths = @($modDir, $backupDir, $cfgBesideDll)')
Check "C1a the config-path mutation applied" ($c1text -ne $freshText)
$c1fix = New-KenshiFixture "mut_cfg"
$c1cfg = Join-Path $c1fix "coop_config.json"
WriteText $c1cfg "{}"
$lock1 = [System.IO.File]::Open($c1cfg, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try { $c1 = Run-Mutant "drop_cwd_config" $c1text @("-KenshiDir", $c1fix, "-OutDir", $outOk, "-NoLaunch") }
finally { $lock1.Dispose() }
Check "C1b the mutant got as far as the delete-then-assert step (not a refusal upstream)" `
    ($c1.text.Contains("delete, then ASSERT ABSENT"))
Check "C1c with the cwd-fallback path dropped, A6's refusal no longer fires" `
    ((RefusedLine $c1.text) -notmatch 'coop_config\.json could not be removed')

# C2 - replace the src\ scan with a hand-written short list.
$c2text = $freshText.Replace('$envNames = @($scanned) | Sort-Object',
                             '$envNames = @("KENSHICOOP_ROLE", "KENSHICOOP_PORT") | Sort-Object')
Check "C2a the hand-list mutation applied" ($c2text -ne $freshText)
$c2fix = New-KenshiFixture "mut_scan"
$c2out = Join-Path $fixRoot "mut_scan_out"
$c2 = Run-Mutant "hand_list" $c2text @("-KenshiDir", $c2fix, "-OutDir", $c2out, "-NoLaunch")
$c2pf = Join-Path $c2out "preflight.json"
$c2j = $null
if (Test-Path $c2pf) { $c2j = Get-Content -Raw $c2pf | ConvertFrom-Json }
Check "C2b with a hand list in place, A8d's >= 100 scanned names turns red" `
    ($c2j -ne $null -and [int]$c2j.envNameCount -lt 100)

# C3 - skip the deployed-DLL hash comparison.
$c3text = $freshText.Replace('if ($deployedSha -eq "" -or $deployedSha -ne $canonSha) {',
                             'if ($false) {')
Check "C3a the DLL-comparison mutation applied" ($c3text -ne $freshText)
Check "C3b the unmutated runner still HAS the byte-identity assertion it removes" `
    ($freshText.Contains('byte-identical to src\plugin\x64\Release\KenshiCoop.dll'))

# C4 - remove the payload freshness refusal.
$c4text = $freshText.Replace('if ($kitSha -ne $relSha0) {', 'if ($false) {')
Check "C4a the payload-freshness mutation applied" ($c4text -ne $freshText)

# ======================================================================= D ===
# The release gate. A gate that can only ever say one thing is not a gate, so
# the verdict is driven against synthetic blockers files in BOTH states.
Write-Host ""
Write-Host "-- GROUP D: the release gate (docs\RELEASE_BLOCKERS.md -> PROVENANCE.json) --"

$blockersDoc = Join-Path $repoRoot "docs\RELEASE_BLOCKERS.md"
$kitScript   = Join-Path $scriptsRoot "make_mod_kit.ps1"
$tracked = $false
try {
    $ErrorActionPreference = "Continue"
    $tracked = (@(& git -C $repoRoot ls-files -- "docs/RELEASE_BLOCKERS.md" 2>$null).Count -gt 0)
} catch { $tracked = $false } finally { $ErrorActionPreference = "Stop" }
Check "D1 docs\RELEASE_BLOCKERS.md exists and is TRACKED (docs\PHASE_*_GATE.md is not)" `
    ((Test-Path $blockersDoc) -and $tracked)

function Get-Verdict([string]$file) {
    $ErrorActionPreference = "Continue"
    $out = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $kitScript -VerdictOnly -BlockersFile $file 2>&1 | ForEach-Object { "$_" })
    $txt = ($out -join "`n")
    try { return ($txt | ConvertFrom-Json) } catch { return $null }
}

$vReal = Get-Verdict $blockersDoc
Check "D2 make_mod_kit.ps1 derives a verdict from the blockers file" ($vReal -ne $null)
Check "D3 at least one blocker is OPEN today, so this build is not shippable" `
    ($vReal -ne $null -and @($vReal.releaseBlockers).Count -ge 1 -and $vReal.shippable -eq $false)
# D4 used to pin WINDOWS-19 into the open set. That made a genuine FIX read as a
# test failure, which is backwards: this check exists to prove the id list is
# READ OUT of the file rather than invented, not to freeze which ids are open.
# Assert the property instead - every id the verdict reports must actually appear
# in the blockers document's open table.
$openTableIds = @()
foreach ($line in (Get-Content -Path $blockersDoc)) {
    if ($line -match '^\s*##\s') { $inOpen = ($line -match '(?i)open blockers') }
    if ($inOpen -and $line -match '^\|\s*([A-Z][A-Z0-9-]+)\s*\|') { $openTableIds += $Matches[1] }
}
$derived = $true
if ($vReal -eq $null) { $derived = $false }
else { foreach ($id in @($vReal.releaseBlockers)) { if ($openTableIds -notcontains $id) { $derived = $false } } }
Check ("D4 every reported open id is read out of the blockers file (reported: " + `
       ((@($vReal.releaseBlockers)) -join ',') + ")") `
    ($derived -and @($vReal.releaseBlockers).Count -ge 1)
# WINDOWS-19/-22 closed on 2026-09-12 (tools/test-runs/windows19_relink_fix.json).
# Pin the direction that matters now: a CLOSED row must not be reported open.
Check "D4b WINDOWS-19 is NOT reported open (it was fixed and measured closed)" `
    ($vReal -ne $null -and (@($vReal.releaseBlockers) -notcontains "WINDOWS-19"))

# Synthetic blockers files: the verdict must flip BOTH ways.
$synOpen   = Join-Path $fixRoot "BLOCKERS_open.md"
$synClosed = Join-Path $fixRoot "BLOCKERS_closed.md"
WriteText $synOpen @'
# Release Blockers

## Open blockers

| ID | Ledger | Player-facing consequence | Evidence | Status |
|----|--------|---------------------------|----------|--------|
| SYNTH-1 | n/a | a synthetic open row | none | open |

## Recently closed

| ID | Closed by | Evidence |
|----|-----------|----------|
| SYNTH-0 | n/a | n/a |
'@
WriteText $synClosed @'
# Release Blockers

## Open blockers

| ID | Ledger | Player-facing consequence | Evidence | Status |
|----|--------|---------------------------|----------|--------|

## Recently closed

| ID | Closed by | Evidence |
|----|-----------|----------|
| SYNTH-1 | a synthetic fix | a synthetic run record |
'@

$vOpen = Get-Verdict $synOpen
Check "D5 with ONE open row the verdict is false and names that row" `
    ($vOpen -ne $null -and $vOpen.shippable -eq $false -and (@($vOpen.releaseBlockers) -contains "SYNTH-1"))
$vClosed = Get-Verdict $synClosed
Check "D6 with NO open row the verdict flips to TRUE (the gate can say both things)" `
    ($vClosed -ne $null -and $vClosed.shippable -eq $true -and @($vClosed.releaseBlockers).Count -eq 0)
Check "D7 a CLOSED row is not miscounted as open (the closed table is not parsed)" `
    ($vClosed -ne $null -and -not (@($vClosed.releaseBlockers) -contains "SYNTH-1"))

$vMissing = Get-Verdict (Join-Path $fixRoot "no_such_blockers_file.md")
Check "D8 a MISSING blockers file is not an empty one: shippable stays false" `
    ($vMissing -ne $null -and $vMissing.shippable -eq $false -and $vMissing.blockersFound -eq $false)

$kitText = [System.IO.File]::ReadAllText($kitScript)
Check "D9 shippable is DERIVED, never a literal in make_mod_kit.ps1" `
    ($kitText.Contains('$shippable = [bool]($gate.found -and $releaseBlockers.Count -eq 0)') -and
     -not ($kitText -match 'shippable\s*=\s*\$(true|false)'))

# The kit's own PROVENANCE.json, if a kit has been packaged.
$prov = Join-Path $repoRoot "dist\mod-kit\PROVENANCE.json"
if (Test-Path $prov) {
    $pj = Get-Content -Raw $prov | ConvertFrom-Json
    Check "D10 dist\mod-kit\PROVENANCE.json carries shippable and releaseBlockers" `
        (($pj.PSObject.Properties.Name -contains "shippable") -and ($pj.PSObject.Properties.Name -contains "releaseBlockers"))
    Check "D11 the packaged verdict agrees with the blockers file" `
        ($vReal -ne $null -and $pj.shippable -eq $vReal.shippable -and
         (@($pj.releaseBlockers) -join ",") -eq (@($vReal.releaseBlockers) -join ","))
    # D12: the reconnect paragraph must track the blocker, in BOTH directions.
    # While WINDOWS-19/-22 is open a player must be warned; once it is closed the
    # warning must be gone, because telling someone to "wait ten seconds before
    # reconnecting" for a defect their build does not have is its own defect.
    $kitReadme  = Join-Path $repoRoot "dist\mod-kit\README.txt"
    $readmeRaw  = if (Test-Path $kitReadme) { Get-Content -Raw $kitReadme } else { "" }
    $warns      = ($readmeRaw -match "(?s)KNOWN ISSUES.*slots full")
    $leakOpen   = (@($vReal.releaseBlockers) -contains "WINDOWS-19") -or `
                  (@($vReal.releaseBlockers) -contains "WINDOWS-22")
    Check ("D12 README.txt's reconnect warning tracks the blocker (open=" + $leakOpen + `
           " warns=" + $warns + ")") `
        ((Test-Path $kitReadme) -and ($warns -eq $leakOpen))
} else {
    Check "D10 a kit has been packaged so its PROVENANCE.json can be checked" $false
}

Write-Host ""
foreach ($m in $script:MutantFiles) {
    try { Remove-Item -LiteralPath $m -Force -ErrorAction SilentlyContinue } catch { }
}
$leftover = @($script:MutantFiles | Where-Object { Test-Path -LiteralPath $_ })
Check "C5 every mutant copy was removed from scripts\ (the working tree is clean)" ($leftover.Count -eq 0)
try { Remove-Item -LiteralPath $fixRoot -Recurse -Force -ErrorAction SilentlyContinue } catch { }

Write-Host ("InstallerRelease: {0}/{1} checks passed - {2}" -f $script:Pass, ($script:Pass + $script:Fail),
    $(if ($script:Fail -eq 0) { "PASS" } else { "FAIL" }))
exit $script:Fail
