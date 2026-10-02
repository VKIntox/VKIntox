# workflows

GitHub Actions runs the source syntax check, config serialization tests, ReShade shader checks, and a vkcube smoke test on Ubuntu.

## local checks

Build VKIntox and the config test executable:

```bash
meson setup build
meson compile -C build
```

Run the config serialization tests:

```bash
meson test -C build --print-errorlogs
```

Smoke test the layer by running `vkcube` under it for a few seconds and failing if the process dies:

```bash
src/tests/smoke_vkcube.sh                      # 11s against the default build
src/tests/smoke_vkcube.sh build/src/libvkintox.so 6
```

It needs `vkcube` from `vulkan-tools` and a display. It uses its own layer manifest under a
name nothing else uses, and sets `VK_LOADER_LAYERS_ENABLE` so that copy is the one in the
chain: on a machine with VKIntox already installed, an installed layer would otherwise be
discovered too and could end up shadowing the build under test, making the run quietly
validate stale code. The script fails rather than passes if the loader never loaded the
library it was pointed at, so a green result always means the layer really ran.

Without a GPU, point it at a software implementation:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  src/tests/smoke_vkcube.sh
```

On a headless machine, wrap it in `xvfb-run -a`, which is what CI does.

Compile every ReShade shader from the package list fetched by `setup`:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkintox-shader-check
```

To use an existing installed shader tree instead of downloading the listed packages, pass its `Shaders` directory:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkintox-shader-check \
  --shader-dir "$HOME/.var/app/org.vinegarhq.Sober/config/VKIntox/reshade/packages/Shaders"
```

Set `VKINTOX_EFFECT_PACKAGES_URL` to use a different `EffectPackages.ini` URL. The default matches `scripts/setup`.
