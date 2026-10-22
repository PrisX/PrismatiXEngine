# Changelog

All notable changes to the published PrismatiX packages (`@prismatix/cli`,
`@prismatix/authoring-sdk`, `@prismatix/runtime`, the per-platform
`@prismatix/native-<platform>-<arch>` runtime packages and
`@prismatix/web-player`) are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the major
version is 0, a minor release (0.x.0) may break authoring APIs and is a new
release line; patch releases within a line stay compatible.

The packages always carry the same version and pin each other exactly.

## [Unreleased]

### Added

- Sharing extensions ([docs/extensions-sharing.md](docs/extensions-sharing.md)).
  `prismatix extension pack [--owner <owner>] [--include-source]` writes a
  reproducible `<owner>-<name>-<version>.pxext` (a canonical stored ZIP with
  `pxext.json`, the manifest, the bundled `dist/` script, generated types,
  README and LICENSE), type-checking and checking capabilities and size limits.
  `prismatix extension add <owner/name[@range] | file.pxext | https URL |
  npm:<package>>` resolves the newest version for the project's engine,
  checks the SHA-256, size, archive form, engine range and command/Action id
  conflicts, shows the capabilities and asks (`--yes` in scripts), installs
  into `Extensions/<owner>/<name>/`, registers it and records it in
  `prismatix-extensions.lock`; nothing in a package runs at install time.
  `remove`, `update [id] [--to <range>]`, `outdated`, `verify` (re-hashes the
  installed files against the lock), `inspect`, `search`, `info`, `publish`
  (presigned upload; token from `PRISMATIX_REGISTRY_TOKEN`), `yank` and
  `registry [<url>]`. The registry comes from `--registry`,
  `PRISMATIX_EXTENSION_REGISTRY` or the project's
  `settings["extensions.registry"]`; there is no default. The client also
  works against a static index, which `scripts/extension-index.mjs` builds
  from entry files. `@prismatix/authoring-sdk` exports
  `validateExtensionPackage` (also as the Node-free subpath
  `@prismatix/authoring-sdk/extension-package`), `writeExtensionPackage`,
  `resolveExtensionVersion`, SemVer range helpers and the registry document
  types; new contracts `extension-package`, `extension-lock` and
  `extension-registry`.
- Packing for other platforms: `prismatix pack --platform
  <win32-x64|linux-x64|darwin-x64|darwin-arm64|all>` (a comma list, may
  repeat) compiles the project once and packs it for each platform into
  `dist/<platform>/`, from any computer. `prismatix add platform <platform>`
  downloads that platform's `@prismatix/native-<platform>` at exactly the
  CLI's version from the npm registry, checks it against the registry's
  SHA-512 integrity and keeps it in `.prismatix/platforms/` without running
  anything in it (`--from` installs an `npm pack` tarball or a runtime bundle;
  `--check` lists what the project can pack for). A missing runtime fails
  before anything is built, with the command that adds it. `--archive` writes
  `<name>-<platform>.zip` (Windows, macOS) or `.tar.gz` (Linux) with Unix
  permissions; a macOS or Linux pack made on Windows always gets one, since
  only the archive keeps the executable bit. The Packager takes a
  `targetPlatform` and checks the Player is that platform's executable
  (PXPKG1277, PXPKG1278).
- macOS signing from any computer: off a Mac, `pack` signs the `.app` ad hoc
  with rcodesign (apple-codesign) when it is installed (`PATH` or
  `PRISMATIX_RCODESIGN`; never downloaded), and with `PRISMATIX_MACOS_P12`,
  `PRISMATIX_MACOS_P12_PASSWORD` and `PRISMATIX_MACOS_NOTARY_KEY` it signs
  with a Developer ID and the hardened runtime, notarizes and staples, on any
  computer. An unsigned `.app` is reported (PXCLI1227) with what players
  will see. docs/shipping.md, Packing for other platforms, has the details.
- Extension handlers can be cancelled cleanly. Every command and Action
  handler gets `context.abortSignal` (a command's new second argument is
  `{abortSignal}`), an AbortSignal-like token with `aborted`, `reason`,
  `onabort`, `addEventListener("abort")`, `onAbort` and `throwIfAborted`.
  When an Action is restarted (`reentry: "restart"`), its scene (route) is
  replaced or closed, Skip reaches a command that holds the Story, or a load,
  rollback, Preview restart, extension reload or shutdown replaces the running
  handlers, the signal aborts, the engine wait the handler awaits rejects with
  an `AbortError` (`code` `PXJS7301`, `kind` `restart`, `closed`, `skip`,
  `load`, ...; a generator gets it thrown at its `yield`) and its `finally`
  blocks run before the Player continues. `isAbortError` tests for it.
