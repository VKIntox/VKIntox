// Wayland event interposition for input blocking.
//
// On Wayland, there is no XGrabPointer equivalent. The game and our overlay
// both receive pointer/keyboard events from the compositor for the same
// surface. This module interposes on wl_proxy_add_listener and
// wl_proxy_add_dispatcher (the underlying C functions that the generated
// protocol helpers use) and wraps the game's pointer/keyboard callbacks.
//
// Blocking is a hitbox, not a global gate: the overlay publishes its rects,
// and only events positioned inside them are withheld from the game. Pointer
// motion/button/axis carry or inherit a position; keyboard has none, so it
// follows the last pointer position.
//
// Overlay-owned proxies are registered via registerOverlayProxy() and are
// always passed through unwrapped.

#include "input/wayland/wayland_interpose.hh"
#include "input/common/input_blocker.hh"
#include "input/wayland/wayland_display.hh"
#include "input/wayland/wayland_input_common.hh"
#include "input/wayland/mouse_input_wayland.hh"
#include "input/wayland/keyboard_input_wayland.hh"
#include "input/wayland/wayland_syncobj_gate.hh"
#include "util/logger.hh"
#include "cursor-shape-v1-client-protocol.h"

#include <wayland-client.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cstdlib>

namespace VKIntox
{
    namespace
    {
        // Vulkan layers are typically loaded RTLD_LOCAL, which prevents global
        // symbol interposition in normal implicit-layer mode. Promote this
        // already-loaded object to RTLD_GLOBAL so our wl_proxy wrappers are
        // visible process-wide.
        __attribute__((constructor)) void promoteSelfToGlobalScope()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&promoteSelfToGlobalScope), &info) == 0 || !info.dli_fname)
                return;

            void* handle = dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL);
            if (!handle)
                handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL);

            if (handle)
                Logger::debug("Wayland interpose: promoted VKIntox library to RTLD_GLOBAL");
        }
    } // namespace

    static std::mutex proxyMutex;
    static std::unordered_set<wl_proxy*> overlayProxies;

    void registerOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.insert(proxy);
    }

    void unregisterOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.erase(proxy);
    }

    static bool isOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        return overlayProxies.count(proxy) > 0;
    }
} // namespace VKIntox

// Pointer position in game-surface coordinates: events on the capture surface
// are translated by its origin so hitbox tests and the layer's mouse state
// share one space.
static std::atomic<double> g_lastPointerX{-1.0};
static std::atomic<double> g_lastPointerY{-1.0};

static bool pointerBlocked()
{
    return VKIntox::isInputBlockedAt((float)g_lastPointerX.load(std::memory_order_acquire),
                                     (float)g_lastPointerY.load(std::memory_order_acquire));
}

// ── Game listener storage ────────────────────────────────────────────────────

// Per-proxy data for wrapped game listeners. We store the game's original
// listener and user_data so we can forward events when not blocking.
struct GamePointerData
{
    wl_pointer_listener original;
    void* userData;
};

struct GameKeyboardData
{
    wl_keyboard_listener original;
    void* userData;
};

static std::mutex gameDataMutex;
static std::unordered_map<wl_pointer*, GamePointerData> gamePointers;
static std::unordered_map<wl_keyboard*, GameKeyboardData> gameKeyboards;

struct GamePointerDispatcherData
{
    wl_dispatcher_func_t original;
    const void* dispatcherData;
};

struct GameKeyboardDispatcherData
{
    wl_dispatcher_func_t original;
    const void* dispatcherData;
};

static std::unordered_map<wl_proxy*, GamePointerDispatcherData> gamePointerDispatchers;
static std::unordered_map<wl_proxy*, GameKeyboardDispatcherData> gameKeyboardDispatchers;

// Per pointer: the surface holding pointer focus and who owns an in-flight
// press. The owner decides forwarding for the whole implicit grab, so a drag
// that starts on the game keeps reaching it even across the overlay.
enum class PressOwner { none, game, overlay };

struct PointerInputState
{
    const void* focusSurface = nullptr;
    PressOwner pressOwner = PressOwner::none;
};
static std::unordered_map<void*, PointerInputState> pointerStates;

