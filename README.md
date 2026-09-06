# KenshiCoop x4

**3–4 player UDP co-op for [Kenshi](https://lofigames.com/)** — an
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
scripts/          Build, deploy, session, and automated-test tooling (PowerShell)
docs/             Build guide, engine/API reference, protocol history, replication pitfalls
third_party/      ENet patches, VC10 compat shim (deps are fetched, not committed)
```

## Install & play — full 4-player walkthrough (LAN / direct UDP)

This walks through a **4-player** game: **one host + three joiners**. For 2 or 3
players it's identical — just fewer joiners. Everyone is on their own machine, and
all three joiners must be able to reach the host over UDP (same LAN, or the host's
port forwarded / a VPN for internet play). Joiners connect to the **host only**,
never to each other.

### Step 1 — Prerequisites (all four players)

1. **Kenshi 1.0.65 (Steam).**
2. **[RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847)** — the free
   Nexus mod that loads the co-op plugin.

### Step 2 — Install the mod (all four players)

Get the **`KenshiCoop`** mod folder from the
[Releases page](https://github.com/Asphacean/KenshiCoop_x4/releases/latest)
(recommended — same build for everyone), or from
[`dist/mod-kit/KenshiCoop`](dist/mod-kit/KenshiCoop) in this repository. Copy it
into your Kenshi `mods` directory so you end up with:

```
<Kenshi>\mods\KenshiCoop\KenshiCoop.dll
```

(default Steam path: `C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\`).
Launch Kenshi and enable **KenshiCoop** in the Mods menu. **All four players must
run the same release** — the protocol version is checked on connect.

### Step 3 — The host shares its address

The host tells the three joiners its **IP and port** (default port `27800`):

- **Same LAN:** the host's local IPv4 (e.g. `192.168.1.10` — find it with
  `ipconfig`).
- **Over the internet:** the host's public IP, with UDP port `27800` forwarded to
  the host machine (or everyone on a VPN like Hamachi/Tailscale, using the host's
  VPN address).

### Step 4 — Each of the three joiners points at the host

Every **joiner** edits `<Kenshi>\mods\KenshiCoop\coop_config.json` (all three use
the *same* host address):

```jsonc
{
  "transport": "udp",
  "ip": "192.168.1.10",   // the HOST's address (from Step 3)
  "port": 27800,          // the HOST's port
  "autoConnect": false
}
```

`ip`/`port` are re-read every time you go online, so no restart after an edit. The
**host** only needs `"transport": "udp"` (its `ip`/`port` are ignored — it listens).

### Step 5 — Host goes online

1. Start a new game and pick **`Multiplayer (Wanderer x4)`** from the start list —
   a ready-made **four-squad** start, one squad per player, so nobody has to split
   tabs by hand. (Or load any existing save; see notes below.)
2. Press **F2**, set **Transport: UDP** and **Role: HOST**, then toggle
   **Connection** to **ONLINE**. The top-left banner shows the host is listening.

### Step 6 — Each joiner connects (one at a time or together)

Each of the three joiners, on their own machine:

1. At the **main menu** (no save needed), press **F2**.
2. Set **Transport: UDP** and **Role: JOIN**, then toggle **Connection** to
   **ONLINE**.
3. The host streams its world over on connect and the joiner loads straight into
   it. Watch the top-left banner for the transfer, then the world loads.

Repeat for the second and third joiner. Once all three are in, you have a
four-player session: host = squad 1, joiners = squads 2, 3, 4. Toggle
**Connection** to **OFFLINE** to leave; others keep playing.

### Good to know

- **One squad tab per player.** With the **`Multiplayer (Wanderer x4)`** start each
  of the four players already has their own squad. Everyone's squad is visible and
  synced on every screen but answers only to its owner. The mod also bundles
  `Multiplayer (Wanderer x2)` (two squads) and `Multiplayer+ (Wanderer x2)` (x2 with
  500,000 shared cats and 50-in-every-stat, to skip the grind). Loading an existing
  save works too — just split units into extra squad tabs in-game, one per player.
- **Joiners don't need the host's save** — it's streamed on connect; an identical
  local copy just skips the transfer.
- **Saving is coordinated.** Any save any player makes becomes one shared save,
  streamed to everyone. To resume, the host loads it and goes online; the others
  reconnect from the main menu.
- **Late join works.** A player can join (or reconnect) after the session is
  already running — only the newcomer gets the world transfer; the others aren't
  interrupted.
- **Two players over Steam (optional).** The original two-player Steam P2P path
  still works (leave Transport on **STEAM**, swap Steam IDs in the F2 panel) but it
  is **not** extended to 3–4 players — use UDP for three or four.

### If something goes wrong

- **"The co-op plugin has not started"** — RE_Kenshi didn't load it. Check
  `<Kenshi>\RE_Kenshi_log.txt` for `KenshiCoop`; reinstalling
  [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) usually fixes it.
- **No connection (UDP)** — the joiners' `ip`/`port` must match the host, and the
  host must be reachable over UDP (LAN, or the port forwarded / VPN for internet
  play). Look for connection lines in `<Kenshi>\KenshiCoop_*.log`.
- **"protocol mismatch" in the log** — someone has a different build; everyone
  should reinstall from the same release.

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

`scripts/` also holds the automated test harness: headless protocol/transport
suites (`build_prototest.cmd`, `build_nettest.cmd`) and a scenario-based live
regression suite that launches host + up to three joins across local installs and
produces PASS/FAIL verdicts from the logs.

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
