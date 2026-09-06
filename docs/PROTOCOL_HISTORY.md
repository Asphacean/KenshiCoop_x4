# KenshiCoop wire protocol history

> **Purpose.** The committed, version-by-version wire-protocol history for
> KenshiCoop. `PROTOCOL_VERSION` is checked on connect and mismatched builds are
> rejected cleanly, so every participant in a session must run the same version.
>
> **Sourcing.** The v53-v61 narrative below is reproduced from `Wire.h`'s own
> per-version doc comments (`Wire.h:29-152`, each version's original author),
> cross-referenced against the packet-tag annotations in the `PacketType` enum
> and the introducing commits (`c2d7ca9` protocol 53-55, `5c6b905` 56, `5126511`
> 57, `d39c77e`/`4553a9a` 58, `f28580d` 59, `0f5d603` 60, `b7513fd` 61). The
> pre-53 appendix is a compact, best-effort reconstruction from the same
> `PacketType`/`EventType` enum comments and struct doc comments (grep `protocol
> [0-9]+` across `Wire.h`), since no earlier narrative doc survives to
> reproduce - it names the introducing packet/event per version, not a full
> per-bump narrative.
>
> **PROTOCOL_VERSION today:** `61` (`Wire.h:153`). This phase changes no wire
> surface - the version stays 61.

## v53 → v61 (full narrative)

### v53 (2-player era; `c2d7ca9` "protocol 53-55")

Prone/crawl posture becomes distinguishable on the wire: `Character::_currentProneState`
rides as a 3-bit field packed into the existing body-state byte (`Wire.h:508`,
`:534`, `:548`), so crawl enforcement can tell "prone from injury" apart from
other posture kinds instead of collapsing them to a single crawl bit.

### v54 (same era)

Property deeds: `PKT_DEED = 47`, `DeedPacket` - a property-ownership row so
faction-owned buildings/land resolve identically across clients.

### v55 (same era; `a0305d6` "a mine both players dig is one mine")

Runtime-fixture identity rows (shared mines): `PKT_FIXTURE = 48`,
`FixturePacket` - lets two players digging the same mine resolve it as ONE
fixture instead of two independently-minted ones.

### v56 (Phase 2 Plan 03; `5c6b905`)

Multi-peer roster: `PKT_PLAYER_JOINED = 49` / `PKT_PLAYER_LEFT = 50`
(`RosterPacket`). Every connected client now learns the FULL set of connected
PlayerIds, not just its own id from WELCOME - the wire's first move beyond a
strict 2-party assumption. No wire-format change to any existing packet.

### v57 (Phase 3 Plan 03; `5126511`)

Host-authoritative ownership ranks: `PKT_OWN_RANKS = 51` (`OwnRanksPacket`).
The host broadcasts the full `map<PlayerId,set<rank>>` as a per-player bitmask
whenever roster membership or dynamic tab ownership changes, generalizing the
`rank=playerId` default (OWN-01/OWN-03) so overrides and dynamically created
squad tabs also resolve identically on every machine.

### v58 (Phase 7 Plans 01+02; `d39c77e`, `4553a9a` - ONE bump for both)

**Plan 01:** replaces the optimistic, first-ACK-wins cross-owner transfer path
with a host-committed two-phase model, closing the load-bearing N>=3 gap where
a join<->join transfer intent silently terminated at the host (no relay, no
forge check, not even the FAILSAFE log) - the true destination owner never
heard about the trade. `InvXferPacket` (the intent) gains `srcOwnerId`/
`dstOwnerId` so the host can route/validate without a game-thread hand lookup;
the intent is now a host-terminated CLIENT-REQUEST
(`routingClassOf(PKT_INV_XFER) == RELAY_NONE`, not `RELAY_FAILSAFE_LOG`), with
`rejectIfForgedOwner` on the receive branch (the one owner-tagged reliable
packet that lacked it). Two new packets carry the commit: `PKT_XFER_COMMIT`
(host -> all, the single authoritative verdict, decided ONCE by the host) and
`PKT_XFER_COMMIT_ACK` (participant -> host, bookkeeping/audit only).

**Plan 02** (same v58 surface, no second bump): world-item claim/pickup
contention gets the same host-committed shape. `WorldItemClaimHeader` gains
`authorClaimMs`; `PKT_WORLD_ITEM_CLAIM` becomes the claim INTENT
(claimant -> host, host-terminated CLIENT-REQUEST); the host runs a bounded
contention window (`ClaimArbiter.h`) and broadcasts exactly one
`PKT_CLAIM_VERDICT = 54` (host -> all, `ClaimVerdictPacket`) naming the
earliest mapped-stamp winner.

Wire surface: +3 packets (`PKT_XFER_COMMIT=52`, `PKT_XFER_COMMIT_ACK=53`,
`PKT_CLAIM_VERDICT=54`), 2 struct extensions (`InvXferPacket`,
`WorldItemClaimHeader`).

### v59 (Phase 8 Plan 02, WORLD-03; `f28580d`)

Cell-claim authority becomes host-authoritative and broadcast, closing the
per-instance divergent reduce (map-iteration-order tie-breaks, single-author
history, no disconnect cleanup, a census plane that could represent only one
author at a time). `PKT_CELL_CLAIM` is re-pointed to a host-terminated INTENT
(wire shape unchanged; routing class only); a new `PKT_CELL_MAP = 55`
(`CellMapPacket`, host -> all) carries the single authoritative map, computed
by a pure `reduceCellMap` function (continuity -> host-if-party ->
lowest-playerId, presence-based hysteresis). `PKT_NPC_CENSUS` moves from Class
B (host-only) to Class A (relayed join->join) with no wire change
(`NpcCensusHeader` already carried `ownerId`).

Wire surface: +1 packet (`PKT_CELL_MAP`), 2 routing-class changes
(`PKT_CELL_CLAIM`, `PKT_NPC_CENSUS`).

### v60 (Phase 9 Plan 01, CONS-01; `0f5d603`)

The shared money pool becomes correct at N>=3 - money-only wire change, the
ONLY wire change this phase. Fixes three genuine at-N defects: a single
scalar `MoneyPacket::ackSeq` the host overwrote with whichever owner folded
last (wallet bounce/double-count across owners); an overdraft path that
folded unconditionally (silently minting value with no reject); and a
reconnect purge that never erased the money-fold state (a rejoined join's
restarted `seq=1` delta dropped forever as "already folded"). Fixed by:
`MoneyPacket`'s `ackSeq` scalar becomes a per-owner ack VECTOR (`ackCount` +
`Entry{ownerId,ackSeq}[MAX_PLAYERS]`, the `OwnRanksPacket` entries[] idiom);
`MoneyDeltaPacket` gains `authorSpendMs` so a would-overdraw spend can be
arbitrated by timestamp order (`MoneyFold.h`, the `ClaimArbiter.h` lineage);
and new `PKT_MONEY_REJECT = 56` (host -> all, `MoneyRejectPacket`) makes a
rejected purchase's verdict observable and identical on every instance.