// Per keyboard: which side saw a press. A blocked press belongs to the overlay
// and its release is withheld; a game-owned key keeps its release so it never
// sticks.
struct KeyOwnership
{
    std::unordered_set<uint32_t> gameOwned;
    std::unordered_set<uint32_t> overlayOwned;
};
static std::unordered_map<void*, KeyOwnership> keyOwnership;

// ── Wrapper pointer listener ─────────────────────────────────────────────────

// The overlay's input capture surface belongs to the game's connection, so the
// game's wl_pointer also hears enter/leave for it. Keep those to ourselves: a
// game seeing focus land on a surface it never made may skip re-applying its
// cursor (or mis-handle the foreign surface), which left the system cursor up.
static bool isOverlaySurface(const void* surface)
{
    const void* overlay = VKIntox::getWaylandInputSurface();
    return overlay && surface == overlay;
}

// ── game cursor memory ──────────────────────────────────────────────────────
// The overlay hides the system cursor over its input surface with a null
// wl_pointer.set_cursor, and compositors keep the last image across focus
// changes: a game that does not re-issue its cursor on re-enter is left with
// no cursor at all. Remember whatever the game last set (a bitmap via
// set_cursor or a named shape via wp_cursor_shape_device_v1.set_shape) and
// re-issue it on the next enter of a game surface, with that enter's serial.
// An empty memory means the game never set a cursor; the compositor default
// is not re-requestable from the client side, so nothing can be done there.
struct GameCursor
{
    enum class Kind { none, surface, shape } kind = Kind::none;
    struct wl_proxy* surface = nullptr;     // Kind::surface
    int32_t hotX = 0, hotY = 0;
    struct wl_proxy* shapeDevice = nullptr;  // Kind::shape
    uint32_t shape = 0;
};
static GameCursor gameCursor;
static bool gameCursorHiddenByOverlay = false;

static void restoreGameCursor(struct wl_proxy* pointer, uint32_t serial)
{
    if (!gameCursorHiddenByOverlay)
        return;
    gameCursorHiddenByOverlay = false;
    if (gameCursor.kind == GameCursor::Kind::shape && gameCursor.shapeDevice)
        wp_cursor_shape_device_v1_set_shape(
            reinterpret_cast<wp_cursor_shape_device_v1*>(gameCursor.shapeDevice),
            serial, gameCursor.shape);
    else if (gameCursor.kind == GameCursor::Kind::surface && gameCursor.surface)
        wl_pointer_set_cursor(reinterpret_cast<wl_pointer*>(pointer), serial,
                              reinterpret_cast<wl_surface*>(gameCursor.surface),
                              gameCursor.hotX, gameCursor.hotY);
    if (wl_display* display = VKIntox::getWaylandDisplay())
        wl_display_flush(display);
}

// The overlay draws its own pointer over the capture surface, so the system
// cursor is hidden there with a null set_cursor on the game's pointer — one
// client-wide cursor image means two pointer resources fighting over it would
// thrash.
static void hideGameCursor(struct wl_proxy* pointer, uint32_t serial)
{
    wl_pointer_set_cursor(reinterpret_cast<wl_pointer*>(pointer), serial, nullptr, 0, 0);
    if (wl_display* display = VKIntox::getWaylandDisplay())
        wl_display_flush(display);
}

static PointerInputState* pointerState(void* proxy)
{
    auto it = pointerStates.find(proxy);
    return it != pointerStates.end() ? &it->second : nullptr;
}

static void toGameCoords(const void* surface, double sx, double sy, double& gx, double& gy)
{
    if (isOverlaySurface(surface))
    {
        gx = sx + VKIntox::getWaylandInputSurfaceX();
        gy = sy + VKIntox::getWaylandInputSurfaceY();
    }
    else
    {
        gx = sx;
        gy = sy;
    }
}

static void storePointer(double x, double y)
{
    g_lastPointerX.store(x, std::memory_order_release);
    g_lastPointerY.store(y, std::memory_order_release);
}

