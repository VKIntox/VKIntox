# test fixtures

Third-party shader files used by `src/tests/smoke_vkcube.sh`, which boots the
built layer under `vkcube` with one depth-consuming effect enabled so the run
exercises the depth capture and resolve path rather than the pass-through path.

These are vendored rather than generated. The smoke test previously used a
hand-written effect, which proved the path ran but asked nothing of the real
compile pipeline: a full, community-maintained shader with uniform
preprocessor definitions, a localisation include and a large UI parameter set
is a much better exercise of the same machinery.

| file | origin | licence |
| :--- | :--- | :--- |
| `ReShade.fxh` | ReShade | `CC0-1.0`, stated in its own header |
| `DisplayDepth.fx` | CeeJay.dk, with additions from the ReShade community | **none stated in the file** |
| `DisplayDepth_L10N.fxh` | ReShade community translations | **none stated in the file** |

copied verbatim from a local `EffectPackages.ini` install, so they are exactly
what a user's own shader directory contains.

## the licence gap

`ReShade.fxh` is explicitly CC0. The other two name an author and the
community but carry no licence identifier, and this repository is zlib. That is
an unresolved question rather than a settled one, and it is recorded here so it
is not discovered later by accident.

If it needs closing, the options are to obtain an explicit grant from the
authors, to replace these two with the hand-written effect the smoke test used
before, or to drop the effect-enabled path from the test and test the
pass-through path only. None of those are decisions to make silently in a test
fixture.

## updating them

These are copies, not mirrors. If the upstream shader gains a fix, nothing here
picks it up automatically, and the smoke test will keep testing whichever
version was copied in. Refresh deliberately:

```bash
cp ~/.config/VKIntox/reshade/packages/Shaders/{DisplayDepth.fx,ReShade.fxh,DisplayDepth_L10N.fxh} src/tests/fixtures/
```

then confirm it still compiles and still reports depth usage:

```bash
build/src/vkintox-shader-check --include src/tests/fixtures src/tests/fixtures/DisplayDepth.fx
```

`success: true` and `usesDepth: true` are both required. A fixture that stops
using depth would silently turn the smoke test back into a pass-through run.
