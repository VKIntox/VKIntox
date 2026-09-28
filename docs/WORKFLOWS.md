# workflows

GitHub Actions runs the source syntax check, config serialization tests, and ReShade shader checks on Ubuntu.

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
