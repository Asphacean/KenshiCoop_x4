<#
.SYNOPSIS
  Resolve a minidump (or a bare module+RVA out of a [crash] log line) against
  KenshiCoop.map: modules, the exception, the register file, the loaded code
  bytes at rip, and candidate return addresses off the faulting stack.

.DESCRIPTION
  The other half of the triage path named in src\plugin\core\CrashDump.cpp:
  _dumplive.ps1 captures, this explains. Deliberately a standalone parser with
  no debugger dependency - cdb/WinDbg are not installed on the rig, and the one
  question a crash report has to answer (does the chain pass through
  KenshiCoop.dll at all?) needs nothing more than the module table and the map.

  WHAT IT PRINTS, and why each part earned its place:

  modules      Base + size + build timestamp for every loaded module, so an
               absolute address becomes module+RVA. ASLR moves every module, so
               RVAs are the only form worth writing down.
  exception    Code, the faulting address, and - for an access violation -
               READ/WRITE and the address touched.
  registers    The full integer file out of the dump's CONTEXT, plus WHICH
               register held the faulting address (exact, or a small positive
               delta, which is then the offset of the guilty struct member).
               The in-process VEH tracer logs the same two facts; this is the
               post-mortem route to them when the log was lost or truncated.
  code         Raw bytes around rip, in the same format the VEH tracer logs and
               _dumplive.ps1 -Bytes prints, so all three can be diffed. This is
               the part that matters most on this game: on 2026-09-07 a fault
               RVA turned out to be MID-INSTRUCTION in kenshi_x64.exe on disk
               under every 64K-aligned base, i.e. the loaded image is not the
               file on disk (RE_Kenshi rewrites it), so the disassembly has to
               come from the dump, not the install. Needs a dump taken with
               -Kind code or full.
  stack scan   NOT an unwind. Every qword on the faulting thread's stack that
               points into a module's code range, listed as a candidate return
               address. A real unwind needs .pdata for Kenshi, which the disk
               image cannot be trusted to supply here; a scan over-reports but
               never invents, and it is enough to answer "is KenshiCoop.dll on
               this stack".
  symbols      Any RVA inside a module that has a .map gets the nearest
               preceding public symbol and a +delta. The map is chosen by
               matching the module's build TIMESTAMP against the map header, so
               a stale map cannot quietly hand back wrong function names - that
               mismatch is called out loudly instead.

  Works with no dump at all: -Resolve alone symbolizes module+RVA strings
  copied straight out of a [crash] log line.

.EXAMPLE
  # Full report on a dump.
  powershell -ExecutionPolicy Bypass -File tools\_dmpinfo.ps1 `
      -Dump tools\Kenshi_x64_5200_code_20260907_172220.dmp

.EXAMPLE
  # No dump - just symbolize frames pasted from a coop log.
  powershell -ExecutionPolicy Bypass -File tools\_dmpinfo.ps1 `
      -Resolve "KenshiCoop.dll+0x2ac1f","KenshiCoop.dll+0x2ade8"

