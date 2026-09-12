// EngineUi.cpp - the in-game co-op UI plane: the debug marker HUD labels, the
// F2 co-op session panel (native DatapanelGUI), and the persistent status
// overlay. Split out of EngineEntity.cpp (Phase 5e code motion, 2026-07-19) so
// the entity capture/resolve/apply TU stays focused on sync and the UI/render
// surface lives with its public header (EngineUi.h).
//
// Owner state: section-private statics / anon-namespace SEH shims only (marker
// colour + create/update/destroy shims, panel widget pointers, overlay label).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them), install
// hooks, or change any log string - "[coop-ui] ..." phrasing is API consumed by
// the harness. The public marker* declarations stay in Engine.h (the Replicator
// uses them for KENSHICOOP_DEBUG_MARKERS); only their definitions moved here.

#include "EngineInternal.h"

// In-game co-op session panel: the native DatapanelGUI window + its interactive
// rows and Win32 key capture. EngineInternal.h already pulls Globals.h (::gui),
// ForgottenGUI.h and MyGUI_Button.h; these add the panel row types, the
// free-function delegate factory, and GetAsyncKeyState/VK_* for the F2 toggle +
// digit entry.
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <windows.h>

#include "../core/SteamId.h" // parseSteamId64 (paste button) + maskSteamId64 (id rows)
#include "DatapanelList.h"   // pure (stuff,count) predicates over guiDatapanels

