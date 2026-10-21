# Web export (experimental)

`prismatix pack --target web` turns a game into a static website that runs the
C++ Player in the browser through WebAssembly. The Web Player shares Runtime IR,
TSX screens, QuickJS extensions, progression, encrypted PDX archives, save
migrations, galleries and replay with the native Player. Browser file, video,
rendering, persistence and accessibility adapters belong to the separate
`PRISMATIX_WEB_PLAYER` composition; Windows, Linux, macOS and Android keep
their own implementations and their own SDL (3.4.10). Studio's `wasm-preview`
is a separate target and cannot be selected as a shipping Player.

Web export is **experimental** in 0.1.0, with either renderer. Test the export
in the browsers you intend to support before you publish it.

## Export a game

The `@prismatix/cli` package carries the Web Player for its own version (both
renderers, in `runtime/web`), so a project needs nothing else:

```bash
pnpm exec prismatix pack --target web                        # WebGL2, into dist/web
pnpm exec prismatix pack --target web --web-renderer webgpu --output dist/web-webgpu
```

Serve the folder from any static host that sends the headers in
[Hosting and updates](#hosting-and-updates); from an engine checkout,
`node scripts/serve-web-export.mjs dist/web 8080` does that locally. Open
`http://127.0.0.1:8080/` and press Start to enable audio.

| Option | Meaning |
| --- | --- |
| `--web-renderer webgl2` (default) | SDL's WebGL 2 renderer plus the WebGL 2 effects layer. Runs wherever WebGL 2 does. |
| `--web-renderer webgpu` | SDL's GPU renderer on WebGPU: the same drawing and effects path as the desktop Player. Needs a WebGPU browser; see [WebGPU status](#webgpu-status). |
| `--web-player <dir>` | Use another Web Player package (for example one you built) instead of the CLI's. It must have the CLI's version. |
| `--web-package <distribution>` | Reuse an existing packaged game and its save lineage instead of packing first. |
| `--ffmpeg <executable>` | The encoder for movie renditions (default `ffmpeg` on `PATH`). |

An export contains only the chosen renderer. To offer both, export twice into
two folders and link the WebGL2 build for browsers without WebGPU; each
WebGPU export tells a browser without WebGPU to use another browser.

Without `--web-package`, the normal Packager first produces a temporary native
distribution; the Web exporter then uses that exact manifest and archive
format. Normal `--artifact`, encryption, sharding and compression options apply
to this first step. The distribution and original media are read-only. The
exporter creates an owned output directory and publishes it atomically; it
refuses to replace arbitrary directories or overlap protected inputs. Generated
game data belongs outside Git; keep project sources and release lineage in
version control.

Movies need FFmpeg with libx264/AAC on the exporting machine. Custom effects
(node, stage and transition shaders) are converted from their SPIR-V at export
time with SPIRV-Cross and Naga 27.0.0: put `spirv-cross` and `naga` on `PATH`,
or point `PRISMATIX_SPIRV_CROSS` and `PRISMATIX_NAGA` at them. The CLI's Web
Player does not carry these host tools (they are per-platform executables); a
Web Player you build yourself includes them in `tools/`. Games without custom
effects need neither.

## Renderers

Both renderers run the same Player, saves, input, audio, accessibility and
offline support. They differ only in how a frame reaches the canvas.

| | WebGL2 | WebGPU |
| --- | --- | --- |
| Status | Experimental | Experimental |
| SDL | 3.4.10, the engine's pin | The unmerged WebGPU backend from [libsdl-org/SDL#16020](https://github.com/libsdl-org/SDL/pull/16020), pinned to one commit |
| Drawing | SDL's WebGL 2 renderer | SDL's GPU renderer on one WebGPU device, shared with the compositor |
| Built-in and custom effects | WebGL 2 versions of the same shaders (`effects.mjs`) | The desktop `Compositor2D` path on SDL_GPU, with WGSL shaders |
| Movies | Decoded by the browser, uploaded as a WebGL texture | Decoded by the browser, copied through the CPU (about half the frame rate) |
| Browsers | Any current browser with WebGL 2 | Only browsers with WebGPU; only Chrome and Edge were tested |
| Download (wasm, brotli) | about 3.7 MB | about 50 KB more |

Effects on the WebGL2 renderer always run on WebGL 2. An older default used
WebGPU for effects whenever the browser had it, which copied every effect pass
between the WebGL canvas and WebGPU; on an effect-heavy scene that cost about
42 ms per frame against 3.8 ms on WebGL 2. That path remains only for
diagnosis, with `?webgpu-effects` in the page address.

### WebGPU status

- **SDL has no official WebGPU backend yet.** The WebGPU renderer is built
  from `TheeStickmahn/SDL_wgpu` at commit `09026288bffe2bf9d7451255b766256a34bd8963`,
  the source of the open pull request libsdl-org/SDL#16020 (SDL 3.5.0-dev),
  with Dawn's emdawnwebgpu bindings `v20260423.175430` (the version Emscripten
  6.0.5 pins; the build refuses another). The pull request's own checklist
  lists missing thread safety, sparse error handling and a WGSL parser that
  does not accept all valid WGSL. Nothing outside the WebGPU Web Player uses
  this SDL.
