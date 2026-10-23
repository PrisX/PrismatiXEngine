# Development notes: friction found while building this sample

Things that were awkward, surprising or missing in the engine, the SDK or the
workflow while writing the Live2D sample. Each item says what happened, what
the sample did about it, and what a fix in the engine would look like.
Severity: **blocker** (needed a workaround that costs something real),
**friction** (works, but more work than it should be), **doc** (unclear or
unverified, not necessarily a defect).

## Engine / SDK

### 1. Scripts cannot read binary resources: friction
`ctx.assets.readText(path)` is the only resource reader. A `.moc3` is binary.
*Sample:* `tools/moc-to-text.mjs` stores it as base64 text, `src/base64.ts` decodes
it (+33 % size, a build step the model author must remember).
*Fix idea:* `ctx.assets.readBytes(path): Uint8Array` (capability-gated, size cap).

### 2. A native handler can only fail with a bare status: friction
`PX_ERROR_FAILED` becomes `PXJS7902` without the library's message, so "Core
not found" and "not a valid moc3" look the same to the script.
*Sample:* every response starts with a u32 status; status 1 carries a message
(`px_live2d_plugin.c`, `protocol.ts`). Every native extension will reinvent this.
*Fix idea:* `host->respond_error(response, message)` whose text reaches the
script's error and the Player log.

### 3. `PxHostApi` gives a plugin no paths: friction
There is no data root or plugin folder in `PxHostApi`, so a plugin cannot find
a companion library (here Live2D's proprietary Core) next to itself. `dlopen`
with a bare name only searches the system paths; the Player's `Plugins/<id>/`
is not on them.
*Sample:* the script passes a path, or the user puts Core on the library path.
*Fix idea:* `host->plugin_directory` / `host->data_directory` (UTF-8).

### 4. Raw upload budget (64 calls per frame) is below a model's drawable count: friction
A Cubism model has 100-300 drawables; one mesh per drawable would be 100+
uploads per frame against a limit of 64.
*Sample:* drawables in render order are merged into "runs" that share texture,
blend mode and mask, one mesh and one upload per run (`renderer.ts`). That is
real extra code (batching, re-batching when render order changes) every
mesh-heavy extension will need.
*Fix idea:* a multi-mesh upload call, or counting bytes only, or a batching
helper in `@prismatix/runtime/raw`.

Related, and not in the docs: `UploadIndices` counts as an upload too, and an
index at or past the mesh's current vertex count throws `PXJS7802`, so a mesh
must get its vertices **before** its indices. Setting up one mesh costs two
uploads, so 31 meshes exhaust the frame. The sample uploads vertices first and
spreads a big (re)layout over several frames (`UploadBudget` in `renderer.ts`).
I found this by reading `ScriptRawGraphics.cpp` after the first draft threw
in the fake host; the guide should state both rules.

### 5. Masks: one mask mesh per mesh, none inside render targets, no inversion: friction
Cubism clips a drawable with several masks (union) and some are inverted.
*Sample:* masks with the same texture are merged into one hidden "union" mask
mesh; masks on different texture pages keep only the first page (warns);
inverted masks are drawn unmasked (warns).
*Fix idea:* `mask: string | string[]` with union semantics, `maskInverted`.

### 6. Is an invisible mask mesh still a mask?: doc
`docs/native-extensions.md` says the mask mesh "still draws by itself unless it
is not `visible`", which implies `visible: false` still masks. In
`ScriptRawGraphics.cpp` the mask is drawn by `Draw(mask->second)` outside the
visibility filter, which agrees, but I could not run the Player to confirm.
The sample relies on it. *Fix idea:* say so explicitly and add a test.

### 7. `RawMeshOptions.transform` exists but I did not use it: doc
Placing the model is done natively (`ax, ay, tx, ty` in the update request), so
moving a model resends every vertex. The mesh `transform` would do it with no
re-upload. I only noticed after the protocol was written; worth reworking.

### 8. Per-vertex work is too slow for the sandbox to do itself: friction
Cubism needs ~20k vertices transformed per frame; in QuickJS that is a loop
the sample avoids by having the native side emit finished 8-float vertices.
That is a design the sandbox forces on every mesh-heavy extension.
*Fix idea:* none needed if the native layer is the intended home for it, but say
so in `docs/native-extensions.md`.

### 9. Journaling of `native.request` inside commands: friction
`native.request` inside a command or Action handler is journaled with its
whole response (hex). A model description is hundreds of KB, so creating a
model in a handler would bloat the journal and every save.
*Sample:* handlers only edit saved state (`ctx.state`); a `frame.update`
listener (not journaled) does the native calls and rebuilds resources.
*Fix idea:* a `transient: true` service flag in the manifest, or an explicit
`requestTransient()`.

### 10. The sandbox lacks TextEncoder, TextDecoder, atob, Date, Math.random: friction
The sample re-implements UTF-8 and base64 (`protocol.ts`, `base64.ts`), and a
seeded LCG for blink timing. `@prismatix/runtime/native` already has
`encodeUtf8`/`decodeUtf8`, but a sample that is not in the pnpm workspace
cannot import it in plain Node tests.
*Fix idea:* export a tiny `@prismatix/runtime/text` (utf8, base64) usable in Node
and in the sandbox.

### 11. Platform coverage: blocker for mobile
Native extensions do not load on Android (`PXCLIEXT1102`) or the Web, and
iOS is not mentioned. Live2D is mostly used on mobile, so as built this
sample cannot ship on the platforms Live2D is most used on.
*Fix idea:* Android `.so` loading (the ABI already fits), iOS static linking of
a plugin into the Player.

## Workflow

### 12. The Player cannot be built here: friction
SDL3, FFmpeg and vcpkg make the Player a heavy build, so nothing in this
sample has run inside a real Player. What is tested: the C bridge against a
mock Core, the plugin through a small harness that mimics `PxHostApi`, and the
script side against a fake raw host. What is **not** tested: the real Player,
real Cubism Core, any real model. The first run on a machine with all three is
the real test.
*Fix idea:* a headless `PrismatiXPlayerQA` build artifact in CI that sandboxes
can download.

### 13. Samples are outside the pnpm workspace: friction
`Samples/*` is not a workspace package, so TypeScript in a sample cannot
resolve `@prismatix/runtime` without a `paths` mapping (see
`tsconfig.check.json`). The Starter avoids it because it has no scripts.
*Fix idea:* a `Samples/*` workspace entry, or `prismatix extension create`
output that works in place.

### 14. Core's header was not reachable: environment
`cubism.live2d.com` and `docs.live2d.com` are blocked by this session's network
policy, so the Core function prototypes in `native/cubism_core_api.h` were
written from memory of the public API, not checked against `Live2DCubismCore.h`.
Because Core is loaded with `dlopen`, a wrong prototype compiles and then
misbehaves. `native/cubism_smoke.c` exists to catch that on a machine with the
real Core. Constants (`PX_CSM_*` flag values, the 64/16 byte alignments) carry
the same caveat. *Do this first when you have the SDK.*

### 15. Cubism details I could not verify: doc
- Whether `csmReadCanvasInfo`'s origin is measured from the top-left or
  bottom-left of the canvas (the sample assumes top-left, y up in model space).
- Whether a mask drawable's own opacity should count in the mask (the sample
  ignores it).
