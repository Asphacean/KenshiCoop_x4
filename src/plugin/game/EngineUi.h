// EngineUi.h - narrow PUBLIC engine surface: the in-game co-op session panel +
// status overlay. Carved out of Engine.h (Phase 5a domain split, 2026-07-19) so
// the UI root (Plugin.cpp) includes only what it needs and the sync/replication
// consumers stop transitively seeing the panel API.
//
// Like Engine.h this is a PUBLIC header: it declares only the SEH-guarded engine
// facade and must NEVER pull in a <kenshi/...> internal header - those live in
// the adapter (EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_UI_H
#define KENSHICOOP_ENGINE_UI_H

namespace coop {
namespace engine {

// ---- In-game co-op session panel ---------------------------------------------
// A native DatapanelGUI opened with F2 that lets the player pick role + transport
// (buttons/checkboxes - the only reliably interactive DatapanelGUI controls;
// MyGUI comboboxes/editboxes have no usable RVAs and don't receive keyboard focus
// during gameplay) and Connect/Disconnect. The friend's Steam ID is entered by
// clipboard: "Copy my Steam ID" puts the player's own id on the clipboard to
// share, and "Paste friend's Steam ID" reads the friend's id back in (per-session,
// never written to disk). The UDP endpoint is entered the same way - "Paste server
// address" reads a "host:port" off the clipboard - and is likewise per-session and
// never written to disk; coop_config.json remains the source when nothing is armed
// in the panel. Only the rows that match the armed transport are shown, so a player
// is never asked to fill in a field their transport ignores.
// The GUI layer stays session-agnostic: live status is passed IN
// via *st and the user's actions are handed BACK through the callbacks (the plugin
// root owns the session/config wiring). Main-thread only; SEH-guarded.
struct CoopPanelState {
    unsigned long long selfSteamId; // steamp2p::selfId (0 = Steam not up)
    unsigned long long peerSteamId; // config steamPeer fallback (0 = unset; pasted id wins)
    bool               running;     // net thread up
    bool               peerPresent; // peer connected
    bool               isHost;      // current armed role (seeds the Host toggle)
    int                transportSel;// current armed transport (0 steam, 1 udp)
    const char*        detail;      // one-line status string for the panel/overlay
    // Join-side save-transfer status (null when not streaming): byte-level
    // progress the one-line detail above has no room for, shown on the F2 panel
    // while a join receives the host's world (e.g. "Streaming host world... 42%
    // (3.1/7.4 MB)"). Set by coopPanelDrive, rendered in dbgVal.
    const char*        transferDetail;
    // Live session state, the SAME 0/1/2 vocabulary the status overlay already
    // uses (0 offline, 1 waiting, 2 connected). The panel's Connection button is
    // a two-position switch over a DESIRED state and cannot express "started,
    // waiting for the peer" - which is precisely the state a player sits in
    // while their friend is still loading, and the moment they most need to be
    // told nothing is broken. coopPanelDrive ASSIGNS the value it already
    // computes for coopOverlayTick rather than deriving it a second time; two
    // derivations of one state are how the panel and the banner drift apart.
    int                sessionState;
};
// The panel's role/transport selections at the moment Connect is hit. peerId is the
// Steam ID pasted in-panel this session (0 if none), and overrides the config
// steamPeer in coopUiConnect. udpAddr is the "host:port" pasted in-panel this
// session (null or empty = nothing armed) and overrides the config endpoint in
// exactly the way peerId overrides steamPeer - an ADDITIONAL source with
// priority when armed, never a replacement for coop_config.json, which is what
// the harness, every prior phase gate and the cross-machine rig run on.
typedef void (*CoopConnectFn)(bool isHost, bool useSteam, unsigned long long peerId,
                              const char* udpAddr);
typedef void (*CoopDisconnectFn)();
void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect);

// Log one line describing ForgottenGUI's datapanel update list (guiDatapanels):
// its count, where the co-op panel sits in it, and whether ANY pointer in it is
// duplicated. That list is a crash surface - ForgottenGUI::shutDown(), reached
// from ~ForgottenGUI during exit() at process quit, walks every index calling
// each entry's deleting destructor WITHOUT erasing as it goes, so a pointer
// present twice is deleted twice and the second delete reads a freed vptr.
// `where` is a short free-form tag naming the edge ("connect", "peer-leave",
// ...) and appears verbatim in the log. Call only on rare session/UI edges,
// never per tick. Safe with no panel open and safe before ::gui exists (it is a
// no-op then). Main-thread only; SEH-guarded.
void datapanelListProbe(const char* where);

// Persistent co-op connection-status banner: a single screen-space label fixed 10
// px in from the top-left corner (a createFloatingLabel MyGUI::Window on the
// spike-48 screenshot-proven "Info" layer) whose caption shows the live session
// status, colored by state (0 = offline/red, 1 = waiting/yellow, 2 =
// connected/green). Needs no player character, so it also shows at the title
// screen; updated in place when the text/state changes and re-minted if the GUI
// destroyed the widget (world load). Pass show=false to remove it. Main-thread
// only; SEH-guarded.
void coopOverlayTick(const char* text, int state, bool show);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_UI_H
