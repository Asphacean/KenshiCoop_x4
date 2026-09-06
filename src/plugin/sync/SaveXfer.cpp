// SaveXfer implementation. See SaveXfer.h. Built for the VS2010 (v100) toolchain.

#define _CRT_SECURE_NO_WARNINGS 1

#include "SaveXfer.h"
#include "../CoopLog.h"
#ifndef KENSHICOOP_PROTOTEST
#include "../game/Engine.h" // engine::saveInfo (runtime save-path resolution)
#include "../net/NetLink.h"
#endif
#include "../../netproto/ContentHash.h" // fnv1aInit/Update (per-file CRC)

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace coop {
namespace savexfer {
namespace {

const unsigned long WATCH_POLL_MS      = 250;   // inventory poll cadence
const unsigned long WATCH_SETTLE_MS    = 1500;  // stable-for window = complete
const unsigned long WATCH_CHANGE_MS    = 30000; // no change within this = timeout
// Protocol 36 (blank-portraits session bug): the engine writes the squad
// portrait atlas (portraits_texture.png) as a late, separate step of the
// save. A folder can look settled - quick.save present, stable 1.5 s -
// before the atlas lands; declaring quiescence then ships/reloads a
// portrait-less save and the squad tab renders blank avatars. Hold the
// settled verdict for the portrait file up to this bound past arm; a save
// that GENUINELY never writes one (no squad portraits yet) still completes
// via the fallback, just later and with a warning.
const unsigned long WATCH_PORTRAIT_MS  = 10000; // bounded portrait-file wait

// Sender pacing: up to SEND_CHUNKS_PER_BURST x 4 KB chunks queued per
// SEND_BURST_MS window (~2.5 MB/s ceiling). save_probe measured the sync
// fixture at 3.7 MB / 35 files -> ~1.5 s in flight.
const unsigned long SEND_BURST_MS         = 50;
const unsigned int  SEND_CHUNKS_PER_BURST = 32;

// Join a folder and a child with exactly one separator.
std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

// Recursive walk helper: accumulate count/bytes/latest-write. Depth-capped so
// a pathological symlink loop cannot hang the main thread.
void walkFolder(const std::string& folder, int depth, unsigned int* files,
                unsigned __int64* bytes, unsigned __int64* latest) {
    if (depth > 4) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pathJoin(folder, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walkFolder(pathJoin(folder, fd.cFileName), depth + 1, files, bytes, latest);
        } else {
            ++*files;
            *bytes += ((unsigned __int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            unsigned __int64 wt =
                ((unsigned __int64)fd.ftLastWriteTime.dwHighDateTime << 32) |
                fd.ftLastWriteTime.dwLowDateTime;
            if (wt > *latest) *latest = wt;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Recursive walk collecting RELATIVE file paths + sizes (the transfer's file
// list). 'prefix' is the accumulated relative path ("" at the root).
struct XferFile { std::string rel; unsigned __int64 size; };
void collectFiles(const std::string& folder, const std::string& prefix, int depth,
                  std::vector<XferFile>* out) {
    if (depth > 4) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pathJoin(folder, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        std::string rel = prefix.empty() ? std::string(fd.cFileName)
                                         : prefix + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collectFiles(pathJoin(folder, fd.cFileName), rel, depth + 1, out);
        } else if (rel.size() <= SAVE_PATH_MAX) {
            XferFile xf;
            xf.rel  = rel;
            xf.size = ((unsigned __int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out->push_back(xf);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Create every intermediate directory of 'fullPath' (a FILE path) that is
// missing. The staging root itself is created by onSaveBegin.
void ensureParentDirs(const std::string& fullPath) {
    for (size_t i = 0; i < fullPath.size(); ++i) {
        if (fullPath[i] == '\\' || fullPath[i] == '/') {
            if (i > 2) CreateDirectoryA(fullPath.substr(0, i).c_str(), 0);
        }
    }
}

// Recursively delete a folder tree (staging cleanup / commit swap). Depth-
// capped like the walkers.
void removeTree(const std::string& folder, int depth) {
    if (depth > 6) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pathJoin(folder, "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' ||
                 (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                continue;
            std::string child = pathJoin(folder, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(child, depth + 1);
            } else {
                SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(folder.c_str());
}

// A rejected path must never escape the staging folder: reject absolute
// paths, drive letters and any ".." component.
// Phase 10 review IN-01: only a FULL ".." path component escapes - two
// adjacent dots INSIDE a filename (e.g. a platoon file derived from a squad
// named "a..b") are harmless, and the sender's collectFiles ships such names
// unfiltered, so rejecting them here just guaranteed a CRC-fail loop for
// that save. Reject exactly the components "\..\" / leading "..\" /
// trailing "\.." / the lone "..".
bool relPathSafe(const char* p, unsigned int len) {
    if (len == 0 || len > SAVE_PATH_MAX) return false;
    if (p[0] == '\\' || p[0] == '/') return false;
    unsigned int compStart = 0;
    for (unsigned int i = 0; i <= len; ++i) {
        if (i == len || p[i] == '\\' || p[i] == '/') {
            if (i - compStart == 2 &&
                p[compStart] == '.' && p[compStart + 1] == '.')
                return false; // a full ".." component escapes staging
            compStart = i + 1;
        } else if (p[i] == ':') {
            return false;
        }
    }
    return true;
}

// ---- Sender state (host, main thread only) ------------------------------------
#ifndef KENSHICOOP_PROTOTEST

bool                  g_sendActive = false;
u32                   g_sendXferId = 0;      // monotonic per-host
std::string           g_sendName;
std::string           g_sendFolder;
std::vector<XferFile> g_sendFiles;
std::vector<u32>      g_sendCrcs;
unsigned int          g_sendFileIdx = 0;
unsigned __int64      g_sendOffset  = 0;     // within the current file
HANDLE                g_sendHandle  = INVALID_HANDLE_VALUE;
u32                   g_sendCurCrc  = 0;
unsigned __int64      g_sendTotalBytes = 0;
unsigned __int64      g_sendSentBytes  = 0;
unsigned long         g_sendStartTick  = 0;
unsigned long         g_sendLastBurst  = 0;
// Phase 10 Plan 01 (SAVE-01/SAVE-04): the destination this transfer streams
// to - OWNER_ID_ALL (the historical behavior) or one specific PlayerId for a
// per-client retry / targeted late-join push. Set once by beginSend and
// carried through every tickSend chunk of the SAME transfer.
u32                   g_sendDestId  = OWNER_ID_ALL;

void sendCloseFile() {
    if (g_sendHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_sendHandle);
        g_sendHandle = INVALID_HANDLE_VALUE;
    }
}

// Phase 11 (11-03): the sender's private snapshot folder for the CURRENT
// transfer ("" = none). See beginSend's snapshot block for why it exists.
std::string g_sendSnapshot;

void sendDropSnapshot() {
    if (g_sendSnapshot.empty()) return;
    removeTree(g_sendSnapshot, 0);
    g_sendSnapshot.clear();
}

void sendAbort(const char* why) {
    char b[192];
    _snprintf(b, sizeof(b) - 1, "[save] XFER-ABORT id=%u %s", g_sendXferId, why);
    b[sizeof(b) - 1] = '\0'; coop::logErrLine(b);
    sendCloseFile();
    sendDropSnapshot();
    g_sendActive = false;
    g_sendFiles.clear();
    g_sendCrcs.clear();
}

#endif // !KENSHICOOP_PROTOTEST (sender state)

// ---- Receiver state (join, main thread only) -----------------------------------

bool             g_recvActive = false;
u32              g_recvXferId = 0;
std::string      g_recvName;
std::string      g_recvStaging;
u16              g_recvFileCount = 0;
unsigned __int64 g_recvTotalBytes = 0;
unsigned __int64 g_recvBytes = 0;
int              g_recvOpenIdx = -1;   // fileIdx of the open handle
HANDLE           g_recvHandle = INVALID_HANDLE_VALUE;
std::vector<u32> g_recvCrcs;           // incremental FNV per fileIdx
std::vector<u8>  g_recvSeen;           // fileIdx touched at least once
unsigned long    g_recvStartTick = 0;

// Scenario gate accessors' backing state.
u32 g_lastSentXferId  = 0;
int g_lastCommitResult = -1;
u32 g_commitSeq        = 0; // bumps on every DONE handled (protocol 32 latch)

void recvCloseFile() {
    if (g_recvHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_recvHandle);
        g_recvHandle = INVALID_HANDLE_VALUE;
    }
    g_recvOpenIdx = -1;
}

// ---- Quiescence watch state (main thread only) --------------------------------

bool          g_watchArmed   = false;
std::string   g_watchFolder;
unsigned long g_watchArmTick = 0;
unsigned long g_watchLastPoll = 0;
bool          g_watchChangeSeen = false;
unsigned long g_watchLastChange = 0;     // tick of the last observed change
unsigned int  g_watchBaseFiles  = 0;     // arm-time inventory (the change baseline)
unsigned __int64 g_watchBaseBytes  = 0;
unsigned __int64 g_watchBaseWrite  = 0;
unsigned int  g_watchCurFiles   = 0;
unsigned __int64 g_watchCurBytes = 0;

} // namespace

#ifdef KENSHICOOP_PROTOTEST
// Prototest seam: redirect the staging/commit root to a caller-owned temp dir
// so the receiver round-trip test never touches the user's real save folder.
static std::string g_testSaveRoot;
void setSaveRootForTest(const std::string& root) { g_testSaveRoot = root; }
#endif

std::string saveFolderFor(const std::string& name) {
    std::string root;
#ifdef KENSHICOOP_PROTOTEST
    if (!g_testSaveRoot.empty()) {
        root = g_testSaveRoot;
    } else {
        const char* lad = getenv("LOCALAPPDATA");
        root = pathJoin(lad ? lad : "", "kenshi\\save");
    }
#else
    char curGame[96], savePath[512];
    if (engine::saveInfo(curGame, sizeof(curGame), savePath, sizeof(savePath)) &&
        savePath[0] != '\0') {
        root = savePath;
    } else {
        const char* lad = getenv("LOCALAPPDATA");
        root = pathJoin(lad ? lad : "", "kenshi\\save");
    }
#endif
    return pathJoin(root, name);
}

bool folderInventory(const std::string& folder, unsigned int* outFiles,
                     unsigned __int64* outBytes, unsigned __int64* outLatestWrite) {
    unsigned int files = 0;
    unsigned __int64 bytes = 0, latest = 0;
    DWORD attrs = GetFileAttributesA(folder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (outFiles) *outFiles = 0;
        if (outBytes) *outBytes = 0;
        if (outLatestWrite) *outLatestWrite = 0;
        return false;
    }
    walkFolder(folder, 0, &files, &bytes, &latest);
    if (outFiles) *outFiles = files;
    if (outBytes) *outBytes = bytes;
    if (outLatestWrite) *outLatestWrite = latest;
    return true;
}

// Path-based fingerprint core (phase 10 review WR-06): folderFingerprint's
// body, taking a FULL folder path instead of a logical save name, so
// onSaveDone can fingerprint the staging dir (a PID-tagged path, not a
// resolvable save name) against a commit-race occupant. 0 = missing/
// unreadable/oversized, same sentinel semantics as folderFingerprint.
static u32 fingerprintFolderPath(const std::string& folder) {
    std::vector<XferFile> files;
    collectFiles(folder, "", 0, &files);
    if (files.empty() || files.size() > 4096) return 0;

    // Per-file content CRC (streamed - same fnv1a the transfer table uses).
    std::vector<unsigned int> crcs(files.size(), 0);
    std::vector<const char*>  paths(files.size(), (const char*)0);
    std::vector<unsigned char> buf(65536);
    for (size_t i = 0; i < files.size(); ++i) {
        paths[i] = files[i].rel.c_str();
        HANDLE h = CreateFileA(pathJoin(folder, files[i].rel).c_str(), GENERIC_READ,
                               FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) return 0; // unreadable = unknown
        unsigned int crc = fnv1aInit();
        DWORD got = 0;
        while (ReadFile(h, &buf[0], (DWORD)buf.size(), &got, 0) && got > 0)
            crc = fnv1aUpdate(crc, &buf[0], (unsigned int)got);
        CloseHandle(h);
        crcs[i] = crc;
    }
    return folderFingerprintOf(&paths[0], &crcs[0], (unsigned int)files.size());
}

u32 folderFingerprint(const std::string& name) {
    return fingerprintFolderPath(saveFolderFor(name));
}

// Phase 11 review WR-01: generalized dead-PID sibling sweep. Phase 10's WR-07
// sweep covered only the receiver's "__incoming_<pid>" staging orphans, but
// Phase 11 added two MORE PID-tagged folder classes to the save root - the
// sender's "__xfersrc_<pid>" snapshot (beginSend) and the commit's
// "__old_<pid>" move-aside (onSaveDone) - with the identical failure mode:
// a crash/hard-kill strands the folder FOREVER (PIDs change every launch, so
// no later run's own-path cleanup ever matches it), and it sits inside the
// save root where Kenshi's load menu lists it as a corrupt/phantom save.
// Sweep any sibling save/<name>__{incoming|xfersrc|old}_<pid> whose tagged
// PID is no longer a live process; a PID that resolves to SOME live process
// (even an unrelated reuse) is conservatively skipped, exactly like WR-07.
//
// "__old_<pid>" gets special handling: it holds the user's PREVIOUS save,
// moved aside between onSaveDone's move-aside and move-in. A crash in that
// window leaves save/<name> MISSING with the only surviving copy stranded
// under the orphan - so when the logical folder is absent, RESTORE the
// orphan over it instead of deleting the user's data; delete only when
// save/<name> exists (the commit completed, the orphan is a stale backup).
// Called from onSaveBegin (receiver) and beginSend (sender), so either
// role's next transfer of the same save self-heals the root.
static void sweepDeadPidSiblings(const std::string& name) {
    static const char* const kTags[] = { "__incoming_", "__xfersrc_", "__old_" };
    for (int ti = 0; ti < 3; ++ti) {
        const bool isOldTag = (ti == 2);
        std::string prefix = name + kTags[ti];
        WIN32_FIND_DATAA fd;
        HANDLE fh = FindFirstFileA(saveFolderFor(prefix + "*").c_str(), &fd);
        if (fh == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (_strnicmp(fd.cFileName, prefix.c_str(), prefix.size()) != 0) continue;
            unsigned long pid = strtoul(fd.cFileName + prefix.size(), 0, 10);
            if (pid == 0 || pid == GetCurrentProcessId()) continue; // own paths handled by their owners
            bool alive = false;
            HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
            if (ph) {
                DWORD ec = 0;
                alive = (GetExitCodeProcess(ph, &ec) != 0) && ec == STILL_ACTIVE;
                CloseHandle(ph);
            } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                alive = true; // exists but not openable (e.g. elevated) - leave it
            }
            if (alive) continue;
            std::string orphan = saveFolderFor(fd.cFileName);
            if (isOldTag &&
                GetFileAttributesA(saveFolderFor(name).c_str()) == INVALID_FILE_ATTRIBUTES) {
                // The crash landed between move-aside and move-in: the orphan
                // IS the user's previous save and save/<name> is gone. Put it
                // back rather than deleting the only copy.
                char rb[704];
                if (MoveFileExA(orphan.c_str(), saveFolderFor(name).c_str(),
                                MOVEFILE_WRITE_THROUGH)) {
                    _snprintf(rb, sizeof(rb) - 1,
                              "[save] XFER restored stranded previous save '%s' "
                              "from dead move-aside orphan '%s' (pid %lu gone)",
                              name.c_str(), orphan.c_str(), pid);
                    rb[sizeof(rb) - 1] = '\0'; coop::logLine(rb);
                } else {
                    _snprintf(rb, sizeof(rb) - 1,
                              "[save] XFER could NOT restore stranded previous save "
                              "from '%s' (pid %lu gone) - left in place",
                              orphan.c_str(), pid);
                    rb[sizeof(rb) - 1] = '\0'; coop::logErrLine(rb);
                }
                continue; // never fall through to delete for the __old_ restore case
            }
            char ob[704];
            _snprintf(ob, sizeof(ob) - 1,
                      "[save] XFER sweeping dead %s orphan '%s' (pid %lu gone)",
                      isOldTag ? "move-aside" : (ti == 1 ? "snapshot" : "staging"),
                      orphan.c_str(), pid);
            ob[sizeof(ob) - 1] = '\0'; coop::logLine(ob);
            removeTree(orphan, 0);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
}

void armWatch(const std::string& name) {
    g_watchArmed      = true;
    g_watchFolder     = saveFolderFor(name);
    g_watchArmTick    = GetTickCount();
    g_watchLastPoll   = 0;
    g_watchChangeSeen = false;
    g_watchLastChange = 0;
    g_watchBaseFiles  = 0;
    g_watchBaseBytes  = 0;
    g_watchBaseWrite  = 0;
    g_watchCurFiles   = 0;
    g_watchCurBytes   = 0;
    folderInventory(g_watchFolder, &g_watchBaseFiles, &g_watchBaseBytes,
                    &g_watchBaseWrite);
    char b[640];
    _snprintf(b, sizeof(b) - 1,
              "[save] WATCH armed folder='%s' baseFiles=%u baseBytes=%I64u",
              g_watchFolder.c_str(), g_watchBaseFiles, g_watchBaseBytes);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

bool watching() { return g_watchArmed; }

int tickWatch(unsigned int* outFiles, unsigned __int64* outBytes,
              unsigned long* outWaitedMs) {
    if (!g_watchArmed) return -1;
    unsigned long now = GetTickCount();
    if (outWaitedMs) *outWaitedMs = now - g_watchArmTick;
    if (outFiles) *outFiles = g_watchCurFiles;
    if (outBytes) *outBytes = g_watchCurBytes;
    if (g_watchLastPoll != 0 && now - g_watchLastPoll < WATCH_POLL_MS) return 0;
    g_watchLastPoll = now;

    unsigned int files = 0;
    unsigned __int64 bytes = 0, latest = 0;
    folderInventory(g_watchFolder, &files, &bytes, &latest);
    g_watchCurFiles = files;
    g_watchCurBytes = bytes;
    if (outFiles) *outFiles = files;
    if (outBytes) *outBytes = bytes;

    bool differsFromBase = (files != g_watchBaseFiles) ||
                           (bytes != g_watchBaseBytes) ||
                           (latest > g_watchBaseWrite);
    if (!g_watchChangeSeen) {
        if (differsFromBase) {
            g_watchChangeSeen = true;
            g_watchLastChange = now;
            // Track "still changing" from this new state onward.
            g_watchBaseFiles = files;
            g_watchBaseBytes = bytes;
            g_watchBaseWrite = latest;
        } else if (now - g_watchArmTick >= WATCH_CHANGE_MS) {
            g_watchArmed = false;
            return 2; // never saw the save land - existing content is the save
        }
        return 0;
    }
    if (differsFromBase) {
        g_watchLastChange = now;
        g_watchBaseFiles  = files;
        g_watchBaseBytes  = bytes;
        g_watchBaseWrite  = latest;
        return 0;
    }
    // Stable since the last change: complete once the settle window elapses
    // AND the folder holds a loadable core (quick.save).
    if (now - g_watchLastChange >= WATCH_SETTLE_MS) {
        DWORD qs = GetFileAttributesA(pathJoin(g_watchFolder, "quick.save").c_str());
        if (qs != INVALID_FILE_ATTRIBUTES) {
            // Portrait gate (protocol 36): the atlas is written late; hold a
            // settled-looking folder for it (bounded) so the transferred /
            // reloaded save doesn't blank the squad-tab avatars.
            DWORD pt = GetFileAttributesA(
                pathJoin(g_watchFolder, "portraits_texture.png").c_str());
            if (pt == INVALID_FILE_ATTRIBUTES &&
                now - g_watchArmTick < WATCH_PORTRAIT_MS)
                return 0; // keep watching; any write re-enters the change path
            if (pt == INVALID_FILE_ATTRIBUTES)
                coop::logLine("[save] WARN quiesced WITHOUT portraits_texture.png "
                              "(bounded wait expired; squad avatars may be blank)");
            g_watchArmed = false;
            return 1;
        }
    }
    return 0;
}

// ---- Sender (host) -------------------------------------------------------------
#ifndef KENSHICOOP_PROTOTEST

bool beginSend(NetLink& net, u32 localId, const std::string& name, u32 destId) {
    sendCloseFile();
    g_sendFiles.clear();
    g_sendCrcs.clear();
    g_sendActive = false;
    g_sendDestId = destId;

    // Phase 10 review CR-04: saveFolderFor("") resolves to the save ROOT
    // itself (pathJoin(root, "") = root + "\\"), which EXISTS as a directory
    // - an empty name would snapshot and stream the user's entire save
    // library. The save root must never be a transfer source.
    if (name.empty()) {
        coop::logErrLine("[save] XFER-BEGIN refused: empty save name "
                         "(would resolve to the save ROOT)");
        return false;
    }

    std::string folder = saveFolderFor(name);
    DWORD attrs = GetFileAttributesA(folder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        char b[640];
        _snprintf(b, sizeof(b) - 1, "[save] XFER-BEGIN refused: no folder '%s'",
                  folder.c_str());
        b[sizeof(b) - 1] = '\0'; coop::logErrLine(b);
        return false;
    }
    // Phase 11 review WR-01: the sender-side half of the dead-PID sweep -
    // a crashed sibling's __xfersrc_/__incoming_/__old_ orphans for this
    // name self-heal on the next SEND too, not only on the next receive.
    sweepDeadPidSiblings(name);

    collectFiles(folder, "", 0, &g_sendFiles);
    if (g_sendFiles.empty() || g_sendFiles.size() > 0xFFFF) {
        coop::logErrLine("[save] XFER-BEGIN refused: empty/oversized file list");
        g_sendFiles.clear();
        return false;
    }

    // Phase 11 (11-03 live matrix, runs 20260905_130015/130752_N4): SNAPSHOT
    // the source folder before sending. On a same-machine rig (and the
    // documented two-installs-one-PC setup) every instance shares ONE
    // physical %LOCALAPPDATA%\kenshi\save root, so while this sender streams
    // files from save/<name>/ over multiple seconds, a RECEIVING sibling
    // process that already finished commits its verified copy ONTO that same
    // logical folder (move-aside + move-in, onSaveDone) - the sender's
    // still-open reads then hold handles that fail the sibling's move-aside
    // ("genuinely still occupied" -> XFER-FAILED badCrc=0), or the folder
    // swap lands between this sender's CRC accumulation and its later file
    // reads so the bytes it ships no longer match the manifest it computed
    // (receiver-side badCrc=1) - both observed live, both self-healed only
    // by burning a coordinator retry. Copying the just-collected file list
    // into a private, PID-tagged sibling folder (save/<name>__xfersrc_<pid>)
    // and streaming from THAT makes the send immune to any concurrent
    // activity on the logical folder, and the sender never holds handles
    // inside the folder receivers commit to. The snapshot is a few MB
    // (quiesced save), deleted at XFER-SENT/abort/next beginSend. On a
    // copy failure the send falls back to the live folder - the pre-existing
    // behavior, no new failure mode.
    sendDropSnapshot();
    {
        char snapSuffix[48];
        _snprintf(snapSuffix, sizeof(snapSuffix) - 1, "__xfersrc_%lu",
                  (unsigned long)GetCurrentProcessId());
        snapSuffix[sizeof(snapSuffix) - 1] = '\0';
        std::string snap = folder + snapSuffix;
        removeTree(snap, 0);
        bool snapOk = true;
        for (size_t i = 0; i < g_sendFiles.size(); ++i) {
            std::string s = pathJoin(folder, g_sendFiles[i].rel);
            std::string d = pathJoin(snap, g_sendFiles[i].rel);
            ensureParentDirs(d);
            if (!CopyFileA(s.c_str(), d.c_str(), FALSE)) { snapOk = false; break; }
        }
        if (snapOk) {
            // Re-collect from the snapshot so sizes/offsets describe exactly
            // the frozen bytes that will be read and CRC'd.
            std::vector<XferFile> snapFiles;
            collectFiles(snap, "", 0, &snapFiles);
            if (!snapFiles.empty() && snapFiles.size() == g_sendFiles.size()) {
                g_sendFiles.swap(snapFiles);
                g_sendSnapshot = snap;
                folder = snap;
            } else {
                snapOk = false;
            }
        }
        if (!snapOk) {
            removeTree(snap, 0);
            coop::logLine("[save] XFER-BEGIN snapshot copy failed - sending from the live folder");
        }
    }
    g_sendTotalBytes = 0;
    for (size_t i = 0; i < g_sendFiles.size(); ++i)
        g_sendTotalBytes += g_sendFiles[i].size;

    ++g_sendXferId;
    g_sendName      = name;
    g_sendFolder    = folder;
    g_sendFileIdx   = 0;
    g_sendOffset    = 0;
    g_sendCurCrc    = fnv1aInit();
    g_sendSentBytes = 0;
    g_sendStartTick = GetTickCount();
    g_sendLastBurst = 0;
    g_sendCrcs.assign(g_sendFiles.size(), 0);
    g_sendActive    = true;

    SaveBeginPacket bp;
    memset(&bp, 0, sizeof(bp));
    bp.type    = (u8)PKT_SAVE_BEGIN;
    bp.ownerId = localId;
    bp.xferId  = g_sendXferId;
    strncpy(bp.name, name.c_str(), sizeof(bp.name) - 1);
    bp.fileCount  = (u16)g_sendFiles.size();
    bp.totalBytes = g_sendTotalBytes;
    net.queueSaveBegin(bp, g_sendDestId);

    char b[224];
    _snprintf(b, sizeof(b) - 1,
              "[save] XFER-BEGIN id=%u name='%s' files=%u bytes=%I64u dest=%u",
              g_sendXferId, name.c_str(), (unsigned)g_sendFiles.size(),
              g_sendTotalBytes, (unsigned)g_sendDestId);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return true;
}

bool sending() { return g_sendActive; }

bool tickSend(NetLink& net, u32 localId) {
    if (!g_sendActive) return false;
    unsigned long now = GetTickCount();
    if (g_sendLastBurst != 0 && now - g_sendLastBurst < SEND_BURST_MS) return false;
    g_sendLastBurst = now;

    unsigned char buf[SAVE_CHUNK_MAX];
    for (unsigned int c = 0; c < SEND_CHUNKS_PER_BURST; ++c) {
        if (g_sendFileIdx >= g_sendFiles.size()) break;
        const XferFile& xf = g_sendFiles[g_sendFileIdx];

        if (g_sendHandle == INVALID_HANDLE_VALUE) {
            g_sendHandle = CreateFileA(pathJoin(g_sendFolder, xf.rel).c_str(),
                                       GENERIC_READ, FILE_SHARE_READ, 0,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
            if (g_sendHandle == INVALID_HANDLE_VALUE) {
                sendAbort("open failed (save changed mid-transfer?)");
                return false;
            }
            g_sendOffset = 0;
            g_sendCurCrc = fnv1aInit();
        }

        DWORD want = SAVE_CHUNK_MAX;
        if (xf.size - g_sendOffset < (unsigned __int64)want)
            want = (DWORD)(xf.size - g_sendOffset);
        DWORD got = 0;
        if (want > 0 && (!ReadFile(g_sendHandle, buf, want, &got, 0) || got != want)) {
            sendAbort("read failed (save changed mid-transfer?)");
            return false;
        }
        g_sendCurCrc = fnv1aUpdate(g_sendCurCrc, buf, (unsigned int)got);

        SaveFileHeader fh;
        fh.type    = (u8)PKT_SAVE_FILE;
        fh.ownerId = localId;
        fh.xferId  = g_sendXferId;
        fh.fileIdx = (u16)g_sendFileIdx;
        fh.pathLen = (u16)xf.rel.size();
        fh.offset  = (u32)g_sendOffset;
        fh.dataLen = (u16)got;
        net.queueSaveFile(fh, xf.rel.c_str(), buf, (unsigned int)got, g_sendDestId);

        g_sendOffset    += got;
        g_sendSentBytes += got;
        if (g_sendOffset >= xf.size) {
            g_sendCrcs[g_sendFileIdx] = g_sendCurCrc;
            sendCloseFile();
            ++g_sendFileIdx;
        }
    }

    if (g_sendFileIdx >= g_sendFiles.size()) {
        SaveDoneHeader dh;
        dh.type      = (u8)PKT_SAVE_DONE;
        dh.ownerId   = localId;
        dh.xferId    = g_sendXferId;
        dh.fileCount = (u16)g_sendFiles.size();
        net.queueSaveDone(dh, g_sendCrcs.empty() ? 0 : &g_sendCrcs[0],
                          (unsigned int)g_sendCrcs.size(), g_sendDestId);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "[save] XFER-SENT id=%u files=%u bytes=%I64u ms=%lu",
                  g_sendXferId, (unsigned)g_sendFiles.size(), g_sendSentBytes,
                  GetTickCount() - g_sendStartTick);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        g_lastSentXferId = g_sendXferId;
        g_sendActive = false;
        g_sendFiles.clear();
        sendDropSnapshot(); // Phase 11: the frozen source has served its purpose
        return true;
    }
    return false;
}

#endif // !KENSHICOOP_PROTOTEST (sender)

u32 lastSentXferId()  { return g_lastSentXferId; }
#ifndef KENSHICOOP_PROTOTEST
u32 sendXferId()      { return g_sendXferId; }
unsigned __int64 sendTotalBytes() { return g_sendTotalBytes; }
#else
u32 sendXferId()      { return 0; } // sender state is compiled out under KENSHICOOP_PROTOTEST
unsigned __int64 sendTotalBytes() { return 0; }
#endif
int lastCommitResult() { return g_lastCommitResult; }
u32 commitSeq()        { return g_commitSeq; }
std::string lastCommitName() { return g_recvName; }

// Receive progress (join, for the F2 loading indicator).
bool             receiving()      { return g_recvActive; }
unsigned __int64 recvBytes()      { return g_recvBytes; }
unsigned __int64 recvTotalBytes() { return g_recvTotalBytes; }
u16              recvFileCount()  { return g_recvFileCount; }

static u32 g_lastAckXferId = 0;
static int g_lastAckOk     = -1;
void noteAck(u32 xferId, int ok) { g_lastAckXferId = xferId; g_lastAckOk = ok; }
u32  lastAckXferId() { return g_lastAckXferId; }
int  lastAckOk()     { return g_lastAckOk; }

#ifndef KENSHICOOP_PROTOTEST
void abortAll() {
    if (g_watchArmed) {
        g_watchArmed = false;
        coop::logLine("[save] WATCH disarmed (superseded by coordinated load)");
    }
    if (g_sendActive) sendAbort("superseded by coordinated load");
}
#endif

// ---- Receiver (join) -------------------------------------------------------------

void onSaveBegin(const SaveBeginPacket& b) {
    recvCloseFile();
    char name[sizeof(b.name) + 1];
    memcpy(name, b.name, sizeof(b.name));
    name[sizeof(b.name)] = '\0';

    // Phase 10 review CR-04 (receiver half): an empty name would stage as
    // "__incoming_<pid>" and COMMIT over saveFolderFor("") - the save ROOT
    // itself (onSaveDone's MoveFileExA would transiently rename the user's
    // ENTIRE save library to "save__old"). Refuse outright; the ACK ok=0
    // path (onSaveDone's !g_recvActive early-out) reports the failure.
    if (!name[0]) {
        coop::logErrLine("[save] XFER-RECV refused: empty save name "
                         "(the save ROOT is never a transfer target)");
        g_recvActive = false;
        g_recvXferId = b.xferId; // stale-chunk guard still keys off the id
        return;
    }

    g_recvName       = name;
    g_recvXferId     = b.xferId;
    g_recvFileCount  = b.fileCount;
    g_recvTotalBytes = b.totalBytes;
    g_recvBytes      = 0;
    g_recvStartTick  = GetTickCount();
    // Staging folder is PID-tagged (not just name-derived): a coordinated
    // save (Phase 10 Plan 01, SAVE-01) can push the SAME name to MULTIPLE
    // already-connected clients concurrently. On separate real machines
    // each has its own filesystem and this would never collide, but the
    // N=4 dev rig runs every clone as a separate process under the SAME
    // Windows user, so run_test4.ps1's own restore step already proved
    // Kenshi/RE_Kenshi reads/writes the ONE shared %LOCALAPPDATA%\kenshi\
    // save regardless of installDir (the "User save location=1" A3 finding
    // does not hold in practice). Without a per-process staging path, two
    // or three receivers opening CreateFileA(..., dwShareMode=0) against
    // the IDENTICAL staging file at the same instant hand every loser an
    // INVALID_HANDLE_VALUE ("[save] XFER chunk write-open FAILED") for the
    // life of the transfer - a real, reproducible blocking failure (10-04
    // live gate run 1), not a false-negative oracle/timing artifact. The
    // FINAL commit directory (finalDir below, saveFolderFor(g_recvName))
    // is deliberately left untouched - onSaveDone's MoveFileExA and the
    // MATCH-path engine::loadSave(name) callers both still resolve the
    // plain logical name, so nothing downstream needs to know about this
    // tag; only the transient staging area needs to be collision-safe.
    char pidTag[24];
    _snprintf(pidTag, sizeof(pidTag) - 1, "__incoming_%lu",
              (unsigned long)GetCurrentProcessId());
    pidTag[sizeof(pidTag) - 1] = '\0';
    g_recvStaging    = saveFolderFor(g_recvName + pidTag);
    g_recvCrcs.assign(b.fileCount, fnv1aInit());
    g_recvSeen.assign(b.fileCount, 0);

    // Phase 10 review WR-07 / Phase 11 review WR-01: sweep DEAD siblings'
    // PID-tagged orphans for this save name - staging (__incoming_<pid>),
    // sender snapshot (__xfersrc_<pid>) and commit move-aside (__old_<pid>)
    // alike; a stranded __old_<pid> whose logical folder is missing is
    // RESTORED, not deleted. See sweepDeadPidSiblings above for the full
    // rationale (the old inline sweep matched only __incoming_).
    sweepDeadPidSiblings(g_recvName);

    // A fresh staging folder: stale partials from an aborted transfer would
    // otherwise pollute the CRC verify.
    removeTree(g_recvStaging, 0);
    ensureParentDirs(pathJoin(g_recvStaging, "x")); // save root may not exist yet
    g_recvActive = (CreateDirectoryA(g_recvStaging.c_str(), 0) != 0) ||
                   (GetLastError() == ERROR_ALREADY_EXISTS);

    char lb[704];
    _snprintf(lb, sizeof(lb) - 1,
              "[save] XFER-RECV id=%u name='%s' files=%u bytes=%I64u staging='%s'%s",
              b.xferId, name, (unsigned)b.fileCount, b.totalBytes,
              g_recvStaging.c_str(), g_recvActive ? "" : " STAGING-FAILED");
    lb[sizeof(lb) - 1] = '\0'; coop::logLine(lb);
}

void onSaveFile(const SaveFileHeader& h, const char* path, const unsigned char* data) {
    if (!g_recvActive || h.xferId != g_recvXferId) return; // stale/aborted transfer
    if (h.fileIdx >= g_recvFileCount) return;
    if (!relPathSafe(path, h.pathLen)) {
        coop::logErrLine("[save] XFER chunk rejected: unsafe relative path");
        return;
    }

    if (g_recvOpenIdx != (int)h.fileIdx) {
        recvCloseFile();
        std::string rel(path, path + h.pathLen);
        std::string full = pathJoin(g_recvStaging, rel);
        ensureParentDirs(full);
        // CREATE_ALWAYS on the file's first chunk; OPEN_EXISTING when a later
        // chunk re-opens it (only happens after an interleave, which the
        // ordered channel + sequential sender never produce - belt/braces).
        g_recvHandle = CreateFileA(full.c_str(), GENERIC_WRITE, 0, 0,
                                   g_recvSeen[h.fileIdx] ? OPEN_EXISTING : CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, 0);
        if (g_recvHandle == INVALID_HANDLE_VALUE) {
            coop::logErrLine("[save] XFER chunk write-open FAILED");
            return;
        }
        g_recvOpenIdx = (int)h.fileIdx;
        g_recvSeen[h.fileIdx] = 1;
    }

    LONG hi = 0;
    SetFilePointer(g_recvHandle, (LONG)h.offset, &hi, FILE_BEGIN);
    DWORD wrote = 0;
    if (h.dataLen > 0) {
        if (!WriteFile(g_recvHandle, data, h.dataLen, &wrote, 0) || wrote != h.dataLen) {
            coop::logErrLine("[save] XFER chunk write FAILED");
            return;
        }
        g_recvCrcs[h.fileIdx] = fnv1aUpdate(g_recvCrcs[h.fileIdx], data, h.dataLen);
        g_recvBytes += h.dataLen;
    }
}

int onSaveDone(const SaveDoneHeader& d, const u32* crcs,
               u16* outFiles, unsigned __int64* outBytes) {
    if (outFiles) *outFiles = 0;
    if (outBytes) *outBytes = 0;
    if (!g_recvActive || d.xferId != g_recvXferId) return 0;
    recvCloseFile();
    g_recvActive = false;

    bool ok = (d.fileCount == g_recvFileCount);
    // Phase 10 review CR-04 (belt/braces): never commit toward the save ROOT
    // - onSaveBegin already refuses an empty name, but a commit with
    // g_recvName empty would MoveFileExA the whole save library aside.
    if (g_recvName.empty()) ok = false;
    unsigned int bad = 0;
    if (ok) {
        for (u16 i = 0; i < d.fileCount; ++i) {
            if (!g_recvSeen[i] || g_recvCrcs[i] != crcs[i]) { ++bad; ok = false; }
        }
    }

    if (ok) {
        // Commit: swap the staged folder over save/<name>/ - the previous
        // save is only removed AFTER the new one is in place.
        //
        // finalDir/oldDir are the LOGICAL (unchanged, non-PID-tagged) names
        // by design - engine::loadSave(name)'s MATCH-arm callers (Plugin.cpp)
        // resolve a save by this exact literal name, so only the transient
        // staging folder above could be made process-unique. On the N=4 dev
        // rig this means finalDir/oldDir are the ONE shared physical target
        // several receiving processes commit into concurrently for the SAME
        // xferId (all receiving byte-identical host-authored data) - a
        // directory rename race that never happens on separate real
        // machines. MoveFileExA has no MOVEFILE_REPLACE_EXISTING equivalent
        // for directories, so losing that race surfaces as a plain move
        // failure. Rather than treating "someone else already finished this
        // exact commit a moment earlier" as a false DROP (10-04 live gate
        // run 2), verify by EXISTENCE after a failed move: if finalDir is
        // there regardless of who put it there, our own CRC-verified data
        // is redundant, not wrong.
        std::string finalDir = saveFolderFor(g_recvName);
        // Phase 11 (11-03, run 20260905_133536_N4): PID-unique oldDir - the
        // shared "__old" name was itself a rendezvous point for sibling
        // receivers committing the same broadcast transfer on a same-machine
        // rig (one sibling's removeTree racing another's move-aside).
        char oldSuffix[40];
        _snprintf(oldSuffix, sizeof(oldSuffix) - 1, "__old_%lu",
                  (unsigned long)GetCurrentProcessId());
        oldSuffix[sizeof(oldSuffix) - 1] = '\0';
        std::string oldDir = finalDir + oldSuffix;
        removeTree(oldDir, 0);
        bool hadOld = false;
        bool adoptedOccupant = false;
        if (GetFileAttributesA(finalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            hadOld = (MoveFileExA(finalDir.c_str(), oldDir.c_str(),
                                  MOVEFILE_WRITE_THROUGH) != 0);
            if (!hadOld && GetFileAttributesA(finalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                // Phase 11 (11-03): same WR-06 occupant-fingerprint resolution
                // as the move-in branch below, applied to the move-ASIDE
                // failure - on a same-machine rig the occupant blocking our
                // aside is usually a SIBLING receiver's just-committed copy of
                // the SAME CRC-verified broadcast data (or the sibling is
                // still holding handles from its own commit). If the
                // occupant's content fingerprint equals our staged data's,
                // our copy is redundant, not wrong - adopt the occupant
                // instead of failing the transfer (run 20260905_133536_N4:
                // full bytes received, badCrc=0, commit lost this exact
                // race and burned a coordinator retry).
                u32 stagedFp   = fingerprintFolderPath(g_recvStaging);
                u32 occupantFp = fingerprintFolderPath(finalDir);
                if (stagedFp != 0 && stagedFp == occupantFp) {
                    adoptedOccupant = true;
                    removeTree(g_recvStaging, 0);
                    coop::logLine("[save] XFER commit-race occupant IDENTICAL "
                                  "(move-aside blocked) - adopted the sibling's commit");
                } else {
                    ok = false; // genuinely still occupied by DIFFERENT data - not resolvable
                }
            }
        }
        if (adoptedOccupant) {
            // Nothing left to move; fall through to the success bookkeeping.
        } else
        if (ok && !MoveFileExA(g_recvStaging.c_str(), finalDir.c_str(),
                               MOVEFILE_WRITE_THROUGH)) {
            if (GetFileAttributesA(finalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                // Phase 10 review WR-06: "finalDir exists" alone is NOT
                // proof a sibling receiver committed our identical data - in
                // production, anything can recreate save/<name> between the
                // two moves (the join's own engine writing that name with
                // suppression just lifted, an autosave, ...). Declaring the
                // race benign then records SC_COMMITTED for a copy that was
                // never CRC-verified - masked divergence, the exact class
                // the CRC table exists to prevent. Verify the OCCUPANT: the
                // race is only benign when its content fingerprint equals
                // our staged (already CRC-verified) data's.
                u32 stagedFp   = fingerprintFolderPath(g_recvStaging);
                u32 occupantFp = fingerprintFolderPath(finalDir);
                if (stagedFp != 0 && stagedFp == occupantFp) {
                    removeTree(g_recvStaging, 0); // identical concurrent commit won the race
                } else {
                    char fb[192];
                    _snprintf(fb, sizeof(fb) - 1,
                              "[save] XFER commit-race occupant DIVERGED "
                              "(stagedFp=%08x occupantFp=%08x) - not committed",
                              stagedFp, occupantFp);
                    fb[sizeof(fb) - 1] = '\0'; coop::logErrLine(fb);
                    ok = false;
                    if (hadOld) MoveFileExA(oldDir.c_str(), finalDir.c_str(),
                                            MOVEFILE_WRITE_THROUGH); // best-effort restore
                }
            } else {
                ok = false;
                if (hadOld) MoveFileExA(oldDir.c_str(), finalDir.c_str(),
                                        MOVEFILE_WRITE_THROUGH); // restore
            }
        }
        if (ok && hadOld) removeTree(oldDir, 0);
    }
    if (!ok) removeTree(g_recvStaging, 0); // never leave a half-written loadable save

    if (outFiles) *outFiles = ok ? g_recvFileCount : 0;
    if (outBytes) *outBytes = ok ? g_recvBytes : 0;

    char b[192];
    _snprintf(b, sizeof(b) - 1,
              "[save] XFER-%s id=%u name='%s' files=%u bytes=%I64u badCrc=%u ms=%lu",
              ok ? "COMMIT" : "FAILED", d.xferId, g_recvName.c_str(),
              (unsigned)g_recvFileCount, g_recvBytes, bad,
              GetTickCount() - g_recvStartTick);
    b[sizeof(b) - 1] = '\0';
    if (ok) coop::logLine(b); else coop::logErrLine(b);
    g_lastCommitResult = ok ? 1 : 0;
    ++g_commitSeq;
    return ok ? 1 : 0;
}

} // namespace savexfer
} // namespace coop
