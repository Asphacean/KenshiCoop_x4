<#
.SYNOPSIS
  Fresh-install verification runner (Phase 16 Plan 03, INST-01/04/05).

.DESCRIPTION
  Criterion 1 says "the mod has never been installed on this machine". A clone
  that has been deployed to all milestone is NOT that, so this runner does not
  assume the fresh condition - it MANUFACTURES it and then PROVES it, the way
  scripts\freshstart_session.ps1 proves the config/environment condition.

  What it asserts, in order:

    1. Guard rails, before the first delete: the two protected Steam installs
       are refused BY NAME, a directory with no kenshi_x64.exe is refused by
       name, a Kenshi running OUT OF THE TARGET is refused (a loaded DLL cannot
       be replaced), and a relative or drive-relative -OutDir is refused
       (WINDOWS #15: a swallowed relative path means no evidence is written).
    2. Delete, then ASSERT ABSENT: <Kenshi>\mods\KenshiCoop,
       <Kenshi>\mods\KenshiCoop.backup, and coop_config.json at BOTH locations
       src\plugin\core\Config.cpp::configFilePath() can resolve - next to
       KenshiCoop.dll and the install-root cwd fallback. A delete that quietly
       failed falls through to the assertion; it never aborts with a raw
       provider exception.
    3. Scrub, then ASSERT NONE REMAINS, for every KENSHICOOP_* name SCANNED OUT
       OF src\ on the fly, unioned with whatever this shell inherited. A
       hand-kept list would rot behind the plugin; the launched game inherits
       this process's environment.
    4. Pre-install whole-tree hash (scripts\hash_tree.ps1), save directory
       excluded, so criterion 4 can be re-measured on a REAL install.
    5. The REAL scripts\install_coop.ps1 - not a reimplementation - and the
       deployed DLL asserted byte-identical to src\plugin\x64\Release.
    6. Unless -NoLaunch: launch the clone through scripts\start_kenshi.ps1,
       close the process THIS RUN STARTED (by pid, never by name), and read the
       plugin's own log back.
    7. Uninstall, re-hash, compare with step 4.
    8. preflight.json + result.json in -OutDir, and a real verdict in the exit
       code: 0 pass, 2 refusal, 3 verification failure.

  KENSHICOOP_LOG is deliberately left unset: the plugin then writes its default
  log name into the process working directory, which is both what a player gets
  and the way to avoid WINDOWS #15's silently swallowed relative log path.

.PARAMETER InjectEnvSurvivor
  TEST SEAM, named so it cannot be mistaken for a feature. Re-sets the named
  KENSHICOOP_* variable immediately after the scrub so the "a variable survived
  the scrub" refusal can actually be driven. A refusal path that has never
  fired is a refusal path that does not work.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\freshinstall_check.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$KenshiDir = "F:\KenshiCoop-Clone3",
    [string]$OutDir = "",
    [switch]$Build,
    [switch]$NoLaunch,
    [int]$LaunchSeconds = 75,
    [string]$InjectEnvSurvivor = ""
)

$ErrorActionPreference = "Stop"
$INV = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$isWhatIf  = [bool]$WhatIfPreference

$RunnerVersion = "1.0.0"

