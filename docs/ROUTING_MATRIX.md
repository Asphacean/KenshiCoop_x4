# KenshiCoop packet-routing matrix

> **Purpose.** ROUTE-01: classify every `PacketType` and `EventType` value
> from `src/netproto/Wire.h` into one of six routing classes, so Phase 3's
> relay layer has a checkable dispatch table instead of re-deriving intent
> from wire-comment prose. Each row states the packet's sender, owner,
> destination(s), and relay rule; the `scripts/tests/RoutingMatrix.Tests.ps1`
> completeness check asserts every Wire.h enum value appears exactly once
> below, and fails on drift (a future packet added to Wire.h without a
> matching row, a renamed/retired identifier still referenced here, or a
> duplicate row).
>
> `docs/PROTOCOL_HISTORY.md` (Phase 11 Plan 01, COMPAT-02) is the committed
> per-protocol-version narrative (what each version bump added and why,
> v53->v61 plus a pre-53 appendix and the mismatch-rejection-behavior
> contract), replacing the phantom `resources/PROTOCOL_HISTORY.md` pointer
> this file used to carry (`resources/` is gitignored and absent from every
> checkout). `Wire.h`'s own struct doc comments remain the per-bump source
> annotations.
>
> **Current-vs-target framing.** No relay/forwarding logic exists anywhere
> in the codebase today. Call-site verification (this phase, Task 2) confirms
> the finding is stronger than "no relay exists" - there is also no per-peer
> unicast primitive at all: `src/plugin/net/NetLink.cpp`'s outbound drain for
> every host-originated channel (time, stealth, cam hints, spawn info, medical,
> treatment, inv-xfer, save-transfer, ...) funnels through the identical
> two-branch send:
> ```
> if (isHost_) enet_host_broadcast(enetHost_, CH, out);
> else if (serverPeer_ connected) enet_peer_send(serverPeer_, CH, out);
> ```
> A grep of `src/plugin/net/` for `sendTo`/`SendTo` returns zero matches -
> `NetLink` has no `sendTo(playerId)` or `broadcastExcept(playerId)` primitive
> today (this is exactly the gap MAIN_GOAL's Milestone A calls out: "Multi-peer
> network model on host: sendTo(playerId), broadcast, broadcastExcept(playerId)").
> The one exception is the connect-time handshake reply (`PKT_WELCOME`), sent
> inline via `enet_peer_send(ev.peer, ...)` directly against the ENetPeer
> handle the connect event just handed the host - this bypasses the generic
> queue entirely and is genuinely already-correct at any N.
>
> Because there is only ever one non-host participant today, "broadcast to
> everyone else," "send to the host only," "answer the requester," and
> "route to the specific other owner" are wire-indistinguishable at N=2 for
> every packet that goes through the generic queue. At N=3 they diverge, and
> nothing in the current send path enforces the difference - a targeted
> response (Class D) or a late-joiner's save-transfer (Class F) would leak to
> every connected client via the same broadcast call a Class B packet uses.
> This matrix documents both the *current* observed behavior and the *target*
> N-player rule per row - see the Relay rule column and `## Routing Classes`.

## Routing class legend

| Class | Name | Meaning |
|-------|------|---------|
| A | Broadcast-authoritative | Author (either host or a join) publishes; the packet must reach every OTHER connected client, relayed via the host, keeping the author's `ownerId` |
| B | Host-broadcast | Host is the sole authority; host publishes to every connected client |
| C | Client-request | A client (join) sends to the host only; not relayed to other clients |
| D | Targeted-response | Host answers the specific requesting client only |
| E | Cross-owner | Source client's packet is routed via the host to one specific OTHER owner (not a broadcast, not host-only) |
| F | Bootstrap | Session/connect-time or bulk-transfer traffic targeted at one specific client, usually a joiner |

## Classification table

Columns: **Type** (Wire.h identifier) · **Class** (A-F, or N/A for the
`EVT_NONE` sentinel / unused `PKT_LEAVE`) · **Sender** · **Owner** ·
**Destination(s)** · **Relay rule** · **Notes**.

Sender/Owner/Destination/Relay-rule cells state each class's general rule
(see `## Routing Classes` for the full per-class rationale); a row's Notes
column calls out anything that deviates from its class default.

| # | Type | Class | Sender | Owner | Destination(s) | Relay rule | Notes |
|---|------|-------|--------|-------|-----------------|------------|-------|
| 1 | `PKT_HELLO` | F | Connecting client | N/A - pre-`PlayerId` handshake | Host only | Client-initiated; not a send-side gap. Host assigns and returns the `PlayerId` in `PKT_WELCOME`. | "client -> host on connect: version + name" |
| 2 | `PKT_WELCOME` | F | Host | N/A - assigns the connecting client's `PlayerId` | The one connecting client | Already correctly targeted at any N - sent inline via `enet_peer_send(ev.peer, ...)` directly against the connecting ENetPeer handle (`NetLink.cpp` connect handler), bypassing the generic broadcast queue entirely. | "host -> client: version echo + assigned playerId" |
| 3 | `PKT_LEAVE` | N/A | - | - | - | not wire-routed / currently unused | "net thread -> game thread marker"; whole-`src/` grep finds zero serialization/send/receive references - dead/reserved enum value, never on the wire. Still listed here (completeness check requires every enum value exactly once). |
| 4 | `PKT_ENTITY_BATCH` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Host-authored: `enet_host_broadcast` already reaches everyone (correct at any N). Join-authored: reaches host only today; needs new host-side relay code for N≥3 (re-broadcast to every OTHER connected client, `ownerId` unchanged). | "either direction: owner-tagged EntityState batch (20 Hz)" |
| 5 | `PKT_EVENT` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. `queueEvent` carries no host-only guard, confirming either side may author. | one-shot transition, idempotent via `eventId` |
| 6 | `PKT_INV_SNAPSHOT` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | container's owning client authors; other clients reconcile |
| 7 | `PKT_WORLD_ITEM` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | "either direction... netId spaces are PER-SENDER" |
| 8 | `PKT_WORLD_ITEM_REMOVE` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | mirrors `PKT_WORLD_ITEM`'s cull, same per-sender scoping |
| 9 | `PKT_WORLD_DROP` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | "EACH client relocates its OWN copy" - must reach every other client, not just host |
| 10 | `PKT_WORLD_PICKUP` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | mirror of `PKT_WORLD_DROP` |
| 11 | `PKT_TIME_PING` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only; never relayed by design. | "join -> host" wall-clock probe |
| 12 | `PKT_TIME_PONG` | D | Host | N/A - host-computed echo | The one requesting client | **Corrected in Phase 3 (03-01):** this row's prior claim ("uses the same `enet_host_broadcast` primitive as Class B... needs a new targeted `sendTo(playerId)` primitive") was stale. Direct code read (`NetLink.cpp`'s `PKT_TIME_PING` receive branch) shows `PKT_TIME_PONG` is composed and sent inline via `enet_peer_send(ev.peer, CH_UNRELIABLE, out)` - directly targeted at the same peer that sent the ping. This is already correctly unicast at any N; no `sendTo()` and no code change were needed. | "host -> join" echo, answers the specific requester |
| 13 | `PKT_MEDICAL` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default (verified: `outMedical_` drain uses the generic broadcast/single pattern, `NetLink.cpp:~1286`). | "each client streams its OWNED members' medical model to the peer" |
| 14 | `PKT_TREATMENT` | A | Author (host or join, the healer) | Author's `ownerId` | Every other connected client | **Reclassified in Phase 6 (06-01, GAP-1):** `routingClassOf(PKT_TREATMENT)` now returns `RELAY_BROADCAST_EXCEPT` (`NetLink.cpp:372-373`), and the receive branch calls `rejectIfForgedOwner` + `relayDispatch` (`NetLink.cpp:~1117-1133`) - previously it only pushed to the host's own Inbound and never relayed at all. No destination-PlayerId field was ever needed: every receiver already applies a treatment ONLY to a body it authoritatively owns (`applyTreatments`'s `ownHands_`/`medNpc_` guard, `ReplicatorChannels.cpp:362-364`), so broadcast-except is correct - a non-authority receiver's copy is a harmless no-op ignore. `applyBandageParts` is raise-only/idempotent (`Wire.h:929-931`), so two concurrent healers converge safely. | "forwarded to the body's OWNER" - broadcast-except + apply-side authority guard now achieves that at any N |
| 15 | `PKT_SPEED_REQ` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only. | "join -> host" |
| 16 | `PKT_SPEED_SET` | B | Host | N/A - host-arbitrated value | All connected clients | Already correct at any N - `enet_host_broadcast` reaches every connected client by design. | "host -> join" arbitrated effective value |
| 17 | `PKT_STATS` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | owned player-squad stats streamed to peer(s) |
| 18 | `PKT_STEALTH` | A | Author (host or join, the detection authority) | Author's `ownerId` | Every other connected client | **Reclassified in Phase 6 (06-01, GAP-2):** `routingClassOf(PKT_STEALTH)` now returns `RELAY_BROADCAST_EXCEPT` (`NetLink.cpp:372-373`), and the receive branch calls `rejectIfForgedOwner` + `relayDispatch` (`NetLink.cpp:~1289-1306`) - previously it only pushed to the host's own Inbound and never relayed at all, so a join's detection feedback about ANOTHER join's sneaker never reached that join. Every publisher already authors symmetrically for its OWN driven copies (`ReplicatorChannels.cpp` publish, ~2123-2192) and every receiver applies feedback ONLY onto a body it owns (`applyStealthFeedback`'s `ownHands_` guard, `ReplicatorChannels.cpp:2206`), so broadcast-except is correct - no destination-PlayerId field was ever needed. | "host -> the sneaker's owner" - broadcast-except + apply-side own-hand guard now achieves that at any N (including join-authored feedback about another join's sneaker) |
| 19 | `PKT_SPAWN_REQ` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only. | "join -> host" |
| 20 | `PKT_SPAWN_INFO` | D | Host | N/A - host-authoritative spawn description | The one requesting client | Currently uses the same `enet_host_broadcast` primitive as Class B (verified: `outSpawnInfo_` drain, `NetLink.cpp:1673-1696`) - indistinguishable from broadcast at N=2, but leaks to every connected client at N≥3. Needs a new targeted `sendTo(playerId)` primitive. | "host -> join" reply to the requester |
| 21 | `PKT_MONEY` | B | Host | N/A - host-authoritative pool total | All connected clients | Already correct at any N - `enet_host_broadcast` reaches every connected client by design. **Phase 9 (09-01, protocol 60, CONS-01):** the single `ackSeq` scalar is now a per-owner ack vector (`ackCount` + `Entry{ownerId,ackSeq}[MAX_PLAYERS]`, the `PKT_OWN_RANKS` idiom) - each join reads only its own entry (`MoneyFold.h::moneyShouldPop`), closing the at-N ack-space bounce/double-count. | "host -> join: the authoritative pool total + per-owner ack vector (protocol 60)" |
| 22 | `PKT_MONEY_DELTA` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only; host arbitrates via `MoneyFold.h`, never relays the raw delta. **Phase 9 (protocol 60):** the fold-tracking two-player assumption is now closed - the host's ack broadcast is a per-owner vector (row 21) and the receive branch gains `rejectIfForgedOwner` (it lacked the check pre-Phase-9, the one owner-tagged reliable packet that did). | "join -> host: one signed change, now carrying `authorSpendMs` (protocol 60)" |
| 23 | `PKT_FACTION` | C | Source client (host or join) | Source's `ownerId` | Host only - the host applies the write and its OWN publisher re-emits the committed relation to every connected client under the host's `ownerId`/`seq` | **Reclassified in Phase 8 (08-01, WORLD-02):** was Class A (`RELAY_BROADCAST_EXCEPT`) - a join's row mutated the global faction-relation fact directly on every OTHER client too, violating the locked "joins never mutate global facts directly" decision and (as a side effect) exposed the ChangeGate.h cross-sender seq collision at N>=3. Now Class C (`routingClassOf(PKT_FACTION) == RELAY_NONE`, the `PKT_INV_XFER`/`PKT_WORLD_ITEM_CLAIM` host-terminated-intent shape): the receive branch calls `rejectIfForgedOwner` before pushing the intent to Inbound (never relayed further), and `applyFactions` (`ReplicatorChannels.cpp`) branches on `ctx.isHost` - the HOST applies `writeRelationBySid` but does NOT update its own publish baseline for a received row, so its own `publishFactions` detects a genuine change next sample and re-emits the committed value under the HOST's `ownerId`/`seq`; a JOIN keeps the original echo guard (baseline updated before the write) applying only host-authored rows. No wire change - `FactionPacket` is unchanged. | "join intent -> host commits + re-emits under host seq" (protocol 58, no bump) |
| 24 | `PKT_TIME` | B (host sample) / C (join sample) | Host (broadcast sample) or Join (host-only sample) - see Notes | N/A - clock reference, not owned state | Host's sample: all connected clients. Join's sample: host only. | Host's sample: `enet_host_broadcast`, already correct at any N (the host is the time authority every join slews toward). Join's sample: targets `serverPeer_` only, already correct at any N (informational input to the host's own clock-brake; never needs to reach other joins). | **Call-site verified (Task 2):** `Replicator::syncTime(gw, in, net, ownerId, isHost)` (`ReplicatorChannels.cpp:2410`) has two branches. The `isHost` branch (2417-2509) reads the join's broadcasted-back sample to run the host's clock-brake, then unconditionally sends the host's own `TimePacket` via `net.queueTime()`. The join branch (2511-2593) reads the host's broadcast sample to compute its own slew, then reports its own sample back via the same `net.queueTime()` call. Both calls route through `NetLink`'s single `queueTime`->drain function, which internally branches on `isHost_` (`NetLink.cpp:1421-1433`) - so the SAME `PacketType` genuinely carries two different routing classes depending on which role sent it. One row, sender-role split recorded here rather than doubling the row (per locked decision). |
| 25 | `PKT_DOOR` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | "SYMMETRIC change-gated channel: each client samples... streams rows"; **Phase 8 (08-01, WORLD-01):** stays Class A (a transient toggle, not a locked global fact - a host-commit round-trip would add latency for zero correctness gain) - only the APPLY-side accept changed, from one shared `seqSeen` counter to a per-sender `std::map<u32,u32>` (`gateSeqAcceptPerSender`, `ChangeGate.h`), closing the N>=3 collision where two authors of the same door hand could silently drop each other's newer row |
| 26 | `PKT_BUILD_PLACE` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | "placer" (either client) authors, others mint a matching proxy |
| 27 | `PKT_BUILD_STATE` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | placer-authoritative construction progress, streamed to others |
| 28 | `PKT_BUILD_DOOR` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | symmetric, translated-key door state; **Phase 8 (08-01, WORLD-01):** stays Class A - identical per-sender `seqSeen` rekey as `PKT_DOOR` (row 25), applied to `BdoorRow` |
| 29 | `PKT_BUILD_REMOVE` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | placer-authoritative removal |
| 30 | `PKT_SAVE_REQ` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only. | "join -> host" |
| 31 | `PKT_SAVE_BEGIN` | F | Host | N/A - bulk-transfer announce | The one receiving client | Uses the same `enet_host_broadcast`-or-`serverPeer_` primitive as Class B/D (verified: `SaveXfer.cpp`'s `net.queueSaveBegin()` funnels through `NetLink.cpp:1712-1730`'s generic outbound drain) - correct at N=2 (only one other client exists) but would broadcast the late-joiner's entire save transfer to every already-connected client at N≥3, contradicting MAIN_GOAL's "bootstrap transfer targeted only to joiner; no global reset" requirement. Needs the same `sendTo(playerId)` primitive as Class D. | targeted bulk-transfer announce (host -> the one receiving client) |
| 32 | `PKT_SAVE_FILE` | F | Host | N/A - bulk-transfer chunk | The one receiving client | Same gap as `PKT_SAVE_BEGIN` - shares the generic outbound drain, needs `sendTo(playerId)` for N≥3. | targeted bulk-transfer chunk |
| 33 | `PKT_SAVE_DONE` | F | Host | N/A - bulk-transfer completion + CRC table | The one receiving client | Same gap as `PKT_SAVE_BEGIN` - shares the generic outbound drain, needs `sendTo(playerId)` for N≥3. | targeted bulk-transfer completion + CRC table |
| 34 | `PKT_SAVE_ACK` | C | Join | N/A - commit acknowledgement | Host only | Already correct at any N - targets `serverPeer_` only. | "join -> host" commit acknowledgement |
| 35 | `PKT_LOAD_GO` | B | Host | N/A - host-initiated coordinated load order | All connected clients | Already correct at any N - `enet_host_broadcast` reaches every connected client by design; "all participating clients must eventually follow." | host-initiated load - broadcast, not targeted |
| 36 | `PKT_LOAD_REQ` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only. | "join -> host" |
| 37 | `PKT_LOAD_NACK` | C | Join | N/A - request, not owned state | Host only | Already correct at any N - targets `serverPeer_` only; the host's response is a fresh targeted `SaveXfer` (Class F), not part of this packet. | "join -> host" |
| 38 | `PKT_PROD` | B | Host | N/A - host-authoritative machine state | All connected clients | Already correct at any N - `enet_host_broadcast` reaches every connected client by design. | "HOST-authoritative machine state row" |
| 39 | `PKT_NPC_CENSUS` | A | Author (host or join) | Author's `ownerId` | Every other connected client | **Reclassified in Phase 8 (08-02, WORLD-03):** was Class B (host-only, `RELAY_NONE` by omission) - the per-owner census intake (`map<ownerId,CensusSet>`, `Replicator::applyOneNpcCensus`) makes a join's own census safe to relay to OTHER joins: no wire change (`NpcCensusHeader` already carries `ownerId`), `routingClassOf(PKT_NPC_CENSUS) == RELAY_BROADCAST_EXCEPT`, and the receive branch gains `rejectIfForgedOwner` before pushing to Inbound + relaying. The host's own census still reaches everyone the same way it always did. | "the host's 1 Hz wide-radius hand list"; **Phase 8 (08-02, WORLD-03):** now genuinely bidirectional - every connected client publishes its own census, relayed to every OTHER client, so the host-authoritative cell-claim map (row 67) has a per-owner existence substrate at N>=3 |
| 40 | `PKT_INV_XFER` | C | Source client (host or join) | Source's `ownerId`; `srcOwnerId`/`dstOwnerId` resolved on the author's game thread (protocol 58) | Host only - the host arbitrates via `XferCommit.h` and broadcasts the verdict on `PKT_XFER_COMMIT` (row 64) | **Reclassified in Phase 7 (07-01, INV-02):** was Class E/`RELAY_FAILSAFE_LOG` - the receive branch never even relayed OR forge-checked it (`NetLink.cpp:1081-1087` prior to this phase), so a join↔join intent silently died at the host with a false-positive ACCEPT to the author. Now Class C (`routingClassOf(PKT_INV_XFER) == RELAY_NONE`, a host-terminated CLIENT-REQUEST): the intent carries `srcOwnerId`/`dstOwnerId` so the host can validate/arbitrate without a game-thread lookup of its own, and the receive branch calls `rejectIfForgedOwner` before `pushInvXfer` (the one owner-tagged reliable packet that lacked it). The raw intent is never relayed to any other client - only the host's own `PKT_XFER_COMMIT` reaches everyone. | cross-owner transfer INTENT (host-terminated, protocol 58) |
| 41 | `PKT_RESEARCH` | B (host stream) / C (join intent) - see Notes | Host (broadcast) or a join (post-baseline unlock intent) - see Notes | N/A - host-authoritative research state; a join's intent is source's `ownerId` | Host's stream: all connected clients. Join's intent: host only. | Host's stream: already correct at any N - `enet_host_broadcast` reaches every connected client by design. Join's intent: `routingClassOf(PKT_RESEARCH)` was ALREADY `RELAY_NONE` pre-Phase-8 (no re-point needed - it fell to the default host-terminated branch by omission from every relay list); the receive branch gains `rejectIfForgedOwner` since Phase 8 (08-01, WORLD-02) is the first time a join legitimately authors this packet. | "HOST-authoritative... the host streams a row for every sid"; **Phase 8 (08-01, WORLD-02):** a join now ALSO publishes - after silently seeding its shared-save baseline, it sends its own genuinely-new post-baseline unlocks as host-terminated intents (`Replicator::publishResearch`/`applyResearch`, `driveSampledChannels`' research row moved `hostAuth` true->false); the host applies via the SAME idempotent `researchStartBySid` a join uses for the host's own broadcast, then its own stream re-broadcasts so every other join converges. No wire change - `ResearchPacket` reuses its existing fields (only its doc comment updated); this is the SAME sender-role-split shape row 24 (`PKT_TIME`) already documents for one wire type carrying two routing classes. |
| 42 | `PKT_CAM_HINT` | A | Author (join today; "BOTH directions" per comment) | Author's `ownerId` | Every other connected client | Host-authored: broadcast already reaches everyone. Join-authored: reaches host only today; needs new relay code for true N-way fan-out at N≥3 (not just pairwise). | "BOTH directions" - must become true N-way fan-out, not pairwise |
| 43 | `PKT_COMBAT_HIT` | C | Join | N/A - damage report against a host-owned NPC | Host only | Already correct at any N - targets `serverPeer_` only; host applies locally and never relays this packet itself (resulting NPC health flows back out via PROD/vitals-style channels, which are Class B). | "join -> host" damage report |
| 44 | `PKT_WORLD_ITEM_CLAIM` | C | Claiming client (host or join) | Claimant's `ownerId`; `authorClaimMs` is the claimant's own claim-detection timestamp (protocol 58) | Host only - the host arbitrates via `ClaimArbiter.h`'s bounded contention window and broadcasts the winner on `PKT_CLAIM_VERDICT` (row 66) | **Reclassified in Phase 7 (07-02, INV-03):** was Class E/`RELAY_UNICAST` routed to the item's `authorId` - the receive branch relayed the raw claim straight to the author, who destroyed its real ground object unconditionally on notice (the `applyWorldClaims` "no track; already gone" silent dup, a bug even at 2 players: two simultaneous claims for the same item each kept a copy). Now Class C (`routingClassOf(PKT_WORLD_ITEM_CLAIM) == RELAY_NONE`, a host-terminated CLIENT-REQUEST, the exact `PKT_INV_XFER` precedent): the claim is now an INTENT carrying `authorClaimMs`, the receive branch calls `rejectIfForgedOwner` before pushing to the host's own arbiter, and the raw intent is never relayed to the item's author or any other client - only the host's own `PKT_CLAIM_VERDICT` reaches everyone. | claim-contention INTENT (host-terminated, protocol 58) |
| 45 | `PKT_CELL_CLAIM` | C | Author (host or join) | Author's `ownerId` | Host only - the host folds every connected owner's claim into `claimSlots_` and runs the reduce; the map it broadcasts (`PKT_CELL_MAP`, row 67) is the only thing every OTHER client ever needs to see | **Reclassified in Phase 8 (08-02, WORLD-03):** was Class A (`RELAY_BROADCAST_EXCEPT`) - the per-instance N-way tie-break reduction this row's OLD notes called for is exactly what a per-instance reduce could never resolve identically (`std::map` iteration order), so the redesign moves the reduce itself to the host instead of trying to relay every claim to every client for a symmetric local computation. Now Class C (`routingClassOf(PKT_CELL_CLAIM) == RELAY_NONE`, the `PKT_INV_XFER`/`PKT_FACTION`/`PKT_DEED` host-terminated-intent shape): every instance still PUBLISHES its own claims (`net.queueCellClaim`, unchanged send-side), the receive branch calls `rejectIfForgedOwner` before pushing to the host's own intake, and the raw claim is never relayed to any other client. No wire change - `CellClaimPacket` is unchanged. | claim INTENT (host-terminated, protocol 59, no wire change) - the host alone reduces + broadcasts the verdict on `PKT_CELL_MAP` |
| 46 | `PKT_INV_XFER_ACK` | D | Host (verdict issuer) | N/A - host-computed verdict; routed by `xferOwnerId` | The one specific `xferOwnerId` owner | Currently uses the same `enet_host_broadcast` primitive as Class B/D - indistinguishable from broadcast at N=2, but leaks the verdict to every connected client at N≥3. Needs a new targeted `sendTo(playerId)` primitive. Classified Class D (targeted-response), not Class E: the "request" it answers is a prior Class E `PKT_INV_XFER` rather than a Class C request, but its routing shape - host answers back to the one specific requester that authored the original intent - is Class D's, not Class E's "route to a different other owner." **Superseded in Phase 7 (07-01, INV-02):** still on the wire byte-for-byte (an old-build receiver's stray ACK still parses), but no longer the settling verdict - first-ACK-wins is ambiguous at N≥3 (row 64, `PKT_XFER_COMMIT`, is now the single host-authored verdict). | verdict routed back to the specific `xferOwnerId` that authored the original intent (a Class D-shaped response to a Class E request); SUPERSEDED as the settle path by `PKT_XFER_COMMIT` (protocol 58) |
| 47 | `PKT_DEED` | C | Source client (host or join) | Source's `ownerId` | Host only - the host applies the write and its OWN publisher re-emits the committed ownership to every connected client under the host's `ownerId`/`seq` | **Reclassified in Phase 8 (08-01, WORLD-02):** was Class A (`RELAY_BROADCAST_EXCEPT`) - the same "joins never mutate global facts directly" violation as `PKT_FACTION` (row 23), for property ownership instead of diplomacy. Now Class C (`routingClassOf(PKT_DEED) == RELAY_NONE`), the identical shape: the receive branch calls `rejectIfForgedOwner`, and `applyDeeds` (`ReplicatorChannels.cpp`) branches on `ctx.isHost` - the HOST applies `writeDeedByHand` but leaves `dr.knownOwned`/`dr.sent`/`dr.lastSendMs` untouched for a received row (those are `publishDeeds`' own bookkeeping, not an echo guard to pre-empt), so the host's own `publishDeeds` sees a genuine change next sample and re-emits the committed ownership under the HOST's `ownerId`/`seq`; a JOIN keeps the original echo guard. The unresolved-hand-leaves-row-unapplied latch and the `walletMoved` double-charge guard are unchanged. No wire change - `DeedPacket` is unchanged. | "join intent -> host commits + re-emits under host seq" (protocol 58, no bump) |
| 48 | `PKT_FIXTURE` | A | Author (host or join) | Author's `ownerId` | Every other connected client | Same as Class A default. | "SYMMETRIC: each client announces... near its own interest centers" |
| 49 | `PKT_PLAYER_JOINED` | B | Host | N/A - host-assigned roster fact, not attributed to a connected client | All ALREADY-connected clients, excluding the newcomer, for the newcomer's own join announcement (plus the targeted per-existing-peer catch-up sent only to the newcomer) | Already correct by construction (Phase 2 Plan 03) - built directly on `sendTo(playerId)` (catch-up) and `broadcastExcept(playerId)` (the newcomer's own join announcement, WR-01 fix, commit `6ae2ffa`) from Plan 02/03 - never broadcasts the newcomer's join back to itself. | "host -> all (except the newcomer itself): a player connected" (protocol 56, `RosterPacket`) |
| 50 | `PKT_PLAYER_LEFT` | B | Host | N/A - host-observed roster fact, not attributed to a connected client | All connected clients | Already correct by construction (Phase 2 Plan 03) - `broadcast()`-only (the departing peer is no longer a valid destination). | "host -> all: a player disconnected" (protocol 56, `RosterPacket`) |
| 51 | `PKT_OWN_RANKS` | B | Host | N/A - host-authoritative ownership assignment, not attributed to a connected client | All connected clients | Already correct by construction (Phase 3 Plan 03) - `Replicator::announceOwnRanks()` -> `NetLink::broadcastOwnRanks()` -> `enet_host_broadcast`, already correct at any N (Class B default). Host-authoritative: clients apply only this host-authored map, never a peer-authored rank claim (T-03-06). | "host -> all: the authoritative map<PlayerId,set<rank>> as a per-player bitmask" (protocol 57, `OwnRanksPacket`) |
| 64 | `PKT_XFER_COMMIT` | B | Host | N/A - host-authoritative transfer verdict, not attributed to a connected client | All connected clients | **New in Phase 7 (07-01, INV-02, protocol 58):** the host-committed replacement for the old first-ACK-wins settle path. `NetLink::queueXferCommit()` mirrors `broadcastOwnRanks()` exactly - `enet_host_broadcast` when authored by the host (`isHost_` true), inert if a join mis-calls it (falls into the no-op else-branch, same shape as `PKT_OWN_RANKS`'s own doc comment). Decided ONCE by the host via `XferCommit.h`'s pending-transfer state machine (keyed `(authorId,transferId)`) for every intent, host-as-participant included, so the log evidence is identical regardless of who the host is. The receive branch is split `!isHost_`/`isHost_` exactly like `PKT_OWN_RANKS` (row 51): a client applies it; the host REJECTS + logs a client that tries to author one (T-03-06 precedent) rather than silently accepting a peer-authored verdict. | "host -> all: the single authoritative outcome for a transfer intent" (protocol 58, `XferCommitPacket`) |
| 65 | `PKT_XFER_COMMIT_ACK` | C | Participant (transfer author, source owner, or destination owner) | N/A - bookkeeping ack, not itself owned state | Host only | **New in Phase 7 (07-01, INV-02, protocol 58):** targets `serverPeer_` only via the generic outbound drain (Class C shape, like `PKT_MONEY_DELTA`); the host's receive branch calls `rejectIfForgedOwner` then pushes to Inbound for audit/correlation only - it never settles anything (the commit already did). | "participant -> host: audit/correlation only, never a settle path" (protocol 58, `XferCommitAckPacket`) |
| 66 | `PKT_CLAIM_VERDICT` | B | Host | N/A - host-authoritative claim-contention verdict, not attributed to a connected client | All connected clients | **New in Phase 7 (07-02, INV-03, protocol 58):** the host-committed answer to `PKT_WORLD_ITEM_CLAIM` (row 44). `NetLink::queueClaimVerdict()` mirrors `queueXferCommit()`/`broadcastOwnRanks()` exactly - `enet_host_broadcast` when authored by the host (`isHost_` true), inert if a join mis-calls it. Decided ONCE by the host via `ClaimArbiter.h`'s bounded contention window (keyed `(authorId,netId)`) - earliest mapped stamp wins, ties within an eps tie-band or an unmappable stamp break to the lowest playerId, commit-final. The receive branch is split `!isHost_`/`isHost_` exactly like `PKT_XFER_COMMIT`/`PKT_OWN_RANKS`: a client applies it; the host REJECTS + logs a client that tries to author one. | "host -> all: the single deterministic winner for one item's contention" (protocol 58, `ClaimVerdictPacket`) |
| 67 | `PKT_CELL_MAP` | B | Host | N/A - host-authoritative cell-claim map, not attributed to a connected client | All connected clients | **New in Phase 8 (08-02, WORLD-03, protocol 59):** the host-committed answer to `PKT_CELL_CLAIM` (row 45) - the single authoritative cell-authority verdict, computed by `CellMap.h`'s engine-free `reduceCellMap` (continuity -> host-if-party -> lowest playerId) over every connected owner's claim slots. `NetLink::queueCellMap()` mirrors `queueClaimVerdict()`/`queueXferCommit()`/`broadcastOwnRanks()` exactly - `enet_host_broadcast` when authored by the host, inert if a join mis-calls it (falls into the no-op else-branch). The receive branch is split `!isHost_`/`isHost_` exactly like the other host-authoritative broadcasts: a client adopts it WHOLESALE (`claimedCells_`/`cellLastOwner_` replaced, own-claim optimism gone - a cell absent from the map fail-opens to host, never to the local sender); the host REJECTS + logs a client that tries to author one. | "host -> all: the single authoritative cell-claim map" (protocol 59, `CellMapPacket`) |
| 68 | `PKT_MONEY_REJECT` | B | Host | N/A - host-authoritative insufficient-funds verdict, not attributed to a connected client | All connected clients | **New in Phase 9 (09-01, CONS-01, protocol 60):** the host-committed answer to a `MoneyDeltaPacket` (row 22) `MoneyFold.h`'s overdraft window rejected - the single deterministic verdict for one `(buyerId, seq)`, computed by `MoneyFold.h`'s engine-free `moneyOffer`/`moneyFinalize` (solvent-immediate fold; a would-overdraw delta opens a bounded contention window arbitrated by TIMESTAMP - earliest mapped stamp folds, eps tie-band broken by lowest playerId, commit-final). `NetLink::queueMoneyReject()` mirrors `queueClaimVerdict()`/`queueCellMap()`/`queueXferCommit()` exactly - `enet_host_broadcast` when authored by the host, inert if a join mis-calls it. The receive branch is split `!isHost_`/`isHost_` exactly like the other host-authoritative broadcasts: a client logs the observation (the buyer's own ack advance, carried on the next `PKT_MONEY` broadcast, is what pops the pending delta and produces the visible refund); the host REJECTS + logs a client that tries to author one. | "host -> all: the single deterministic insufficient-funds verdict" (protocol 60, `MoneyRejectPacket`) |
| 69 | `PKT_LOAD_ACK` | C | Join | N/A - completion report, not owned state | Host only | **New in Phase 10 (10-01, SAVE-02, protocol 61):** the missing positive half of the load ACK/NACK pair - `PKT_LOAD_NACK` (row 37) existed, but the host never learned when a join's coordinated load actually SUCCEEDED. Class C shape (the `PKT_SAVE_ACK` precedent, row 34) - targets `serverPeer_` only via the generic outbound drain; never relayed. The receive branch calls `rejectIfForgedOwner` before pushing to Inbound (the same T-10-01 gap `PKT_SAVE_ACK` closed) - a forged `ownerId` could otherwise mark ANOTHER client's `LoadClient` LOADED/FAILED once `LoadCoord.h::loadNoteAck` keys per-owner state on it. `ok` latches through the WORLD-RELOAD gameplay-live edge (`sessionResetForWorldReload`), not `loadSave()`'s earlier deferred-issue point - loads take tens of seconds, so "issued" is not "complete". | "join -> host: positive coordinated-load completion (or ok=0 the early-fail signal)" (protocol 61, `LoadAckPacket`) |
| 70 | `PKT_COORD_REJECT` | D | Host | N/A - host-arbitrated first-wins verdict; routed by `requesterId` | The one specific rejected `requesterId` | **New in Phase 10 (10-01, SAVE-03, protocol 61):** the observable half of the first-wins save/load arbiter (`LoadCoord.h::CoordArbiter`) - a concurrent/overlapping `PKT_SAVE_REQ`/`PKT_LOAD_REQ` the host's shared arbiter refuses (busy with a DIFFERENT active transition) is answered with this Class D unicast, carrying BOTH the rejected request's own ids and the currently-active transition's ids (the requester may retry after it completes). `NetLink::queueCoordReject()` targets `sendTo(requesterId)` directly (never `enet_host_broadcast` - a leaked reject would disclose another client's in-flight request, the Class D "no blind rebroadcast" security rule this matrix's own Security note states). The receive branch is split `!isHost_`/`isHost_` exactly like the other host-authoritative packets: a client applies it (logs `[coord] REJECTED ...`); the host REJECTS + logs a client that tries to author one (a join has no authority to arbitrate). | "host -> the rejected requester only: first-wins arbitration verdict" (protocol 61, `CoordRejectPacket`) |

### EventType block (`EVT_*`, carried on `PKT_EVENT` / `EventPacket`)

`EventPacket` is always `ownerId` + `eventId`-tagged (`ownerId` = network
player id of the sender; `eventId` = "monotonic per-sender" counter for
idempotent apply - `[VERIFIED: src/netproto/Wire.h:154-155]`). Both host and
join author events for their own subjects: `NetLink::queueEvent`
(`NetLink.cpp:149`) carries no host-only guard, unlike the money/prod/research
send paths, and the receive switch (`NetLink.cpp:545`) accepts `PKT_EVENT`
from either role. Every `EVT_KNOCKOUT`..`EVT_SQUAD_MOVE` value is therefore
classified as a block: Class A (broadcast-authoritative - the observing
client authors it, host relays to all others, idempotent via `eventId`).
`EVT_NONE` is the zero-value sentinel, not a transmitted event. No single
`EVT_*` value surfaced a host-only-authored exception during this
call-site pass (Assumption A3 in RESEARCH.md is therefore not triggered).

| # | Type | Class | Sender | Owner | Destination(s) | Relay rule | Notes |
|---|------|-------|--------|-------|-----------------|------------|-------|
| 52 | `EVT_NONE` | N/A | - | - | - | not wire-routed / sentinel | Zero-value sentinel ("no event"); never itself transmitted as a distinct wire payload. Listed for completeness. |
| 53 | `EVT_KNOCKOUT` | A | Observing client (host or join) | Subject's owning client's `ownerId` | Every other connected client | Same as Class A default - `PKT_EVENT`'s generic send path. | subject went down / unconscious (BODY_DOWN edge) |
| 54 | `EVT_DEATH` | A | Observing client (host or join) | Subject's owning client's `ownerId` | Every other connected client | Same as Class A default. | subject died (BODY_DEAD edge) - permanent, latched on the join |
| 55 | `EVT_REVIVE` | A | Observing client (host or join) | Subject's owning client's `ownerId` | Every other connected client | Same as Class A default. | subject stood back up (down -> upright edge) |
| 56 | `EVT_AMPUTATE` | A | Observing client (host or join) | Subject's owning client's `ownerId` | Every other connected client | Same as Class A default. | subject lost a limb; arg = RobotLimbs::Limb |
| 57 | `EVT_CRUSH` | A | Observing client (host or join) | Subject's owning client's `ownerId` | Every other connected client | Same as Class A default. | subject's limb was crushed; arg = limb |
| 58 | `EVT_PICKUP_BODY` | A | Carrier's client (host or join) | Carrier's `ownerId` | Every other connected client | Same as Class A default. | carrier lifted the subject onto its shoulder |
| 59 | `EVT_DROP_BODY` | A | Carrier's client (host or join) | Carrier's `ownerId` | Every other connected client | Same as Class A default. | carrier released the subject |
| 60 | `EVT_ENTER_FURNITURE` | A | Observing client (host or join) | Occupant's owning client's `ownerId` | Every other connected client | Same as Class A default. | occupant was placed in / climbed into the furniture |
| 61 | `EVT_EXIT_FURNITURE` | A | Observing client (host or join) | Occupant's owning client's `ownerId` | Every other connected client | Same as Class A default. | occupant left / was removed from the furniture |
| 62 | `EVT_RECRUIT` | A | Recruiting client (host or join) | Recruiter's `ownerId` | Every other connected client | Same as Class A default. | subject's old hand re-keyed to its new hand after recruitment |
| 63 | `EVT_SQUAD_MOVE` | A | Moving client (host or join) | Mover's `ownerId` | Every other connected client | Same as Class A default. | subject's old hand re-keyed to its new hand after a squad-tab move; all-zero actor = dismissal |

**Completeness note:** 70 rows total - 58 `PacketType` values
(`PKT_HELLO=1` through `PKT_COORD_REJECT=58`, Phase 7 (07-01) adds
`PKT_XFER_COMMIT=52`/`PKT_XFER_COMMIT_ACK=53` at rows 64-65, Phase 7 (07-02)
adds `PKT_CLAIM_VERDICT=54` at row 66, Phase 8 (08-02) adds
`PKT_CELL_MAP=55` at row 67, Phase 9 (09-01) adds `PKT_MONEY_REJECT=56`
at row 68, and Phase 10 (10-01) adds `PKT_LOAD_ACK=57`/`PKT_COORD_REJECT=58`
at rows 69-70 (row NUMBERS are this doc's own thematic ordering, not the
enum's numeric value - see the legend above), no gaps) and
12 `EventType` values (`EVT_NONE=0` through `EVT_SQUAD_MOVE=11`,
`[VERIFIED: src/netproto/Wire.h:88-124]`, no gaps). `RoutingMatrix.Tests.ps1`
asserts this set matches Wire.h exactly.

## Routing Classes

General sender/owner/destination/relay-rule rule per class. Per-row Notes in
the classification table above override only where a specific packet
deviates from its class's default.

### Class A - Broadcast-authoritative

- **Sender:** the author of the state - either the host or a join, for its
  own owned subject (squad member, world item, etc).
- **Owner:** the author's `ownerId`, carried unmodified on the wire and never
  rewritten as host-owned.
- **Destination(s):** every OTHER connected client (not the author).
- **Relay rule:** host-authored Class A packets already reach every connected
  client today via `enet_host_broadcast` - correct at any N with no code
  change. Join-authored Class A packets reach only the host today
  (`enet_peer_send(serverPeer_, ...)`) - **needs new host-side relay code for
  N≥3**: on receipt from a join, the host must re-broadcast to every OTHER
  connected client (excluding the author), stamping/validating the relayed
  `ownerId` against the sender-peer-to-PlayerId mapping the host itself
  assigned at connect time (see Security note below), never rewriting it as
  host-owned.
- **Relay implemented in Phase 3 (03-01):** every Class A row in the
  classification table above now relays through `NetLink::routingClassOf()` ->
  `RELAY_BROADCAST_EXCEPT` - a validated fresh-copy `broadcastExcept(author)`
  called from each Class A packet's receive branch, gated by
  `hdr.ownerId == sourcePlayerId` exactly as described here.
- **`PKT_FACTION`/`PKT_DEED` reclassified OUT of Class A in Phase 8 (08-01,
  WORLD-02):** both were the last two LOCKED global-fact channels still
  broadcasting a join's row directly to every other client, in violation of
  the "joins never mutate global facts directly" decision - see rows 23/47
  and Class C below.

### Class B - Host-broadcast

- **Sender:** host only.
- **Owner:** N/A - the value is host-authoritative, not attributed to a
  specific connected client.
- **Destination(s):** all connected clients.
- **Relay rule:** already correct at any N - every Class B send already goes
  through `enet_host_broadcast`, which reaches every connected ENet peer
  regardless of how many are connected. No new code needed.
- **Relay implemented in Phase 3 (03-03):** `PKT_OWN_RANKS` joins this class -
  `Replicator::announceOwnRanks()` builds the authoritative
  `map<PlayerId,set<rank>>` and calls `NetLink::broadcastOwnRanks()` ->
  `enet_host_broadcast`, whenever roster membership or the tab set changes.

### Class C - Client-request

- **Sender:** a join (client), never the host.
- **Owner:** N/A - a request, not a piece of owned replicated state.
- **Destination(s):** the host only.
- **Relay rule:** already correct at any N - every Class C send already
  targets `serverPeer_` exclusively via `enet_peer_send`. By design this
  class is **never** relayed to other clients; the host must not blindly
  rebroadcast a Class C packet to unrelated clients (see Security note
  below) even after the relay layer exists for Class A/E.

### Class D - Targeted-response

- **Sender:** host only, answering a specific client's Class C request.
- **Owner:** N/A - host-computed, not attributed to a connected client.
- **Destination(s):** the one specific client that made the request.
- **Relay rule:** **currently broken at N≥3, not merely "not yet extended".**
  Every Class D send today reuses the exact same `enet_host_broadcast`
  primitive as Class B - the host has no way today to target one specific
  peer from the generic outbound queue (no `sendTo(playerId)` exists in
  `NetLink`). At N=2 this is indistinguishable from a true targeted send
  (there is only one other peer to broadcast to); at N≥3 it would leak the
  response to every connected client instead of only the requester. **Needs
  a new `sendTo(playerId)` primitive** before N≥3 correctness, plus a host-side
  map from the servicing request to the requester's `PlayerId` (today the
  requester is implicit - whichever single join is connected). Until that
  primitive exists, the host must not fall back to blindly rebroadcasting a
  Class D response to every connected client to work around the missing
  unicast - that would expose a targeted, potentially host-only-intended
  answer (e.g. `PKT_STEALTH`'s detection map) to unrelated clients (see
  Security note below).
- **Relay implemented in Phase 3 (03-01), partially:** `PKT_TIME_PONG` needed
  no change (row 12 - already correct). `PKT_INV_XFER_ACK` now relays through
  `NetLink::routingClassOf()` -> `RELAY_UNICAST` - a validated fresh-copy
  `sendTo(xferOwnerId)`. `PKT_STEALTH`/`PKT_SPAWN_INFO` route to
  `RELAY_FAILSAFE_LOG` instead (their destination cannot be resolved on the net
  thread this phase - see Class E's note and RESEARCH.md Pitfall 4): the host
  logs `relay FAILSAFE no-relay type=.. player=..` and drops rather than
  falling back to a leaky broadcast.
- **`PKT_STEALTH` reclassified out of Class D in Phase 6 (06-01, GAP-2):** the
  host-computed-targeted-response framing above never actually matched
  `PKT_STEALTH`'s wire shape - every machine (host or join) authors its own
  detection feedback symmetrically, and the receiver-side `ownHands_` apply
  guard already makes an untargeted broadcast-except correct (row 18). It now
  lives in Class A instead of needing the deferred `sendTo(playerId)`
  primitive this section describes.

### Class E - Cross-owner

- **Sender:** the source client (host or join) initiating a cross-owner
  interaction (treatment, inventory transfer, item claim).
- **Owner:** the source's `ownerId`; the packet is routed by a
  destination-owner field (the body's owner, `authorId`, `xferOwnerId`).
- **Destination(s):** the one specific OTHER owner the interaction targets -
  never a broadcast, never host-only.
- **Relay rule:** join-authored Class E packets reach only the host today
  (same collapse as Class A). **Needs both new host-side relay code AND the
  same new `sendTo(playerId)` primitive as Class D** for N≥3: the host must
  resolve the destination-owner field to a connected `PlayerId` and route
  the packet there exclusively - never broadcasting it (a Class E packet
  reaching an unrelated third client would leak private interaction data
  and could desync inventory conservation).
- **Relay implemented in Phase 3 (03-01), partially:** `PKT_WORLD_ITEM_CLAIM`
  already carries a routable destination field (`authorId`) and now relays
  through `NetLink::routingClassOf()` -> `RELAY_UNICAST` - a validated
  fresh-copy `sendTo(authorId)`. `PKT_TREATMENT` and `PKT_INV_XFER` route to
  `RELAY_FAILSAFE_LOG` instead: their destination is "the body's owner" /
  "the destination container's owner", which needs a game-thread
  `ownerOfHand()` lookup this phase does not build (RESEARCH.md Pitfall 4) -
  the host logs `relay FAILSAFE no-relay type=.. player=..` and drops rather
  than leaking a cross-owner interaction to an unrelated third client.
- **`PKT_TREATMENT` reclassified out of Class E in Phase 6 (06-01, GAP-1):**
  the `ownerOfHand()` game-thread lookup this section says Class E needs was
  never actually required - `applyTreatments`'s existing `ownHands_`/`medNpc_`
  authority guard already makes an untargeted broadcast-except correct (row
  14), because a non-authority receiver simply ignores the relayed delta.
  `PKT_INV_XFER` remains genuinely Class E/`RELAY_FAILSAFE_LOG` - it has no
  equivalent apply-side authority guard and still needs the deferred
  `ownerOfHand()` lookup.
- **`PKT_INV_XFER` reclassified out of Class E in Phase 7 (07-01, INV-02):**
  the deferred `ownerOfHand()` lookup this section says Class E needs is now
  resolved on the AUTHOR's game thread instead (`srcOwnerId`/`dstOwnerId` on
  the wire, protocol 58) - the host never needs its own hand lookup, so the
  packet is a host-terminated CLIENT-REQUEST (Class C shape,
  `routingClassOf(PKT_INV_XFER) == RELAY_NONE`), not a Class E cross-owner
  route. The host arbitrates and broadcasts the single verdict on the new
  `PKT_XFER_COMMIT` (Class B, row 64) instead of routing the raw intent
  anywhere.
- **`PKT_WORLD_ITEM_CLAIM` reclassified out of Class E in Phase 7 (07-02,
  INV-03):** this section's own "Relay implemented in Phase 3 (03-01),
  partially" bullet above described the OLD semantics - a routable
  destination field (`authorId`) unicast straight to the item's author, who
  destroyed its real ground object unconditionally on notice. That shape is
  exactly what let two simultaneous claims for the same item each keep a
  copy (no arbitration ever ran). The claim is now a host-terminated
  CLIENT-REQUEST (Class C shape, `routingClassOf(PKT_WORLD_ITEM_CLAIM) ==
  RELAY_NONE`), carrying `authorClaimMs` so the host's `ClaimArbiter.h`
  contention window can map it; the host arbitrates and broadcasts the
  single deterministic winner on `PKT_CLAIM_VERDICT` (Class B, row 66)
  instead of routing the raw claim to the author.

### Class F - Bootstrap

- **Sender:** either role, depending on direction - the connecting client
  sends `PKT_HELLO`; the host sends everything else in this class
  (`PKT_WELCOME`, the `PKT_SAVE_*` transfer).
- **Owner:** N/A - session/connect-time or bulk-transfer traffic, not
  per-tick replicated state.
- **Destination(s):** the one specific client the bootstrap concerns (the
  connecting client for HELLO/WELCOME, the receiving client for a save
  transfer).
- **Relay rule:** split within the class. The connect-time handshake
  (`PKT_WELCOME`) is **already correctly targeted at any N** - it bypasses
  the generic outbound queue and sends inline against the actual connecting
  `ENetPeer` handle. The save-transfer sub-group (`PKT_SAVE_BEGIN/FILE/DONE`)
  shares Class D's gap: it funnels through the generic
  `enet_host_broadcast`-or-`serverPeer_` primitive, so a late joiner's save
  bootstrap at N≥3 would broadcast to every already-connected client instead
  of just the joiner - **needs the same `sendTo(playerId)` primitive as
  Class D**, and this is a functional requirement (MAIN_GOAL's "bootstrap
  transfer targeted only to joiner; no global reset"), not just a
  security/efficiency nicety.
- **Relay implemented in Phase 3 (03-01), partially:** `PKT_HELLO`/`PKT_WELCOME`
  needed no change (already correctly targeted inline). The
  `PKT_SAVE_BEGIN/FILE/DONE` save-transfer sub-group routes to
  `NetLink::routingClassOf()` -> `RELAY_FAILSAFE_LOG`: these structs carry no
  destination-player field on the wire at all (RESEARCH.md Pitfall 4), so the
  host logs `relay FAILSAFE no-relay type=.. player=..` and drops rather than
  broadcasting a late-joiner's save transfer to every already-connected
  client. Full `sendTo(playerId)`-based bootstrap targeting remains deferred
  to a later Milestone-B phase.

### Security note (all classes)

Two rules the relay implementation must enforce, referenced from the
per-class rules above (STRIDE threat T-01-02, this plan's threat model):

- **Class A/E - `ownerId` validation.** When the host relays a Class A or
  Class E packet, it must stamp/validate the packet's `ownerId` (or
  `authorId`/`xferOwnerId` routing field) against the sender-peer-to-`PlayerId`
  mapping the host itself assigned at connect time (`ev.peer->data`,
  `NetLink.cpp:491`) - never trust a client's self-reported ownership field at
  face value, and never let a relayed packet be re-attributed to the host.
- **Class C/D - no blind rebroadcast.** A Class C packet (client-request) must
  never be relayed to any other client - it terminates at the host by
  definition. A Class D packet (targeted-response) must reach only the one
  requesting client, even once the relay layer and any `broadcastExcept`-style
  primitive exist - falling back to a broadcast to route around a missing
  unicast primitive would leak host-only-intended data (e.g. `PKT_STEALTH`'s
  detection map) to clients that were never meant to see it.

## Proposed Protocol Changes (proposal only - no implementation)

These are candidate directions for Phase 3's relay layer, informed by the
gaps this matrix surfaces. None of this is implemented in Phase 1 - this
section documents intent so Phase 3 does not have to re-derive it.

1. **Add `sendTo(PlayerId)` and `broadcastExcept(PlayerId)` primitives to
   `NetLink`.** Every Class D/E packet and the Class F save-transfer
   sub-group need a true unicast; every Class A relay-from-join needs a
   broadcast-minus-the-author. Both can be built on the existing
   `enet_peer_send`/`enet_host_broadcast` primitives plus a `PlayerId ->
   ENetPeer*` map the host already implicitly has via `ev.peer->data` (the
   connect handler already stores the assigned id there, `NetLink.cpp:491`).
2. **Introduce an explicit relay-dispatch table keyed by `PacketType` ->
   routing class**, generated from (or checked against) this matrix, so the
   host's receive switch can look up "relay to all except author" (A),
   "no relay, apply locally" (B/C), "unicast to requester" (D), "unicast to
   resolved other owner" (E), or "unicast to bootstrap target" (F) instead of
   hard-coding per-packet-type relay logic ad hoc.
3. **Validate `ownerId` at the relay boundary**, not just at apply time: when
   the host relays a Class A/E packet, it should stamp or reject the
   `ownerId` field against the sender-peer's own assigned `PlayerId` (the
   `ev.peer->data` mapping), so a client cannot forge another player's
   `ownerId` on a packet it relays through the host. This is a proposal for
   Phase 3's implementation; Phase 1 makes no code change.
4. **Extend the connect-time guard** (`if (id >= 2) netErr(...)`,
   `NetLink.cpp:487`) from a warning into a real per-`PacketType`-class
   dispatch once the relay layer exists, rather than removing it outright -
   the guard should stay until every Class A/D/E/F(save) gap above is closed,
   so a 3rd/4th connection never silently desyncs in the interim.

## Verification Strategy

How later phases should confirm this matrix stays accurate and that the
relay implementation it describes actually satisfies it:

1. **Static completeness (this phase, automated):**
   `scripts/tests/RoutingMatrix.Tests.ps1`, wired into `scripts/verify.ps1`
   step 4, asserts every `PacketType`/`EventType` value in `Wire.h` has
   exactly one row here, and that no row references a retired/renamed
   identifier. This runs on every commit with zero build/game requirement.
2. **Relay-dispatch unit tests (Phase 3, when the relay layer lands):** a
   `prototest`-style unit (mirroring `src/prototest/main.cpp`'s `CHECK`
   convention) that feeds the relay-dispatch table every `PacketType` and
   asserts the returned routing behavior (broadcast / unicast / no-relay)
   matches this matrix's Class column - a drift check the same shape as
   this phase's own completeness check, one level up the stack.
3. **Scenario-level regression (Phase 3+, 3-4 process harness):** once
   `scripts/regress.ps1`'s multi-client harness exists (MAIN_GOAL's
   Milestone A POC gate), add scenarios that specifically probe the gaps
   this matrix surfaces - a Class D response with 3 clients connected
   (assert only the requester receives it), a Class E interaction between
   join2 and join3 relayed via the host (assert join1 does NOT receive it),
   and a late-join Class F save-transfer with 2 already-connected clients
   (assert only the joiner receives the transfer).
4. **Security-relevant relay checks (Phase 3):** a scenario that has a
   client attempt to relay a packet with a forged `ownerId` (not its own
   assigned `PlayerId`) and asserts the host rejects/logs it rather than
   relaying the forged attribution - the enforcement side of the `ownerId`
   validation this matrix's Relay rule column already calls out as a
   requirement.
5. **Manual review (per `01-VALIDATION.md`, non-blocking this phase):**
   routing-class semantic correctness (a packet classified into a
   plausible-but-wrong class) is a judgment call automated checks cannot
   fully replace. The call-site verification pass in this phase (Task 2)
   is where that judgment was exercised and is recorded inline in each
   row's Notes/Relay-rule cell with the exact file:line evidence.
