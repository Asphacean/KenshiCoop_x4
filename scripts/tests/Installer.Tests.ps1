<#
.SYNOPSIS
  Headless guard for the player installer (Phase 16 Plan 01, INST-01/03/04/05).
  Launches no game, needs no KenshiCoop.dll, and drives the REAL
  scripts\install_coop.ps1 against a synthetic Kenshi tree, so the round-trip
  proof exercises shipped code rather than a copy of it.

.DESCRIPTION
  Four groups.

  GROUP A - the criterion-4 measurement (D-06). Hash the whole tree with
  scripts\hash_tree.ps1, install, hash again and require the two to DIFFER (a
  round trip whose before and after match because the installer did nothing is
  the failure mode to rule out first), uninstall, hash a third time and require
  BYTE EQUALITY with the first, file set included. Plugins_x64.cfg and
  data\mods.cfg are compared separately as raw bytes, so a line-ending change
  cannot hide inside a tree-level pass.

  GROUP B - the double-install backup rule (D-03). Two installs over a
  pre-existing DLL must leave TWO backups with DIFFERENT basenames, each still
  hashing to the content its own name encodes, and no backup file written
  twice. This is the check that catches the naive fixed-basename backup step
  this milestone has already produced once.

  GROUP C - the seven prerequisite behaviours (D-04). Each refusal is asserted
  to name the specific thing that is missing - the three DLL names, RE_Kenshi,
  or the two version strings - and to leave no mod directory and no backup
  directory behind. Merely being non-empty is not asserted anywhere.

  GROUP D - mutation sensitivity. A green run must mean the test LOOKED. One
  mutation at a time is introduced and the matching verdict is required to
  flip, while the unmutated fixture (Group A) passes all of them.

  Exit code = number of failed checks (0 = PASS), matching
  RelinkLever.Tests.ps1 / CensusRepro.Tests.ps1 so verify.ps1 can sum it.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\tests\Installer.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$INV         = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                        # scripts
$repoRoot    = Split-Path -Parent $scriptsRoot                      # repo root

$installer = Join-Path $scriptsRoot "install_coop.ps1"
$hashTree  = Join-Path $scriptsRoot "hash_tree.ps1"

# ---- tiny assert harness (verbatim shape from RelinkLever.Tests.ps1) ---------
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

Write-Host "== Installer: install/uninstall round trip, backups, prerequisites =="

Check "scripts\install_coop.ps1 present" (Test-Path $installer)
Check "scripts\hash_tree.ps1 present"    (Test-Path $hashTree)
if (-not (Test-Path $installer) -or -not (Test-Path $hashTree)) {
    Write-Host ""
    Write-Host ("Installer: {0}/{1} checks passed - FAIL" -f $script:Pass, ($script:Pass + $script:Fail))
    exit $script:Fail
}

$fixRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_install_fix_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))

# ------------------------------------------------------------------ helpers
$UTF8NB = New-Object System.Text.UTF8Encoding($false)

function WriteText([string]$path, [string]$text) {
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($path, $text, $UTF8NB)
}

function Sha256([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($path))
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function BytesEqual([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $a) -or -not (Test-Path -LiteralPath $b)) { return $false }
    $x = [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($a))
    $y = [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($b))
    if ($x.Length -ne $y.Length) { return $false }
    for ($i = 0; $i -lt $x.Length; $i++) { if ($x[$i] -ne $y[$i]) { return $false } }
    return $true
}

# A stub payload DLL that carries the plugin's real build-stamp literals, so the
# installer's VERSION.txt extraction path is genuinely exercised. The bytes
# around the format string mirror what the compiler emits.
function PayloadDllBytes([string]$tag) {
    $s = $tag + "`0" + "KenshiCoop: build %s %s" + "`0`0`0" + "Sep 12 2026" + "`0`0`0`0`0" + "12:55:27" + "`0"
    return [System.Text.Encoding]::ASCII.GetBytes($s)
}

