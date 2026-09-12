<#
.SYNOPSIS
  Install, verify and REMOVE KenshiCoop in a Kenshi installation, with every
  replaced file backed up first and the backup proved by hash before the
  original is touched.

.DESCRIPTION
  This script writes into a directory it does not own and cannot recreate, so
  the backup and the uninstall are its substance, not its bookkeeping.

  Three front ends, one code path:

    -Install    (default)  locate, detect prerequisites, back up, write, record
    -Uninstall             reverse INSTALL-MANIFEST.json, verifying every hash
    -Info                  read the installed build back, with no writes at all

  THE RULES THIS SCRIPT EXISTS TO ENFORCE

  1. Backups are ADDRESSED BY CONTENT and VERIFIED BY HASH.
     A backup is named <basename>.<first 8 hex of its sha256>.bak, is re-opened
     and re-hashed after it is written, and the original is touched only when
     that hash matches. A fixed backup basename means the SECOND install
     silently destroys the FIRST install's superseded copy - the exact defect
     docs\CROSS_MACHINE_RIG.md section 2 records, and one a naive backup step
     reproduced again during this milestone.

  2. Prerequisites are detected BY FILE PRESENCE, never by an installer's exit
     code. "dependencies\vcredist_x64.exe /q" has been observed to complete
     silently under Wine while installing nothing, so its return value proves
     nothing. Only the three runtime DLL names being findable counts.

  3. Uninstall is MANIFEST-DRIVEN, never inferred. A file whose current hash
     differs from the one recorded at install time is REPORTED AND LEFT IN
     PLACE - the player may have edited it, and silently deleting someone's
     edits is worse than leaving a stray file. An absent manifest is a refusal
     with instructions, not a best-effort sweep.

  4. The game executable is NEVER touched. On Windows the root kenshi_x64.exe
     is pristine and RE_Kenshi keeps its own downgraded copy inside
     <Kenshi>\RE_Kenshi\. The write set here is the mod folder, and at most one
     appended line each in Plugins_x64.cfg and data\mods.cfg.

  5. Nothing is redistributed. Kenshi and RE_Kenshi are DETECTED and NAMED;
     not a byte of either is copied by this script.

  WHAT IT WRITES (and nothing else)

    <Kenshi>\mods\KenshiCoop\KenshiCoop.dll
    <Kenshi>\mods\KenshiCoop\RE_Kenshi.json
    <Kenshi>\mods\KenshiCoop\KenshiCoop.mod
    <Kenshi>\mods\KenshiCoop\VERSION.txt
    <Kenshi>\Plugins_x64.cfg        one appended line, only if RE_Kenshi is
                                    installed and its Ogre plugin line is absent
    <Kenshi>\data\mods.cfg          one appended line, only if absent
    <Kenshi>\mods\KenshiCoop.backup\*.bak
    <Kenshi>\mods\KenshiCoop.backup\INSTALL-MANIFEST.json

  coop_config.json is deliberately NOT installed: since Phase 15 a fresh
  install with no config file is a working state (the F2 panel arms the
  endpoint), and writing one would overwrite a player's LAN settings on every
  re-install.

.PARAMETER KenshiDir
  The Kenshi installation to act on. When omitted, the script auto-detects and
  proceeds only if exactly one candidate is found.

.PARAMETER Source
  Directory holding KenshiCoop.dll, RE_Kenshi.json and KenshiCoop.mod. When
  omitted, resolved from dist\mod-kit\KenshiCoop, then a KenshiCoop folder
  beside this script, then the repo's Release build.

.PARAMETER OutDir
  ABSOLUTE directory for the install/uninstall record. A relative path is
  refused: WINDOWS #15 - it is swallowed silently and no evidence is written.

.PARAMETER CrtSearchPath
  Where to look for the VC++ 2010 runtime DLLs. Defaults to %WINDIR%\System32
  plus the install root. Supplying it REPLACES the defaults, which is the seam
  the test suite uses to exercise the missing-runtime path without touching the
  real system.

  Several directories may be given either as an array (when the script is
  dot-invoked or called with &) or as one ';'-separated string. The second form
  exists because this project invokes every script as
  "powershell -NoProfile -ExecutionPolicy Bypass -File <script>", and under
  -File an array parameter cannot take more than one bare token: the second one
  fails to bind with "A positional parameter cannot be found". Without the
  ';' form the seam would be unusable from the only invocation style the
  project actually uses.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_coop.ps1 -KenshiDir "C:\Games\Kenshi" -WhatIf

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_coop.ps1 -KenshiDir "C:\Games\Kenshi"

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_coop.ps1 -KenshiDir "C:\Games\Kenshi" -Uninstall

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_coop.ps1 -KenshiDir "C:\Games\Kenshi" -Info
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$KenshiDir = "",
    [string]$Source = "",
    [switch]$Uninstall,
    [switch]$Info,
    [switch]$Force,
    [string]$OutDir = "",
    [string[]]$CrtSearchPath = @(),
    # Uninstall only: leave the backup directory in place instead of removing
    # the backups this manifest owns once every restore has been verified.
    [switch]$KeepBackups
)

$ErrorActionPreference = "Stop"
$INV = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$isWhatIf  = [bool]$WhatIfPreference

# ------------------------------------------------------------------ constants
$InstallerVersion = "1.0.0"
$SchemaVersion    = 1

# The manifest schema version is a CONTRACT: plan 16-02's POSIX front end emits
# the same keys, and an uninstall reads manifests written by either one.

$CrtDllNames   = @("mfc100u.dll", "msvcp100.dll", "msvcr100.dll")
$SupportedKenshiVersions = @("1.0.65")
$OgrePluginLine = "Plugin=RE_Kenshi"
$ModListEntry   = "KenshiCoop.mod"
$PayloadNames   = @("KenshiCoop.dll", "RE_Kenshi.json", "KenshiCoop.mod")

# Wording taken verbatim from scripts\kit_preflight.ps1's message register so a
# player never meets two different explanations of the same failure.
$ReKenshiUrl  = "https://www.nexusmods.com/kenshi/mods/847"
$ReKenshiWhy  = "Without it the co-op plugin is never loaded and the game runs vanilla."

$ModDirRel    = "mods/KenshiCoop"
$BackupDirRel = "mods/KenshiCoop.backup"
$ManifestName = "INSTALL-MANIFEST.json"

