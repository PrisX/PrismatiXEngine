# Low-level and native extensions

An extension normally talks to the engine through the high-level facade,
`createPrismatiXContext()` from `@prismatix/runtime`, inside the Player's
QuickJS sandbox. Two lower layers sit beside it for work the facade cannot
express:

| Layer | Entry | Runs where | Capabilities | Platforms |
|---|---|---|---|---|
| 1. Raw drawing (experimental) | `@prismatix/runtime/raw` | the sandbox | `raw.mesh`, `raw.texture`, `raw.target` | every Player: desktop, WebGL2 and WebGPU Web Players, WASM Preview |
| 2. Native services | `@prismatix/runtime/native` + a signed C library | the game process | `native` | Windows x64, Linux x64, macOS (x64, Apple silicon); not the Web or Android |

- [Layer 1: raw drawing](#layer-1-raw-drawing)
- [Layer 2: native services](#layer-2-native-services)
- [The C ABI](#the-c-abi)
- [Signing](#signing)
- [Packaging, installing and the registry](#packaging-installing-and-the-registry)
- [Build targets](#build-targets)
- [Error codes](#error-codes)

## Layer 1: raw drawing

```ts
import {createPrismatiXRaw, quadVertices, QUAD_INDICES} from "@prismatix/runtime/raw";

const raw = createPrismatiXRaw();               // wraps the EngineRaw global
const noise = raw.createTexture("fx.noise", 64, 64, {filter: "nearest"});
const glow = raw.createTarget("fx.glow", 256, 256, {clear: {r: 0, g: 0, b: 0, a: 0}});

raw.createMesh("fx.blob", {target: glow.id, texture: noise.source, blend: "additive"})
  .vertices(quadVertices({x: 0, y: 0, w: 256, h: 256}));
const shown = raw.createMesh("fx.shown", {texture: glow.source, blend: "premultiplied", phase: "after-stage"});
shown.vertices(quadVertices({x: 400, y: 120, w: 256, h: 256}));
shown.indices(QUAD_INDICES);

const pixels = new Uint8Array(64 * 64 * 4);
raw.onFrame(() => {
  // ...fill pixels...
  noise.upload(pixels);                          // one upload per frame
});
```

It is a separate entry so the facade stays small and stable; this module is
experimental and may change in a minor release. The same calls run on every
Player because they reduce to SDL geometry, streaming textures and texture
render targets, which SDL implements on Direct3D, Metal, Vulkan and OpenGL,
on the WebGL2 Web Player and on SDL_GPU's WebGPU backend.

**Meshes** (`raw.mesh`). `mesh.vertices` takes a `Float32Array` of
interleaved vertices, 8 floats each: `x, y, u, v, r, g, b, a` (positions in
logical pixels, or target pixels inside a render target; colors 0-1); with
`firstVertex` it overwrites that range of the existing vertices.
`mesh.indices` takes a `Uint16Array` or `Uint32Array` triangle list; without
indices the vertices themselves are a triangle list. Indices are checked
against the vertices the mesh has when they arrive (`PXJS7802` past the
end), so on separate calls send the vertices first; `mesh.geometry(vertices,
indices)` sends both in one call.

**Many meshes per frame.** Every upload call counts against the per-frame
budget once, so a model whose 100-300 parts all move each frame (a
Live2D-style rig) sends them in one batch:

```ts
raw.upload(parts.map((part) => ({mesh: part.id, vertices: part.vertices})));
// A part may also carry firstVertex or indices; each mesh at most once.
```

A batch is checked whole before anything changes: one bad part throws and
leaves every mesh as it was. It counts as one upload; its bytes still count
toward the 16 MiB. Options:

| Option | |
|---|---|
| `phase` | `before-stage`, `after-stage` (default), `before-ui`, `after-ui` |
| `order` | lower draws first in its phase or target |
| `visible`, `route` | as retained render nodes |
| `texture` | `{kind: "asset", path}`, `{kind: "texture", id}`, `{kind: "target", id}`; null: vertex colors only |
| `blend` | `normal`, `premultiplied` (render target contents), `additive`, `multiply`, `replace` |
| `mask` | a mesh id, or a list of up to 16, whose coverage (alpha) masks this one; a list unites their coverage; screen meshes only |
| `maskInverted` | `true` shows the mesh where the masks are not (default `false`) |
| `target` | draw into this render target instead of the screen |
| `transform` | `{a, b, c, d, tx, ty}` applied to the positions |
| `effect` | `{id, parameters?, progress?, seed?}`: draw this screen mesh through a packaged node `.pxeffect` (below); null removes it |

**Writable textures** (`raw.texture`): RGBA8, rows top to bottom; `upload`
replaces the whole texture or a `{x, y, w, h}` region with exactly
`w * h * 4` bytes. **Render targets** (`raw.target`): every frame, before the
screen, each target is cleared (`clear`, or kept with `null`) and its meshes
draw into it, targets in `order`. A target's contents are premultiplied, so
show it with `blend: "premultiplied"`; a mesh cannot sample the target it
draws into.

A mask mesh draws by itself unless it is `visible: false`; an invisible
mesh still masks. Masks are drawn source-over whatever their own `blend`, so
an additive or multiply mesh still masks by its alpha.

**Effects.** A screen mesh can be drawn through a packaged `.pxeffect`
whose `targetLayer` is `node`, the shaders `ctx.stage.nodeEffect` applies to
Stage nodes ([custom render effects](ui-reference.md#custom-render-effects)).
Draw a whole model into a render target, then show it through an outline,
glow, dissolve or distortion shader on the GPU:

```ts
const model = raw.createTarget("live2d.model", 1024, 1024, {clear: {r: 0, g: 0, b: 0, a: 0}});
// ...the model's part meshes draw into it with target: model.id...
const outline = {id: "character-outline", parameters: {width: 2, color: [1, 0.85, 0.4, 1]}} as const;
const shown = raw.createMesh("live2d.shown", {texture: model.source, blend: "premultiplied", effect: outline});
shown.geometry(quadVertices({x: 448, y: 40, w: 1024, h: 1024}), QUAD_INDICES);

let pulse = 0;
raw.onFrame((delta) => {
  pulse = (pulse + delta) % 1;
  shown.update({effect: {...outline, progress: pulse}});  // every frame, never journaled
});
```

`parameters` are named uniform values in the shapes `nodeEffect` takes (a
number, `[x, y]`, a color as `[r, g, b, a]` in 0-1); undeclared names, the
wrong number of components and values outside the declared range throw
`TypeError` `PXJS7803` when the option is set, as do an unknown id and an
effect that is not a `node` effect. `progress` is clamped to 0-1 (default
1); `seed` is the shader's `randomSeed` (default: derived from the mesh and
effect ids). `update({effect})` replaces the whole effect. Effects apply to
screen meshes only: `effect` with `target` throws `PXJS7803`.

How the shader sees the mesh:

- A **textured** mesh is drawn with the shader in place, the way a Stage leaf
  is: the shader samples the mesh's texture at the mesh's UVs and the mesh's
  `blend` composites what it returns. A render target holds premultiplied
  colors, so show it with `blend: "premultiplied"`; a shader that is linear
  in color (tints, channel mixes, outlines built from samples) then keeps the
  alpha once.
- A mesh with **vertex colors only** has no texture to sample. It is drawn
  into a transparent screen-size layer, as a Stage group is, the shader
  samples that layer at screen UVs, and the layer composites as
  premultiplied colors. Its `blend` applies inside the layer.
- `viewport` is the mesh's on-screen bounds in logical pixels and
  `texelSize` one logical pixel of the screen, as for Stage nodes; on a quad
  whose UVs span its texture, `1 / viewport.zw` is one screen pixel in UVs.
- With a `mask`, the effect draws the mesh and the mask then cuts the result.
  A mask mesh's effect does not change the coverage it masks with.

The game ships the effect: declare its `.pxeffect` in the project's
`effects` with `graphicsTier: "gpu-effects"`, and the Packager compiles it
with the game's other effects. Effects are loaded with the package, before
any extension runs. A `.pxext` package cannot carry `.pxeffect` or HLSL
files, so an extension that needs an effect says so in its README and the
game adds it. A game without the GPU-effects tier (`graphicsTier: "basic"`,
which cannot declare effects) has no effect list: the option is checked for
shape only and the mesh draws without its effect. That fallback, an effect that fails
when drawn, and meshes past `maxEffectsPerFrame` (32) all draw the mesh
plain and count in `stats().skippedEffects`; a mesh is never skipped because
of its effect.

**Where the math goes.** QuickJS interprets: transforming tens of thousands
of vertices in script every frame is slow. Move a whole mesh with
`transform` (no vertex upload at all), and leave per-vertex deformation to a
native service (layer 2) that returns finished vertices for `raw.upload`.

**Presentation only.** Raw resources are not saved, not recorded in the
operation journal and not rolled back. A load or rollback keeps them as they
are, so rebuild what matters from saved state (`ctx.state`), for example in a
`save.loaded` listener. `create*` on an existing id replaces it, so a handler
that is replayed after a load can create its meshes again harmlessly.

**Budgets**, per extension (`raw.limits()`; `raw.stats()` counts the frame):

| | |
|---|---|
| vertices / indices drawn per frame | 65,536 / 196,608 |
| uploads per frame | 64 calls (a batch is one) and 16 MiB |
| per mesh | 65,536 vertices, 196,608 indices |
| resources | 512 meshes, 64 textures, 8 targets, 4096 px per side, 64 MiB of textures and targets |
| effects | 32 meshes drawn through an effect per frame (the rest draw plain) |

An upload over budget throws `RangeError` `PXJS7801`. A mesh that would draw
past the vertex budget is skipped for that frame and counted in
`stats().skippedMeshes`. A mesh whose mask fails is skipped, never drawn
unmasked.

Every `EngineRaw` call checks the manifest: without the capability it throws
`PermissionError` `PXJS7101`, and builds warn first (`PXCLIEXT2004`, also for
`createPrismatiXRaw()` members). `limits`, `stats`, `clear` and `onFrame`
need none.

## Layer 2: native services

A native extension adds a signed C library per platform. The library
registers **named services**; the script sends a service bytes and gets bytes
back, and can receive messages the library posts. No function pointer ever
reaches JavaScript, so another platform (later, the Web) can provide the same
service name differently without the script changing.

```json
{
  "capabilities": ["runtime", "native"],
  "native": {
    "abi": 1,
    "services": ["steam.achievements"],
    "libraries": {
      "win32-x64": {"path": "native/win32-x64/steamfx.dll", "files": [{"path": "native/win32-x64/steam_api64.dll"}]},
      "darwin-arm64": {"path": "native/darwin-arm64/libsteamfx.dylib"}
    }
  }
}
```

`platforms` (optional) lists where an extension runs: `win32-x64`,
`linux-x64`, `darwin-x64`, `darwin-arm64`, `android-arm64`, `web`. Without it
a JavaScript extension runs everywhere and a native one runs where it has a
library. Pack fills `publicKey` and each library's `sha256` and `signature`.

```ts
import {createPrismatiXNative} from "@prismatix/runtime/native";

const native = createPrismatiXNative();
if (native.has("steam.achievements"))
  native.requestJson("steam.achievements", {unlock: "chapter-1"});
native.onMessage("steam.achievements", (bytes) => ctx.log.info(`steam: ${bytes.length} bytes`));
```

- A script may call only the services its own manifest lists.
- `request` runs synchronously on the game thread; requests and responses are
  at most 16 MiB. Inside a command or Action it is journaled like an engine
  operation, request and response bytes included: a load or rollback replays
  the recorded response instead of calling the library again, and a save
  made mid-command carries the journal. Calls that only feed presentation
  (a model's vertices each frame) belong in `frame.update` listeners
  (`raw.onFrame`, `ctx.frame.onUpdate`), which are not journaled; keep
  journaled requests to the small ones that change game state.
- The Player loads the library when the manifest is loaded, before the entry
  runs. A missing, unsigned or altered library, or one built for another
  ABI, is never loaded: its services are unavailable (`has()` is false,
  `request()` throws `PXJS7901`) and the game goes on.
- In a packed game the libraries are loose files in
  `<data root>/Plugins/<extension id>/` (beside `Package/`); the Player
  checks every file against the SHA-256 the packaged manifest pins (and on
  Windows holds the files open, write-locked, until the library is loaded),
  then the signature, and only then loads it.
  `PRISMATIX_NATIVE_PLUGIN_PATH` (a path list) adds folders to search first.

## The C ABI

`PrismatiXEngine/Engine/Native/PxPluginAbi.h` is the whole contract, plain C:

```c
#define PX_PLUGIN_BUILD 1
#include "PxPluginAbi.h"

static const PxHostApi* host;

static PxStatus Unlock(void* user, const uint8_t* request, size_t size, PxResponse* response) {
    /* ...parse the request... */
    return host->respond(response, (const uint8_t*)"{}", 2);
}

PX_PLUGIN_EXPORT uint32_t px_plugin_abi_version(void) { return PX_PLUGIN_ABI_VERSION; }

PX_PLUGIN_EXPORT PxStatus px_plugin_init(const PxHostApi* api) {
    if (api->abi_version >> 16 != PX_PLUGIN_ABI_MAJOR) return PX_ERROR_ABI;
    host = api;
    return api->register_service(api->host, "steam.achievements", Unlock, NULL);
}

PX_PLUGIN_EXPORT void px_plugin_shutdown(void) {}
```

- **ABI check, both ways.** The manifest's `native.abi` must be the Player's
  major version (`1`). The library's `px_plugin_abi_version()` must have the
  same major version and a minor version no newer than the Player's.
  `PxHostApi` carries the Player's `abi_version` and `struct_size`; later minor
  versions only append fields.
- `register_service` works only inside `px_plugin_init` and only for names in
  `native.services` (`PX_ERROR_NOT_DECLARED`).
- `respond` copies the response, at most once per call. A handler that
  returns anything but `PX_OK` makes the request throw `PXJS7902`.
- `respond_error(response, message)` (ABI 1.1) fails the request with a
  UTF-8 message, cut to 4 KiB: the script's call throws `PXJS7902` with
  "The native service 'name' failed: message", whatever the handler returns.
  Call it instead of `respond`, not after.
- `plugin_directory` (ABI 1.1) is the absolute folder holding the library,
  without a trailing separator: load a companion library (`files` in the
  manifest) from there at run time. Libraries the loader resolves on its own
  also come from there on Windows (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`); on
  Linux and macOS link with an `$ORIGIN` / `@loader_path` run path.
- ABI 1.1 only appended `respond_error` and `plugin_directory`: a library
  built against 1.0 loads unchanged, and one built against 1.1 needs a 1.1
  Player (the minor-version check above).
- `post_message` is the only thread-safe function: it queues up to 256
  messages per service, delivered on the game thread from the next frame.
- `log` writes to the Player log; `px_plugin_shutdown` runs before the library
  is unloaded at shutdown or reload.

## Signing

Each platform's library is signed with ECDSA P-256 over SHA-256 of a
descriptor that binds it to the extension (raw `r||s`, hex):

```
PrismatiX native plugin v1
id <manifest id>
version <version>
platform <platform>
abi <abi>
services <services, sorted, comma-separated>
file <sha256> <path>          (the library, then its files by path)
```

`nativeSignatureMessage(manifest, platform)` in `@prismatix/authoring-sdk`
builds it and `verifyNativeSignature` checks it (Web Crypto); the Player
rebuilds it from the manifest it loads and checks it with mbedTLS.

```sh
prismatix extension keygen ~/keys/owen-native.pem     # keep it out of the project
prismatix extension pack glow --owner owen --signing-key ~/keys/owen-native.pem
```

The public key goes into the manifest; a key that does not match an existing
`publicKey` is refused. For local builds (`run`, `build`, `pack` of the game)
the CLI signs a project's own native extensions with
`PRISMATIX_EXTENSION_SIGNING_KEY`, or with a development key it keeps in
`.prismatix/native-development-key.pem`; `validate` only checks.

## Packaging, installing and the registry

`extension pack` writes the main package (everything but the libraries; its
manifest pins and signs each platform) and one **native pack** per platform
whose files exist: `<owner>-<name>-<version>-native-<platform>.pxext`, a
canonical `.pxext` ZIP holding exactly that platform's `native/<platform>/`
files (at most 64 MiB). A platform whose library is not built is left out of
the package with warning `PXCLIEXT2006`.

```sh
prismatix extension inspect owen/steamfx                # shows the native warning, services and key
prismatix extension add owen/steamfx --allow-native     # this computer's platform
prismatix extension add owen/steamfx --allow-native --platform win32-x64,darwin-arm64
prismatix extension update owen/steamfx --platform linux-x64   # fetch another platform's pack
```

- Installing a native extension needs `--allow-native` (the summary warns that
  the code runs outside the sandbox with the player's permissions). An update
  signed by the key the lock accepted needs no new consent; a different key
  asks again.
- `--platform` picks the native packs (default: this computer's). Each pack
  is checked against the hashes the main package's manifest pins before
  anything is written; it installs into `Extensions/<owner>/<name>/native/`.
- The lock records each installed pack's SHA-256 and size under `platforms`
  and the signing key's SHA-256 as `nativeKey`; `verify` re-hashes the
  installed files, libraries included.
- File and URL sources look for the native packs beside the main package;
  an npm package holds them beside its `.pxext`.

Registry API v1 additions (`Contracts/extension-registry.schema.json`):

- `platforms` on search results, extension documents, version summaries and
  version documents (absent: every platform), and `native: true` on search
  results and extension documents.
- `GET /v1/search?platform=<platform>` keeps extensions that run there;
  `GET /v1/extensions/{owner}/{name}/resolve?platform=` resolves the newest
  version that runs there (`platform_unsupported` otherwise).
  `resolveExtensionVersion(versions, range, engine, platform)` is the rule.
- A version document has `platformDownloads`: `{"<platform>": {download,
  sha256, size}}`; the client checks size and SHA-256 like the main download.
- Publishing announces `platformPackages` (`{sha256, size}` per platform);
  the response's `platformUploads` gives each one's upload URL, and the
  confirmed version must list them in `platformDownloads`.
- The static index builder (`scripts/extension-index.mjs`) takes each native
  pack from beside its package (or an entry's `"native": {"<platform>":
  {"url", "sha256"}}`) and writes `platformDownloads`.

## Build targets

- `pack --target web` and `validate --target web` refuse a native extension
  (`PXCLIEXT1101`) and an extension whose `platforms` leave out `web`
  (`PXCLIEXT1103`) before building anything.
- `pack --target android` refuses native extensions (`PXCLIEXT1102`): the
  Android Player does not load native libraries yet.
- A desktop pack copies each native extension's files for the target
  platform into `Plugins/<extension id>/`, and fails (`PXCLIEXT1104`) when a
  library for that platform is missing from the project.
  `validate --target desktop --platform <list>` runs the same checks.

## Error codes

| Code | |
|---|---|
| PXJS7101 | a raw or native call without its capability |
| PXJS7801 | a raw upload over the frame budget |
| PXJS7802 | an unknown raw id, an index past the vertices or a region outside the texture |
| PXJS7803 | invalid raw arguments or options, including an unknown effect or parameters that do not match its uniforms |
| PXJS7804 | a raw resource limit (count, size, texture memory, masks per mesh, batch parts); also `readBytes` past 64 MiB |
| PXJS7901 | a native service that is unavailable or not declared |
| PXJS7902 | a native service that failed (with its `respond_error` message when it gave one) |
| PXJS7903 | (log) a native library that could not be verified or loaded |
| PXEXTNAT1001-1006 | manifest checks: capability, ABI, platforms, paths, unsigned, bad signature |
| PXEXTPKG1015 / 1016 | a native pack with missing or extra files / a file that does not match its pinned SHA-256 |
| PXCLIEXT1101-1105 | build target checks and signing-key mismatch |
| PXCLIEXT2006 | a platform's library is not built, so the package leaves it out |
