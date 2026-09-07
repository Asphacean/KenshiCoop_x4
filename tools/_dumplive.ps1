<#
.SYNOPSIS
  Capture state from a LIVE (running, crashed-and-wedged, or hung) Kenshi
  process from OUTSIDE it: a minidump, or a raw byte window of its loaded code.

.DESCRIPTION
  This is the tool src\plugin\core\CrashDump.cpp's header points at. The reason
  it has to live outside the process is measured, not assumed - all three
  in-process dump shapes were tried on 2026-08-03 and all three failed:
  SetUnhandledExceptionFilter never runs (Kenshi unwinds through the CRT before
  its own crash dialog), MiniDumpWriteDump on the faulting thread re-faults
  inside dbghelp, and calling it from a worker while the faulting thread waits
  never returns. Kenshi's own dumper asks for a FULL memory image and, with a
  second Kenshi resident, outran the page file and locked the machine. Dumping
  from a separate process cannot deadlock the target and cannot ask it to
  allocate.

  TWO MODES, and the cheap one is usually the one you want:

  -Bytes  Read a window of the target's memory with ReadProcessMemory and print
          it as hex. This exists because of what blocked the 2026-09-07 crash:
          the faulting RVA was MID-INSTRUCTION in kenshi_x64.exe on disk under
          every 64K-aligned base tried, i.e. the LOADED image is not the file on
          disk (RE_Kenshi rewrites it - it ships MinHook and courgette), so a
          module+RVA from a crash log cannot be disassembled from the install.
          The loaded bytes are the missing input, and this fetches them in one
          call with no dump file and no dump parser in between.

  (default) Write a minidump. -Kind picks how much:
            code  Normal + code sections of every loaded module + thread info +
                  unloaded modules + memory indirectly referenced by registers.
                  The default, because the code sections are what makes a crash
                  log's module+RVA disassemblable after the fact, and the
                  indirectly-referenced memory is what lets you look at the
                  object a bad pointer pointed at. Measured at ~130 MB for a
                  PowerShell process; expect more from Kenshi. The size is
                  reported on completion.
            mini  Stacks and the module list only. Smallest; enough to attribute
                  frames, not enough to decode them.
            full  Whole address space. This is the request that locked the
                  machine on 2026-08-03 with two Kenshi instances resident -
                  refuses to run without -Force for that reason.

  WEDGED-ON-THE-CRASH-DIALOG CAVEAT: by the time Kenshi shows "Kenshi has
  crashed" it has already UNWOUND, so the frames that faulted are below rsp and
  no dump taken then can show them. That is what the in-process VEH tracer's
  log lines are for; a dump taken at the dialog is still worth having for the
  loaded code bytes and the module map, which do not unwind.

  Requires 64-bit PowerShell (dbghelp and the target must agree on bitness) and
  the same user account as the target, or elevation.

.EXAMPLE
  # What faulted? Fetch the loaded bytes around a rip from a [crash] log line.
  powershell -ExecutionPolicy Bypass -File tools\_dumplive.ps1 `
      -Bytes "Kenshi_x64.exe+0x6ea900:0x200"

.EXAMPLE
  # Four clones running; dump the one that is wedged.
  powershell -ExecutionPolicy Bypass -File tools\_dumplive.ps1 -List
  powershell -ExecutionPolicy Bypass -File tools\_dumplive.ps1 -ProcessId 5200

.EXAMPLE
  # Every live Kenshi at once, before anybody clicks the crash dialog away.
  powershell -ExecutionPolicy Bypass -File tools\_dumplive.ps1 -All
#>
param(
    # Target. Explicit pid wins; otherwise every process named -Name is a
    # candidate, and with more than one you must pick (or pass -All).
    [int]$ProcessId = 0,
    [string]$Name = "Kenshi_x64",
    [switch]$All,
    [switch]$List,

    # Minidump mode.
    [ValidateSet('code', 'mini', 'full')]
    [string]$Kind = 'code',
    [string]$OutDir = $PSScriptRoot,
    [switch]$Force,

    # Byte mode: "<module>+0x<rva>:0x<len>" or "0x<absolute>:0x<len>".
    # Length is optional and defaults to 0x100.
    [string]$Bytes = ""
)

$ErrorActionPreference = "Stop"

if (-not [Environment]::Is64BitProcess) {
    throw ("This must run in 64-bit PowerShell: dbghelp and ReadProcessMemory " +
           "cannot cross bitness, and Kenshi_x64 is 64-bit. Use " +
           "%SystemRoot%\system32\WindowsPowerShell\v1.0\powershell.exe.")
}

if (-not ('LiveDump' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class LiveDump {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf,
                                              IntPtr size, out IntPtr read);
  [DllImport("dbghelp.dll", SetLastError=true)]
  public static extern bool MiniDumpWriteDump(IntPtr h, uint pid, IntPtr file,
                                              uint type, IntPtr ex, IntPtr user,
                                              IntPtr cb);
  // PROCESS_QUERY_INFORMATION | PROCESS_VM_READ is all MiniDumpWriteDump and
  // ReadProcessMemory need. Asking for less than ALL_ACCESS is the difference
  // between working and not on a target we did not start.
  public const uint RIGHTS = 0x0400 | 0x0010;
}
"@
}

# ---------------------------------------------------------------------------
# Target selection
# ---------------------------------------------------------------------------
function Get-Candidates {
    $procs = @(Get-Process -Name $Name -ErrorAction SilentlyContinue)
    # kenshi_x64.exe (the launcher) relaunches the real game as a separate
    # process with the same base name, so both show up here; the real game is
    # the one with a main window and the bigger working set. Reported, not
    # guessed at - the caller sees both rows and can pick.
    return $procs | Sort-Object Id
}

$cands = Get-Candidates

if ($List) {
    if (-not $cands.Count) { Write-Host "no process named '$Name' is running"; return }
    Write-Host ("== live '{0}' processes ==" -f $Name)
    $cands | ForEach-Object {
        $win = if ($_.MainWindowHandle -ne 0) { $_.MainWindowTitle } else { "<no window>" }
        "{0,-8} ws={1,7:N0} MB  started={2:HH:mm:ss}  {3}" -f `
            $_.Id, ($_.WorkingSet64 / 1MB), $_.StartTime, $win
    } | Write-Host
    return
}

