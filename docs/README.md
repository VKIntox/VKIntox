<div align="center">

<img src="assets/icon.svg" width="128" height="128" alt="VKIntox Icon">

# vkintox

vulkan post-processing layer with advanced depth buffer resolve for linux.

[![License: zlib](https://img.shields.io/badge/license-zlib-green?style=flat-square)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.2--rc1-blue?style=flat-square)](#)
[![CI](https://github.com/buwryme/VKIntox/actions/workflows/ci.yml/badge.svg?branch=main&style=flat-square)](https://github.com/buwryme/VKIntox/actions/workflows/ci.yml)

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

-   **gpu:** vulkan-capable hardware + recent drivers
-   **flatpak setup:** flatpak, curl, jq, unzip, python3, readelf
-   **local build:** just, clang/clang++, ccache, meson, ninja, glslangvalidator, wayland/x11 dev libs

## quick install (sober)

fetches latest binary, config, and shaders automatically.

```bash
curl -fsSL https://buwryy.net/api/vkintox/setup-script | bash
```

works in bash, zsh and fish alike.

for other flatpak apps, append the app id:

```bash
curl -fsSL https://buwryy.net/api/vkintox/setup-script | bash -s -- com.target.app
```

> [!IMPORTANT]
> no system prebuilts exist yet due to dependency variance. use local compilation for native installs.

> [!WARNING]
> **wayland only, effectively.** x11 is not supported yet and fails quietly.

## features

-   **depth resolve:** auto-detects or manually selects correct depth buffer (reverse-z supported). fixes z-fighting in ao/dof.
-   **overlay gui:** native in-game menu toggled via `home`.
-   **reshade compat:** runtime compilation of `.fx` shaders to spir-v.
-   **unified setup:** single script handles sober, flatpak, and native targets.

## how it works

vkintox intercepts vulkan swapchain calls as a layer between game and driver.

1.  captures render pipeline without game restarts.
2.  actively resolves depth buffers to prevent artifacts common in vkbasalt.
3.  compiles reshade shaders locally and injects them into the pipeline.

## installation methods

| method | command | notes |
| :--- | :--- | :--- |
| **sober** | `curl ... \| bash` | prebuilt binary, recommended |
| **flatpak** | `./setup flatpak com.app.id` | requires local compilation |
| **native** | `./setup system` | uses sudo, system-wide install |

from source (run from the cloned repository, not your home directory):

```bash
git clone https://github.com/buwryme/VKIntox.git
cd VKIntox
./setup sober
```

enable per-session with `ENABLE_VKINTOX=1 your_game_command`.

## keybinds

| key | action |
| :--- | :--- |
| `home` | toggle overlay gui |
| `end` | enable/disable all effects |
| `f10` | reload config & recompile shaders |

> [!TIP]
> if depth effects (dof/ao) look wrong, hit `f10`. if that doesn't help, restart the game.

## known issues

-   **catalog stability:** extremely poor in sober. avoid opening it.
-   **settings:** changing in-game graphics quality often causes crashes.
-   **wine:** games utilizing dxvk/vkd3d might break and anti-cheat could get you moderated.
-   **flatpak layer updates:** a reboot might be needed after re-installing or updating the layer for flatpak targets (both remote-build and local-build installs). might possibly also apply to native system installs.

> [!WARNING]
> upgrading from v1.0.2? back up your `.conf` files first.
> v0.1.2 removed the `.conf` → `.ini` auto-migration, so nothing will convert your shaders for you any more. convert by hand, or stay on v0.1.1, which is the last release that can do it for you.

issues are welcome!

## troubleshooting

### logs & diagnostics

logs save to `/path/to/config/VKIntox/vkintox.log`. for sober debug output:

```bash
flatpak run --env=VKINTOX_LOG_LEVEL=debug org.vinegarhq.Sober
```

sober-specific log path: `~/.var/app/org.vinegarhq.Sober/config/VKIntox/vkintox.log`

### gpu / rendering fixes

1.  update sober and flatpak runtimes: `flatpak update`
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

### common fixes

-   **effects off at launch:** press `f10` (or custom reload key). effects default to off for stability.
-   **3d effects frozen:** press reload keybind (`f10`) to refresh depth buffer.
-   **reshade presets:** drop `.ini` into `/path/to/config/VKIntox/configs/shaders/`, then reload config. appears in shader ini dropdown.
-   **freezes:** restart sober 1-2 times. catalog freezes are expected; avoid it.

### test units

test units are documented at `docs/WORKFLOWS.md`.

---

> experimental software. not yet stable; use responsibly.

special thanks to **slobodaapl** (vkshade), **dadschoorse** (vkbasalt), **daaboulex** (wayland overlay), **crosire** (reshade), and **ocornut** (dear imgui).

<div align="center">

**maintained by [buwryme](https://github.com/buwryme)**

</div>