.EXAMPLE
  # The loaded bytes of a Kenshi function, out of the dump, for a disassembler.
  powershell -ExecutionPolicy Bypass -File tools\_dmpinfo.ps1 -Dump x.dmp `
      -Bytes "Kenshi_x64.exe+0x6ea950:0x90"
#>
param(
    [string]$Dump = "",
    # Override the .map. Default: every src\plugin\x64\*\KenshiCoop.map, picked
    # by build-timestamp match against the dump's KenshiCoop.dll.
    [string]$Map = "",
    # "KenshiCoop.dll+0x2ac1f", or a bare absolute "0x7fffbb29ac1f" (needs a dump
    # to attribute), or "0x2ac1f" with -Module.
    [string[]]$Resolve = @(),
    [string]$Module = "KenshiCoop.dll",
    # "<module>+0x<rva>[:0x<len>]" read out of the dump's captured memory.
    [string]$Bytes = "",
    [int]$CodeWindow = 0x40,
    [switch]$AllModules,
    [switch]$NoStackScan
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# powershell -File does NOT split an array argument on commas the way a
# dot-invocation does - it hands the whole "a,b,c" over as one string - so a
# copy-pasted comma list has to be split here or it parses as one bad address.
$Resolve = @($Resolve | ForEach-Object { $_ -split '[,;]' } |
             ForEach-Object { $_.Trim() } | Where-Object { $_ })

# ===========================================================================
# .map: publics by value -> nearest preceding symbol
# ===========================================================================
# Format (VC10 linker):
#    Preferred load address is 0000000180000000
#    Timestamp is 6a9d1293 (Sun Sep 06 10:13:23 2026)
#     0001:00029ac0       ?report@...@@YAXPEAU_EXCEPTION_POINTERS@@PEBD@Z 000000018002aac0 f   CrashDump.obj
# RVA = (Rva+Base column) - (preferred load address). The section:offset column
# is NOT the RVA - the two differ by the header size (0x1000 here), and using it
# is the classic way to land one page off every symbol.
function Read-MapFile([string]$path) {
    $pref = [uint64]0
    $stamp = ""
    $syms = New-Object System.Collections.ArrayList
    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($pref -eq 0 -and $line -match 'Preferred load address is ([0-9a-fA-F]+)') {
            $pref = [Convert]::ToUInt64($Matches[1], 16); continue
        }
        if (-not $stamp -and $line -match 'Timestamp is ([0-9a-fA-F]+)') {
            $stamp = $Matches[1].ToLower(); continue
        }
        if ($line -notmatch '^\s+(\d{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s') { continue }
        if ($Matches[1] -eq '0000') { continue }        # <absolute>/<linker-defined>
        $rvaBase = [Convert]::ToUInt64($Matches[4], 16)
        if ($rvaBase -lt $pref) { continue }
        [void]$syms.Add([pscustomobject]@{
            Rva = [uint64]($rvaBase - $pref); Name = $Matches[3]; Obj = $line.Trim()
        })
    }
    return [pscustomobject]@{
        Path = $path
        Preferred = $pref
        Timestamp = $stamp
        Symbols = @($syms | Sort-Object Rva)
    }
}

$script:maps = @()
if ($Map) {
    $script:maps = @(Read-MapFile (Resolve-Path $Map).Path)
} else {
    foreach ($m in @(Get-ChildItem -Path (Join-Path $repo "src\plugin\x64") `
                     -Filter "KenshiCoop.map" -Recurse -ErrorAction SilentlyContinue)) {
        $script:maps += (Read-MapFile $m.FullName)
    }
}

# dbghelp undecorates MSVC-mangled names. Optional: a raw '?report@...@@YAX...'
# is still readable, so a failure here degrades instead of aborting.
$script:canUndec = $false
try {
    if ('Undec' -as [type]) { $script:canUndec = $true }
    else { Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Undec {
  [DllImport("dbghelp.dll", CharSet=CharSet.Ansi, SetLastError=true)]
  public static extern uint UnDecorateSymbolName(string name, StringBuilder outp,
                                                 uint maxLen, uint flags);
}
"@
          $script:canUndec = $true }
} catch { }

function Undecorate([string]$name) {
    if (-not $script:canUndec -or -not $name.StartsWith('?')) { return $name }
    try {
        $sb = New-Object Text.StringBuilder 1024
        # 0x1000 UNDNAME_NAME_ONLY: just the qualified name. The signature is
        # noise in a frame list and pushes the useful part off the line.
        if ([Undec]::UnDecorateSymbolName($name, $sb, 1024, 0x1000) -gt 0) {
            return $sb.ToString()
        }
    } catch { }
    return $name
}

