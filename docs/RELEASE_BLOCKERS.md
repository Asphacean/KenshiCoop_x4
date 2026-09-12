# Release Blockers

**This file gates PUBLICATION, not development.**

Building, testing and packaging the mod is legitimate work and is what Phase 16
does. Declaring the result shippable is a different claim, and this file is where
that claim is decided. A row is closed only by **evidence** - a measurement, a
run record, a check that can be re-run - never by an argument that the defect is
unlikely, unimportant or already understood.

`scripts/make_mod_kit.ps1` reads this file and derives `releaseBlockers` and
`shippable` into `dist/mod-kit/PROVENANCE.json`. `shippable` is `true` only when
the open table below is empty. The kit still packs while blockers are open, on
purpose: a packaging step that refuses drives people to a hand-rolled zip with
no provenance at all, which is how `dist/KenshiCoop-friend-kit.zip` came to
exist. A kit that states plainly what it is beats both.

This file is **tracked**. `docs/PHASE_*_GATE.md` is gitignored; this is not,
because it must survive a clone and be readable by whoever picks the project up.

## Open blockers

| ID | Ledger | Player-facing consequence | Evidence | Status |
|----|--------|---------------------------|----------|--------|
| WINDOWS-19 | `.planning/WINDOWS.md` row 19 | A player who disconnects and reconnects leaks a host slot. The client's `stop()` calls `enet_host_destroy()` with no clean `enet_peer_disconnect`, so the host keeps the stale peer until ENet times it out while the reconnect already returned and took the NEXT free id. With 2 players there is headroom; at 3 players only one slot remains, so a second reconnect inside the stale window reaches a genuine `slots full` and the player simply cannot get back in. | Measured three times, independently. Phase 14 harness: `tools/test-runs/20260912_110440_N3_relink`, host `peer connected id=3` 11:07:00.412 then `peer disconnected id=1` 11:07:05.830 (5.4 s), join2's own roster `peers=3 ids={0,1,3}`, `analyze_relink.ps1` check `h` FAIL. Phase 15 re-measurement: stale `id=1` alive 5.417 s. **Live human F2 session, 2026-09-12: 3.49 s and 3.83 s across two reconnect cycles, the join coming back as `id=2` instead of its own `id=1`.** | open |
| WINDOWS-22 | `.planning/WINDOWS.md` row 22 | The same mechanism failing the relink judge outright, so the project cannot currently demonstrate a clean reconnect at all. | `tools/test-runs/20260912_124533_N3_relink`, `tools/test-runs/phase15_relink.json`: stale `id=1` alive 5.417 s, `analyze_relink.ps1` `d/readmit-same-ids[boundary 2]` and `h/no-phantom-double-slot` both FAIL, the scenario's relink-post predicate timing out at `recoveredMs=60000 ok=0`, host `SCENARIO RESULT: FAIL`. Not a `9011527` regression and not introduced by 15-01. | open |
| INST-AUTODETECT | Phase 16 plan 03 | `install_coop.ps1`'s auto-detection walks `%ProgramFiles%`, `%ProgramFiles(x86)%` and every ready drive looking for `steamapps\common\Kenshi`. The **consumer** of that walk is now covered; the **walk itself** has never been exercised against a real filesystem layout other than this rig's. A player whose Kenshi lives somewhere the walk does not look gets the zero-candidate refusal, which is survivable (it names `-KenshiDir`), but the walk's coverage of real-world Steam library layouts is unmeasured. | Closed in part by `scripts/tests/InstallerRelease.Tests.ps1` Group R, which drives the shipped `Resolve-KenshiDir` with exactly one, zero and two candidates and proves the one-candidate case flips to FAIL when the `@()` of `9b7665a` is reverted. Group R stubs the candidate PRODUCER, so `Find-KenshiInstalls`' own root enumeration is still untested. | open |
| KIT-PROVENANCE | Phase 16 plan 03 | `dist/KenshiCoop-friend-kit.zip` was hand-assembled for one session and carries **no `PROVENANCE.json`**. A recipient cannot tell which build they have, which protocol it speaks, or whether it came from this repo at all - which is exactly the support case INST-05 exists to remove. Anything distributed must come out of the kit pipeline so it is stamped. | `dist/friend-kit/` contains `KenshiCoop/`, `install_coop.ps1`, `install_coop.sh` and `friend-kit.zip`, and no `PROVENANCE.json`. `dist/mod-kit/PROVENANCE.json`, produced by `scripts/make_mod_kit.ps1`, is what a distributed artifact should carry. | open |

## What must close the two WINDOWS rows

Phase 14's relink judge, run **before and after** any fix, on the same schedule:

- `scripts/relink_probe.ps1` drives the reconnect schedule.
- `scripts/analyze_relink.ps1` check `h/no-phantom-double-slot` is the decisive
  one; `d/readmit-same-ids` must hold at every boundary.

A fix is accepted only when the judge that currently fails passes, on a run
recorded under `tools/test-runs/`. Neither row may be closed by reasoning about
`NetLink.cpp`. Both candidate fixes change teardown semantics at the
use-after-free boundary that `9011527` repaired, so the before/after pair is not
optional.

## Recently closed

Kept here so the gate is visibly capable of saying more than one thing.

| ID | Closed by | Evidence |
|----|-----------|----------|
| UI-01 | Phase 15 + a live human F2 session | Role chosen entirely from the panel; `scripts/check_panel_log.ps1` returned `panel check: PASS` - 4 connects, 4 role-sourced ranks, 0 problems. |
| UI-02 | Phase 15 + the same session | Transport chosen from the panel across the same 4 connects. |
| UI-03 | Phase 15 + the same session | Peer address entered and edited in the panel; `endpointSrc=panel` proves the address came from the panel while **no `coop_config.json` existed**. |
| UI-04 | Phase 15 + the same session | Connect and disconnect driven from the panel with no environment variable set; both reconnect cycles exercised (and they are what re-measured WINDOWS-19 live). |
