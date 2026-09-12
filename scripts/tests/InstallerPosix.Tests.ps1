<#
.SYNOPSIS
  Headless guard for the POSIX (Proton/Wine) player installer, Phase 16 Plan 02
  (INST-02/03/04). Drives the REAL scripts\install_coop.sh through bash against
  a synthetic Proton-shaped Kenshi tree.

.DESCRIPTION
  This is the half of criterion 2 that can be proved without a Steam Deck: the
  script's LOGIC. The Deck run recorded in the plan's SUMMARY proves the
  ENVIRONMENT - that Kenshi actually starts under Proton with the plugin
  loaded. Neither substitutes for the other, and this file does not claim to
  cover the second.

  GROUP A - the real round trip, executed. A synthetic Proton-shaped tree is
  built from nothing and the REAL install_coop.sh is invoked through bash, with
  paths converted by cygpath. Nothing here reimplements the installer's
  behaviour in PowerShell: a test that models the code instead of running it
  proves nothing about what ships. The tree is measured with
  scripts\hash_tree.ps1 before, after the install (required to DIFFER) and after
  the uninstall (required to be BYTE-IDENTICAL to the first, file set
  included). Plugins_x64.cfg and data/mods.cfg are compared separately as raw
  bytes so a line-ending change cannot hide inside a tree-level pass.

  GROUP B - cross-platform equality of the MEASUREMENT. install_coop.sh
  --hash-tree and hash_tree.ps1 must agree byte for byte on the same tree,
  including a path with a space and a path nested two levels deep, because
  separator handling and locale-ordered sorts are where two implementations of
  one measurement drift first. Without this equality the Deck's criterion-4
  proof cannot be compared with the Windows one at all.

  GROUP C - the contract pins between the two front ends. Both installers are
  run for real on equivalent trees and the two manifests they WRITE are
  compared key for key - the artifact, not a regex over the source. Plus: the
  three prerequisite identifiers are the same three in both sources; both
  name backups after the hash of their content; install_coop.sh INVOKES
  neither the Steam client binary nor a Steam URL handler anywhere (both route
  to Remote Play and open the game on the wrong machine); and the file has no
  carriage returns.

  GROUP D - the Proton-specific refusals (INST-03).

  GROUP E - mutation sensitivity. Each mutation must turn a Group A or D
  verdict red, one at a time, while the unmutated fixture passes.

  If bash is unavailable this file FAILS LOUDLY rather than skipping: a silent
  skip would let criterion 2's only headless evidence evaporate unnoticed.

  Exit code = number of failed checks (0 = PASS), matching Installer.Tests.ps1
  so verify.ps1's INSTALLER GUARD can sum it.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\tests\InstallerPosix.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$INV         = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                        # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                      # repo root

$shInstaller = Join-Path $scriptsRoot "install_coop.sh"
$psInstaller = Join-Path $scriptsRoot "install_coop.ps1"
$hashTree    = Join-Path $scriptsRoot "hash_tree.ps1"

# ---- tiny assert harness (verbatim shape from Installer.Tests.ps1) ----------
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

Write-Host "== InstallerPosix: the Proton/Wine front end, run for real under bash =="

Check "scripts\install_coop.sh present"  (Test-Path $shInstaller)
Check "scripts\install_coop.ps1 present" (Test-Path $psInstaller)
Check "scripts\hash_tree.ps1 present"    (Test-Path $hashTree)

# ---- bash must exist. An absent bash is a FAILURE, never a skip. ------------
$bashExe = $null
try { $bashExe = (Get-Command bash -ErrorAction SilentlyContinue) } catch { $bashExe = $null }
Check "bash is available (an absent bash FAILS this suite, it does not skip it)" ($null -ne $bashExe)

if ((-not (Test-Path $shInstaller)) -or (-not (Test-Path $psInstaller)) -or
    (-not (Test-Path $hashTree)) -or ($null -eq $bashExe)) {
    Write-Host ""
    Write-Host ("InstallerPosix: {0}/{1} checks passed - FAIL" -f $script:Pass, ($script:Pass + $script:Fail))
    exit ([Math]::Max($script:Fail, 1))
}

$fixRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_posix_fix_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))

# ------------------------------------------------------------------ helpers
$UTF8NB = New-Object System.Text.UTF8Encoding($false)

function WriteText([string]$path, [string]$text) {
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($path, $text, $UTF8NB)
}

function WriteBytesText([string]$path, [string]$text) {
    # Exact bytes, no encoding surprises: the cfg files are compared byte-wise.
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllBytes($path, [System.Text.Encoding]::ASCII.GetBytes($text))
}

