# Cross-machine test rig — Windows host + Steam Deck join over a tailnet

How to run a KenshiCoop **N=3 gate on two physical machines**: a Windows host
plus a local Windows join, and a third join on a Steam Deck reached over a
private mesh VPN (Tailscale or equivalent). This is the configuration that
first surfaced the simultaneous-join census divergence, and the only one on
record where *both* host-vs-join pairs diverged — so it is the sharpest
available test, not a bonus.

Everything here is machine-independent on purpose. Substitute your own values
for `<deck-tailnet-name>`, `<host-tailnet-addr>` and your install paths. **Do
not commit real addresses, hostnames or key material into this file.**

> **Scope note.** This is *not* standard N=3 coverage. Standard N=3 coverage is
> `scripts/repro_census_n3.ps1` driving `scripts/local.rig.n3.example.json`
> across three local clones over loopback. This document covers the additional
> cross-machine leg only.

---

## 0. Why the reproducer cannot drive this

`scripts/repro_census_n3.ps1` refuses — before building, deploying or launching
anything — a rig that does not declare exactly 3 instances, and it launches
every declared instance **locally**. The remote participant is not launchable
from the Windows rig, so the cross-machine leg composes the same pieces by
hand:

| Piece | Role |
|---|---|
| `scripts/run_test4.ps1` | launches the **local** host + join1 pair |
| the Deck | runs the third participant (`join2`) |
| `scripts/analyze_run4.ps1` at `-ExpectedInstances 3` | the **unchanged** offline oracle produces the verdict |

The oracle already supports this offline three-log shape. **Nothing in the
reproducer, the oracle, the rig templates, the launcher's stagger default or
the netsim knob defaults may be modified to accommodate this leg.**

---

## 1. Prerequisites on the Deck (one-time)

The Deck runs Kenshi through Proton. Assume the default SteamOS user `deck` and
a normal Steam library:

```
$HOME/.local/share/Steam/steamapps/common/Kenshi        # install
$HOME/.local/share/Steam/steamapps/compatdata/233860/pfx # Proton prefix
```

**Key-based SSH** from the Windows machine to `deck@<deck-tailnet-name>` must
work non-interactively (`ssh -o BatchMode=yes … 'echo ok'`).

### RE_Kenshi loads as an Ogre plugin — there is no `winmm` proxy

This trips people who know other Kenshi mod loaders. RE_Kenshi is loaded by
Ogre from the game's own plugin list, not by DLL-name hijacking:

```
# $HOME/.local/share/Steam/steamapps/common/Kenshi/Plugins_x64.cfg
PluginFolder=.\
Plugin=RenderSystem_Direct3D11_x64
Plugin=Plugin_ParticleUniverse_x64
Plugin=Plugin_Terrain_x64
Plugin=RE_Kenshi
```

**Consequences:**

- **No `WINEDLLOVERRIDES` is involved.** Do not add one; it will not help and
  it obscures real failures.
- The vanilla file must be preserved as `Plugins_x64_vanilla.cfg` before it is
  edited, and `kenshi_x64.exe` as `kenshi_x64_vanilla.exe`, so the rollback in
  section 8 stays valid.

### The VC10 runtime DLLs must sit beside the exe

```
$HOME/.local/share/Steam/steamapps/common/Kenshi/mfc100u.dll
$HOME/.local/share/Steam/steamapps/common/Kenshi/msvcp100.dll
$HOME/.local/share/Steam/steamapps/common/Kenshi/msvcr100.dll
```

The plugin is built with the Visual C++ 2010 (v100) toolset. Without these
three files **the game does not start at all** — not "the mod fails to load".
If Kenshi exits instantly under Proton, check these first.

### Never launch through Steam's URL handler or `-applaunch`

```
# WRONG - both route to Remote Play and open the game on the WINDOWS PC:
steam://rungameid/233860
steam -applaunch 233860
```

