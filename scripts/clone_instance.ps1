<#
.SYNOPSIS
  Provision ONE independent Kenshi clone: hardlinked (or copied) static
  assets, an isolated save/ directory, RE_Kenshi + the KenshiCoop mod-kit
  deployed. Generalizes scripts/setup_join_install.cmd's proven 2-instance
  recipe to an arbitrary -DestDir so plan 02 can call it in a loop.

.DESCRIPTION
  (1) Mirrors the read-only static asset tree from -SourceDir into -DestDir:
      hardlinked file-by-file in "hardlink" mode (same volume only), or
      robocopy-mirrored in "copy" mode. The per-instance-writable set is
      EXCLUDED from this mirror and always handled as a real, independent
      copy: the save/ directory and settings.cfg/controls.cfg/kenshi.cfg/
      the known log filenames (setup_join_install.cmd's own exclusion set).
  (2) Seeds an independent save/ into -DestDir on first run only (real copy).
  (3) Injects "User save location=1" into -DestDir\settings.cfg if absent,
      so this clone reads/writes its OWN save/ (never %LOCALAPPDATA%).
  (4) Installs RE_Kenshi (downloads the GitHub release zip into
      scripts/vendor/ if not already cached there) into -DestDir's root -
      the loader DLL(s) must sit in the install root (kit_preflight.ps1).
  (5) Deploys the mod: KenshiCoop.mod/RE_Kenshi.json/coop_config.json come
      from dist\mod-kit\KenshiCoop\ (identical between build configs), but
      KenshiCoop.dll itself is taken from src\plugin\x64\<-Config>\ (default
      "Harness" - the scenario-runner build scripts\run_test.ps1 and this
      whole test rig require KENSHICOOP_TEST_SECONDS/KENSHICOOP_SCENARIO to
      do anything at all), falling back to the mod-kit's own DLL only if that
      build config hasn't been compiled. A real Task 3 run first deployed via
      the mod-kit's DLL alone and it turned out to be a stale build (older
      hash than src\plugin\x64\Harness\KenshiCoop.dll, missing the scenario
      harness entirely) - deploying the current Harness build here closes
      that gap for future clone provisioning. Pass -Config Release to opt
      into the shipped player build instead when that is genuinely what a
      caller wants to test.

  GUARD (hard, threat T-04-01): refuses to run if -DestDir equals, is a
  parent of, or resolves inside -SourceDir or the well-known primary
  install path - this script only ever reads/links FROM the source, never
  writes into or deletes from it.

  Idempotent and re-runnable: existing clone files (hardlinked or copied),
  the clone's own save/, and its settings.cfg are never re-clobbered.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\clone_instance.ps1 `
      -SourceDir "F:\SteamLibrary\steamapps\common\Kenshi" -DestDir "F:\KenshiCoop-Clone1"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$DestDir,
    [ValidateSet("hardlink", "copy")]
    [string]$Mode = "hardlink",
    [switch]$SkipReKenshi,
    [switch]$SkipModKit,
    # Which compiled KenshiCoop.dll to deploy (Phase 1 build separation).
    # "Harness" (default) is the test-runner build scripts\run_test.ps1
    # requires (KENSHICOOP_TEST_SECONDS/KENSHICOOP_SCENARIO self-exit +
    # scenario markers); "Release" is the shipped player build.
    [ValidateSet("Harness", "Release", "Debug")]
    [string]$Config = "Harness",
    [string]$RepoRoot = "",
    [string]$ReKenshiVersion = "0.3.5",
    # Override: path to an already-downloaded RE_Kenshi zip (per user_setup,
    # if the automated GitHub download is blocked).
    [string]$ReKenshiZip = ""
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot is normally reliable for -File invocation, but resolve with a
# fallback chain rather than fail outright if the calling shell/host leaves it
# empty (observed under some MSYS/git-bash -> powershell -File invocations).
if ([string]::IsNullOrEmpty($RepoRoot)) {
    if (-not [string]::IsNullOrEmpty($PSScriptRoot)) {
        $RepoRoot = Split-Path -Parent $PSScriptRoot
    } elseif ($MyInvocation.MyCommand.Path) {
        $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    } else {
        $RepoRoot = Split-Path -Parent (Get-Location).Path
    }
}