function Sha256File([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($path))
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function ToU([string]$win) {
    # cygpath through bash, with the path handed over in the environment so no
    # quoting level can mangle it.
    $env:KC_TESTPATH = $win
    $u = (& bash -c 'cygpath -u "$KC_TESTPATH"' 2>&1 | Select-Object -First 1)
    $env:KC_TESTPATH = $null
    return ([string]$u).Trim()
}

function Invoke-Sh {
    param([string[]]$ShArgs)
    $all = @((ToU $shInstaller)) + $ShArgs
    $text = (& bash @all 2>&1 | Out-String)
    return @{ text = $text; exit = $LASTEXITCODE }
}

function Invoke-Ps {
    param([string[]]$PsArgs)
    $all = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $psInstaller) + $PsArgs
    $text = (& powershell @all 2>&1 | Out-String)
    return @{ text = $text; exit = $LASTEXITCODE }
}

function HashTreePs([string]$root, [string]$outFile) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $hashTree -Root $root -OutFile $outFile | Out-Null
    return [System.IO.File]::ReadAllBytes($outFile)
}

function BytesEqual($a, $b) {
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}

# A Proton-shaped Kenshi install, built from nothing. The three runtime DLLs sit
# BESIDE THE EXE, which is where they must be under Proton
# (docs\CROSS_MACHINE_RIG.md section 1).
$CrtNames = @("mfc100u.dll", "msvcp100.dll", "msvcr100.dll")
$PluginsCfgText = "PluginFolder=.\`r`nPlugin=RenderSystem_Direct3D11_x64`r`nPlugin=Plugin_ParticleUniverse_x64`r`nPlugin=Plugin_Terrain_x64`r`n"

function New-SyntheticKenshi([string]$tag) {
    $dir     = Join-Path $fixRoot ("kenshi_" + $tag)
    $payload = Join-Path $fixRoot ("payload_" + $tag)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $dir "data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $dir "dependencies") | Out-Null
    New-Item -ItemType Directory -Force -Path $payload | Out-Null

    WriteText (Join-Path $dir "kenshi_x64.exe")   ("MZ-stub-exe-" + $tag)
    WriteBytesText (Join-Path $dir "Plugins_x64.cfg") $PluginsCfgText
    WriteBytesText (Join-Path $dir "data\mods.cfg")   "SomeOther.mod`r`n"
    WriteText (Join-Path $dir "currentVersion.txt") "1.0.65"
    WriteText (Join-Path $dir "RE_Kenshi.dll")      ("RE-KENSHI-STUB-" + $tag)
    WriteText (Join-Path $dir "dependencies\vcredist_x64.exe") "VCREDIST-STUB"
    foreach ($n in $CrtNames) { WriteText (Join-Path $dir $n) ("CRT-STUB-" + $n) }

    # A stub payload: the build stamp is deliberately in the bytes so VERSION.txt
    # has something honest to anchor on, and nothing here is a real binary.
    WriteText (Join-Path $payload "KenshiCoop.dll") ("PAYLOAD-" + $tag + "-KenshiCoop: build Sep 12 2026 11:22:33-end")
    WriteText (Join-Path $payload "RE_Kenshi.json") '{"Plugins":["KenshiCoop.dll"]}'
    WriteText (Join-Path $payload "KenshiCoop.mod") "KenshiCoop mod list entry stub"

    return @{
        tag      = $tag
        dir      = $dir
        dirU     = (ToU $dir)
        payload  = $payload
        payloadU = (ToU $payload)
        modDir   = (Join-Path $dir "mods\KenshiCoop")
        backup   = (Join-Path $dir "mods\KenshiCoop.backup")
        manifest = (Join-Path $dir "mods\KenshiCoop.backup\INSTALL-MANIFEST.json")
        cfg      = (Join-Path $dir "Plugins_x64.cfg")
        modsCfg  = (Join-Path $dir "data\mods.cfg")
    }
}

