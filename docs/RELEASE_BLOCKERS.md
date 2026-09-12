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
| INST-AUTODETECT | Phase 16 plan 03 | `install_coop.ps1`'s auto-detection walks `%ProgramFiles%`, `%ProgramFiles(x86)%` and every ready drive looking for `steamapps\common\Kenshi`. The **consumer** of that walk is now covered; the **walk itself** has never been exercised against a real filesystem layout other than this rig's. A player whose Kenshi lives somewhere the walk does not look gets the zero-candidate refusal, which is survivable (it names `-KenshiDir`), but the walk's coverage of real-world Steam library layouts is unmeasured. | Closed in part by `scripts/tests/InstallerRelease.Tests.ps1` Group R, which drives the shipped `Resolve-KenshiDir` with exactly one, zero and two candidates and proves the one-candidate case flips to FAIL when the `@()` of `9b7665a` is reverted. Group R stubs the candidate PRODUCER, so `Find-KenshiInstalls`' own root enumeration is still untested. | open |
| KIT-PROVENANCE | Phase 16 plan 03 | `dist/KenshiCoop-friend-kit.zip` was hand-assembled for one session and carries **no `PROVENANCE.json`**. A recipient cannot tell which build they have, which protocol it speaks, or whether it came from this repo at all - which is exactly the support case INST-05 exists to remove. Anything distributed must come out of the kit pipeline so it is stamped. | `dist/friend-kit/` contains `KenshiCoop/`, `install_coop.ps1`, `install_coop.sh` and `friend-kit.zip`, and no `PROVENANCE.json`. `dist/mod-kit/PROVENANCE.json`, produced by `scripts/make_mod_kit.ps1`, is what a distributed artifact should carry. | open |

## What closed the two WINDOWS rows

The bar this file set was: Phase 14's relink judge, run **before and after** the
fix on the same schedule, with `h/no-phantom-double-slot` decisive and
`d/readmit-same-ids` holding at every boundary; no closing by reasoning about
`NetLink.cpp`; and the before/after pair non-optional because the change lands on
the use-after-free boundary `9011527` repaired.

That bar was met on 2026-09-12, with a third leg added rather than substituted:

1. **Before** - the judge run on a binary built from `b4b9b0a` with the fix
   reverted: FAIL, 4 checks, stale peer 5.402 s.
2. **After** - the judge run on the fixed binary: PASS, 0 checks.
3. **Mutation** - the judge run on the *same fixed binary* with only the
   teardown semantics reverted at runtime (`KENSHICOOP_NET_DIRTY_STOP=1`,
   harness-only): back to FAIL, and it flips exactly `d` and `h` - the two
   roster checks - while every scenario-level check stays PASS. A green suite
   that could not see the defect was the failure mode to avoid; this is the
   measurement that rules it out.

`9011527` still holds afterwards, shown rather than assumed: both host
boundaries of the passing run print `roster-reset where=stop role=host slots=2
ids={1,2}`, then an empty table, then the **same** ids re-admitted, with zero
`slots full` and zero crash dumps.

Full per-run record: `tools/test-runs/windows19_relink_fix.json`.

## Recently closed

Kept here so the gate is visibly capable of saying more than one thing.

| ID | Closed by | Evidence |
|----|-----------|----------|
| WINDOWS-19 | A clean `enet_peer_disconnect` before `enet_host_destroy`, measured before and after on the same schedule | `tools/test-runs/windows19_relink_fix.json`. **Before** (`tools/test-runs/20260912_182020_N3_relink`, pre-change binary): host `peer connected id=3` 18:22:42.779 then `peer disconnected id=1` 18:22:48.181 - stale peer alive **5.402 s**, `h/no-phantom-double-slot` FAIL at peak 3 live slots `ids={1,2,3}`, 4 failed checks. **After** (`tools/test-runs/20260912_190234_N3_relink`): `peer disconnected id=2` 19:04:53.743 then `peer connected id=2` 19:04:53.744 - **1 ms**, and the SAME id, twice in the run; `h` PASS at peak 2. **Mutation** (`tools/test-runs/20260912_190843_N3_relink`, same binary with `KENSHICOOP_NET_DIRTY_STOP=1` restoring pre-fix teardown): stale peer back to 5.402 s / 5.405 s and `failedChecks` 0 -> **2**, flipping exactly `h` and `d` while every scenario-level check stays PASS - so the roster judge is responding to the transport change and nothing else. |
| WINDOWS-22 | The same fix, plus three harness-shape corrections the fix exposed | `scripts/relink_probe.ps1` default schedule now returns **`RELINK PROBE RESULT: PASS`, 0 failed checks**, all 19 checks green, on `tools/test-runs/20260912_190234_N3_relink`. `9011527` is re-confirmed live in that same run, both boundaries: `roster-reset where=stop role=host slots=2 ids={1,2}` -> `roster-reset where=launch ... ids={}` -> `peer connected id=1` / `peer connected id=2`, with **zero** `slots full` and **zero** crash dumps on all three clones. `role=join`, run live for the first time ever (`tools/test-runs/20260912_191444_N3_relink`), shows the leak closed on that path too (`h` PASS peak 2, `e` PASS 0 slots-full, relinker 2/2); its 5 remaining failures are judge/predicate shape, recorded as WINDOWS row 24, not transport. |
| UI-01 | Phase 15 + a live human F2 session | Role chosen entirely from the panel; `scripts/check_panel_log.ps1` returned `panel check: PASS` - 4 connects, 4 role-sourced ranks, 0 problems. |
| UI-02 | Phase 15 + the same session | Transport chosen from the panel across the same 4 connects. |
| UI-03 | Phase 15 + the same session | Peer address entered and edited in the panel; `endpointSrc=panel` proves the address came from the panel while **no `coop_config.json` existed**. |
| UI-04 | Phase 15 + the same session | Connect and disconnect driven from the panel with no environment variable set; both reconnect cycles exercised (and they are what re-measured WINDOWS-19 live). |