Launch Proton directly instead (section 3). If you find yourself watching
Kenshi start on the wrong machine, this is why.

---

## 2. STEP 0 — remote binary provenance (MANDATORY, every single run)

**This is a gate, not a warm-up, and it is not a one-off.** Run it before
*every* cross-machine measurement. A Deck carrying a stale DLL will happily
produce a green verdict that says nothing about the code you think you are
testing — and a defect that escapes detection even one run in three makes that
green verdict actively misleading. "I copied the file" is not verification.

1. **Build and deploy on Windows.**

   ```
   scripts\build_plugin.cmd Harness
   scripts\deploy.cmd "<host install>"  Harness
   scripts\deploy.cmd "<join1 install>" Harness
   certutil -hashfile src\plugin\x64\Harness\KenshiCoop.dll SHA256
   ```

   Never deploy into the real Steam Kenshi install; use dedicated clones.

2. **Record what is being replaced, and back it up.** On the Deck, *before*
   overwriting anything — and with Kenshi killed first, because a loaded DLL is
   the usual reason a deploy silently no-ops:

   ```
   pkill -f 'kenshi_x64[.]exe'; sleep 3
   M=$HOME/.local/share/Steam/steamapps/common/Kenshi/mods/KenshiCoop
   PRE=$(sha256sum "$M/KenshiCoop.dll" | cut -d' ' -f1)
   echo "$PRE"; ls -la --time-style=full-iso "$M/KenshiCoop.dll"
   mkdir -p $HOME/rekit/backup
   cp -p "$M/KenshiCoop.dll" "$HOME/rekit/backup/KenshiCoop.dll.prefix-${PRE:0:8}"
   ```

   **Name the backup after the hash it holds.** A plain
   `cp … $HOME/rekit/backup/` writes the same basename every time, so the
   *second* cross-machine run silently destroys the first run's superseded
   binary — the one artifact that proves what the Deck was carrying before.
   Because STEP 0 runs before every measurement, that overwrite is the normal
   case, not an edge case.

3. **Deploy and prove the copy landed.** Confirm the mod directory really is
   where this install loads from (`RE_Kenshi_log.txt` shows
   `RE_Kenshi: KenshiCoop -> KenshiCoop.dll`) — do not assume it:

   ```
   scp src/plugin/x64/Harness/KenshiCoop.dll deck@<deck-tailnet-name>:"$M/KenshiCoop.dll"
   ssh deck@<deck-tailnet-name> sha256sum "$M/KenshiCoop.dll"
   ```

   **The two SHA-256 values must be equal.** If they are not, the deploy did
   not take — stop and fix it. Do not proceed.

4. **Prove it loads, from the Deck's own coop log.** Run one **unmeasured**
   verification launch (section 3). No host is needed: a join boots to the
   title screen, brings networking up and retry-connects every 2 s, writing its
   banner long before it needs a peer. Then read:

   ```
   KenshiCoop: build <Mon DD YYYY HH:MM:SS>
   KenshiCoop: role=JOIN proto=vNN port=… save='…'
   ```

   The build stamp must be the fresh build's, and `proto` must match the
   Windows side (`docs/PROTOCOL_HISTORY.md` for the current version). Assert
   the protocol **now**: a mismatch is rejected at handshake by design and
   would otherwise surface as an unexplained connection failure mid-run.

5. **Stop the provenance launch before the gate run.** It is a full game
   process holding the same port and save fixture. Kill it explicitly
   (`pkill -f 'kenshi_x64[.]exe'`) and close the ssh session that started it;
   a leftover provenance instance would join the measured run as a fourth,
   undeclared participant.

6. **Record it.** Both SHA-256 values, the superseded hash and its mtime, the
   build stamp and the protocol, in a provenance file kept with the run
   artifacts. This launch is a provenance check, **not** a gate run — do not
   record it in `tools/test-runs/census_repro_history.jsonl`.

   **Append, do not overwrite.** When a later run re-runs STEP 0, keep the
   earlier measurement in a `history` array inside the same provenance file
   rather than replacing it. The value of a provenance record is that it can
   be read back later; a record that only ever holds the newest measurement
   cannot answer "what was the Deck running when run 1 was judged?".

