#!/usr/bin/env bash
# smoke test: run vkcube under the freshly built layer and fail if it dies
#
# the point is not that vkcube renders correctly, it is that driving a real
# swapchain through the layer for a few seconds does not take the process down.
# that catches the class of bug unit tests cannot: a bad interceptor, a null
# handle dereference, a use-after-free on a handle whose lifetime we guessed at.
#
# usage: src/tests/smoke_vkcube.sh [LIBVKINTOX_SO] [SECONDS]
#
# needs a display. on CI wrap it: xvfb-run -a src/tests/smoke_vkcube.sh
# there is no GPU on a CI runner either, so point VK_ICD_FILENAMES at lavapipe
# or the loader will find nothing to run on.

set -euo pipefail

LIBRARY="${1:-build/src/libvkintox.so}"
DURATION="${2:-11}"
LAYER_NAME="VK_LAYER_VKINTOX_smoke_test"
# the installed production layer, which on a developer machine would otherwise
# be discovered alongside ours and could shadow the build we actually want to test
INSTALLED_LAYER="VK_LAYER_VKINTOX_post_processing"

if [[ -t 1 ]]; then
    B='\033[1m'; BLUE='\033[34m'; GREEN='\033[32m'; YELLOW='\033[33m'; RED='\033[31m'; DIM='\033[2m'; NC='\033[0m'
else
    B=''; BLUE=''; GREEN=''; YELLOW=''; RED=''; DIM=''; NC=''
fi

say() { printf "${BLUE}  →${NC} %s\n" "$*"; }
ok() { printf "${GREEN}  ✓${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}  !${NC} %s\n" "$*"; }
die() {
    printf "${RED}  ✗${NC} %s\n" "$*" >&2
    if [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        printf "  ${DIM}artifacts kept: %s${NC}\n" "$WORK_DIR" >&2
    fi
    exit 1
}
finish() { printf "\n${BLUE}${B}%s${NC}\n" "$*"; }

command -v vkcube >/dev/null 2>&1 || die "vkcube not found; install vulkan-tools"
[[ -f "$LIBRARY" ]] || die "layer library not found: $LIBRARY (build it first)"
LIBRARY="$(cd "$(dirname "$LIBRARY")" && pwd)/$(basename "$LIBRARY")"

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    # A headless runner has no display at all and vkcube cannot open a window
    # without one. Re-exec under xvfb-run so the same command works on a
    # developer desktop and on CI. The env marker keeps a broken xvfb-run from
    # turning this into an exec loop.
    if [[ -z "${VKINTOX_SMOKE_XVFB:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
        VKINTOX_SMOKE_XVFB=1 exec xvfb-run -a "$0" "$@"
    fi
    die "no display, and xvfb-run is not available to provide one"
fi

WORK_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT
    if (( status == 0 )) && [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        rm -rf -- "$WORK_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT

# A manifest of our own, under a layer name nothing else uses, so the loader
# cannot mistake it for an installed copy and drop it as a duplicate.
LAYER_DIR="$WORK_DIR/layer"
CONFIG_DIR="$WORK_DIR/config"
mkdir -p "$LAYER_DIR" "$CONFIG_DIR"
cat >"$LAYER_DIR/$LAYER_NAME.json" <<JSON
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "$LAYER_NAME",
    "type": "GLOBAL",
    "library_path": "$LIBRARY",
    "api_version": "1.3.223",
    "implementation_version": "1",
    "description": "vkintox smoke test",
    "functions": {
      "vkGetInstanceProcAddr": "VKIntox_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "VKIntox_GetDeviceProcAddr"
    },
    "enable_environment": { "ENABLE_VKINTOX": "1" },
    "disable_environment": { "DISABLE_VKINTOX": "1" }
  }
}
JSON
jq -e . "$LAYER_DIR/$LAYER_NAME.json" >/dev/null || die "generated manifest is not valid json"

say "layer      $LIBRARY"
say "duration   ${DURATION}s"

# VK_LOADER_LAYERS_ENABLE is what guarantees our layer is the one that ends up in
# the chain. Without it, a VKIntox already installed on the machine wins and the
# run silently tests the installed build instead of this one.
#
# The XDG_CONFIG_HOME redirect sends the layer's own log somewhere disposable, so
# "did the layer actually engage" is answerable without reading a stale log from
# an earlier run, and a local run cannot scribble on a real config.
set +e
VK_LOADER_DEBUG=layer \
    VK_LOADER_LAYERS_ENABLE="$LAYER_NAME" \
    VK_LOADER_LAYERS_DISABLE="$INSTALLED_LAYER" \
    VK_LAYER_PATH="$LAYER_DIR" \
    ENABLE_VKINTOX=1 \
    XDG_CONFIG_HOME="$CONFIG_DIR" \
    timeout "$DURATION" vkcube >"$WORK_DIR/vkcube.log" 2>&1
STATUS=$?
set -e

LOADED=no
if grep -qF "$LIBRARY" "$WORK_DIR/vkcube.log"; then
    LOADED=yes
fi

if [[ "$LOADED" != yes ]]; then
    printf "%s\n" "--- vkcube + loader output ---" >&2
    cat "$WORK_DIR/vkcube.log" >&2
    die "the loader never loaded $LIBRARY, so this run proves nothing. an installed copy of the layer may be shadowing it; check the loader lines above."
fi
ok "layer loaded from the build directory"

LAYER_LOG="$CONFIG_DIR/VKIntox/vkintox.log"
if [[ -s "$LAYER_LOG" ]]; then
    ok "layer engaged ($(wc -l <"$LAYER_LOG") log lines)"
else
    warn "layer loaded but wrote no log; it may not have reached initialisation"
fi

# 124 is what timeout reports when it had to stop the process, which is the only
# outcome that means "still alive when we stopped watching".
case "$STATUS" in
    124) ok "vkcube survived ${DURATION}s without crashing" ;;
    0)   ok "vkcube exited cleanly on its own" ;;
    137) die "vkcube was SIGKILLed (exit 137), usually the OOM killer" ;;
    139) die "vkcube segfaulted (exit 139) under the layer" ;;
    134) die "vkcube aborted (exit 134) under the layer" ;;
    *)
        printf "%s\n" "--- vkcube output ---" >&2
        cat "$WORK_DIR/vkcube.log" >&2
        die "vkcube exited with $STATUS"
        ;;
esac

finish "smoke test PASSED"
