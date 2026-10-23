# Live2D (Cubism) sample

Shows a Live2D model in a PrismatiX game, using both lower extension layers
([docs/native-extensions.md](../../docs/native-extensions.md)):

- **Layer 2, a native library** (`native/`) runs Cubism Core, which only
  native code can. It is a thin bridge: it loads Core, revives a `.moc3` and
  answers "describe the model" once and "update it" every frame.
- **Layer 1, raw meshes** (`extension/`) does everything else in TypeScript:
  parses `model3.json`, plays `motion3.json` and `exp3.json`, blinks and
  breathes, batches the drawables into a few meshes and masks them.

```text
Story  [live2d.show id=haru model="Assets/Live2D/Haru/Haru.model3.json"]
  │ commands edit the saved scene state (ctx.state)
  ▼
frame.update ── Live2DScene.update ──► parameters (motion, expression, blink, breath, held)
                      │                       │ native.request("live2d.model.update")
                      │                       ▼
                      │               libpx_live2d ──► Cubism Core
                      │                       │ finished 8-float vertices of what changed
                      ▼                       ▼
                 ModelRenderer ──► raw meshes (runs of drawables, hidden union meshes for masks)
```

Everything a save needs is in the scene state; meshes, Core handles and motion
clocks are rebuilt from it every frame, so load and rollback need no special
cases. Nothing here ran in a real Player yet: see "What has been tested".

## Layout

| Path | What |
| --- | --- |
| `extension/` | The extension: manifest, `main.ts`, `src/*.ts`, tests |
| `native/` | The C bridge (`cubism_bridge.c`), its plugin glue (`px_live2d_plugin.c`), `cubism_smoke`, and a mock Core + plugin harness for tests (`native/test/`) |
| `tools/moc-to-text.mjs` | Turns a `.moc3` into base64 text a script can read |
| `tools/test.sh` | Builds everything and runs the tests (POSIX) |
| `DEVELOPMENT_NOTES.md` | What was awkward while building this, for the engine's maintainers |

## Try it

1. **Get Cubism Core.** Live2D's license does not let this repository ship it.
   Download the Cubism SDK for Native from Live2D and take the shared library
   (`Live2DCubismCore.dll`, `libLive2DCubismCore.dylib`; Linux may only come as
   a static library, see `DEVELOPMENT_NOTES.md` item 15). Read their license
   before you publish a game with it.
2. **Check your Core against the bridge.** The bridge declares the Core
   functions itself (`native/cubism_core_api.h`), written from the public
   API and not checked against `Live2DCubismCore.h`. Build and run the smoke
   tool on a real `.moc3` first:

   ```sh
   cmake -S native -B build -DPX_ENGINE_INCLUDE=<repo>/PrismatiXEngine
   cmake --build build
   build/cubism_smoke /path/to/Live2DCubismCore.dll Haru.moc3
   ```

   It prints the model's description and one update, or fails. If it crashes
   or prints nonsense, fix the prototype in `cubism_core_api.h` before going on.
3. **Install.** The `cmake --build` above also wrote the plugin into
   `extension/native/<platform>/`. Copy `extension/` to
   `Content/Extensions/live2d-cubism/` in your project and register it:

   ```json
   "extensions": ["Content/Extensions/live2d-cubism/live2d-cubism.pxextension"]
   ```

   Builds sign the library with your project's development key
   (`.prismatix/native-development-key.pem`); see the native-extensions guide
   for release signing.
4. **Add a model.** Put the model's folder in the project, run
   `node tools/moc-to-text.mjs <folder>` to write `<name>.moc3.b64` beside the
   `.moc3`, and make the folder a resource root the Player packs (the PNGs, the
   `.model3.json`, the `.motion3.json`/`.exp3.json` files and the `.b64`).
5. **Point at Core and show the model:**

   ```pxstory
   [live2d.core path="C:/Live2D/Live2DCubismCore.dll"]
   [live2d.show id=haru model="Assets/Live2D/Haru/Haru.model3.json" x=0.5 y=1 height=0.95]
   [live2d.motion id=haru group=TapBody]
   [live2d.expression id=haru name=f01]
   [live2d.param id=haru param=ParamMouthOpenY value=0.8]
   [live2d.param id=haru param=ParamMouthOpenY release=true]
   [live2d.move id=haru x=0.3]
   [live2d.hide id=haru]
   ```

   Positions are fractions of the screen: `x`, `y` is where the model's feet
   stand (0.5, 1 is bottom centre), `height` is the canvas height.

Commands are listed in the manifest (`extension/live2d-cubism.pxextension`)
and show up in Studio's command picker like any extension command.

## What it does and does not do

Does: `.moc3` models of any version the Core you load supports; motions
(parameter and part-opacity curves, Bezier/linear/stepped, fades, loops, an
idle group); expressions (add, multiply, overwrite); breathing; blinking from
the `EyeBlink` group; held parameters (lip sync is a `live2d.param` per frame
from your own script, or fixed values); several models at once; save, load
and rollback.

Does not: physics (`physics3.json`, hair and clothes sway), pose switching,
motion user-data events, hit areas, the mouse-follow helper, inverted masks,
masks that span more than one texture page (the first page wins, with a
warning), and Cubism 5's extra blend modes. It also does not run on the Web,
Android or iOS: native extensions do not load there (`DEVELOPMENT_NOTES.md`
item 11).

Limits to know: a Player frame allows 64 raw uploads and the sample spends 60
of them; a model that needs more runs in one frame finishes over several
frames. Vertices per frame are capped by the Player at 65,536 across all
meshes, which bounds how many high-detail models fit on screen.

## What has been tested

`tools/test.sh` (POSIX; Node 22.18 or newer) builds the bridge, the plugin, a
mock Core and a small harness that mimics `PxHostApi`, then runs 31 tests:

- the wire format, `model3`, motion and expression parsing and evaluation,
  blink and the parameter bank, as plain unit tests;
- the renderer against a fake raw host that enforces the Player's rules
  (vertices before indices, 64 uploads a frame);
- the whole script side against the **real plugin library** and the mock
  Core: models appear with the right transform, UVs, blend modes and masks,
  parameters and motions move vertices, render-order changes restack, hide and
  reshow work, bad Cores and bad `.moc3` files are reported once.

`prismatix validate` accepts the extension in a freshly created project.

**Not tested:** the real Player, the real Cubism Core, any real model, any
platform but Linux. The mock Core has two drawables' worth of behaviour, not
a face. Expect the first run against a real model to turn up differences
(`DEVELOPMENT_NOTES.md` items 14 and 15 list the assumptions most likely to
be wrong).

## Licensing

This sample is MIT like the engine. It contains no Live2D code or data: the
Core prototypes in `native/cubism_core_api.h` are written out here as
declarations, and the mock Core is original. Cubism Core, the Cubism SDK and
any Live2D model are licensed by Live2D Inc. and their model authors on their
own terms.