> **Worked example — this gate is not theatre.** Phase 13 plan 13-02 rebuilt
> the Harness configuration on Windows for the loopback leg. That changed
> nothing in `src/`, but it changed the binary's `__DATE__`/`__TIME__` stamp
> and therefore its hash. At the start of plan 13-03 the Deck was still
> carrying plan 13-01's copy (`f533540d…`, stamp `Sep 12 2026 07:47:28`)
> against the current Windows build (`a96e0790…`, stamp
> `Sep 12 2026 08:20:29`). Nothing else in the pipeline would have noticed:
> the protocol matched, the run would have completed, and the verdict would
> have been green. **A local rebuild of any kind makes the remote copy stale.**

---

## 3. Launching Kenshi on the Deck

Use a launcher that starts Proton directly and dismisses the two modal dialogs
that block an unattended boot. The reference implementation used by this
project lives on the Deck at `~/rekit/launch_auto.sh`; it wraps a
`~/rekit/launch.sh` that sets the Steam compat variables and `exec`s

```
"$RUNTIME/run" -- "$PROTON/proton" waitforexitandrun "$KENSHI/kenshi_x64.exe"
```

then waits for and dismisses, in order:

1. the **"Steam Error"** dialog (Kenshi cannot reach the Steam client when
   started outside the Steam UI — non-fatal, Enter = OK), and
2. the **Kenshi launcher** MFC dialog (Enter = default button = start game).

**Use keyboard injection, not mouse clicks.** XTEST mouse clicks are swallowed
by KWin/Wayland; `xdotool key --clearmodifiers Return` gets through. Export
`DISPLAY=:0`, `XDG_RUNTIME_DIR=/run/user/1000` and an `XAUTHORITY` pointing at
`/run/user/1000/xauth_*` before calling `xdotool`.

### Launch it as a user unit, so it survives the ssh session

```
ssh deck@<deck-tailnet-name>   'systemd-run --user --unit=kenshicoop --collect --setenv=DISPLAY=:0      --setenv=XDG_RUNTIME_DIR=/run/user/1000 $HOME/rekit/launch_auto.sh'
```

`setsid`, `nohup` and `&` + immediate disconnect all **die with the ssh
session** — systemd-logind reaps the session scope when the connection closes,
and the orphaned children go with it. `tmux new-session -d` does **not** work
either: it tears the session down as soon as its command returns and takes the
children with it.

A **user unit** is the thing that survives, because it lives in the user
manager rather than in the ssh session's scope. Verified 2026-09-12: a client
started this way kept running across ssh disconnection and reconnection for a
whole co-op session. Inspect and stop it with:

```
ssh deck@<deck-tailnet-name> 'systemctl --user status kenshicoop'
ssh deck@<deck-tailnet-name> 'systemctl --user stop   kenshicoop'
```

`--collect` makes the unit disappear once it exits, so the same `--unit` name
can be reused on the next run without a `reset-failed`. An attached ssh session
(`ssh -o ServerAliveInterval=20 deck@… '$HOME/rekit/launch_auto.sh'`) still
works and is fine for a short run you intend to watch, but it ties the run's
lifetime to a network connection for no benefit.

**Put the environment in a small per-run wrapper on the Deck, not on the ssh
command line.** Write `~/rekit/<run>_launch.sh` containing the exports below
followed by `exec "$HOME/rekit/launch_auto.sh"`, `scp` it over, `chmod +x`,
then run *that* one path over ssh. A single
`ssh deck 'A=1 B=2 … ~/rekit/launch_auto.sh'` has to survive two levels of
shell quoting and is where a silently-dropped variable hides; the wrapper is
also the artifact that records exactly what the remote participant's
environment was. Strip CRLFs (`sed -i 's/\r$//'`) if it was authored on
Windows — a `\r` on the shebang line makes the Deck report
`bad interpreter`.

