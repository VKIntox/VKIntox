#!/usr/bin/env bash
# key uniform end-to-end: run vkcube under the freshly built layer with a
# shader whose output color IS the key uniform value, inject key presses via
# XTest, and assert the rendered pixels.
#
# the shader itself is the oracle: key.fx writes the hold uniform into the red
# channel and its inverted copy into green, so any miswritten uniform shows as
# a wrong color and cannot hide behind other effect logic.
#
# usage: src/tests/key_vkcube_test.sh [LIBVKINTOX_SO] [SECONDS]
# needs an X display (XWayland is fine); keys are injected through XTest,
# never wtype.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBRARY="${1:-build/src/libvkintox.so}"
DURATION="${2:-40}"
LAYER_NAME="VK_LAYER_VKINTOX_key_test"
INSTALLED_LAYER="VK_LAYER_VKINTOX_post_processing"

if [[ -t 1 ]]; then
    B='\033[1m'; BLUE='\033[34m'; GREEN='\033[32m'; YELLOW='\033[33m'; RED='\033[31m'; DIM='\033[2m'; NC='\033[0m'
else
    B=''; BLUE=''; GREEN=''; YELLOW=''; RED=''; DIM=''; NC=''
fi
say() { printf "${BLUE}  →${NC} %s\n" "$*"; }
ok() { printf "${GREEN}  ✓${NC} %s\n" "$*"; }
die() {
    printf "${RED}  ✗${NC} %s\n" "$*" >&2
    if [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        printf "  ${DIM}artifacts kept: %s${NC}\n" "$WORK_DIR" >&2
    fi
    exit 1
}

command -v vkcube >/dev/null 2>&1 || die "vkcube not found; install vulkan-tools"
command -v import >/dev/null 2>&1 || die "imagemagick's import not found (screenshot step)"
command -v convert >/dev/null 2>&1 || die "imagemagick's convert not found (pixel sampling)"
command -v cc >/dev/null 2>&1 || die "cc not found (compiles the x helper)"
[[ -f "$LIBRARY" ]] || die "layer library not found: $LIBRARY (build it first)"
[[ -f "$SCRIPT_DIR/key.fx" ]] || die "missing $SCRIPT_DIR/key.fx"
LIBRARY="$(cd "$(dirname "$LIBRARY")" && pwd)/$(basename "$LIBRARY")"
[[ -n "${DISPLAY:-}" ]] || die "no DISPLAY; this test drives real X input"

WORK_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "${VKCUBE_PID:-}" ]]; then
        kill "$VKCUBE_PID" 2>/dev/null || true
    fi
    if (( status == 0 )) && [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        rm -rf -- "$WORK_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT

# tiny X helper: find a window by owning process id, inject XTest key events
cat >"$WORK_DIR/xkey.c" <<'EOF'
#include <X11/Xlib.h>
#include <X11/extensions/XRes.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Window windowOfPid(Display* d, int want)
{
    Window root, parent, *children = NULL;
    unsigned int n = 0;
    if (XQueryTree(d, DefaultRootWindow(d), &root, &parent, &children, &n) == 0)
        return 0;
    Window found = 0;
    for (unsigned int i = 0; i < n && !found; ++i)
    {
        XWindowAttributes a;
        if (!XGetWindowAttributes(d, children[i], &a) || a.map_state != IsViewable)
            continue;
        XResClientIdSpec spec = { children[i], XRES_CLIENT_ID_PID_MASK };
        long nids = 0;
        XResClientIdValue* ids = NULL;
        if (XResQueryClientIds(d, 1, &spec, &nids, &ids) == Success && ids)
        {
            for (long j = 0; j < nids && !found; ++j)
            {
                if (XResGetClientIdType(ids + j) == XRES_CLIENT_ID_PID &&
                    XResGetClientPid(ids + j) == want)
                    found = children[i];
            }
            XResClientIdsDestroy(nids, ids);
        }
    }
    if (children)
        XFree(children);
    return found;
}

int main(int argc, char** argv)
{
    Display* d = XOpenDisplay(NULL);
    if (!d) { printf("no display\n"); return 1; }
    if (argc >= 3 && strcmp(argv[1], "winid") == 0)
    {
        Window found = windowOfPid(d, atoi(argv[2]));
        if (!found) { printf("window not found\n"); return 1; }
        printf("%lu\n", (unsigned long)found);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "code") == 0)
    {
        printf("%d\n", XKeysymToKeycode(d, (KeySym)strtoul(argv[2], NULL, 0)));
        return 0;
    }
    if (argc >= 3 && (strcmp(argv[1], "down") == 0 || strcmp(argv[1], "up") == 0))
    {
        KeyCode code = (KeyCode)atoi(argv[2]);
        if (code == 0) { printf("bad keycode\n"); return 1; }
        XTestFakeKeyEvent(d, code, argv[1][0] == 'd', 0);
        XSync(d, False);
        return 0;
    }
    printf("usage: xkey winid <pid> | down <keycode> | up <keycode> | code <keysym>\n");
    return 1;
}
EOF
cc "$WORK_DIR/xkey.c" -o "$WORK_DIR/xkey" -lX11 -lXtst -lXRes 2>"$WORK_DIR/cc.log" ||
    { cat "$WORK_DIR/cc.log" >&2; die "x helper compile failed"; }