# Per-instance-writable set: never hardlinked, never mirrored over an
# existing clone copy. Starts from the exact exclusion set
# scripts/setup_join_install.cmd already proved for the 2-instance rig, PLUS
# Plugins_x64.cfg - which setup_join_install.cmd never needed to touch (it
# never installs RE_Kenshi), but this script DOES edit per-instance to
# register "Plugin=RE_Kenshi" (see Step 4). A real 2026-09-01 run of this
# script hardlinked Plugins_x64.cfg BEFORE this fix, then edited it in place,
# writing the edit into the SHARED inode and corrupting the primary
# install's copy too (Pitfall 2, realized) - never repeat that mistake by
# hardlinking any file this script itself later writes into.
$WritableFileNames = @(
    "settings.cfg", "controls.cfg", "kenshi.cfg", "Plugins_x64.cfg",
    "kenshi.log", "kenshi_info.log", "Havok.log", "FileIOLog.txt", "RE_Kenshi_log.txt"
)
$WritableDirNames = @("save")

# Newline-safe config-line append: Add-Content concatenates onto the last
# line with no separator if the file doesn't already end in a newline
# (exactly how the Plugins_x64.cfg corruption above happened - the source
# file has no trailing newline). Always guarantees the new line starts on
# its own line.
function Add-ConfigLine {
    param([string]$Path, [string]$Line)
    $existing = ""
    if (Test-Path -LiteralPath $Path) { $existing = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue }
    if ([string]::IsNullOrEmpty($existing)) {
        Set-Content -LiteralPath $Path -Value $Line
    } elseif ($existing.EndsWith("`n") -or $existing.EndsWith("`r")) {
        Add-Content -LiteralPath $Path -Value $Line
    } else {
        Set-Content -LiteralPath $Path -Value ($existing + "`r`n" + $Line) -NoNewline
    }
}