static bool handlePointerEnter(void* proxy, uint32_t serial, const void* surface, double sx, double sy)
{
    double gx = 0, gy = 0;
    toGameCoords(surface, sx, sy, gx, gy);
    storePointer(gx, gy);
    VKIntox::mirrorPointerPosition(gx, gy);

    PointerInputState& state = pointerStates[proxy];
    state.focusSurface = surface;
    state.pressOwner = PressOwner::none;

    if (isOverlaySurface(surface))
    {
        VKIntox::setWaylandPointerHover(true);
        hideGameCursor(reinterpret_cast<struct wl_proxy*>(proxy), serial);
        return false;
    }

    VKIntox::setWaylandPointerHover(false);
    restoreGameCursor(reinterpret_cast<struct wl_proxy*>(proxy), serial);
    return true;
}

static bool handlePointerLeave(void* proxy, const void* surface)
{
    if (isOverlaySurface(surface))
        VKIntox::setWaylandPointerHover(false);
    if (PointerInputState* state = pointerState(proxy))
        state->focusSurface = nullptr;
    return true;
}

static bool handlePointerMotion(void* proxy, double sx, double sy)
{
    PointerInputState* state = pointerState(proxy);
    const void* surface = state ? state->focusSurface : nullptr;
    double gx = 0, gy = 0;
    toGameCoords(surface, sx, sy, gx, gy);
    storePointer(gx, gy);
    VKIntox::mirrorPointerPosition(gx, gy);

    if (!state)
        return true;
    if (state->pressOwner == PressOwner::overlay)
        return false;
    if (state->pressOwner == PressOwner::none && isOverlaySurface(surface))
        return false;
    if (state->pressOwner == PressOwner::none && VKIntox::isInputBlockedAt((float)gx, (float)gy))
        return false;
    return true;
}

static bool handlePointerButton(void* proxy, uint32_t button, uint32_t state)
{
    const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    VKIntox::mirrorButtonState(button, pressed);

    PointerInputState* sp = pointerState(proxy);
    if (pressed)
    {
        const void* surface = sp ? sp->focusSurface : nullptr;
        const bool overlayOwns = isOverlaySurface(surface)
            || ((!sp || sp->pressOwner == PressOwner::none) && pointerBlocked());
        if (sp)
            sp->pressOwner = overlayOwns ? PressOwner::overlay : PressOwner::game;
        return !overlayOwns;
    }

    const PressOwner owner = sp ? sp->pressOwner : PressOwner::none;
    if (sp && owner != PressOwner::none)
        sp->pressOwner = PressOwner::none;
    return owner != PressOwner::overlay;
}

static bool pointerEventWithheld(void* proxy)
{
    PointerInputState* state = pointerState(proxy);
    if (state)
    {
        if (state->pressOwner == PressOwner::overlay)
            return true;
        if (state->pressOwner == PressOwner::game)
            return false;
        if (isOverlaySurface(state->focusSurface))
            return true;
    }
    return pointerBlocked();
}

static bool handlePointerAxis(void* proxy, uint32_t axis, double value)
{
    VKIntox::mirrorScrollAxis(axis, value);
    return !pointerEventWithheld(proxy);
}

static bool handlePointerAxisSource(void* proxy, uint32_t /*source*/)
{
    return !pointerEventWithheld(proxy);
}

static bool handlePointerAxisStop(void* proxy, uint32_t /*time*/, uint32_t /*axis*/)
{
    return !pointerEventWithheld(proxy);
}

static bool handlePointerAxisDiscrete(void* proxy, uint32_t axis, int32_t discrete)
{
    VKIntox::mirrorScrollDiscrete(axis, discrete);
    return !pointerEventWithheld(proxy);
}

static bool handlePointerAxisValue120(void* proxy, uint32_t axis, int32_t value120)
{
    VKIntox::mirrorScrollValue120(axis, value120);
    return !pointerEventWithheld(proxy);
}

static bool handlePointerFrame(void* proxy)
{
    VKIntox::resetScrollFrame();
    return !pointerEventWithheld(proxy);
}

static void wp_enter(void* data, wl_pointer* p, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    if (!handlePointerEnter(p, serial, s, wl_fixed_to_double(x), wl_fixed_to_double(y)))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.enter)
        it->second.original.enter(it->second.userData, p, serial, s, x, y);
}