- Native event listener removal: `Engine.on` returns a listener id and
  `Engine.off(id)` removes it from the Player. The function `ctx.events.on`,
  `once`, `ctx.frame.onUpdate`, `ctx.animation.onMarker` (which now returns
  one) and the `observe*` functions return removes the native listener, and
  `ctx.events.listenerCount(event?)` / `Engine.listenerCount` count the live
  ones.
- `prismatix run` (and so Studio's 試玩) passes the Player's output through
  the CLI and shows extension errors, warnings and log lines at the `.ts`
  file and line, using the build's source maps like `prismatix test`; with
  `--json` each is a `run.extension` record with a `span`.

### Changed

- The Windows executable's icon, version information and manifest are written
  by the Packager itself instead of Windows' resource API, so Linux and macOS
  pack the same Windows build as Windows does. It rebuilds the resource
  section, moves the relocation table behind it, drops a stale Authenticode
  signature and recomputes the PE checksum.
- A Mac refuses to pack a Windows build of a project with custom effects
  (PXPKG1511): Windows Players need DXIL, which the DirectX Shader Compiler
  cannot produce on macOS. Windows and Linux compile SPIR-V, DXIL and MSL for
  every platform.
- The CLI no longer carries every platform's runtime (272 MB packed). Each
  platform's native Player, Packager and Preview Host is its own package,
  `@prismatix/native-win32-x64`, `-linux-x64`, `-darwin-x64` and
  `-darwin-arm64`, which `@prismatix/cli` lists as exact-version optional
  dependencies so pnpm and npm install only the matching one; the CLI names
  the missing package when it was left out (`--no-optional`) or the platform
  has none. The Web Player is the separate `@prismatix/web-player`, installed
  on demand: `pack --target web` without it prints the exact install command,
  and `prismatix add web-player [--check]` installs (or reports) it with the
  project's package manager. `create` does not add it.
- The Linux and macOS runtime binaries are stripped (the Linux Player went
  from 131 MB to 48 MB); the removed symbols are a separate
  `-debug-symbols.tar.gz` asset on the GitHub Release.

- `prismatix create` (without `--empty`): the sample story is new. Two
  characters, 白音 (`shirone`) and 小春 (`koharu`), meet on a street; it
  shows show/hide/move, a background zoom, a choice between two routes and a
  variable that picks the closing line. The template's earlier pictures are
  removed. The street and both portraits are AI-generated images supplied by
  the project owner for use in samples only; replace them before releasing a
  game.
- `Samples/Starter` now has the same two characters on its street, as
  PrismatiX Studio's new projects do. `scripts/check-sample-provenance.mjs`
  accepts the owner's `sample-only` pictures, for their exact bytes only.

### Fixed

- On Windows, packing no longer fails with PXPKG1315 when a virus scanner or
  the search indexer still has the new Player open; the Packager waits up to
  five seconds before giving up.
- A game that cannot start no longer crashes the Player (access violation
  0xC0000005) while it closes. Whatever stops the boot (an extension's
  `engine.ready` handler that throws, a missing UI template, a damaged or
  missing `Content.pdx`, ...), the Player logs `PXPLAYER5031 The game could
  not start` with the cause, releases what it had started and exits with
  code 1. A double-clicked game shows the cause in a message box; launched by
  a tool (output to a pipe or file, `prismatix run`, the QA Player or
  `PRISMATIX_NO_ERROR_DIALOG=1`) it shows none. A runtime that cannot start
  says why in `PXPLAYER5007`. `prismatix test` reports such a journey as
  "The game could not start" with the Player's report (`bootFailure` in
  `--json`), extension lines at their `.ts` file and line.

## [0.1.0] - 2026-10-21

First public release. See [docs/releases/0.1.0.md](docs/releases/0.1.0.md) for
supported platforms, verification status and known limitations.

### Added