$ProtectedInstalls = @(
    "F:\SteamLibrary\steamapps\common\Kenshi",
    "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$installer   = Join-Path $scriptDir "install_coop.ps1"
$hashTree    = Join-Path $scriptDir "hash_tree.ps1"
$startKenshi = Join-Path $scriptDir "start_kenshi.ps1"
$releaseDll  = Join-Path $repoRoot  "src\plugin\x64\Release\KenshiCoop.dll"

$ModDirRel     = "mods\KenshiCoop"
$BackupDirRel  = "mods\KenshiCoop.backup"

# -------------------------------------------------------------------- helpers
$UTF8NB = New-Object System.Text.UTF8Encoding($false)

function Refuse([string]$reason) {
    Write-Host ""
    Write-Host ("REFUSED: {0}" -f $reason)
    exit 2
}

function Fail([string]$reason) {
    Write-Host ""
    Write-Host ("FRESH INSTALL CHECK: FAIL - {0}" -f $reason)
    $script:Problems += $reason
    Write-Result
    exit 3
}

function NormDir([string]$p) {
    if (-not $p) { return "" }
    try { $p = [System.IO.Path]::GetFullPath($p) } catch { }
    return $p.TrimEnd('\', '/').ToLowerInvariant()
}

function NowUtc() {
    return ([DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", $INV))
}

function Sha256File([string]$path) {
    # The .NET reader survives an inherited -WhatIf, which Get-FileHash does
    # not always do cleanly inside a ShouldProcess block (freshstart_session.ps1).
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $fs = [System.IO.File]::OpenRead($path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $sha.Dispose() }
    } finally { $fs.Dispose() }
}

function WriteJson([string]$path, $obj) {
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent -Confirm:$false | Out-Null
    }
    [System.IO.File]::WriteAllText($path, ($obj | ConvertTo-Json -Depth 12), $UTF8NB)
}

# Removes a path and reports whether it is gone AFTERWARDS. A provider error is
# swallowed on purpose: the verdict is the assertion, not the delete's exit.
function RemoveThenAbsent([string]$path) {
    if (Test-Path -LiteralPath $path) {
        if ($isWhatIf) { return $true }
        try { Remove-Item -LiteralPath $path -Recurse -Force -Confirm:$false -ErrorAction Stop } catch { }
    }
    return (-not (Test-Path -LiteralPath $path))
}

$script:Problems = @()
$script:Result   = [ordered]@{}

function Write-Result() {
    if ($isWhatIf -or -not $script:OutAbs) { return }
    $script:Result["problems"]  = @($script:Problems)
    $script:Result["verdict"]   = $(if ($script:Problems.Count -eq 0) { "PASS" } else { "FAIL" })
    $script:Result["finishedUtc"] = (NowUtc)
    WriteJson (Join-Path $script:OutAbs "result.json") $script:Result
}

Write-Host "== KenshiCoop fresh-install check (Phase 16 Plan 03) =="
Write-Host ("  runner {0}   started {1}" -f $RunnerVersion, (NowUtc))
if ($isWhatIf) { Write-Host "  -WhatIf: the whole plan is printed and NOTHING is changed." }
Write-Host ""

# ======================================================= 0. guard rails ======
Write-Host "=== 0. guard rails (all of them BEFORE the first delete) ==="

# --- -OutDir must be absolute, and not drive-relative -----------------------
if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot ("tools\test-runs\phase16_freshinstall_" + [DateTime]::UtcNow.ToString("yyyyMMdd_HHmmss", $INV))
}
if ($OutDir -match '^[A-Za-z]:(?![\\/])') {
    Refuse ("-OutDir '$OutDir' is DRIVE-RELATIVE. [System.IO.Path]::IsPathRooted says true for it, but it resolves against that drive's current directory, so the evidence would land somewhere nobody looks. Pass a fully qualified path such as '$repoRoot\tools\test-runs\phase16_freshinstall'.")
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    Refuse ("-OutDir '$OutDir' is a RELATIVE path. A relative output directory is swallowed silently and no evidence is written. Pass a fully qualified path such as '$repoRoot\tools\test-runs\phase16_freshinstall'.")
}
$script:OutAbs = [System.IO.Path]::GetFullPath($OutDir).TrimEnd('\', '/')
Write-Host ("  [ok] -OutDir is absolute: {0}" -f $script:OutAbs)

# --- the target must exist, be a Kenshi install, and not be protected -------
if (-not $KenshiDir) {
    Refuse "-KenshiDir is empty. Point it at the folder that holds kenshi_x64.exe."
}
# The protected-install refusal is asserted BEFORE the existence check on
# purpose: whether this runner may be aimed at the developer's real Kenshi must
# not depend on that install happening to be present on the machine.
$kd = [System.IO.Path]::GetFullPath($KenshiDir).TrimEnd('\', '/')
foreach ($prot in $ProtectedInstalls) {
    if ((NormDir $kd) -eq (NormDir $prot)) {
        Refuse ("'$prot' is a PROTECTED Kenshi install and this runner DELETES before it installs. It may never be aimed there. Use a clone, for example F:\KenshiCoop-Clone3.")
    }
}
Write-Host ("  [ok] '{0}' is neither of the two protected installs." -f $kd)

if (-not (Test-Path -LiteralPath $kd)) {
    Refuse "there is no directory at '$kd'. Point -KenshiDir at the folder that holds kenshi_x64.exe."
}

if (-not (Test-Path -LiteralPath (Join-Path $kd "kenshi_x64.exe"))) {
    Refuse "'$kd' is not a Kenshi installation: there is no kenshi_x64.exe in it."
}
Write-Host "  [ok] kenshi_x64.exe is present."

# --- no Kenshi running OUT OF THE TARGET ------------------------------------
# Scoped to the target on purpose (16-02 deviation 5): an instance running out
# of a DIFFERENT folder holds no file in this one open, and this rig routinely
# has other clones - and the developer's own game - up.
$mine = @()
$others = @()
foreach ($p in @(Get-Process -Name "kenshi_x64" -ErrorAction SilentlyContinue)) {
    $ppath = ""
    try { $ppath = $p.Path } catch { $ppath = "" }
    if (-not $ppath) { $mine += ("pid " + $p.Id + " (path unreadable)") ; continue }
    if ((NormDir (Split-Path -Parent $ppath)) -eq (NormDir $kd)) { $mine += ("pid " + $p.Id + " " + $ppath) }
    else { $others += ("pid " + $p.Id + " " + $ppath) }
}
if ($mine.Count -gt 0) {
    Refuse ("Kenshi is RUNNING out of the target install ('$kd'): " + ($mine -join "; ") + ". A loaded KenshiCoop.dll cannot be replaced and cannot be deleted, so both the delete and the deploy would fail halfway. Close that game and re-run.")
}
Write-Host "  [ok] no Kenshi is running out of the target install."
if ($others.Count -gt 0) {
    Write-Host ("  note: {0} unrelated Kenshi instance(s) elsewhere, LEFT ALONE: {1}" -f $others.Count, ($others -join "; "))
}

foreach ($need in @($installer, $hashTree, $releaseDll)) {
    if (-not (Test-Path -LiteralPath $need)) {
        if ($need -eq $releaseDll) {
            Refuse "the Release build is missing ('$releaseDll'). Build it with: scripts\build_plugin.cmd Release"
        }
        Refuse "a required script is missing: '$need'."
    }
}
Write-Host "  [ok] install_coop.ps1, hash_tree.ps1 and the Release DLL are present."

# --- what the installer would place must BE the Release build ---------------
# install_coop.ps1 resolves dist\mod-kit\KenshiCoop by default. A stale kit
# would deploy a DLL that is not the one this repo calls Release, and criterion
# 1's claim is about what a PLAYER gets, so a stale kit is a refusal rather
# than a silent substitution.
$kitDll = Join-Path $repoRoot "dist\mod-kit\KenshiCoop\KenshiCoop.dll"
if (Test-Path -LiteralPath $kitDll) {
    $kitSha  = Sha256File $kitDll
    $relSha0 = Sha256File $releaseDll
    if ($kitSha -ne $relSha0) {
        Refuse ("the payload install_coop.ps1 resolves by default ('$kitDll', $kitSha) is NOT byte-identical to the canonical Release build ('$releaseDll', $relSha0). Refresh it with: scripts\make_mod_kit.ps1 -SkipBuild")
    }
    Write-Host ("  [ok] the default payload is byte-identical to the Release build ({0})." -f $kitSha.Substring(0, 8))
}

# ---------------------------------------------------- optional Release build
if ($Build) {
    $cmd = Join-Path $scriptDir "build_plugin.cmd"
    if ($isWhatIf) {
        Write-Host ("  WOULD build: {0} Release" -f $cmd)
    } else {
        Write-Host "  building Release ..."
        & cmd.exe /c "`"$cmd`" Release" | Out-Null
        if ($LASTEXITCODE -ne 0) { Refuse "the Release build failed (scripts\build_plugin.cmd Release exited $LASTEXITCODE)." }
    }
}

# ======================== 1. the KENSHICOOP_* name set, scanned out of src\ ===
Write-Host ""
Write-Host "=== 1. KENSHICOOP_* names, SCANNED OUT OF src\ (never a hand list) ==="
$srcRoot = Join-Path $repoRoot "src"
$scanned = New-Object System.Collections.Generic.HashSet[string]
if (Test-Path -LiteralPath $srcRoot) {
    foreach ($f in @(Get-ChildItem -LiteralPath $srcRoot -Recurse -File -ErrorAction SilentlyContinue)) {
        $txt = ""
        try { $txt = [System.IO.File]::ReadAllText($f.FullName) } catch { continue }
        foreach ($m in [regex]::Matches($txt, 'KENSHICOOP_[A-Z0-9_]+')) {
            [void]$scanned.Add($m.Value)
        }
    }
}
if ($scanned.Count -eq 0) {
    Refuse "the KENSHICOOP_* scan over '$srcRoot' found NOTHING. A scan that returns an empty set would make the environment assertion vacuous, so it is a refusal rather than a pass."
}
$inherited = @(Get-ChildItem Env: | Where-Object { $_.Name -like "KENSHICOOP_*" } | ForEach-Object { $_.Name })
foreach ($n in $inherited) { [void]$scanned.Add($n) }
$envNames = @($scanned) | Sort-Object
Write-Host ("  scanned out of src\: {0} name(s); inherited by this shell: {1}; union: {2}" -f `
    ($scanned.Count - @($inherited | Where-Object { $_ }).Count), $inherited.Count, $envNames.Count)

# ============================================ 2. the paths the fresh state is =
$modDir     = Join-Path $kd $ModDirRel
$backupDir  = Join-Path $kd $BackupDirRel
# BOTH locations Config.cpp::configFilePath() can resolve.
$cfgBesideDll = Join-Path $modDir "coop_config.json"
$cfgCwd       = Join-Path $kd "coop_config.json"
$freshPaths = @($modDir, $backupDir, $cfgBesideDll, $cfgCwd)

$preflight = [ordered]@{
    schemaVersion     = 1
    runnerVersion     = $RunnerVersion
    startedUtc        = (NowUtc)
    kenshiDir         = $kd
    outDir            = $script:OutAbs
    whatIf            = $isWhatIf
    protectedInstalls = @($ProtectedInstalls)
    freshPathsChecked = @($freshPaths)
    configPaths       = [ordered]@{ besideDll = $cfgBesideDll; cwdFallback = $cfgCwd }
    envNamesScanned   = @($envNames)
    envNameCount      = $envNames.Count
    envInherited      = @($inherited)
    releaseDll        = $releaseDll
    noLaunch          = [bool]$NoLaunch
    launchSeconds     = $LaunchSeconds
    otherKenshiRunning = @($others)
}

Write-Host ""
Write-Host "=== 2. delete, then ASSERT ABSENT (mod dir, backup dir, BOTH config paths) ==="
$absent = [ordered]@{}
foreach ($p in $freshPaths) {
    if ($isWhatIf) {
        Write-Host ("  WOULD delete then assert absent: {0}" -f $p)
        $absent[$p] = $true
        continue
    }
    $ok = RemoveThenAbsent $p
    $absent[$p] = $ok
    if ($ok) { Write-Host ("  ASSERTED ABSENT: {0}" -f $p) }
    else     { Write-Host ("  STILL PRESENT  : {0}" -f $p) }
}
$preflight["assertedAbsent"] = $absent
if (-not $isWhatIf) {
    $survivors = @($freshPaths | Where-Object { -not $absent[$_] })
    if ($survivors.Count -gt 0) {
        $bad = @($survivors | Where-Object { $_ -like "*coop_config.json" })
        if ($bad.Count -gt 0) {
            Refuse ("coop_config.json could not be removed from " + ($bad -join " and ") + ". A stale config beside the DLL is an INVISIBLE INPUT to the plugin, so the fresh condition would be a fiction. Close whatever holds the file and re-run.")
        }
        Refuse ("the fresh condition could not be established; these survived the delete: " + ($survivors -join "; ") + ".")
    }
}

# =============================================== 3. scrub the environment ====
Write-Host ""
Write-Host "=== 3. KENSHICOOP_* environment: scrub, then ASSERT NONE REMAINS ==="
if ($isWhatIf) {
    Write-Host ("  WOULD scrub {0} KENSHICOOP_* name(s) and assert none remains." -f $envNames.Count)
} else {
    foreach ($n in $envNames) {
        if (Test-Path -LiteralPath ("Env:" + $n)) { Remove-Item -LiteralPath ("Env:" + $n) -Force -ErrorAction SilentlyContinue }
    }
    if ($InjectEnvSurvivor) {
        # TEST SEAM - see .PARAMETER InjectEnvSurvivor.
        Set-Item -LiteralPath ("Env:" + $InjectEnvSurvivor) -Value "injected-by-test"
    }
    $remaining = @(Get-ChildItem Env: | Where-Object { $_.Name -like "KENSHICOOP_*" } | ForEach-Object { $_.Name })
    if ($remaining.Count -gt 0) {
        $preflight["envRemaining"] = @($remaining)
        Refuse ("a KENSHICOOP_* variable survived the scrub and the launched game would INHERIT it: " + ($remaining -join ", ") + ".")
    }
    Write-Host ("  ASSERTED: none of the {0} scanned KENSHICOOP_* names remains in the environment the game inherits." -f $envNames.Count)
}
$preflight["envRemaining"] = @()

if ($isWhatIf) {
    Write-Host ""
    Write-Host "=== 4-7. WOULD do, in order ==="
    Write-Host ("  hash the tree      : {0} -Root {1} (save\ excluded)" -f $hashTree, $kd)
    Write-Host ("  install            : {0} -KenshiDir {1} -OutDir {2}" -f $installer, $kd, $script:OutAbs)
    Write-Host ("  assert DLL equals  : {0}" -f $releaseDll)
    if ($NoLaunch) { Write-Host "  launch             : SKIPPED (-NoLaunch)" }
    else           { Write-Host ("  launch             : {0} for {1}s, then close the pid THIS RUN started" -f $startKenshi, $LaunchSeconds) }
    Write-Host ("  read VERSION.txt   : {0}\VERSION.txt, and {1} -Info" -f $modDir, $installer)
    Write-Host ("  uninstall          : {0} -KenshiDir {1} -Uninstall" -f $installer, $kd)
    Write-Host ("  re-hash and compare with the pre-install measurement")
    Write-Host ("  write              : {0}\preflight.json and {0}\result.json" -f $script:OutAbs)
    Write-Host ""
    Write-Host "FRESH INSTALL CHECK: -WhatIf - plan printed, nothing changed."
    exit 0
}

New-Item -ItemType Directory -Force -Path $script:OutAbs -Confirm:$false | Out-Null
WriteJson (Join-Path $script:OutAbs "preflight.json") $preflight
$script:Result = [ordered]@{
    schemaVersion = 1
    runnerVersion = $RunnerVersion
    preflight     = $preflight
}

# ================================================ 4. pre-install tree hash ===
Write-Host ""
Write-Host "=== 4. pre-install whole-tree hash ==="
$excl = @("save", "*.log", "*.txt.bak")
$preHashFile = Join-Path $script:OutAbs "tree_pre.txt"
& powershell -NoProfile -ExecutionPolicy Bypass -File $hashTree -Root $kd -Exclude ($excl -join ";") -OutFile $preHashFile | Out-Null
if (-not (Test-Path -LiteralPath $preHashFile)) { Fail "the pre-install tree hash produced no output file." }
$preSha = Sha256File $preHashFile
$preCount = @(Get-Content -LiteralPath $preHashFile).Count
Write-Host ("  pre-install : {0}  ({1} entries)" -f $preSha, $preCount)
$script:Result["treePre"] = [ordered]@{ sha256 = $preSha; entries = $preCount; exclude = @($excl) }

# ==================================================== 5. the real installer ==
Write-Host ""
Write-Host "=== 5. the REAL scripts\install_coop.ps1 ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -KenshiDir $kd -OutDir (Join-Path $script:OutAbs "install")
$installExit = $LASTEXITCODE
Write-Host ("  install exit: {0}" -f $installExit)
$script:Result["installExit"] = $installExit
if ($installExit -ne 0) { Fail "install_coop.ps1 exited $installExit." }

$deployedDll = Join-Path $modDir "KenshiCoop.dll"
$deployedSha = Sha256File $deployedDll
$canonSha    = Sha256File $releaseDll
Write-Host ("  deployed DLL: {0}" -f $deployedSha)
Write-Host ("  Release  DLL: {0}" -f $canonSha)
$script:Result["dll"] = [ordered]@{ deployed = $deployedSha; canonicalRelease = $canonSha; identical = ($deployedSha -eq $canonSha -and $deployedSha -ne "") }
if ($deployedSha -eq "" -or $deployedSha -ne $canonSha) {
    Fail "the deployed DLL is not byte-identical to src\plugin\x64\Release\KenshiCoop.dll."
}
Write-Host "  ASSERTED: the deployed DLL is byte-identical to the Release build."

# ======================================================== 6. launch + banner =
$script:Result["launch"] = [ordered]@{ attempted = (-not $NoLaunch) }
if ($NoLaunch) {
    Write-Host ""
    Write-Host "=== 6. launch: SKIPPED (-NoLaunch) - the load banner is NOT observed in this run ==="
    $script:Result["launch"]["observed"] = $false
    $script:Result["launch"]["note"]     = "-NoLaunch was passed: this run proves the install and the file state, and NOTHING about the plugin loading."
} else {
    Write-Host ""
    Write-Host "=== 6. launch the clone and read the plugin's OWN log back ==="
    # KENSHICOOP_LOG stays unset on purpose: the plugin writes KenshiCoop_host.log
    # into the process working directory, which is what a player gets.
    $logPath = Join-Path $kd "KenshiCoop_host.log"
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue }
    $before = @(Get-Process -Name "kenshi_x64" -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    & powershell -NoProfile -ExecutionPolicy Bypass -File $startKenshi -ExePath (Join-Path $kd "kenshi_x64.exe") -WorkDir $kd -TimeoutSec $LaunchSeconds | Out-Null
    Start-Sleep -Seconds $LaunchSeconds
    # Close ONLY what this run started, by pid. Never by name: the developer's
    # own game and other clones are routinely up on this machine.
    $started = @(Get-Process -Name "kenshi_x64" -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
    foreach ($p in $started) {
        $ppath = ""
        try { $ppath = $p.Path } catch { }
        if ($ppath -and (NormDir (Split-Path -Parent $ppath)) -ne (NormDir $kd)) { continue }
        Write-Host ("  closing the process THIS RUN started: pid {0}" -f $p.Id)
        try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { }
    }
    Start-Sleep -Seconds 3
    $lines = @()
    if (Test-Path -LiteralPath $logPath) { $lines = @(Get-Content -LiteralPath $logPath | ForEach-Object { "$_" }) }
    $loaded = @($lines | Where-Object { $_ -match 'KenshiCoop loaded!' })       | Select-Object -First 1
    $build  = @($lines | Where-Object { $_ -match 'KenshiCoop: build ' })       | Select-Object -First 1
    $role   = @($lines | Where-Object { $_ -match 'role=.* proto=v' })          | Select-Object -First 1
    $script:Result["launch"]["logPath"]  = $logPath
    $script:Result["launch"]["loaded"]   = "$loaded"
    $script:Result["launch"]["build"]    = "$build"
    $script:Result["launch"]["role"]     = "$role"
    $script:Result["launch"]["observed"] = [bool]($loaded -and $build -and $role)
    Write-Host ("  loaded : {0}" -f $loaded)
    Write-Host ("  build  : {0}" -f $build)
    Write-Host ("  role   : {0}" -f $role)
    if (-not $script:Result["launch"]["observed"]) {
        $script:Problems += "the plugin's load banner was not found in '$logPath'."
    }
}

# ================================================= 7. version cross-check ====
Write-Host ""
Write-Host "=== 7. version, readable WITHOUT launching the game ==="
$versionTxt = Join-Path $modDir "VERSION.txt"
$vLines = @()
# "$_" makes a PLAIN string. Get-Content decorates every line with PSPath /
# PSProvider note-properties, and ConvertTo-Json -Depth 12 then walks into the
# FileSystem provider object and never comes back - the run hung here after
# every measurement had already been taken, losing the record it existed to
# write. Anything from Get-Content that reaches the JSON must be flattened.
if (Test-Path -LiteralPath $versionTxt) {
    $vLines = @(Get-Content -LiteralPath $versionTxt | ForEach-Object { "$_" })
}
$vBuild = ""
$vProto = ""
foreach ($l in $vLines) {
    if ($l -match '(?i)build\s*:\s*(.+?)(\s*\(|$)')    { if (-not $vBuild) { $vBuild = $Matches[1].Trim() } }
    if ($l -match '(?i)protocol\s*:\s*v?([0-9]+)')     { if (-not $vProto) { $vProto = $Matches[1].Trim() } }
}
$wireProto = ""
$wireH = Join-Path $repoRoot "src\netproto\Wire.h"
if (Test-Path -LiteralPath $wireH) {
    $m = [regex]::Match([System.IO.File]::ReadAllText($wireH), 'PROTOCOL_VERSION\s*=\s*([0-9]+)')
    if ($m.Success) { $wireProto = $m.Groups[1].Value }
}
$logBuild = ""
$logProto = ""
if ($script:Result["launch"]["build"]) {
    $m = [regex]::Match([string]$script:Result["launch"]["build"], 'KenshiCoop: build (.+)$')
    if ($m.Success) { $logBuild = $m.Groups[1].Value.Trim() }
}
if ($script:Result["launch"]["role"]) {
    $m = [regex]::Match([string]$script:Result["launch"]["role"], 'proto=v([0-9]+)')
    if ($m.Success) { $logProto = $m.Groups[1].Value }
}

$infoOut = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -KenshiDir $kd -Info 2>&1)
$infoExit = $LASTEXITCODE
$infoText = ($infoOut -join "`n")

$protoSources = @{}
$protoSources["VERSION.txt"] = $vProto
$protoSources["Wire.h"]      = $wireProto
if ($logProto) { $protoSources["plugin log"] = $logProto }
# The @() must wrap the WHOLE pipeline: `@(x) | Sort-Object` hands a
# one-element result back UNWRAPPED, and $protoValues[0] would then be the
# first CHARACTER of "61". That is the very defect 9b7665a fixed in
# install_coop.ps1, reproduced here during this plan's own live run.
$protoValues = @($protoSources.Values | Where-Object { $_ } | Sort-Object -Unique)
$protoAgree  = ($protoValues.Count -eq 1 -and $protoValues[0] -eq "61")

$buildSources = @{}
$buildSources["VERSION.txt"] = $vBuild
if ($logBuild) { $buildSources["plugin log"] = $logBuild }
$buildValues = @($buildSources.Values | Where-Object { $_ } | Sort-Object -Unique)
# One source is not an agreement. With -NoLaunch there is no plugin log, so the
# build stamp is REPORTED and explicitly marked as not cross-checked rather
# than passed on a single reading.
$buildCrossChecked = (@($buildSources.Values | Where-Object { $_ }).Count -ge 2)
$buildAgree  = ($buildCrossChecked -and $buildValues.Count -eq 1)

$protoSourceCount = @($protoSources.Values | Where-Object { $_ }).Count
$script:Result["version"] = [ordered]@{
    versionTxtPath = $versionTxt
    versionTxt     = @($vLines)
    buildSources   = $buildSources
    buildAgree     = $buildAgree
    buildCrossChecked = $buildCrossChecked
    protocolSourceCount = $protoSourceCount
    protocolSources = $protoSources
    protocolAgree  = $protoAgree
    protocolExpected = "61"
    infoExit       = $infoExit
    infoOutput     = @($infoOut | ForEach-Object { "$_" })
    dllOnDiskSha256 = (Sha256File $deployedDll)
    dllRecordedSha256 = $deployedSha
}
Write-Host ("  VERSION.txt build : {0}" -f $vBuild)
Write-Host ("  plugin log build  : {0}" -f $(if ($logBuild) { $logBuild } else { "(not observed - no launch)" }))
Write-Host ("  protocol          : VERSION.txt={0} log={1} Wire.h={2}  agree-on-61={3}" -f `
    $(if ($vProto) { $vProto } else { "?" }), $(if ($logProto) { $logProto } else { "-" }), `
    $(if ($wireProto) { $wireProto } else { "?" }), $protoAgree)
Write-Host ("  -Info exit        : {0}" -f $infoExit)
if (-not $protoAgree)  { $script:Problems += "the protocol version does not agree across VERSION.txt, the plugin log and Wire.h (expected 61)." }
if ($buildCrossChecked -and -not $buildAgree) { $script:Problems += "the build stamp does not agree between VERSION.txt and the plugin's own log." }
if (-not $buildCrossChecked) { Write-Host "  NOTE: the build stamp was read from ONE source only; it is reported, NOT cross-checked." }
if ($infoExit -ne 0)   { $script:Problems += "install_coop.ps1 -Info exited $infoExit." }
if ((Sha256File $deployedDll) -ne $deployedSha) { $script:Problems += "the installed DLL changed on disk between install and read-back." }

# ============================================ 8. uninstall and re-measure ====
Write-Host ""
Write-Host "=== 8. uninstall, then re-measure the whole tree ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -KenshiDir $kd -Uninstall -OutDir (Join-Path $script:OutAbs "uninstall")
$uninstallExit = $LASTEXITCODE
Write-Host ("  uninstall exit: {0}" -f $uninstallExit)
$script:Result["uninstallExit"] = $uninstallExit
if ($uninstallExit -ne 0) { $script:Problems += "install_coop.ps1 -Uninstall exited $uninstallExit." }

$postHashFile = Join-Path $script:OutAbs "tree_post.txt"
& powershell -NoProfile -ExecutionPolicy Bypass -File $hashTree -Root $kd -Exclude ($excl -join ";") -OutFile $postHashFile | Out-Null
$postSha = Sha256File $postHashFile
$postCount = 0
if (Test-Path -LiteralPath $postHashFile) { $postCount = @(Get-Content -LiteralPath $postHashFile).Count }
$identical = ($preSha -ne "" -and $preSha -eq $postSha)
Write-Host ("  post-uninstall: {0}  ({1} entries)" -f $postSha, $postCount)
Write-Host ("  byte-identical to pre-install: {0}" -f $identical)
$diff = @()
if (-not $identical -and (Test-Path -LiteralPath $postHashFile)) {
    $diff = @(Compare-Object (Get-Content -LiteralPath $preHashFile) (Get-Content -LiteralPath $postHashFile) |
              ForEach-Object { ("{0} {1}" -f $_.SideIndicator, $_.InputObject) })
    Write-Host "  --- diff (verbatim, NOT re-run for a cleaner result) ---"
    foreach ($d in $diff) { Write-Host ("  " + $d) }
    Write-Host "  --- end ---"
}
$script:Result["treePost"] = [ordered]@{ sha256 = $postSha; entries = $postCount; identicalToPre = $identical; diff = @($diff) }
if (-not $identical) { $script:Problems += "the post-uninstall tree hash is NOT identical to the pre-install one." }

# what this run did NOT observe - stated in the record, not left to the reader
$script:Result["notObserved"] = @(
    "UI-01..UI-04: this run did not drive the F2 panel. A boot probe proves the install and load path and NOTHING about what a player does in the panel.",
    "WINDOWS #19 / #22: the reconnect slot leak is untouched by this run and remains release-gating."
)
if ($NoLaunch) {
    $script:Result["notObserved"] += "The plugin's load banner: -NoLaunch was passed, so the game was never started from this install."
}
# git writes to stderr on a bad invocation and $ErrorActionPreference = "Stop"
# turns that into a terminating NativeCommandError - which would kill the run
# AFTER every measurement was taken but BEFORE result.json was written. The
# record is the deliverable; nothing this late may be allowed to lose it.
$head = ""
try {
    $ErrorActionPreference = "Continue"
    $head = (& git -C $repoRoot rev-parse HEAD 2>$null)
} catch { $head = "" } finally { $ErrorActionPreference = "Stop" }
$script:Result["repoHeadSha"] = "$head"

Write-Result
Write-Host ""
if ($script:Problems.Count -eq 0) {
    Write-Host "FRESH INSTALL CHECK: PASS"
    Write-Host ("  record: {0}\result.json" -f $script:OutAbs)
    exit 0
}
Write-Host "FRESH INSTALL CHECK: FAIL"
foreach ($p in $script:Problems) { Write-Host ("  - " + $p) }
Write-Host ("  record: {0}\result.json" -f $script:OutAbs)
exit 3
