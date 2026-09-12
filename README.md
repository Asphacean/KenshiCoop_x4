# KenshiCoop x4

**2–4 player UDP co-op for [Kenshi](https://lofigames.com/)** — an
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin that extends
the original two-player co-op mod to **2, 3, or 4 players** over direct
UDP / LAN networking in a host-centered star topology.

> ### About this project — an experiment
>
> This mod is the result of an experiment with **Spec-Driven Development** using
> the **GSD Core** framework in combination with **Claude Code** and a conditional
> "unlimited" budget. My goal was to evaluate the quality of the product that this
> combination of tools can realistically produce — how far a spec-driven,
> agent-executed workflow can carry a non-trivial, correctness-critical C++
> networking project on its own.
>
> The 3–4 player expansion was built phase by phase against a written
> specification, each phase gated by automated verification: headless
> protocol/transport test suites plus a live multi-instance regression matrix
> across 2-, 3-, and 4-player configurations. It is a working, tested expansion —
> but it remains a hobby experiment. Expect rough edges.

It builds directly on **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**
(the original two-player co-op mod). All of the original gameplay is preserved and
generalized to N players: squads, NPCs, combat, inventory and equipment, direct
cross-player trades, ground item drops/pickups, base building and containers, the
shared money pool, game speed, game time, coordinated saves, and late-join.

> **Status: work in progress.** Hobby project, expect desyncs and crashes. The
> two-player path still works through the same generalized code; three and four
> players is the new capability.

## What's different from the original

- **Up to four players** (one host + three joiners) instead of exactly two.
- **Per-player network identity** everywhere — every piece of replicated state is
  keyed by player, so no player's squad, inventory, money, or world edits can be
  confused with, corrupted by, or destroyed by another player's actions or
  disconnects.
- **Correct N-player consensus** — shared money with per-player ACK and
  deterministic overdraft resolution, game-speed voting (slowest wins, combat cap,
  a leaver's vote dropped instantly), and time sync.
- **Item conservation across all players** — cross-owner trades and world-item
  contention resolve to a single deterministic winner with no duplication or loss.
- **True late-join** — a fourth player can join an established three-player session
  and receive the world without resetting anyone else.
- **UDP / LAN transport is the target.** (Steam networking is out of scope for the
  expansion; the original two-player Steam path is left intact but not extended.)

## Install

**Every player needs, first:**

1. **Kenshi 1.0.65+ (Steam).**
2. **[RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847)** — the free
   Nexus mod that loads the co-op plugin. Without it the plugin is never loaded
   and the game just runs vanilla.
3. **The Microsoft Visual C++ 2010 x64 runtime.** The plugin is built with the
   VC++ 2010 (v100) toolset, a KenshiLib requirement. On Windows it is usually
   already installed; under Proton / Steam Deck the installer checks for it and
   names what is missing.

**Then**, download `KenshiCoop-kit.zip` from the
[Releases page](https://github.com/Asphacean/KenshiCoop_x4/releases/latest),
extract it (on Windows: right-click the zip → Properties → Unblock first, if
shown), and from the extracted folder run:

```
powershell -ExecutionPolicy Bypass -File install_coop.ps1                       # Windows
sh ./install_coop.sh --kenshi-dir ~/.local/share/Steam/steamapps/common/Kenshi  # Linux / Steam Deck
```

The installer checks the prerequisites above, backs up anything it replaces —
verifying each backup by hash *before* touching the original — and writes the
mod. On Windows it auto-detects your Kenshi installation and stops rather than
guessing if it finds more than one; pass `-KenshiDir "<path>"` in that case. The
Linux script does not auto-detect at all — `--kenshi-dir` is always required.
Then launch Kenshi and enable **KenshiCoop** in the Mods menu.

`install_coop.ps1 -Info` reports what is installed without changing anything;
`-Uninstall` reverses the install from the manifest it recorded, restoring the
backups it verified and leaving alone any file you edited yourself.

> **Everyone must install the same build.** The protocol version is checked at
> handshake and a mismatch is rejected by design — which, from inside the game,
> looks like nothing more than "it just won't connect". If that happens, check
> `<Kenshi>\KenshiCoop_*.log` for `protocol mismatch`, and compare `dllSha256` in
> the `PROVENANCE.json` that ships in the kit.

## Play (LAN / direct UDP)

The connection is configured **entirely from the in-game F2 panel** — role,
transport, and the host's address. There is no config file to edit.

1. **Host:** start a new game and pick a co-op start matching your player count
   (see below), or load an existing save. Press **F2**, set **Transport: UDP** and
   **Role: HOST**, then toggle **Connection** to **ONLINE**. Tell the others your
   address and port (default `27800`).
2. **Each joiner:** press **F2** — this works at the **main menu**, no save needed
   — set **Transport: UDP** and **Role: JOIN**, paste the host's address into the
   peer address field, then toggle **Connection** to **ONLINE**.
3. The host streams its world over on connect and the joiner loads straight into
   it. The status line and the top-left banner show the transfer. Toggle
   **Connection** to **OFFLINE** to leave; the others keep playing.

Joiners connect to the **host only**, never to each other. Everyone must be able
to reach the host over UDP: same LAN, or the host's port forwarded / everyone on a
VPN (Tailscale, Hamachi) for internet play.

### Good to know

- **One squad tab per player.** The mod bundles `Multiplayer (Wanderer x4)` (four
  squads), `Multiplayer (Wanderer x2)`, and `Multiplayer+ (Wanderer x2)` (x2 with
  500,000 shared cats and 50-in-every-stat, to skip the grind). Pick the start that
  matches your group — **a squad tab beyond the number of connected players is
  owned by nobody**: every client can move it and its state is not synced. Loading
  an existing save works too; split units into extra squad tabs in-game, one per
  player.
- **Joiners don't need the host's save** — it's streamed on connect; an identical
  local copy just skips the transfer.
- **Saving is coordinated.** Any save any player makes becomes one shared save,
  streamed to everyone. To resume, the host loads it and goes online; the others
  reconnect from the main menu.
- **Late join works.** A player can join (or reconnect) after the session is
  already running — only the newcomer gets the world transfer.
- **Two players over Steam (optional).** The original two-player Steam P2P path
  still works (leave Transport on **STEAM**, swap Steam IDs in the F2 panel) but it
  is **not** extended to 3–4 players — use UDP for three or four.
- **Known defects are tracked in the open.** `.planning/WINDOWS.md` is the current
  list; `docs/RELEASE_BLOCKERS.md` is what gates publication.

## How it works

- `KenshiCoop.dll` is loaded into the game by RE_Kenshi. It hooks the engine via
  KenshiLib and drives all game mutation on the main thread.
- Networking is [ENet](https://github.com/lsalzman/enet) over UDP: reliable
  channels for one-shot events, unreliable 20 Hz state that self-heals.
- The host is authoritative for the world; each player is authoritative for its own
  squad. Client state flows Client → Host → other clients exactly once. See
  [docs/API_REFERENCE.md](docs/API_REFERENCE.md) for the engine-control surface and
  [docs/ROUTING_MATRIX.md](docs/ROUTING_MATRIX.md) / [docs/PROTOCOL_HISTORY.md](docs/PROTOCOL_HISTORY.md)
  for the wire protocol.

```
src/plugin/       The KenshiCoop plugin (net, sync/replication, engine facade, scenarios)
src/netproto/     Shared wire-protocol headers (plain C++03, compiled by everything)
src/nettest/      Standalone ENet console app (transport / multi-peer tests)
src/prototest/    Wire-protocol unit tests
scripts/          Build, install, session, and automated-test tooling (PowerShell)
docs/             Build guide, engine/API reference, protocol history, replication pitfalls
                  (incl. CROSS_MACHINE_RIG.md - the two-machine Windows-host +
                  Steam-Deck-join test rig procedure)
third_party/      ENet patches, VC10 compat shim (deps are fetched, not committed)
```

## Building

The plugin must be compiled with the **Visual C++ 2010 (v100) x64 toolset** (a
KenshiLib requirement — C++03, no modern STL). Full toolchain setup, gotchas, and
install steps are in [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md). Short version,
once prerequisites are in place:

```
scripts\build_plugin.cmd
```

Dependencies are fetched, not committed:

- KenshiLib + precompiled libs: clone
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  into `third_party/KenshiLib_deps/`
- ENet: clone [lsalzman/enet](https://github.com/lsalzman/enet) into
  `third_party/enet/enet/` and apply the patches in `third_party/enet/patches/`
  (see `third_party/enet/README.md`)

`scripts/make_mod_kit.ps1` packages the release kit — it builds Release (no
scenario harness), hash-stamps every packaged file into `PROVENANCE.json`, and
derives a `shippable` verdict from `docs/RELEASE_BLOCKERS.md`. `scripts/` also
holds the automated test harness: headless protocol/transport suites
(`build_prototest.cmd`, `build_nettest.cmd`) and a scenario-based live regression
suite that launches host + up to three joins across local installs and produces
PASS/FAIL verdicts from the logs.

## Credits

This project stands entirely on the original mod and the tooling underneath it:

- **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)** — the original
  two-player co-op mod this expansion is built on.
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) — RE_Kenshi and
  KenshiLib, which make plugins like this possible.
- [lsalzman/enet](https://github.com/lsalzman/enet) — UDP networking library.
- [zeroit789](https://github.com/zeroit789) — the original "Multiplayer (Wanderer)"
  co-op game start.
- Lo-Fi Games — Kenshi.

## License & contact

[AGPL-3.0](LICENSE). KenshiLib and RE_Kenshi are GPLv3; this plugin links KenshiLib
under GPLv3 section 13 (GPL/AGPL combination). Not affiliated with Lo-Fi Games.

**This is a non-commercial fan project.** I'm open to questions, feedback, and
collaboration — feel free to open an issue or a discussion on this repository.