$rc = 0
try {

# ===================================================================== A =====
Write-Host ""
Write-Host "-- A. the real round trip, executed through bash --"

$a = New-SyntheticKenshi "A"
$preFile  = Join-Path $fixRoot "A.pre.txt"
$midFile  = Join-Path $fixRoot "A.mid.txt"
$postFile = Join-Path $fixRoot "A.post.txt"

$aPre     = HashTreePs $a.dir $preFile
$cfgBefore = [System.IO.File]::ReadAllBytes($a.cfg)
$modsBefore = [System.IO.File]::ReadAllBytes($a.modsCfg)

$aDry = Invoke-Sh @("--kenshi-dir", $a.dirU, "--source", $a.payloadU, "--dry-run")
Check "A1  --dry-run exits 0"                              ($aDry.exit -eq 0)
Check "A2  --dry-run prints the whole plan"                ($aDry.text -match 'create\s+mods/KenshiCoop/KenshiCoop\.dll')
Check "A3  --dry-run names each content-addressed backup"  ($aDry.text -match 'back up\s+Plugins_x64\.cfg\s+->\s+mods/KenshiCoop\.backup/Plugins_x64\.cfg\.[0-9a-f]{8}\.bak')
Check "A4  --dry-run says plainly that it did nothing"     ($aDry.text -match 'DRY-RUN: nothing above was done')
$aDryHash = HashTreePs $a.dir (Join-Path $fixRoot "A.dry.txt")
Check "A5  --dry-run wrote NOTHING (tree hash unchanged)"  (BytesEqual $aPre $aDryHash)

$aIns = Invoke-Sh @("--kenshi-dir", $a.dirU, "--source", $a.payloadU)
Check "A6  install exits 0"                                ($aIns.exit -eq 0)
Check "A7  install reports completion"                     ($aIns.text -match 'INSTALL: COMPLETE')
Check "A8  the mod directory exists"                       (Test-Path (Join-Path $a.modDir "KenshiCoop.dll"))
Check "A9  VERSION.txt was written beside it"              (Test-Path (Join-Path $a.modDir "VERSION.txt"))
Check "A10 the manifest was written"                       (Test-Path $a.manifest)

$aMid = HashTreePs $a.dir $midFile
Check "A11 the install CHANGED the tree"                   (-not (BytesEqual $aPre $aMid))

$aUn = Invoke-Sh @("--kenshi-dir", $a.dirU, "--uninstall")
Check "A12 uninstall exits 0"                              ($aUn.exit -eq 0)
Check "A13 uninstall reports a verified sweep"             ($aUn.text -match 'UNINSTALL: COMPLETE')
Check "A14 uninstall says each restore was hash-verified"  ($aUn.text -match 'verified sha256 [0-9a-f]{64} == the content recorded before the install')

$aPost = HashTreePs $a.dir $postFile
Check "A15 the tree is BYTE-IDENTICAL to its pre-install state" (BytesEqual $aPre $aPost)
Check "A16 Plugins_x64.cfg is byte-identical, line endings included" (BytesEqual $cfgBefore ([System.IO.File]::ReadAllBytes($a.cfg)))
Check "A17 data/mods.cfg is byte-identical, line endings included"   (BytesEqual $modsBefore ([System.IO.File]::ReadAllBytes($a.modsCfg)))
Check "A18 no empty mods\KenshiCoop is left behind"        (-not (Test-Path $a.modDir))
Check "A19 no empty mods\KenshiCoop.backup is left behind" (-not (Test-Path $a.backup))

# --info reads the build back without writing anything.
$a2 = New-SyntheticKenshi "A2"
Invoke-Sh @("--kenshi-dir", $a2.dirU, "--source", $a2.payloadU) | Out-Null
$infoPre  = HashTreePs $a2.dir (Join-Path $fixRoot "A2.info.pre.txt")
$aInfo    = Invoke-Sh @("--kenshi-dir", $a2.dirU, "--info")
$infoPost = HashTreePs $a2.dir (Join-Path $fixRoot "A2.info.post.txt")
Check "A20 --info exits 0"                                 ($aInfo.exit -eq 0)
Check "A21 --info prints the protocol line from VERSION.txt" ($aInfo.text -match 'protocol:')
Check "A22 --info states that it launched nothing"         ($aInfo.text -match 'the game was not launched')
Check "A23 --info wrote nothing (whole-tree hash unchanged)" (BytesEqual $infoPre $infoPost)

# ===================================================================== B =====
Write-Host ""
Write-Host "-- B. one measurement, two implementations, byte-identical --"

$bRoot = Join-Path $fixRoot "measure"
New-Item -ItemType Directory -Force -Path (Join-Path $bRoot "a dir with spaces\deeper one") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $bRoot "nested\two\levels") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $bRoot "an empty dir") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $bRoot "Zebra") | Out-Null
WriteText (Join-Path $bRoot "a dir with spaces\deeper one\file with spaces.txt") "alpha"
WriteText (Join-Path $bRoot "nested\two\levels\deep.bin") "beta"
WriteText (Join-Path $bRoot "Zebra\upper.txt") "gamma"
WriteText (Join-Path $bRoot "apple.txt") "delta"
WriteText (Join-Path $bRoot "empty-file.txt") ""