function Find-Symbol($map, [uint64]$rva) {
    if (-not $map -or -not $map.Symbols.Count) { return $null }
    $lo = 0; $hi = $map.Symbols.Count - 1; $best = -1
    while ($lo -le $hi) {
        $mid = [int](($lo + $hi) / 2)
        if ($map.Symbols[$mid].Rva -le $rva) { $best = $mid; $lo = $mid + 1 }
        else { $hi = $mid - 1 }
    }
    if ($best -lt 0) { return $null }
    $s = $map.Symbols[$best]
    return [pscustomobject]@{
        Name = (Undecorate $s.Name); Raw = $s.Name; Rva = $s.Rva
        Delta = [uint64]($rva - $s.Rva)
    }
}

# ===========================================================================
# minidump reader
# ===========================================================================
$script:fs = $null
$script:mods = @()
$script:ranges = @()
$script:streams = @{}

function Read-Raw([long]$off, [int]$n) {
    $buf = New-Object byte[] $n
    $script:fs.Position = $off
    $got = 0
    while ($got -lt $n) {
        $k = $script:fs.Read($buf, $got, $n - $got)
        if ($k -le 0) { break }
        $got += $k
    }
    return $buf
}
function U16([long]$o) { [BitConverter]::ToUInt16((Read-Raw $o 2), 0) }
function U32([long]$o) { [BitConverter]::ToUInt32((Read-Raw $o 4), 0) }
function U64([long]$o) { [BitConverter]::ToUInt64((Read-Raw $o 8), 0) }
function MdString([long]$rva) {
    $n = U32 $rva
    if ($n -le 0 -or $n -gt 8192) { return "" }
    return [Text.Encoding]::Unicode.GetString((Read-Raw ($rva + 4) $n))
}

function Open-Dump([string]$path) {
    $script:fs = [IO.File]::Open($path, 'Open', 'Read', 'Read')
    if ((U32 0) -ne 0x504D444D) { throw "$path is not a minidump (bad MDMP signature)." }
    $nStreams = U32 8
    $dirRva = U32 12
    for ($i = 0; $i -lt $nStreams; $i++) {
        $e = $dirRva + ($i * 12)
        $type = U32 $e
        $script:streams[[int]$type] = [pscustomobject]@{ Size = (U32 ($e + 4)); Rva = (U32 ($e + 8)) }
    }

    # --- ModuleListStream (4): count, then 108-byte MINIDUMP_MODULE records ---
    if ($script:streams.ContainsKey(4)) {
        $rva = $script:streams[4].Rva
        $n = U32 $rva
        $mods = New-Object System.Collections.ArrayList
        for ($i = 0; $i -lt $n; $i++) {
            $m = $rva + 4 + ($i * 108)
            $full = MdString (U32 ($m + 20))
            [void]$mods.Add([pscustomobject]@{
                Base = (U64 $m)
                Size = (U32 ($m + 8))
                Stamp = ('{0:x8}' -f (U32 ($m + 16)))
                Name = (Split-Path $full -Leaf)
                Full = $full
            })
        }
        $script:mods = @($mods | Sort-Object Base)
    }

    # --- memory: MemoryListStream (5) and/or Memory64ListStream (9) ----------
    $rs = New-Object System.Collections.ArrayList
    if ($script:streams.ContainsKey(5)) {
        $rva = $script:streams[5].Rva
        $n = U32 $rva
        for ($i = 0; $i -lt $n; $i++) {
            # MINIDUMP_MEMORY_DESCRIPTOR: u64 start, then a LOCATION_DESCRIPTOR
            # { u32 DataSize; u32 Rva } - so size at +8 and file offset at +12.
            $d = $rva + 4 + ($i * 16)
            [void]$rs.Add([pscustomobject]@{
                Start = (U64 $d); Size = [uint64](U32 ($d + 8)); Off = [uint64](U32 ($d + 12))
            })
        }
    }
    if ($script:streams.ContainsKey(9)) {
        # MINIDUMP_MEMORY64_LIST: u64 count, u64 baseRva, then {u64 start, u64
        # size} records whose DATA is contiguous from baseRva. This is the layout
        # a full-memory dump uses.
        $rva = $script:streams[9].Rva
        $n = U64 $rva
        $off = U64 ($rva + 8)
        for ($i = 0; $i -lt [int]$n; $i++) {
            $d = $rva + 16 + ($i * 16)
            $start = U64 $d
            $size = U64 ($d + 8)
            [void]$rs.Add([pscustomobject]@{ Start = $start; Size = $size; Off = $off })
            $off += $size
        }
    }
    $script:ranges = @($rs | Sort-Object Start)
}