# ---- Guard: never target the primary install (T-04-01) ---------------------
function Assert-SafeDest {
    param([string]$SourceDir, [string]$DestDir)
    $s = [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\')
    $d = [System.IO.Path]::GetFullPath($DestDir).TrimEnd('\')
    if ($d -ieq $s -or
        $d.StartsWith("$s\", [System.StringComparison]::OrdinalIgnoreCase) -or
        $s.StartsWith("$d\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "REFUSING: -DestDir '$DestDir' equals or overlaps -SourceDir '$SourceDir'. clone_instance.ps1 must never write into or delete from the primary install."
    }
    $primary = "F:\SteamLibrary\steamapps\common\Kenshi"
    $p = [System.IO.Path]::GetFullPath($primary).TrimEnd('\')
    if ($d -ieq $p -or $d.StartsWith("$p\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "REFUSING: -DestDir '$DestDir' resolves inside the primary Kenshi install '$primary'."
    }
}

if (-not (Test-Path (Join-Path $SourceDir "kenshi_x64.exe"))) {
    throw "SourceDir '$SourceDir' does not look like a Kenshi install (no kenshi_x64.exe)."
}
Assert-SafeDest -SourceDir $SourceDir -DestDir $DestDir

New-Item -ItemType Directory -Path $DestDir -Force | Out-Null

# NTFS hardlinks cannot cross volumes; fall back to copy rather than crash
# uninformatively if -DestDir lands on a different drive than -SourceDir.
$srcVol = Split-Path -Path (Resolve-Path $SourceDir) -Qualifier
$dstVol = Split-Path -Path (Resolve-Path $DestDir) -Qualifier
if ($Mode -eq "hardlink" -and $srcVol -ine $dstVol) {
    Write-Host "  -Mode hardlink requested but -DestDir volume ($dstVol) differs from -SourceDir volume ($srcVol) - NTFS hardlinks cannot cross volumes; falling back to 'copy'."
    $Mode = "copy"
}

# ---- Step 1: mirror the read-only static asset tree ------------------------
Write-Host "=== Step 1: mirroring static assets ($Mode mode) from '$SourceDir' to '$DestDir' ==="
if ($Mode -eq "hardlink") {
    $sourceRoot = (Resolve-Path $SourceDir).Path.TrimEnd('\')
    $files = Get-ChildItem -Path $sourceRoot -Recurse -File -Force
    $linked = 0
    $skippedExisting = 0
    foreach ($f in $files) {
        if ($WritableFileNames -contains $f.Name) { continue }
        $rel = $f.FullName.Substring($sourceRoot.Length + 1)
        $topDir = ($rel -split '\\')[0]
        if ($WritableDirNames -contains $topDir) { continue }

        $destPath = Join-Path $DestDir $rel
        $destDirPath = Split-Path -Path $destPath -Parent
        if (-not (Test-Path -LiteralPath $destDirPath)) { New-Item -ItemType Directory -Path $destDirPath -Force | Out-Null }
        if (Test-Path -LiteralPath $destPath) { $skippedExisting++; continue }
        # Use cmd's mklink /H, not New-Item -Target: PowerShell's FileSystem
        # provider treats '[' ']' in -Target as wildcard glob characters (no
        # -LiteralTarget escape hatch exists), and Kenshi's own asset tree has
        # real filenames containing brackets (e.g. "..._[HEARTH]_NML.dds"),
        # which New-Item -Target fails to resolve as "path does not exist".
        $mklinkOut = & cmd /c "mklink /H `"$destPath`" `"$($f.FullName)`"" 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "mklink /H failed for '$destPath' <- '$($f.FullName)': $mklinkOut"
        }
        $linked++
    }
    Write-Host "  hardlinked $linked static file(s) ($skippedExisting already present, left unchanged)"
} else {
    $xfArgs = @("/XF") + $WritableFileNames
    $roboArgs = @($SourceDir, $DestDir, "/E", "/XO", "/R:1", "/W:1", "/NFL", "/NDL", "/NP", "/NJH", "/NJS",
                  "/XD", (Join-Path $SourceDir "save")) + $xfArgs
    & robocopy @roboArgs | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ge 8) { throw "robocopy static mirror failed with code $rc" }
    Write-Host "  robocopy mirror complete (exit code $rc)"
}

# ---- Step 2: seed an independent save/ (first run only) --------------------
$destSave = Join-Path $DestDir "save"
if (-not (Test-Path $destSave)) {
    Write-Host "=== Step 2: seeding independent save/ ==="
    $srcSave = Join-Path $SourceDir "save"
    if (Test-Path $srcSave) {
        & robocopy $srcSave $destSave /E /R:1 /W:1 /NFL /NDL /NP /NJH /NJS | Out-Null
        $rc = $LASTEXITCODE
        if ($rc -ge 8) { throw "robocopy save seed failed with code $rc" }
    } else {
        New-Item -ItemType Directory -Path $destSave -Force | Out-Null
    }
} else {
    Write-Host "=== Step 2: save/ already exists at '$destSave' - preserving this clone's own state (idempotent) ==="
}

# ---- Step 3: inject "User save location=1" ---------------------------------
Write-Host "=== Step 3: ensuring save-location isolation ==="
$destCfg = Join-Path $DestDir "settings.cfg"
if (-not (Test-Path -LiteralPath $destCfg)) { New-Item -ItemType File -Path $destCfg -Force | Out-Null }
$cfgText = Get-Content -LiteralPath $destCfg -Raw -ErrorAction SilentlyContinue
if (-not $cfgText -or $cfgText -notmatch '(?im)^\s*User save location\s*=') {
    Add-ConfigLine -Path $destCfg -Line "User save location=1"
    Write-Host "  added 'User save location=1' to '$destCfg'"
} else {
    Write-Host "  '$destCfg' already declares a save location - left unchanged"
}

# ---- Step 4: install RE_Kenshi ----------------------------------------------
if (-not $SkipReKenshi) {
    Write-Host "=== Step 4: installing RE_Kenshi $ReKenshiVersion ==="
    # Plugins_x64.cfg is in the writable set (never hardlinked - see the
    # comment on $WritableFileNames above), so Step 1 never populated it.
    # Seed it as a REAL, independent copy from the source on first run,
    # exactly once, before registering RE_Kenshi in this clone's own copy.
    $destPluginsCfg = Join-Path $DestDir "Plugins_x64.cfg"
    if (-not (Test-Path -LiteralPath $destPluginsCfg)) {
        $srcPluginsCfg = Join-Path $SourceDir "Plugins_x64.cfg"
        if (Test-Path -LiteralPath $srcPluginsCfg) {
            Copy-Item -LiteralPath $srcPluginsCfg -Destination $destPluginsCfg -Force
        }
    }
    $alreadyInstalled = (Test-Path (Join-Path $DestDir "RE_Kenshi.dll")) -or
                        (Test-Path (Join-Path $DestDir "dinput8.dll"))
    if ($alreadyInstalled) {
        Write-Host "  RE_Kenshi already present in '$DestDir' - skipping (idempotent)"
    } else {
        $vendorDir = Join-Path $RepoRoot "scripts\vendor"
        # NOTE: this must be the "loose" release asset (RE_Kenshi_vX.Y.Z_loose.zip),
        # not the plain release zip (which only contains a GUI installer .exe -
        # unusable headlessly). The GitHub release page names both; the loose
        # asset's own zip layout nests the actual runtime payload under an
        # "install/" folder (RE_Kenshi.dll + its native deps + a RE_Kenshi/
        # data subfolder) - that is what a real install places in the Kenshi
        # root, and what we replicate here without running the installer GUI.
        $zipPath = if ($ReKenshiZip -ne "") { $ReKenshiZip } else { Join-Path $vendorDir "RE_Kenshi-$ReKenshiVersion.zip" }
        if (-not (Test-Path $zipPath)) {
            New-Item -ItemType Directory -Path $vendorDir -Force | Out-Null
            $url = "https://github.com/BFrizzleFoShizzle/RE_Kenshi/releases/download/v$ReKenshiVersion/RE_Kenshi_v${ReKenshiVersion}_loose.zip"
            Write-Host "  downloading RE_Kenshi (loose) from $url ..."
            Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing
        }
        $extractDir = Join-Path $vendorDir "RE_Kenshi-$ReKenshiVersion-extracted"
        if (-not (Test-Path $extractDir)) {
            Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force
        }
        $installSrc = Join-Path $extractDir "install"
        if (-not (Test-Path $installSrc)) {
            throw "RE_Kenshi zip at '$zipPath' has no 'install\' folder after extraction - expected the '_loose.zip' release asset layout (RE_Kenshi.dll + native deps + RE_Kenshi\ data under install\)."
        }
        Copy-Item -Path (Join-Path $installSrc "*") -Destination $DestDir -Recurse -Force
        $rekDll = Join-Path $DestDir "RE_Kenshi.dll"
        if (-not (Test-Path $rekDll)) {
            throw "RE_Kenshi install completed but '$rekDll' is missing - unexpected release layout."
        }
        Write-Host "  installed RE_Kenshi.dll + native deps + RE_Kenshi\ data into '$DestDir'"
    }

    # Copying RE_Kenshi.dll alone does NOT make Kenshi's Ogre engine load it:
    # RE_Kenshi is a proper Ogre plugin, loaded only if Plugins_x64.cfg lists
    # it ("Plugin=RE_Kenshi", matching the DLL's own filename minus extension,
    # resolved relative to PluginFolder=.\). The real GUI installer adds this
    # line; replicated here so a headless clone actually loads the plugin
    # (confirmed via a real Task 3 run: without this line, kenshi_x64.exe
    # boots to the vanilla title screen with no RE_Kenshi_log.txt at all).
    $pluginsCfg = Join-Path $DestDir "Plugins_x64.cfg"
    if (Test-Path -LiteralPath $pluginsCfg) {
        $pluginsText = Get-Content -LiteralPath $pluginsCfg -Raw
        if ($pluginsText -notmatch '(?im)^\s*Plugin\s*=\s*RE_Kenshi\s*$') {
            Add-ConfigLine -Path $pluginsCfg -Line "Plugin=RE_Kenshi"
            Write-Host "  added 'Plugin=RE_Kenshi' to '$pluginsCfg'"
        } else {
            Write-Host "  '$pluginsCfg' already registers RE_Kenshi - left unchanged"
        }
    } else {
        Write-Warning "  '$pluginsCfg' not found - cannot register RE_Kenshi as an Ogre plugin; the game will run vanilla."
    }
}

# ---- Step 5: deploy the KenshiCoop mod --------------------------------------
if (-not $SkipModKit) {
    Write-Host "=== Step 5: deploying KenshiCoop mod (Config=$Config) ==="
    # KenshiCoop.mod / RE_Kenshi.json / coop_config.json are build-config
    # agnostic - the mod-kit's copies are fine for any -Config. Only
    # KenshiCoop.dll itself differs per build (Phase 1 build separation:
    # Harness defines KENSHICOOP_HARNESS and includes the scenario runner
    # scripts\run_test.ps1 drives; Release is the shipped player build with
    # no scenario harness at all).
    $kitSrc = Join-Path $RepoRoot "dist\mod-kit\KenshiCoop"
    if (-not (Test-Path $kitSrc)) {
        throw "Mod-kit not found at '$kitSrc'. Build it first (scripts\make_mod_kit.ps1 / the Harness build)."
    }
    $kitDest = Join-Path $DestDir "mods\KenshiCoop"
    New-Item -ItemType Directory -Path $kitDest -Force | Out-Null
    Copy-Item -Path (Join-Path $kitSrc "*") -Destination $kitDest -Recurse -Force

    $configDll = Join-Path $RepoRoot "src\plugin\x64\$Config\KenshiCoop.dll"
    if (Test-Path $configDll) {
        Copy-Item -Path $configDll -Destination (Join-Path $kitDest "KenshiCoop.dll") -Force
        Write-Host "  deployed mod-kit -> '$kitDest' (KenshiCoop.dll from $Config build: '$configDll')"
    } else {
        Write-Warning "  '$configDll' not found - deployed the mod-kit's own KenshiCoop.dll instead (build it first: scripts\build_plugin.cmd $Config)."
        Write-Host "  deployed mod-kit -> '$kitDest' (KenshiCoop.dll from mod-kit, NOT $Config)"
    }
}

Write-Host ""
Write-Host "Clone ready at '$DestDir' (mode=$Mode)."