function New-SyntheticKenshi([string]$name) {
    <#
      Everything the installer inspects, built from nothing: a stub exe, the
      four real Ogre plugin lines, a mods.cfg with unrelated entries, the real
      version string, a stub RE_Kenshi loader, Kenshi's own redist stub, a fake
      CRT directory the -CrtSearchPath seam can be pointed at, and a payload of
      stubs with known content. The test proves the installer's FILE HANDLING,
      not the plugin.
    #>
    $d = Join-Path $fixRoot $name
    $k = Join-Path $d "K"
    foreach ($sub in @("K\data", "K\dependencies", "crt", "nocrt", "payload")) {
        New-Item -ItemType Directory -Force -Path (Join-Path $d $sub) | Out-Null
    }
    WriteText (Join-Path $k "kenshi_x64.exe")      "stub-kenshi-exe-do-not-touch"
    WriteText (Join-Path $k "currentVersion.txt")  "Kenshi 1.0.65 - x64 (Newland)`r`n"
    WriteText (Join-Path $k "Plugins_x64.cfg") `
        ("# Defines plugins to load`r`n`r`n# Define plugin folder`r`nPluginFolder=.\`r`n`r`n" +
         "# Define plugins`r`nPlugin=RE_Kenshi`r`nPlugin=RenderSystem_Direct3D11_x64`r`n" +
         "Plugin=Plugin_ParticleUniverse_x64`r`nPlugin=Plugin_Terrain_x64`r`n")
    WriteText (Join-Path $k "data\mods.cfg")       "SomeOtherMod.mod`r`nAnotherMod.mod`r`n"
    WriteText (Join-Path $k "RE_Kenshi.dll")       "stub-re-kenshi-loader"
    WriteText (Join-Path $k "dependencies\vcredist_x64.exe") "stub-redist"
    foreach ($n in @("mfc100u.dll", "msvcp100.dll", "msvcr100.dll")) {
        WriteText (Join-Path $d ("crt\" + $n)) "stub-crt"
    }
    [System.IO.File]::WriteAllBytes((Join-Path $d "payload\KenshiCoop.dll"), (PayloadDllBytes "PAYLOAD-V1"))
    WriteText (Join-Path $d "payload\RE_Kenshi.json") '{"Plugins":["KenshiCoop.dll"]}'
    WriteText (Join-Path $d "payload\KenshiCoop.mod") "stub-mod-data-v1"
    return [pscustomobject]@{
        base    = $d
        k       = $k
        crt     = (Join-Path $d "crt")
        nocrt   = (Join-Path $d "nocrt")
        payload = (Join-Path $d "payload")
        out     = (Join-Path $d "out")
        backup  = (Join-Path $k "mods\KenshiCoop.backup")
        modDir  = (Join-Path $k "mods\KenshiCoop")
        man     = (Join-Path $k "mods\KenshiCoop.backup\INSTALL-MANIFEST.json")
        cfg     = (Join-Path $k "Plugins_x64.cfg")
        modsCfg = (Join-Path $k "data\mods.cfg")
    }
}

function Invoke-Script([string]$script, [string[]]$Arguments, [string]$WorkTag) {
    # Every argument is quoted: a user's TEMP can contain spaces, and an
    # unquoted path there would split into two arguments and bind wrongly.
    $outFile = Join-Path $fixRoot ($WorkTag + ".out.txt")
    $errFile = Join-Path $fixRoot ($WorkTag + ".err.txt")
    $parts = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $script) + $Arguments
    $argLine = ($parts | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $p = Start-Process -FilePath "powershell" -ArgumentList $argLine -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $txt = ""
    if (Test-Path $outFile) { $txt += (Get-Content -Raw -LiteralPath $outFile) }
    if (Test-Path $errFile) { $txt += (Get-Content -Raw -LiteralPath $errFile) }
    return @{ exit = $p.ExitCode; text = "$txt" }
}

$script:runSeq = 0
function Invoke-Installer($t, [string[]]$Extra, [string[]]$SearchPath) {
    $script:runSeq++
    $sp = if ($SearchPath) { ($SearchPath -join ";") } else { $t.crt }
    $a = @("-KenshiDir", $t.k, "-Source", $t.payload, "-OutDir", $t.out, "-CrtSearchPath", $sp) + $Extra
    return (Invoke-Script $installer $a ("run" + $script:runSeq.ToString($INV)))
}

function Get-TreeHash($t, [string]$tag) {
    $f = Join-Path $t.base ($tag + ".tree.txt")
    $r = Invoke-Script $hashTree @("-Root", $t.k, "-OutFile", $f) ("tree_" + $tag + "_" + $script:runSeq.ToString($INV))
    if ($r.exit -ne 0) { return "HASH_TREE_FAILED: $($r.text)" }
    return [System.IO.File]::ReadAllText($f)
}

function Invoke-RoundTrip {
    <#
      The reusable judge Groups A and D both rest on: measure, install, measure,
      optionally MUTATE, uninstall, measure. Returns named sub-verdicts so a
      mutation can be required to flip exactly one of them.
    #>
    param($t, [scriptblock]$Mutate = $null, [switch]$KeepCfgCopies)

    $cfgBefore  = Join-Path $t.base "Plugins_x64.cfg.before"
    $modsBefore = Join-Path $t.base "mods.cfg.before"
    Copy-Item -LiteralPath $t.cfg     -Destination $cfgBefore  -Force
    Copy-Item -LiteralPath $t.modsCfg -Destination $modsBefore -Force

    $h0 = Get-TreeHash $t "pre"
    $ins = Invoke-Installer $t @() $null
    $h1 = Get-TreeHash $t "post-install"
    if ($Mutate) { & $Mutate $t }
    $uni = Invoke-Installer $t @("-Uninstall") $null
    $h2 = Get-TreeHash $t "post-uninstall"

    return [pscustomobject]@{
        installExit     = $ins.exit
        installText     = $ins.text
        uninstallExit   = $uni.exit
        uninstallText   = $uni.text
        installChanged  = ($h0 -ne $h1)
        treeEqual       = ($h0 -eq $h2)
        pluginsCfgEqual = (BytesEqual $cfgBefore  $t.cfg)
        modsCfgEqual    = (BytesEqual $modsBefore $t.modsCfg)
        preHash         = $h0
        postHash        = $h2
        cfgBefore       = $cfgBefore
        modsBefore      = $modsBefore
    }
}

New-Item -ItemType Directory -Force -Path $fixRoot | Out-Null
try {

# =================================================================== Group A ==
Write-Host ""
Write-Host "-- Group A: the round trip, measured by whole-tree hash (D-06) --"

$a = New-SyntheticKenshi "A"
$ra = Invoke-RoundTrip $a

Check "A1 install succeeds (exit 0, got $($ra.installExit))" ($ra.installExit -eq 0)
Check "A2 the install CHANGED the tree (a no-op install would make A4 meaningless)" $ra.installChanged
Check "A3 uninstall succeeds (exit 0, got $($ra.uninstallExit))" ($ra.uninstallExit -eq 0)
Check "A4 the post-uninstall tree hash is byte-identical to the pre-install one" $ra.treeEqual
Check "A5 Plugins_x64.cfg is byte-identical to its pre-install content" $ra.pluginsCfgEqual
Check "A6 data\mods.cfg is byte-identical to its pre-install content" $ra.modsCfgEqual
Check "A7 no mods\KenshiCoop left after the uninstall"        (-not (Test-Path $a.modDir))
Check "A8 no mods\KenshiCoop.backup left after the uninstall" (-not (Test-Path $a.backup))
Check "A9 the uninstall record was written to the absolute -OutDir" (Test-Path (Join-Path $a.out "UNINSTALL-RECORD.json"))
Check "A10 the install manifest survives outside the install, as evidence" (Test-Path (Join-Path $a.out "INSTALL-MANIFEST.json"))
Check "A11 the game executable was never touched" `
    ((Sha256 (Join-Path $a.k "kenshi_x64.exe")) -eq (Sha256 (Join-Path $a.k "kenshi_x64.exe")) -and
     ([System.IO.File]::ReadAllText((Join-Path $a.k "kenshi_x64.exe")) -eq "stub-kenshi-exe-do-not-touch"))

# The tree measurement must itself be able to see a difference, or A4 is
# unfalsifiable: add one byte and require the hash to change.
$probe = Join-Path $a.k "zz-probe.txt"
WriteText $probe "x"
$hProbe = Get-TreeHash $a "probe"
Remove-Item -LiteralPath $probe -Force
Check "A12 hash_tree.ps1 sees a single added file (the measurement can fail)" ($hProbe -ne $ra.postHash)
# ...and an EMPTY directory too, which a files-only walk would miss.
$emptyDir = Join-Path $a.k "zz-empty"
New-Item -ItemType Directory -Force -Path $emptyDir | Out-Null
$hEmpty = Get-TreeHash $a "empty"
Remove-Item -LiteralPath $emptyDir -Force
Check "A13 hash_tree.ps1 sees an empty directory (the file set is compared too)" ($hEmpty -ne $ra.postHash)

# -Info and -WhatIf, the two read-only front ends.
$b = New-SyntheticKenshi "A_info"
$rw = Invoke-Installer $b @("-WhatIf") $null
Check "A14 -WhatIf exits 0 (got $($rw.exit))" ($rw.exit -eq 0)
Check "A15 -WhatIf names the content-addressed backup it WOULD take" ($rw.text -match '\.bak')
Check "A16 -WhatIf created no mods\KenshiCoop"        (-not (Test-Path $b.modDir))
Check "A17 -WhatIf created no mods\KenshiCoop.backup" (-not (Test-Path $b.backup))
$ri = Invoke-Installer $b @() $null
Check "A18 install after -WhatIf succeeds (exit 0, got $($ri.exit))" ($ri.exit -eq 0)
Check "A19 VERSION.txt was written" (Test-Path (Join-Path $b.modDir "VERSION.txt"))
$verTxt = if (Test-Path (Join-Path $b.modDir "VERSION.txt")) { Get-Content -Raw (Join-Path $b.modDir "VERSION.txt") } else { "" }
Check "A20 VERSION.txt carries the build stamp read out of the DLL" ($verTxt -match 'Sep 12 2026 12:55:27')
Check "A21 VERSION.txt says WHERE the build stamp came from" ($verTxt -match 'read out of KenshiCoop\.dll')
$dllSha = Sha256 (Join-Path $b.modDir "KenshiCoop.dll")
Check "A22 VERSION.txt carries the installed DLL's own sha256" ($verTxt -match [regex]::Escape($dllSha))
$hBeforeInfo = Get-TreeHash $b "pre-info"
$rinfo = Invoke-Installer $b @("-Info") $null
Check "A23 -Info exits 0 and reads the build back (got $($rinfo.exit))" ($rinfo.exit -eq 0 -and $rinfo.text -match 'Sep 12 2026 12:55:27')
# Measured, not assumed: the whole tree must be unchanged by -Info. Counting
# files in the backup directory would not notice a rewritten one.
$hAfterInfo = Get-TreeHash $b "post-info"
Check "A24 -Info changed nothing at all in the install tree" ($hBeforeInfo -eq $hAfterInfo)

# =================================================================== Group B ==
Write-Host ""
Write-Host "-- Group B: a second install must not destroy the first install's backup (D-03) --"

$c = New-SyntheticKenshi "B"
# A pre-existing DLL, so install #1 REPLACES rather than creates.
New-Item -ItemType Directory -Force -Path $c.modDir | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $c.modDir "KenshiCoop.dll"), (PayloadDllBytes "PRE-EXISTING-V0"))
$shaV0 = Sha256 (Join-Path $c.modDir "KenshiCoop.dll")

$b1 = Invoke-Installer $c @() $null
Check "B1 first install succeeds (exit 0, got $($b1.exit))" ($b1.exit -eq 0)
$shaV1 = Sha256 (Join-Path $c.modDir "KenshiCoop.dll")

# A different payload, then a second install over the top.
[System.IO.File]::WriteAllBytes((Join-Path $c.payload "KenshiCoop.dll"), (PayloadDllBytes "PAYLOAD-V2"))
$b2 = Invoke-Installer $c @("-Force") $null
Check "B2 second install with -Force succeeds (exit 0, got $($b2.exit))" ($b2.exit -eq 0)
$b3 = Invoke-Installer $c @() $null
Check "B3 a second install WITHOUT -Force is refused (exit 3, got $($b3.exit))" ($b3.exit -eq 3)

$dllBaks = @(Get-ChildItem -LiteralPath $c.backup -Filter "KenshiCoop.dll.*.bak" -File)
Check "B4 two separate backups exist for the replaced DLL (found $($dllBaks.Count))" ($dllBaks.Count -eq 2)
Check "B5 their basenames differ" (($dllBaks | Select-Object -ExpandProperty Name | Sort-Object -Unique).Count -eq $dllBaks.Count)
Check "B6 the FIRST install's superseded copy still holds the pre-existing DLL" `
    (@($dllBaks | Where-Object { (Sha256 $_.FullName) -eq $shaV0 }).Count -eq 1)
Check "B7 the SECOND install's backup holds install #1's DLL" `
    (@($dllBaks | Where-Object { (Sha256 $_.FullName) -eq $shaV1 }).Count -eq 1)

# The negative, asserted directly: EVERY backup in the directory still hashes to
# the content its own name encodes. A backup written twice cannot survive this.
$allBaks = @(Get-ChildItem -LiteralPath $c.backup -Filter "*.bak" -File)
$addressOk = $true
foreach ($f in $allBaks) {
    $m = [regex]::Match($f.Name, '\.([0-9a-f]{8})\.bak$')
    if (-not $m.Success) { $addressOk = $false; continue }
    if ((Sha256 $f.FullName).Substring(0, 8) -ne $m.Groups[1].Value) { $addressOk = $false }
}
Check "B8 every backup still hashes to the content its name encodes ($($allBaks.Count) file(s))" ($addressOk -and $allBaks.Count -ge 2)

# And the reason it matters: after two installs, the uninstall must restore the
# PLAYER's original file, not install #1's.
$bu = Invoke-Installer $c @("-Uninstall") $null
Check "B9 uninstall after two installs succeeds (exit 0, got $($bu.exit))" ($bu.exit -eq 0)
Check "B10 the pre-existing DLL is back, not install #1's" `
    ((Sha256 (Join-Path $c.modDir "KenshiCoop.dll")) -eq $shaV0)

# =================================================================== Group C ==
Write-Host ""
Write-Host "-- Group C: the seven prerequisite behaviours, each naming what is missing (D-04) --"

function Assert-NothingWritten($t, [string]$label) {
    Check "$label leaves no mods\KenshiCoop"        (-not (Test-Path $t.modDir))
    Check "$label leaves no mods\KenshiCoop.backup" (-not (Test-Path $t.backup))
}

# C1 - no VC++ 2010 runtime on any search path.
$t = New-SyntheticKenshi "C1"
$r = Invoke-Installer $t @() @($t.nocrt)
Check "C1 no-runtime exits 3 (got $($r.exit))" ($r.exit -eq 3)
foreach ($n in @("mfc100u.dll", "msvcp100.dll", "msvcr100.dll")) {
    Check "C1 the message names $n" ($r.text -match [regex]::Escape($n))
}
Check "C1 the message names the player's own dependencies\vcredist_x64.exe" ($r.text -match 'dependencies\\vcredist_x64\.exe')
Check "C1 the message states the symptom plainly (no start, 0xc0000135, before any log)" `
    ($r.text -match '0xc0000135' -and $r.text -match 'before any log')
Assert-NothingWritten $t "C1 no-runtime"

# C2 - the runtime is present in only SOME of the search locations.
$t = New-SyntheticKenshi "C2"
$r = Invoke-Installer $t @() @($t.nocrt, $t.crt)
Check "C2 partial-runtime installs rather than refusing (exit 0, got $($r.exit))" ($r.exit -eq 0)
Check "C2 a manifest was written" (Test-Path $t.man)
if (Test-Path $t.man) {
    $j = Get-Content -Raw -LiteralPath $t.man | ConvertFrom-Json
    $where = @($j.prerequisites.crt.dlls | ForEach-Object { "$($_.foundAt)" })
    Check "C2 the manifest records WHICH location satisfied the runtime" `
        (@($where | Where-Object { $_ -eq $t.crt }).Count -eq 3)
    Check "C2 the manifest records both searched locations" (@($j.prerequisites.crt.searched).Count -eq 2)
}

# C3 - RE_Kenshi loader absent.
$t = New-SyntheticKenshi "C3"
Remove-Item -LiteralPath (Join-Path $t.k "RE_Kenshi.dll") -Force
$r = Invoke-Installer $t @() $null
Check "C3 no-RE_Kenshi exits 3 (got $($r.exit))" ($r.exit -eq 3)
Check "C3 the message names RE_Kenshi" ($r.text -match 'RE_Kenshi')
Check "C3 the message gives the Nexus page" ($r.text -match 'nexusmods\.com/kenshi/mods/847')
Check "C3 the message says the game runs vanilla without it" ($r.text -match 'runs vanilla')
Assert-NothingWritten $t "C3 no-RE_Kenshi"

# C4 - RE_Kenshi present, its Ogre plugin line absent: REPAIR, do not refuse.
$t = New-SyntheticKenshi "C4"
$cfgRaw = [System.IO.File]::ReadAllText($t.cfg)
WriteText $t.cfg ($cfgRaw -replace "Plugin=RE_Kenshi`r`n", "")
$preLines = @([System.IO.File]::ReadAllText($t.cfg) -split "`r`n" | Where-Object { $_ -ne "" })
$r = Invoke-Installer $t @() $null
Check "C4 missing-plugin-line installs rather than refusing (exit 0, got $($r.exit))" ($r.exit -eq 0)
$cfgAfter = [System.IO.File]::ReadAllText($t.cfg)
Check "C4 the Ogre plugin line was appended" ([regex]::IsMatch($cfgAfter, '(?m)^Plugin=RE_Kenshi\s*$'))
$keptAll = $true
foreach ($l in $preLines) { if (-not $cfgAfter.Contains($l)) { $keptAll = $false } }
Check "C4 every pre-existing line survived, render system and terrain included" `
    ($keptAll -and $cfgAfter -match 'Plugin=RenderSystem_Direct3D11_x64' -and $cfgAfter -match 'Plugin=Plugin_Terrain_x64')
Check "C4 the file is still CRLF throughout" (-not [regex]::IsMatch($cfgAfter, "(?<!`r)`n"))
if (Test-Path $t.man) {
    $j = Get-Content -Raw -LiteralPath $t.man | ConvertFrom-Json
    Check "C4 the manifest records the repair" ($j.prerequisites.ogrePlugin.repaired -eq $true)
    Check "C4 the manifest records Plugins_x64.cfg as replaced, with a verified backup" `
        (@($j.files | Where-Object { $_.path -eq "Plugins_x64.cfg" -and $_.action -eq "replaced" -and $_.backupPath -and $_.backupSha256 }).Count -eq 1)
} else { Check "C4 the manifest exists" $false }

