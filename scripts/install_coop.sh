#!/usr/bin/env bash
# =============================================================================
# install_coop.sh - install / uninstall / inspect KenshiCoop in a Kenshi
#                   install that runs through Proton or Wine.
#
# This is the POSIX front end of the SAME contract scripts/install_coop.ps1
# implements on Windows: the same prerequisite checks, the same
# content-addressed hash-verified backups, and the same INSTALL-MANIFEST.json,
# so one uninstall contract covers both platforms.
#
# The Proton facts encoded here are NOT derived fresh - they come from
# docs/CROSS_MACHINE_RIG.md, which a live Phase 13 run proved:
#
#   * RE_Kenshi loads as an OGRE PLUGIN through Plugins_x64.cfg. There is no
#     winmm DLL proxy, so no WINEDLLOVERRIDES is involved. Suggesting one
#     obscures real failures, so this script never mentions it.        (sec. 1)
#   * mfc100u.dll / msvcp100.dll / msvcr100.dll must sit BESIDE THE EXE.
#     Without them the game does not start at all - it exits with 0xc0000135
#     before any log file exists.                                      (sec. 1)
#   * steam://rungameid/233860 and "steam -applaunch 233860" route to Remote
#     Play and open the game on the WRONG machine. This script therefore
#     invokes neither the Steam client nor a Steam URL handler, anywhere.
#                                                                      (sec. 1)
#   * A loaded DLL is the usual reason a deploy silently no-ops: close the
#     game before installing.                                          (sec. 2)
#   * Name a backup after the hash of what it holds, or a second run destroys
#     the first run's superseded copy.                                 (sec. 2)
#
# jq is NOT assumed: the manifest is emitted with printf and read back with a
# small awk JSON flattener further down.
#
# LINE ENDINGS: this file must stay LF-only. A carriage return on the shebang
# line makes SteamOS report "bad interpreter" (CROSS_MACHINE_RIG sec. 3).
#
# Usage:
#   install_coop.sh --kenshi-dir DIR [--source DIR] [--out-dir DIR] [--force]
#   install_coop.sh --kenshi-dir DIR --uninstall [--out-dir DIR] [--force]
#   install_coop.sh --kenshi-dir DIR --info
#   install_coop.sh --hash-tree DIR [--exclude GLOB]...
# =============================================================================

set -euo pipefail

INSTALLER_VERSION="1.0.0"
SCHEMA_VERSION=1
PLATFORM="linux-proton"

CRT_DLLS="mfc100u.dll msvcp100.dll msvcr100.dll"
SUPPORTED_KENSHI_VERSIONS="1.0.65"
OGRE_PLUGIN_LINE="Plugin=RE_Kenshi"
MOD_LIST_ENTRY="KenshiCoop.mod"
PAYLOAD_NAMES="KenshiCoop.dll RE_Kenshi.json KenshiCoop.mod"

REKENSHI_URL="https://www.nexusmods.com/kenshi/mods/847"
REKENSHI_WHY="Without it the co-op plugin is never loaded and the game runs vanilla."

MOD_DIR_REL="mods/KenshiCoop"
BACKUP_DIR_REL="mods/KenshiCoop.backup"
MANIFEST_NAME="INSTALL-MANIFEST.json"

SCRIPT_PATH="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
SCRIPT_DIR="$(dirname "$SCRIPT_PATH")"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

# ----------------------------------------------------------------- arguments
KENSHI_DIR=""
SOURCE_DIR=""
OUT_DIR=""
HASH_TREE_DIR=""
DO_UNINSTALL=0
DO_INFO=0
FORCE=0
DRY_RUN=0
KEEP_BACKUPS=0
CRT_SEARCH_PATH=""
EXCLUDES=""

usage() {
    cat <<'USAGE'
install_coop.sh - install, uninstall or inspect KenshiCoop in a Proton/Wine
                  Kenshi install. Same contract as scripts/install_coop.ps1.

MODES
  --kenshi-dir DIR     the Kenshi install folder (the one holding
                       kenshi_x64.exe). Required for every mode except
                       --hash-tree and --help.
  --uninstall          reverse INSTALL-MANIFEST.json, verifying every hash.
  --info               read the installed build back. Writes nothing, and does
                       not launch the game.
  --hash-tree DIR      print one sha256 per file plus a marker per EMPTY
                       directory, ordinal-sorted. Byte-identical to
                       scripts/hash_tree.ps1 for the same tree.
  --help               this text.

OPTIONS
  --source DIR         where KenshiCoop.dll, RE_Kenshi.json and KenshiCoop.mod
                       are read from. Auto-detected beside this script when
                       omitted.
  --out-dir DIR        ABSOLUTE path for the run record. A relative path is
                       refused, because it is swallowed silently.
  --force              install over an existing install; on --uninstall, remove
                       or overwrite files that changed since they were
                       installed.
  --dry-run            print the whole plan and touch nothing.
  --keep-backups       --uninstall only: leave the backup directory in place.
  --crt-search-path P  ':'-separated extra directories to look for the VC++
                       2010 x64 runtime in, searched after the exe's folder
                       and the Proton prefix.
  --exclude GLOB       --hash-tree only, repeatable: omit entries whose
                       relative path matches GLOB.

The install is reversed with --uninstall. Nothing here launches Kenshi.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --kenshi-dir)       KENSHI_DIR="${2:-}"; shift 2 ;;
        --source)           SOURCE_DIR="${2:-}"; shift 2 ;;
        --out-dir)          OUT_DIR="${2:-}"; shift 2 ;;
        --hash-tree)        HASH_TREE_DIR="${2:-}"; shift 2 ;;
        --crt-search-path)  CRT_SEARCH_PATH="${2:-}"; shift 2 ;;
        --exclude)          EXCLUDES="$EXCLUDES${EXCLUDES:+$'\n'}${2:-}"; shift 2 ;;
        --uninstall)        DO_UNINSTALL=1; shift ;;
        --info)             DO_INFO=1; shift ;;
        --force)            FORCE=1; shift ;;
        --dry-run)          DRY_RUN=1; shift ;;
        --keep-backups)     KEEP_BACKUPS=1; shift ;;
        --help|-h)          usage; exit 0 ;;
        *) printf 'REFUSED: unknown option "%s". Run --help for the list of modes.\n' "$1"; exit 3 ;;
    esac
done

# ------------------------------------------------------------------- helpers
JOURNAL_FILE=""

refuse() {
    undo_journal
    printf 'REFUSED: %s\n' "$1"
    exit 3
}

now_utc() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

sha256() {
    # Empty output means "no such file"; callers treat that as absent.
    [ -f "$1" ] || { printf ''; return 0; }
    LC_ALL=C sha256sum -- "$1" | cut -d' ' -f1
}

norm_dir() {
    # Absolute, no trailing slash. No realpath dependency: SteamOS has it, but
    # a minimal Wine box may not.
    ( cd "$1" 2>/dev/null && pwd -P ) || printf ''
}