static void wp_leave(void* data, wl_pointer* p, uint32_t serial, wl_surface* s)
{
    if (!handlePointerLeave(p, s))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.leave)
        it->second.original.leave(it->second.userData, p, serial, s);
}

static void wp_motion(void* data, wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    if (!handlePointerMotion(p, wl_fixed_to_double(x), wl_fixed_to_double(y)))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.motion)
        it->second.original.motion(it->second.userData, p, time, x, y);
}

static void wp_button(void* data, wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    if (!handlePointerButton(p, button, state))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.button)
        it->second.original.button(it->second.userData, p, serial, time, button, state);
}

static void wp_axis(void* data, wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (!handlePointerAxis(p, axis, wl_fixed_to_double(value)))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis)
        it->second.original.axis(it->second.userData, p, time, axis, value);
}

static void wp_frame(void* data, wl_pointer* p)
{
    if (!handlePointerFrame(p))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.frame)
        it->second.original.frame(it->second.userData, p);
}

static void wp_axis_source(void* data, wl_pointer* p, uint32_t source)
{
    if (!handlePointerAxisSource(p, source))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_source)
        it->second.original.axis_source(it->second.userData, p, source);
}

static void wp_axis_stop(void* data, wl_pointer* p, uint32_t time, uint32_t axis)
{
    if (!handlePointerAxisStop(p, time, axis))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_stop)
        it->second.original.axis_stop(it->second.userData, p, time, axis);
}

static void wp_axis_discrete(void* data, wl_pointer* p, uint32_t axis, int32_t discrete)
{
    if (!handlePointerAxisDiscrete(p, axis, discrete))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_discrete)
        it->second.original.axis_discrete(it->second.userData, p, axis, discrete);
}

static void wp_axis_value120(void* data, wl_pointer* p, uint32_t axis, int32_t value120)
{
    if (!handlePointerAxisValue120(p, axis, value120))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_value120)
        it->second.original.axis_value120(it->second.userData, p, axis, value120);
}

static const wl_pointer_listener wrapperPointerListener = {
    .enter = wp_enter,
    .leave = wp_leave,
    .motion = wp_motion,
    .button = wp_button,
    .axis = wp_axis,
    .frame = wp_frame,
    .axis_source = wp_axis_source,
    .axis_stop = wp_axis_stop,
    .axis_discrete = wp_axis_discrete,
    .axis_value120 = wp_axis_value120,
    // .axis_relative_direction removed in newer wayland-protocols
};

// ── Wrapper keyboard listener ────────────────────────────────────────────────

static bool keyboardBlocked()
{
    return pointerBlocked() || VKIntox::isOverlayHoldingKeyboard();
}

static void wk_keymap(void* data, wl_keyboard* kb, uint32_t format, int32_t fd, uint32_t size)
{
    // the layer mmaps without closing: this fd belongs to the forwarded event
    // and the game's listener owns closing it
    VKIntox::mirrorKeyboardKeymap(format, fd, size, false);
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.keymap)
        it->second.original.keymap(it->second.userData, kb, format, fd, size);
}

static void wk_enter(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s, wl_array* keys)
{
    // Forward enter so the game's keyboard state stays consistent; individual
    // keys are withheld in wk_key while the pointer is over the overlay or the
    // interactive shell owns the keyboard.
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.enter)
        it->second.original.enter(it->second.userData, kb, serial, s, keys);
}

static void wk_leave(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s)
{
    VKIntox::mirrorKeyboardFocusLost();
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.leave)
        it->second.original.leave(it->second.userData, kb, serial, s);
}

static bool routeKey(void* proxy, uint32_t key, uint32_t state)
{
    bool forward = false;
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);
        KeyOwnership& own = keyOwnership[proxy];
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        {
            if (keyboardBlocked())
                own.overlayOwned.insert(key);
            else
            {
                own.gameOwned.insert(key);
                own.overlayOwned.erase(key);
                forward = true;
            }
        }
        else
        {
            forward = own.gameOwned.erase(key) > 0;
            own.overlayOwned.erase(key);
        }
    }
    return forward;
}