### The Deck's environment

Neither `launch_auto.sh` nor `launch.sh` sets any `KENSHICOOP_*` variable —
the caller exports them. For a gate run, export exactly:

```
KENSHICOOP_MODE=join            KENSHICOOP_SAVE=<save fixture>
KENSHICOOP_TRANSPORT=udp        KENSHICOOP_SCENARIO=milestone_a_gate
KENSHICOOP_STEAM_PEER=0         KENSHICOOP_TEST_SECONDS=<same as -Seconds>
KENSHICOOP_PORT=<host port>     KENSHICOOP_CELL_AUTH=0
KENSHICOOP_IP=<host-tailnet-addr>
KENSHICOOP_LOG=$HOME/rekit/<run>.log
```

**`KENSHICOOP_CELL_AUTH=0` is not optional.** `scripts/CoopHarness.psm1`
(`Set-CoopDiagEnv`) pins it to `0` for every instance the Windows harness
launches. A remote join started outside that harness inherits the **shipped**
default instead, and you get a gate run with one participant on a different
configuration — which is exactly what happened in this project's archived
pre-fix cross-machine run (`cellAuth=1` on the Deck against `cellAuth=0`
locally). See section 6.

**Set no `KENSHICOOP_NETSIM_*` key on the Deck.** Injected receive-side delay
masks the simultaneous-join census race just as a deferred launch does; it is
one of the three forbidden escapes (`docs/PHASE_12_GATE.md`), and the remote
machine is not a back door around that rule.

---

## 4. The save fixture must be identical on both machines

The host's connect-push **bakes the live world over the loaded save**, so a
Deck that has taken part in a previous run no longer holds the pristine
fixture. `scripts/run_test4.ps1` restores the fixture into every *local*
instance's save root before each run; do the same for the Deck by hand:

```
# Windows (Git Bash)
tar -C fixtures/saves -cf /tmp/fixture.tar <save-name>
scp /tmp/fixture.tar deck@<deck-tailnet-name>:/tmp/

# Deck - prefix AppData save root
S=$HOME/.local/share/Steam/steamapps/compatdata/233860/pfx/drive_c/users/steamuser/AppData/Local/kenshi/save
rm -rf "$S/<save-name>" && tar -C "$S" -xf /tmp/fixture.tar
```

**Keep the archive path free of a Windows drive letter.** GNU tar parses
`C:\…` / `C:/…` as `host:path` and fails with
`tar: Cannot connect to C: resolve failed`. Use a POSIX-style path
(`/tmp/…`, or `cygpath -u` the destination first).

### Verify the whole save tree, not just `quick.save`

```
# both machines - must print the same 14 lines
cd <save root>/<save-name> && find . -type f | sort | \
  while read f; do echo "$(sha256sum "$f" | cut -d' ' -f1) ${f#./}"; done
```

A single `sha256sum quick.save` is **not** sufficient, because the drift this
step exists to undo is not confined to `quick.save`. Measured on this
project's Deck before cross-machine run 2: the drifted `wanderer4` held **27**
files against the repo fixture's **14** — thirteen extra `platoon/` files
(`Dust Bandits_0`, `Herbivore_0`, four `Holy Nation Outlaws_*`, five
`Starving Bandits_*`, `Trade Ninjas_1`) baked in by the previous run's
connect-push, plus different `zone/` and `portraits_texture.png` contents. The
`rm -rf` above removes them — but if anyone ever extracts *over* the existing
directory, a `quick.save`-only check passes while the extra bodies remain and
the census oracle silently compares two different worlds.

---

## 5. Running the gate

### 5a. Rig file

Copy `scripts/local.rig.n3.crossmachine.example.json` to
`scripts/local.rig.n3.crossmachine.json` (gitignored) and set the two local
`installDir` paths. It declares **2** instances on purpose — the remote
participant is deliberately absent, because `run_test4.ps1` would otherwise try
to launch it locally. Leave `env: {}` and both `reconnectAtSec` /
`disconnectAtSec` null.

