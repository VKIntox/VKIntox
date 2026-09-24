<div align="center">

# vkintox

vulkan post-processing layer with advanced depth buffer resolve for linux.

[![License](https://img.shields.io/badge/license-zlib-green?style=flat-square)](./LICENSE)

</div>

---

an independent fork of **vkShade** that fixes the depth buffer issues inherent to vkBasalt. it provides a robust, reshade-like experience for native linux games and flatpak runtimes like [Sober](https://vinegarhq.org/Home/index.html).

### as shown here:

<div align="center">

<img src="assets/showcase_screenshot1.png" width="45%" alt="VKIntox in action"> <img src="assets/showcase_screenshot2.png" width="45%" alt="VKIntox UI">

<img src="assets/showcase_screenshot3.png" width="90%" alt="Depth resolve modes">

</div>

## features

*   **advanced depth resolve:** automatically picks the correct depth buffer or allows manual selection (Reverse-Z, etc).
*   **in-game overlay:** a native gui toggled via `HOME` to manage effects on the fly.
*   **reshade compatibility:** supports `.fx` files by compiling them to spir-v at runtime.
*   **sober integration:** a dedicated setup script for running roblox with full post-processing support.

## how it works

vkintox operates as a vulkan layer that sits between the game and the driver.

1.  **interception:** it captures vulkan swapchain calls.
2.  **depth management:** unlike standard implementations, it actively resolves depth buffers to prevent z-fighting in effects like ambient occlusion.
3.  **shader injection:** it compiles reshade shaders locally and injects them into the render pipeline without requiring game restarts for config changes.

## requirements

*   **gpu:** vulkan-capable hardware with recent drivers.
*   **sober setup:** python3, curl, unzip.
*   **native build:** gcc 9+, meson, ninja, glslangValidator, wayland/x11 development libraries.

> [!IMPORTANT]
> some anti-cheat systems or games using dxvk/vkd3d may conflict with vulkan layers. if you experience crashes, try disabling `ENABLE_VKINTOX=1` for that specific title.

## installation

### for sober (flatpak)
this script fetches the gnome sdk, compiles vkintox, and installs it as a flatpak extension.

```bash
git clone https://github.com/buwryme/VKIntox.git
cd VKIntox
chmod +x setup_sober.sh; ./setup_sober.sh
```

### for native games
build and install system-wide.

```bash
git clone https://github.com/buwryme/VKIntox.git
cd VKIntox
meson setup --buildtype=release --prefix=/usr build-release
ninja -C build-release
sudo ninja -C build-release install
```

to enable for a specific game, launch it with:
```bash
ENABLE_VKINTOX=1 your_game_command
```

## usage & keybinds

| key | action |
| :--- | :--- |
| `Home` | toggle the overlay gui |
| `End` | enable/disable all active effects |
| `F10` | reload configuration and recompile shaders |

### configuring depth
if depth-dependent effects (like DOF) look incorrect:
1.  open the **Advanced** tab in the overlay.
2.  try different **Depth Resolve Modes**.
3.  press `F10` to apply changes.
4.  for **roblox**, use **Reverse-Z** with inversion **ON**.

## known issues

*   **initial startup:** a restart may be required after the first launch for depth detection to stabilize.
*   **graphics switching:** changing quality settings in-game can lead to crashes or the depth buffer "freezing".
*   **performance:** shader compilation on first load may cause a brief stutter.
*   **stability:** stability in the home screen is *very* poor. it's best to join games via your browser instead

## troubleshooting

*   **logs:** Sober logs are saved to `~/.var/app/org.vinegarhq.Sober/config/VKIntox/vkintox.log`. to capture diagnostics, enable debug logging before launching Sober:

```bash
flatpak override --user --env=VKINTOX_LOG_LEVEL=debug org.vinegarhq.Sober
```

Remove the override once done with `flatpak override --user --unset-env=VKINTOX_LOG_LEVEL org.vinegarhq.Sober`.

*   **sober stopped launching:** try running `setup_sober.sh` again, and run:

```bash
flatpak override --user --env=VK_LOADER_DRIVERS_DISABLE=lvp_icd.x86_64.json --env=__NV_PRIME_RENDER_OFFLOAD=1 --env=__GLX_VENDOR_LIBRARY_NAME=nvidia --env=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json org.sober.Sober
```

and also make sure Sober, and your flatpak driver runtimes are up to date:

```bash
flatpak upgrade org.vinegarhq.Sober
```

*   **sober freezes on launch:** i suggest trying a simple restart... 1-2 times.
*   **sober freezes inside the catalog!:** opening the catalog re-builds graphics constantly that leads to freezes... don't use it

---

> experimental software. use at your own risk.

special thanks to **slobodaapl** (vkShade) and **DadSchoorse** (vkBasalt).

<div align="center">

**maintained by [buwryme](https://github.com/buwryme)**

</div>