- **Browsers.** A browser without WebGPU cannot run a WebGPU export; export
  the WebGL2 renderer for those. Only Chrome and Edge on Windows were tested
  (with a hardware GPU). Firefox, Safari and mobile browsers are untested.
- **Video** plays at roughly half to two thirds of the WebGL2 frame rate: the
  backend exposes no WebGPU texture to upload a decoded frame into, so each
  frame is copied through a 2D canvas and `SDL_UpdateTexture` (about 19 ms per
  1080p frame). A 1080p30 clip on a 75 Hz display ran at 44 to 53 fps, against
  75 fps on WebGL2.
- **Size.** The WebGPU runtime is about 256 KB larger as raw wasm (+1.5%) and
  about 50 KB larger with brotli.
- **Suspension.** Both renderers suspend the C++ stack with Asyncify (frame
  waits, remote reads; on WebGPU also adapter/device requests, fences and
  swapchain acquires). JSPI was measured in a prototype: it works with wasm
  exceptions and wasm setjmp/longjmp and makes the wasm 38% smaller, but it
  would require rebuilding every dependency that way and Safari has no JSPI,
  so 0.1.0 keeps Asyncify for both renderers.
- **Measured** (Chrome, RTX 4070 SUPER, 75 Hz): both renderers hold the
  display rate on an effect scene; WebGPU does the frame's work in about
  1 ms against 3.8 ms on WebGL2. With vsync off, WebGL2 has more headroom
  (Asyncify turns the backend's waits into `setTimeout`).
- **Not exercised:** device loss, high-DPI and fullscreen toggling on WebGPU,
  custom node and transition effects on WebGPU, low-end and mobile GPUs.
- **Next:** switch to SDL's official WebGPU backend once it is merged, add a
  zero-copy video upload when the backend exposes its textures, and evaluate
  JSPI for both renderers.

## Browser behavior

- PDX and media use SHA-256-addressed one-MiB chunks. Reads are on demand, with
  existing engine preloads and a bounded browser memory cache. Large games do
  not require loading a complete archive or movie into WASM memory.
- Every exported movie has an AVC/AAC MP4 rendition. The browser decodes video
  and the engine draws it in the same canvas. Volume, skip, pause and lifecycle
  controls use the Player's video API. Original assets and story references
  remain intact. Encodings are reused from `.prismatix/web-video-cache`.
- Saves retain native encryption and game-data identity. Successful atomic
  writes wait for IndexedDB. Save export, import and accepted updates request
  a native-frame persistence flush before reloading; live imports are applied
  before the next Player boot. Settings and progress flush when a tab becomes
  hidden. A browser's forced termination cannot await a final write.
- The complete-download button verifies and caches all game chunks. It reports
  success only after completion; unavailable storage, quota failures or failed
  downloads leave it incomplete. Browsers may evict storage, so exported save
  backups remain useful. The download includes original packaged videos as well
  as their MP4 renditions; size is the manifest's complete logical inventory.
- The canvas accepts keyboard, mouse and SDL touch input. Mobile detection
  selects the engine's mobile UI and texture budget. Web options expose
  fullscreen. ARIA controls mirror native semantic actions and editable text;
  speech uses the browser's available voices for the game's locale.

## Hosting and updates

Upload the entire export to one HTTPS origin. Localhost is allowed for testing;
opening `index.html` with `file:` is insufficient. Serve `.wasm` as
`application/wasm` and `.mjs` as JavaScript. Apply the generated `_headers`
directives, or their equivalent in your own server/CDN: COOP `same-origin`,
COEP `require-corp`, and `no-cache` for the shell, manifest and service worker.
Only `_data/` is immutable. The host need not implement byte ranges: the service
worker assembles verified chunks into seekable video responses, including
offline responses. A working Service Worker is required for movie playback.

Publish updates as a complete, consistent export. Upload new immutable chunks
first, then promote the shell/manifest/service worker together; retain older
chunks while earlier versions are still in use. A running game keeps its
installed shell and content version. The update button flushes saves and
activates the waiting version when the player chooses to reload. Native save
identity and migration rules still apply to the newly packaged content.

`Notices/` contains original runtime dependency licenses, including SDL's and,
for WebGPU exports, Dawn's. `web-player.json` names the renderer, its SDL
source and the SHA-256 of every runtime file. The export includes
`RuntimeSource.tar.gz` and the exact `FriBidiSource.tar.gz` alongside them, with
links in Web options. Runtime source archives omit game assets and ignored
local files.

## Build the Web Player

Only needed to change the engine or to test a local build; the CLI already
carries one. `scripts/build-wasm-player.mjs` runs on Windows, Linux and macOS.
Install Node.js 24+, CMake, Ninja, the Emscripten SDK **6.0.5** (the CI pin;
the script refuses another version), Rust/Cargo, and a native vcpkg install
that provides shadercross and SPIRV-Cross (a native build of the engine, see
[shipping](shipping.md), or `vcpkg install sdl3-shadercross` with this
repository's `vcpkg-configuration.json`). Activate Emscripten first
(`source emsdk_env.sh`, or `. emsdk_env.ps1` in PowerShell):

```bash
pnpm install
pnpm run build
cargo install naga-cli --version 27.0.0 --locked --root out/tools/naga
node scripts/build-wasm-player.mjs
```

It builds both renderers (CMake presets `wasm-player` and
`wasm-player-webgpu`; `--renderer webgl2|webgpu` builds one) and writes the
package to `out/package/wasm-player`:

| Path | Contents |
| --- | --- |
| `web-player.json` | Schema revision 2: version (equal to the CLI's), each renderer's SDL source and suspension mode, and the SHA-256 of every file below |
| `webgl2/`, `webgpu/` | `prismatix-player.mjs`, `prismatix-player.wasm` and the renderer's own notices (SDL; Dawn for WebGPU) |
| `web-shaders.json`, `Notices/`, `RuntimeSource.tar.gz`, `FriBidiSource.tar.gz` | Shared by both renderers |
| `tools/`, `ToolNotices/` | Naga and SPIRV-Cross for custom effects; omitted with `--no-host-tools` (CI and the npm package) |

Export validates every hash and copies only the chosen renderer's files.
`--package-root`, `--build-dir-webgl2`, `--build-dir-webgpu`,
`--native-build-root`, `--vcpkg-installed`, `--triplet`, `--shadercross`,
`--spirv-cross` and `--naga` override locations; `--skip-build` packages
existing builds. On Windows keep build directories short (for example
`C:/b/wgl`): FetchContent's stamp paths exceed `MAX_PATH` otherwise.

The WebGPU renderer embeds its built-in compositor and rule shaders from
`PrismatiXEngine/Shaders/Builtin/webgpu/*.wgsl`: the Naga WGSL of the native
SPIR-V, rebound for SDL_GPU's WebGPU backend (texture *i* at binding 2*i*, its
sampler at 2*i*+1). The script regenerates them and fails if the committed
files differ; `--update-builtin-wgsl` rewrites them. Custom effects are rebound
the same way at export.

`emcmake cmake --preset wasm-player -DPRISMATIX_WEB_DIAGNOSTICS=ON` enables
assertions and function names for diagnosis. Turn the option off and rebuild
before packaging a production runtime. No Preview or native build flags are
changed by this option or by `PRISMATIX_WEB_RENDERER`.

## Verification

```bash
node scripts/test-web-files.mjs
node scripts/test-web-player-browser.mjs dist/web
```

The browser harness uses installed Chrome/Chromium or Edge, and stores logs,
screenshots and results next to the export in `<export>-qa`. It reads the
export's renderer and, for WebGPU, checks that the Player created SDL's GPU
renderer. Its optional journeys use environment variables set to `1`:

| Variable | Journey |
| --- | --- |
| `PRISMATIX_WEB_TEST_STORY` | Title screen, start, first Story line, quick save and quick load (labels default to the Starter sample's; `PRISMATIX_WEB_TEST_FIRST_LINE` checks the line) |
| `PRISMATIX_WEB_TEST_SAVES` | Save export and import round trip |
| `PRISMATIX_WEB_TEST_EFFECTS` | Effect pixel checks (WebGL2 export; expects the effects fixture) |
| `PRISMATIX_WEB_TEST_VIDEO` | Engine movie playback (expects the video fixture) |
| `PRISMATIX_WEB_TEST_UPDATE`, `PRISMATIX_WEB_TEST_OFFLINE` | Update approval; complete download and offline reload |
| `PRISMATIX_WEB_TEST_PLATFORM` | Viewport layout, fullscreen and, with the video fixture, tab visibility and media suspension |
| `PRISMATIX_WEB_MOBILE` | Android metrics and touch emulation in Chromium (not a substitute for a phone) |

`PRISMATIX_WEB_HARDWARE=1` uses the machine's GPU; otherwise Chrome draws with
SwiftShader. `PRISMATIX_WEB_REQUIRE_WEBGPU=1` requires the opt-in WebGPU effect
pixel checks to pass.

CI builds both renderers on Linux with the pinned Emscripten, exports
`Samples/Starter` with each and runs the Story and save journeys in headless
Chrome. GitHub's runners have no GPU, so the WebGPU run uses Dawn's SwiftShader
fallback adapter (Chrome offers it only with `--disable-gpu`) and is
best-effort: it is reported but does not fail the build. On Windows that
software path creates the GPU renderer and then loses its WebGPU instance
before the title screen; with a hardware GPU the same export passes. Firefox, Safari
and physical Android/iPhone validation still require those browsers and
devices.

Web export preserves the current author's content and screens. A game ported
from another engine remains a port in progress: this export does not establish
parity with every route or visual detail in the original game.