# Real game installs on this project's machines. The guard is active only when
# this script is run from the development checkout (see Test-DevRepo): a player
# running the shipped copy must be able to install into their own Steam folder.
$ProtectedInstalls = @(
    "F:\SteamLibrary\steamapps\common\Kenshi",
    "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

# ------------------------------------------------------------------- helpers
$script:Journal = @()   # what this invocation has changed, oldest first

function Undo-Journal {
    # Put the install back the way it was when a write fails halfway. Reverse
    # order, and never noisier than the failure that triggered it.
    if ($script:Journal.Count -eq 0) { return }
    Write-Host ""
    Write-Host "Rolling back this invocation (newest change first):"
    for ($i = $script:Journal.Count - 1; $i -ge 0; $i--) {
        $e = $script:Journal[$i]
        try {
            switch ($e.kind) {
                "file-created" {
                    if (Test-Path -LiteralPath $e.path) {
                        Remove-Item -LiteralPath $e.path -Force -Confirm:$false -ErrorAction SilentlyContinue
                        Write-Host ("  removed  " + $e.path)
                    }
                }
                "file-replaced" {
                    if (Test-Path -LiteralPath $e.backup) {
                        Copy-Item -LiteralPath $e.backup -Destination $e.path -Force -Confirm:$false -ErrorAction SilentlyContinue
                        Write-Host ("  restored " + $e.path)
                    } else {
                        Write-Host ("  COULD NOT RESTORE " + $e.path + " - backup missing: " + $e.backup)
                    }
                }
                "dir-created" {
                    if (Test-Path -LiteralPath $e.path) {
                        $left = @(Get-ChildItem -LiteralPath $e.path -Force -ErrorAction SilentlyContinue)
                        if ($left.Count -eq 0) {
                            Remove-Item -LiteralPath $e.path -Force -Confirm:$false -ErrorAction SilentlyContinue
                            Write-Host ("  removed  " + $e.path)
                        }
                    }
                }
            }
        } catch { }
    }
    $script:Journal = @()
}

function Refuse([string]$reason) {
    Undo-Journal
    Write-Host ""
    Write-Host "REFUSED: $reason"
    exit 3
}

function NowUtc() { (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", $INV) }

function Sha256([string]$path) {
    # .NET rather than Get-FileHash: Get-FileHash honours an inherited -WhatIf
    # and returns nothing, which would print a blank hash for the very file the
    # -WhatIf plan is about to name as its backup.
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($path))
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace("-", "").ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function NormDir([string]$p) {
    if (-not $p) { return "" }
    return ($p.TrimEnd('\', '/')).ToLowerInvariant()
}

function RelPath([string]$root, [string]$full) {
    # Manifest paths are relative to the install root and use forward slashes,
    # so the same manifest reads identically on Windows and under Proton/Wine.
    $r = [System.IO.Path]::GetFullPath($root).TrimEnd('\', '/')
    $f = [System.IO.Path]::GetFullPath($full)
    if ($f.Length -le ($r.Length + 1)) { return ($f -replace '\\', '/') }
    return ($f.Substring($r.Length + 1) -replace '\\', '/')
}

function AbsFrom([string]$root, [string]$rel) {
    return (Join-Path $root ($rel -replace '/', '\'))
}

function Write-TextNoBom([string]$path, [string]$text) {
    # Windows PowerShell 5.1's -Encoding utf8 writes a BOM, and a BOM breaks a
    # first-line match for every later grep (CROSS_MACHINE_RIG section 5e).
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($path), $text, $enc)
}

function Test-DevRepo() {
    return ((Test-Path -LiteralPath (Join-Path $repoRoot "src\plugin\KenshiCoop.vcxproj")) -and
            (Test-Path -LiteralPath (Join-Path $repoRoot ".git")))
}

function Get-RepoHeadSha() {
    try {
        $sha = (& git -C $repoRoot rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and $sha) { return "$sha".Trim() }
    } catch { }
    return ""
}

# ------------------------------------------------------- install location
function Find-KenshiInstalls() {
    $cands = New-Object System.Collections.Generic.List[string]
    $roots = @()
    if ($env:ProgramFiles)        { $roots += (Join-Path $env:ProgramFiles "Steam\steamapps\common\Kenshi") }
    if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\Kenshi") }
    foreach ($d in @([System.IO.DriveInfo]::GetDrives() | Where-Object { $_.IsReady })) {
        $roots += (Join-Path $d.Name "SteamLibrary\steamapps\common\Kenshi")
        $roots += (Join-Path $d.Name "Steam\steamapps\common\Kenshi")
    }
    foreach ($r in $roots) {
        if ((Test-Path -LiteralPath (Join-Path $r "kenshi_x64.exe")) -and
            (-not ($cands | Where-Object { (NormDir $_) -eq (NormDir $r) }))) {
            [void]$cands.Add($r)
        }
    }
    return @($cands)
}

function Resolve-KenshiDir([string]$given) {
    if ($given) {
        if (-not (Test-Path -LiteralPath $given)) {
            Refuse "there is no directory at '$given'. Point -KenshiDir at the folder that holds kenshi_x64.exe."
        }
        $abs = [System.IO.Path]::GetFullPath($given).TrimEnd('\', '/')
    } else {
        $found = Find-KenshiInstalls
        if ($found.Count -eq 0) {
            Refuse "no Kenshi installation was found automatically. Pass -KenshiDir with the folder that holds kenshi_x64.exe (for example: -KenshiDir `"C:\Program Files (x86)\Steam\steamapps\common\Kenshi`")."
        }
        if ($found.Count -gt 1) {
            Refuse ("more than one Kenshi installation was found, so this script will not guess which one you meant. Pass -KenshiDir with one of: " + ($found -join "; "))
        }
        $abs = [System.IO.Path]::GetFullPath($found[0]).TrimEnd('\', '/')
        Write-Host ("  auto-detected install: {0}" -f $abs)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $abs "kenshi_x64.exe"))) {
        Refuse "'$abs' is not a Kenshi installation: there is no kenshi_x64.exe in it."
    }
    if (Test-DevRepo) {
        foreach ($prot in $ProtectedInstalls) {
            if ((NormDir $abs) -eq (NormDir $prot)) {
                Refuse "this is the REAL Kenshi install ($prot) and you are running install_coop.ps1 from the development checkout, which may never write there. Use a clone (for example F:\KenshiCoop-Clone3)."
            }
        }
    }
    return $abs
}

function Assert-GameNotRunning() {
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq "kenshi_x64" -or $_.Name -eq "Kenshi_x64" })
    if ($running.Count -eq 0) { return }
    $ids = ($running | ForEach-Object { "$($_.Id)" }) -join ", "
    if ($isWhatIf) {
        # -WhatIf copies nothing, so a running game cannot break anything here;
        # say it would be refused rather than refusing to print the plan.
        Write-Host ("  warning: Kenshi is running (PID {0}). A real install would be refused until it is closed." -f $ids)
        return
    }
    Refuse "Kenshi is running (PID $ids). It holds KenshiCoop.dll open, so a copy would fail halfway. Close every Kenshi window and run this again."
}

function Resolve-OutDir([string]$given, [string]$tag) {
    if ($given) {
        if (-not [System.IO.Path]::IsPathRooted($given)) {
            Refuse "-OutDir must be an ABSOLUTE path (got '$given'). A relative path is swallowed silently and no record is written."
        }
        return ([System.IO.Path]::GetFullPath($given).TrimEnd('\', '/'))
    }
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss", $INV)
    if (Test-DevRepo) {
        return (Join-Path $repoRoot ("tools\test-runs\" + $tag + "_" + $stamp))
    }
    $base = $env:LOCALAPPDATA
    if (-not $base) { $base = $env:TEMP }
    return (Join-Path $base ("KenshiCoop\" + $tag + "_" + $stamp))
}

# ------------------------------------------------------------------ payload
function Resolve-Payload([string]$given) {
    $dirs = @()
    if ($given) {
        $dirs = @($given)
    } else {
        $dirs = @(
            (Join-Path $repoRoot "dist\mod-kit\KenshiCoop"),
            (Join-Path $scriptDir "KenshiCoop"),
            (Join-Path $repoRoot "dist\mods\KenshiCoop")
        )
    }
    foreach ($d in $dirs) {
        if (-not $d) { continue }
        if (-not (Test-Path -LiteralPath $d)) { continue }
        $ok = $true
        foreach ($n in $PayloadNames) { if (-not (Test-Path -LiteralPath (Join-Path $d $n))) { $ok = $false } }
        if ($ok) { return ([System.IO.Path]::GetFullPath($d).TrimEnd('\', '/')) }
    }
    # Repo fallback: the freshly built Release DLL plus the repo's mod files.
    if (-not $given) {
        $rel = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"
        $j   = Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"
        $m   = Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"
        if ((Test-Path -LiteralPath $rel) -and (Test-Path -LiteralPath $j) -and (Test-Path -LiteralPath $m)) {
            return "@split:$rel|$j|$m"
        }
    }
    if ($given) {
        $missing = @()
        foreach ($n in $PayloadNames) { if (-not (Test-Path -LiteralPath (Join-Path $given $n))) { $missing += $n } }
        Refuse ("-Source '$given' is not a KenshiCoop payload folder: it is missing " + ($missing -join ", ") + ".")
    }
    Refuse ("no KenshiCoop payload was found. Looked for " + ($PayloadNames -join ", ") + " in: " + ($dirs -join "; ") + ". Pass -Source with the folder that holds them.")
}

function Get-PayloadFiles([string]$resolved) {
    $out = [ordered]@{}
    if ($resolved.StartsWith("@split:")) {
        $parts = $resolved.Substring(7).Split("|")
        $out["KenshiCoop.dll"]  = $parts[0]
        $out["RE_Kenshi.json"]  = $parts[1]
        $out["KenshiCoop.mod"]  = $parts[2]
    } else {
        foreach ($n in $PayloadNames) { $out[$n] = (Join-Path $resolved $n) }
    }
    return $out
}

# ------------------------------------------------------------- prerequisites
function Get-KenshiVersionInfo([string]$dir) {
    $file = Join-Path $dir "currentVersion.txt"
    $res = [ordered]@{
        file      = (RelPath $dir $file)
        found     = $false
        raw       = ""
        parsed    = $false
        version   = ""
        supported = @($SupportedKenshiVersions)
        ok        = $false
    }
    if (-not (Test-Path -LiteralPath $file)) { return $res }
    $res.found = $true
    $raw = ""
    try { $raw = [System.IO.File]::ReadAllText([System.IO.Path]::GetFullPath($file)) } catch { $raw = "" }
    $res.raw = $raw.Trim()
    $m = [regex]::Match($res.raw, '(\d+\.\d+\.\d+)')
    if (-not $m.Success) { return $res }
    $res.parsed  = $true
    $res.version = $m.Groups[1].Value
    $res.ok      = ($SupportedKenshiVersions -contains $res.version)
    return $res
}

function Test-CoopPrerequisites {
    <#
      One function, one structured result, embedded verbatim in the manifest so
      what the installer checked stays recoverable from the install itself.
      Detection is BY FILE PRESENCE only (D-04).
    #>
    param(
        [string]$Dir,
        [string[]]$SearchPath
    )

    # --- 1. VC++ 2010 x64 runtime ------------------------------------------
    $dlls = @()
    $missing = @()
    foreach ($n in $CrtDllNames) {
        $at = ""
        foreach ($p in $SearchPath) {
            if (-not $p) { continue }
            if (Test-Path -LiteralPath (Join-Path $p $n)) { $at = $p; break }
        }
        if (-not $at) { $missing += $n }
        $dlls += [ordered]@{ name = $n; foundAt = $at }
    }
    $crt = [ordered]@{
        ok         = ($missing.Count -eq 0)
        required   = @($CrtDllNames)
        searched   = @($SearchPath)
        dlls       = @($dlls)
        missing    = @($missing)
        redistPath = (Join-Path $Dir "dependencies\vcredist_x64.exe")
        redistPresent = (Test-Path -LiteralPath (Join-Path $Dir "dependencies\vcredist_x64.exe"))
    }

    # --- 2. RE_Kenshi loader ------------------------------------------------
    $rekDll = Join-Path $Dir "RE_Kenshi.dll"
    $rek = [ordered]@{
        ok         = (Test-Path -LiteralPath $rekDll)
        loaderPath = (RelPath $Dir $rekDll)
        url        = $ReKenshiUrl
    }

    # --- 3. Ogre plugin registration (separate: it fails and is fixed
    #        differently from RE_Kenshi simply being absent) -----------------
    $cfg = Join-Path $Dir "Plugins_x64.cfg"
    $lineFound = $false
    $cfgFound  = (Test-Path -LiteralPath $cfg)
    if ($cfgFound) {
        $txt = [System.IO.File]::ReadAllText([System.IO.Path]::GetFullPath($cfg))
        $lineFound = [bool]([regex]::IsMatch($txt, '(?im)^\s*Plugin\s*=\s*RE_Kenshi\s*$'))
    }
    $ogre = [ordered]@{
        ok        = $lineFound
        cfgPath   = (RelPath $Dir $cfg)
        cfgFound  = $cfgFound
        lineFound = $lineFound
        repaired  = $false
    }

    # --- 4. Kenshi version --------------------------------------------------
    $ver = Get-KenshiVersionInfo $Dir

    return [ordered]@{
        checkedUtc    = (NowUtc)
        crt           = $crt
        reKenshi      = $rek
        ogrePlugin    = $ogre
        kenshiVersion = $ver
    }
}

function Show-Prerequisites($p) {
    Write-Host "  prerequisites:"
    $crtWhere = @()
    foreach ($d in $p.crt.dlls) { if ($d.foundAt) { $crtWhere += ($d.name + " <- " + $d.foundAt) } }
    if ($p.crt.ok) {
        Write-Host ("    [ok]   VC++ 2010 x64 runtime: " + ($crtWhere -join "; "))
    } else {
        Write-Host ("    [MISS] VC++ 2010 x64 runtime: missing " + ($p.crt.missing -join ", "))
    }
    Write-Host ("    " + $(if ($p.reKenshi.ok) { "[ok]  " } else { "[MISS]" }) + " RE_Kenshi loader: " + $p.reKenshi.loaderPath)
    Write-Host ("    " + $(if ($p.ogrePlugin.ok) { "[ok]  " } else { "[FIX] " }) + " Ogre plugin line '" + $OgrePluginLine + "' in " + $p.ogrePlugin.cfgPath)
    if ($p.kenshiVersion.found -and $p.kenshiVersion.parsed) {
        Write-Host ("    " + $(if ($p.kenshiVersion.ok) { "[ok]  " } else { "[MISS]" }) + " Kenshi version: " + $p.kenshiVersion.version + " (supported: " + ($SupportedKenshiVersions -join ", ") + ")")
    } elseif ($p.kenshiVersion.found) {
        Write-Host ("    [MISS] Kenshi version: currentVersion.txt is unreadable as a version")
    } else {
        Write-Host ("    [MISS] Kenshi version: currentVersion.txt is missing")
    }
}

function Assert-Prerequisites($p, [string]$Dir) {
    # Each refusal names ONE thing and ONE action, in plain sentences. Nothing
    # has been written at this point, so a refusal leaves no backup directory
    # and no mod directory behind.
    if (-not $p.crt.ok) {
        $redistHint = if ($p.crt.redistPresent) {
            "Run " + $p.crt.redistPath + " - Kenshi ships that installer in its own folder - then run this script again."
        } else {
            "Install the Microsoft Visual C++ 2010 x64 redistributable (Kenshi normally ships it as dependencies\vcredist_x64.exe), then run this script again."
        }
        Refuse ("the Microsoft Visual C++ 2010 x64 runtime is missing. KenshiCoop needs " +
                ($CrtDllNames -join ", ") + ", and " + ($p.crt.missing -join ", ") +
                " could not be found in: " + (($p.crt.searched | Where-Object { $_ }) -join "; ") + ". " +
                $redistHint +
                " Without this runtime the game does not start at all: it exits with error 0xc0000135 before any log file is created, so there is nothing to read afterwards.")
    }
    if (-not $p.reKenshi.ok) {
        Refuse ("RE_Kenshi is not installed in '" + $Dir + "': RE_Kenshi.dll is not there. " +
                $ReKenshiWhy + " Install RE_Kenshi from " + $ReKenshiUrl + " and run this script again.")
    }
    $v = $p.kenshiVersion
    if (-not $v.found) {
        Refuse ("currentVersion.txt is missing from '" + $Dir + "', so this script cannot tell which Kenshi version you have. " +
                "This is not an unsupported version - the file that states the version is not there. " +
                "Check that -KenshiDir points at the game folder itself (the one holding kenshi_x64.exe), or verify the game files through Steam.")
    }
    if (-not $v.parsed) {
        Refuse ("currentVersion.txt in '" + $Dir + "' does not contain a version number. It reads: '" + $v.raw + "'. " +
                "Verify the game files through Steam so the file is rewritten, then run this script again.")
    }
    if (-not $v.ok) {
        Refuse ("this Kenshi is version " + $v.version + ", and KenshiCoop supports " + ($SupportedKenshiVersions -join ", ") + ". " +
                "currentVersion.txt reads: '" + $v.raw + "'. " +
                "Update or roll back Kenshi to " + ($SupportedKenshiVersions -join ", ") + " and run this script again.")
    }
}

# ------------------------------------------------------- build stamp / version
function Read-BuildStampFromDll([string]$dllPath) {
    <#
      The plugin builds its banner with _snprintf(b, "KenshiCoop: build %s %s",
      __DATE__, __TIME__) (Plugin.cpp:3178), so the DATE and TIME literals sit
      in the binary near that format string. Anchor on the format string and
      look in a window around it, rather than scanning the whole DLL for
      anything date-shaped.

      Returns @{ stamp; source } - source is "dll" or "" when it could not be
      recovered. A VERSION.txt that can disagree with the DLL beside it is
      worse than none, so an unrecoverable stamp is reported as such.
    #>
    $res = @{ stamp = ""; source = "" }
    if (-not (Test-Path -LiteralPath $dllPath)) { return $res }
    try {
        $bytes = [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($dllPath))
    } catch { return $res }
    $latin = [System.Text.Encoding]::GetEncoding(28591)   # ISO-8859-1: byte-for-char, no .NET 5 Latin1Encoding on this runtime
    $text  = $latin.GetString($bytes)
    $anchor = $text.IndexOf("KenshiCoop: build ")
    if ($anchor -lt 0) { return $res }
    $from = [Math]::Max(0, $anchor - 1024)
    $len  = [Math]::Min($text.Length - $from, 2048)
    $window = $text.Substring($from, $len)
    $md = [regex]::Match($window, '(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9][0-9] 20[0-9][0-9]')
    $mt = [regex]::Match($window, '[0-2][0-9]:[0-5][0-9]:[0-5][0-9]')
    if ($md.Success -and $mt.Success) {
        $res.stamp  = ($md.Value + " " + $mt.Value)
        $res.source = "dll"
    }
    return $res
}

function Resolve-ProtocolVersion([string]$dllPath, [string]$payloadDir, [bool]$payloadFromRepo) {
    <#
      PROTOCOL_VERSION reaches the log through a "%u" format, so the NUMBER is
      an immediate operand, not a string, and cannot be scanned out of the DLL.
      Try in order and SAY which source answered, because a protocol number
      that silently came from the wrong place is how two players end up unable
      to explain a rejected handshake.
    #>
    $res = @{ version = ""; source = "" }

    if (Test-Path -LiteralPath $dllPath) {
        try {
            $bytes = [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($dllPath))
            $latin = [System.Text.Encoding]::GetEncoding(28591)   # ISO-8859-1: byte-for-char, no .NET 5 Latin1Encoding on this runtime
            $m = [regex]::Match($latin.GetString($bytes), 'proto=v(\d+)')
            if ($m.Success) { return @{ version = $m.Groups[1].Value; source = "dll" } }
        } catch { }
    }

    # Only sources that belong to THIS payload are consulted. A PROVENANCE.json
    # or a Wire.h that happens to sit in some unrelated repo beside the script
    # could state a protocol the installed DLL does not implement, and a
    # VERSION.txt that can disagree with the DLL beside it is worse than none.
    $provCands = @()
    if ($payloadDir) {
        $provCands += (Join-Path $payloadDir "PROVENANCE.json")
        $parent = Split-Path -Parent $payloadDir
        if ($parent) { $provCands += (Join-Path $parent "PROVENANCE.json") }
    }
    if ($payloadFromRepo) { $provCands += (Join-Path $repoRoot "dist\mod-kit\PROVENANCE.json") }
    foreach ($p in $provCands) {
        if (Test-Path -LiteralPath $p) {
            try {
                $j = Get-Content -Raw -LiteralPath $p | ConvertFrom-Json
                if ($j.protocolVersion) { return @{ version = "$($j.protocolVersion)"; source = (Split-Path -Leaf $p) } }
            } catch { }
        }
    }

    $wire = Join-Path $repoRoot "src\netproto\Wire.h"
    if ($payloadFromRepo -and (Test-Path -LiteralPath $wire)) {
        $m = [regex]::Match(([System.IO.File]::ReadAllText($wire)), 'PROTOCOL_VERSION\s*=\s*(\d+)')
        if ($m.Success) { return @{ version = $m.Groups[1].Value; source = "src/netproto/Wire.h" } }
    }
    return $res
}

function Build-VersionText {
    param(
        [string]$Stamp, [string]$StampSource,
        [string]$Proto, [string]$ProtoSource,
        [string]$DllSha, [long]$DllBytes,
        [string]$KenshiVersion, [string]$Dir
    )
    $stampText = if ($Stamp) { $Stamp } else { "unknown" }
    $stampFrom = if ($StampSource -eq "dll") { "read out of KenshiCoop.dll" } else { "NOT RECOVERABLE from KenshiCoop.dll" }
    $protoText = if ($Proto) { $Proto } else { "unknown" }
    $protoFrom = if ($ProtoSource) { "read from " + $ProtoSource } else { "NOT RECOVERABLE" }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("KenshiCoop - installed build")
    [void]$sb.AppendLine("===========================")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("You do not need to launch Kenshi to read this file.")
    [void]$sb.AppendLine("Every line says where its value came from.")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine(("build:             {0}    ({1})" -f $stampText, $stampFrom))
    [void]$sb.AppendLine(("protocol:          {0}    ({1})" -f $protoText, $protoFrom))
    [void]$sb.AppendLine(("dll sha256:        {0}" -f $DllSha))
    [void]$sb.AppendLine(("dll bytes:         {0}" -f $DllBytes.ToString($INV)))
    [void]$sb.AppendLine(("installer version: {0}" -f $InstallerVersion))
    [void]$sb.AppendLine(("installed (UTC):   {0}" -f (NowUtc)))
    [void]$sb.AppendLine(("kenshi version:    {0}" -f $KenshiVersion))
    [void]$sb.AppendLine(("install directory: {0}" -f $Dir))
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("Both players must run the same protocol version: a mismatch is rejected at")
    [void]$sb.AppendLine("handshake by design, with no backwards compatibility. If your friend cannot")
    [void]$sb.AppendLine("connect, compare the 'protocol' line in this file on both machines.")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("Written by scripts/install_coop.ps1. To remove the mod, run that script")
    [void]$sb.AppendLine("again with -Uninstall.")
    return $sb.ToString()
}

# ------------------------------------------------------------------- backups
function New-ContentAddressedBackup {
    <#
      D-03, the point of this script. Hash the original, copy it to
      <basename>.<first 8 hex of sha256>.bak, then RE-OPEN the backup and hash
      it, and require the two to be equal. The name carries the content, so a
      second install cannot destroy the first install's superseded copy.

      Returns @{ path; sha256; relPath } or refuses.
    #>
    param([string]$SourceFile, [string]$BackupDir, [string]$InstallRoot)

    $sha = Sha256 $SourceFile
    if (-not $sha) { Refuse "could not hash '$SourceFile' before backing it up, so this script will not overwrite it." }
    $base = Split-Path -Leaf $SourceFile
    $dest = Join-Path $BackupDir ($base + "." + $sha.Substring(0, 8) + ".bak")

    if (Test-Path -LiteralPath $dest) {
        # Same content-address: the byte-identical copy is already there. Prove
        # that rather than assume it - an existing file with the right name and
        # the wrong content would be worse than no backup at all.
        $existing = Sha256 $dest
        if ($existing -eq $sha) {
            return @{ path = $dest; sha256 = $sha; relPath = (RelPath $InstallRoot $dest) }
        }
        Refuse ("the backup file '" + $dest + "' already exists but holds different content (" + $existing + " instead of " + $sha + "). " +
                "That name encodes its content, so this is a corrupted backup directory. Move it aside and run this script again.")
    }

    Copy-Item -LiteralPath $SourceFile -Destination $dest -Force -Confirm:$false
    if (-not (Test-Path -LiteralPath $dest)) {
        Refuse "the backup copy of '$SourceFile' did not land at '$dest'. Nothing has been overwritten."
    }
    $back = Sha256 $dest
    if ($back -ne $sha) {
        Refuse ("the backup of '" + $SourceFile + "' does not match the file it was taken from (backup " + $back + ", original " + $sha + "). " +
                "Nothing has been overwritten.")
    }
    return @{ path = $dest; sha256 = $sha; relPath = (RelPath $InstallRoot $dest) }
}

# --------------------------------------------------------------- line editing
function Get-DominantEol([byte[]]$bytes) {
    $crlf = 0; $lf = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 10) {
            if ($i -gt 0 -and $bytes[$i - 1] -eq 13) { $crlf++ } else { $lf++ }
        }
    }
    if ($lf -gt $crlf) { return "`n" }
    return "`r`n"
}

function Add-LineToFileBytes([string]$path, [string]$line) {
    # Append by BYTES so every existing line, the file's encoding and its line
    # endings survive untouched; only the appended line is new.
    $bytes = @()
    if (Test-Path -LiteralPath $path) { $bytes = [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($path)) }
    $eol = Get-DominantEol $bytes
    $prefix = ""
    if ($bytes.Length -gt 0) {
        $last = $bytes[$bytes.Length - 1]
        if ($last -ne 10 -and $last -ne 13) { $prefix = $eol }
    }
    $append = [System.Text.Encoding]::ASCII.GetBytes($prefix + $line + $eol)
    $out = New-Object byte[] ($bytes.Length + $append.Length)
    if ($bytes.Length -gt 0) { [Array]::Copy($bytes, 0, $out, 0, $bytes.Length) }
    [Array]::Copy($append, 0, $out, $bytes.Length, $append.Length)
    [System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($path), $out)
}

function Test-LinePresent([string]$path, [string]$pattern) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $txt = [System.IO.File]::ReadAllText([System.IO.Path]::GetFullPath($path))
    return [bool]([regex]::IsMatch($txt, $pattern))
}

# =============================================================== INFO =========
function Invoke-Info([string]$Dir) {
    Write-Host "=== install_coop: -Info (reads only, writes nothing) ==="
    Write-Host ("  install: {0}" -f $Dir)
    $verFile = AbsFrom $Dir ($ModDirRel + "/VERSION.txt")
    if (Test-Path -LiteralPath $verFile) {
        Write-Host ""
        Write-Host ("--- {0} ---" -f (RelPath $Dir $verFile))
        Write-Host ([System.IO.File]::ReadAllText([System.IO.Path]::GetFullPath($verFile)))
        exit 0
    }
    $man = AbsFrom $Dir ($BackupDirRel + "/" + $ManifestName)
    if (Test-Path -LiteralPath $man) {
        Write-Host "  VERSION.txt is not there; falling back to the install manifest."
        $j = Get-Content -Raw -LiteralPath $man | ConvertFrom-Json
        Write-Host ""
        Write-Host ("  installed (UTC):   {0}" -f $j.installedUtc)
        Write-Host ("  installer version: {0}" -f $j.installerVersion)
        foreach ($f in $j.files) {
            if ($f.path -like "*KenshiCoop.dll") {
                Write-Host ("  dll sha256:        {0}" -f $f.sha256After)
            }
        }
        exit 0
    }
    Write-Host ""
    Write-Host ("KenshiCoop is not installed in '" + $Dir + "': there is no mods\KenshiCoop\VERSION.txt and no install manifest.")
    exit 3
}

# ============================================================ UNINSTALL =======
function Invoke-Uninstall([string]$Dir, [string]$Out) {
    Write-Host "=== install_coop: -Uninstall (manifest-driven, never inferred) ==="
    Write-Host ("  install: {0}" -f $Dir)

    $backupDir = AbsFrom $Dir $BackupDirRel
    $manPath   = Join-Path $backupDir $ManifestName
    if (-not (Test-Path -LiteralPath $manPath)) {
        Refuse ("there is no install manifest at '" + $manPath + "', so this script does not know what it put there and will not guess. " +
                "If KenshiCoop was copied in by hand, remove the folder '" + (AbsFrom $Dir $ModDirRel) + "' yourself; nothing else was changed by hand-copying. " +
                "If you moved the manifest, put it back and run this again.")
    }
    $man = $null
    try { $man = Get-Content -Raw -LiteralPath $manPath | ConvertFrom-Json } catch {
        Refuse ("the install manifest at '" + $manPath + "' is not readable JSON, so this script will not act on it. Nothing has been changed.")
    }
    if ([int]$man.schemaVersion -ne $SchemaVersion) {
        Refuse ("the install manifest at '" + $manPath + "' has schema version " + $man.schemaVersion +
                ", and this script understands version " + $SchemaVersion + ". Use the installer version that wrote it (" + $man.installerVersion + ").")
    }

    Assert-GameNotRunning

    Write-Host ("  manifest: {0} (written {1} by installer {2})" -f (RelPath $Dir $manPath), $man.installedUtc, $man.installerVersion)

    $actions  = @()
    $skipped  = @()
    $files    = @($man.files)

    # Reverse recorded order: the last thing written is the first thing undone.
    for ($i = $files.Count - 1; $i -ge 0; $i--) {
        $f    = $files[$i]
        $abs  = AbsFrom $Dir $f.path
        $cur  = Sha256 $abs
        $rec  = [ordered]@{ path = $f.path; action = $f.action; result = ""; sha256 = ""; note = "" }

        if ($f.action -eq "created") {
            if (-not $cur) {
                $rec.result = "already-absent"
                Write-Host ("  gone     {0} (nothing to remove)" -f $f.path)
            } elseif ($cur -eq $f.sha256After) {
                if (-not $isWhatIf) { Remove-Item -LiteralPath $abs -Force -Confirm:$false }
                $rec.result = "deleted"
                $rec.sha256 = $cur
                Write-Host ("  removed  {0}" -f $f.path)
            } elseif ($Force) {
                if (-not $isWhatIf) { Remove-Item -LiteralPath $abs -Force -Confirm:$false }
                $rec.result = "deleted-modified-forced"
                $rec.sha256 = $cur
                $rec.note   = "content differed from the installed file; removed because -Force was given"
                Write-Host ("  removed  {0} (MODIFIED since install - removed because -Force was given)" -f $f.path)
            } else {
                $rec.result = "left-modified"
                $rec.sha256 = $cur
                $rec.note   = "current content differs from the installed file; left in place"
                $skipped += $f.path
                Write-Host ("  LEFT     {0} - it has changed since it was installed, so it was NOT deleted." -f $f.path)
                Write-Host ("           installed {0}, now {1}. Delete it yourself, or re-run with -Force." -f $f.sha256After, $cur)
            }
        }
        elseif ($f.action -eq "replaced") {
            $bak = AbsFrom $Dir $f.backupPath
            if (-not (Test-Path -LiteralPath $bak)) {
                Refuse ("the backup '" + $f.backupPath + "' that '" + $f.path + "' must be restored from is missing. " +
                        "Nothing has been restored from this entry and nothing further will be touched. Put the backup back and run this again.")
            }
            $bakSha = Sha256 $bak
            if ($bakSha -ne $f.backupSha256) {
                Refuse ("the backup '" + $f.backupPath + "' no longer matches what was recorded for it (" + $bakSha +
                        " instead of " + $f.backupSha256 + "). Restoring it could put wrong content into '" + $f.path +
                        "', so nothing has been touched.")
            }
            if ($cur -and $cur -ne $f.sha256After -and -not $Force) {
                $rec.result = "left-modified"
                $rec.sha256 = $cur
                $rec.note   = "current content differs from the installed file; the backup was NOT restored over it"
                $skipped += $f.path
                Write-Host ("  LEFT     {0} - it has changed since it was installed, so the backup was NOT restored over it." -f $f.path)
                Write-Host ("           installed {0}, now {1}. Re-run with -Force to restore the backup anyway." -f $f.sha256After, $cur)
            } else {
                if (-not $isWhatIf) {
                    Copy-Item -LiteralPath $bak -Destination $abs -Force -Confirm:$false
                    $after = Sha256 $abs
                    if ($after -ne $f.sha256Before) {
                        Refuse ("the restore of '" + $f.path + "' did not produce the content recorded before the install (" +
                                $after + " instead of " + $f.sha256Before + "). The file is left as restored; do not run the game until this is resolved.")
                    }
                    $rec.sha256 = $after
                } else {
                    $rec.sha256 = $f.sha256Before
                }
                $rec.result = "restored"
                Write-Host ("  restored {0} <- {1}" -f $f.path, $f.backupPath)
                Write-Host ("           verified sha256 {0} == the content recorded before the install" -f $rec.sha256)
            }
        }
        else {
            Refuse ("the install manifest records an action this script does not understand ('" + $f.action + "') for '" + $f.path + "'. Nothing further has been touched.")
        }
        $actions += $rec
    }

    # ---- backups this manifest owns -----------------------------------------
    $backupsRemoved = @()
    if ($skipped.Count -gt 0) {
        Write-Host ""
        Write-Host ("  {0} file(s) were left in place, so the backups are KEPT: {1}" -f $skipped.Count, ($BackupDirRel))
    } elseif ($KeepBackups) {
        Write-Host ""
        Write-Host ("  -KeepBackups given: the backups and the manifest stay in {0}" -f $BackupDirRel)
    } else {
        # Every restore verified, so each backup is now redundant with the file
        # it was taken from. The manifest is copied into -OutDir first, so the
        # receipt outlives the directory.
        foreach ($f in $files) {
            if ($f.backupPath) {
                $bak = AbsFrom $Dir $f.backupPath
                if (Test-Path -LiteralPath $bak) {
                    if (-not $isWhatIf) { Remove-Item -LiteralPath $bak -Force -Confirm:$false }
                    $backupsRemoved += $f.backupPath
                }
            }
        }
        if (-not $isWhatIf) { Remove-Item -LiteralPath $manPath -Force -Confirm:$false }
        $backupsRemoved += (RelPath $Dir $manPath)
    }

    # ---- directories, most-nested first, only when empty ---------------------
    $dirsRemoved = @()
    foreach ($d in @($man.createdDirs)) {
        $abs = AbsFrom $Dir $d
        if (-not (Test-Path -LiteralPath $abs)) { continue }
        $left = @(Get-ChildItem -LiteralPath $abs -Force -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) {
            if (-not $isWhatIf) { Remove-Item -LiteralPath $abs -Force -Confirm:$false }
            $dirsRemoved += $d
            Write-Host ("  removed  {0}\ (empty)" -f $d)
        } else {
            Write-Host ("  kept     {0}\ - not empty ({1} item(s) that were not put there by this installer)" -f $d, $left.Count)
        }
    }

    # ---- the record ----------------------------------------------------------
    $record = [ordered]@{
        schemaVersion    = $SchemaVersion
        installerVersion = $InstallerVersion
        uninstalledUtc   = (NowUtc)
        kenshiDir        = $Dir
        platform         = "windows"
        manifestPath     = (RelPath $Dir $manPath)
        manifestWrittenUtc = "$($man.installedUtc)"
        whatIf           = $isWhatIf
        forced           = [bool]$Force
        actions          = @($actions)
        skipped          = @($skipped)
        backupsRemoved   = @($backupsRemoved)
        dirsRemoved      = @($dirsRemoved)
    }
    if (-not $isWhatIf) {
        New-Item -ItemType Directory -Force -Path $Out -Confirm:$false | Out-Null
        Write-TextNoBom (Join-Path $Out "UNINSTALL-RECORD.json") (($record | ConvertTo-Json -Depth 12))
        # Keep the manifest as evidence outside the install, since the copy in
        # the install has just been removed.
        Write-TextNoBom (Join-Path $Out $ManifestName) (($man | ConvertTo-Json -Depth 12))
    }

    Write-Host ""
    Write-Host ("  record: {0}\UNINSTALL-RECORD.json" -f $Out)
    if ($skipped.Count -gt 0) {
        Write-Host ("UNINSTALL: PARTIAL - {0} file(s) were changed after the install and were left in place." -f $skipped.Count)
        Write-Host "  This is deliberate: your edits are not deleted silently. Remove them yourself, or re-run with -Force."
        exit 0
    }
    Write-Host "UNINSTALL: COMPLETE - every recorded file was restored or removed, each one hash-verified."
    exit 0
}

# ============================================================== INSTALL =======
function Invoke-Install([string]$Dir, [string]$Out) {
    Write-Host "=== install_coop: install ==="
    Write-Host ("  mode:    {0}" -f $(if ($isWhatIf) { "WhatIf (prints the plan, changes nothing)" } else { "install" }))
    Write-Host ("  install: {0}" -f $Dir)
    Write-Host ("  guard:   {0}" -f $(if (Test-DevRepo) { "development checkout - the real Steam installs are refused by name" } else { "shipped copy" }))

    Assert-GameNotRunning

    # ---- payload ------------------------------------------------------------
    $payloadResolved = Resolve-Payload $Source
    $payload = Get-PayloadFiles $payloadResolved
    Write-Host ("  payload: {0}" -f $(if ($payloadResolved.StartsWith("@split:")) { "repo Release build + dist\mods\KenshiCoop" } else { $payloadResolved }))
    foreach ($k in $payload.Keys) {
        if (-not (Test-Path -LiteralPath $payload[$k])) { Refuse ("the payload file '" + $k + "' is missing (looked at '" + $payload[$k] + "').") }
        Write-Host ("    {0}  {1}" -f (Sha256 $payload[$k]).Substring(0, 8), $k)
    }

    # ---- prerequisites: BEFORE any write, so a refusal leaves nothing -------
    $searchPath = @()
    if ($CrtSearchPath -and $CrtSearchPath.Count -gt 0) {
        # Accept both an array and one ';'-separated string: under -File, which
        # is how every script in this project is invoked, only the latter can
        # carry more than one directory.
        foreach ($p in $CrtSearchPath) {
            foreach ($q in ("$p" -split ';')) {
                $q = $q.Trim()
                if ($q) { $searchPath += $q }
            }
        }
    } else {
        $searchPath = @((Join-Path $env:WINDIR "System32"), $Dir)
    }
    $prereq = Test-CoopPrerequisites -Dir $Dir -SearchPath $searchPath
    Show-Prerequisites $prereq
    Assert-Prerequisites $prereq $Dir

    # ---- an earlier install? ------------------------------------------------
    $backupDir   = AbsFrom $Dir $BackupDirRel
    $manPath     = Join-Path $backupDir $ManifestName
    $prevMan     = $null
    if (Test-Path -LiteralPath $manPath) {
        try { $prevMan = Get-Content -Raw -LiteralPath $manPath | ConvertFrom-Json } catch { $prevMan = $null }
        if (-not $Force) {
            Refuse ("KenshiCoop is already installed here (there is an install manifest at '" + $manPath + "', written " +
                    $(if ($prevMan) { "$($prevMan.installedUtc)" } else { "at an unknown time" }) + "). " +
                    "Run this script with -Uninstall first, or pass -Force to install over it.")
        }
        Write-Host ("  note:    an earlier install is recorded here; -Force given, so it will be installed over.")
    }

    # ---- build the write plan ----------------------------------------------
    $modDir  = AbsFrom $Dir $ModDirRel
    $plan    = @()   # ordered: mod payload, VERSION.txt, then the two cfg files

    foreach ($n in $PayloadNames) {
        $plan += [ordered]@{ rel = ($ModDirRel + "/" + $n); kind = "copy"; from = $payload[$n] }
    }
    $plan += [ordered]@{ rel = ($ModDirRel + "/VERSION.txt"); kind = "text"; from = "" }

    $cfgPath  = Join-Path $Dir "Plugins_x64.cfg"
    $needOgre = ($prereq.reKenshi.ok -and -not $prereq.ogrePlugin.ok -and $prereq.ogrePlugin.cfgFound)
    if ($needOgre) {
        $plan += [ordered]@{ rel = "Plugins_x64.cfg"; kind = "append"; line = $OgrePluginLine }
    } elseif ($prereq.reKenshi.ok -and -not $prereq.ogrePlugin.cfgFound) {
        Write-Host ("  note:    {0} does not exist; the Ogre plugin line cannot be repaired and is left alone." -f $prereq.ogrePlugin.cfgPath)
    }

    $modsCfg     = AbsFrom $Dir "data/mods.cfg"
    $modsCfgHas  = Test-LinePresent $modsCfg ('(?im)^\s*' + [regex]::Escape($ModListEntry) + '\s*$')
    if (-not $modsCfgHas) {
        if (Test-Path -LiteralPath $modsCfg) {
            $plan += [ordered]@{ rel = "data/mods.cfg"; kind = "append"; line = $ModListEntry }
        } elseif (Test-Path -LiteralPath (AbsFrom $Dir "data")) {
            $plan += [ordered]@{ rel = "data/mods.cfg"; kind = "append"; line = $ModListEntry }
        } else {
            Write-Host "  note:    there is no data\ folder, so the mod list entry cannot be written."
        }
    }

    # ---- directories this installer would create ----------------------------
    $dirsNeeded = @()
    foreach ($d in @((AbsFrom $Dir "mods"), $modDir, $backupDir)) {
        if (-not (Test-Path -LiteralPath $d)) { $dirsNeeded += $d }
    }

    # ---- print the plan -----------------------------------------------------
    Write-Host ""
    Write-Host "  plan:"
    foreach ($d in $dirsNeeded) { Write-Host ("    create dir   {0}" -f (RelPath $Dir $d)) }
    foreach ($p in $plan) {
        $abs = AbsFrom $Dir $p.rel
        if (Test-Path -LiteralPath $abs) {
            $s = Sha256 $abs
            Write-Host ("    back up      {0}  ->  {1}/{2}.{3}.bak" -f $p.rel, $BackupDirRel, (Split-Path -Leaf $abs), $s.Substring(0, 8))
            if ($p.kind -eq "append") {
                Write-Host ("    append line  {0}  <<  {1}" -f $p.rel, $p.line)
            } else {
                Write-Host ("    replace      {0}" -f $p.rel)
            }
        } else {
            Write-Host ("    create       {0}" -f $p.rel)
        }
    }
    if ($isWhatIf) {
        Write-Host ""
        Write-Host ("  record would be written to: {0}\INSTALL-RECORD.json" -f $Out)
        Write-Host "WHATIF: nothing above was done. No file was created, replaced or backed up."
        exit 0
    }

    # ---- create directories -------------------------------------------------
    foreach ($d in $dirsNeeded) {
        New-Item -ItemType Directory -Force -Path $d -Confirm:$false | Out-Null
        $script:Journal += @{ kind = "dir-created"; path = $d }
    }

    # ---- previous-install baseline -----------------------------------------
    # A re-install must not lose the state the FIRST install replaced: if it
    # did, an uninstall after two installs would restore install #1's file
    # rather than the player's original. Carry the earliest recorded baseline
    # forward for every path we see again.
    $inherited = @{}
    if ($prevMan) {
        foreach ($f in @($prevMan.files)) { $inherited[$f.path] = $f }
        $prevBak = New-ContentAddressedBackup -SourceFile $manPath -BackupDir $backupDir -InstallRoot $Dir
        Write-Host ("  superseded manifest kept as {0}" -f $prevBak.relPath)
    }

    # ---- execute ------------------------------------------------------------
    Write-Host ""
    $entries = @()
    $dllShaAfter = ""
    foreach ($p in $plan) {
        $abs    = AbsFrom $Dir $p.rel
        $exists = Test-Path -LiteralPath $abs
        $shaBefore   = ""
        $backupRel   = ""
        $backupSha   = ""
        $action      = if ($exists) { "replaced" } else { "created" }

        if ($exists) {
            $b = New-ContentAddressedBackup -SourceFile $abs -BackupDir $backupDir -InstallRoot $Dir
            $shaBefore = $b.sha256
            $backupRel = $b.relPath
            $backupSha = $b.sha256
            Write-Host ("  backed up {0}  ->  {1}  (verified {2})" -f $p.rel, $b.relPath, $b.sha256.Substring(0, 16))
            $script:Journal += @{ kind = "file-replaced"; path = $abs; backup = $b.path }
        } else {
            $script:Journal += @{ kind = "file-created"; path = $abs }
        }

        switch ($p.kind) {
            "copy" {
                Copy-Item -LiteralPath $p.from -Destination $abs -Force -Confirm:$false
            }
            "append" {
                Add-LineToFileBytes $abs $p.line
            }
            "text" {
                # VERSION.txt is written after the DLL is in place, so its build
                # stamp and hash describe the INSTALLED file, not the source.
                $instDll  = AbsFrom $Dir ($ModDirRel + "/KenshiCoop.dll")
                $stamp    = Read-BuildStampFromDll $instDll
                $srcDir   = if ($payloadResolved.StartsWith("@split:")) { "" } else { $payloadResolved }
                $fromRepo = ($payloadResolved.StartsWith("@split:")) -or
                            ((NormDir $srcDir).StartsWith((NormDir $repoRoot) + "\"))
                $proto    = Resolve-ProtocolVersion $instDll $srcDir $fromRepo
                $dllInfo  = Get-Item -LiteralPath $instDll
                $dllShaAfter = Sha256 $instDll
                $txt = Build-VersionText -Stamp $stamp.stamp -StampSource $stamp.source `
                                         -Proto $proto.version -ProtoSource $proto.source `
                                         -DllSha $dllShaAfter -DllBytes $dllInfo.Length `
                                         -KenshiVersion $prereq.kenshiVersion.version -Dir $Dir
                Write-TextNoBom $abs $txt
                Write-Host ("  build stamp: {0} ({1}); protocol {2} ({3})" -f `
                    $(if ($stamp.stamp) { $stamp.stamp } else { "unknown" }),
                    $(if ($stamp.source) { $stamp.source } else { "not recoverable" }),
                    $(if ($proto.version) { $proto.version } else { "unknown" }),
                    $(if ($proto.source) { $proto.source } else { "not recoverable" }))
            }
        }

        $shaAfter = Sha256 $abs
        if (-not $shaAfter) { Refuse ("'" + $p.rel + "' was not written.") }

        # Inherit the ORIGINAL baseline when this path was already recorded by
        # an earlier install (see above).
        if ($inherited.ContainsKey($p.rel)) {
            $prev = $inherited[$p.rel]
            $action    = "$($prev.action)"
            $shaBefore = "$($prev.sha256Before)"
            $backupRel = "$($prev.backupPath)"
            $backupSha = "$($prev.backupSha256)"
            Write-Host ("  baseline  {0}  kept from the first install ({1})" -f $p.rel, $action)
        }

        $entries += [ordered]@{
            path          = $p.rel
            action        = $action
            existedBefore = ($action -eq "replaced")
            sha256Before  = $shaBefore
            backupPath    = $backupRel
            backupSha256  = $backupSha
            sha256After   = $shaAfter
        }
        Write-Host ("  wrote     {0}  ({1})" -f $p.rel, $shaAfter.Substring(0, 16))
    }

    if ($needOgre) { $prereq.ogrePlugin.repaired = $true }

    # ---- createdDirs, most-nested first ------------------------------------
    $createdDirs = @()
    foreach ($d in $dirsNeeded) { $createdDirs += (RelPath $Dir $d) }
    if ($prevMan) {
        foreach ($d in @($prevMan.createdDirs)) { if ($createdDirs -notcontains $d) { $createdDirs += $d } }
    }
    # Most-nested first, so the uninstall's empty-only removal never meets a
    # parent before its child. Depth is the primary key; length breaks ties.
    $createdDirs = @($createdDirs |
        Sort-Object -Property `
            @{ Expression = { ($_ -split '/').Count }; Descending = $true }, `
            @{ Expression = { $_.Length }; Descending = $true })

    # ---- manifest: the canonical receipt and the uninstall's only input -----
    $manifest = [ordered]@{
        schemaVersion    = $SchemaVersion
        installerVersion = $InstallerVersion
        installedUtc     = (NowUtc)
        kenshiDir        = $Dir
        platform         = "windows"
        repoHeadSha      = (Get-RepoHeadSha)
        supersedes       = $(if ($prevMan) { "$($prevMan.installedUtc)" } else { "" })
        prerequisites    = $prereq
        files            = @($entries)
        createdDirs      = @($createdDirs)
    }
    Write-TextNoBom $manPath (($manifest | ConvertTo-Json -Depth 12))
    Write-Host ("  manifest  {0}" -f (RelPath $Dir $manPath))

    New-Item -ItemType Directory -Force -Path $Out -Confirm:$false | Out-Null
    Write-TextNoBom (Join-Path $Out "INSTALL-RECORD.json") (($manifest | ConvertTo-Json -Depth 12))

    $script:Journal = @()   # committed

    Write-Host ""
    Write-Host ("INSTALL: COMPLETE - {0} file(s) written, every replaced file backed up and hash-verified." -f $entries.Count)
    Write-Host ("  read the build back with: -KenshiDir `"{0}`" -Info" -f $Dir)
    Write-Host ("  remove it with:           -KenshiDir `"{0}`" -Uninstall" -f $Dir)
    Write-Host ("  record: {0}\INSTALL-RECORD.json" -f $Out)
    exit 0
}

# ================================================================ dispatch ====
if ($Uninstall -and $Info) { Refuse "-Uninstall and -Info do different things; pass one of them, not both." }

$dir = Resolve-KenshiDir $KenshiDir

if ($Info) { Invoke-Info $dir }

$tag = if ($Uninstall) { "uninstall" } else { "install" }
$out = Resolve-OutDir $OutDir $tag

if ($Uninstall) { Invoke-Uninstall $dir $out }
Invoke-Install $dir $out