The host binds `ENET_HOST_ANY`, so it serves the loopback join and the remote
join on the same port simultaneously; the rig's `ip` stays `127.0.0.1` (the
address **join1** dials) and only the Deck dials the routable address.

### 5b. Launch order — remote join FIRST

This is a constraint, not a preference. A join whose networking is already
running **never auto-loads its own save** (`titleUpdate_hook` early-returns);
it reaches gameplay only by receiving the host's connect-push, and it retries
connect every 2 s until a host exists. Starting the host first has already cost
this project a wasted measurement run.

1. Start the Deck join (section 3). Wait until its log shows the
   `effective cfg` banner and a `[boot] title-tick` line — it is now at the
   title screen with networking up, retry-connecting.
2. Start the Windows pair.

### 5c. The Windows pair — use an ABSOLUTE `-OutDir`

```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_test4.ps1 ^
  -RigConfig scripts\local.rig.n3.crossmachine.json ^
  -Scenario milestone_a_gate ^
  -ExpectedInstances 2 ^
  -Seconds 210 ^
  -OutDir <ABSOLUTE path>\tools\test-runs\<stamp>_N3_xdeck
```

> **Gotcha that costs a whole run.** `run_test4.ps1` passes `-OutDir` straight
> into each instance's `KENSHICOOP_LOG`. The game's working directory is its
> own install dir, so a **relative** `-OutDir` is resolved against
> `…\KenshiCoop-CloneN\`, where `CoopLog.cpp`'s `logInit` →
> `std::fopen(path,"w")` fails **silently** (`g_fp` stays NULL, no error is
> raised) and **no coop log is written at all**. The run appears to succeed and
> yields nothing to judge. `scripts/repro_census_n3.ps1` avoids this by
> building `Join-Path $repoRoot …`; the hand-composed cross-machine leg must
> supply the absolute path itself.

`-ExpectedInstances 2` matches what this rig launches. `-Seconds` should match
the value the loopback leg uses, so the two are comparable, and the Deck's
`KENSHICOOP_TEST_SECONDS` must be the same number.

### 5d. Collect

```
scp deck@<deck-tailnet-name>:$HOME/rekit/<run>.log <runDir>/join2.log
```

**Leave `run_meta.json` exactly as `run_test4.ps1` wrote it** —
`instanceCount=2`, `scheduledReconnect=[]`, `diagEnv={}`. It is a truthful
record of what the *Windows rig* launched; patching it to look like a
three-instance launch would be a lie about the run. Describe the third
participant in a sibling `xmachine_meta.json` instead:

```json
{
  "remoteRole": "join2",
  "remoteLogName": "join2.log",
  "remoteMachine": "steam-deck (SteamOS/Proton)",
  "transport": "udp over tailnet",
  "dllSha256": "<from the STEP 0 record>",
  "buildStamp": "<from join2.log>",
  "proto": "<from join2.log>",
  "deferred": false,
  "netsimKeys": [],
  "launchedAtUtc": "<ISO-8601 UTC>",
  "fetchedFrom": "deck:<path>",
  "expectedInstances": 3
}
```

### 5e. Judge with the unchanged oracle, at N=3

```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze_run4.ps1 ^
  -RunDir <runDir> -ExpectedInstances 3
```

**Capture the full console output to `<runDir>/judge.log`.** The per-pair
census figures exist *only* there — `verdict.json`'s `census_convergence` entry
carries an empty `metrics` object. Read the two `host vs joinK: …` lines and
the final `WNPC4 RESULT:` line from `judge.log`.

On Windows PowerShell, `Tee-Object` writes UTF-16, which `grep`, `git` and any
non-PowerShell check cannot read. Every later verification in this project
greps `judge.log`, so capture it as UTF-8 **without a BOM** in the first place:

```powershell
$out  = & powershell -NoProfile -ExecutionPolicy Bypass -File scripts\analyze_run4.ps1 `
          -RunDir $runDir -ExpectedInstances 3 2>&1
$text = ($out | ForEach-Object { $_.ToString() }) -join "`r`n"
[System.IO.File]::WriteAllText(
    (Join-Path $runDir 'judge.log'), $text,
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host $text
```