namespace coop {
namespace engine {

// ---- Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate) --
// ForgottenGUI::createScreenLabel + ScreenLabel::setTracking pin a colored text
// label to a character; the engine's own per-frame projection keeps it on the
// body (spike 47 render proof). The Replicator uses these to make join-side
// authority states self-explaining on screen: who is host-driven, who is
// hidden, who is a local-only ghost. C2712 split: the outer fns build the
// std::string/Colour/Vector3 (unwindable), POD-only inner fns hold the SEH.

namespace {

void markerColour(int colorId, MyGUI::Colour* col) {
    switch (colorId) {
    case 0:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // driven
    case 1:  *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // hidden
    case 2:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // local-only
    default: *col = MyGUI::Colour(0.80f, 0.80f, 0.80f, 1.0f); break;
    }
}

ScreenLabel* markerCreateSeh(ForgottenGUI* g, Character* c,
                             const std::string* text, const MyGUI::Colour* col,
                             const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_SMALL,
                                              ScreenLabel::RS_STOPPED);
        if (l) {
            l->_NV_setRisingSpeed(ScreenLabel::RS_STOPPED);
            l->_NV_setTracking(c->handle, *off);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool markerUpdateSeh(ScreenLabel* l, const std::string* text,
                     const MyGUI::Colour* col) {
    __try {
        l->_NV_setCaption(*text);
        l->_NV_setColor(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Is this label one the GUI still owns? ForgottenGUI keeps the authoritative
// registry of live ScreenLabels (plus deferred add/remove queues), and that
// registry - not our cached pointer - is the only ground truth available: the
// GUI destroys labels on its own schedule and notifies nobody, so a handle held
// across ticks dangles silently. Measured 2026-08-03: the join minted three
// proxies in one burst, debugMark found stale map entries sitting at RECYCLED
// Character* addresses, and setCaption on the previous occupants' dead labels
// faulted twice inside ScreenLabel::setCaption (kenshi+0x6e451e) moments before
// the process died. Same lesson as the world-item proxy hands - never
// dereference a cached engine pointer without re-asking the engine.
//
// Deliberately unlocked. guiScreenLabelsMutex guards these lektors, but taking
// an engine shared_mutex from inside a detour is its own class of hazard, and a
// torn read here is harmless BECAUSE we test for an exact pointer match: garbage
// answers "not present", which costs one discarded marker, whereas a false
// "alive" would need the torn word to equal the very pointer we are asking
// about. The safe direction is the likely one.
bool labelListHas(const lektor<ScreenLabelInterface*>* v, const void* l) {
    __try {
        ScreenLabelInterface* const* p = v->stuff;
        unsigned int n = v->count;
        if (!p || n > 8192u) return false;   // bound a torn/garbage count
        for (unsigned int i = 0; i < n; ++i)
            if ((const void*)p[i] == l) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool markerDestroySeh(ForgottenGUI* g, ScreenLabel* l) {
    __try {
        g->destroy(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void* markerCreate(Character* c, const char* text, int colorId) {
    if (!c || !text) return 0;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return 0;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    Ogre::Vector3 off(0.0f, 2.2f, 0.0f); // head height (spike 47)
    return markerCreateSeh(g, c, &t, &col, &off);
}

bool markerAlive(void* label) {
    if (!label) return false;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return false;
    // Queued for removal counts as dead: the GUI has already decided, and we
    // would only be racing its next flush.
    if (labelListHas(&g->guiScreenLabelsToRemove, label)) return false;
    // A freshly created label sits in the add queue until the GUI flushes it, so
    // both lists are "alive" or markerCreate's own handle would look dead.
    return labelListHas(&g->guiScreenLabels, label) ||
           labelListHas(&g->guiScreenLabelsToAdd, label);
}

bool markerUpdate(void* label, const char* text, int colorId) {
    if (!label || !text || !markerAlive(label)) return false;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    return markerUpdateSeh((ScreenLabel*)label, &t, &col);
}

void markerDestroy(void* label) {
    if (!label) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    // Handing the GUI a label it has already destroyed is the same
    // use-after-free as updating one; prune paths reach here with handles whose
    // Character went away, which is exactly when the GUI has cleaned up too.
    if (!markerAlive(label)) return;
    markerDestroySeh(g, (ScreenLabel*)label);
}

// ---- In-game co-op session panel (config-driven, spike-50 DatapanelGUI stack) -
// A native DatapanelGUI window toggled with F2. The player picks role + transport
// (toggle BUTTONS - the only DatapanelGUI control with a callable RVA callback;
// MyGUI comboboxes/editboxes have no reachable getters and never receive keyboard
// focus during gameplay) and connects/leaves via a bound checkbox. The friend code
// (peer SteamID) + UDP endpoint come from coop_config.json and are shown READ-ONLY;
// a "Copy my Steam ID" button puts the player's own id on the clipboard to share.
// The GUI layer is session-agnostic: live status arrives via *st; the user's
// actions leave via the onConnect/onDisconnect callbacks (the plugin root owns the
// net/session/config wiring).
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they all sit inside one __try, provided NO std::string temporary is
// constructed in that frame. The one exception is createDatapanel's BY-VALUE
// std::string 'layer' arg (an unwindable temporary => C2712), so the window is
// created in the outer, non-SEH function; ::gui is verified non-null first and the
// createScreenLabel/createFloatingLabel factory family is render-proven (46-48).

namespace {

// Write a UTF-8/ANSI string to the Windows clipboard (CF_TEXT). Mirror of the
// paste-read: OpenClipboard -> EmptyClipboard -> GlobalAlloc+copy -> SetClipboardData
// -> CloseClipboard. Used by the "Copy my Steam ID" button. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    size_t n = strlen(text);
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
        if (h) {
            char* dst = (char*)GlobalLock(h);
            if (dst) {
                memcpy(dst, text, n);
                dst[n] = '\0';
                GlobalUnlock(h);
                if (SetClipboardData(CF_TEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out. Prefers CF_UNICODETEXT (what the
// Steam overlay / browsers usually publish) and falls back to CF_TEXT, converting
// either to a narrow std::string (the SteamID parse keeps only ASCII digits, so a
// lossy WideCharToMultiByte is fine here). Used by the "Paste friend's Steam ID"
// button. Win32 only (no MyGUI). Returns true iff some text was retrieved.
bool clipboardGetText(std::string& out) {
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    HANDLE hw = GetClipboardData(CF_UNICODETEXT);
    if (hw) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hw);
        if (src) {
            int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            if (need > 0) {
                std::string tmp((size_t)need, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, src, -1, &tmp[0], need, 0, 0) > 0) {
                    if (!tmp.empty() && tmp[tmp.size() - 1] == '\0') tmp.resize(tmp.size() - 1);
                    out = tmp;
                    ok = true;
                }
            }
            GlobalUnlock(hw);
        }
    }
    if (!ok) {
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (ha) {
            const char* src = (const char*)GlobalLock(ha);
            if (src) { out = src; ok = true; GlobalUnlock(ha); }
        }
    }
    CloseClipboard();
    return ok;
}

struct CoopPanelUi {
    DatapanelGUI* panel;
    bool          open, built;
    bool          hostFlag;      // true = HOST role armed
    bool          steamFlag;     // true = Steam transport armed (else UDP)
    bool          connectedFlag; // desired connection state (Online/Offline toggle)
    bool          lastConnected; // last observed st->running (external-change sync)
    bool          lastChkVal;    // last toggle value (connect/disconnect edge)
    bool          needsRebuild;
    bool          f2Down;        // F2 held last tick (rising-edge toggle)
    std::string   lastStatus;    // last status text shown (refresh gate)
    std::string   lastTransfer;  // last save-transfer line shown (refresh gate)
    CoopPanelUi()
        : panel(0), open(false), built(false), hostFlag(true), steamFlag(true),
          connectedFlag(false), lastConnected(false), lastChkVal(false),
          needsRebuild(false), f2Down(false) {}
};

CoopPanelUi             g_panel;
DataPanelLine_Button*   g_roleBtn      = 0;
DataPanelLine_Button*   g_transBtn     = 0;
DataPanelLine_Button*   g_connBtn      = 0; // Online/Offline toggle (replaces the checkbox)
DataPanelLine_Button*   g_copyIdBtn    = 0;
DataPanelLine_Button*   g_pasteIdBtn   = 0; // "Paste friend's Steam ID" from clipboard
DataPanelLine*          g_debugLine    = 0; // white connection-status debug row
DataPanelLine*          g_peerLine     = 0; // white "Friend's Steam ID" row
DataPanelLine*          g_selfLine     = 0; // white "Your Steam ID" row
std::string             g_selfIdStr;   // self SteamID as digits (set each tick; "" = none)

// Friend's SteamID pasted in-panel this session (0 = none). Per-session by
// design: it lives only in memory, so relaunching Kenshi clears it and the
// friend's id is re-pasted (nothing is written to disk). Passed to onConnect,
// where it overrides the (usually empty) config steamPeer.
unsigned long long      g_pastedPeer   = 0;
bool                    g_pasteFailed  = false; // last paste wasn't a valid Steam ID

// Button callbacks (free functions - MyGUI::newDelegate wraps them without any
// raw-MyGUI link). A press flips the armed flag and requests a rebuild so the
// caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
}
void onTransBtn(DataPanelLine*) {
    g_panel.steamFlag = !g_panel.steamFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.steamFlag ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
}
// Online/Offline toggle: flip the desired connection state. The connect/disconnect
// edge (connectedFlag vs lastChkVal) is handled in coopPanelTick, same as before.
void onConnBtn(DataPanelLine*) {
    g_panel.connectedFlag = !g_panel.connectedFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.connectedFlag ? "[coop-ui] connection -> ONLINE"
                                        : "[coop-ui] connection -> OFFLINE");
}
// Copy the player's own SteamID to the clipboard so they can paste it to a friend
// (who pastes it into their panel via "Paste friend's Steam ID").
void onCopyIdBtn(DataPanelLine*) {
    if (g_selfIdStr.empty()) {
        coop::logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] copied Steam ID to clipboard: %s",
              ok ? "ok" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
// Paste the friend's SteamID from the clipboard: read text, extract + validate a
// SteamID64, and store it as the session peer (used on the next Connect). No
// typing, no config edit. Rejects arbitrary clipboard junk (g_pasteFailed drives
// the peer-row hint).
void onPasteIdBtn(DataPanelLine*) {
    std::string clip;
    unsigned long long id = 0;
    if (clipboardGetText(clip) && coop::parseSteamId64(clip, id)) {
        g_pastedPeer  = id;
        g_pasteFailed = false;
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste friend id=%llu ok=1", id);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    } else {
        g_pasteFailed = true;
        coop::logLine("[coop-ui] paste friend id=0 ok=0 (clipboard not a Steam ID)");
    }
    g_panel.needsRebuild = true;
}

// POD-only pointer bundle so the row-build SEH frame constructs no std::string.
struct PanelStrings {
    const std::string *title, *roleKey, *roleCap, *transKey, *transCap;
    const std::string *connKey, *connCap;
    const std::string *dbgKey, *dbgVal;
    const std::string *peerKey, *peerVal, *pasteKey, *pasteCap;
    const std::string *selfKey, *selfVal, *copyKey, *copyCap;
    const std::string *empty;
};

void panelBuildSeh(DatapanelGUI* p, const PanelStrings* s) {
    __try {
        p->_NV_clear();
        p->setCaption(*s->title);
        g_roleBtn  = p->setLineButton(*s->roleKey,  *s->roleCap,  0);
        g_transBtn = p->setLineButton(*s->transKey, *s->transCap, 0);
        g_connBtn  = p->setLineButton(*s->connKey,  *s->connCap,  0);
        p->addSpace(0, 0.35f);
        // Connection-status debug line (coloured white below, outside SEH).
        g_debugLine = p->setLine(*s->dbgKey, *s->dbgVal, *s->empty, 0, false, true);
        p->addSpace(0, 0.35f);
        // Friend's SteamID: pasted in-panel (Copy on their side -> Paste here).
        g_peerLine = p->setLine(*s->peerKey, *s->peerVal, *s->empty, 0, false, true);
        g_pasteIdBtn = p->setLineButton(*s->pasteKey, *s->pasteCap, 0);
        p->addSpace(0, 0.35f);
        g_selfLine = p->setLine(*s->selfKey, *s->selfVal, *s->empty, 0, false, true);
        g_copyIdBtn = p->setLineButton(*s->copyKey, *s->copyCap, 0);
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Colour a line's key + value TextBoxes for readability. Runs AFTER
// panelBuildSeh's _NV_update (the w1/w2 widgets exist by then). MyGUI::Colour is a
// trivial 4-float struct (no destructor), so it may live in the SEH frame.
// yellow=true tints the value column amber - used for the join's live
// "Streaming host world..." transfer line so it reads as in-progress activity.
void dbgColourSeh(DataPanelLine* line, bool yellow) {
    if (!line) return;
    __try {
        MyGUI::Colour white(1.0f, 1.0f, 1.0f, 1.0f);
        MyGUI::Colour amber(1.0f, 0.82f, 0.20f, 1.0f);
        if (line->w1) line->w1->setTextColour(white);
        if (line->w2) line->w2->setTextColour(yellow ? amber : white);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- guiDatapanels observability -------------------------------------------
// Read ForgottenGUI's datapanel update list and report where a given panel sits
// in it. This exists because that list is the crash surface: shutDown() walks
// every index calling each entry's deleting destructor without erasing as it
// goes, so a pointer present twice is deleted twice. See DatapanelList.h.
//
// The lektor members are read through the vendored header (guiDatapanels.count /
// .stuff), never through hard-coded offsets, and the walk itself lives in the
// pure helper so the shipped predicate is the one prototest exercises. Same
// unlocked-but-fail-safe discipline as labelListHas above.
bool datapanelSnapshotSeh(ForgottenGUI* g, const void* needle,
                          DatapanelListScan* out) {
    __try {
        const lektor<DatapanelGUI*>* v = &g->guiDatapanels;
        datapanelListScan((const void* const*)v->stuff, v->count, needle, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One log line per rare UI/session edge - never per tick. `panel` is used ONLY
// as a comparison needle and is never dereferenced, so it is safe to pass a
// pointer that has just been freed (which is exactly the "is the dead panel
// still listed?" question the close path needs answered).
void datapanelProbeLog(const char* where, ForgottenGUI* g, const void* panel) {
    DatapanelListScan s;
    datapanelScanInit(&s);
    bool read = (g != 0) && datapanelSnapshotSeh(g, panel, &s);
    char b[224];
    _snprintf(b, sizeof(b) - 1,
              "[coop-ui] datapanels %s: read=%d sane=%d count=%u panel=%p "
              "occurrences=%u firstIdx=%u dupPointers=%u",
              where ? where : "?", read ? 1 : 0, s.sane ? 1 : 0, s.count,
              panel, s.occurrences, s.firstIndex, s.dupPointers);
    b[sizeof(b) - 1] = '\0';
    // A duplicate is the defect this probe exists to catch, so it goes to the
    // error stream where a log scan cannot miss it.
    if (s.occurrences > 1u || s.dupPointers > 0u) coop::logErrLine(b);
    else                                          coop::logLine(b);
}

// Arm a freshly-minted panel: make sure it is registered for ForgottenGUI's
// per-frame refresh, and make it visible. createDatapanel returns a
// built-but-hidden window; without the show the F2 toggle logs open/close yet
// nothing ever draws (the render bug in the reconstruction).
//
// THE REGISTRATION IS GUARDED, and that guard is a crash fix, not a
// micro-optimisation. ForgottenGUI::createDatapanel ALREADY appends the panel to
// guiDatapanels itself, and addDatapanelToUpdateList is an unconditional
// push_back with no dedupe, so the unguarded pair put one pointer in two
// adjacent slots. Nothing heals that while the panel stays open: no destructor
// unregisters, and ForgottenGUI::shutDown() - reached from ~ForgottenGUI during
// exit() at process quit - walks every index calling each entry's deleting
// destructor without erasing as it goes. The panel was therefore deleted at its
// first slot and dereferenced again at its second, faulting on the freed vptr.
// Measured on this machine, one process, title screen: panel left open at quit
// => 0xc0000005 at kenshi_x64.exe+0x6ea9ab with rdi=3 (the duplicate slot) and
// rcx = the panel - the same RVA and the same frames as the two field crashes.
//
// datapanelShouldRegister keeps the call rather than deleting it so that a
// future Kenshi build which stops self-registering still gets the panel
// registered; see DatapanelList.h for why an unreadable list fails toward NOT
// registering. `outAdded` reports the branch taken so the caller can log it
// without doing string work inside the SEH frame.
// PODs only, so the whole thing sits in one SEH frame.
bool uiPanelArmSeh(ForgottenGUI* g, DatapanelGUI* p, bool* outAdded) {
    if (outAdded) *outAdded = false;
    if (!g || !p) return false;
    __try {
        const lektor<DatapanelGUI*>* v = &g->guiDatapanels;
        if (datapanelShouldRegister((const void* const*)v->stuff, v->count, p)) {
            g->addDatapanelToUpdateList(p);
            if (outAdded) *outAdded = true;
        }
        p->_NV_show(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void panelDestroySeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return;
    // Pull it off the refresh list BEFORE destroying so ForgottenGUI never
    // dereferences the freed panel on the next frame.
    __try {
        g->removeDatapanelFromUpdateList(p);
        g->destroy(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

void datapanelListProbe(const char* where) {
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    datapanelProbeLog(where, g, g_panel.panel);
}

void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect) {
    if (!st) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); } }
    if (!g) return;

    // Cache the self id as a string for the Copy button (used by onCopyIdBtn).
    if (st->selfSteamId) {
        char b[32];
        _snprintf(b, sizeof(b) - 1, "%llu", (unsigned long long)st->selfSteamId);
        b[sizeof(b) - 1] = '\0';
        g_selfIdStr = b;
    } else {
        g_selfIdStr.clear();
    }

    // F2 rising edge toggles the panel open/closed.
    bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !g_panel.f2Down) {
        if (!g_panel.open) {
            g_panel.hostFlag      = st->isHost;
            g_panel.steamFlag     = (st->transportSel == 0);
            g_panel.connectedFlag = st->running;
            g_panel.lastConnected = st->running;
            g_panel.lastChkVal    = st->running;
            g_panel.open = true;
            g_panel.needsRebuild = true;
            coop::logLine("[coop-ui] panel opened");
        } else {
            datapanelProbeLog("f2-close-before", g, g_panel.panel);
            panelDestroySeh(g, g_panel.panel);
            // g_panel.panel is FREED from here on. It is passed to the probe as
            // a comparison needle only (never dereferenced) to answer the one
            // question the close path must get right: is the dead panel - in
            // either of its slots - gone from guiDatapanels?
            datapanelProbeLog("f2-close-after", g, g_panel.panel);
            g_panel.panel = 0; g_panel.built = false;
            g_roleBtn = 0; g_transBtn = 0; g_connBtn = 0; g_copyIdBtn = 0;
            g_pasteIdBtn = 0;
            g_debugLine = 0; g_peerLine = 0; g_selfLine = 0;
            g_panel.open = false;
            coop::logLine("[coop-ui] panel closed");
        }
    }
    g_panel.f2Down = f2;

    if (!g_panel.open) return;

    // Keep the Online/Offline toggle honest when the session state changes
    // underneath us (a peer-driven connect, a failed connect that stopped, etc):
    // resync the desired flag to the real state and rebuild so the button caption
    // + debug line reflect it.
    if (st->running != g_panel.lastConnected) {
        g_panel.lastConnected = st->running;
        g_panel.connectedFlag = st->running;
        g_panel.lastChkVal    = st->running;
        g_panel.needsRebuild = true;
    }

    std::string detail = st->detail ? std::string(st->detail) : std::string();
    if (detail != g_panel.lastStatus) g_panel.needsRebuild = true;

    // Join save-transfer line (throttled to whole-percent by the caller): rebuild
    // when it changes so the "Streaming host world... NN%" line advances live.
    std::string transfer = st->transferDetail ? std::string(st->transferDetail)
                                               : std::string();
    if (transfer != g_panel.lastTransfer) g_panel.needsRebuild = true;

    // Create the window once (outside SEH - see the header note on C2712).
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    if (!g_panel.panel) {
        std::string layer = "Info";
        g_panel.panel = g->createDatapanel(0.22f, 0.30f, 0.30f, 0.44f, false, layer, true);
        g_panel.built = false;
        if (!g_panel.panel) {
            coop::logErrLine("[coop-ui] createDatapanel FAILED");
        } else {
            // The decisive pair. createDatapanel is believed to register the
            // panel in guiDatapanels itself, so "after-create" should already
            // report occurrences=1; if the arm step then appends it again,
            // "after-arm" reports occurrences=2 and the list is primed to
            // double-delete at exit-time teardown.
            datapanelProbeLog("after-create", g, g_panel.panel);
            bool armAdded = false;
            if (!uiPanelArmSeh(g, g_panel.panel, &armAdded))
                coop::logErrLine("[coop-ui] panel arm (update-list/show) FAILED");
            coop::logLine(armAdded
                ? "[coop-ui] panel arm: registered in guiDatapanels"
                : "[coop-ui] panel arm: already listed by createDatapanel - add skipped");
            datapanelProbeLog("after-arm", g, g_panel.panel);
        }
    }

    // (Re)populate the rows when anything visible changed.
    if (g_panel.panel && (g_panel.needsRebuild || !g_panel.built)) {
        std::string title    = "Co-op Session    -    F2 to close";
        std::string roleKey  = "role";
        std::string roleCap  = std::string("Role: ") + (g_panel.hostFlag ? "HOST" : "JOIN") + "    (switch)";
        std::string transKey = "trans";
        std::string transCap = std::string("Transport: ") + (g_panel.steamFlag ? "STEAM" : "UDP") + "    (switch)";
        std::string connKey  = "conn";
        std::string connCap  = std::string("Connection: ") + (g_panel.connectedFlag ? "ONLINE" : "OFFLINE") + "    (switch)";

        // White debug line: describes the live connection state + type. Reflects
        // the ACTUAL running session when online; the armed toggles when offline.
        std::string transStr = (st->transportSel == 0) ? "Steam" : "UDP";
        std::string dbgKey   = "Connection status";
        std::string dbgVal;
        if (st->running) {
            if (st->peerPresent)
                dbgVal = (st->isHost ? std::string("Hosting") : std::string("Joining")) +
                         " over " + transStr + " - peer connected";
            else if (st->isHost)
                dbgVal = std::string("Hosting over ") + transStr + " - waiting for peer...";
            else
                dbgVal = std::string("Joining over ") + transStr + " - connecting to host...";
        } else {
            dbgVal = std::string("Offline - will ") + (g_panel.hostFlag ? "host" : "join") +
                     " over " + (g_panel.steamFlag ? "Steam" : "UDP") + " on Connect";
        }
        // A join streaming the host's world at the menu has no leader for the
        // screen overlay, so surface the live progress here instead (amber).
        if (!transfer.empty()) { dbgVal = transfer; dbgKey = "World transfer"; }

        // Friend's SteamID: prefer the value pasted in-panel this session; fall
        // back to the config (steamPeer, mainly for advanced/back-compat use).
        // Both id rows show only the last 4 digits - the panel is often on screen
        // while streaming. Nothing needs the full digits by eye: Copy puts the
        // real id on the clipboard and Paste takes it back off.
        std::string peerKey = "Friend's Steam ID";
        std::string peerVal;
        unsigned long long peerShown = g_pastedPeer ? g_pastedPeer
                                                     : (unsigned long long)st->peerSteamId;
        if (peerShown != 0) {
            peerVal = coop::maskSteamId64(peerShown);
        } else if (g_pasteFailed) {
            peerVal = "(clipboard was not a Steam ID - copy theirs and retry)";
        } else {
            peerVal = "(click Paste friend's Steam ID)";
        }
        std::string pasteKey = "pasteid";
        std::string pasteCap = "Paste friend's Steam ID";

        std::string selfKey  = "Your Steam ID";
        std::string selfVal  = st->selfSteamId
                                   ? coop::maskSteamId64((unsigned long long)st->selfSteamId)
                                   : std::string("(Steam not running)");
        std::string copyKey  = "copyid";
        std::string copyCap  = "Copy my Steam ID";
        std::string empty    = "";

        PanelStrings ps;
        ps.title = &title; ps.roleKey = &roleKey; ps.roleCap = &roleCap;
        ps.transKey = &transKey; ps.transCap = &transCap;
        ps.connKey = &connKey; ps.connCap = &connCap;
        ps.dbgKey = &dbgKey; ps.dbgVal = &dbgVal;
        ps.peerKey = &peerKey; ps.peerVal = &peerVal;
        ps.pasteKey = &pasteKey; ps.pasteCap = &pasteCap;
        ps.selfKey = &selfKey; ps.selfVal = &selfVal;
        ps.copyKey = &copyKey; ps.copyCap = &copyCap;
        ps.empty = &empty;
        panelBuildSeh(g_panel.panel, &ps);

        // Delegate assignment + white-colouring live OUTSIDE the SEH frame (pointer
        // targets are valid post-build; assignment can't fault) so no delegate
        // temporary lands in it.
        if (g_roleBtn)    g_roleBtn->callback    = MyGUI::newDelegate(&onRoleBtn);
        if (g_transBtn)   g_transBtn->callback   = MyGUI::newDelegate(&onTransBtn);
        if (g_connBtn)    g_connBtn->callback    = MyGUI::newDelegate(&onConnBtn);
        if (g_copyIdBtn)  g_copyIdBtn->callback  = MyGUI::newDelegate(&onCopyIdBtn);
        if (g_pasteIdBtn) g_pasteIdBtn->callback = MyGUI::newDelegate(&onPasteIdBtn);
        dbgColourSeh(g_debugLine, !transfer.empty()); // amber while streaming
        dbgColourSeh(g_peerLine, false);
        dbgColourSeh(g_selfLine, false);

        g_panel.built = true;
        g_panel.needsRebuild = false;
        g_panel.lastStatus = detail;
        g_panel.lastTransfer = transfer;
    }

    // Connect / disconnect on the Online/Offline toggle edge (edge, not level, so
    // a connect that hasn't reported running yet is not re-fired every tick). The
    // pasted friend id (0 if none) is handed to the plugin, which lets a non-zero
    // value override the config steamPeer; UDP ip/port still come from the config.
    if (g_panel.connectedFlag != g_panel.lastChkVal) {
        g_panel.lastChkVal = g_panel.connectedFlag;
        if (g_panel.connectedFlag && !st->running) {
            char b[80];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s",
                      g_panel.hostFlag ? "HOST" : "JOIN",
                      g_panel.steamFlag ? "steam" : "udp");
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            if (onConnect) onConnect(g_panel.hostFlag, g_panel.steamFlag, g_pastedPeer);
        } else if (!g_panel.connectedFlag && st->running) {
            coop::logLine("[coop-ui] DISCONNECT requested");
            if (onDisconnect) onDisconnect();
        }
    }
}

// ---- Persistent co-op status overlay ----------------------------------------
// A fixed banner in the top-left corner of the screen showing live session status
// colored by state (0 = offline/red, 1 = waiting/yellow, 2 = connected/green).
// Unlike the character-tracked ScreenLabel this replaces, it needs no player
// character and holds its place while the camera moves - which also makes it
// visible at the title screen, where a join has no leader while it streams the
// host's world. Removed when show=false.
//
// Two widgets, because neither factory alone does the job. Measured 2026-08-04:
// createFloatingLabel hands back a bare MyGUI::Window that IS visible and
// layer-attached at the coords we ask for, but its skin carries no text region at
// all - setCaption is silently dropped (getCaption().size() stays 0) and it has no
// children, so it draws nothing. It is still the only way to get a widget parented
// to a screen layer instead of to another window, so we keep it as an invisible
// container and put Kenshi's own label factory inside it (createLabelAbs ->
// MyGUI::TextBox with a text-bearing skin, the one the datapanel rows use).
// Caption + colour go to the child; the container is only geometry.

namespace {
// Banner box in pixels: 10 px in from the top-left corner. Applied with the
// absolute setCoord instead of createFloatingLabel's normalized coords, since a
// corner inset is a pixel quantity and the reconstructed header's top/left
// argument order is ambiguous (the normalized values are overwritten either way).
const int kOverlayX = 10;
const int kOverlayY = 10;
const int kOverlayW = 520;
const int kOverlayH = 26;

MyGUI::Window*  g_overlayBox   = 0; // container: geometry + layer attachment
MyGUI::TextBox* g_overlay      = 0; // the label that actually draws the text
int             g_overlayState = -1;
std::string     g_overlayText;

int overlayColorId(int state) { return state == 2 ? 0 : (state == 1 ? 2 : 1); }

// Put the freshly-minted container in its pixel box and mint the label inside it.
// createLabelAbs takes its text by const-ref and MyGUI::Align is a trivial int
// wrapper (no destructor), so this whole frame is SEH-safe - the same rule
// dbgColourSeh follows for MyGUI::Colour.
MyGUI::TextBox* overlayBuildSeh(MyGUI::Window* box, const std::string* text) {
    __try {
        box->setCoord(kOverlayX, kOverlayY, kOverlayW, kOverlayH);
        box->setVisible(true);
        MyGUI::TextBox* l = ::gui->createLabelAbs(box, 0, 0, kOverlayW, kOverlayH,
                                                  *text, MyGUI::Align::Left);
        if (l) {
            l->setTextAlign(MyGUI::Align::Left);
            l->setVisible(true);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Caption + colour in place. MyGUI::UString owns a buffer (destructor => C2712),
// so the caller builds it outside this frame and passes a pointer. false = the
// widget faulted; the caller then treats the pointer as dead.
bool overlayUpdateSeh(MyGUI::TextBox* l, const MyGUI::UString* text,
                      const MyGUI::Colour* col) {
    __try {
        l->setCaption(*text);
        l->setTextColour(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Destroying the container takes its label child with it (MyGUI owns the subtree).
void overlayDestroySeh(ForgottenGUI* g, MyGUI::Window* box) {
    __try { g->destroyWidget(box); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

void coopOverlayTick(const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    if (!show) {
        if (g_overlayBox) {
            overlayDestroySeh(g, g_overlayBox);
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay) {
        // createFloatingLabel takes the layer BY VALUE (an unwindable temporary
        // => C2712), so the container mint stays outside SEH, exactly like
        // createDatapanel above; ::gui was verified non-null. Layer MUST be "Info"
        // for the same reason the panel uses it - nothing draws on "Windows".
        if (g_overlayBox) { overlayDestroySeh(g, g_overlayBox); g_overlayBox = 0; }
        std::string layer = "Info";
        std::string empty;
        g_overlayBox = g->createFloatingLabel(0.01f, 0.01f, 0.30f, 0.03f, empty,
                                              MyGUI::Align::Default, layer);
        if (!g_overlayBox) {
            coop::logErrLine("[coop-ui] banner container FAILED");
            return;
        }
        g_overlay = overlayBuildSeh(g_overlayBox, &t);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] banner box=%p label=%p",
                  (void*)g_overlayBox, (void*)g_overlay);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (!g_overlay) {
            coop::logErrLine("[coop-ui] banner label FAILED");
            return;
        }
        g_overlayState = -1;   // no caller state is -1: forces the caption pass
        g_overlayText.clear();
    }

    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        MyGUI::UString u(t.c_str());
        if (overlayUpdateSeh(g_overlay, &u, &col)) {
            g_overlayText = t; g_overlayState = state;
        } else {
            // The GUI destroyed the widgets under us - clearGUI() on a world load
            // empties the layer and notifies nobody, so a pointer held across
            // ticks dangles silently (same lesson as the ScreenLabel registry
            // note above). Forget them and re-mint on the next tick.
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
    }
}

} // namespace engine
} // namespace coop