$targets = @()
if ($ProcessId -gt 0) {
    $targets = @(Get-Process -Id $ProcessId)   # throws with a clear message if gone
} elseif ($All) {
    $targets = @($cands)
} elseif ($cands.Count -eq 1) {
    $targets = @($cands[0])
} elseif ($cands.Count -eq 0) {
    throw "no process named '$Name' is running (pass -ProcessId, or -Name)."
} else {
    throw (("{0} processes named '{1}' are running - the local rig runs up to " +
            "four. Pass -ProcessId <id> (see -List), or -All to dump every one.") `
           -f $cands.Count, $Name)
}

# ---------------------------------------------------------------------------
# Byte mode
# ---------------------------------------------------------------------------
function Resolve-Spec($proc, [string]$spec) {
    # "<module>+0x<rva>[:0x<len>]" or "0x<abs>[:0x<len>]"
    $len = 0x100
    $body = $spec
    if ($spec -match '^(.*):\s*(0x[0-9a-fA-F]+|\d+)\s*$') {
        $body = $Matches[1]
        $len = if ($Matches[2] -like '0x*') { [Convert]::ToInt32($Matches[2].Substring(2), 16) }
               else { [int]$Matches[2] }
    }
    $body = $body.Trim()

    if ($body -match '^(.+?)\s*\+\s*(0x[0-9a-fA-F]+|\d+)$') {
        $modName = $Matches[1].Trim()
        $rva = if ($Matches[2] -like '0x*') { [Convert]::ToInt64($Matches[2].Substring(2), 16) }
               else { [int64]$Matches[2] }
        $mod = $null
        try {
            # Refresh first: a Process object snapshots its module list, and on a
            # target that has only just started that snapshot can be short by
            # everything except the exe itself.
            $proc.Refresh()
            $mod = @($proc.Modules | Where-Object { $_.ModuleName -ieq $modName })[0]
        } catch {
            throw (("cannot enumerate modules of pid {0}: {1}. Same-bitness and " +
                    "same-user (or elevated) PowerShell is required.") -f $proc.Id, $_.Exception.Message)
        }
        if (-not $mod) {
            $have = (@($proc.Modules | ForEach-Object { $_.ModuleName }) -join ', ')
            throw "module '$modName' is not loaded in pid $($proc.Id). Loaded: $have"
        }
        return [pscustomobject]@{
            Abs = [int64]$mod.BaseAddress + $rva
            Module = $mod.ModuleName
            Base = [int64]$mod.BaseAddress
            Rva = $rva
            Len = $len
        }
    }

    if ($body -match '^(0x)?([0-9a-fA-F]+)$') {
        $abs = [Convert]::ToInt64($Matches[2], 16)
        # Attribute it back to a module so the output carries an RVA too - an
        # absolute address is worthless in a later triage session (ASLR).
        $mod = $null
        try {
            $mod = @($proc.Modules | Where-Object {
                $abs -ge [int64]$_.BaseAddress -and
                $abs -lt ([int64]$_.BaseAddress + $_.ModuleMemorySize)
            })[0]
        } catch { }
        $mName = "<unmapped>"; $mBase = [int64]0; $mRva = [int64]0
        if ($mod) {
            $mName = $mod.ModuleName
            $mBase = [int64]$mod.BaseAddress
            $mRva = $abs - $mBase
        }
        return [pscustomobject]@{
            Abs = $abs; Module = $mName; Base = $mBase; Rva = $mRva; Len = $len
        }
    }

    throw (("cannot parse -Bytes '{0}'. Expected '<module>+0x<rva>[:0x<len>]' " +
            "or '0x<absolute>[:0x<len>]'.") -f $spec)
}

function Read-Window($proc, $spec) {
    $t = Resolve-Spec $proc $spec
    if ($t.Len -le 0 -or $t.Len -gt 0x10000) {
        throw "length 0x$('{0:x}' -f $t.Len) out of range (1..0x10000)."
    }
    $h = [LiveDump]::OpenProcess([LiveDump]::RIGHTS, $false, [uint32]$proc.Id)
    if ($h -eq [IntPtr]::Zero) {
        throw ("OpenProcess(pid={0}) failed: win32 error {1}" -f $proc.Id,
               [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    try {
        $buf = New-Object byte[] $t.Len
        $got = [IntPtr]::Zero
        $ok = [LiveDump]::ReadProcessMemory($h, [IntPtr]$t.Abs, $buf,
                                            [IntPtr]$t.Len, [ref]$got)
        $n = [int]$got
        if (-not $ok -and $n -eq 0) {
            throw ("ReadProcessMemory(0x{0:x16}, 0x{1:x}) failed: win32 error {2}" -f `
                   $t.Abs, $t.Len, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
        Write-Host ""
        Write-Host ("== pid {0} live bytes: {1}+0x{2:x} = 0x{3:x16}, {4} of 0x{5:x} bytes read ==" -f `
                    $proc.Id, $t.Module, $t.Rva, $t.Abs, $n, $t.Len)
        Write-Host ("   (LOADED image - this is what executed, and it need not " +
                    "match the file on disk)")
        for ($i = 0; $i -lt $n; $i += 16) {
            $row = @()
            for ($j = 0; $j -lt 16 -and ($i + $j) -lt $n; $j++) {
                $row += ('{0:x2}' -f $buf[$i + $j])
            }
            # Row label is the RVA, not the absolute address: RVAs are what a
            # [crash] log line carries and what survives the next ASLR shuffle.
            Write-Host ("   +0x{0:x6}  {1}" -f ($t.Rva + $i), ($row -join ' '))
        }
        Write-Host ""
    } finally {
        [void][LiveDump]::CloseHandle($h)
    }
}

# ---------------------------------------------------------------------------
# Minidump mode
# ---------------------------------------------------------------------------
$KINDS = @{
    # Normal
    'mini' = 0x00000000
    # Normal | WithUnloadedModules | WithIndirectlyReferencedMemory |
    # WithThreadInfo | WithCodeSegs
    'code' = 0x00000020 -bor 0x00000040 -bor 0x00001000 -bor 0x00002000
    # WithFullMemory | WithHandleData | WithThreadInfo | WithFullMemoryInfo
    'full' = 0x00000002 -bor 0x00000004 -bor 0x00001000 -bor 0x00000800
}

function Write-Dump($proc, [string]$kind) {
    if ($kind -eq 'full' -and -not $Force) {
        throw ("-Kind full asks for the whole address space. That exact request " +
               "is what outran the page file with two Kenshi instances resident " +
               "on 2026-08-03 and needed a reboot. Re-run with -Force if you " +
               "really mean it, or use -Kind code (default), which carries the " +
               "loaded code bytes at a fraction of the size.")
    }
    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
    # Named after the pid (and the minute), per .gitignore's note: they never
    # collide and they are never committed.
    $out = Join-Path $OutDir ("{0}_{1}_{2}_{3}.dmp" -f $proc.ProcessName, $proc.Id,
                              $kind, (Get-Date -Format "yyyyMMdd_HHmmss"))

    $h = [LiveDump]::OpenProcess([LiveDump]::RIGHTS, $false, [uint32]$proc.Id)
    if ($h -eq [IntPtr]::Zero) {
        throw (("OpenProcess(pid={0}) failed: win32 error {1}. Same user, or run " +
                "elevated.") -f $proc.Id,
               [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    $fs = $null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $fs = [IO.File]::Create($out)
        $ok = [LiveDump]::MiniDumpWriteDump($h, [uint32]$proc.Id,
                  $fs.SafeFileHandle.DangerousGetHandle(), [uint32]$KINDS[$kind],
                  [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $fs.Flush()
        $len = $fs.Length
        $fs.Close(); $fs = $null
        if (-not $ok) {
            # A 0-byte file is the classic in-process failure signature; say so
            # rather than leaving a plausible-looking empty dump behind.
            Remove-Item $out -Force -ErrorAction SilentlyContinue
            throw (("MiniDumpWriteDump(pid={0}, kind={1}) failed after {2:N1}s: " +
                    "win32/hresult 0x{3:x8} ({3}). Nothing was kept.") -f `
                   $proc.Id, $kind, $sw.Elapsed.TotalSeconds, $err)
        }
        Write-Host ("dumped pid {0} ({1}) kind={2} in {3:N1}s -> {4} ({5:N1} MB)" -f `
                    $proc.Id, $proc.ProcessName, $kind, $sw.Elapsed.TotalSeconds,
                    $out, ($len / 1MB))
        Write-Host ("  resolve it with: powershell -ExecutionPolicy Bypass -File " +
                    "tools\_dmpinfo.ps1 -Dump `"$out`"")
        return $out
    } finally {
        if ($fs) { $fs.Close() }
        [void][LiveDump]::CloseHandle($h)
    }
}

# ---------------------------------------------------------------------------
foreach ($t in $targets) {
    if ($Bytes) { Read-Window $t $Bytes } else { [void](Write-Dump $t $Kind) }
}
