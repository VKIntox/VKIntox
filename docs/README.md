<div align="center">

<img src="assets/icon.svg" width="128" height="128" alt="VKIntox Icon">

# vkintox

proper reshade (with depth!) for linux games, on linux. utilizes vulkan layers. batteries included, along with a fun, lively material 3 design

[![version](https://img.shields.io/badge/version-0.2.0-blueviolet?style=flat-square)](#)
[![license](https://img.shields.io/badge/license-zlib-2e8b57?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-linux-555555?style=flat-square)](#)
[![CI](https://github.com/VKIntox/VKIntox/actions/workflows/ci.yml/badge.svg?branch=main&style=flat-square)](https://github.com/VKIntox/VKIntox/actions/workflows/ci.yml)
[![discord server](https://img.shields.io/badge/discord-server-5865f2?style=flat-square&logo=discord&logoColor=white)](https://discord.gg/HrUrzXAxPV)

</div>

---

## showcase

<div align="center">
  <img src="assets/showcase_screenshot1.png" width="45%" alt="VKIntox in action">&nbsp;&nbsp;
  <img src="assets/showcase_screenshot2.png" width="45%" alt="VKIntox UI">
</div>

<div align="center">
  <img src="assets/showcase_screenshot3.png" width="90%" alt="Depth resolve modes">
</div>

## requirements

-   **gpu:** vulkan-capable hardware with recent drivers ([lavapipe](https://docs.mesa3d.org/drivers/llvmpipe.html) works for testing)
-   **remote install:** flatpak, curl, jq, unzip, python3, readelf
-   **local build:** just, clang/clang++, ccache, meson, ninja, glslangvalidator, wayland and/or x11 dev headers

## usage

### install

supports flatpak and multiple distros (arch, fedora, debian, nix, openSUSE)

```bash
curl -fsSL https://buwryy.net/api/vkintox/setup-script | bash
```

uninstall (keeps config):

```bash
curl -fsSL https://buwryy.net/api/vkintox/setup-script | bash -s -- uninstall
```

the script lets you choose everything interactively, and lets you download preview versions.

from a checkout:

```bash
git clone https://github.com/VKIntox/VKIntox.git
cd VKIntox
./setup sober          # or: ./setup flatpak com.app.id | ./setup system
```

enable per-session with `ENABLE_VKINTOX=1 your_game_command`.

### keybinds

| key | action |
| :--- | :--- |
| `home` | toggle the overlay gui |
| `end` | enable/disable every effect |
| `f10` | reload config and recompile shaders |

## features

**overlay**

-   **material 3 ui:** the overlay is a from-scratch material 3 implementation that patches the vendored dear imgui directly. real m3 widgets (filled/tonal/outlined buttons, switches, sliders, chips, progress, cards), an animated press morph, elevation, and slide-and-fade transitions between views. the shell itself rises into place on open and sinks back down on close with the same bezier as the dialogs, its shadow, title bar, and blurred backdrop fading in step.
-   **navigation:** the top nav is one connected button group. the selected segment swaps to the secondary-container colour and its accent pill widens as it slides between tabs.
-   **menus:** one shared module backs every dropdown and context menu: wider panels, centred labels, a thin scrollbar that hides when the list fits, width-based label truncation, and an accent on the selected row. the shader INI dropdown bullets the active preset.
-   **theme editor:** pick a seed colour, variant (expressive, tonal-spot, vibrant, neutral, monochrome, fidelity, content), contrast and light/dark; edit every colour role; tune density and corner scale; and browse a live widget gallery. changes write to `theme.colors` and hot-reload. **matugen is supported with hot reloading!**
-   **backdrop blur:** the frame behind the overlay is captured, downsampled, and blurred with a separable gaussian on ping-pong targets before the ui is drawn over it. the theme editor exposes the blur toggle, background opacity, size, and pass count (1–10, default 7). the foreground stays sharp, and nothing is recorded while the blur is off or the background is opaque.
-   **window:** resizable and movable, anchored to the game framebuffer, with right-click title drag, a close button, and an about tab that reports the version compiled into the library. the title bar fades with the shell animation.

**effects**

-   **reshade fx compat:** runtime compilation of `.fx` shaders to spir-v, cached per shader and extent.
-   **depth resolve:** auto-detects or manually selects the correct depth buffer to fix z-fighting in ao and dof. depth capture is on by default, so depth effects work on a game's first launch without a restart, and per-game defaults seed themselves; sober starts with inverted depth, everything else does not.
-   **effect list:** card-based and scrolls independently of the view, with the preset actions right-aligned on the effects row.
-   **add effects:** a dedicated view with a back breadcrumb, package tabs (all / vkintox / every installed pack), ranked search, recents, keyboard navigation, and a per-effect duplicate stepper (`-` count `+`) that stops at the max-effects limit.
-   **shader presets:** create (inherits the active preset), rename, delete behind a confirm, and import a reshade `.ini` through the desktop portal, with an in-overlay browser as the fallback.
-   **shader manager:** compile-test every discovered shader, grey the test out once the installed set is unchanged, and flag which shaders need a depth buffer.
-   **built-in shaders:** a from-scratch morphological msaa shader (search steps, edge threshold, blend strength, three refinement passes) and an exponential height fog, each shipping in its own `vkintox` package.
-   **per-app profiles:** one config per detected game, shown by name and a joystick icon.

**diagnostics**

-   frame time and gpu stats sampled from launch, plus a debug window for the effect registry and log output.

**input**

-   x11 and wayland backends, with overlay popups (dialogs, dropdowns) withholding pointer input so clicks never leak into the game.

**setup**

-   **one installer** for sober, flatpak and native targets, with an interactive version picker and an uninstall that leaves your config alone.
-   **assets:** seeds the full google sans + material symbols set and their licences, installs effect packages under `reshade/` grouped by package, and drops the built-in vkintox shaders beside them — those live in their own repo, [VKIntoxEffects](https://github.com/VKIntox/VKIntoxEffects), and install as one of the effect packages.

**under the hood**

-   raii for every c resource, `std::optional` registry lookups, a central deferred-destroy queue, atomic config writes, and six unit suites (documented in `docs/WORKFLOWS.md`).

## how it works

vkintox intercepts vulkan swapchain calls as a layer between the game and the driver.

1.  captures the render pipeline without a game restart.
2.  resolves depth buffers to prevent the artifacts common in vkbasalt.
3.  compiles reshade shaders locally and runs them in the effect chain.
4.  draws the overlay onto the swapchain image after the effects, capturing and blurring that frame first when the backdrop blur is on.

## caveats

### known issues

-   **wine:** games using dxvk/vkd3d might break (depends on the game itself), and anti-cheat *could* get you moderated.
-   **flatpak layer updates:** a reboot can be needed after re-installing or updating the layer for flatpak targets (remote and local builds alike). doesn't apply to native installs.

**flatpak runtime branch:** the flatpak layer is built for `org.freedesktop.Platform` 26.08. apps on other freedesktop branches or on GNOME/KDE runtimes will not load it.

> [!IMPORTANT]
> Sober's homescreen stability remains poor. Either 1) Switch to Roséline or 2) Join games with browser instead
> This, unfortunately, is mostly a Roblox-sided issue, and/or isn't in my hands, 
> as this doesn't happen on macOS builds, but strangely happens on Android APK builds (Sober)

### troubleshooting

**logs and diagnostics.** logs save to `/path/to/config/VKIntox/vkintox.log`, and the `about` tab reports the embedded version. for sober debug output:

```bash
flatpak run --env=VKINTOX_LOG_LEVEL=debug org.vinegarhq.Sober
```

the sober log path is `~/.var/app/org.vinegarhq.Sober/config/VKIntox/vkintox.log`.

**gpu and rendering fixes.**

1.  update the runtimes: `flatpak update`
2.  clear stale overrides:
    ```bash
    flatpak override --user --unset-env=VK_ICD_FILENAMES \
      --unset-env=VK_LOADER_DRIVERS_DISABLE \
      --unset-env=__NV_PRIME_RENDER_OFFLOAD \
      --unset-env=__GLX_VENDOR_LIBRARY_NAME org.vinegarhq.Sober
    ```
3.  force nvidia on hybrid laptops:
    ```bash
    flatpak override --user --env=__NV_PRIME_RENDER_OFFLOAD=1 \
      --env=__GLX_VENDOR_LIBRARY_NAME=nvidia org.vinegarhq.Sober
    ```

verify active drivers with `flatpak --gl-drivers`. host and flatpak nvidia runtime versions must match.

**common fixes.**

-   **effects off at launch:** press `f10` (or your reload key) to re-enable them. effects are on by default, so a shader that failed to compile at launch is the usual cause.
-   **3d effects frozen:** press the reload keybind (`f10`) to refresh the depth buffer.
-   **reshade presets:** drop the `.ini` into `/path/to/config/VKIntox/configs/shaders/`, then reload. it appears in the shader ini dropdown.

> [!NOTE]
> wayland and x11 are both supported. on x11 (including xwayland) the overlay takes a pointer grab while the cursor is over it, and a keyboard grab only while it is consuming keys (a text field is active or a popup is open); if the compositor does not honour xwayland's keyboard-grab protocol, the game can still receive keys while you type in the overlay. games that don't route through an abstractor like SDL or GLFW may handle input in ways the overlay can't intercept, so input blocking can stop working.

## credits

thanks to **slobodaapl** (vkshade), **dadschoorse** (vkbasalt), **daaboulex** (wayland overlay), **crosire** (reshade), and **ocornut** (dear imgui).

---

> [!IMPORTANT]
> beta-stage software.

<div align="center">

made with ♡ by [buwryme](https://github.com/buwryme). join the [discord server!](https://discord.gg/HrUrzXAxPV)

</div>