- Linux: I believe the SDK ships Core as a static library on Linux, in which
  case there is no `.so` to `dlopen` and you must wrap it yourself.

### 16. No official test double for `EngineRaw` / `EngineNative`: friction
To test the script side without a Player I wrote `FakeRaw` (which re-states
the Player's rules: vertex-before-index, 64 uploads a frame, index range) and
a C harness that mimics `PxHostApi` and serves requests over FIFOs
(`extension/test/support.ts`, `native/test/plugin_harness.c`). Both can drift
from the Player, and every native-extension author needs the same pair.
*Fix idea:* ship them (`@prismatix/runtime/testing`, a `px_plugin_harness`
tool in the engine), and run the engine's own checks against them so they
cannot drift.

### 17. `prismatix extension types` is not `extensionTypeDeclarations()`: doc
Writing `prismatix-extension.d.ts` by calling the SDK's
`extensionTypeDeclarations(manifest)` produced a file the CLI then flagged as
stale (`PXCLIEXT2003`); the file `extension types` writes passed. I did not
find out how they differ. The sample commits the CLI's output.

### 18. `prismatix create` exits with an error after creating the project: friction
Without a Player installed, `create` writes the project, then fails with
`PXCLI1501 RuntimeDiscoveryFailure` and a non-zero exit. The project is
usable (I validated the extension in it), but a script cannot tell success
from failure. *Fix idea:* make the runtime pin a warning, or a distinct exit code.

### 19. The sample provenance gate hashes every file: friction
`pnpm samples:check` wants `SAMPLE_PROVENANCE.json` to list every file under
`Samples/<name>` with its SHA-256, so each edit to a sample needs
`node scripts/check-sample-provenance.mjs --write Samples/<name>` before the
commit. Source-only samples (no art) pay this for little. Build outputs
(`extension/native/*`) are gitignored so they do not enter the inventory.
The generator also writes the Starter's origin text ("Written by `prismatix
create`...") for every text file, so a sample of another origin has to
correct the `origin` fields by hand after each `--write`.
*Fix idea:* a `--check` that only asserts the licence of each file and hashes
only binary assets, or an `ignore` list in the inventory.

### 20. Node 22 and TypeScript 7 details for sample tests: friction
Running the sample's TypeScript tests with plain `node --test` works because
Node strips types, but only for erasable syntax: constructor parameter
properties (`constructor(private x: T)`) and enums fail, so the sample avoids
them and its tsconfig sets `erasableSyntaxOnly`. The repo's TypeScript (7.0)
also no longer accepts `baseUrl`, which most `paths` examples still use.
Relative imports need the `.ts` extension for Node; the CLI's esbuild bundle
accepts them.