$bPsFile = Join-Path $fixRoot "measure.ps.txt"
$bPs = HashTreePs $bRoot $bPsFile
$bShText = (& bash (ToU $shInstaller) --hash-tree (ToU $bRoot) | Out-String)
# Out-String normalises the line endings PowerShell saw; compare the BYTES the
# script actually produced instead, by having bash write the file itself.
$bShFile = Join-Path $fixRoot "measure.sh.txt"
$env:KC_SH = (ToU $shInstaller); $env:KC_ROOT = (ToU $bRoot); $env:KC_OUT = (ToU $bShFile)
& bash -c 'bash "$KC_SH" --hash-tree "$KC_ROOT" > "$KC_OUT"' | Out-Null
$env:KC_SH = $null; $env:KC_ROOT = $null; $env:KC_OUT = $null
$bSh = [System.IO.File]::ReadAllBytes($bShFile)

Check "B1  install_coop.sh --hash-tree produced output"    ($bSh.Length -gt 0)
Check "B2  the two measurements are BYTE-IDENTICAL"        (BytesEqual $bPs $bSh)
$bShStr = [System.Text.Encoding]::UTF8.GetString($bSh)
Check "B3  a path with a space is measured"                ($bShStr -match 'a dir with spaces/deeper one/file with spaces\.txt')
Check "B4  a path nested two levels deep is measured"      ($bShStr -match 'nested/two/levels/deep\.bin')
Check "B5  an EMPTY directory is listed, not just files"   ($bShStr -match '(?m)^emptydir an empty dir/$')
Check "B6  paths use forward slashes with no leading ./"   (-not ($bShStr -match '(?m)^\S+ \./'))
Check "B7  the sort is BYTE order, not a locale collation (Zebra before apple)" `
    ($bShStr.IndexOf("Zebra/upper.txt") -lt $bShStr.IndexOf("apple.txt"))
Check "B8  the output has no BOM"                          (-not ($bSh.Length -ge 3 -and $bSh[0] -eq 0xEF -and $bSh[1] -eq 0xBB -and $bSh[2] -eq 0xBF))
Check "B9  every line is LF-terminated, never CRLF"        (-not ($bShStr -match "`r"))

# ===================================================================== C =====
Write-Host ""
Write-Host "-- C. the contract pins between the two front ends --"

# Compare the manifests the two installers actually WRITE, not a regex over the
# source: the artifact is the contract.
$cSh = New-SyntheticKenshi "Csh"
$cPs = New-SyntheticKenshi "Cps"
Invoke-Sh @("--kenshi-dir", $cSh.dirU, "--source", $cSh.payloadU) | Out-Null
Invoke-Ps @("-KenshiDir", $cPs.dir, "-Source", $cPs.payload, "-CrtSearchPath", $cPs.dir) | Out-Null

Check "C1  the POSIX front end wrote a manifest"           (Test-Path $cSh.manifest)
Check "C2  the Windows front end wrote a manifest"         (Test-Path $cPs.manifest)

if ((Test-Path $cSh.manifest) -and (Test-Path $cPs.manifest)) {
    $mSh = (Get-Content -Raw $cSh.manifest) | ConvertFrom-Json
    $mPs = (Get-Content -Raw $cPs.manifest) | ConvertFrom-Json
    $kSh = @($mSh.PSObject.Properties.Name | Sort-Object)
    $kPs = @($mPs.PSObject.Properties.Name | Sort-Object)
    Check "C3  the top-level manifest key set is IDENTICAL" (($kSh -join ",") -eq ($kPs -join ","))
    Check "C4  supersedes is one of those keys (16-01 deviation 2)" ($kSh -contains "supersedes")

    $fSh = @(@($mSh.files)[0].PSObject.Properties.Name | Sort-Object)
    $fPs = @(@($mPs.files)[0].PSObject.Properties.Name | Sort-Object)
    Check "C5  the per-file key set is IDENTICAL"          (($fSh -join ",") -eq ($fPs -join ","))
    Check "C6  the per-file keys are the seven the contract names" `
        (($fSh -join ",") -eq "action,backupPath,backupSha256,existedBefore,path,sha256After,sha256Before")

    $pSh = @($mSh.prerequisites.PSObject.Properties.Name | Sort-Object)
    $pPs = @($mPs.prerequisites.PSObject.Properties.Name | Sort-Object)
    Check "C7  the prerequisites key set is IDENTICAL"     (($pSh -join ",") -eq ($pPs -join ","))
    Check "C8  the crt sub-object key set is IDENTICAL" `
        ((@($mSh.prerequisites.crt.PSObject.Properties.Name | Sort-Object) -join ",") -eq
         (@($mPs.prerequisites.crt.PSObject.Properties.Name | Sort-Object) -join ","))
    Check "C9  schemaVersion agrees"                       ([int]$mSh.schemaVersion -eq [int]$mPs.schemaVersion)
    Check "C10 platform says linux-proton on the POSIX side" ("$($mSh.platform)" -eq "linux-proton")
    Check "C11 platform says windows on the Windows side"    ("$($mPs.platform)" -eq "windows")
    Check "C12 manifest paths are relative with forward slashes" `
        (@($mSh.files | Where-Object { $_.path -match '\\' -or $_.path -match '^[A-Za-z]:' -or $_.path -match '^/' }).Count -eq 0)
    Check "C13 the POSIX manifest records the SAME file set as the Windows one" `
        ((@($mSh.files | ForEach-Object { $_.path } | Sort-Object) -join ",") -eq
         (@($mPs.files | ForEach-Object { $_.path } | Sort-Object) -join ","))
}