`New-Object System.Text.UTF8Encoding($false)` is the part that matters:
`-Encoding utf8` in Windows PowerShell 5.1 writes a BOM, and a BOM breaks a
first-line match.

---

## 6. Verify the participants actually ran the same gate

Before trusting any verdict, check all four:

1. **Same binary.** The STEP 0 SHA-256 equality (section 2).
2. **Same build, all three logs.** `KenshiCoop: build …` identical in
   `host.log`, `join1.log` and `join2.log`.
3. **Same protocol.** `proto=vNN` identical in all three.
4. **Same configuration, the two joins.** `join1.log` and `join2.log` must
   carry **identical** `KenshiCoop: effective cfg …` text from `scenario=`
   onward. Compare them programmatically, not by eye.

If any residual difference remains at step 4, **record it verbatim in the run's
write-up rather than hiding it**, and state plainly what it means for
comparability with any earlier run that had a different configuration. A
cross-machine run whose remote participant differs in configuration from the
local ones is not a byte-identical replay of anything, and must not be
presented as one.

Also confirm no escape was taken: `NET SIM on` must appear in none of the three
logs, `run_meta.json` must show `scheduledReconnect=[]` and `diagEnv={}`, and
`xmachine_meta.json` must show `deferred=false`, `netsimKeys=[]`.

---

## 7. Record the run

Append **exactly one** record per run performed — pass *and* fail alike — to
`tools/test-runs/census_repro_history.jsonl`, in the same schema
`scripts/repro_census_n3.ps1` writes, plus the additive `config` key:

```json
{"timestamp":"<UTC ISO-8601>","runDir":"<abs run dir>","pass":true,"census_convergence":"PASS","headSha":"<short sha>","config":"crossmachine-deck"}
```

Format timestamps with `[System.Globalization.CultureInfo]::InvariantCulture` —
this project is developed under a uk-UA locale and the default formatter does
not produce the expected shape.

**A run is never discarded and re-run to obtain a cleaner set.** A failure is
data: record it with its per-pair figures and stop.

---

## 8. Rollback — returning the Deck to vanilla

Everything installed was backed up first, so the Deck can be restored fully:

```
K=$HOME/.local/share/Steam/steamapps/common/Kenshi
rm -f  "$K/mods/KenshiCoop/KenshiCoop.dll" "$K/KenshiLib.dll" "$K/CompressToolsLib.dll"
rm -rf "$K/RE_Kenshi"
mv "$K/kenshi_x64_vanilla.exe"     "$K/kenshi_x64.exe"
mv "$K/Plugins_x64_vanilla.cfg"    "$K/Plugins_x64.cfg"
rm -rf $HOME/rekit
```

Restore the save fixture directory from its `*.PRISTINE-BAK` sibling if the
Deck's own saves matter. Verify by launching Kenshi normally through Steam and
confirming `RE_Kenshi_log.txt` is no longer produced.

---

## Related

- `docs/PHASE_12_GATE.md` — the census race: reproducer, root cause, fix, and
  the three forbidden escapes.
- `docs/BUILD_SETUP.md` — the Visual C++ 2010 (v100) toolchain.
- `docs/PROTOCOL_HISTORY.md` — the current `PROTOCOL_VERSION`.
- `scripts/local.rig.n3.crossmachine.example.json` — the Windows-side rig
  template for this shape.
- `scripts/local.rig.n3.example.json` + `scripts/repro_census_n3.ps1` — the
  standard local N=3 coverage this document does **not** replace.
