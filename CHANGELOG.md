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

### Changed

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
  entry. `"layout": "bundled"` keeps only the executable at the top of a
  Windows distribution, with the runtime and game data in an engine folder
  that Windows loads as a private assembly; `"flat"` stays the default.
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