$shSrc = [System.IO.File]::ReadAllText($shInstaller)
$psSrc = [System.IO.File]::ReadAllText($psInstaller)
$shBytes = [System.IO.File]::ReadAllBytes($shInstaller)

Check "C14 install_coop.sh names all three CRT DLLs"       ((($CrtNames | Where-Object { $shSrc -match [regex]::Escape($_) }).Count) -eq 3)
Check "C15 install_coop.ps1 names the same three"          ((($CrtNames | Where-Object { $psSrc -match [regex]::Escape($_) }).Count) -eq 3)
Check "C16 both name backups after the hash of their content" `
    (($shSrc -match '\$\{sha:0:8\}') -and ($psSrc -match '\$sha\.Substring\(0, 8\)'))
Check "C17 install_coop.sh has NO carriage returns" `
    ((0..($shBytes.Length - 1) | Where-Object { $shBytes[$_] -eq 13 } | Measure-Object).Count -eq 0)
Check "C18 install_coop.sh is ASCII only" `
    ((0..($shBytes.Length - 1) | Where-Object { $shBytes[$_] -gt 127 } | Measure-Object).Count -eq 0)

# The Remote Play trap: scan for an INVOCATION of the Steam client or a Steam
# URL handler, not for the words. The script is required to EXPLAIN why not to
# use them, so a naive "does the file contain steam://" scan would be
# unfalsifiable - it would fail on the very warning that keeps players safe.
$shLines = [System.IO.File]::ReadAllLines($shInstaller)
$steamInvocations = @()
for ($i = 0; $i -lt $shLines.Length; $i++) {
    $l = $shLines[$i]
    $code = ($l -replace '^\s*#.*$', '')                     # drop whole-line comments
    if ($code -match '^\s*$') { continue }
    if ($code -match '(^|[;&|`(]|\$\()\s*(steam|steamcmd)\b') { $steamInvocations += ("line " + ($i + 1)) }
    if ($code -match 'xdg-open\s+["'']?steam://')             { $steamInvocations += ("line " + ($i + 1)) }
    # An invocation would put steam:// where a command expects an argument, not
    # inside a printf format string that explains the hazard.
    if ($code -match '(^|[;&|`(]|\$\()\s*[^\s]*\s+steam://')  { $steamInvocations += ("line " + ($i + 1)) }
}
Check "C19 install_coop.sh INVOKES neither the Steam client nor a steam:// handler" `
    ($steamInvocations.Count -eq 0)
Check "C20 it does explain the Remote Play hazard in prose"  ($shSrc -match 'Remote Play')
Check "C21 it names no WINEDLLOVERRIDES and no DLL-proxy workaround" `
    (($shSrc -notmatch 'WINEDLLOVERRIDES=') -and ($shSrc -notmatch 'winmm\.dll'))
Check "C22 it points at a direct Proton invocation of kenshi_x64.exe" `
    ($shSrc -match 'waitforexitandrun')
Check "C23 install_coop.sh launches no game itself"          ($shSrc -notmatch '(?m)^\s*exec\s')

# ===================================================================== D =====
Write-Host ""
Write-Host "-- D. the Proton-specific refusals --"

$d1 = New-SyntheticKenshi "D1"
foreach ($n in $CrtNames) { Remove-Item -LiteralPath (Join-Path $d1.dir $n) -Force }
$r1 = Invoke-Sh @("--kenshi-dir", $d1.dirU, "--source", $d1.payloadU)
Check "D1a a missing runtime REFUSES"                      ($r1.exit -ne 0 -and $r1.text -match 'REFUSED')
Check "D1b the refusal names mfc100u.dll"                  ($r1.text -match 'mfc100u\.dll')
Check "D1c the refusal names msvcp100.dll"                 ($r1.text -match 'msvcp100\.dll')
Check "D1d the refusal names msvcr100.dll"                 ($r1.text -match 'msvcr100\.dll')
Check "D1e it says the game does not start AT ALL"         ($r1.text -match 'the game does not start at all')
Check "D1f it names 0xc0000135 and the absent log"         ($r1.text -match '0xc0000135' -and $r1.text -match 'before any log file is created')
Check "D1g it names the player's own vcredist_x64.exe"     ($r1.text -match 'vcredist_x64\.exe')
Check "D1h it says that installer's exit code is not evidence under Wine" `
    ($r1.text -match 'exit code is NOT evidence under Wine')
