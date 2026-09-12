KenshiCoop x4 - 3-4 player co-op mod
====================================

This zip contains the "KenshiCoop" folder (that folder IS the mod), plus this
README and PROVENANCE.json. Supports 2, 3, or 4 players over direct UDP / LAN.
Everyone must run this same build.

INSTALL (every player)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Copy the "KenshiCoop" folder into your Kenshi mods folder:
       <Kenshi>\mods\
     so you end up with:
       <Kenshi>\mods\KenshiCoop\KenshiCoop.dll   (and the other files)
     The default Steam path is:
       C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

PREREQUISITES (every player)
----------------------------
  1. Kenshi 1.0.65 (Steam).
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. The host must be reachable over UDP by every joiner: same LAN, or the
     host's port forwarded / a VPN for internet play.

PLAY (LAN / direct UDP)
-----------------------
  1. Each JOINER edits <Kenshi>\mods\KenshiCoop\coop_config.json (Notepad):
       "transport": "udp"
       "ip":   the HOST's address   (e.g. "192.168.1.10")
       "port": the HOST's port      (default 27800)
     ip/port are re-read whenever you go ONLINE, so no restart after an edit.
     The host only needs "transport": "udp".
  2. HOST: load a save, or start a new game and pick a co-op start from the list
     that matches your player count (see GAME STARTS below). Press F2, set
     Transport: UDP and Role: HOST, then toggle Connection to ONLINE.
  3. EACH JOINER: press F2 (works at the MAIN MENU - no save needed), set
     Transport: UDP and Role: JOIN, then toggle Connection to ONLINE. The host
     streams its world to you on connect and you load right into it. Joiners
     connect to the host only, never to each other. (If you already have an
     identical copy of the host's save on disk it is used as-is instead of
     transferring.)
  4. The white status line and the TOP-LEFT banner show live connection/transfer
     state, at the main menu as well as in-game. Toggle Connection to OFFLINE to
     leave.

GAME STARTS (one squad tab per player)
--------------------------------------
  The host runs squad 1; joiners take squads 2, 3, 4. Everyone's squad is
  visible and synced on every screen but answers only to its owner. New Game ->
  pick the bundled start that matches your player count, each pre-splits
  wanderers into separate squads so nobody has to split tabs by hand:
    * "Multiplayer (Wanderer x4)"  - four squads, for 3-4 players.
    * "Multiplayer (Wanderer x2)"  - two squads, for two players.
    * "Multiplayer+ (Wanderer x2)" - the x2 start with 500,000 cats (shared
                                      wallet) and both characters at 50 in every
                                      stat, to skip the early grind.
  With fewer players than squads, the unused squads just sit idle. You can also
  load any existing save and split units into extra squad tabs in-game.

STEAM (two players only)
------------------------
  The original two-player Steam P2P path still works but is NOT extended to 3-4
  players. For two players you may instead leave Transport on STEAM and swap
  Steam IDs in-game (F2 -> "Copy my Steam ID" / "Paste friend's Steam ID").

SAVING
------
  Any save any player makes during a session becomes one shared save on every
  machine, streamed automatically. To resume, the host loads it and goes online;
  the others reconnect from the main menu.

UNINSTALL
---------
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (UDP): the joiners' ip/port must match the host, and the host
    must be reachable over UDP (LAN, or port forwarded / VPN for internet play).
    Look for connection lines in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": someone has a different build; everyone should use the
    same release.

KNOWN ISSUES (this build is NOT a finished release)
---------------------------------------------------
  * Open release blockers in this build: INST-AUTODETECT, KIT-PROVENANCE
    See docs/RELEASE_BLOCKERS.md in the repository for what each one means.