Wire surface: +1 packet (`PKT_MONEY_REJECT`), 2 struct changes
(`MoneyPacket`, `MoneyDeltaPacket`).

### v61 (Phase 10 Plan 01, SAVE-01/02/03; `b7513fd` - CURRENT)

The coordinated save/load planes become correct at N>=3 - save/load-only wire
change, the ONLY wire change this phase. Fixes three genuine at-N defects: a
last-of-N-ACKs-wins collapse (the ACK drain discarded `SaveAckPacket.ownerId`,
so one client's `ok=1` structurally "committed" the whole group); no positive
load ACK existed on the wire (only `LOAD_NACK`), so the host never learned a
join finished a coordinated load and a join stuck in the NACK-flow waited
forever; and concurrent save/load requests silently last-winsed (no
arbitration, no rejection). Fixed by two new engine-free host-side headers
(`SaveCoord.h`/`LoadCoord.h`, the `ClaimArbiter.h`/`MoneyFold.h` lineage)
driving per-client transfer/ACK/retry/drop state machines and a first-wins
`CoordArbiter` shared by both planes, plus two new packets: `PKT_LOAD_ACK = 57`
(join -> host, the missing positive half of the load ACK/NACK pair) and
`PKT_COORD_REJECT = 58` (host -> requester unicast, the observable first-wins
rejection verdict naming both the rejected and active request ids). No struct
change to any existing save/load packet; unicast targeting for a
retry-to-one-client is transport-level (a queue-side destId), not a wire
field.

Wire surface: +2 packets (`PKT_LOAD_ACK`, `PKT_COORD_REJECT`).

## Pre-53 appendix (compact, versions 15-52)

One line per version, keyed to the packet/event it introduced (per the
`PacketType`/`EventType` enum comments and struct doc comments in `Wire.h`).
This is a reconstruction, not a reproduction of a lost per-bump narrative -
detail depth matches what the current header comments preserve.

| Ver | Introducing packet/event | What it covers |
|-----|---------------------------|-----------------|
| 15 | Combat STANCE split | Kenshi's `AttackSlotManager` grants only a subset of stances; the wire splits stance state accordingly (`Wire.h:457`) |
| 16 | Limb loss (`RobotLimbs::Limb` order) | `LimbState` per-limb tracking for amputation/crush events |
| 18 | `EVT_PICKUP_BODY` / `EVT_DROP_BODY` | Carried-body sync - subject = carried body, actor = carrier, both resolved locally |
| 19 | `EVT_ENTER_FURNITURE` / `EVT_EXIT_FURNITURE` | Furniture occupancy (bed/cage), later extended by v41 for chain/pole |
| 20 | Stealth sync | `Character::stealthMode` streamed exactly (the mode bool the engine reads) |
| 21 | Faction relation identity | Proxy-spawn stringID identity round-trip groundwork for the faction channel (v24) |
| 23 | `EVT_RECRUIT` | Recruitment sync - subject = recruited body's old hand, actor = its new hand after re-containering |
| 24 | `PKT_FACTION = 22` | Player-faction relation row (`FactionPacket`), keyed by GameData stringID |
| 25 | `PKT_TIME = 23` | Host-authoritative game clock (`TimePacket`) |
| 26 | `PKT_DOOR = 24` | Baked-door open/lock state row (`DoorPacket`) |
| 27 | `PKT_BUILD_PLACE = 25` / `PKT_BUILD_STATE = 26` | Placed-building describe/mint + placer-authoritative construction progress |
| 28 | `PKT_BUILD_DOOR = 27` / `PKT_BUILD_REMOVE = 28` | Placed-building door row (translated key) + placer-authoritative removal |
| 31 | `PKT_SAVE_REQ..PKT_SAVE_ACK = 29-33` | The original 2-player save-transfer plane (request/announce/chunk/CRC/ack) |
| 32 | `PKT_LOAD_GO/REQ/NACK = 34-36` | The original 2-player coordinated-load plane |
| 33 | `PKT_PROD = 37` | Host-authoritative machine state row (`ProdPacket`) |
| 35 | `EVT_SQUAD_MOVE` | Squad management sync - same shape as `EVT_RECRUIT`, an all-zero actor means dismissal |
| 36 | `PKT_NPC_CENSUS = 38` | Wide-radius NPC existence list (1 Hz), later re-classed Class A in v59 |
| 37 | `PKT_INV_XFER = 39` | Cross-owner transfer intent, later re-shaped host-committed in v58 |
| 38 | `PKT_RESEARCH = 40` | Host-authoritative known-research row |
| 39 | Host body age | Animal body SCALE derivation from age, streamed on the entity plane |
| 41 | Furniture occupancy `arg=3` (chained/pole) | Extends v19's furniture channel; actor slots carry the owner's hand instead of a building |
| 42 | LockedArmour (shackle) flag | Phase 6b inventory item flag for shackled items |
| 43 | `PKT_CAM_HINT = 41` | Join camera center hint, unreliable ~1 Hz, both directions |
| 45 | `PKT_COMBAT_HIT = 42` | Join-dealt authoritative damage report |
| 46 | Inventory snapshot flags / read-depth raise | `INV_FLAG_TRUNCATED`; container read depth raised 20 -> 64 (MAXC) |
| 47 | `PKT_WORLD_ITEM_CLAIM = 43` | Proxy-consumed notice; W1 mirrors a drop by minting a template proxy on the peer |
| 48 | Item PARENT REFERENCE | 0 = item sits directly in the container being synced (container-nesting support) |
| 49 | `PKT_CELL_CLAIM = 44` | Zone-cell presence claim, later re-pointed host-authoritative in v59 |
| 50 | `PKT_INV_XFER_ACK = 45` | Transfer verdict (superseded as the settle path by v58's `PKT_XFER_COMMIT`) |
| 51 | Craft/item GRADE | Kenshi's named item grades (Prototype/Shoddy/...) added to the inventory wire |
| 52 | `PKT_MONEY = 21` / `PKT_MONEY_DELTA = 46` | Shared money-pool total + join-authored delta, later hardened for N>=3 in v60 |

## Mismatch-rejection behavior (COMPAT-02)

**Handshake contract.** The client's `HelloPacket` carries
`version = PROTOCOL_VERSION` (`NetLink.cpp:779` loopback-host path,
`NetLink.cpp:1934` client path). The host's `WelcomePacket` echoes its own
`PROTOCOL_VERSION` so the client can re-check it too (`Wire.h:282-286`).

**Host-side reject (the authoritative gate).** `NetLink.cpp:831-838`: if the
incoming `HelloPacket.version != PROTOCOL_VERSION`, the host logs
`"protocol mismatch: peer v%u, ours v%u; rejecting"` and calls
`enet_peer_disconnect(ev.peer, 0)` - **before** the peer is ever inserted into
`registry_` and before it is ever assigned a `PlayerId`. The reject is clean:
no WELCOME is ever sent, no roster broadcast fires, and no registry residue
remains for a subsequent correctly-versioned connect to trip over.

**Client-side check (documented asymmetry, not a wire change).**
`NetLink.cpp:938-944`: if a client receives a `WelcomePacket` whose version
disagrees with its own, it logs `"protocol mismatch: host v%u, ours v%u"` and
does **not** set `myId_` - so the client never considers itself connected -
but it does **not** call `enet_peer_disconnect` on its own end. This is
functionally safe: every real mismatch is already rejected host-side (a
client only reaches this branch if it deliberately connects to a
differently-versioned host), and an unset `myId_` keeps the client's local
state inert regardless. A symmetric client-side disconnect would be a
reasonable defensive addition but is **not required** and is **not** a wire
change - noted here as a documented asymmetry, not a defect.

**Live proof (Phase 11 Plan 01).** Before this plan, this behavior was
covered only at the serialization level (`prototest` pins `PROTOCOL_VERSION
== 61` and byte-checks the HELLO version field's offset) and by a passing
comment in `nettest` comparing an unrelated rejection to "a protocol-version
mismatch" - no leg ever actually forged a version over a live transport.
`src/nettest/main.cpp`'s raw-HELLO forged-version leg closes that gap: a raw
ENet peer (not `NetLink`, which always sends the real `PROTOCOL_VERSION`)
hand-builds a `HelloPacket` with `version = PROTOCOL_VERSION + 1` and asserts
(a) the host logs the clean mismatch reject, (b) the forger never receives a
WELCOME and so never acquires a local id, and (c) a subsequently-connecting
correctly-versioned client still connects and gets its expected slot - proving
the reject left no registry residue.

## Log identity contract

**TEST-03 (Phase 11 Plan 02).** Every network/gameplay log line families this
project emits should let an offline reader identify: the local playerId
(which process wrote this line), the ownerId/author (whose data this is), the
source peer (who sent it, when different from the author), the destination
playerId (unicast targeting), the packet type (implicit in the log-line tag),
and a seq/event/transfer id (for dedup/ordering). This section certifies the
canonical identity source and documents which families already carry the full
tuple, which carried a minor gap this plan closed, and the append-only rule
that keeps every oracle regex working across the extension.

### Canonical local-id lines

A log line's *fixed prefix* (`[HH:MM:SS.mmm] [TAG] LEVEL: msg`, `CoopLog.cpp`)
does **not** carry the local playerId - `TAG` is only `HOST`/`JOIN`, set once
at `logInit` before a join's id is even assigned (WELCOME arrives after). The
canonical, certified source of a log's own local playerId is the connect-time
line each role already writes exactly once:

- **Host:** `"peer connected id=%u player=%u (proto v%u)"` (`NetLink.cpp:908-911`).
- **Join:** `"peer connected id=%u (proto v%u) - received WELCOME"` (`NetLink.cpp:948-950`).

These are certified as the identity source rather than modified - Harness
builds additionally emit `"SCENARIO MAGATE start ownRank=%u host=%d"`
(`ScenarioMilestoneA.cpp` and every N=4/N-instance gate scenario that reuses
its schema), which the PowerShell oracles' `Get-MagateOwnRank` resolves per
log for exactly this purpose.

### Per-family identity tuple

| Family | Local id | ownerId/author | Source peer | Dest id | Seq/event/xfer id | Status |
|--------|----------|-----------------|--------------|---------|--------------------|--------|
| `[wallet]` | tag | ✓ | =owner | bcast | ✓ seq | complete |
| `[xfer]` | tag | ✓ author/from | ✓ | bcast | ✓ id | complete |
| `[save]` | tag | ✓ | ✓ | ✓ dest | ✓ xferId | complete |
| `[load]` | tag | ✓ | ✓ | bcast/unicast | ✓ loadId | complete |
| `[boot]` | tag | host-authored | host | ✓ dest | ✓ id | complete |
| `[coord]` | tag | ✓ requester | ✓ | unicast | ✓ reqId | complete |
| `[cell]` | tag | ✓ | ✓ | bcast | ✓ seq | complete |
| `[speed]` | tag | ✓ (RECV) | ✓ | bcast | ✓ seq (Plan 02: appended to `REQ RECV`) | complete |
| `[wi]` | tag | ✓ on the claim plane; `SEND`/`MOVE` (Plan 02: appended `owner=`) | mixed | bcast | ✓ netId | complete |
| `[fac]` | tag | ✓ (Plan 02: appended `owner=` to `RECV`, naming the sender) | ✗ | bcast | ✓ seq | complete |
| `[deed]` | tag | `owner='%s'` is the faction name, not a playerId; `RECV` (Plan 02: appended `sender=`) | ✗ | bcast | ✓ seq | complete |
| `[recruit]` | tag | via the paired `[event]` line | via `[event]` | bcast | ✓ via `[event]` | complete (pairing) |
| `[event]` | tag | ✓ | ✓ | bcast | ✓ ev, deduped (ownerId,eventId) | complete |
| `[net]` | ✓ (this IS the local/peer-id family - see Canonical local-id lines above) | ✓ | ✓ | - | HELLO/WELCOME | complete |
| `[time]` | local-plane lines by design | via `[leave] ... owner=%u` reports | - | - | - | complete (receiver-side per-owner map) |

### Append-only field extensions (Plan 02 closure)

Three named gaps existed where a family's `RECV`/`SEND`/`MOVE` line omitted a
field its sibling lines already carried. All three were closed by
**appending** the missing field(s) at the **end** of the line - never
inserting or renaming an existing token:

- `[speed] REQ RECV owner=%u mult=%.2f paused=%d combat=%d` gained a trailing
  `seq=%u` (`ReplicatorChannels.cpp`) - the seq was already on the wire
  (`SpeedPacket.seq`, used by this same code path's own dedup guard) but was
  not logged.
- `[wi] SEND netId=... hash=%u` and `[wi] MOVE netId=... live=%d`
  (`ReplicatorItems.cpp`) each gained a trailing `owner=%u`, matching the
  `owner=` convention their sibling `[wi] SPAWN`/`[wi] CULL` lines already use
  for the same proxy.
- `[fac] RECV sid=... seq=%u` and `[deed] RECV hand=... seq=%u`
  (`ReplicatorChannels.cpp`) each gained a trailing sender field (`owner=%u`
  / `sender=%u`) naming the per-sender seq space these gates already key on
  internally (`gateSeqAcceptPerSender`) but did not surface in the log line.

### The extend-never-break rule

Every oracle pattern in `scripts/oracles/*.ps1` and `scripts/CoopOraclesN.psm1`
/ `scripts/CoopOracles.psm1` is shaped `[stamp].*<literal tokens with
positional capture groups>`. Appending ` key=value` tokens at the **end** of a
line is safe for every pattern except an **end-anchored** one (a trailing `$`
in the regex). A full sweep of `scripts/` found exactly **one** hard
end-anchor in the entire oracle surface:

- `scripts/oracles/World.ps1:1222`: `$writeTRx = 't=(\d+)$'` - the DOORWRITE
  sentinel line's secondary match requires `t=<n>` to be the **last** token.

None of Plan 02's TEST-03 extensions touch a DOORWRITE line, so this anchor
is untouched. Any **future** extension to a DOORWRITE line must insert its new
field(s) **before** `t=`, or update this regex (and its fixtures) in the same
commit. Never insert or rename a field between two existing tokens - a
positional capture group (e.g. `$saveClientPat`'s owner/xferId/state order)
breaks the moment a new field lands ahead of what it currently captures.

**The hard proof:** after every log-format change, every
`scripts/tests/*.Tests.ps1` fixture suite must stay green (fixtures are
synthetic logs, so one whose lines exercise a newly-appended field also
proves the extended shape still parses) **and** an archived live run must
re-judge to its previously-recorded verdict via `analyze_run4.ps1`. Phase 11
Plan 02 re-ran this proof against `tools/test-runs/20260904_143622_N4` for
`save_load_gate` (PASS before and after) and every named `*.Tests.ps1` suite
in the repo.

## Maintenance

When you bump `PROTOCOL_VERSION` in `Wire.h`, add the matching entry to the
top of the **v53 → v61** section above (keep the existing entries; this is an
append-only history) and update `Wire.h`'s own inline comment for the new
version to match. `prototest` should gain or update a byte-level pin for the
new version; `nettest`'s mismatch leg does not need per-version changes (it
always forges `PROTOCOL_VERSION + 1` at build time).
