#include "CrashDump.h"
#include "../CoopLog.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace coop {
namespace crashdump {

// What this is, and what it deliberately is NOT.
//
// It is a first-chance FAULT tracer: one log line naming the faulting module +
// offset, what address was read or written (or what C++ type was thrown), and the
// call chain. With KenshiCoop.map those offsets resolve to function names, which is
// the question a crash report has to answer. It always returns CONTINUE_SEARCH, so
// Kenshi's crash dialog and RE_Kenshi's emergency save behave exactly as before.
//
// It watches more than access violations, because on 2026-08-03 an AV-only tracer
// watched the join die and logged nothing about it. The killer was an unhandled C++
// throw (0xE06D7363) whose ThrowInfo sat in msvcr100 - a std:: exception - and the
// only reason we ever learned that was Kenshi's own dump happening to survive that
// run. Anything that can end the process has to be able to name itself here, or a
// session costs a dump-forensics detour to learn one word.
//
// It is NOT a minidump writer. Three shapes were measured on 2026-08-03, all on a
// deliberately planted fault:
//   1. SetUnhandledExceptionFilter never runs at all. Kenshi catches its own faults
//      through the CRT/MFC and UNWINDS before showing "Kenshi has crashed", so no
//      unhandled-exception filter is ever reached - and dumping the process from
//      outside while it sits on that dialog is too late, because the frames that
//      faulted are already below rsp and no longer captured.
//   2. MiniDumpWriteDump called on the faulting thread re-faults inside dbghelp
//      ("READ of 0xFFFFFFFFFFFFFFFF"), leaving a 0-byte file.
//   3. MiniDumpWriteDump called from a worker thread while the faulting thread
//      waits never returns - it timed out after 20 s, still 0 bytes. Blocking the
//      faulting thread that long is worse than having no dump, because it delays
//      the engine's emergency save.
// The engine's own dumper does not fare better: it asks for a FULL memory image,
// and with a second Kenshi resident that request outran the page file, logging
// "CRASHDUMP FAIL: [1455] The paging file is too small" and locking the machine
// hard enough to need a reboot.
//
// So: no dump from inside the fault. For a client wedged on the crash dialog (or
// any hang), tools\_dumplive.ps1 takes a small dump from OUTSIDE the process,
// which cannot deadlock it, and tools\_dmpinfo.ps1 resolves it against the map.
//
// The one cost of a vectored handler is that it sees faults somebody means to
// handle, and both kinds do occur: the mod's own SEH-guarded probes trip (the same
// 2026-08-03 session caught markerUpdate dereferencing a GUI label the engine had
// already destroyed), and the engine throws and catches C++ exceptions as routine
// traffic. Per-site budgeting is what keeps that readable - see budgetFor - and a
// reported fault is therefore evidence of a bug, not proof of a crash.

namespace {

const DWORD ACCESS_VIOLATION = 0xC0000005;
// A C++ throw. Mostly routine - the engine throws and catches constantly - but an
// UNCAUGHT one ends the process, and first chance is the only chance we get to see
// it, so it is reported and left to the budget to keep quiet.
const DWORD CPP_THROW        = 0xE06D7363;
const ULONG_PTR CPP_MAGIC    = 0x19930520;   // ExceptionInformation[0] on a real throw

// Codes that end a process outright. None has been seen here yet; they are listed
// because the cost of missing one is another blind session.
bool hardFatal(DWORD code) {
    switch (code) {
    case 0xC0000374:   // heap corruption
    case 0xC00000FD:   // stack overflow
    case 0xC000001D:   // illegal instruction
    case 0xC0000096:   // privileged instruction
    case 0xC0000094:   // integer divide by zero
    case 0xC0000409:   // fast-fail / security check failure
        return true;
    default:
        return false;
    }
}

LPTOP_LEVEL_EXCEPTION_FILTER g_prev = 0;
PVOID g_veh       = 0;
bool  g_installed = false;
// Re-entry guard: a fault raised while we are already reporting one must not come
// back through here.
LONG  g_busy      = 0;

// Repeat suppression, PER SITE rather than globally. Faults do repeat every frame
// once state is corrupt, so some cap is needed - but a single shared budget is
// what blinded this tracer on 2026-08-03: about twenty HANDLED null reads from one
// MSVCR100 site burned the whole allowance inside 200 ms, and the fatal
// CharBody::_patrol fault eight minutes later never appeared as a VEH line at all.
// Only the UEF backstop caught it, without the stack. A noisy site must never cost
// us the first sighting of a new one, so each address gets its own small budget
// and an unseen address is always reported.
const int MAX_SITES       = 64;
const int REPORTS_PER_SITE = 3;
// Throws are budgeted separately and far more generously, because a throw report
// is ONE line where a fault report is thirty. Measured on leader_move: with stacks
// attached, eight benign caught Ogre throws produced 240 of the run's 608 log lines
// - and these logs are what the PowerShell oracles read, so drowning them to
// describe exceptions the engine handles on purpose is a bad trade. The type name
// is the part worth having (it is the single word that identified the 2026-08-03
// killer), the call chain is not, so throws get a cheap line and a long allowance.
const int REPORTS_PER_THROW = 8;
struct Site { const void* addr; int n; };
Site g_sites[MAX_SITES];
int  g_nSites = 0;

// >0 report it (and if == cap, say we are muting the site), 0 stay quiet.
int budgetFor(const void* addr, int cap) {
    for (int i = 0; i < g_nSites; ++i) {
        if (g_sites[i].addr != addr) continue;
        if (g_sites[i].n >= cap) return 0;
        return ++g_sites[i].n;
    }
    if (g_nSites >= MAX_SITES) return 0;
    g_sites[g_nSites].addr = addr;
    g_sites[g_nSites].n    = 1;
    ++g_nSites;
    return 1;
}

// ntdll's own unwinder. It is the only stack walker safe to call from here: no
// dbghelp, no symbol load, no file write and no blocking - which is exactly what
// disqualified MiniDumpWriteDump in all three shapes measured above. Resolved by
// name because the SDK this builds against does not declare it.
typedef USHORT (WINAPI* CaptureStackFn)(ULONG, ULONG, PVOID*, PULONG);
CaptureStackFn g_capture = 0;

// Name the module a fault address lands in, as module+RVA. A bare absolute address
// is worthless in a later triage session because ASLR moves every module, so the
// log has to carry the offset - that is what KenshiCoop.map resolves to a function.
void attribute(const void* addr, char* out, size_t cap) {
    HMODULE m = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) || !m) {
        _snprintf(out, cap - 1, "<unmapped>");
        out[cap - 1] = '\0';
        return;
    }
    char full[MAX_PATH]; full[0] = '\0';
    GetModuleFileNameA(m, full, sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';
    const char* leaf = full;
    for (const char* p = full; *p; ++p) if (*p == '\\' || *p == '/') leaf = p + 1;
    _snprintf(out, cap - 1, "%s+0x%llx", leaf,
              (unsigned long long)((const char*)addr - (const char*)m));
    out[cap - 1] = '\0';
}

// ---- C++ throw decoding -------------------------------------------------------
// A throw record carries ExceptionInformation = { magic, thrown object, ThrowInfo,
// image base }, and ThrowInfo's internal links are RVAs against that base. Walking
// ThrowInfo -> CatchableTypeArray -> first CatchableType -> TypeDescriptor yields
// the mangled type name, which is the single most useful word about a throw: it is
// what told us the 2026-08-03 killer came from msvcr100 rather than MyGUI or Ogre.
// Every read is inside SEH because the whole point is that process state is
// suspect.
struct ThrowInfoRec         { int attributes; int unwind; int forwardCompat; int types; };
struct CatchableTypeArrayRec { int count; int types[1]; };
struct CatchableTypeRec      { int properties; int descriptor; };

bool cppTypeName(const EXCEPTION_RECORD* er, char* out, size_t cap) {
    __try {
        if (er->NumberParameters < 4 || er->ExceptionInformation[0] != CPP_MAGIC)
            return false;
        const char* base = (const char*)er->ExceptionInformation[3];
        const ThrowInfoRec* ti = (const ThrowInfoRec*)er->ExceptionInformation[2];
        if (!base || !ti || !ti->types) return false;
        const CatchableTypeArrayRec* cta =
            (const CatchableTypeArrayRec*)(base + ti->types);
        if (cta->count < 1) return false;
        const CatchableTypeRec* ct = (const CatchableTypeRec*)(base + cta->types[0]);
        // TypeDescriptor is { void* vftable; void* spare; char name[] } - so the
        // mangled name (".?AVlength_error@std@@") starts 16 bytes in. Left mangled
        // on purpose: undecorating means dbghelp, and dbghelp is what this tracer
        // exists to avoid calling from inside a fault.
        const char* nm = (const char*)(base + ct->descriptor) + 16;
        size_t i = 0;
        for (; i + 1 < cap; ++i) {
            char ch = nm[i];
            if (!ch) break;
            if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) return false;
            out[i] = ch;
        }
        out[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The thrown object's what(), when it happens to derive from std::exception: MSVC's
// layout there is { vptr; const char* _Mywhat; bool _Mydofree }. There is no way to
// verify the base class from here, so this is a guess validated by its result - a
// non-printable or unterminated answer means the object was something else and we
// say nothing rather than print rubbish.
bool cppWhat(const EXCEPTION_RECORD* er, char* out, size_t cap) {
    __try {
        if (er->NumberParameters < 2 || !er->ExceptionInformation[1]) return false;
        const char* const* obj = (const char* const*)er->ExceptionInformation[1];
        const char* w = obj[1];
        if (!w) return false;
        size_t i = 0;
        for (; i + 1 < cap; ++i) {
            char ch = w[i];
            if (!ch) break;
            if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) return false;
            out[i] = ch;
        }
        out[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- fault context: which register, and what bytes actually ran ---------------
// Added 2026-09-07, after a fault this tracer described but nobody could explain.
// Two inputs were missing, and both are cheap to record from inside the handler:
//
//   * WHICH REGISTER held the bad pointer. The READ/WRITE line says what address
//     was touched; it does not say through what. Naming the register turns a
//     shape-match guess into a yes/no question the log answers by itself - the
//     leading candidate for the 2026-09-07 fault predicts a vtable load through
//     rcx == -1, and "rcx+0x0" versus anything else decides it. The near-miss
//     form matters as much as the exact one: a fault at rcx+0x48 is a field load
//     off a struct pointer, which names the offset of the guilty member.
//
//   * THE BYTES THAT ACTUALLY EXECUTED. That fault's RVA was MID-INSTRUCTION in
//     kenshi_x64.exe on disk, under every 64K-aligned base tried, and no base
//     made the frame chain self-consistent - so the disk image is NOT the code
//     that ran (RE_Kenshi rewrites the image at load; it ships MinHook and
//     courgette). Neither the vendored KenshiLib header RVAs nor RE_Kenshi's own
//     1.0.65 RVA tables bridge the gap: under 6% of either lands on a .pdata
//     function start in the installed exe, and no constant delta exists. So a
//     module+RVA alone cannot be symbolized, and the one input that would let
//     the decode and the instruction-boundary test be redone offline is the
//     LOADED bytes around rip. This carries them out in the log.
//
// Cost is ~15 extra lines per fault report, which the existing per-site budget
// already caps at REPORTS_PER_SITE. Both stay inside the handler's rules: no
// allocation, no locks, fixed stack buffers, and every read of possibly-bad
// memory in its own __try, so a wild rip costs one row rather than the report.

// Window around rip. Wide enough that the longest x64 instruction cannot escape
// either end, and that a disassembler has real context to re-sync on.
const int CODE_BACK = 0x40;
const int CODE_FWD  = 0x40;
const int CODE_ROW  = 16;

// Copy one row out of memory that may not be there. Row at a time on purpose: a
// window straddling the end of a mapped page still yields everything up to the
// boundary instead of nothing.
bool readRow(const unsigned char* src, unsigned char* dst) {
    __try {
        for (int i = 0; i < CODE_ROW; ++i) dst[i] = src[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Plain lowercase hex, space separated, nothing else - these rows get pasted
// straight into a byte decoder, so no ASCII column and no half-row marker.
void hexRow(const unsigned char* row, char* out) {
    static const char* HEX = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < CODE_ROW; ++i) {
        out[k++] = HEX[(row[i] >> 4) & 0xf];
        out[k++] = HEX[row[i] & 0xf];
        out[k++] = ' ';
    }
    out[k - 1] = '\0';
}

void reportCode(const void* rip) {
    if (!rip) return;
    const unsigned char* start = (const unsigned char*)rip - CODE_BACK;

    char where[MAX_PATH + 32];
    attribute(start, where, sizeof(where));

    char b[MAX_PATH + 256];
    _snprintf(b, sizeof(b) - 1,
              "[crash]   code window %s = rip-0x%x (%d bytes of the LOADED image, "
              "which need not match the file on disk)",
              where, (unsigned)CODE_BACK, (int)(CODE_BACK + CODE_FWD));
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    for (int off = -CODE_BACK; off < CODE_FWD; off += CODE_ROW) {
        const unsigned char* p = (const unsigned char*)rip + off;
        unsigned char row[CODE_ROW];
        char hex[CODE_ROW * 3];
        if (readRow(p, row)) {
            hexRow(row, hex);
        } else {
            _snprintf(hex, sizeof(hex) - 1, "<unreadable>");
            hex[sizeof(hex) - 1] = '\0';
        }
        _snprintf(b, sizeof(b) - 1, "[crash]   code rip%c0x%02x %p  %s",
                  off < 0 ? '-' : '+', (unsigned)(off < 0 ? -off : off), p, hex);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// The integer register file, x64. Ordered rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15
// - the encoding order, not alphabetical, so a ModRM field read out of the code
// window above indexes straight into this list.
void reportRegs(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    char b[512];
    if (!c) return;
    if ((c->ContextFlags & CONTEXT_INTEGER) != CONTEXT_INTEGER) {
        _snprintf(b, sizeof(b) - 1,
                  "[crash]   regs unavailable (contextFlags=0x%08lx)",
                  (unsigned long)c->ContextFlags);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        return;
    }

    static const char* NAMES[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                     "r8","r9","r10","r11","r12","r13","r14","r15" };
    const ULONG_PTR r[16] = { c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp,
                              c->Rsi, c->Rdi, c->R8,  c->R9,  c->R10, c->R11,
                              c->R12, c->R13, c->R14, c->R15 };

    for (int i = 0; i < 16; i += 4) {
        _snprintf(b, sizeof(b) - 1,
                  "[crash]   %-3s=%016llx %-3s=%016llx %-3s=%016llx %-3s=%016llx",
                  NAMES[i + 0], (unsigned long long)r[i + 0],
                  NAMES[i + 1], (unsigned long long)r[i + 1],
                  NAMES[i + 2], (unsigned long long)r[i + 2],
                  NAMES[i + 3], (unsigned long long)r[i + 3]);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
    _snprintf(b, sizeof(b) - 1, "[crash]   rip=%016llx eflags=%08lx",
              (unsigned long long)c->Rip, (unsigned long)c->EFlags);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    // Which register the faulting address came out of. Exact match means the
    // pointer itself was bad; a small positive delta means a field load off a
    // bad base, and the delta IS the member offset.
    if (ep->ExceptionRecord->ExceptionCode != ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2)
        return;
    const ULONG_PTR at = ep->ExceptionRecord->ExceptionInformation[1];
    char hit[256]; hit[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < 16; ++i) {
        const ULONG_PTR d = (ULONG_PTR)(at - r[i]);
        if (d > 0xfff) continue;                 // unsigned: negatives wrap out
        // A near miss only counts from a register that could BE a pointer. Most
        // registers are zero most of the time, so on a low fault address every
        // one of them sits within 0x1000 of it - measured: a planted read of
        // 0x48 named ELEVEN registers, which is a wall of noise hiding the one
        // line that matters. An exact match is always reported (that IS the
        // pointer); a delta is only reported off a register big enough to be an
        // address. When nothing qualifies and the address is itself tiny, the
        // address alone already says "null-ish base + that offset".
        if (d != 0 && r[i] < 0x10000) continue;
        char one[40];
        _snprintf(one, sizeof(one) - 1, "%s%s+0x%x",
                  used ? " " : "", NAMES[i], (unsigned)d);
        one[sizeof(one) - 1] = '\0';
        const size_t l = strlen(one);
        if (used + l + 1 >= sizeof(hit)) break;
        memcpy(hit + used, one, l + 1);
        used += l;
    }
    _snprintf(b, sizeof(b) - 1, "[crash]   fault address held by: %s",
              used ? hit : "<no integer register within 0x1000 - computed or "
                          "spilled operand>");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// A throw, in one line: which type, what it said, and where from. No stack and no
// second line - see REPORTS_PER_THROW for why. Reads as a breadcrumb rather than an
// alarm, because the overwhelming majority of these are caught; the ones that matter
// are identified by being the LAST such line before the log falls silent.
void reportThrow(EXCEPTION_POINTERS* ep) {
    char ty[192]; char wh[192];
    bool haveTy = cppTypeName(ep->ExceptionRecord, ty, sizeof(ty));
    bool haveWh = cppWhat(ep->ExceptionRecord, wh, sizeof(wh));
    // A bare `throw;` re-raises with no ThrowInfo, so there is no type to recover
    // and nothing is wrong - worth saying so, because "<undecodable>" reads like a
    // broken decoder and sent me looking for one.
    bool rethrow = !haveTy && ep->ExceptionRecord->NumberParameters >= 3 &&
                   ep->ExceptionRecord->ExceptionInformation[2] == 0;
    char b[512];
    _snprintf(b, sizeof(b) - 1,
              "[crash] THROW type=%s what=%s thread=%lu (first chance - usually caught)",
              haveTy ? ty : (rethrow ? "<rethrow>" : "<undecodable>"),
              haveWh ? wh : "<none>",
              (unsigned long)GetCurrentThreadId());
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

void report(EXCEPTION_POINTERS* ep, const char* origin) {
    char where[MAX_PATH + 32];
    attribute(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));

    char b[MAX_PATH + 192];
    _snprintf(b, sizeof(b) - 1, "[crash] %s code=0x%08lx addr=%p (%s) thread=%lu",
              origin,
              (unsigned long)ep->ExceptionRecord->ExceptionCode,
              ep->ExceptionRecord->ExceptionAddress,
              where,
              (unsigned long)GetCurrentThreadId());
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    // For an access violation, what was touched and how - which separates "read a
    // freed pointer" from "wrote through a null one".
    if (ep->ExceptionRecord->ExceptionCode == ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        void* at = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        char atWhere[MAX_PATH + 32];
        attribute(at, atWhere, sizeof(atWhere));
        _snprintf(b, sizeof(b) - 1, "[crash]   %s of 0x%p (%s)",
                  kind == 0 ? "READ" : (kind == 1 ? "WRITE" : "EXECUTE"),
                  at, atWhere);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }

    // Registers first, then the code that ran: together they say what the bad
    // pointer was held in and which instruction dereferenced it - the two facts
    // a module+RVA on its own cannot supply. See the block above reportCode.
    reportRegs(ep);
    reportCode(ep->ExceptionRecord->ExceptionAddress);

    // The caller chain, as module+RVA per frame. This is the difference between
    // naming a fault and explaining it: the faulting address alone identified
    // CharBody::_patrol on 2026-08-03, but not who asked it to patrol, so the
    // cause stayed guesswork. Engine frames resolve against the "RVA = 0x..."
    // comments in the vendored KenshiLib headers, ours against KenshiCoop.map -
    // and a chain that passes through KenshiCoop.dll at all is the answer to the
    // only question that really matters, which is whether the crash is ours.
    // A VEH runs ON the faulting thread's stack, so our own handler frames sit
    // directly on top of the frames that faulted.
    if (g_capture) {
        void* fr[28];
        USHORT n = g_capture(0, 28, fr, 0);
        for (USHORT i = 0; i < n; ++i) {
            char w[MAX_PATH + 32];
            attribute(fr[i], w, sizeof(w));
            _snprintf(b, sizeof(b) - 1, "[crash]   #%02u %p (%s)",
                      (unsigned)i, fr[i], w);
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
        }
    }
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Everything that can end the process. Breakpoints, single-steps and the
    // debugger's thread-name exception are not in that set and stay ignored.
    const bool watched = code == ACCESS_VIOLATION || code == CPP_THROW ||
                         hardFatal(code);
    if (watched && InterlockedCompareExchange(&g_busy, 1, 0) == 0) {
        // Which "site" is being budgeted matters more for throws than for faults.
        // Every C++ throw in the process is raised from the same address inside
        // KERNELBASE, so keying on ExceptionAddress would let the engine's routine
        // throws mute ALL throws after three - which is precisely the blindness
        // this change exists to remove. ThrowInfo identifies the thrown TYPE, so
        // budgeting on it means a familiar exception goes quiet while a novel one
        // is always reported.
        const bool thrown = code == CPP_THROW;
        const void* site = ep->ExceptionRecord->ExceptionAddress;
        if (thrown && ep->ExceptionRecord->NumberParameters >= 3)
            site = (const void*)ep->ExceptionRecord->ExceptionInformation[2];
        const int cap = thrown ? REPORTS_PER_THROW : REPORTS_PER_SITE;
        int n = budgetFor(site, cap);
        if (n > 0) {
            if (thrown) reportThrow(ep); else report(ep, "VEH");
            if (n == cap)
                coop::logLine("[crash]   (further reports from this site muted; "
                              "other sites still report)");
        }
        InterlockedExchange(&g_busy, 0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Backstop, for faults that do reach the top of the chain (nothing has yet, but a
// fault on one of our own threads would).
LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedCompareExchange(&g_busy, 1, 0) == 0) {
        // Unbudgeted, and for a throw the type name goes out ALONGSIDE the stack:
        // reaching here means nothing caught it, so this is the report that
        // explains the crash rather than a breadcrumb about routine traffic.
        if (ep->ExceptionRecord->ExceptionCode == CPP_THROW) {
            coop::logLine("[crash] UNCAUGHT C++ exception - this ends the process");
            reportThrow(ep);
        }
        report(ep, "UEF");
        InterlockedExchange(&g_busy, 0);
    }
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install(const char* dir, const char* modeTag) {
    (void)dir; (void)modeTag;   // kept for callers; nothing is written to disk now
    if (g_installed) return;
    g_installed = true;

    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt) g_capture = (CaptureStackFn)GetProcAddress(nt, "RtlCaptureStackBackTrace");

    // First=1 puts us ahead of any other vectored handler, including RE_Kenshi's.
    g_veh  = AddVectoredExceptionHandler(1, &veh);
    g_prev = SetUnhandledExceptionFilter(&filter);

    char b[192];
    _snprintf(b, sizeof(b) - 1,
              "[crash] fault tracer installed (veh=%d, uefChains=%d, stacks=%d, "
              "regs+code=1); watching AV + C++ throws + hard-fatal codes; dumps "
              "come from tools\\_dumplive.ps1, not from inside the fault",
              g_veh ? 1 : 0, g_prev ? 1 : 0, g_capture ? 1 : 0);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

} // namespace crashdump
} // namespace coop
