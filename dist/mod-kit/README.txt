KenshiCoop x4 - 3-4 player co-op mod
====================================

This zip contains the "KenshiCoop" folder (that folder IS the mod), an installer
for Windows and one for Linux/Steam Deck, this README, and PROVENANCE.json.
Supports 2, 3, or 4 players over direct UDP / LAN. Everyone must run this same
build: the protocol version is checked when you connect and a mismatch is
rejected, which from the game looks simply like "it will not connect".

PROVENANCE.json records which build this is - the DLL's SHA-256, the protocol
version, and a hash of every file in this zip. If you are unsure what someone
sent you, that file is how you check.

PREREQUISITES (every player)
----------------------------
  1. Kenshi 1.0.65+ (Steam).
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. The Microsoft Visual C++ 2010 x64 runtime (the plugin is built with it).
     On Windows it is usually already present. Under Proton / Steam Deck the
     installer checks for it and tells you what is missing.
  4. The host must be reachable over UDP by every joiner: same LAN, or the
     host's port forwarded / a VPN (Tailscale, Hamachi) for internet play.

INSTALL (every player)
----------------------
  Windows:
    1. Right-click the downloaded zip > Properties > Unblock (if shown), then
       extract it.
    2. In the extracted folder, run:
         powershell -ExecutionPolicy Bypass -File install_coop.ps1
       It finds your Kenshi installation, backs up anything it replaces
       (verifying the backup by hash first), and writes the mod. If it finds
       more than one install it stops and asks which, so pass:
         powershell -ExecutionPolicy Bypass -File install_coop.ps1 -KenshiDir "<path to Kenshi>"
    3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

  Linux / Steam Deck:
    1. Extract the zip, then in that folder run:
         sh ./install_coop.sh --kenshi-dir ~/.local/share/Steam/steamapps/common/Kenshi
       This script does NOT auto-detect: --kenshi-dir is always required, and
       must point at the folder holding kenshi_x64.exe. Same behaviour
       otherwise, plus a check for the VC++ 2010 runtime files Proton needs
       beside kenshi_x64.exe.
    2. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

  To see what is installed without changing anything:
      install_coop.ps1 -Info            /  install_coop.sh --kenshi-dir DIR --info
  To remove it:
      install_coop.ps1 -Uninstall       /  install_coop.sh --kenshi-dir DIR --uninstall
  (The uninstall is driven by the manifest written at install time and restores
  the backups it verified; a file you edited yourself is reported and left
  alone, not deleted.)

PLAY (LAN / direct UDP)
-----------------------
  The connection is set up entirely in-game. There is no config file to edit.

  1. HOST: load a save, or start a new game and pick a co-op start from the list
     that matches your player count (see GAME STARTS below). Press F2, set
     Transport: UDP and Role: HOST, then toggle Connection to ONLINE. Tell the
     other players your IP address and port (default 27800).
  2. EACH JOINER: press F2 (works at the MAIN MENU - no save needed), set
     Transport: UDP and Role: JOIN, paste the host's address into the peer
     address field, then toggle Connection to ONLINE. The host streams its world
     to you on connect and you load right into it. Joiners connect to the host
     only, never to each other. (If you already have an identical copy of the
     host's save on disk it is used as-is instead of transferring.)
  3. The white status line and the TOP-LEFT banner show live connection/transfer
     state, at the main menu as well as in-game. Toggle Connection to OFFLINE to
     leave; the others keep playing.

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
  Run the installer again with -Uninstall (Windows) or --uninstall (Linux). It
  reverses exactly what it recorded at install time. Removing
  <Kenshi>\mods\KenshiCoop by hand also works.

KNOWN LIMITATIONS (worth knowing before you start)
--------------------------------------------------
  * Take a save sized to your group. A squad tab beyond the number of connected
    players is owned by NOBODY: every client can move it, and its state is not
    synced between you. Use the start that matches your player count.
  * A knocked-out body's resting position can drift apart between clients while
    it lies there. It is static while down and corrects when the character gets
    back up.
  * Three and four players over UDP is the new capability and it is a hobby
    project. Expect rough edges; the full, current list of known defects is in
    .planning/WINDOWS.md in the repository.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (UDP): the address each joiner pasted into the F2 panel must
    be the host's, and the host must be reachable over UDP (LAN, or port
    forwarded / VPN for internet play). Look for connection lines in
    <Kenshi>\KenshiCoop_*.log.
  * It just will not connect, with no other symptom: check the log for
    "protocol mismatch". Someone has a different build. Every player must
    install the SAME zip - compare dllSha256 in PROVENANCE.json.