static void wk_key(void* data, wl_keyboard* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    VKIntox::mirrorKeyboardKey(key, state);
    if (!routeKey(kb, key, state))
        return;
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.key)
        it->second.original.key(it->second.userData, kb, serial, time, key, state);
}

static void wk_modifiers(void* data, wl_keyboard* kb, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    // Always forward modifiers — games need modifier state for keybinds
    VKIntox::mirrorKeyboardModifiers(depressed, latched, locked, group);
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.modifiers)
        it->second.original.modifiers(it->second.userData, kb, serial, depressed, latched, locked, group);
}

static void wk_repeat_info(void* data, wl_keyboard* kb, int32_t rate, int32_t delay)
{
    // Always forward repeat info
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.repeat_info)
        it->second.original.repeat_info(it->second.userData, kb, rate, delay);
}

static const wl_keyboard_listener wrapperKeyboardListener = {
    .keymap = wk_keymap,
    .enter = wk_enter,
    .leave = wk_leave,
    .key = wk_key,
    .modifiers = wk_modifiers,
    .repeat_info = wk_repeat_info,
};

// ── Wrapper dispatcher callbacks ────────────────────────────────────────────

static int wd_pointer(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto* proxy = static_cast<wl_proxy*>(target);

    bool forward = true;
    switch (opcode)
    {
    case 0: // enter: serial, surface, x, y
        forward = handlePointerEnter(proxy, args[0].u, args[1].o,
                                     wl_fixed_to_double(args[2].f), wl_fixed_to_double(args[3].f));
        break;
    case 1: // leave: serial, surface
        forward = handlePointerLeave(proxy, args[1].o);
        break;
    case 2: // motion: time, x, y
        forward = handlePointerMotion(proxy, wl_fixed_to_double(args[1].f), wl_fixed_to_double(args[2].f));
        break;
    case 3: // button: serial, time, button, state
        forward = handlePointerButton(proxy, args[2].u, args[3].u);
        break;
    case 4: // axis: time, axis, value
        forward = handlePointerAxis(proxy, args[1].u, wl_fixed_to_double(args[2].f));
        break;
    case 5: // frame
        forward = handlePointerFrame(proxy);
        break;
    case 6: // axis_source: source
        forward = handlePointerAxisSource(proxy, args[0].u);
        break;
    case 7: // axis_stop: time, axis
        forward = handlePointerAxisStop(proxy, args[0].u, args[1].u);
        break;
    case 8: // axis_discrete: axis, discrete
        forward = handlePointerAxisDiscrete(proxy, args[0].u, args[1].i);
        break;
    case 9: // axis_value120: axis, value120
        forward = handlePointerAxisValue120(proxy, args[0].u, args[1].i);
        break;
    default:
        break;
    }

    if (!forward)
        return 0;

    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gamePointerDispatchers.find(proxy);
    if (it != gamePointerDispatchers.end() && it->second.original)
        return it->second.original(it->second.dispatcherData, target, opcode, msg, args);
    return 0;
}

static int wd_keyboard(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto* proxy = static_cast<wl_proxy*>(target);

    bool forward = true;
    if (opcode == 0) // keymap: format, fd, size
    {
        VKIntox::mirrorKeyboardKeymap(args[0].u, args[1].i, args[2].u, false);
    }
    else if (opcode == 2) // leave: serial, surface
    {
        VKIntox::mirrorKeyboardFocusLost();
    }
    else if (opcode == 3) // key: serial, time, key, state
    {
        VKIntox::mirrorKeyboardKey(args[2].u, args[3].u);
        forward = routeKey(proxy, args[2].u, args[3].u);
    }
    else if (opcode == 4) // modifiers: serial, depressed, latched, locked, group
    {
        VKIntox::mirrorKeyboardModifiers(args[1].u, args[2].u, args[3].u, args[4].u);
    }

    if (!forward)
        return 0;

    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = gameKeyboardDispatchers.find(proxy);
    if (it != gameKeyboardDispatchers.end() && it->second.original)
        return it->second.original(it->second.dispatcherData, target, opcode, msg, args);
    return 0;
}