Check "D1i it says the three names are re-checked afterwards" ($r1.text -match 're-checks the three names')
Check "D1j the refusal left no mod directory behind"       (-not (Test-Path $d1.modDir))
Check "D1k the refusal left no backup directory behind"    (-not (Test-Path $d1.backup))

$d2 = New-SyntheticKenshi "D2"
Remove-Item -LiteralPath (Join-Path $d2.dir "RE_Kenshi.dll") -Force
$r2 = Invoke-Sh @("--kenshi-dir", $d2.dirU, "--source", $d2.payloadU)
Check "D2a a missing loader REFUSES"                       ($r2.exit -ne 0 -and $r2.text -match 'REFUSED')
Check "D2b the refusal names RE_Kenshi"                    ($r2.text -match 'RE_Kenshi\.dll is not there')
Check "D2c it gives the Nexus page"                        ($r2.text -match 'nexusmods\.com/kenshi/mods/847')
# The REFUSAL line only: the prerequisite summary above it legitimately lists
# the runtime as [ok], so scanning the whole transcript would assert nothing.
$r2Refusal = (($r2.text -split "`n") | Where-Object { $_ -match '^REFUSED:' }) -join " "
Check "D2d the refusal itself names ONE cause and does not drag in the CRT" `
    (($r2Refusal -match 'RE_Kenshi') -and ($r2Refusal -notmatch 'mfc100u'))

# The Ogre plugin registration: a missing LINE is repaired, not refused - it
# fails and is fixed differently from RE_Kenshi simply being absent.
$d3 = New-SyntheticKenshi "D3"
$d3CfgBefore = [System.IO.File]::ReadAllBytes($d3.cfg)
$r3 = Invoke-Sh @("--kenshi-dir", $d3.dirU, "--source", $d3.payloadU)
Check "D3a a missing plugin line does NOT refuse, it is repaired" ($r3.exit -eq 0)
$d3Cfg = [System.IO.File]::ReadAllText($d3.cfg)
Check "D3b Plugins_x64.cfg now registers RE_Kenshi"        ($d3Cfg -match '(?m)^Plugin=RE_Kenshi\s*$')
Check "D3c the three pre-existing plugin lines survive unchanged" `
    (($d3Cfg -match 'Plugin=RenderSystem_Direct3D11_x64') -and
     ($d3Cfg -match 'Plugin=Plugin_ParticleUniverse_x64') -and
     ($d3Cfg -match 'Plugin=Plugin_Terrain_x64'))
Check "D3d the PluginFolder line survives"                 ($d3Cfg -match 'PluginFolder=')
$d3Bak = @(Get-ChildItem -Path $d3.backup -Filter "Plugins_x64.cfg.*.bak" -File -ErrorAction SilentlyContinue)
Check "D3e the cfg was backed up BEFORE it was edited"     ($d3Bak.Count -eq 1)
Check "D3f that backup holds the cfg's pre-edit bytes"     ($d3Bak.Count -eq 1 -and (BytesEqual $d3CfgBefore ([System.IO.File]::ReadAllBytes($d3Bak[0].FullName))))
Check "D3g the backup's NAME encodes the hash of what it holds" `
    ($d3Bak.Count -eq 1 -and $d3Bak[0].Name -eq ("Plugins_x64.cfg." + (Sha256File $d3Bak[0].FullName).Substring(0, 8) + ".bak"))
Check "D3h the manifest records the repair"                ((Get-Content -Raw $d3.manifest) -match '"repaired":\s*true')
$r3u = Invoke-Sh @("--kenshi-dir", $d3.dirU, "--uninstall")
Check "D3i uninstall puts the original cfg back byte for byte" `
    (BytesEqual $d3CfgBefore ([System.IO.File]::ReadAllBytes($d3.cfg)))

# Unsupported / unparseable / missing version: three distinct refusals.
$d4 = New-SyntheticKenshi "D4"
WriteText (Join-Path $d4.dir "currentVersion.txt") "1.0.51"
$r4 = Invoke-Sh @("--kenshi-dir", $d4.dirU, "--source", $d4.payloadU)
Check "D4a an unsupported version REFUSES and names both versions" `
    ($r4.exit -ne 0 -and $r4.text -match '1\.0\.51' -and $r4.text -match '1\.0\.65')
WriteText (Join-Path $d4.dir "currentVersion.txt") "not a version"
$r5 = Invoke-Sh @("--kenshi-dir", $d4.dirU, "--source", $d4.payloadU)
Check "D4b an unparseable version is a DIFFERENT refusal" `
    ($r5.exit -ne 0 -and $r5.text -match 'does not contain a version number')
