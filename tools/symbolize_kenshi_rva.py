#!/usr/bin/env python3
"""Symbolize Kenshi crash frames against the image that ACTUALLY RUNS.

WHY THIS EXISTS
---------------
RE_Kenshi downgrades Kenshi 1.0.68 -> 1.0.65 with courgette and writes the result
to disk as a REAL FILE:

    <install>\\RE_Kenshi\\kenshi_x64.exe        <- this is what the process loads
    <install>\\kenshi_x64.exe                   <- this is NOT

Decoding a crash RVA against the pristine exe puts every frame mid-instruction.
A 2026-09-07 debug session hit exactly that, brute-forced 360 image bases looking
for a relocation, and concluded "static symbolization is impossible; requires a
live minidump" -- a conclusion that blocked the investigation for five days. The
correct image was sitting in a subdirectory the whole time. See
.planning/debug/ (crash-av-worker-thread, evidence E-07/E-08/E-49/E-50).

RULE: if a frame lands mid-instruction, you have the WRONG FILE. That is not a
failure of the method.

USAGE
-----
    python tools/symbolize_kenshi_rva.py 0x6ea9ab 0x6eaa2c 0xed6456
    python tools/symbolize_kenshi_rva.py --compare 0x6ea9ab
    python tools/symbolize_kenshi_rva.py --disasm 0x6ea990:0x30
    python tools/symbolize_kenshi_rva.py --install F:\\KenshiCoop-Clone4 0x6ea9ab
    python tools/symbolize_kenshi_rva.py --image <any pe file> 0x6ea9ab

A clean instruction boundary confirms you are on the right image. `--compare`
shows the pristine image alongside, which is how the wrong-file failure mode is
recognised at a glance.

NOTES
-----
* Disassembly requires `capstone` (pip install capstone). Without it the tool
  still reports .pdata enclosing-function bounds, which is often enough to
  identify a frame via RE_Kenshi's RVA tables.
* This reads the file on disk. It matches the loaded image byte for byte except
  at sites MinHook patches at runtime; for those, read the live process with
  tools/_dumplive.ps1 -Bytes instead.
* To turn a function NAMED in a vendored KenshiLib header into an RVA, use the
  index-parallel table lookup: GOG_1.0.65.br[i] <-> Steam_1.0.65.br[i] under
  <install>\\RE_Kenshi\\RVAs\\. The two techniques compose and neither needs a
  running process.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

DEFAULT_INSTALL = r"F:\KenshiCoop-Clone1"
LOADED_SUBPATH = os.path.join("RE_Kenshi", "kenshi_x64.exe")
PRISTINE_SUBPATH = "kenshi_x64.exe"


class PEImage(object):
    """Minimal PE64 reader: section map, .pdata function table, code bytes."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("%s is not a PE file (no MZ header)" % path)
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s has no PE signature" % path)
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x20B:
            raise ValueError("%s is not PE32+ (x64)" % path)
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]

        # Data directory 3 == .pdata (exception table).
        pdata_rva, pdata_sz = struct.unpack_from("<II", d, opt + 112 + 3 * 8)

        self.sections = []
        for i in range(nsec):
            o = opt + optsz + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsz, vaddr, rsz, raddr = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, vaddr, vsz, raddr, rsz))

        self.functions = []
        po = self.rva_to_offset(pdata_rva)
        if po is not None:
            for i in range(pdata_sz // 12):
                begin, end, _unwind = struct.unpack_from("<III", d, po + i * 12)
                if begin == 0 and end == 0:
                    break
                self.functions.append((begin, end))
            self.functions.sort()

    def rva_to_offset(self, rva):
        for _name, vaddr, vsz, raddr, rsz in self.sections:
            if vaddr <= rva < vaddr + max(vsz, rsz):
                return raddr + (rva - vaddr)
        return None

    def section_of(self, rva):
        for name, vaddr, vsz, raddr, rsz in self.sections:
            if vaddr <= rva < vaddr + max(vsz, rsz):
                return name
        return None

    def enclosing_function(self, rva):
        """Binary-search the sorted .pdata table for the function containing rva."""
        lo, hi, best = 0, len(self.functions) - 1, None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.functions[mid][0] <= rva:
                best = self.functions[mid]
                lo = mid + 1
            else:
                hi = mid - 1
        if best and best[0] <= rva < best[1]:
            return best
        return None

    def read(self, rva, length):
        off = self.rva_to_offset(rva)
        if off is None:
            return None
        return self.data[off:off + length]


def get_disassembler():
    try:
        import capstone
    except ImportError:
        return None, None
    return capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64), capstone.__version__


