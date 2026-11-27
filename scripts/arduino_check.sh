#!/usr/bin/env bash
# The Arduino check, as one command: builds the sketches CI builds (the
# arduino job in .github/workflows/release.yml: Minimal on esp32dev, C3 and
# S3; Bluetooth on C3 and S3, all with min_spiffs.csv) against the sibling
# worktrees, prints the firmware size table, and re-runs every library
# compile command with -fsyntax-only -std=gnu++2a, because PlatformIO builds
# the libraries at library.json's -std=gnu++17 while CI's arduino-cli
# compiles everything at the core's gnu++2a.
#
# Usage: scripts/arduino_check.sh <build-dir>
#   <build-dir> is created if missing and must be outside this repository;
#   two PlatformIO projects are generated in it, minimal/ and bluetooth/,
#   each with the sketch copied into src/. Logs stay there (build-*.log).
#
# The sibling worktrees come from TESSERAPI_DIR or TESSERAPI_PATH,
# TESSERUI_DIR or TESSERUI_PATH and NOWTP_PATH or NOWTP_DIR, defaulting to
# the directories next to this repository. Their HEADs are printed and
# compared with deps.lock's pins; a mismatch is a warning, not a failure.
# PIO=/path/to/pio overrides PlatformIO's location; without one on the PATH
# it runs through `uvx platformio`. Linux and macOS (bash 3.2).
#
# The generated projects carry no build_flags, build_unflags or
# lib_ldf_mode: lib_ldf_mode = deep+ makes the BLE transport fail with
# "BLE2902.h: No such file" (the LDF cannot evaluate CONFIG_* conditions),
# and a sketch-level -std is silently overridden by the framework's anyway.
set -euo pipefail

die() { echo "arduino_check: $*" >&2; exit 1; }

