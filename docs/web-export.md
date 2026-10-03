# Web export

The shipping Web Player runs the C++ Player through WebAssembly. It shares
Runtime IR, TSX screens, QuickJS extensions, progression, encrypted PDX archives,
save migrations, galleries and replay with the native Player. Browser file,
video, rendering, persistence and accessibility adapters belong to the separate
`PRISMATIX_WEB_PLAYER` composition. Windows and Android keep their existing
implementations. Studio's `wasm-preview` is a separate target and cannot be
selected as a shipping Player.

## Build the runtime

The packaging script currently runs on Windows. Install Node.js 24+, CMake,
Ninja, Emscripten (tested with 4.0.12), Rust/Cargo and FFmpeg with libx264/AAC.
The native x64 release build supplies the host SDL shadercross and SPIRV-Cross
tools; see [shipping](shipping.md) for the native build. Activate Emscripten
in the PowerShell session before building:

```powershell
. C:/path/to/emsdk/emsdk_env.ps1
npm install
npm run build --workspace @prismatix/authoring-sdk
npm run cli:build
cargo install naga-cli --version 27.0.0 --locked --root out/tools/naga
./scripts/build-wasm-player.ps1
```

The output is `out/package/wasm-player`. Its `web-player.json` identifies the
shipping variant and SHA-256 hashes of runtime, shaders, notices and source
archives. Export validates these hashes. `-NativeBuildRoot`, `-Shadercross`,
`-SpirvCross`, `-Naga` and `-PackageRoot` override tool/output locations.
`-SkipBuild` packages an already built Player. Custom effect conversion uses
Naga 27.0.0 and SPIRV-Cross; the packaging script copies both host tools into
the runtime package. `PRISMATIX_NAGA` and `PRISMATIX_SPIRV_CROSS` override them
when exporting on another host.

`emcmake cmake --preset wasm-player -DPRISMATIX_WEB_DIAGNOSTICS=ON` enables
assertions and function names for diagnosis. Turn the option off and rebuild
before packaging a production runtime. No Preview or native build flags are
changed by this option.

## Export and preview a game

```powershell
node packages/cli/dist/src/cli.js pack ../my-game --target web `
  --web-player out/package/wasm-player `
  --runtime out/build/x64-release/PrismatiXEngine `
  --archive-shards 4 --compression fast --output ../my-game/dist/web
node scripts/serve-web-export.mjs ../my-game/dist/web 8080
```

Open `http://127.0.0.1:8080/` and press Start to enable audio. With an installed
project-local CLI, use `prismatix pack` instead of the explicit Node entry point.
`--web-package <native-distribution>` reuses an existing packaged game and its
save lineage. The distribution and original media are read-only.
`--ffmpeg <executable>` selects the encoder. Without `--web-package`, the normal
Packager first produces a temporary native distribution; the Web exporter then
uses that exact manifest and archive format. Normal `--artifact`, encryption,
sharding and compression options apply to this first step.

The exporter creates an owned output directory and publishes it atomically.
It refuses to replace arbitrary directories or overlap protected inputs.
Generated game data belongs outside Git; keep project sources and release
lineage in version control.

## Browser behavior

- PDX and media use SHA-256-addressed one-MiB chunks. Reads are on demand, with
  existing engine preloads and a bounded browser memory cache. Large games do
  not require loading a complete archive or movie into WASM memory.
- Every exported movie has an AVC/AAC MP4 rendition. The browser decodes video
  and the engine draws it in the same canvas. Volume, skip, pause and lifecycle
  controls use the Player's video API. Original assets and story references
  remain intact. Encodings are reused from `.prismatix/web-video-cache`.
- SDL's ordinary 2D canvas uses WebGL 2. Effects prefer WebGPU. Built-in post
  effects and custom node, stage and
  transition shaders also have WebGL 2 implementations; export converts and
  validates the packaged SPIR-V ahead of time. Rule masks and particles use
  the engine's existing rendering logic. WebGPU effects currently copy between
  the SDL WebGL canvas and WebGPU targets, so effect-heavy scenes can cost more
  than their native equivalents. Ordinary scenes bypass those copies.
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

`Notices/` contains original runtime dependency licenses. The export includes
`RuntimeSource.tar.gz` and the exact `FriBidiSource.tar.gz` alongside them, with
links in Web options. Host tool notices stay in the runtime package's
`ToolNotices/`; host converters are not downloaded by players. Runtime source
archives omit game assets and ignored local files.

## Verification

```powershell
node scripts/test-web-files.mjs
node scripts/test-web-player-browser.mjs ../my-game/dist/web
```

The browser harness uses installed Chrome/Chromium or Edge, and stores logs,
screenshots and results next to the export in `<export>-qa`. Its optional
journeys use `PRISMATIX_WEB_TEST_EFFECTS`, `PRISMATIX_WEB_TEST_VIDEO`,
`PRISMATIX_WEB_TEST_SAVES`, `PRISMATIX_WEB_TEST_UPDATE` and
`PRISMATIX_WEB_TEST_OFFLINE` set to `1`. Effects/video journeys expect the
corresponding test fixture. `PRISMATIX_WEB_HARDWARE=1` uses the machine's GPU;
`PRISMATIX_WEB_REQUIRE_WEBGPU=1` requires WebGPU pixel checks to pass.
`PRISMATIX_WEB_TEST_PLATFORM=1` checks viewport layout, browser fullscreen and,
with the video fixture, real tab visibility/media suspension.
`PRISMATIX_WEB_MOBILE=1` emulates Android metrics and touch input in Chromium;
it does not substitute for a physical phone test.

Acceptance in this checkout includes native Windows regression, the separate
WASM Preview build, CLI/transport tests, browser effect pixel checks and
actual Player video, save import/export and update journeys. Sakura's complete
export includes all three movies; its opening, settings, save/load, imported
CG gallery, music, replay, full offline download and offline movie seeking have
been exercised in Chrome; Edge also passes the effect/video/save/update/offline
fixture and fullscreen/background/foreground journeys. Firefox, Safari and physical Android/iPhone
validation still require those browsers/devices. Android emulation in Chrome
passes canvas touch activation, viewport/fullscreen, media lifecycle and save
backup/import checks. This environment has no
Android SDK/NDK installation usable for a new Android build; Windows lifecycle,
touch, graphics and persistence regression tests do not replace device testing.

Web export preserves the current author's content and screens. Sakura remains
an in-progress game port: this export does not establish parity with every
route or visual detail in the original game.