function Find-ModuleAt([uint64]$va) {
    foreach ($m in $script:mods) {
        if ($va -ge $m.Base -and $va -lt ($m.Base + [uint64]$m.Size)) { return $m }
    }
    return $null
}

function Get-MapFor($mod) {
    if (-not $mod) { return $null }
    if ($mod.Name -inotmatch '^KenshiCoop\.dll$') { return $null }
    if (-not $script:maps.Count) { return $null }
    # Exact build identity: the map header's "Timestamp is XXXXXXXX" is the same
    # PE TimeDateStamp the dump records. Symbolizing against a map from a
    # different build silently renames every function, so a match is required
    # before the names are trusted.
    $hit = @($script:maps | Where-Object { $_.Timestamp -eq $mod.Stamp })
    if ($hit.Count) { return $hit[0] }
    return $null
}

function Read-Va([uint64]$va, [int]$n) {
    foreach ($r in $script:ranges) {
        if ($va -lt $r.Start -or $va -ge ($r.Start + $r.Size)) { continue }
        $avail = [int][Math]::Min([uint64]$n, ($r.Start + $r.Size - $va))
        return Read-Raw ([long]($r.Off + ($va - $r.Start))) $avail
    }
    return $null
}

# ===========================================================================
# formatting
# ===========================================================================
function Format-Addr([uint64]$va) {
    $m = Find-ModuleAt $va
    if (-not $m) { return ('0x{0:x16} <unmapped>' -f $va) }
    $rva = [uint64]($va - $m.Base)
    $s = Find-Symbol (Get-MapFor $m) $rva
    $txt = '0x{0:x16} {1}+0x{2:x}' -f $va, $m.Name, $rva
    if ($s) { $txt += ('  {0}+0x{1:x}' -f $s.Name, $s.Delta) }
    return $txt
}

function Show-Hex([byte[]]$b, [uint64]$rvaAt, [string]$label) {
    if (-not $b -or -not $b.Length) { Write-Host "   <no memory captured here>"; return }
    Write-Host ("   $label")
    for ($i = 0; $i -lt $b.Length; $i += 16) {
        $row = @()
        for ($j = 0; $j -lt 16 -and ($i + $j) -lt $b.Length; $j++) {
            $row += ('{0:x2}' -f $b[$i + $j])
        }
        Write-Host ("   +0x{0:x6}  {1}" -f ($rvaAt + $i), ($row -join ' '))
    }
}