Remove-Item -LiteralPath (Join-Path $d4.dir "currentVersion.txt") -Force
$r6 = Invoke-Sh @("--kenshi-dir", $d4.dirU, "--source", $d4.payloadU)
Check "D4c a MISSING currentVersion.txt is a third, distinct refusal" `
    ($r6.exit -ne 0 -and $r6.text -match 'This is not an unsupported version')

# A relative --out-dir is refused rather than silently swallowed.
$d5 = New-SyntheticKenshi "D5"
$r7 = Invoke-Sh @("--kenshi-dir", $d5.dirU, "--source", $d5.payloadU, "--out-dir", "relative/path")
Check "D5a a relative --out-dir REFUSES"                   ($r7.exit -ne 0 -and $r7.text -match 'must be an ABSOLUTE path')
$r8 = Invoke-Sh @("--kenshi-dir", (ToU $fixRoot), "--source", $d5.payloadU)
Check "D5b a folder with no kenshi_x64.exe REFUSES"        ($r8.exit -ne 0 -and $r8.text -match 'kenshi_x64\.exe is not in it')
$r9 = Invoke-Sh @("--kenshi-dir", $d5.dirU, "--uninstall")
Check "D5c --uninstall with no manifest REFUSES"           ($r9.exit -ne 0 -and $r9.text -match 'no install manifest')

# A second install must not destroy the first install's superseded backup, and
# an uninstall afterwards must restore the PLAYER'S original.
$d6 = New-SyntheticKenshi "D6"
WriteText (Join-Path $d6.dir "mods\KenshiCoop\KenshiCoop.dll") "THE PLAYERS OWN PRE-EXISTING DLL"
$d6Orig = Sha256File (Join-Path $d6.dir "mods\KenshiCoop\KenshiCoop.dll")
Invoke-Sh @("--kenshi-dir", $d6.dirU, "--source", $d6.payloadU) | Out-Null
WriteText (Join-Path $d6.payload "KenshiCoop.dll") "PAYLOAD-D6-SECOND-BUILD-KenshiCoop: build Sep 12 2026 11:22:33-end"
$d6b = Invoke-Sh @("--kenshi-dir", $d6.dirU, "--source", $d6.payloadU, "--force")
Check "D6a a second install without --force REFUSES"       ((Invoke-Sh @("--kenshi-dir", $d6.dirU, "--source", $d6.payloadU)).text -match 'already installed here')
Check "D6b a second install with --force succeeds"         ($d6b.exit -eq 0)
Check "D6c the manifest records what it superseded"        ((Get-Content -Raw $d6.manifest) -match '"supersedes":\s*"20')
$d6Baks = @(Get-ChildItem -Path $d6.backup -Filter "KenshiCoop.dll.*.bak" -File -ErrorAction SilentlyContinue)
Check "D6d two installs left TWO differently-named DLL backups" ($d6Baks.Count -eq 2)
Check "D6e every backup still hashes to the content its own name encodes" `
    (@($d6Baks | Where-Object { $_.Name -ne ($_.Name -replace '\.[0-9a-f]{8}\.bak$', '') + "." + (Sha256File $_.FullName).Substring(0, 8) + ".bak" }).Count -eq 0)
Invoke-Sh @("--kenshi-dir", $d6.dirU, "--uninstall") | Out-Null
Check "D6f uninstall after two installs restores the PLAYER'S original DLL" `
    ((Sha256File (Join-Path $d6.dir "mods\KenshiCoop\KenshiCoop.dll")) -eq $d6Orig)

# ===================================================================== E =====
Write-Host ""
Write-Host "-- E. mutation sensitivity: each proof must be able to fail --"

# E1 - a manifest entry dropped, so the uninstall skips a file.
$e1 = New-SyntheticKenshi "E1"
$e1Pre = HashTreePs $e1.dir (Join-Path $fixRoot "E1.pre.txt")
Invoke-Sh @("--kenshi-dir", $e1.dirU, "--source", $e1.payloadU) | Out-Null
$m1 = (Get-Content -Raw $e1.manifest) | ConvertFrom-Json
$m1.files = @($m1.files | Where-Object { $_.path -ne "mods/KenshiCoop/KenshiCoop.mod" })
WriteText $e1.manifest ($m1 | ConvertTo-Json -Depth 12)
Invoke-Sh @("--kenshi-dir", $e1.dirU, "--uninstall") | Out-Null
$e1Post = HashTreePs $e1.dir (Join-Path $fixRoot "E1.post.txt")
Check "E1a dropping a manifest entry leaves the file behind" (Test-Path (Join-Path $e1.modDir "KenshiCoop.mod"))
Check "E1b and the round-trip proof FAILS, as it must"       (-not (BytesEqual $e1Pre $e1Post))

# E2 - a recorded sha256Before altered: the restore verification must catch it.
$e2 = New-SyntheticKenshi "E2"
Invoke-Sh @("--kenshi-dir", $e2.dirU, "--source", $e2.payloadU) | Out-Null
$m2 = (Get-Content -Raw $e2.manifest) | ConvertFrom-Json
foreach ($f in $m2.files) { if ($f.path -eq "Plugins_x64.cfg") { $f.sha256Before = ("0" * 64) } }
WriteText $e2.manifest ($m2 | ConvertTo-Json -Depth 12)
$r2e = Invoke-Sh @("--kenshi-dir", $e2.dirU, "--uninstall")
Check "E2a a wrong sha256Before REFUSES rather than restoring blind" `
    ($r2e.exit -ne 0 -and $r2e.text -match 'did not produce the content recorded before the install')