// ── Symbol interposition ─────────────────────────────────────────────────────
//
// wl_pointer_add_listener is a static inline in <wayland-client-protocol.h>
// that calls wl_proxy_add_listener. We interpose on the underlying C function
// so that even inlined calls are caught.

using AddListenerFn = int (*)(struct wl_proxy*, void (**)(void), void*);
using DlsymFn = void* (*)(void*, const char*);

static DlsymFn getRealDlsym()
{
    static DlsymFn fn = nullptr;
    if (fn)
        return fn;

    // Prefer a versioned lookup to avoid self-recursion when we interpose dlsym.
    fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    if (!fn)
        fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"));
    return fn;
}

template <typename FnType>
static FnType resolveWaylandSymbol(const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    // Preferred path for normal implicit-layer usage.
    void* sym = realDlsym(RTLD_NEXT, symbol);
    if (sym)
        return reinterpret_cast<FnType>(sym);

    // Fallback for preload / early-load scenarios where RTLD_NEXT may not
    // reach libwayland-client yet.
    static void* waylandHandle = nullptr;
    if (!waylandHandle)
    {
        waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOLOAD | RTLD_NOW);
        if (!waylandHandle)
            waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    }

    if (waylandHandle)
    {
        sym = realDlsym(waylandHandle, symbol);
        if (sym)
            return reinterpret_cast<FnType>(sym);
    }

    return nullptr;
}

static AddListenerFn getRealAddListener()
{
    static AddListenerFn fn = resolveWaylandSymbol<AddListenerFn>("wl_proxy_add_listener");
    return fn;
}

using AddDispatcherFn = int (*)(struct wl_proxy*, wl_dispatcher_func_t, const void*, void*);
using MarshalArrayFlagsFn = wl_proxy* (*)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);

static AddDispatcherFn getRealAddDispatcher()
{
    static AddDispatcherFn fn = resolveWaylandSymbol<AddDispatcherFn>("wl_proxy_add_dispatcher");
    return fn;
}

static MarshalArrayFlagsFn getRealMarshalArrayFlags()
{
    static MarshalArrayFlagsFn fn = resolveWaylandSymbol<MarshalArrayFlagsFn>("wl_proxy_marshal_array_flags");
    return fn;
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_listener(struct wl_proxy* proxy,
                          void (**implementation)(void),
                          void* data)
{
    AddListenerFn real = getRealAddListener();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, implementation, data);

    // Skip overlay-owned proxies — they use their own listeners
    if (VKIntox::isOverlayProxy(proxy))
        return real(proxy, implementation, data);

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto* gameListener = (const wl_pointer_listener*)implementation;
        auto* ptr = (wl_pointer*)proxy;

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GamePointerData gpd;
            gpd.original = *gameListener;
            gpd.userData = data;
            gamePointers[ptr] = gpd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game pointer listener");
        return real(proxy, (void (**)(void))&wrapperPointerListener, nullptr);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto* gameListener = (const wl_keyboard_listener*)implementation;
        auto* kb = (wl_keyboard*)proxy;

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GameKeyboardData gkd;
            gkd.original = *gameListener;
            gkd.userData = data;
            gameKeyboards[kb] = gkd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game keyboard listener");
        return real(proxy, (void (**)(void))&wrapperKeyboardListener, nullptr);
    }

    return real(proxy, implementation, data);
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_dispatcher(struct wl_proxy* proxy,
                            wl_dispatcher_func_t dispatcher_func,
                            const void* dispatcher_data,
                            void* data);

extern "C" __attribute__((visibility("default")))
wl_proxy* wl_proxy_marshal_array_flags(struct wl_proxy* proxy,
                                       uint32_t opcode,
                                       const struct wl_interface* iface,
                                       uint32_t version,
                                       uint32_t flags,
                                       union wl_argument* args)
{
    MarshalArrayFlagsFn real = getRealMarshalArrayFlags();
    if (!real)
        return nullptr;