# ===========================================================================
if ($Dump) {
    $Dump = (Resolve-Path $Dump).Path
    Open-Dump $Dump
    $fi = Get-Item $Dump
    Write-Host ""
    Write-Host ("== dump {0} ({1:N1} MB, taken {2:yyyy-MM-dd HH:mm:ss}) ==" -f `
                (Split-Path $Dump -Leaf), ($fi.Length / 1MB), $fi.LastWriteTime)

    # SystemInfoStream (7): arch at +0, build number at +16.
    if ($script:streams.ContainsKey(7)) {
        $s = $script:streams[7].Rva
        $arch = switch ([int](U16 $s)) { 9 { 'x64' } 0 { 'x86' } default { "arch$([int](U16 $s))" } }
        Write-Host ("   {0}, windows build {1}, {2} module(s), {3} memory range(s)" -f `
                    $arch, (U32 ($s + 16)), $script:mods.Count, $script:ranges.Count)
    }
    if (-not $script:ranges.Count) {
        Write-Host ("   NOTE: this dump captured no memory ranges - re-take it " +
                    "with -Kind code to get the loaded code bytes.")
    }

    # --- modules ---------------------------------------------------------
    Write-Host ""
    Write-Host "== modules =="
    $show = if ($AllModules) { $script:mods } else {
        @($script:mods | Where-Object {
            $_.Name -imatch '^(Kenshi_x64\.exe|KenshiCoop\.dll|RE_Kenshi\.dll|KenshiLib\.dll|ntdll\.dll|MSVCR100\.dll|MyGUIEngine_x64\.dll|OgreMain_x64\.dll|kernel32\.dll|KERNELBASE\.dll)$'
        })
    }
    '{0,-24} {1,-18} {2,-10} {3}' -f 'MODULE', 'BASE', 'SIZE', 'BUILD-STAMP' | Write-Host
    foreach ($m in $show) {
        $mapNote = ""
        if ($m.Name -ieq 'KenshiCoop.dll') {
            $mp = Get-MapFor $m
            if ($mp) { $mapNote = "  map=" + (Split-Path (Split-Path $mp.Path -Parent) -Leaf) }
            elseif ($script:maps.Count) {
                $mapNote = ("  NO MATCHING MAP (have {0}) - symbols withheld" -f `
                            (($script:maps | ForEach-Object { $_.Timestamp }) -join ','))
            } else { $mapNote = "  no .map found under src\plugin\x64" }
        }
        '{0,-24} 0x{1:x16} 0x{2:x8}  {3}{4}' -f $m.Name, $m.Base, $m.Size, $m.Stamp, $mapNote | Write-Host
    }
    if (-not $AllModules -and $script:mods.Count -gt $show.Count) {
        Write-Host ("   ({0} more; -AllModules for the full table)" -f ($script:mods.Count - $show.Count))
    }

    # --- exception -------------------------------------------------------
    if ($script:streams.ContainsKey(6)) {
        $es = $script:streams[6].Rva
        $tid = U32 $es
        $er = $es + 8
        $code = U32 $er
        $addr = U64 ($er + 16)
        $nPar = U32 ($er + 24)
        $ctxRva = U32 ($es + 160 + 4)

        Write-Host ""
        Write-Host "== exception =="
        Write-Host ("   thread {0}, code 0x{1:x8}, at {2}" -f $tid, $code, (Format-Addr $addr))
        if ($code -eq 0xC0000005 -and $nPar -ge 2) {
            $kind = switch ([int](U64 ($er + 32))) { 0 { 'READ' } 1 { 'WRITE' } 8 { 'EXECUTE' } default { 'ACCESS' } }
            $at = U64 ($er + 40)
            Write-Host ("   {0} of {1}" -f $kind, (Format-Addr $at))
        }

        # --- registers (CONTEXT_AMD64) -----------------------------------
        if ($ctxRva -gt 0) {
            $c = $ctxRva
            $regs = [ordered]@{
                rax = (U64 ($c + 0x78)); rcx = (U64 ($c + 0x80)); rdx = (U64 ($c + 0x88))
                rbx = (U64 ($c + 0x90)); rsp = (U64 ($c + 0x98)); rbp = (U64 ($c + 0xa0))
                rsi = (U64 ($c + 0xa8)); rdi = (U64 ($c + 0xb0)); r8  = (U64 ($c + 0xb8))
                r9  = (U64 ($c + 0xc0)); r10 = (U64 ($c + 0xc8)); r11 = (U64 ($c + 0xd0))
                r12 = (U64 ($c + 0xd8)); r13 = (U64 ($c + 0xe0)); r14 = (U64 ($c + 0xe8))
                r15 = (U64 ($c + 0xf0))
            }
            $rip = U64 ($c + 0xf8)
            Write-Host ""
            Write-Host "== registers =="
            $names = @($regs.Keys)
            for ($i = 0; $i -lt $names.Count; $i += 4) {
                $parts = @()
                for ($j = 0; $j -lt 4; $j++) {
                    $k = $names[$i + $j]
                    $parts += ('{0,-3}={1:x16}' -f $k, $regs[$k])
                }
                Write-Host ("   " + ($parts -join ' '))
            }
            Write-Host ("   rip={0:x16} eflags={1:x8}" -f $rip, (U32 ($c + 0x44)))

            if ($code -eq 0xC0000005 -and $nPar -ge 2) {
                # Which register the bad address came out of. An exact match
                # means the pointer itself was bad; a small positive delta means
                # a field load off a bad base, and the delta is that member's
                # offset - which is usually the fact that names the bug.
                $at = U64 ($er + 40)
                $hits = @()
                foreach ($k in $names) {
                    $v = $regs[$k]
                    if ($at -lt $v) { continue }
                    $d = [uint64]($at - $v)
                    if ($d -gt 0xfff) { continue }
                    # Same rule as the in-process tracer: an exact match is
                    # always the pointer, but a DELTA only counts off a register
                    # big enough to be an address. Without that floor, a low
                    # fault address matches every zeroed register - a planted
                    # read of 0x48 named eleven of them.
                    if ($d -ne 0 -and $v -lt 0x10000) { continue }
                    $hits += ('{0}+0x{1:x}' -f $k, $d)
                }
                $held = '<no integer register within 0x1000 - computed or spilled operand>'
                if ($hits.Count) { $held = $hits -join ' ' }
                Write-Host ("   fault address held by: {0}" -f $held)
            }

            # --- code window ---------------------------------------------
            Write-Host ""
            Write-Host "== code at rip =="
            $start = [uint64]($addr - [uint64]$CodeWindow)
            $m = Find-ModuleAt $start
            $b = Read-Va $start ($CodeWindow * 2)
            if ($m) {
                Show-Hex $b ([uint64]($start - $m.Base)) `
                    ("{0}+0x{1:x} = rip-0x{2:x}, RVA-labelled rows of 16 (LOADED image)" -f `
                     $m.Name, ($start - $m.Base), $CodeWindow)
            } else {
                Show-Hex $b $start "unmapped address, absolute-labelled rows of 16"
            }

            # --- stack scan ----------------------------------------------
            if (-not $NoStackScan) {
                Write-Host ""
                Write-Host "== stack scan (candidate return addresses, NOT an unwind) =="
                $sb = Read-Va $regs['rsp'] 0x1000
                if (-not $sb) {
                    Write-Host "   <the faulting thread's stack was not captured>"
                } else {
                    $shown = 0
                    for ($i = 0; $i + 8 -le $sb.Length -and $shown -lt 48; $i += 8) {
                        $q = [BitConverter]::ToUInt64($sb, $i)
                        if ($q -lt 0x10000) { continue }
                        $mm = Find-ModuleAt $q
                        if (-not $mm) { continue }
                        Write-Host ("   rsp+0x{0:x4}  {1}" -f $i, (Format-Addr $q))
                        $shown++
                    }
                    if (-not $shown) { Write-Host "   <no module-resident qwords in the first 4 KB>" }
                }
            }
        }
    } elseif ($Dump) {
        Write-Host ""
        Write-Host ("== exception ==`n   none recorded (this dump was taken on " +
                    "demand, not at a fault)")
    }

    # --- threads ---------------------------------------------------------
    if ($script:streams.ContainsKey(3)) {
        Write-Host ""
        Write-Host ("== threads: {0} ==" -f (U32 $script:streams[3].Rva))
    }
}

# ===========================================================================
# -Bytes: arbitrary window out of the dump
# ===========================================================================
if ($Bytes) {
    if (-not $Dump) { throw "-Bytes needs -Dump (use tools\_dumplive.ps1 -Bytes for a live process)." }
    $len = 0x100
    $body = $Bytes
    if ($Bytes -match '^(.*):\s*(0x[0-9a-fA-F]+|\d+)\s*$') {
        $body = $Matches[1]
        $len = if ($Matches[2] -like '0x*') { [Convert]::ToInt32($Matches[2].Substring(2), 16) } else { [int]$Matches[2] }
    }
    $body = $body.Trim()
    if ($body -match '^(.+?)\s*\+\s*(?:0x)?([0-9a-fA-F]+)$') {
        $mn = $Matches[1].Trim()
        $rva = [Convert]::ToUInt64($Matches[2], 16)
        $m = @($script:mods | Where-Object { $_.Name -ieq $mn })[0]
        if (-not $m) { throw "module '$mn' is not in this dump's module list." }
        Write-Host ""
        Write-Host ("== bytes {0}+0x{1:x}, 0x{2:x} requested ==" -f $m.Name, $rva, $len)
        Show-Hex (Read-Va ([uint64]($m.Base + $rva)) $len) $rva "RVA-labelled rows of 16"
    } elseif ($body -match '^(?:0x)?([0-9a-fA-F]+)$') {
        $va = [Convert]::ToUInt64($Matches[1], 16)
        $m = Find-ModuleAt $va
        $label = $va
        if ($m) { $label = [uint64]($va - $m.Base) }
        Write-Host ""
        Write-Host ("== bytes at {0} ==" -f (Format-Addr $va))
        Show-Hex (Read-Va $va $len) $label "rows of 16"
    } else {
        throw "cannot parse -Bytes '$Bytes'. Expected '<module>+0x<rva>[:0x<len>]' or '0x<va>[:0x<len>]'."
    }
}

# ===========================================================================
# -Resolve: symbolize addresses, with or without a dump
# ===========================================================================
if ($Resolve.Count) {
    Write-Host ""
    Write-Host "== resolve =="
    foreach ($spec in $Resolve) {
        $s = $spec.Trim()
        if ($s -match '^(.+?)\s*\+\s*(?:0x)?([0-9a-fA-F]+)$') {
            $mn = $Matches[1].Trim()
            $rva = [Convert]::ToUInt64($Matches[2], 16)
            if ($script:mods.Count) {
                $m = @($script:mods | Where-Object { $_.Name -ieq $mn })[0]
                if ($m) { Write-Host ("   {0} -> {1}" -f $s, (Format-Addr ([uint64]($m.Base + $rva)))); continue }
            }
            # No dump (or module absent from it): the map alone still answers,
            # which is the point - these strings get pasted out of a coop log.
            if ($mn -imatch '^KenshiCoop\.dll$' -and $script:maps.Count) {
                foreach ($mp in $script:maps) {
                    $sym = Find-Symbol $mp $rva
                    $cfg = Split-Path (Split-Path $mp.Path -Parent) -Leaf
                    if ($sym) {
                        Write-Host ("   {0} -> [{1} map, stamp {2}] {3}+0x{4:x}" -f `
                                    $s, $cfg, $mp.Timestamp, $sym.Name, $sym.Delta)
                    } else {
                        Write-Host ("   {0} -> [{1} map] no symbol at or below that RVA" -f $s, $cfg)
                    }
                }
                Write-Host ("     (no dump given, so the BUILD is unverified - a " +
                            "map from another build renames every function)")
            } else {
                Write-Host (("   {0} -> no .map for '{1}'; needs the loaded bytes " +
                             "(tools\_dumplive.ps1 -Bytes) to go further") -f $s, $mn)
            }
            continue
        }
        if ($s -match '^(?:0x)?([0-9a-fA-F]+)$') {
            $v = [Convert]::ToUInt64($Matches[1], 16)
            if ($script:mods.Count) { Write-Host ("   {0} -> {1}" -f $s, (Format-Addr $v)) }
            else { Write-Host ("   {0} -> absolute address needs -Dump to attribute (ASLR)" -f $s) }
            continue
        }
        Write-Host ("   {0} -> cannot parse (want 'module.dll+0xRVA' or '0xVA')" -f $s)
    }
}

if ($script:fs) { $script:fs.Close() }
if (-not $Dump -and -not $Resolve.Count) {
    Write-Host ("nothing to do. Pass -Dump <file.dmp> (see tools\_dumplive.ps1) " +
                "and/or -Resolve `"KenshiCoop.dll+0x2ac1f`".")
}