# E3 - a backup truncated: nothing may be restored from it.
$e3 = New-SyntheticKenshi "E3"
Invoke-Sh @("--kenshi-dir", $e3.dirU, "--source", $e3.payloadU) | Out-Null
$e3Bak = @(Get-ChildItem -Path $e3.backup -Filter "*.bak" -File)[0]
WriteText $e3Bak.FullName "TRUNCATED"
$r3e = Invoke-Sh @("--kenshi-dir", $e3.dirU, "--uninstall")
Check "E3a a truncated backup REFUSES"                      ($r3e.exit -ne 0 -and $r3e.text -match 'no longer matches what was recorded for it')
Check "E3b and nothing was restored from it"                ((Get-Content -Raw $e3.modsCfg) -match 'KenshiCoop\.mod')

# E4 - the repaired cfg restored with a changed line ending: the SEPARATE byte
#      comparison must see it, without leaning on the tree hash.
$e4 = New-SyntheticKenshi "E4"
$e4CfgBefore = [System.IO.File]::ReadAllBytes($e4.cfg)
Invoke-Sh @("--kenshi-dir", $e4.dirU, "--source", $e4.payloadU) | Out-Null
Invoke-Sh @("--kenshi-dir", $e4.dirU, "--uninstall") | Out-Null
Check "E4a the unmutated fixture passes the cfg comparison" (BytesEqual $e4CfgBefore ([System.IO.File]::ReadAllBytes($e4.cfg)))
$e4Text = [System.IO.File]::ReadAllText($e4.cfg)
WriteBytesText $e4.cfg ($e4Text -replace "`r`n", "`n")
Check "E4b flipping the restored cfg to LF makes that comparison FAIL" `
    (-not (BytesEqual $e4CfgBefore ([System.IO.File]::ReadAllBytes($e4.cfg))))
Check "E4c the mutated file still holds the same TEXT (only line endings moved)" `
    ((([System.IO.File]::ReadAllText($e4.cfg)) -replace "`n", "") -eq ($e4Text -replace "`r`n", ""))

# E5 - the hash-tree equality itself must be able to fail.
WriteText (Join-Path $bRoot "apple.txt") "delta-MUTATED"
$env:KC_SH = (ToU $shInstaller); $env:KC_ROOT = (ToU $bRoot); $env:KC_OUT = (ToU (Join-Path $fixRoot "measure.sh2.txt"))
& bash -c 'bash "$KC_SH" --hash-tree "$KC_ROOT" > "$KC_OUT"' | Out-Null
$env:KC_SH = $null; $env:KC_ROOT = $null; $env:KC_OUT = $null
$bSh2 = [System.IO.File]::ReadAllBytes((Join-Path $fixRoot "measure.sh2.txt"))
Check "E5a changing one byte in one file changes the measurement" (-not (BytesEqual $bSh $bSh2))
$bPs2 = HashTreePs $bRoot (Join-Path $fixRoot "measure.ps2.txt")
Check "E5b the two implementations still agree on the mutated tree" (BytesEqual $bPs2 $bSh2)

} finally {
    if (Test-Path $fixRoot) { Remove-Item -Recurse -Force -Path $fixRoot -ErrorAction SilentlyContinue }
}

# This suite proves the POSIX front end's LOGIC. It does NOT prove that Kenshi
# starts under Proton - that needs the live Steam Deck run recorded in
# tools/test-runs/phase16_deck_install.json and quoted in the plan's SUMMARY.
# Neither may be reported as standing in for the other.

Write-Host ""
Write-Host ("InstallerPosix: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
