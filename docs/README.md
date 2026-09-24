<div align="center">

# vkintox

vulkan post-processing layer with advanced depth buffer resolve for linux.

[![License](https://img.shields.io/badge/license-zlib-green?style=flat-square)](./LICENSE) ![Version](https://img.shields.io/badge/version-0.1.0-blue?style=flat-square)

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
*   **unified setup:** one script for Sober, Flatpak games, and native system installs.

## how it works

vkintox operates as a vulkan layer that sits between the game and the driver.

1.  **interception:** it captures vulkan swapchain calls.
2.  **depth management:** unlike standard implementations, it actively resolves depth buffers to prevent z-fighting in effects like ambient occlusion.
3.  **shader injection:** it compiles reshade shaders locally and injects them into the render pipeline without requiring game restarts for config changes.

## requirements

*   **gpu:** vulkan-capable hardware with recent drivers.
*   **setup:** `just`; Sober setup also uses Flatpak, Python 3, curl, and unzip. System installation uses Meson, Ninja, and sudo.
*   **native build:** Clang/Clang++, ccache, Meson, Ninja, glslangValidator, Wayland/X11 development libraries.

> [!IMPORTANT]
> some anti-cheat systems or games using dxvk/vkd3d may conflict with vulkan layers. if you experience crashes, try disabling `ENABLE_VKINTOX=1` for that specific title.

## installation

### for Sober (Flatpak)
This builds VKIntox as a Flatpak Vulkan layer, installs the ReShade packages listed in `../assets/EffectPackages.ini`, and enables the layer in Sober. Re-running it skips the build and package download when their inputs have not changed.

```bash
git clone https://github.com/buwryme/VKIntox.git
cd VKIntox
./setup sober
```

### for another Flatpak game
This runs the same build, package installation, and configuration steps as Sober, targeting the selected app ID and its Flatpak config directory:

```bash
./setup flatpak com.target.app
```

### for native games
Build and install VKIntox system-wide (the script uses `sudo` for installation):

```bash
./setup system
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

1.  try hitting your config reload keybind (default: `F10`)
2.  restart your game

## known issues

*   **initial startup:** a restart may be required after the first launch for stability
*   **graphics switching:** changing quality settings in-game often leads to crashes
*   **performance:** shader compilation on first load may cause a brief stutter.
*   **stability:** stability in the catalog is *very* poor. it's best to not use it

## troubleshooting / FAQ

*   **logs:** vkintox saves logs to `/path/to/config/VKIntox/vkintox.log`. to capture diagnostics, run sober with debug logging enabled:

```bash
flatpak run --env=VKINTOX_LOG_LEVEL=debug org.vinegarhq.Sober
```

you can then inspect the log file at `~/.var/app/org.vinegarhq.Sober/config/VKIntox/vkintox.log`. (in the case of specifically Sober)

*   **sober stopped launching:** try running `./setup sober` again, and run:

```bash
flatpak override --user --env=VK_LOADER_DRIVERS_DISABLE=lvp_icd.x86_64.json --env=__NV_PRIME_RENDER_OFFLOAD=1 --env=__GLX_VENDOR_LIBRARY_NAME=nvidia --env=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json org.sober.Sober
```

and also make sure Sober, and your flatpak driver runtimes are up to date:

```bash
flatpak upgrade org.vinegarhq.Sober
```

*   **sober freezes on launch:** i suggest trying a simple restart... 1-2 times.
*   **sober freezes inside the catalog!:** opening the catalog re-builds graphics constantly that leads to freezes... don't use it
*   **my effects are off on launch?:** press F1 (or whatever you have it set to, if you changed). effects are OFF on launch by default for more stable startups
*   **my 3d effects look frozen!:** press your reload keybind (default: `F10`) to reload depth
*   **i just added an .ini preset from ReShade, how do I use it?:** reload your config once you've put your .ini file in the right directory (`/path/to/config/VKIntox/configs/shaders/`), and it will appear on the Shader INI dropdown

---

> experimental software. use at your own risk.

special thanks to **slobodaapl** (vkShade), **DadSchoorse** (vkBasalt), **Daaboulex** (Wayland overlay), **crosire** (ReShade), and **ocornut** (Dear ImGui).

<div align="center">

**maintained by [buwryme](https://github.com/buwryme)**

</div>
