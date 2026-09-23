# Build your first PrismatiX game

Install Node.js and npm first. CI is configured for Node.js 24.14.0; this candidate's local Windows journey was verified with Node.js 24.2.0. Other versions have not been revalidated for this candidate. Node.js is an author prerequisite, not installed by the commands below. The platform CLI package must contain the matching native runtime; authors do not need C++, CMake, vcpkg, an engine checkout or a separate runtime download.

**Release status:** the commands below describe the intended published-package interface. Current evidence is from an isolated candidate registry, not public npm publication. Check the [release audit](releases/0.2.0-audit.md) for actual platform results.

## Three commands

```bash
npm install -g @prismatix/cli
prismatix create my-game
prismatix dev my-game
```

`create` prepares a short Traditional Chinese story with original character/background pictures, two choices and endings, a title, default dialogue HUD and save/load routes. It copies Noto Sans TC and its license, prepares editor settings, installs exact project CLI/SDK versions and writes `package-lock.json`. No npm init, second SDK install, hand-written UUIDs or initialization command is needed. Initial preparation needs registry access; failures preserve existing author data.

All options are non-interactive:

```bash
prismatix create "my game" --name "First Light" --id first-light --locale zh-TW --width 1280 --height 720
```

`--locale` sets the locale tag; it does not translate the Chinese template. The bundled font covers the starter text. Add and test appropriate fonts before authoring other writing systems.

Edit `my-game/Story/main.pxstory` and save. Keep `[id main.welcome]` when rewriting that line. `dev` watches before its first build, so fixing initial syntax errors can start Preview without rerunning the command. Each successful edit prepares an immutable revision and replacement Preview, waits for apply/play acknowledgements, then closes the prior Preview. **The current policy restarts the scene after a successful edit.** It does not promise to preserve dialogue position. Errors retain the previous playable revision. Preview starts at Title; Start opens the story HUD. Its Save/Load slots hold session-only checkpoints, cleared on rebuild or Host close. Packaged Player Save/Load uses persistent player saves.

When the bundled Runtime includes `share/PrismatiX/PrismatiXUiTypeRegistry.json`,
`validate`, `dev`, `build`, `pack`, and `run` also verify UI runtime property
names and types before launching the Player. For a separate native SDK, pass
`--ui-registry <path-to-PrismatiXUiTypeRegistry.json>` to these commands.
Without a registry, canonical UI schema checks still run, but native property
type compatibility cannot be established until the Player loads the scene.

Press Ctrl+C to close a watch session. Superseded and rejected revisions are retired only after their Preview process confirms closure. The last successfully acknowledged revision remains in `.prismatix/dev/` for diagnosis and is ignored by Git. A process that cannot confirm closure keeps its resources.

## Daily work and distribution

```bash
prismatix dev my-game
prismatix validate my-game
prismatix build my-game
prismatix build my-game --output "release/First Light"
```

`build` creates a desktop distribution with Player, game content and redistributable dependencies in `my-game/dist/<project-id>/`. Players need neither Node.js nor the SDK. `--output` changes only location. Automation that previously used `build --output` for compiler artifacts must add `--artifact-only`:

```bash
prismatix build my-game --artifact-only --output my-game/.prismatix/artifact
```

To distribute an artifact that has already been built, pass it to `pack`,
`build`, or `run` with `--artifact`. This skips author-source compilation and
the intermediate artifact copy; the native Packager still checks every input's
size and SHA-256 before accepting it. The artifact must be an intact CLI-owned
build directory with its `.prismatix-output.json` marker. Changes to Story or
UI source are **not** included until you build or otherwise update the artifact.

```bash
prismatix pack my-game --artifact my-game/.prismatix/artifact
```

Never choose Story, Assets, another source input or an unrelated existing folder as output. `pack` is an explicit distribution command; `run` packages and launches. Quote paths containing spaces.

Output replacement verifies the previous build's inventory. If you have played a distribution and it now contains saves/logs, or edited its files, use a new output directory; do not delete player data to bypass the guard. Test migration with a copy of the old saves before distributing an update.

Long builds report loading, compilation, copy counts and bytes, fingerprinting,
and publication on stderr. Packaging also forwards throttled native phase/count
updates as they happen. With `--json`, CLI progress is NDJSON on stderr and
native Packager events are streamed on stdout; neither waits for the whole
archive to finish before showing progress.

The global command delegates project commands to the exact local CLI pinned in the game's package.json. Commit that manifest and its lock. A missing or mismatched local CLI is diagnosed instead of silently using the global version. After preparation, dev/build use local packages and bundled runtime offline. Upgrade tool versions deliberately in a branch, reinstall the pinned dependencies, and test old saves and the packaged Player before distributing an update.

## Diagnostics and troubleshooting

The terminal shows readable diagnostics by default. Pass `--json` for stable NDJSON codes, paths and source spans on stderr; preserve stderr for editor integrations. `PXSTORY1199` identifies an unknown Story command. Unknown options and invalid typed values fail. `dev --once` tests initial apply/play and shutdown, not an interactive editing session.

Normal discovery uses the matching `runtime/<platform>-<architecture>/` bundle inside the CLI package. A missing or incompatible bundle is an installation/release problem. Do not install C++ build tools as an author workaround. Explicit `--runtime`, per-tool options and `PRISMATIX_*` overrides are developer diagnostics. Manifest validation cannot attest to the compiler ABI of an arbitrary executable supplied as an override.

For registry or proxy errors during create, fix npm's registry/proxy configuration and retry. Do not use sudo or disable security controls. A nonempty or read-only destination is rejected. After abrupt termination, inspect a leftover create lock before removing its empty lock directory and retrying.

## Verify a runtime bundle release

Native build instructions are in the [contributor section](../README.md#engine-contributors). Release engineers run:

```bash
node scripts/phase1-clean-install.mjs --mock --keep
node scripts/phase1-clean-install.mjs --runtime /path/to/installed-runtime --keep
```

Here `--runtime` supplies binaries for the candidate npm package; it is never passed to the simulated author CLI. The harness serves candidate packages through a loopback registry, installs into a temporary global prefix, creates the project, watches edit/error/repair, closes the registry, reopens offline, builds and launches Player. Failed workspaces are retained; `--keep` also retains success evidence. Mock proves orchestration only. Native acknowledgements and launch do not prove visual interaction, audio or glyph correctness.

Windows x64, Linux x64 and macOS x64/arm64 are intended desktop targets. Each exact platform/architecture needs its own candidate evidence. WASM Preview is separate from full Web game distribution; mobile, console and full Web shipping are outside this desktop release.

Continue with the [cookbook](cookbook.md), [custom effects](custom-effects.md), [runtime extension contracts](../runtime-sdk/README.md) and [release compatibility](releases/0.2.0.md).