    // Block compositor window move/resize requests while overlay input is blocked.
    // xdg_toplevel requests:
    //   4: show_window_menu
    //   5: move
    //   6: resize
    const char* cls = wl_proxy_get_class(proxy);
    if (pointerBlocked() && cls && std::strcmp(cls, "xdg_toplevel") == 0 &&
        (opcode == 4 || opcode == 5 || opcode == 6))
    {
        return nullptr;
    }

    if (cls)
    {
        // feed the explicit-sync gate: pairings, point sets and every commit,
        // so it can hold foreign commits out of an open present sequence
        if (std::strcmp(cls, "wl_surface") == 0)
        {
            if (opcode == 6) // commit
            {
                const uint32_t id = wl_proxy_get_id(proxy);
                VKIntox::WaylandSyncobjGate::beginCommit(id);
                struct wl_proxy* result = real(proxy, opcode, iface, version, flags, args);
                VKIntox::WaylandSyncobjGate::endCommit(id);
                return result;
            }
            else if (opcode == 0 && gameCursor.surface == proxy) // destroy
            {
                // the remembered cursor surface is gone; nothing to restore
                gameCursor.kind = GameCursor::Kind::none;
                gameCursor.surface = nullptr;
            }
        }
        else if (std::strcmp(cls, "wl_pointer") == 0 && opcode == 0) // set_cursor
        {
            // args: serial, surface, hotspot_x, hotspot_y. A null surface is
            // the overlay hiding the pointer (its own resource, or the game's
            // on the capture surface); a real surface is the game's cursor to
            // remember for the re-enter.
            if (args && !args[1].o)
                gameCursorHiddenByOverlay = true;
            else if (args)
            {
                gameCursor.kind = GameCursor::Kind::surface;
                gameCursor.surface = reinterpret_cast<struct wl_proxy*>(args[1].o);
                gameCursor.hotX = args[2].i;
                gameCursor.hotY = args[3].i;
                gameCursor.shapeDevice = nullptr;
            }
        }
        else if (std::strcmp(cls, "wp_cursor_shape_device_v1") == 0 && opcode == 1) // set_shape
        {
            // args: serial, shape
            if (args && !VKIntox::isOverlayProxy(proxy))
            {
                gameCursor.kind = GameCursor::Kind::shape;
                gameCursor.shapeDevice = proxy;
                gameCursor.shape = args[1].u;
                gameCursor.surface = nullptr;
            }
        }
        else if (std::strcmp(cls, "wp_linux_drm_syncobj_surface_v1") == 0)
        {
            const uint32_t id = wl_proxy_get_id(proxy);
            if (opcode == 0) // destroy
                VKIntox::WaylandSyncobjGate::noteSyncobjDestroyed(id);
            else if (opcode == 1 || opcode == 2) // set_acquire_point / set_release_point
                VKIntox::WaylandSyncobjGate::notePointSetForSyncobj(id);
        }
        else if (std::strcmp(cls, "wp_linux_drm_syncobj_manager_v1") == 0 && opcode == 1) // get_surface
        {
            // args[0] is the new_id slot, args[1] the surface it describes
            struct wl_proxy* created = real(proxy, opcode, iface, version, flags, args);
            if (created && args)
                VKIntox::WaylandSyncobjGate::noteSyncobjSurface(
                    wl_proxy_get_id(created),
                    wl_proxy_get_id(reinterpret_cast<struct wl_proxy*>(args[1].o)));
            return created;
        }
    }

    return real(proxy, opcode, iface, version, flags, args);
}