# C5 - unsupported Kenshi version.
$t = New-SyntheticKenshi "C5"
WriteText (Join-Path $t.k "currentVersion.txt") "Kenshi 1.0.64 - x64 (Newland)`r`n"
$r = Invoke-Installer $t @() $null
Check "C5 unsupported-version exits 3 (got $($r.exit))" ($r.exit -eq 3)
Check "C5 the message prints the version FOUND (1.0.64)"     ($r.text -match '1\.0\.64')
Check "C5 the message prints the version SUPPORTED (1.0.65)" ($r.text -match '1\.0\.65')
Assert-NothingWritten $t "C5 unsupported-version"

# C6 - currentVersion.txt missing: a DISTINCT refusal, not "unsupported".
$t = New-SyntheticKenshi "C6"
Remove-Item -LiteralPath (Join-Path $t.k "currentVersion.txt") -Force
$r = Invoke-Installer $t @() $null
Check "C6 missing-version-file exits 3 (got $($r.exit))" ($r.exit -eq 3)
Check "C6 the message says the FILE is missing" ($r.text -match 'currentVersion\.txt is missing')
Check "C6 the message does NOT report it as an unsupported version" (-not ($r.text -match 'and KenshiCoop supports'))
Assert-NothingWritten $t "C6 missing-version-file"