# layer manifest of our own so the installed copy cannot shadow the build
LAYER_DIR="$WORK_DIR/layer"
CONFIG_DIR="$WORK_DIR/config"
SHADER_DIR="$CONFIG_DIR/VKIntox/reshade/Shaders"
mkdir -p "$LAYER_DIR" "$SHADER_DIR" "$CONFIG_DIR/VKIntox/configs"
cat >"$LAYER_DIR/$LAYER_NAME.json" <<JSON
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "$LAYER_NAME",
    "type": "GLOBAL",
    "library_path": "$LIBRARY",
    "api_version": "1.3.223",
    "implementation_version": "1",
    "description": "vkintox key uniform test",
    "functions": {
      "vkGetInstanceProcAddr": "VKIntox_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "VKIntox_GetDeviceProcAddr"
    },
    "enable_environment": { "ENABLE_VKINTOX": "1" },
    "disable_environment": { "DISABLE_VKINTOX": "1" }
  }
}
JSON

cp "$SCRIPT_DIR/key.fx" "$SHADER_DIR/key.fx"

# the effect is referenced by direct path, the resolution the registry tries first
cat >"$CONFIG_DIR/VKIntox/configs/vkcube.conf" <<CONF
enableOnLaunch = true
overlayKey = End
reloadKey = F10
showFps = false
depthCapture = off
KeyTest = $SHADER_DIR/key.fx
effects = KeyTest
CONF

say "layer      $LIBRARY"
say "duration   ${DURATION}s"

set +e
VKINTOX_LOG_LEVEL=debug \
    VK_LOADER_LAYERS_ENABLE="$LAYER_NAME" \
    VK_LOADER_LAYERS_DISABLE="$INSTALLED_LAYER" \
    VK_LAYER_PATH="$LAYER_DIR" \
    ENABLE_VKINTOX=1 \
    XDG_CONFIG_HOME="$CONFIG_DIR" \
    env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET vkcube >"$WORK_DIR/vkcube.log" 2>&1 &
VKCUBE_PID=$!
set -e

# wait until the layer compiled the effect and is presenting frames
for _ in $(seq 1 60); do
    if grep -q "key uniform keycode=119" "$CONFIG_DIR/VKIntox/vkintox.log" 2>/dev/null; then
        break
    fi
    sleep 0.5
done
grep -q "key uniform keycode=119" "$CONFIG_DIR/VKIntox/vkintox.log" 2>/dev/null ||
    { cat "$WORK_DIR/vkcube.log" >&2; die "the layer never parsed the key uniform; see vkcube.log"; }
ok "layer parsed the key uniform"

sleep 2   # let frames settle

WIN=$("$WORK_DIR/xkey" winid "$VKCUBE_PID") || { cat "$WORK_DIR/vkcube.log" >&2; die "vkcube window not found"; }
ok "vkcube window $WIN"

# VK_F8: the keycode the layer's vk map resolves it to on this display
F8CODE=$("$WORK_DIR/xkey" code $((0xFFC5))) || die "no keycode for F8 on this keymap"
[[ "$F8CODE" != "0" ]] || die "no keycode for F8 on this keymap"
ok "F8 injected as keycode $F8CODE"

shot() {
    import -window "$WIN" "$1" 2>/dev/null || die "screenshot failed"
    sleep 0.4
}
sample() {
    convert "$1" -format "%[fx:int(255*mean.r)] %[fx:int(255*mean.g)] %[fx:int(255*mean.b)]" info: 2>/dev/null
}
expect() {
    # $1 file, $2 channel (r/g), $3 expected 0/1, $4 label
    local rgb
    rgb=$(sample "$1")
    local r=$(echo "$rgb" | cut -d' ' -f1)
    local g=$(echo "$rgb" | cut -d' ' -f2)
    local b=$(echo "$rgb" | cut -d' ' -f3)
    if (( b < 250 )); then
        printf "    %s: rgb=%s\n" "$4" "$rgb"
        die "sanity channel failed: the shader's own consistency bit is unset, so the run is invalid"
    fi
    local v
    if [[ "$2" == r ]]; then v=$r; else v=$g; fi
    if (( $3 == 1 )); then
        (( v > 200 )) || { printf "    %s: rgb=%s\n" "$4" "$rgb"; return 1; }
    else
        (( v < 50 )) || { printf "    %s: rgb=%s\n" "$4" "$rgb"; return 1; }
    fi
    return 0
}

shot "$WORK_DIR/up1.png"
if expect "$WORK_DIR/up1.png" r 0 "key up, hold"; then ok "hold reads up (red)"; else die "hold should read up (red)"; fi
if expect "$WORK_DIR/up1.png" g 1 "key up, inverted"; then ok "inverted reads up (green)"; else die "inverted should read up (green)"; fi

"$WORK_DIR/xkey" down "$F8CODE"
sleep 1
shot "$WORK_DIR/down.png"
if expect "$WORK_DIR/down.png" r 1 "key down, hold"; then ok "hold reads down (green)"; else die "hold should read down (green)"; fi
if expect "$WORK_DIR/down.png" g 0 "key down, inverted"; then ok "inverted reads down (red)"; else die "inverted should read down (red)"; fi

"$WORK_DIR/xkey" up "$F8CODE"
sleep 1
shot "$WORK_DIR/up2.png"
if expect "$WORK_DIR/up2.png" r 0 "key up again, hold"; then ok "hold reads up again (red)"; else die "hold should read up again (red)"; fi
if expect "$WORK_DIR/up2.png" g 1 "key up again, inverted"; then ok "inverted reads up again (green)"; else die "inverted should read up again (green)"; fi

kill "$VKCUBE_PID" 2>/dev/null || true
wait "$VKCUBE_PID" 2>/dev/null || true

finish() { printf "\n${BLUE}${B}%s${NC}\n" "$*"; }
finish "KEY UNIFORM PIPELINE: ALL PASS"
