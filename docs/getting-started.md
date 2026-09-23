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

Edit `my-game/Story/main.pxstory` and save. The generated Story has no inline `[id]` directives: `create` puts stable save identities in the adjacent `.pxids.json` sidecar. Keep its `; @prismatix-ids` marker and the sidecar together. `dev` watches before its first build, so fixing initial syntax errors can start Preview without rerunning the command. In one `dev` session, unchanged Story inputs reuse their compiled program when only UI or other non-Story inputs change; Story text, identity sidecars, index, character/game/extension declarations, locale resource aliases, and asset catalog changes invalidate that reuse. This does not yet skip loading, dependency scanning, media copying, artifact fingerprinting, or Preview restart. Each successful edit prepares an immutable revision and replacement Preview, waits for apply/play acknowledgements, then closes the prior Preview. **The current policy restarts the scene after a successful edit.** It does not promise to preserve dialogue position. Errors retain the previous playable revision. Preview starts at Title; Start opens the story HUD. Its Save/Load slots hold session-only checkpoints, cleared on rebuild or Host close. Packaged Player Save/Load uses persistent player saves.

When the bundled Runtime includes `share/PrismatiX/PrismatiXUiTypeRegistry.json`,
`validate`, `dev`, `build`, `pack`, and `run` also verify UI runtime property
names and types before launching the Player. A selected `--runtime` prefix or
native build directory supplies its own registry for these commands; `validate`
accepts `--runtime` too. A fresh `pack`/distribution build also looks next to
its selected Packager (or in that bundle's `share/PrismatiX` directory).
For a separate native SDK, you can still pass
`--ui-registry <path-to-PrismatiXUiTypeRegistry.json>` explicitly.
Selecting `--runtime` without a registry is an error. Without any selected or
bundled registry, canonical UI schema checks still run, but CLI emits
`PXCLI1042` to say native property compatibility was not checked. In a local
CMake build, regenerate an outdated manifest with the
`PrismatiXUiTypeRegistryManifest` target; CLI rejects a manifest older than
its adjacent native exporter.

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

If a post-build tool must add or replace artifact files, use the supported
[`augment` command](artifact-augmentation.md) before `pack --artifact`;
ordinary media should instead be declared with `resourceIncludes`.

For an unencrypted compressed distribution, Packager writes an
`Package/entry-cache.json` inventory. The CLI passes the prior published
distribution as a read-only cache source even though it builds the replacement
in a separate staging directory. Repacking to the same intact output can
reuse matching compressed entry bytes after checking the old bytes and the
current source against SHA-256; a corrupt cache is ignored and rebuilt.
Compression-mode changes and encrypted packages do not reuse this cache.
Packager still verifies every input and writes a complete new `Content.pdx`:
this reduces redundant compression and one full input-read pass, but is not
yet block-level incremental packaging.

Never choose Story, Assets, another source input or an unrelated existing folder as output. `pack` is an explicit distribution command; `run` packages and launches. Quote paths containing spaces.

Output replacement verifies the previous build's inventory. If you have played a distribution and it now contains saves/logs, or edited its files, use a new output directory; do not delete player data to bypass the guard. Test migration with a copy of the old saves before distributing an update.

Long builds report loading, per-scene Story parse/compile counts across locales,
copy counts and bytes, fingerprinting, and publication on stderr. Packaging also forwards throttled native phase/count
updates as they happen. With `--json`, CLI progress is NDJSON on stderr and
native Packager events are streamed on stdout; neither waits for the whole
archive to finish before showing progress.

`create` already prepares an identity sidecar for the starter Story. For an
older or separately authored Story with save/seen history, run
`prismatix ids my-game --write` once to move line-by-line `[id]`
anchors into adjacent `.pxstory.pxids.json` files while keeping the prose
readable. External sidecars are the default; `--inline` is an explicit legacy
mode. The command backs up original Story sources. `prismatix ids my-game
--check` then audits coverage. Do not delete a sidecar: the source marker makes
the compiler reject a missing one rather than silently changing save ids.
After adding or removing executable Story lines, preview the identity update
with `prismatix ids my-game --sync`. The JSON report includes each new
operation's Story path, one-based line and short source preview, plus each
removed anchor's original operation position and identity. Check these against
your edit; if the preserved/added/removed operations match your intent, use
`--sync --write` to update only the sidecars
with a backup. Reorders and ambiguous duplicate lines require manual identity
review. For a rewrite immediately beside an insertion, sync the rewrite first,
then insert and sync again; a combined edit cannot always be safely matched
because the sidecar stores fingerprints, not the old prose. Test existing saves
before shipping a revised story.

The global command delegates project commands to the exact local CLI pinned in the game's package.json. Commit that manifest and its lock. A missing or mismatched local CLI is diagnosed instead of silently using the global version. After preparation, dev/build use local packages and bundled runtime offline. Upgrade tool versions deliberately in a branch, reinstall the pinned dependencies, and test old saves and the packaged Player before distributing an update.

If you intentionally link a local Engine checkout with `file:` dependencies,
run `npm run build` in that checkout after editing CLI or Authoring SDK source.
Local source packages carry a content-based build stamp; an out-of-date `dist`
now fails with `PXSDKBUILD1001` instead of silently running old compiler code.
Registry packages contain no source tree and do not perform this local check.

For media selected by a dynamic runtime path rather than a static Story/UI
reference, add `"resourceIncludes": ["Assets/MyFamily", "Assets/Audio/confirm.ogg"]`
to `prismatix.json`. Directories are expanded during validate/build; the files
are packaged as owned dependencies, while this author-only field is omitted
from the runtime project document. Missing or empty roots fail validation.

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

Continue with the [cookbook](cookbook.md), [layered portrait authoring](portrait-authoring.md), [custom effects](custom-effects.md), [runtime extension contracts](../runtime-sdk/README.md) and [release compatibility](releases/0.2.0.md).