extern "C" __attribute__((visibility("default")))
void* dlsym(void* handle, const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    void* resolved = realDlsym(handle, symbol);

    // Keep behavior unchanged unless VKIntox is explicitly enabled.
    // Cache the check — this function is called for every dlsym in the process.
    static const bool VKIntoxEnabled = []{
        const char* e = std::getenv("ENABLE_VKINTOX");
        return e && std::strcmp(e, "1") == 0;
    }();
    if (!VKIntoxEnabled || !resolved)
        return resolved;

    bool wantsListener = (std::strcmp(symbol, "wl_proxy_add_listener") == 0);
    bool wantsDispatcher = (std::strcmp(symbol, "wl_proxy_add_dispatcher") == 0);
    bool wantsMarshalArrayFlags = (std::strcmp(symbol, "wl_proxy_marshal_array_flags") == 0);
    if (!wantsListener && !wantsDispatcher && !wantsMarshalArrayFlags)
        return resolved;

    Dl_info info{};
    if (dladdr(resolved, &info) == 0 || !info.dli_fname)
        return resolved;

    std::string module(info.dli_fname);
    std::transform(module.begin(), module.end(), module.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Only redirect lookups that currently resolve to libwayland-client.
    if (module.find("libwayland-client") == std::string::npos)
    {
        return resolved;
    }

    if (wantsListener)
        return reinterpret_cast<void*>(&wl_proxy_add_listener);
    if (wantsDispatcher)
        return reinterpret_cast<void*>(&wl_proxy_add_dispatcher);
    return reinterpret_cast<void*>(&wl_proxy_marshal_array_flags);
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_dispatcher(struct wl_proxy* proxy,
                            wl_dispatcher_func_t dispatcher_func,
                            const void* dispatcher_data,
                            void* data)
{
    AddDispatcherFn real = getRealAddDispatcher();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, dispatcher_func, dispatcher_data, data);

    // Skip overlay-owned proxies — they use their own dispatchers
    if (VKIntox::isOverlayProxy(proxy))
        return real(proxy, dispatcher_func, dispatcher_data, data);

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto* ptr = static_cast<wl_proxy*>(proxy);

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GamePointerDispatcherData gpd;
            gpd.original = dispatcher_func;
            gpd.dispatcherData = dispatcher_data;
            gamePointerDispatchers[ptr] = gpd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game pointer dispatcher");
        return real(proxy, wd_pointer, nullptr, data);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto* kb = static_cast<wl_proxy*>(proxy);

        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            GameKeyboardDispatcherData gkd;
            gkd.original = dispatcher_func;
            gkd.dispatcherData = dispatcher_data;
            gameKeyboardDispatchers[kb] = gkd;
        }

        VKIntox::Logger::debug("Wayland interpose: wrapped game keyboard dispatcher");
        return real(proxy, wd_keyboard, nullptr, data);
    }

    return real(proxy, dispatcher_func, dispatcher_data, data);
}

namespace VKIntox
{
    bool waylandPointerWrapped()
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);
        return !gamePointers.empty() || !gamePointerDispatchers.empty();
    }

    bool waylandKeyboardWrapped()
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);
        return !gameKeyboards.empty() || !gameKeyboardDispatchers.empty();
    }

    // Edge-triggered by the blocker: on withhold, every key the game saw a
    // press for is synth-released so a held movement key stops; on release,
    // keys still owned by the game are synth-pressed back. Keys the overlay
    // consumed while blocked are left alone — the game never saw them.
    void withholdGameKeys(bool withhold)
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);

        const uint32_t state = withhold ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED;
        size_t changed = 0;

        for (auto& [proxy, own] : keyOwnership)
        {
            if (own.gameOwned.empty())
                continue;

            auto lit = gameKeyboards.find((wl_keyboard*)proxy);
            auto dit = gameKeyboardDispatchers.find((wl_proxy*)proxy);
            if (lit != gameKeyboards.end() && lit->second.original.key)
            {
                for (uint32_t key : own.gameOwned)
                {
                    lit->second.original.key(lit->second.userData, (wl_keyboard*)proxy, 0, 0, key, state);
                    changed++;
                }
            }
            else if (dit != gameKeyboardDispatchers.end() && dit->second.original)
            {
                for (uint32_t key : own.gameOwned)
                {
                    union wl_argument args[4] = {};
                    args[0].u = 0; // serial
                    args[1].u = 0; // time
                    args[2].u = key;
                    args[3].u = state;
                    dit->second.original(dit->second.dispatcherData, proxy, 3, &wl_keyboard_interface.events[3], args);
                    changed++;
                }
            }
        }

        if (changed > 0)
            Logger::debug(std::string("Wayland interpose: ") + (withhold ? "released " : "re-pressed ")
                          + std::to_string(changed) + " held game key(s)");
    }
} // namespace VKIntox
