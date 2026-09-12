// DatapanelList.h - pure, engine-free predicates over ForgottenGUI's datapanel
// update list (lektor<DatapanelGUI*> guiDatapanels, count at +0x228 / data at
// +0x230 in the loaded image).
//
// WHY THIS EXISTS
// ---------------
// The engine's own list API has no dedupe: ForgottenGUI::createDatapanel already
// appends the freshly built panel to guiDatapanels itself (decoded from the
// LOADED image: `mov [rax+rcx*8],rbx ; inc dword [rbp+0x228]`), and
// ForgottenGUI::addDatapanelToUpdateList is an unconditional push_back with no
// duplicate check and no null check. Calling both puts one pointer in two
// adjacent slots. Nothing heals that on its own: neither ~DatapanelGUI nor
// ~GUIWindow removes an object from the list, and ForgottenGUI::shutDown() -
// reached from ~ForgottenGUI during exit() at process quit - walks every index
// 0..count-1 calling each entry's scalar deleting destructor WITHOUT erasing or
// nulling as it goes, zeroing count only after the walk. So a duplicated pointer
// is deleted at index i and then dereferenced again at index i+1, which reads
// the freed block's vptr and calls through it.
//
// These helpers are deliberately POD-only and take the (stuff, count) pair as
// raw arguments rather than a lektor<>, for three reasons:
//   1. they can then be unit-tested in src/prototest, which links no Kenshi
//      headers at all (the same discipline as core/PeerRoster.h and
//      test/PauseSchedule.h - test the SHIPPED predicate, never a paraphrase);
//   2. the caller keeps ownership of the SEH frame, so a torn read of a live
//      engine lektor cannot escape into code that allocates or unwinds;
//   3. no includes at all, so the Harness/Release plugin build and the CRT-only
//      prototest build compile the identical text.
//
// Threading: pure functions over a caller-supplied snapshot. No locking. See
// EngineUi.cpp's note on why the neighbouring guiScreenLabels scans are also
// deliberately unlocked - an exact-pointer-match test fails SAFE under a torn
// read (garbage answers "not present"), whereas a false "already present" would
// need the torn word to equal the very pointer being asked about.

#ifndef KENSHICOOP_DATAPANEL_LIST_H
#define KENSHICOOP_DATAPANEL_LIST_H

namespace coop {
namespace engine {

// Upper bound on a believable guiDatapanels count. The real list holds a handful
// of panels; anything past this is a torn or garbage read and the array must not
// be walked. Mirrors the 8192 bound labelListHas already applies to
// guiScreenLabels (EngineUi.cpp).
const unsigned int DATAPANEL_LIST_MAX = 8192u;

// Result of one scan. `count` is always the count field as read, even when the
// scan refused to walk, so a garbage count is visible in the log rather than
// silently swallowed.
struct DatapanelListScan {
    unsigned int count;       // guiDatapanels.count as read
    unsigned int occurrences; // slots in [0,count) holding `needle`
    unsigned int firstIndex;  // index of the first occurrence, or count if none
    unsigned int dupPointers; // DISTINCT non-null pointers appearing more than once
    bool         sane;        // data non-null and count within DATAPANEL_LIST_MAX
};

inline void datapanelScanInit(DatapanelListScan* out) {
    if (!out) return;
    out->count       = 0u;
    out->occurrences = 0u;
    out->firstIndex  = 0u;
    out->dupPointers = 0u;
    out->sane        = false;
}

// Is `needle` already in stuff[0,count)? This is the predicate the arm path uses
// to refuse a second registration. Returns false for a null needle, a null
// array, or an implausible count - i.e. it fails SAFE toward "not present",
// which preserves the pre-existing behaviour (register it) whenever the list
// cannot be trusted.
inline bool datapanelListHas(const void* const* stuff, unsigned int count,
                             const void* needle) {
    if (!stuff || !needle || count > DATAPANEL_LIST_MAX) return false;
    for (unsigned int i = 0u; i < count; ++i) {
        if (stuff[i] == needle) return true;
    }
    return false;
}

// Full scan: where `needle` sits and how many times, plus whether ANY pointer in
// the list is duplicated (the condition that makes shutDown() double-delete).
// dupPointers counts DISTINCT offenders, not extra slots, so one pointer present
// three times reports 1.
inline void datapanelListScan(const void* const* stuff, unsigned int count,
                              const void* needle, DatapanelListScan* out) {
    if (!out) return;
    datapanelScanInit(out);
    out->count      = count;
    out->firstIndex = count;
    if (!stuff || count > DATAPANEL_LIST_MAX) return;
    out->sane = true;

    for (unsigned int i = 0u; i < count; ++i) {
        if (needle && stuff[i] == needle) {
            if (out->occurrences == 0u) out->firstIndex = i;
            ++out->occurrences;
        }
    }

    // Distinct duplicated pointers. O(n^2) over a list that holds a handful of
    // entries and is scanned only on rare UI/session edges, never per tick.
    for (unsigned int i = 0u; i < count; ++i) {
        const void* p = stuff[i];
        if (!p) continue;
        bool seenEarlier = false;
        for (unsigned int k = 0u; k < i; ++k) {
            if (stuff[k] == p) { seenEarlier = true; break; }
        }
        if (seenEarlier) continue; // already counted at its first position
        for (unsigned int j = i + 1u; j < count; ++j) {
            if (stuff[j] == p) { ++out->dupPointers; break; }
        }
    }
}

// Should the caller hand `needle` to ForgottenGUI::addDatapanelToUpdateList?
//
// This is THE shipped decision, not a paraphrase of it: EngineUi.cpp's
// uiPanelArmSeh calls exactly this function, so the prototest checks below it
// exercise the code that runs in the game (the discipline core/PeerRoster.h and
// test/PauseSchedule.h already set).
//
// Register only when the list was readable AND does not already hold the panel.
// Both halves of that condition are deliberate:
//   * "does not already hold it" is the fix - createDatapanel has already
//     appended the panel, and addDatapanelToUpdateList is an unconditional
//     push_back, so registering again is the duplicate that makes shutDown()
//     delete the panel twice at process exit.
//   * "was readable" makes the UNTRUSTED case fail toward NOT registering.
//     That is the safe direction under this project's correctness priority
//     order (no crashes/corruption outranks visual smoothness): skipping a
//     redundant registration costs nothing while the engine keeps registering
//     panels itself, whereas adding a second copy is an exit-time double free.
//     Should a future Kenshi build stop self-registering, a readable-and-absent
//     list still takes the add, so the panel keeps refreshing.
inline bool datapanelShouldRegister(const void* const* stuff, unsigned int count,
                                    const void* needle) {
    if (!needle) return false;
    DatapanelListScan s;
    datapanelScanInit(&s);
    datapanelListScan(stuff, count, needle, &s);
    return s.sane && s.occurrences == 0u;
}

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_DATAPANEL_LIST_H