def check_boundary(img, rva, md):
    """Return (verdict, instruction_or_None, func_bounds_or_None).

    An instruction beginning exactly at `rva` means this image is the one that
    produced the crash record. Mid-instruction means wrong image.
    """
    func = img.enclosing_function(rva)
    if func is None:
        return "NO .pdata ENTRY", None, None
    if md is None:
        return "(no capstone - bounds only)", None, func
    start, end = func
    code = img.read(start, end - start)
    if code is None:
        return "UNMAPPED", None, func
    for ins in md.disasm(code, start):
        if ins.address == rva:
            return "BOUNDARY", ins, func
        if ins.address > rva:
            break
    return "MID-INSTRUCTION", None, func


def report(img, label, rvas, md):
    print("\n=== %s" % label)
    print("    %s" % img.path)
    print("    image base 0x%x   %d .pdata functions" %
          (img.image_base, len(img.functions)))
    for rva in rvas:
        verdict, ins, func = check_boundary(img, rva, md)
        bounds = "[func 0x%x-0x%x]" % func if func else "[no func]"
        sect = img.section_of(rva) or "?"
        if ins is not None:
            print("  RVA 0x%-8x %-16s %-40s bytes: %-24s %s  (%s)" % (
                rva, verdict, "%s %s" % (ins.mnemonic, ins.op_str),
                ins.bytes.hex(" "), bounds, sect))
        else:
            print("  RVA 0x%-8x %-16s %-40s %-32s %s  (%s)" % (
                rva, verdict, "", "", bounds, sect))


def disasm_range(img, rva, length, md):
    if md is None:
        print("capstone is not installed; cannot disassemble "
              "(pip install capstone)", file=sys.stderr)
        return 2
    code = img.read(rva, length)
    if code is None:
        print("RVA 0x%x is not mapped in %s" % (rva, img.path), file=sys.stderr)
        return 2
    print("\n=== disassembly %s  RVA 0x%x..0x%x" % (img.path, rva, rva + length))
    for ins in md.disasm(code, rva):
        print("  0x%-8x %-24s %s %s" % (
            ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str))
    return 0


def parse_int(text):
    return int(text, 16) if text.lower().startswith("0x") else int(text, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Symbolize Kenshi RVAs against RE_Kenshi's downgraded image "
                    "(the one that actually runs).",
        epilog="If a frame lands MID-INSTRUCTION you have the wrong file, not a "
               "broken method.")
    ap.add_argument("rvas", nargs="*",
                    help="RVAs to test, e.g. 0x6ea9ab (module base is not needed)")
    ap.add_argument("--install", default=DEFAULT_INSTALL,
                    help="Kenshi install root (default: %s)" % DEFAULT_INSTALL)
    ap.add_argument("--image", default=None,
                    help="explicit PE path, overrides --install")
    ap.add_argument("--compare", action="store_true",
                    help="also test the pristine <install>/kenshi_x64.exe, to show "
                         "the wrong-file contrast")
    ap.add_argument("--disasm", metavar="RVA:LEN", default=None,
                    help="disassemble a window, e.g. 0x6ea990:0x30")
    args = ap.parse_args(argv)

    if not args.rvas and not args.disasm:
        ap.error("give at least one RVA, or --disasm RVA:LEN")

    loaded_path = args.image or os.path.join(args.install, LOADED_SUBPATH)
    if not os.path.isfile(loaded_path):
        print("ERROR: %s does not exist.\n"
              "       Expected RE_Kenshi's downgraded image there. Pass --install "
              "or --image." % loaded_path, file=sys.stderr)
        return 2

    md, cs_version = get_disassembler()
    if cs_version:
        print("capstone %s" % cs_version)
    else:
        print("capstone not installed - reporting .pdata bounds only "
              "(pip install capstone for boundary tests)")

    img = PEImage(loaded_path)
    rvas = [parse_int(r) for r in args.rvas]

    if rvas:
        report(img, "LOADED IMAGE (RE_Kenshi downgrade) - trust this one", rvas, md)
        if args.compare:
            pristine = os.path.join(args.install, PRISTINE_SUBPATH)
            if os.path.isfile(pristine):
                report(PEImage(pristine),
                       "PRISTINE INSTALL - DOES NOT RUN, shown only for contrast",
                       rvas, md)
            else:
                print("\n(no pristine image at %s to compare against)" % pristine)

    rc = 0
    if args.disasm:
        spec = args.disasm.split(":")
        if len(spec) != 2:
            ap.error("--disasm wants RVA:LEN, e.g. 0x6ea990:0x30")
        rc = disasm_range(img, parse_int(spec[0]), parse_int(spec[1]), md)
    return rc


if __name__ == "__main__":
    sys.exit(main())
