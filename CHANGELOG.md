# Changelog

All notable changes to the published PrismatiX packages (`@prismatix/cli`,
`@prismatix/authoring-sdk`, `@prismatix/runtime`) and the native runtime they
bundle are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the major
version is 0, a minor release (0.x.0) may break authoring APIs and is a new
release line; patch releases within a line stay compatible.

The three packages always carry the same version and pin each other exactly.

## [Unreleased]

## [0.1.0] - 2026-10-21

First public release. See [docs/releases/0.1.0.md](docs/releases/0.1.0.md) for
supported platforms, verification status and known limitations.

### Added

- `@prismatix/cli`: `create` (interactive or scripted, minimal and ADV
  templates, pnpm or npm), `dev` live Preview with edit/error/repair, `validate`,
  `build`, `pack` (desktop; `--target web` with a separately built WASM Player;
  `--target android`, experimental), `release`, `add character`/`add scene`,
  `extension create`, `eject ui`, `import artemis`, `ids --sync`, signed
  `create-patch`/`create-overlay`, and `test` journeys against a QA Player.
  The package bundles the native Player, Packager and Preview Host for
  Windows x64, Linux x64, macOS x64 and macOS arm64 under
  `runtime/<platform>-<arch>/`.
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

[Unreleased]: https://github.com/owenliu0924/PrismatiXEngine/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/owenliu0924/PrismatiXEngine/releases/tag/v0.1.0