# C7 - currentVersion.txt unparseable: distinct from both of the above.
$t = New-SyntheticKenshi "C7"
WriteText (Join-Path $t.k "currentVersion.txt") "Kenshi - the newest one`r`n"
$r = Invoke-Installer $t @() $null
Check "C7 unparseable-version exits 3 (got $($r.exit))" ($r.exit -eq 3)
Check "C7 the message says it holds no version number" ($r.text -match 'does not contain a version number')
Check "C7 the message quotes what the file actually reads" ($r.text -match 'Kenshi - the newest one')
Check "C7 the message does NOT report it as an unsupported version" (-not ($r.text -match 'and KenshiCoop supports'))
Assert-NothingWritten $t "C7 unparseable-version"

# C8 - the wording is SHARED with kit_preflight.ps1 rather than restated, so a
# player never meets two explanations of the same failure (D-01).
$preflight = Join-Path $scriptsRoot "kit_preflight.ps1"
if (Test-Path $preflight) {
    $pfText  = Get-Content -Raw -LiteralPath $preflight
    $insText = Get-Content -Raw -LiteralPath $installer
    $pfUrl = [regex]::Match($pfText,  'https://www\.nexusmods\.com/kenshi/mods/\d+')
    $inUrl = [regex]::Match($insText, 'https://www\.nexusmods\.com/kenshi/mods/\d+')
    Check "C8 the RE_Kenshi URL matches kit_preflight.ps1's (no second wording)" `
        ($pfUrl.Success -and $inUrl.Success -and $pfUrl.Value -eq $inUrl.Value)
} else {
    Check "C8 scripts\kit_preflight.ps1 present" $false
}

# C9 - a refusal never reports success, and an absent manifest is a refusal with
# instructions rather than a guessing sweep (T-16-03).
$t = New-SyntheticKenshi "C9"
$r = Invoke-Installer $t @("-Uninstall") $null
Check "C9 uninstall with no manifest exits 3 (got $($r.exit))" ($r.exit -eq 3)
Check "C9 the refusal explains what to do instead of guessing" `
    ($r.text -match 'no install manifest' -and $r.text -match 'will not guess')

# =================================================================== Group D ==
Write-Host ""
Write-Host "-- Group D: mutation sensitivity - a green run must mean the test looked --"

# D1 - a manifest entry deleted, so the uninstall skips a file it wrote.
#      Target verdict: treeEqual must go FALSE while the uninstall still exits 0.
$d1 = New-SyntheticKenshi "D1"
$r1 = Invoke-RoundTrip $d1 -Mutate {
    param($t)
    $j = Get-Content -Raw -LiteralPath $t.man | ConvertFrom-Json
    $j.files = @($j.files | Where-Object { $_.path -ne "mods/KenshiCoop/VERSION.txt" })
    [System.IO.File]::WriteAllText($t.man, ($j | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))
}
Check "D1 a deleted manifest entry makes the round-trip measurement FAIL" (-not $r1.treeEqual)
Check "D1 the skipped file really is still on disk" (Test-Path (Join-Path $d1.modDir "VERSION.txt"))
Check "D1 the uninstall itself still reported success (so only A4 catches this)" ($r1.uninstallExit -eq 0)

# D2 - sha256Before altered, so a restore would silently accept wrong content.
#      Target verdict: the uninstall must REFUSE rather than accept it.
$d2 = New-SyntheticKenshi "D2"
$r2 = Invoke-RoundTrip $d2 -Mutate {
    param($t)
    $j = Get-Content -Raw -LiteralPath $t.man | ConvertFrom-Json
    foreach ($f in $j.files) {
        if ($f.path -eq "data/mods.cfg") { $f.sha256Before = ("0" * 64) }
    }
    [System.IO.File]::WriteAllText($t.man, ($j | ConvertTo-Json -Depth 12), (New-Object System.Text.UTF8Encoding($false)))
}
Check "D2 an altered sha256Before makes the uninstall REFUSE (exit 3, got $($r2.uninstallExit))" ($r2.uninstallExit -eq 3)
Check "D2 the refusal says the restore did not produce the recorded content" `
    ($r2.uninstallText -match 'did not produce the content recorded before the install')

# D3 - the backup truncated. Target verdict: refuse, and leave the file alone.
$d3 = New-SyntheticKenshi "D3"
$script:d3InstalledSha = ""
$r3 = Invoke-RoundTrip $d3 -Mutate {
    param($t)
    $j = Get-Content -Raw -LiteralPath $t.man | ConvertFrom-Json
    foreach ($f in $j.files) {
        if ($f.path -eq "data/mods.cfg" -and $f.backupPath) {
            $bak = Join-Path $t.k ($f.backupPath -replace '/', '\')
            [System.IO.File]::WriteAllBytes($bak, [byte[]]@(1, 2, 3))
        }
    }
}
Check "D3 a truncated backup makes the uninstall REFUSE (exit 3, got $($r3.uninstallExit))" ($r3.uninstallExit -eq 3)
Check "D3 the refusal says the backup no longer matches what was recorded" `
    ($r3.uninstallText -match 'no longer matches what was recorded')
Check "D3 nothing was restored over data\mods.cfg from the corrupt backup" `
    ([System.IO.File]::ReadAllText($d3.modsCfg) -match 'KenshiCoop\.mod')

# D4 - a line-ending change in Plugins_x64.cfg. Target verdict: the SEPARATE
#      byte comparison sees it, so a cfg difference cannot hide behind a
#      tree-level pass. (The tree hash sees it too; the point is that the cfg
#      check does not depend on the tree check to catch it.)
$d4 = New-SyntheticKenshi "D4"
$r4 = Invoke-RoundTrip $d4
Check "D4 the unmutated fixture passes the cfg comparison" $r4.pluginsCfgEqual
$restored = [System.IO.File]::ReadAllText($d4.cfg)
WriteText $d4.cfg ($restored -replace "`r`n", "`n")
Check "D4 flipping Plugins_x64.cfg to LF makes the cfg comparison FAIL" `
    (-not (BytesEqual $r4.cfgBefore $d4.cfg))
Check "D4 the mutated file still holds the same TEXT (only the line endings changed)" `
    (([System.IO.File]::ReadAllText($d4.cfg) -replace "`n", "") -eq ($restored -replace "`r`n", ""))

# D5 - a file the player edited after the install must be reported, not deleted.
$d5 = New-SyntheticKenshi "D5"
$i5 = Invoke-Installer $d5 @() $null
WriteText (Join-Path $d5.modDir "RE_Kenshi.json") '{"Plugins":["KenshiCoop.dll"],"edited":true}'
$u5 = Invoke-Installer $d5 @("-Uninstall") $null
Check "D5 an edited file is LEFT in place, not deleted" (Test-Path (Join-Path $d5.modDir "RE_Kenshi.json"))
Check "D5 the player's edit survives byte-for-byte" `
    ((Get-Content -Raw (Join-Path $d5.modDir "RE_Kenshi.json")) -match '"edited":true')
Check "D5 the uninstall says so rather than reporting a clean sweep" `
    ($u5.text -match 'UNINSTALL: PARTIAL' -and $u5.text -match 'changed since it was installed')
Check "D5 the backups are KEPT while anything was left behind" (Test-Path $d5.backup)

} finally {
    if (Test-Path $fixRoot) { Remove-Item -Recurse -Force -Path $fixRoot -ErrorAction SilentlyContinue }
}

# This suite never launches a game and never touches a real Kenshi install. The
# live proof against a clone is a run of install_coop.ps1 recorded in the plan's
# SUMMARY, not exercised here.

Write-Host ""
Write-Host ("Installer: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