- `@prismatix/cli`: `create` (interactive or scripted, minimal and ADV
  templates, pnpm or npm), `dev` live Preview with edit/error/repair, `validate`,
  `build`, `pack` (desktop; `--target web`, experimental, with
  `--web-renderer webgl2|webgpu`; `--target android`, experimental), `release`,
  `add character`/`add scene`,
  `extension create`, `eject ui`, `import artemis`, `ids --sync`, signed
  `create-patch`/`create-overlay`, and `test` journeys against a QA Player.
  The native Player, Packager and Preview Host for Windows x64, Linux x64,
  macOS x64 and macOS arm64 come from `@prismatix/native-<platform>-<arch>`
  (an optional dependency per platform), and the experimental Web Player
  (WebGL2 and WebGPU renderers, one version with the CLI) from
  `@prismatix/web-player`, which a project adds when it exports for the web.
- Web Player (experimental): the C++ Player in WebAssembly with a WebGL2
  renderer (SDL 3.4.10; effects on WebGL 2) or a WebGPU renderer (SDL's GPU
  renderer and the desktop compositor on one WebGPU device, built from the
  unmerged SDL WebGPU backend of libsdl-org/SDL#16020 pinned to one commit).
  `scripts/build-wasm-player.mjs` builds both on Windows, Linux and macOS;
  CI builds them on Linux and plays the Starter sample in headless Chrome.
- Desktop distributions are branded and laid out per platform
  (`prismatix.json` `distribution`): `icon` takes a square PNG as well as an
  `.ico` and becomes a 16-256 px Windows icon, a macOS `.icns` and Linux icon
  theme sizes; `productName`, `companyName`, `copyright` and `version` become
  the Windows version information (Explorer's Details tab) and the macOS
  `Info.plist`. macOS packs a `.app` bundle (`distribution.macos.bundleIdentifier`,
  ad-hoc signed when packed on a Mac); Linux ships `lib/` and a `.desktop`
  entry. A Windows distribution keeps only the executable at the top level,
  with the runtime and game data in an `Engine` folder that Windows loads as a
  private assembly (`"layout": "bundled"`, the default); `"layout": "flat"`
  keeps everything beside the executable.
- The shipped Windows Player is a GUI program, so a game opens no console
  window. Started from a terminal it prints to that console, and `--console`
  opens one; the log file is unchanged. `PrismatiXPlayerQA` stays a console
  program.
- `@prismatix/authoring-sdk`: `.pxstory` parsing with source spans and
  recovery, project compilation to Runtime IR and source maps, stable save
  identities, validation of every canonical document (revision 1), a React-free
  JSX runtime for TSX screens, layered portraits, motion and input actions.
- `@prismatix/runtime`: typed API for sandboxed JavaScript/TypeScript runtime
  extensions (commands, Actions, events, saved state, logging), with handler
  types generated from the extension manifest.
- Native runtime: SDL3 Player with GPU stage, effects and transitions, text
  layout with bidi and line breaking, backlog, save/load with migrations,
  galleries, replay, accessibility adapters, PDX5 archives (optional
  encryption, sharding, WebP/Opus transcoding) and QuickJS-NG extensions.
- VS Code extension source (`packages/vscode-pxstory`, not published to the
  Marketplace in this release).
- Repository sample `Samples/Starter`, the project PrismatiX Studio creates
  by default, with a provenance inventory checked before every release (not
  part of the npm packages).

### Changed

- The release line is renumbered to 0.1.0. Development builds called
  themselves 0.2.0, but nothing was published under that number. Extension
  manifests must declare a range that includes 0.1.0, for example
  `"requiredEngineVersion": ">=0.1.0 <0.2.0"`; `prismatix extension create`
  writes that range. Document formats and wire contracts are unchanged
  (revision 1, PDX5).
- `prismatix create` uses pnpm when it was not launched by a package manager
  and pnpm is installed (previously npm). `--package-manager npm` keeps npm.
- The CLI loads the TypeScript compiler only when a command needs it, so
  commands that do not build TSX screens or TypeScript extensions start
  faster; project commands load the CLI twice (global, then the project's
  pinned copy) and gain twice.

### Fixed

- Preview sampling of performance timelines looked up every track for every
  node; dense timelines (2,048 tracks on 256 nodes) sample about 25 times
  faster.

[Unreleased]: https://github.com/PrisX/PrismatiXEngine/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/PrisX/PrismatiXEngine/releases/tag/v0.1.0