[ $# -eq 1 ] || die "usage: $(basename "$0") <build-dir>"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

# --- build dir -------------------------------------------------------------
BUILD_DIR=$(mkdir -p -- "$1" && cd -- "$1" && pwd)
case $BUILD_DIR in
    "$ROOT"|"$ROOT"/*) die "build-dir must be outside the TesserKIT worktree ($ROOT)" ;;
esac

# --- PlatformIO ------------------------------------------------------------
if [ -n "${PIO:-}" ]; then
    PIO_CMD=("$PIO")
elif command -v pio >/dev/null 2>&1; then
    PIO_CMD=(pio)
elif [ -x "$HOME/.local/bin/pio" ]; then
    PIO_CMD=("$HOME/.local/bin/pio")
elif command -v uvx >/dev/null 2>&1; then
    PIO_CMD=(uvx platformio)
else
    die "PlatformIO not found (set PIO=/path/to/pio, or install uv)"
fi
pio() { "${PIO_CMD[@]}" "$@"; }

# pioarduino's platform for the Arduino-ESP32 core the arduino job builds
# with: its release names carry ESP-IDF's version too (55.03.37 is core
# 3.3.7 on ESP-IDF 5.5), so the pair is kept here and checked against CI.
CORE=3.3.7
PLATFORM=55.03.37
ci_core=$(awk '/name: esp32:esp32/ { f = 1 } f && /version:/ { print $2; exit }' \
    "$ROOT/.github/workflows/release.yml")
[ "$ci_core" = "$CORE" ] \
    || echo "warning: release.yml builds Arduino-ESP32 $ci_core; this script's platform $PLATFORM is for $CORE: update CORE and PLATFORM" >&2

# --- sibling worktrees -----------------------------------------------------
first_set() { # $1..$n-1: env var names, $n: default
    local vars=("${@:1:$#-1}") v val=""
    for v in "${vars[@]}"; do
        if [ -n "${!v:-}" ]; then val=${!v}; break; fi
    done
    [ -n "$val" ] || val=${!#}
    (cd -- "$val" && pwd) || die "no such directory: $val"
}

SIBLING_TESSERKIT=$ROOT
SIBLING_TESSERAPI=$(first_set TESSERAPI_DIR TESSERAPI_PATH "$ROOT/../TesserAPI")
SIBLING_TESSERUI=$(first_set TESSERUI_DIR TESSERUI_PATH "$ROOT/../TesserUI")
SIBLING_NOWTP=$(first_set NOWTP_PATH NOWTP_DIR "$ROOT/../NowTP")

for d in "$SIBLING_TESSERKIT" "$SIBLING_TESSERAPI" "$SIBLING_TESSERUI" "$SIBLING_NOWTP"; do
    [ -f "$d/library.json" ] || die "no library.json in $d (a sibling worktree?)"
done

# The pin for a repo name in deps.lock ("owner/repo@sha", one per line).
pinned_sha() {
    awk -F@ -v n="$1" '$1 !~ /^#/ && $1 ~ ("/" n "$") { print $2; exit }' \
        "$ROOT/deps.lock"
}

echo "Siblings:"
report_sibling() { # $1: name, $2: dir
    local name=$1 dir=$2 head pin
    if ! head=$(git -C "$dir" rev-parse HEAD 2>/dev/null); then
        echo "  $name: $dir (not a git checkout; no pin check)"
        return
    fi
    pin=$(pinned_sha "$name")
    if [ -n "$pin" ] && [ "$head" != "$pin" ]; then
        echo "  $name: ${head:0:7} (warning: deps.lock pins ${pin:0:7})"
    else
        echo "  $name: ${head:0:7}"
    fi
}
report_sibling TesserKIT "$SIBLING_TESSERKIT"
report_sibling TesserAPI "$SIBLING_TESSERAPI"
report_sibling TesserUI "$SIBLING_TESSERUI"
report_sibling NowTP "$SIBLING_NOWTP"

# --- project generation ----------------------------------------------------
gen_project() { # $1: sketch dir name, rest: its CI envs
    local sketch=$1; shift
    local proj env
    proj="$BUILD_DIR/$(echo "$sketch" | tr '[:upper:]' '[:lower:]')"
    [ -d "$ROOT/examples/arduino/$sketch" ] || die "no examples/arduino/$sketch"
    mkdir -p "$proj/src"
    cp -r -- "$ROOT/examples/arduino/$sketch"/. "$proj/src/"
    {
        echo "; Generated by TesserKIT's scripts/arduino_check.sh -- do not edit."
        echo "; Mirrors the arduino job in .github/workflows/release.yml:"
        echo "; pioarduino $PLATFORM (Arduino-ESP32 $CORE), CI's boards,"
        echo "; min_spiffs.csv, the sibling worktrees via symlink://."
        echo "[env]"
        echo "platform = https://github.com/pioarduino/platform-espressif32/releases/download/$PLATFORM/platform-espressif32.zip"
        echo "framework = arduino"
        echo "board_build.partitions = min_spiffs.csv"
        echo "lib_deps ="
        echo "    symlink://$SIBLING_TESSERKIT"
        echo "    symlink://$SIBLING_TESSERUI"
        echo "    symlink://$SIBLING_TESSERAPI"
        echo "    symlink://$SIBLING_NOWTP"
        echo "    bblanchon/ArduinoJson@^7.4.3"
        for env in "$@"; do
            echo ""
            echo "[env:$env]"
            echo "board = $env"
        done
    } >"$proj/platformio.ini"
}

# The arduino job's matrix: Bluedroid does not fit a classic ESP32's slot,
# so Bluetooth is C3 and S3 only.
gen_project Minimal esp32dev esp32-c3-devkitm-1 esp32-s3-devkitc-1
gen_project Bluetooth esp32-c3-devkitm-1 esp32-s3-devkitc-1

# --- builds ----------------------------------------------------------------
# The verbose run captures the compile commands for the syntax pass. It can
# crash at the end with a known, intermittent scons bug (in print_cmd_line,
# at the firmware.bin step, after every compile has finished); a plain run
# then finishes the build, so that one error is retryable and nothing else.
PIO_TYPEERROR="TypeError: unsupported operand type(s) for +: '_Null' and 'str'"

build_env() { # $1: project dir, $2: env
    local proj=$1 env=$2 vlog plog attempt=1
    vlog="$proj/build-$env.verbose.log"
    plog="$proj/build-$env.plain.log"
    echo "==> $(basename "$proj") / $env: pio run -v"
    # From clean objects (libraries stay installed), or a rerun compiles
    # nothing and the syntax pass has no commands to check.
    rm -rf -- "$proj/.pio/build/$env"
    if (cd "$proj" && pio run -e "$env" -v) >"$vlog" 2>&1; then
        return 0
    fi
    if ! grep -qF "$PIO_TYPEERROR" "$vlog"; then
        echo "error: 'pio run -e $env' failed in $proj (last lines of $vlog):" >&2
        tail -n 40 "$vlog" >&2
        return 1
    fi
    while [ "$attempt" -le 2 ]; do
        echo "note: known TypeError in the verbose run; plain run (attempt $attempt/2)"
        if (cd "$proj" && pio run -e "$env") >"$plog" 2>&1; then
            return 0
        fi
        if ! grep -qF "$PIO_TYPEERROR" "$plog"; then
            echo "error: 'pio run -e $env' still failing in $proj (last lines of $plog):" >&2
            tail -n 40 "$plog" >&2
            return 1
        fi
        attempt=$((attempt + 1))
    done
    echo "error: 'pio run -e $env' failed with the known TypeError twice in $proj" >&2
    return 1
}

ROWS=() # "Sketch env" pairs, in report order
for env in esp32dev esp32-c3-devkitm-1 esp32-s3-devkitc-1; do
    build_env "$BUILD_DIR/minimal" "$env" || exit 1
    ROWS+=("Minimal $env")
done
for env in esp32-c3-devkitm-1 esp32-s3-devkitc-1; do
    build_env "$BUILD_DIR/bluetooth" "$env" || exit 1
    ROWS+=("Bluetooth $env")
done

# --- size table ------------------------------------------------------------
commas() {
    awk -v n="$1" 'BEGIN {
        neg = (n < 0); if (neg) n = -n
        s = sprintf("%d", n); out = ""
        while (length(s) > 3) {
            out = "," substr(s, length(s) - 2) out
            s = substr(s, 1, length(s) - 3)
        }
        print (neg ? "-" : "") s out
    }'
}

# The app0 slot of the partition file every env builds with.
app_slot() {
    local csv slot
    csv=$(find "$HOME/.platformio/packages/framework-arduinoespressif32" \
        -name min_spiffs.csv 2>/dev/null | head -n 1 || true)
    if [ -n "$csv" ]; then
        slot=$(awk -F, '$1 == "app0" { print $5; exit }' "$csv")
        if [[ $slot =~ ^0[xX][0-9a-fA-F]+$ ]]; then
            echo "$((slot))"
            return
        fi
    fi
    echo 1966080 # min_spiffs.csv's app0: 0x1E0000
}

APP_SLOT=$(app_slot)

echo
echo "| Sketch | Target | firmware.bin | App slot | Free in slot |"
echo "|---|---|---|---|---|"
FAIL=0
for row in "${ROWS[@]}"; do
    IFS=' ' read -r sketch env <<<"$row"
    proj="$BUILD_DIR/$(echo "$sketch" | tr '[:upper:]' '[:lower:]')"
    bin="$proj/.pio/build/$env/firmware.bin"
    [ -f "$bin" ] || die "missing $bin after a successful build"
    size=$(wc -c <"$bin" | tr -d ' ')
    free=$((APP_SLOT - size))
    if [ "$free" -lt 0 ]; then
        FAIL=1
        echo "| $sketch | $env | $(commas "$size") B | $(commas "$APP_SLOT") B (min_spiffs) | over by $(commas "$(( -free ))") B |"
    else
        echo "| $sketch | $env | $(commas "$size") B | $(commas "$APP_SLOT") B (min_spiffs) | $(commas "$free") B |"
    fi
done

# --- warnings from the sibling sources -------------------------------------
echo
echo "Warnings from the sibling sources:"
warns=$(grep -h 'warning:' "$BUILD_DIR"/minimal/build-*.verbose.log \
        "$BUILD_DIR"/bluetooth/build-*.verbose.log 2>/dev/null \
    | grep -F -e "$SIBLING_TESSERKIT/src" -e "$SIBLING_TESSERAPI/src" \
        -e "$SIBLING_TESSERUI/src" -e "$SIBLING_NOWTP/src" \
    | sort -u || true)
if [ -n "$warns" ]; then
    while IFS= read -r w; do echo "  $w"; done <<<"$warns"
else
    echo "  none"
fi

# --- gnu++2a syntax pass ----------------------------------------------------
# The library compile commands of a verbose build, rewritten for a
# syntax-only pass: -c, -o <arg>, -MMD and -MQ <arg> dropped, and
# -fsyntax-only -std=gnu++2a appended (the last -std wins).
rewrite_cmd() {
    local words=() w skip=0 out=""
    read -r -a words <<<"$1"
    for w in "${words[@]}"; do
        if [ "$skip" = 1 ]; then skip=0; continue; fi
        case $w in
            -c|-MMD) ;;
            -o|-MQ) skip=1 ;;
            *) out+="$w " ;;
        esac
    done
    printf '%s-fsyntax-only -std=gnu++2a\n' "$out"
}

extract_cmds() { # $1: verbose log; prints rewritten, deduped command lines
    awk -v roots="$SIBLING_TESSERKIT/src $SIBLING_TESSERAPI/src \
$SIBLING_TESSERUI/src $SIBLING_NOWTP/src" '
        BEGIN { n = split(roots, R, " ") }
        {
            src = $NF
            if (src !~ /\.(cpp|c)$/) next
            for (i = 1; i <= n; i++)
                if (index(src, R[i]) == 1) { print; break }
        }' "$1" \
    | while IFS= read -r line; do rewrite_cmd "$line"; done \
    | sort -u
}

# The verbose log prints each argv element shell-escaped (arguments with
# spaces or quotes are wrapped and their quotes backslashed), so the line is
# run through eval, not word-split: -DARDUINO_BOARD="Espressif ESP32 Dev
# Module" is one argument. No other shell metacharacters occur in these lines.
run_cmd_line() { # run one command line in the cwd
    eval "$1"
}

# The verbose logs name the cross compiler bare, and older platforms leave
# other toolchains with the same names in ~/.platformio/packages: the syntax
# pass puts the env's own toolchain, as PlatformIO reports it, first.
toolchain_bin() { # $1: project dir, $2: env
    (cd "$1" && pio project metadata -e "$2" --json-output 2>/dev/null) | python3 -c '
import json, os, sys
for line in sys.stdin:
    if line.startswith("{"):
        print(os.path.dirname(json.loads(line)[sys.argv[1]]["cxx_path"]))
        break' "$2"
}

echo
SYNTAX_ROWS=()
for row in "${ROWS[@]}"; do
    IFS=' ' read -r sketch env <<<"$row"
    proj="$BUILD_DIR/$(echo "$sketch" | tr '[:upper:]' '[:lower:]')"
    vlog="$proj/build-$env.verbose.log"
    cmds="$proj/syntax-$env.cmds"
    results="$proj/syntax-$env.results"

    tcbin=$(toolchain_bin "$proj" "$env")
    [ -d "$tcbin" ] || die "no toolchain for $env (pio project metadata)"
    extract_cmds "$vlog" >"$cmds"
    total=$(wc -l <"$cmds" | tr -d ' ')
    [ "$total" -gt 0 ] \
        || die "no library compile commands in $vlog"

    # Negative control: the harness must be able to fail.
    first=$(head -n 1 "$cmds")
    if (cd "$proj" && PATH="$tcbin:$PATH" run_cmd_line "$first -include /nonexistent.h") >/dev/null 2>&1; then
        die "negative control passed unexpectedly; the syntax pass cannot detect failures"
    fi

    echo "==> $sketch, $env: gnu++2a syntax pass over $total library sources ($tcbin)"
    # The commands use relative -I paths (.pio/...), so they run in the
    # project dir. A rewritten line ends "<source> -fsyntax-only -std=gnu++2a";
    # the source is recovered by stripping that suffix, not by word count
    # (flag values may contain spaces).
    # One line per argument (-0 -n1): BSD xargs's -I can't take lines this long.
    # shellcheck disable=SC2016  # $1 is bash -c's, expanded there
    tr '\n' '\0' <"$cmds" | (cd "$proj" && PATH="$tcbin:$PATH" xargs -0 -n1 -P "${SYNTAX_JOBS:-$(getconf _NPROCESSORS_ONLN)}" \
        bash -c 'rest=${1%" -fsyntax-only -std=gnu++2a"}; src=${rest##* }; if ! eval "$1" >/dev/null 2>&1; then printf "FAIL %s\n" "$src"; fi' _) \
        >"$results"
    fails=$(grep -c '^FAIL' "$results" || true)
    fails=${fails:-0}

    if [ "$fails" -gt 0 ]; then
        FAIL=1
        echo "  $fails failure(s); first errors:" >&2
        while IFS= read -r fl; do
            src=${fl#FAIL }
            cmd=$(grep -F " $src -fsyntax-only" "$cmds" | head -n 1)
            echo "  --- $src" >&2
            (cd "$proj" && PATH="$tcbin:$PATH" run_cmd_line "$cmd") || true
        done < <(grep '^FAIL' "$results")
    fi
    SYNTAX_ROWS+=("| $sketch, $env | $total | $fails |")
done

echo
echo "| Build | Sources checked | Failures |"
echo "|---|---|---|"
printf '%s\n' "${SYNTAX_ROWS[@]}"
echo "Negative control: the same harness with -include /nonexistent.h fails, as it should."

# --- summary ----------------------------------------------------------------
cc="$(toolchain_bin "$BUILD_DIR/minimal" esp32dev)/$(awk '{ if ($NF ~ /\.cpp$/) { print $1; exit } }' \
    "$BUILD_DIR/minimal/build-esp32dev.verbose.log")"
echo
echo "Built with: $(pio --version 2>/dev/null | head -n 1), $("$cc" --version 2>/dev/null | head -n 1)"
echo "Logs and projects: $BUILD_DIR/{minimal,bluetooth}/"

if [ "$FAIL" -ne 0 ]; then
    echo "arduino_check: FAILED (firmware over its slot or syntax pass failures above)" >&2
    exit 1
fi
echo "arduino_check: OK -- every build fits its slot and every library source passes gnu++2a"