relpath() { # relpath ROOT ABS
    local root="$1" abs="$2"
    case "$abs" in
        "$root"/*) printf '%s' "${abs#"$root"/}" ;;
        "$root")   printf '' ;;
        *)         printf '%s' "$abs" ;;
    esac
}

absfrom() { printf '%s/%s' "$1" "$2"; }

json_escape() {
    # Only the escapes JSON requires. Product strings here are ASCII.
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\t'/\\t}"
    s="${s//$'\r'/\\r}"
    s="${s//$'\n'/\\n}"
    printf '%s' "$s"
}

jstr() { printf '"%s"' "$(json_escape "$1")"; }

jbool() { if [ "$1" = "1" ] || [ "$1" = "true" ]; then printf 'true'; else printf 'false'; fi; }

write_text_nobom() { # write_text_nobom PATH   (text on stdin)
    cat > "$1"
}

# ------------------------------------------------------- rollback journal
journal_init() {
    JOURNAL_FILE="$(mktemp "${TMPDIR:-/tmp}/coopinst.XXXXXX")"
}
journal_add() { [ -n "$JOURNAL_FILE" ] && printf '%s\t%s\n' "$1" "$2" >> "$JOURNAL_FILE"; }
journal_clear() { [ -n "$JOURNAL_FILE" ] && : > "$JOURNAL_FILE"; }

undo_journal() {
    # Newest first. Only this invocation's own writes are undone.
    [ -n "$JOURNAL_FILE" ] || return 0
    [ -s "$JOURNAL_FILE" ] || return 0
    printf '  rolling back what this run already wrote:\n'
    local kind path
    while IFS=$'\t' read -r kind path; do
        case "$kind" in
            file-created) [ -e "$path" ] && rm -f -- "$path" && printf '    removed %s\n' "$path" ;;
            file-backed-up)
                # path is "<original>|<backup>"
                local orig bak
                orig="${path%%|*}"; bak="${path#*|}"
                if [ -f "$bak" ]; then cp -p -- "$bak" "$orig" && printf '    restored %s\n' "$orig"; fi ;;
            dir-created) [ -d "$path" ] && rmdir -- "$path" 2>/dev/null && printf '    removed %s/\n' "$path" ;;
        esac
    done < <(tac "$JOURNAL_FILE")
    : > "$JOURNAL_FILE"
}

# ============================================================== HASH TREE ====
# Byte-identical to scripts/hash_tree.ps1:
#   <64 lowercase hex><space><relative/path><LF>   for a file
#   emptydir<space><relative/path>/<LF>            for an EMPTY directory
# sorted by the SORT KEY (the relative path, with a trailing "/" for a
# directory) under LC_ALL=C so the ordering is byte order, not a locale
# collation. This machine's Windows counterpart uses [StringComparer]::Ordinal
# for exactly the same reason.
excluded() {
    local rel="$1" pat
    [ -n "$EXCLUDES" ] || return 1
    while IFS= read -r pat; do
        [ -n "$pat" ] || continue
        # shellcheck disable=SC2254
        case "$rel" in $pat) return 0 ;; esac
    done <<< "$EXCLUDES"
    return 1
}

hash_tree() {
    local root
    root="$(norm_dir "$1")"
    if [ -z "$root" ]; then
        printf "REFUSED: there is no directory at '%s'.\n" "$1"
        exit 3
    fi

    {
        local f d rel h
        while IFS= read -r -d '' f; do
            rel="${f#"$root"/}"
            excluded "$rel" && continue
            h="$(LC_ALL=C sha256sum -- "$f" | cut -d' ' -f1)"
            printf '%s\t%s %s\n' "$rel" "$h" "$rel"
        done < <(find "$root" -mindepth 1 -type f -print0)

        while IFS= read -r -d '' d; do
            rel="${d#"$root"/}"
            excluded "$rel/" && continue
            printf '%s/\t%s %s/\n' "$rel" "emptydir" "$rel"
        done < <(find "$root" -mindepth 1 -type d -empty -print0)
    } | LC_ALL=C sort -t"$(printf '\t')" -k1,1 | cut -f2-
}

if [ -n "$HASH_TREE_DIR" ]; then
    hash_tree "$HASH_TREE_DIR"
    exit 0
fi

# ========================================================= JSON FLATTENER ====
# Reads a JSON document on stdin and prints "path<TAB>value" lines, e.g.
#     .schemaVersion	1
#     .files[0].path	mods/KenshiCoop/KenshiCoop.dll
# Whitespace-insensitive, so it reads a manifest written by either front end.
JSON_FLATTEN_AWK='
function hex2dec(h,   i,c,v,d) {
    v = 0
    for (i = 1; i <= length(h); i++) {
        c = tolower(substr(h, i, 1)); d = index("0123456789abcdef", c) - 1
        if (d < 0) d = 0
        v = v * 16 + d
    }
    return v
}
function skipws() {
    while (i <= n) { c = substr(s, i, 1); if (c == " " || c == "\t" || c == "\n" || c == "\r") i++; else break }
}
function pstring(   out, ch, esc) {
    i++
    out = ""
    while (i <= n) {
        ch = substr(s, i, 1)
        if (ch == "\\") {
            i++; esc = substr(s, i, 1); i++
            if (esc == "n") out = out "\n"
            else if (esc == "t") out = out "\t"
            else if (esc == "r") out = out "\r"
            else if (esc == "b") out = out "\b"
            else if (esc == "f") out = out "\f"
            else if (esc == "u") { out = out sprintf("%c", hex2dec(substr(s, i, 4))); i += 4 }
            else out = out esc
        } else if (ch == "\"") { i++; return out }
        else { out = out ch; i++ }
    }
    return out
}
function pvalue(path,   ch, k, idx, v) {
    skipws(); ch = substr(s, i, 1)
    if (ch == "{") {
        i++; skipws()
        if (substr(s, i, 1) == "}") { i++; print path "\t" "{}"; return }
        while (1) {
            skipws(); k = pstring(); skipws(); i++
            pvalue(path "." k)
            skipws(); ch = substr(s, i, 1); i++
            if (ch == "}") return
            if (ch != ",") return
        }
    } else if (ch == "[") {
        i++; idx = 0; skipws()
        if (substr(s, i, 1) == "]") { i++; print path "\t" "[]"; return }
        while (1) {
            pvalue(path "[" idx "]"); idx++
            skipws(); ch = substr(s, i, 1); i++
            if (ch == "]") return
            if (ch != ",") return
        }
    } else if (ch == "\"") { v = pstring(); print path "\t" v }
    else {
        v = ""
        while (i <= n) {
            ch = substr(s, i, 1)
            if (ch == "," || ch == "}" || ch == "]" || ch == " " || ch == "\n" || ch == "\r" || ch == "\t") break
            v = v ch; i++
        }
        print path "\t" v
    }
}
{ s = s $0 "\n" }
END { n = length(s); i = 1; pvalue("") }
'

flat_get() { # flat_get FLATFILE KEY
    LC_ALL=C awk -F'\t' -v k="$2" '$1==k { sub(/^[^\t]*\t/, ""); print; exit }' "$1"
}
flat_count() { # flat_count FLATFILE PREFIX SUFFIX  -> number of array members
    LC_ALL=C awk -F'\t' -v p="$2" -v sfx="$3" '
        index($1, p) == 1 {
            rest = substr($1, length(p) + 1)
            if (match(rest, /^[0-9]+\]/)) {
                idx = substr(rest, 1, RLENGTH - 1) + 0
                if (idx + 1 > m) m = idx + 1
            }
        }
        END { print m + 0 }' "$1"
}

# ============================================================== DETECTION ====
proton_prefix_for() { # proton_prefix_for KENSHIDIR
    local d="$1"
    case "$d" in
        */steamapps/common/*) printf '%s/steamapps/compatdata/233860/pfx' "${d%%/steamapps/common/*}" ;;
        *) printf '' ;;
    esac
}

resolve_kenshi_dir() {
    local given="$1"
    [ -n "$given" ] || refuse "no Kenshi folder was given. Pass --kenshi-dir with the folder that holds kenshi_x64.exe (under Proton that is normally \$HOME/.local/share/Steam/steamapps/common/Kenshi)."
    local d
    d="$(norm_dir "$given")"
    [ -n "$d" ] || refuse "there is no directory at '$given'."
    if [ ! -f "$d/kenshi_x64.exe" ]; then
        refuse "'$d' does not look like a Kenshi install: kenshi_x64.exe is not in it. Point --kenshi-dir at the game folder itself."
    fi
    printf '%s' "$d"
}

get_kenshi_version() { # -> "found|parsed|version|raw"
    local dir="$1" file="$1/currentVersion.txt" raw="" ver=""
    if [ ! -f "$file" ]; then printf '0|0||'; return 0; fi
    raw="$(tr -d '\r\n' < "$file" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    ver="$(printf '%s' "$raw" | LC_ALL=C grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1 || true)"
    if [ -z "$ver" ]; then printf '1|0||%s' "$raw"; return 0; fi
    printf '1|1|%s|%s' "$ver" "$raw"
}

# Prerequisite results, filled by detect_prerequisites.
CRT_OK=0; CRT_MISSING=""; CRT_FOUND_AT=""; CRT_SEARCHED=""
REDIST_PATH=""; REDIST_PRESENT=0
REK_OK=0; REK_PATH="RE_Kenshi.dll"
OGRE_OK=0; OGRE_CFG_REL="Plugins_x64.cfg"; OGRE_CFG_FOUND=0; OGRE_LINE_FOUND=0; OGRE_REPAIRED=0
KV_FOUND=0; KV_PARSED=0; KV_VERSION=""; KV_RAW=""; KV_OK=0
PREREQ_CHECKED_UTC=""

detect_prerequisites() {
    local dir="$1"
    PREREQ_CHECKED_UTC="$(now_utc)"

    # --- 1. the VC++ 2010 x64 runtime, BY FILE PRESENCE ONLY (D-04) --------
    # Beside the exe FIRST: under Proton that is where the loader looks, and
    # CROSS_MACHINE_RIG sec. 1 records that their absence kills the game
    # before any log exists. The Proton prefix's system32 is consulted second
    # and recorded, because a runtime installed into the prefix does satisfy
    # the game even though the files are not beside the exe.
    local pfx search n at p
    pfx="$(proton_prefix_for "$dir")"
    search="$dir"
    [ -n "$pfx" ] && search="$search:$pfx/drive_c/windows/system32"
    if [ -n "$CRT_SEARCH_PATH" ]; then search="$search:$CRT_SEARCH_PATH"; fi
    CRT_SEARCHED="$search"

    CRT_MISSING=""; CRT_FOUND_AT=""
    for n in $CRT_DLLS; do
        at=""
        local IFS_SAVE="$IFS"; IFS=':'
        for p in $search; do
            IFS="$IFS_SAVE"
            [ -n "$p" ] || continue
            if [ -f "$p/$n" ]; then at="$p"; break; fi
            IFS=':'
        done
        IFS="$IFS_SAVE"
        [ -n "$at" ] || CRT_MISSING="$CRT_MISSING${CRT_MISSING:+, }$n"
        CRT_FOUND_AT="$CRT_FOUND_AT${CRT_FOUND_AT:+$'\n'}$n|$at"
    done
    if [ -z "$CRT_MISSING" ]; then CRT_OK=1; else CRT_OK=0; fi

    REDIST_PATH="$dir/dependencies/vcredist_x64.exe"
    if [ -f "$REDIST_PATH" ]; then REDIST_PRESENT=1; else REDIST_PRESENT=0; fi

    # --- 2. the RE_Kenshi loader -------------------------------------------
    if [ -f "$dir/RE_Kenshi.dll" ]; then REK_OK=1; else REK_OK=0; fi

    # --- 3. the Ogre plugin registration -----------------------------------
    # Separate from "RE_Kenshi is absent": it fails and is fixed differently.
    local cfg="$dir/Plugins_x64.cfg"
    OGRE_LINE_FOUND=0
    if [ -f "$cfg" ]; then
        OGRE_CFG_FOUND=1
        if LC_ALL=C grep -qiE '^[[:space:]]*Plugin[[:space:]]*=[[:space:]]*RE_Kenshi[[:space:]]*$' "$cfg"; then
            OGRE_LINE_FOUND=1
        fi
    else
        OGRE_CFG_FOUND=0
    fi
    OGRE_OK=$OGRE_LINE_FOUND

    # --- 4. the Kenshi version ---------------------------------------------
    local kv; kv="$(get_kenshi_version "$dir")"
    KV_FOUND="${kv%%|*}"; kv="${kv#*|}"
    KV_PARSED="${kv%%|*}"; kv="${kv#*|}"
    KV_VERSION="${kv%%|*}"; KV_RAW="${kv#*|}"
    KV_OK=0
    local sv
    for sv in $SUPPORTED_KENSHI_VERSIONS; do
        [ "$KV_VERSION" = "$sv" ] && KV_OK=1
    done
}

show_prerequisites() {
    printf '  prerequisites:\n'
    if [ "$CRT_OK" = "1" ]; then
        local line name at where=""
        while IFS= read -r line; do
            name="${line%%|*}"; at="${line#*|}"
            where="$where${where:+; }$name <- $at"
        done <<< "$CRT_FOUND_AT"
        printf '    [ok]   VC++ 2010 x64 runtime: %s\n' "$where"
    else
        printf '    [MISS] VC++ 2010 x64 runtime: missing %s\n' "$CRT_MISSING"
    fi
    if [ "$REK_OK" = "1" ]; then printf '    [ok]   RE_Kenshi loader: %s\n' "$REK_PATH"
    else printf '    [MISS] RE_Kenshi loader: %s\n' "$REK_PATH"; fi
    if [ "$OGRE_OK" = "1" ]; then printf "    [ok]   Ogre plugin line '%s' in %s\n" "$OGRE_PLUGIN_LINE" "$OGRE_CFG_REL"
    else printf "    [FIX]  Ogre plugin line '%s' in %s\n" "$OGRE_PLUGIN_LINE" "$OGRE_CFG_REL"; fi
    if [ "$KV_FOUND" = "1" ] && [ "$KV_PARSED" = "1" ]; then
        if [ "$KV_OK" = "1" ]; then printf '    [ok]   Kenshi version: %s (supported: %s)\n' "$KV_VERSION" "$SUPPORTED_KENSHI_VERSIONS"
        else printf '    [MISS] Kenshi version: %s (supported: %s)\n' "$KV_VERSION" "$SUPPORTED_KENSHI_VERSIONS"; fi
    elif [ "$KV_FOUND" = "1" ]; then
        printf '    [MISS] Kenshi version: currentVersion.txt is unreadable as a version\n'
    else
        printf '    [MISS] Kenshi version: currentVersion.txt is missing\n'
    fi
}

assert_prerequisites() {
    local dir="$1"
    if [ "$CRT_OK" != "1" ]; then
        local hint
        if [ "$REDIST_PRESENT" = "1" ]; then
            hint="Run $REDIST_PATH under the same Proton or Wine prefix this Kenshi uses - Kenshi ships that installer in its own folder - then run this script again. Its exit code is NOT evidence under Wine: it has been seen to report success and install nothing, so this script re-checks the three names above afterwards instead of trusting it."
        else
            hint="Install the Microsoft Visual C++ 2010 x64 redistributable into the Proton or Wine prefix this Kenshi uses (Kenshi normally ships it as dependencies/vcredist_x64.exe), then run this script again. An installer's exit code is NOT evidence under Wine; this script re-checks the three names above afterwards."
        fi
        refuse "the Microsoft Visual C++ 2010 x64 runtime is missing. KenshiCoop needs $(printf '%s' "$CRT_DLLS" | tr ' ' ',' | sed 's/,/, /g'), and $CRT_MISSING could not be found in: $(printf '%s' "$CRT_SEARCHED" | tr ':' ';' | sed 's/;/; /g'). Under Proton these three files must sit beside kenshi_x64.exe. $hint Without this runtime the game does not start at all: it exits with error 0xc0000135 before any log file is created, so there is nothing to read afterwards."
    fi
    if [ "$REK_OK" != "1" ]; then
        refuse "RE_Kenshi is not installed in '$dir': RE_Kenshi.dll is not there. $REKENSHI_WHY Install RE_Kenshi from $REKENSHI_URL and run this script again."
    fi
    if [ "$KV_FOUND" != "1" ]; then
        refuse "currentVersion.txt is missing from '$dir', so this script cannot tell which Kenshi version you have. This is not an unsupported version - the file that states the version is not there. Check that --kenshi-dir points at the game folder itself (the one holding kenshi_x64.exe), or verify the game files through Steam."
    fi
    if [ "$KV_PARSED" != "1" ]; then
        refuse "currentVersion.txt in '$dir' does not contain a version number. It reads: '$KV_RAW'. Verify the game files through Steam so the file is rewritten, then run this script again."
    fi
    if [ "$KV_OK" != "1" ]; then
        refuse "this Kenshi is version $KV_VERSION, and KenshiCoop supports $SUPPORTED_KENSHI_VERSIONS. currentVersion.txt reads: '$KV_RAW'. Update or roll back Kenshi to $SUPPORTED_KENSHI_VERSIONS and run this script again."
    fi
}

# ============================================================== PAYLOAD ======
PAYLOAD_DIR=""
PAYLOAD_FROM_REPO=0

resolve_payload() {
    local given="$1" c d ok n
    local candidates=""
    if [ -n "$given" ]; then
        candidates="$given"
    else
        candidates="$SCRIPT_DIR/payload
$REPO_ROOT/dist/mod-kit
$REPO_ROOT/dist/mod-kit/mods/KenshiCoop
$SCRIPT_DIR"
    fi
    while IFS= read -r c; do
        [ -n "$c" ] || continue
        d="$(norm_dir "$c")"
        [ -n "$d" ] || continue
        ok=1
        for n in $PAYLOAD_NAMES; do [ -f "$d/$n" ] || ok=0; done
        if [ "$ok" = "1" ]; then PAYLOAD_DIR="$d"; break; fi
    done <<< "$candidates"

    if [ -z "$PAYLOAD_DIR" ]; then
        if [ -n "$given" ]; then
            refuse "'$given' does not hold the files this installer writes. It needs all of: $PAYLOAD_NAMES."
        fi
        refuse "the mod files to install could not be found. Pass --source with the folder holding $PAYLOAD_NAMES."
    fi
    case "$PAYLOAD_DIR" in
        "$REPO_ROOT"/*) PAYLOAD_FROM_REPO=1 ;;
        *) PAYLOAD_FROM_REPO=0 ;;
    esac
}

repo_head_sha() {
    ( cd "$REPO_ROOT" 2>/dev/null && git rev-parse HEAD 2>/dev/null ) || printf ''
}

# ---------------------------------------------------------- build / version
read_build_stamp() { # read_build_stamp DLL -> "stamp" or ""
    local dll="$1" anchor date_off time_off best_date="" best_time="" off txt
    [ -f "$dll" ] || { printf ''; return 0; }
    anchor="$(LC_ALL=C grep -aob 'KenshiCoop: build' -- "$dll" 2>/dev/null | head -n1 | cut -d: -f1 || true)"
    [ -n "$anchor" ] || { printf ''; return 0; }
    # __DATE__ and __TIME__ are emitted as literals within ~1 KB of the format
    # string. Take the pair nearest the anchor.
    while IFS= read -r line; do
        off="${line%%:*}"; txt="${line#*:}"
        if [ $((off - anchor)) -gt -2048 ] && [ $((off - anchor)) -lt 2048 ]; then
            best_date="$txt"; break
        fi
    done < <(LC_ALL=C grep -aobE '[A-Z][a-z][a-z] [ 0-9][0-9] [0-9]{4}' -- "$dll" 2>/dev/null || true)
    while IFS= read -r line; do
        off="${line%%:*}"; txt="${line#*:}"
        if [ $((off - anchor)) -gt -2048 ] && [ $((off - anchor)) -lt 2048 ]; then
            best_time="$txt"; break
        fi
    done < <(LC_ALL=C grep -aobE '[0-9]{2}:[0-9]{2}:[0-9]{2}' -- "$dll" 2>/dev/null || true)
    if [ -n "$best_date" ] && [ -n "$best_time" ]; then printf '%s %s' "$best_date" "$best_time"; else printf ''; fi
}

PROTO_VALUE=""; PROTO_SOURCE=""
resolve_protocol() {
    # Scoped to THE PAYLOAD. A VERSION.txt that quotes an unrelated repo's
    # protocol number is worse than none (16-01 deviation 3).
    PROTO_VALUE=""; PROTO_SOURCE=""
    local p="$PAYLOAD_DIR" v=""
    if [ -f "$p/PROVENANCE.json" ]; then
        v="$(LC_ALL=C grep -oE '"protocol"[^0-9]*([0-9]+)' "$p/PROVENANCE.json" | LC_ALL=C grep -oE '[0-9]+' | head -n1 || true)"
        if [ -n "$v" ]; then PROTO_VALUE="$v"; PROTO_SOURCE="PROVENANCE.json"; return 0; fi
    fi
    if [ -f "$p/../PROVENANCE.json" ]; then
        v="$(LC_ALL=C grep -oE '"protocol"[^0-9]*([0-9]+)' "$p/../PROVENANCE.json" | LC_ALL=C grep -oE '[0-9]+' | head -n1 || true)"
        if [ -n "$v" ]; then PROTO_VALUE="$v"; PROTO_SOURCE="PROVENANCE.json"; return 0; fi
    fi
    if [ "$PAYLOAD_FROM_REPO" = "1" ] && [ -f "$REPO_ROOT/src/netproto/Wire.h" ]; then
        v="$(LC_ALL=C grep -oE 'PROTOCOL_VERSION[^0-9]*([0-9]+)' "$REPO_ROOT/src/netproto/Wire.h" | LC_ALL=C grep -oE '[0-9]+' | head -n1 || true)"
        if [ -n "$v" ]; then PROTO_VALUE="$v"; PROTO_SOURCE="src/netproto/Wire.h"; return 0; fi
    fi
}

build_version_text() { # build_version_text DLLPATH DIR
    local dll="$1" dir="$2"
    local stamp stamp_from proto proto_from dllsha dllbytes
    stamp="$(read_build_stamp "$dll")"
    if [ -n "$stamp" ]; then stamp_from="read out of KenshiCoop.dll"; else stamp="unknown"; stamp_from="NOT RECOVERABLE from KenshiCoop.dll"; fi
    resolve_protocol
    if [ -n "$PROTO_VALUE" ]; then proto="$PROTO_VALUE"; proto_from="read from $PROTO_SOURCE"; else proto="unknown"; proto_from="NOT RECOVERABLE"; fi
    dllsha="$(sha256 "$dll")"
    dllbytes="$(LC_ALL=C wc -c < "$dll" | tr -d ' ')"
    cat <<EOF
KenshiCoop - installed build
===========================

You do not need to launch Kenshi to read this file.
Every line says where its value came from.

build:             $stamp    ($stamp_from)
protocol:          $proto    ($proto_from)
dll sha256:        $dllsha
dll bytes:         $dllbytes
installer version: $INSTALLER_VERSION
installed (UTC):   $(now_utc)
kenshi version:    $KV_VERSION
install directory: $dir
platform:          $PLATFORM

Both players must run the same protocol version: a mismatch is rejected at
handshake by design, with no backwards compatibility. If your friend cannot
connect, compare the 'protocol' line in this file on both machines.

Written by scripts/install_coop.sh. To remove the mod, run that script
again with --uninstall.
EOF
}

# ------------------------------------------------------------------- backups
new_content_addressed_backup() { # SOURCEFILE BACKUPDIR -> "abs|sha"
    local src="$1" bdir="$2" sha base dest existing back
    sha="$(sha256 "$src")"
    [ -n "$sha" ] || refuse "could not hash '$src' before backing it up, so this script will not overwrite it."
    base="$(basename -- "$src")"
    dest="$bdir/$base.${sha:0:8}.bak"
    if [ -f "$dest" ]; then
        existing="$(sha256 "$dest")"
        if [ "$existing" = "$sha" ]; then printf '%s|%s' "$dest" "$sha"; return 0; fi
        refuse "the backup file '$dest' already exists but holds different content ($existing instead of $sha). That name encodes its content, so this is a corrupted backup directory. Move it aside and run this script again."
    fi
    cp -p -- "$src" "$dest"
    [ -f "$dest" ] || refuse "the backup copy of '$src' did not land at '$dest'. Nothing has been overwritten."
    back="$(sha256 "$dest")"
    if [ "$back" != "$sha" ]; then
        refuse "the backup of '$src' does not match the file it was taken from (backup $back, original $sha). Nothing has been overwritten."
    fi
    printf '%s|%s' "$dest" "$sha"
}

# --------------------------------------------------------------- line editing
dominant_eol() { # dominant_eol PATH -> "lf" | "crlf"
    local cr=0 lf=0 bare
    if [ -f "$1" ]; then
        cr="$(LC_ALL=C tr -dc '\r' < "$1" | LC_ALL=C wc -c | tr -d ' ')"
        lf="$(LC_ALL=C tr -dc '\n' < "$1" | LC_ALL=C wc -c | tr -d ' ')"
    fi
    bare=$((lf - cr))
    if [ "$bare" -gt "$cr" ]; then printf 'lf'; else printf 'crlf'; fi
}

add_line_to_file() { # add_line_to_file PATH LINE
    # Append by BYTES so every existing line, the file's encoding and its line
    # endings survive untouched; only the appended line is new.
    local path="$1" line="$2" eol prefix="" last
    if [ "$(dominant_eol "$path")" = "lf" ]; then eol=$'\n'; else eol=$'\r\n'; fi
    if [ -s "$path" ]; then
        last="$(LC_ALL=C tail -c 1 -- "$path" | LC_ALL=C od -An -tu1 | tr -d ' \n')"
        if [ "$last" != "10" ] && [ "$last" != "13" ]; then prefix="$eol"; fi
    fi
    printf '%s%s%s' "$prefix" "$line" "$eol" >> "$path"
}

line_present() { # line_present PATH EREGEX
    [ -f "$1" ] || return 1
    LC_ALL=C grep -qiE "$2" "$1"
}

# ================================================================= INFO ======
invoke_info() {
    local dir="$1" man="$dir/$BACKUP_DIR_REL/$MANIFEST_NAME" ver="$dir/$MOD_DIR_REL/VERSION.txt"
    printf 'KenshiCoop install info\n'
    printf '  kenshi dir: %s\n' "$dir"
    printf '  platform:   %s\n' "$PLATFORM"
    if [ -f "$ver" ]; then
        printf '  --- %s/VERSION.txt ---\n' "$MOD_DIR_REL"
        sed 's/^/  /' "$ver"
    else
        printf '  %s/VERSION.txt is not there, so no build is installed by this installer.\n' "$MOD_DIR_REL"
    fi
    if [ -f "$man" ]; then
        local flat; flat="$(mktemp "${TMPDIR:-/tmp}/coopman.XXXXXX")"
        LC_ALL=C awk "$JSON_FLATTEN_AWK" < "$man" > "$flat"
        printf '  manifest:          %s\n' "$BACKUP_DIR_REL/$MANIFEST_NAME"
        printf '  installer version: %s\n' "$(flat_get "$flat" ".installerVersion")"
        printf '  installed (UTC):   %s\n' "$(flat_get "$flat" ".installedUtc")"
        printf '  platform:          %s\n' "$(flat_get "$flat" ".platform")"
        printf '  supersedes:        %s\n' "$(flat_get "$flat" ".supersedes")"
        rm -f "$flat"
    else
        printf '  there is no install manifest at %s/%s.\n' "$BACKUP_DIR_REL" "$MANIFEST_NAME"
    fi
    printf '  nothing was written and the game was not launched.\n'
    exit 0
}

# ============================================================== UNINSTALL ====
invoke_uninstall() {
    local dir="$1" out="$2"
    local bdir="$dir/$BACKUP_DIR_REL" man="$dir/$BACKUP_DIR_REL/$MANIFEST_NAME"
    printf 'KenshiCoop uninstall\n'
    printf '  kenshi dir: %s\n' "$dir"
    if [ ! -f "$man" ]; then
        refuse "there is no install manifest at '$man', so this script does not know what was installed or what to put back. If you installed by hand, remove $MOD_DIR_REL yourself; nothing has been touched."
    fi
    local flat; flat="$(mktemp "${TMPDIR:-/tmp}/coopman.XXXXXX")"
    LC_ALL=C awk "$JSON_FLATTEN_AWK" < "$man" > "$flat"

    local schema; schema="$(flat_get "$flat" ".schemaVersion")"
    if [ "$schema" != "$SCHEMA_VERSION" ]; then
        rm -f "$flat"
        refuse "the install manifest at '$man' has schema version $schema, and this script understands version $SCHEMA_VERSION. Use the installer version that wrote it ($(flat_get "$flat" ".installerVersion"))."
    fi
    printf '  manifest: %s/%s (written %s by installer %s)\n' "$BACKUP_DIR_REL" "$MANIFEST_NAME" \
        "$(flat_get "$flat" ".installedUtc")" "$(flat_get "$flat" ".installerVersion")"

    # Copy the receipt out BEFORE anything is removed, so it outlives the
    # backup directory.
    if [ -n "$out" ] && [ "$DRY_RUN" != "1" ]; then
        mkdir -p "$out"; cp -p -- "$man" "$out/UNINSTALL-MANIFEST.json"
    fi

    local nfiles; nfiles="$(flat_count "$flat" ".files[" "")"
    local skipped=0 i path action shaAfter shaBefore bpath bsha cur bak_abs after
    for (( i = nfiles - 1; i >= 0; i-- )); do
        path="$(flat_get "$flat" ".files[$i].path")"
        action="$(flat_get "$flat" ".files[$i].action")"
        shaAfter="$(flat_get "$flat" ".files[$i].sha256After")"
        shaBefore="$(flat_get "$flat" ".files[$i].sha256Before")"
        bpath="$(flat_get "$flat" ".files[$i].backupPath")"
        bsha="$(flat_get "$flat" ".files[$i].backupSha256")"
        cur="$(sha256 "$dir/$path")"

        if [ "$action" = "created" ]; then
            if [ -z "$cur" ]; then
                printf '  gone     %s (nothing to remove)\n' "$path"
            elif [ "$cur" = "$shaAfter" ]; then
                [ "$DRY_RUN" = "1" ] || rm -f -- "$dir/$path"
                printf '  removed  %s\n' "$path"
            elif [ "$FORCE" = "1" ]; then
                [ "$DRY_RUN" = "1" ] || rm -f -- "$dir/$path"
                printf '  removed  %s (MODIFIED since install - removed because --force was given)\n' "$path"
            else
                skipped=$((skipped + 1))
                printf '  LEFT     %s - it has changed since it was installed, so it was NOT deleted.\n' "$path"
                printf '           installed %s, now %s. Delete it yourself, or re-run with --force.\n' "$shaAfter" "$cur"
            fi
        elif [ "$action" = "replaced" ]; then
            bak_abs="$dir/$bpath"
            if [ ! -f "$bak_abs" ]; then
                rm -f "$flat"
                refuse "the backup '$bpath' that '$path' must be restored from is missing. Nothing has been restored from this entry and nothing further will be touched. Put the backup back and run this again."
            fi
            local bakcur; bakcur="$(sha256 "$bak_abs")"
            if [ "$bakcur" != "$bsha" ]; then
                rm -f "$flat"
                refuse "the backup '$bpath' no longer matches what was recorded for it ($bakcur instead of $bsha). Restoring it could put wrong content into '$path', so nothing has been touched."
            fi
            if [ -n "$cur" ] && [ "$cur" != "$shaAfter" ] && [ "$FORCE" != "1" ]; then
                skipped=$((skipped + 1))
                printf '  LEFT     %s - it has changed since it was installed, so the backup was NOT restored over it.\n' "$path"
                printf '           installed %s, now %s. Re-run with --force to restore the backup anyway.\n' "$shaAfter" "$cur"
            else
                if [ "$DRY_RUN" != "1" ]; then
                    cp -p -- "$bak_abs" "$dir/$path"
                    after="$(sha256 "$dir/$path")"
                    if [ "$after" != "$shaBefore" ]; then
                        rm -f "$flat"
                        refuse "the restore of '$path' did not produce the content recorded before the install ($after instead of $shaBefore). The file is left as restored; do not run the game until this is resolved."
                    fi
                else
                    after="$shaBefore"
                fi
                printf '  restored %s <- %s\n' "$path" "$bpath"
                printf '           verified sha256 %s == the content recorded before the install\n' "$after"
            fi
        else
            rm -f "$flat"
            refuse "the install manifest records an action this script does not understand ('$action') for '$path'. Nothing further has been touched."
        fi
    done

    # ---- the backups this manifest owns -----------------------------------
    if [ "$skipped" -gt 0 ]; then
        printf '\n  %d file(s) were left in place, so the backups are KEPT: %s\n' "$skipped" "$BACKUP_DIR_REL"
    elif [ "$KEEP_BACKUPS" = "1" ]; then
        printf '\n  --keep-backups given: the backups and the manifest stay in %s\n' "$BACKUP_DIR_REL"
    else
        for (( i = 0; i < nfiles; i++ )); do
            bpath="$(flat_get "$flat" ".files[$i].backupPath")"
            [ -n "$bpath" ] || continue
            if [ -f "$dir/$bpath" ]; then
                [ "$DRY_RUN" = "1" ] || rm -f -- "$dir/$bpath"
            fi
        done
        [ "$DRY_RUN" = "1" ] || rm -f -- "$man"
    fi

    # ---- directories, most-nested first, only when empty -------------------
    local ndirs d abs left
    ndirs="$(flat_count "$flat" ".createdDirs[" "")"
    for (( i = 0; i < ndirs; i++ )); do
        d="$(flat_get "$flat" ".createdDirs[$i]")"
        [ -n "$d" ] || continue
        abs="$dir/$d"
        [ -d "$abs" ] || continue
        left="$(find "$abs" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')"
        if [ "$left" = "0" ]; then
            [ "$DRY_RUN" = "1" ] || rmdir -- "$abs"
            printf '  removed  %s/ (empty)\n' "$d"
        else
            printf '  kept     %s/ - not empty (%s item(s) that were not put there by this installer)\n' "$d" "$left"
        fi
    done

    rm -f "$flat"
    printf '\n'
    if [ "$DRY_RUN" = "1" ]; then
        printf 'DRY-RUN: nothing above was done.\n'
    elif [ "$skipped" -gt 0 ]; then
        printf 'UNINSTALL: COMPLETE with %d file(s) left in place - see the LEFT lines above.\n' "$skipped"
    else
        printf 'UNINSTALL: COMPLETE - every restore was hash-verified against the content recorded before the install.\n'
    fi
    exit 0
}

# ================================================================ INSTALL ====
invoke_install() {
    local dir="$1" out="$2"
    local bdir="$dir/$BACKUP_DIR_REL" man="$dir/$BACKUP_DIR_REL/$MANIFEST_NAME"
    local moddir="$dir/$MOD_DIR_REL"

    printf 'KenshiCoop install\n'
    printf '  kenshi dir: %s\n' "$dir"
    printf '  payload:    %s\n' "$PAYLOAD_DIR"
    printf '  platform:   %s (Proton/Wine)\n' "$PLATFORM"

    detect_prerequisites "$dir"
    show_prerequisites
    assert_prerequisites "$dir"

    # A loaded DLL is the usual reason a deploy silently no-ops
    # (CROSS_MACHINE_RIG sec. 2). Warn rather than refuse: we cannot see the
    # process list of every environment this might run in.
    if command -v pgrep >/dev/null 2>&1; then
        if pgrep -f 'kenshi_x64[.]exe' >/dev/null 2>&1; then
            refuse "Kenshi is still running. A loaded DLL cannot be replaced, and the copy would silently do nothing. Close the game and run this script again."
        fi
    fi

    local prevman_utc=""
    local prevflat=""
    if [ -f "$man" ]; then
        prevflat="$(mktemp "${TMPDIR:-/tmp}/coopprev.XXXXXX")"
        LC_ALL=C awk "$JSON_FLATTEN_AWK" < "$man" > "$prevflat"
        prevman_utc="$(flat_get "$prevflat" ".installedUtc")"
        if [ "$FORCE" != "1" ]; then
            rm -f "$prevflat"
            refuse "KenshiCoop is already installed here (there is an install manifest at '$man', written $prevman_utc). Run this script with --uninstall first, or pass --force to install over it."
        fi
        printf '  note:    an earlier install is recorded here; --force given, so it will be installed over.\n'
    fi

    # ---- build the write plan ---------------------------------------------
    # rel <TAB> kind <TAB> arg
    local plan=""
    local n
    for n in $PAYLOAD_NAMES; do
        plan="$plan$MOD_DIR_REL/$n"$'\t'"copy"$'\t'"$PAYLOAD_DIR/$n"$'\n'
    done
    plan="$plan$MOD_DIR_REL/VERSION.txt"$'\t'"text"$'\t'$'\n'

    local need_ogre=0
    if [ "$REK_OK" = "1" ] && [ "$OGRE_OK" != "1" ] && [ "$OGRE_CFG_FOUND" = "1" ]; then
        need_ogre=1
        plan="${plan}Plugins_x64.cfg"$'\t'"append"$'\t'"$OGRE_PLUGIN_LINE"$'\n'
    elif [ "$REK_OK" = "1" ] && [ "$OGRE_CFG_FOUND" != "1" ]; then
        printf '  note:    %s does not exist; the Ogre plugin line cannot be repaired and is left alone.\n' "$OGRE_CFG_REL"
    fi

    if ! line_present "$dir/data/mods.cfg" "^[[:space:]]*$MOD_LIST_ENTRY[[:space:]]*$"; then
        if [ -f "$dir/data/mods.cfg" ] || [ -d "$dir/data" ]; then
            plan="${plan}data/mods.cfg"$'\t'"append"$'\t'"$MOD_LIST_ENTRY"$'\n'
        else
            printf '  note:    there is no data/ folder, so the mod list entry cannot be written.\n'
        fi
    fi

    # ---- directories this installer would create ---------------------------
    local dirs_needed=""
    local d
    for d in "$dir/mods" "$moddir" "$bdir"; do
        [ -d "$d" ] || dirs_needed="$dirs_needed$d"$'\n'
    done

    # ---- print the plan ----------------------------------------------------
    printf '\n  plan:\n'
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        printf '    create dir   %s\n' "$(relpath "$dir" "$d")"
    done <<< "$dirs_needed"

    local rel kind arg abs s
    while IFS=$'\t' read -r rel kind arg; do
        [ -n "$rel" ] || continue
        abs="$dir/$rel"
        if [ -f "$abs" ]; then
            s="$(sha256 "$abs")"
            printf '    back up      %s  ->  %s/%s.%s.bak\n' "$rel" "$BACKUP_DIR_REL" "$(basename -- "$abs")" "${s:0:8}"
            if [ "$kind" = "append" ]; then
                printf '    append line  %s  <<  %s\n' "$rel" "$arg"
            else
                printf '    replace      %s\n' "$rel"
            fi
        else
            printf '    create       %s\n' "$rel"
        fi
    done <<< "$plan"

    if [ "$DRY_RUN" = "1" ]; then
        if [ -n "$out" ]; then printf '\n  record would be written to: %s/INSTALL-RECORD.json\n' "$out"; else printf '\n'; fi
        printf 'DRY-RUN: nothing above was done. No file was created, replaced or backed up.\n'
        [ -n "$prevflat" ] && rm -f "$prevflat"
        exit 0
    fi

    journal_init

    while IFS= read -r d; do
        [ -n "$d" ] || continue
        mkdir -p "$d"
        journal_add "dir-created" "$d"
    done <<< "$dirs_needed"

    # ---- write, backing up first ------------------------------------------
    local entries="" created_dirs=""
    local existed before bres bak baksha bakrel after
    while IFS=$'\t' read -r rel kind arg; do
        [ -n "$rel" ] || continue
        abs="$dir/$rel"
        if [ -f "$abs" ]; then existed=1; before="$(sha256 "$abs")"; else existed=0; before=""; fi

        bak=""; baksha=""; bakrel=""
        if [ "$existed" = "1" ]; then
            bres="$(new_content_addressed_backup "$abs" "$bdir")"
            bak="${bres%%|*}"; baksha="${bres#*|}"
            bakrel="$(relpath "$dir" "$bak")"
            journal_add "file-backed-up" "$abs|$bak"
        else
            journal_add "file-created" "$abs"
        fi

        # A re-install must not lose the state the FIRST install replaced:
        # carry the EARLIEST recorded baseline forward for every path we see
        # again, or an uninstall after two installs restores install #1's file
        # rather than the player's original. (16-01 deviation 2.)
        local act="created"
        if [ -n "$prevflat" ]; then
            local j pn pcount
            pcount="$(flat_count "$prevflat" ".files[" "")"
            for (( j = 0; j < pcount; j++ )); do
                pn="$(flat_get "$prevflat" ".files[$j].path")"
                if [ "$pn" = "$rel" ]; then
                    act="$(flat_get "$prevflat" ".files[$j].action")"
                    existed="$(flat_get "$prevflat" ".files[$j].existedBefore")"
                    [ "$existed" = "true" ] && existed=1 || existed=0
                    before="$(flat_get "$prevflat" ".files[$j].sha256Before")"
                    bakrel="$(flat_get "$prevflat" ".files[$j].backupPath")"
                    baksha="$(flat_get "$prevflat" ".files[$j].backupSha256")"
                    break
                fi
            done
        fi
        if [ -z "$prevflat" ] || [ "$act" = "created" ]; then
            if [ -n "$bakrel" ]; then act="replaced"; else act="created"; fi
        fi

        case "$kind" in
            copy)  cp -p -- "$arg" "$abs" ;;
            text)  build_version_text "$dir/$MOD_DIR_REL/KenshiCoop.dll" "$dir" | write_text_nobom "$abs" ;;
            append) add_line_to_file "$abs" "$arg" ;;
        esac
        after="$(sha256 "$abs")"
        [ -n "$after" ] || refuse "'$rel' was not written. Nothing further will be done."

        # RS is 0x1f, not a tab: IFS treats a tab as whitespace and COLLAPSES
        # runs of it, which silently shifts every field that follows an empty
        # one - which is what put a "before" hash into sha256Before for a file
        # that never existed.
        entries="$entries$rel"$'\x1f'"$act"$'\x1f'"$existed"$'\x1f'"$before"$'\x1f'"$bakrel"$'\x1f'"$baksha"$'\x1f'"$after"$'\n'
        printf '  wrote     %s  (%s)\n' "$rel" "${after:0:16}"
    done <<< "$plan"

    [ "$need_ogre" = "1" ] && OGRE_REPAIRED=1

    # ---- createdDirs, most-nested first ------------------------------------
    local cdirs=""
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        cdirs="$cdirs$(relpath "$dir" "$d")"$'\n'
    done <<< "$dirs_needed"
    if [ -n "$prevflat" ]; then
        local k pd pdcount
        pdcount="$(flat_count "$prevflat" ".createdDirs[" "")"
        for (( k = 0; k < pdcount; k++ )); do
            pd="$(flat_get "$prevflat" ".createdDirs[$k]")"
            [ -n "$pd" ] || continue
            case $'\n'"$cdirs" in *$'\n'"$pd"$'\n'*) ;; *) cdirs="$cdirs$pd"$'\n' ;; esac
        done
    fi
    # Most-nested first: depth is the primary key, length breaks ties.
    created_dirs="$(printf '%s' "$cdirs" | LC_ALL=C awk 'NF { n = gsub("/", "/"); print n "\t" length($0) "\t" $0 }' | LC_ALL=C sort -k1,1nr -k2,2nr | cut -f3-)"

    # ---- manifest: the canonical receipt and the uninstall's only input ----
    {
        printf '{\n'
        printf '    "schemaVersion":  %d,\n' "$SCHEMA_VERSION"
        printf '    "installerVersion": %s,\n' "$(jstr "$INSTALLER_VERSION")"
        printf '    "installedUtc":   %s,\n' "$(jstr "$(now_utc)")"
        printf '    "kenshiDir":      %s,\n' "$(jstr "$dir")"
        printf '    "platform":       %s,\n' "$(jstr "$PLATFORM")"
        printf '    "repoHeadSha":    %s,\n' "$(jstr "$(repo_head_sha)")"
        printf '    "supersedes":     %s,\n' "$(jstr "$prevman_utc")"
        printf '    "prerequisites": {\n'
        printf '        "checkedUtc": %s,\n' "$(jstr "$PREREQ_CHECKED_UTC")"
        printf '        "crt": {\n'
        printf '            "ok": %s,\n' "$(jbool "$CRT_OK")"
        printf '            "required": ['
        local first=1
        for n in $CRT_DLLS; do [ "$first" = "1" ] || printf ', '; printf '%s' "$(jstr "$n")"; first=0; done
        printf '],\n'
        printf '            "searched": ['
        first=1
        local IFS_SAVE="$IFS"; IFS=':'
        for n in $CRT_SEARCHED; do IFS="$IFS_SAVE"; [ "$first" = "1" ] || printf ', '; printf '%s' "$(jstr "$n")"; first=0; IFS=':'; done
        IFS="$IFS_SAVE"
        printf '],\n'
        printf '            "dlls": ['
        first=1
        local line name at
        while IFS= read -r line; do
            name="${line%%|*}"; at="${line#*|}"
            [ "$first" = "1" ] || printf ','
            printf '\n                { "name": %s, "foundAt": %s }' "$(jstr "$name")" "$(jstr "$at")"
            first=0
        done <<< "$CRT_FOUND_AT"
        printf '\n            ],\n'
        printf '            "missing": ['
        first=1
        if [ -n "$CRT_MISSING" ]; then
            local m
            local IFS_SAVE2="$IFS"; IFS=','
            for m in $CRT_MISSING; do IFS="$IFS_SAVE2"; m="$(printf '%s' "$m" | sed 's/^ *//')"; [ "$first" = "1" ] || printf ', '; printf '%s' "$(jstr "$m")"; first=0; IFS=','; done
            IFS="$IFS_SAVE2"
        fi
        printf '],\n'
        printf '            "redistPath": %s,\n' "$(jstr "$REDIST_PATH")"
        printf '            "redistPresent": %s\n' "$(jbool "$REDIST_PRESENT")"
        printf '        },\n'
        printf '        "reKenshi": {\n'
        printf '            "ok": %s,\n' "$(jbool "$REK_OK")"
        printf '            "loaderPath": %s,\n' "$(jstr "$REK_PATH")"
        printf '            "url": %s\n' "$(jstr "$REKENSHI_URL")"
        printf '        },\n'
        printf '        "ogrePlugin": {\n'
        printf '            "ok": %s,\n' "$(jbool "$OGRE_OK")"
        printf '            "cfgPath": %s,\n' "$(jstr "$OGRE_CFG_REL")"
        printf '            "cfgFound": %s,\n' "$(jbool "$OGRE_CFG_FOUND")"
        printf '            "lineFound": %s,\n' "$(jbool "$OGRE_LINE_FOUND")"
        printf '            "repaired": %s\n' "$(jbool "$OGRE_REPAIRED")"
        printf '        },\n'
        printf '        "kenshiVersion": {\n'
        printf '            "file": %s,\n' "$(jstr "currentVersion.txt")"
        printf '            "found": %s,\n' "$(jbool "$KV_FOUND")"
        printf '            "raw": %s,\n' "$(jstr "$KV_RAW")"
        printf '            "parsed": %s,\n' "$(jbool "$KV_PARSED")"
        printf '            "version": %s,\n' "$(jstr "$KV_VERSION")"
        printf '            "supported": ['
        first=1
        for n in $SUPPORTED_KENSHI_VERSIONS; do [ "$first" = "1" ] || printf ', '; printf '%s' "$(jstr "$n")"; first=0; done
        printf '],\n'
        printf '            "ok": %s\n' "$(jbool "$KV_OK")"
        printf '        }\n'
        printf '    },\n'
        printf '    "files": ['
        first=1
        local e_rel e_act e_existed e_before e_bak e_baksha e_after
        while IFS=$'\x1f' read -r e_rel e_act e_existed e_before e_bak e_baksha e_after; do
            [ -n "$e_rel" ] || continue
            [ "$first" = "1" ] || printf ','
            printf '\n        {\n'
            printf '            "path": %s,\n' "$(jstr "$e_rel")"
            printf '            "action": %s,\n' "$(jstr "$e_act")"
            printf '            "existedBefore": %s,\n' "$(jbool "$e_existed")"
            printf '            "sha256Before": %s,\n' "$(jstr "$e_before")"
            printf '            "backupPath": %s,\n' "$(jstr "$e_bak")"
            printf '            "backupSha256": %s,\n' "$(jstr "$e_baksha")"
            printf '            "sha256After": %s\n' "$(jstr "$e_after")"
            printf '        }'
            first=0
        done <<< "$entries"
        printf '\n    ],\n'
        printf '    "createdDirs": ['
        first=1
        while IFS= read -r d; do
            [ -n "$d" ] || continue
            [ "$first" = "1" ] || printf ','
            printf '\n        %s' "$(jstr "$d")"
            first=0
        done <<< "$created_dirs"
        printf '\n    ]\n'
        printf '}\n'
    } | write_text_nobom "$man"

    printf '  manifest  %s/%s\n' "$BACKUP_DIR_REL" "$MANIFEST_NAME"

    if [ -n "$out" ]; then
        mkdir -p "$out"
        cp -p -- "$man" "$out/INSTALL-RECORD.json"
    fi

    journal_clear
    [ -n "$prevflat" ] && rm -f "$prevflat"

    local count; count="$(printf '%s' "$entries" | LC_ALL=C grep -c . || true)"
    printf '\n'
    printf 'INSTALL: COMPLETE - %s file(s) written, every replaced file backed up and hash-verified.\n' "$count"
    printf '  read the build back with: --kenshi-dir "%s" --info\n' "$dir"
    printf '  remove it with:           --kenshi-dir "%s" --uninstall\n' "$dir"
    [ -n "$out" ] && printf '  record: %s/INSTALL-RECORD.json\n' "$out"
    printf '\n'
    printf 'To start the game, run kenshi_x64.exe through Proton directly, for example\n'
    printf '  "$STEAM/steamapps/common/SteamLinuxRuntime_sniper/run" -- \\\n'
    printf '      "$STEAM/steamapps/common/Proton - Experimental/proton" waitforexitandrun \\\n'
    printf '      "%s/kenshi_x64.exe"\n' "$dir"
    printf 'Do NOT start it with a steam:// URL or with the Steam client "-applaunch" option:\n'
    printf 'both route through Remote Play and open the game on whichever machine your Steam\n'
    printf 'session is streaming to, which is usually not this one.\n'
    exit 0
}

# ================================================================ DISPATCH ===
if [ "$DO_UNINSTALL" = "1" ] && [ "$DO_INFO" = "1" ]; then
    printf 'REFUSED: --uninstall and --info do different things; pass one of them, not both.\n'
    exit 3
fi

if [ -n "$OUT_DIR" ]; then
    case "$OUT_DIR" in
        /*) ;;
        *) printf "REFUSED: --out-dir must be an ABSOLUTE path (got '%s'). A relative path is swallowed silently and nothing is written there.\n" "$OUT_DIR"; exit 3 ;;
    esac
fi

DIR="$(resolve_kenshi_dir "$KENSHI_DIR")"

if [ "$DO_INFO" = "1" ]; then invoke_info "$DIR"; fi

if [ "$DO_UNINSTALL" = "1" ]; then
    detect_prerequisites "$DIR" >/dev/null 2>&1 || true
    invoke_uninstall "$DIR" "$OUT_DIR"
fi

resolve_payload "$SOURCE_DIR"
invoke_install "$DIR" "$OUT_DIR"
