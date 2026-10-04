# workflows

GitHub Actions builds VKIntox, runs the unit tests, compiles every ReShade shader from the package list, and smoke-tests the layer under `vkcube`. CI runs on Ubuntu with lavapipe, so the shader and smoke jobs exercise the layer without a GPU.

## local checks

Build the layer and every test executable:

```bash
meson setup build
meson compile -C build
```

Run all six unit tests:

```bash
meson test -C build --print-errorlogs
```

Each test is a small `main()` that prints `N checks, M failures` and exits non-zero on failure, so `meson test` reports pass/fail per suite.

## what the tests cover

**config serializer** (`src/tests/config_serializer_test.cc`). Exercises `ConfigSerializer`: profile save/load round-trips, the legacy `.conf` to per-game migration, settings persistence, imported ReShade presets, disabled-effect sidecars, and the rejection of path separators in config/profile names. Tells you that writing a profile and reading it back gives the same effects, params and macros — the file format the whole UI depends on.

**vk handle ordering** (`src/render/vk_handle_test.cc`). Exercises `DeferredDestroyQueue`: releases run in ascending phase order, within a phase in reverse registration order, `flush` drains exactly once, and `discard` drops without running. Tells you that a view is destroyed before the image it was made from and device memory is freed last — the order that keeps a driver from faulting during teardown, which a headless test cannot observe through a real GPU.

**async writer** (`src/core/async_writer_test.cc`). Exercises the background `AsyncWriter`: jobs run FIFO, a throwing job does not poison the queue, a null job is dropped, `waitForIdle` returns on an unused writer, and concurrent submitters lose nothing. Tells you that profile and settings writes leave the present thread without being dropped or reordered.

**compile cache** (`src/effects/compile_cache_test.cc`). Exercises `CompileCache`: store/lookup round-trip, source and flag partitioning, re-store is a no-op, FIFO eviction at capacity, `clear`, hash determinism, and a threaded store/lookup. Tells you the cache that keeps a chain reload from re-parsing every ReShade effect is keyed correctly and bounded.

**depth copy state** (`src/render/depth_copy_state_test.cc`). Exercises `DepthCopyState`: publish/consume returns each copy once, the ring hands out slots in order, `detach` clears, and a threaded publish/consume never observes a torn `DepthState`. Tells you the mutex guarding the `CmdEndRenderPass` → `QueueSubmit` handoff and the copy ring actually holds — the race it covers only shows as an intermittent device loss under load.

**m3 theme** (`src/tests/m3_theme_test.cc`). Links the M3 theme and ImGui core with no renderer and exercises colour maths, token parsing, the exporter round-trip, the button press morph, and the connected button-group geometry. Tells you a colour, theme-file or motion regression is caught without a GPU, since none of it can be eyeballed in CI.

## reshade shader check

Compile every shader from the package list fetched by `setup`:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkintox-shader-check
```

This does not need a GPU: `vkintox-shader-check` runs the embedded reshadefx compiler over each `.fx` and exits non-zero if any shader fails. It tells you the compiler integration still builds a real community shader pack, which the unit tests never touch.

To use an installed shader tree instead of downloading the listed packages, pass its `Shaders` directory:

```bash
python3 src/tests/test_reshade_shaders.py \
  --shader-check build/src/vkintox-shader-check \
  --shader-dir "$HOME/.var/app/org.vinegarhq.Sober/config/VKIntox/reshade/packages/Shaders"
```

Set `VKINTOX_EFFECT_PACKAGES_URL` to use a different `EffectPackages.ini` URL. The default matches `scripts/setup`.

## smoke test

Run `vkcube` under the freshly built layer for a few seconds and fail if the process dies:

```bash
src/tests/smoke_vkcube.sh                                  # 11s against the default build
src/tests/smoke_vkcube.sh build/src/libvkintox.so 6
```

It needs `vkcube` from `vulkan-tools` and a display. It writes its own layer manifest under a name nothing else uses and sets `VK_LOADER_LAYERS_ENABLE` so that copy is the one in the chain: on a machine with VKIntox already installed, the installed layer would otherwise be discovered too and could shadow the build under test, making a green run validate stale code. The script fails rather than passes if the loader never loaded the library it was pointed at, so a green result always means the layer really ran.

It seeds a config with one depth-consuming effect and asserts the effect built a render pass and that a depth attachment was seen, so a run that silently falls back to pass-through fails instead of passing vacuously.

Without a GPU, point it at a software implementation:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  src/tests/smoke_vkcube.sh
```

On a headless machine wrap it in `xvfb-run -a`, which is what CI does. Under X11 plus lavapipe the layer is deliberately pass-through, so on CI the smoke test proves the layer loads, builds a real effect and does not crash; the depth path needs a GPU and is not covered there.
