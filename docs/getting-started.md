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

Edit `my-game/Story/main.pxstory` and save. The generated Story has no inline `[id]` directives: `create` puts stable save identities in the adjacent `.pxids.json` sidecar. Keep its `; @prismatix-ids` marker and the sidecar together. `dev` watches before its first build, so fixing initial syntax errors can start Preview without rerunning the command. In one `dev` session, unchanged Story inputs reuse their compiled program when only UI or other non-Story inputs change; Story text, identity sidecars, index, character/game/extension declarations, locale resource aliases, and asset catalog changes invalidate that reuse. On later revisions, `dev` content-checks media against the prior owned revision and hard-links unchanged files where supported, falling back to source copies. It still loads and scans dependencies, verifies source content and published ownership, fingerprints the new artifact, and restarts Preview. Each successful edit prepares an immutable revision and replacement Preview, waits for apply/play acknowledgements, then closes the prior Preview. **The current policy restarts the scene after a successful edit.** It does not promise to preserve dialogue position. Errors retain the previous playable revision. Preview starts at Title; Start opens the story HUD. Its Save/Load slots hold session-only checkpoints, cleared on rebuild or Host close. Packaged Player Save/Load uses persistent player saves.

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

Rebuilding the same CLI-owned artifact compares the compiled files and media
sources by SHA-256 with its existing ownership inventory. If every file is
unchanged, the CLI verifies the existing artifact under its publication lock
and keeps that directory without staging or replacing it. Otherwise, it
hard-links unchanged media into the new staging revision when both directories
are on the same volume and copies changed media. A damaged or unowned old
artifact is never silently accepted or overwritten. This avoids a second
multi-GiB media copy and reduces peak disk space, but still reads authored
inputs and the existing artifact for integrity; it is not a block-level
incremental build. When linked files come from that same output directory, the
mandatory pre-publish integrity check also validates their content without
hashing those staged hard links again.

To distribute an artifact that has already been built, pass it to `pack`,
`build`, or `run` with `--artifact`. This skips author-source compilation and
the intermediate artifact copy; the native Packager still checks every input's
size and SHA-256 before accepting it. The artifact must be an intact CLI-owned
build directory with its `.prismatix-output.json` marker. Changes to Story or
UI source are **not** included until you build or otherwise update the artifact.

```bash
prismatix pack my-game --artifact my-game/.prismatix/artifact
```

For multi-GiB developer builds, use an optimized Release or RelWithDebInfo
Packager even when testing with a Debug Player. The native Packager performs
large JSON validation and repeated cryptographic reads; a Debug binary can
make those operations much slower. `--packager` and `--player` may point to
different developer build configurations. The opt-in developer checkout
fallback prefers Release when available. Explicit paths are honored, and the
CLI warns when a Debug Packager is selected for an artifact of at least
1 GiB. Build both configurations from the same Engine revision before
mixing them.

If a post-build tool must add or replace artifact files, use the supported
[`augment` command](artifact-augmentation.md) before `pack --artifact`;
ordinary media should instead be declared with `resourceIncludes`.

Before starting native packaging, the CLI checks free space on the output
volume against the artifact's indexed input sizes. With `--compression none`,
it stops early if the inputs already exceed the available space. Compressed
packages only get an advisory warning because the final archive size cannot
be known in advance. The check includes a small allowance for the Player and
archive metadata, but is an estimate, not a guarantee of a successful pack.
Use `--output` to select a volume with more space; `--ignore-disk-space` skips
this preflight if you have independently verified the package will fit.

For an unencrypted compressed distribution, Packager writes an
`Package/entry-cache.json` inventory. The CLI passes the prior published
distribution as a read-only cache source even though it builds the replacement
in a separate staging directory. Repacking to the same intact output can
reuse matching compressed entry bytes after checking the old bytes and the
current source against SHA-256; a corrupt cache is ignored and rebuilt.
Compression-mode changes and encrypted packages do not reuse this
entry-level cache. `Package/archive-shards.json` separately records each
archive's source-content and output SHA-256 fingerprints. When an archive is
unchanged, Packager checks the prior bytes and rechecks every current source
before reusing the whole PDX in a staged publication. The CLI retires the old
distribution after successful replacement; a caller that keeps an external
cache source does not hard-link its archives by default. With one archive,
any content change still rewrites `Content.pdx`.

Native input validation checks paths, sizes, and declared SHA-256 syntax up
front. For recognized image, audio, and streaming-video extensions, the raw
SHA-256 read is deferred to the archive writer or whole-shard reuse check;
these paths hash the current source bytes before publication. Story, UI,
project, font, and other inputs used by structural validation are still
content-checked before parsing. The ambiguous `.ts` extension stays on this
eager path because it may contain a TypeScript module rather than video. A
media mismatch therefore may be reported during archive writing or reuse
rather than during the initial validation phase. This removes one media read,
not the integrity checks or the time needed to verify
old PDX shards and publish a distribution.

For large games, `pack`, `run`, and distribution `build` accept
`--archive-shards N` (1–64). The default `1` keeps `Content.pdx`; larger
values assign each resource to a stable URI-hash shard and emit
`Content-00.pdx`, `Content-01.pdx`, etc. The manifest lists only nonempty
shards, and Player mounts all listed files. Each compressed shard has its own
`Package/entry-cache-XX.json`. Keeping the same shard count preserves a
resource's shard when other resources are added; changing the count
repartitions the package and its cache. In an unencrypted package, unchanged
shards can be reused without rewriting their PDX. Encrypted packages also
reuse shards when the entire content identity is unchanged, but changing any
content rotates the current package-derived archive key and rebuilds all
encrypted shards. Every source is still verified and the CLI still fingerprints
the published output, so this
is not a claim of constant-time or measured large-project speedup. This is
split packaging, not automatic patch delivery: placing an extra `patch.pdx`
beside the game does not mount or apply it.

For `pack --artifact` to the same output, the CLI also records
`Package/cli-package-provenance.json`. If the package request, artifact
inventory, Packager/Player binaries, adjacent runtime libraries, and release
notice files are unchanged, it verifies the existing distribution and hashes
every artifact input under the publication lock before keeping the output
without running Packager again. A changed option, source, tool, or output
falls back to normal packaging or fails the relevant integrity check. Older
distributions need one full pack to gain this record; script-based developer
Packager overrides do not use this shortcut because their imported code is
not bounded by the native runtime inventory. This path still reads the full
artifact and output, so it is not constant-time.

When an artifact does change, the CLI first checks the existing distribution's
owned names under its publication lock, then lets native Packager validate the
artifact and decide which PDX shards can be hard-linked from the old output.
For reported reused shards, the CLI reuses their recorded output hashes only
after checking file identity; it still hashes the complete old output before
promotion and rechecks the links. Modified or unowned old output is never
replaced. Native input, contract, and shard-content checks still run, so this
reduces redundant publication I/O but does not make a changed pack fast.

Each packaged distribution now includes `Package/content-index.json`, a
per-resource URI, SHA-256, source-size, and owning-archive inventory. To
compare two complete distributions, run:

```sh
prismatix plan-patch --base path/to/old-distribution --target path/to/new-distribution --json
```

This read-only plan reports added, modified, and removed resources, changed
raw bytes, and target archives containing additions or modifications. It
does not create a PDX, install an update, or establish trust in a package.
Use `prismatix verify-package path/to/distribution` to read and SHA-256-check
every resource in its PDX archives against the index, or add
`--verify-contents` to `plan-patch` to check both distributions before
comparing them. Native Packager also accepts `--verify-package` with an
absolute distribution path. These checks detect missing, corrupt, and
misindexed resources but are not cryptographic signatures: someone who can
replace the archives and their metadata together can forge a passing local
verification. The base and target must be packages for the same game and both
must include the new index.

For a small update between two complete, CLI-owned distributions, create a
whole-file delta bundle and explicitly apply it:

```sh
prismatix create-patch --base path/to/v1 --target path/to/v2 --output path/to/update-v1-v2
prismatix apply-patch --base path/to/v1 --patch path/to/update-v1-v2 --output path/to/updated-game
```

`create-patch` verifies both PDX contents and carries only changed files in
`Payload/`, plus `Patch/manifest.json` with the exact base and target file
inventories. `apply-patch` verifies the base and patch bundle, reconstructs a
complete target in a staging directory, verifies its PDX contents, and
publishes it after comparing every staged file against the target inventory.
Removed files are omitted from the assembled target. It refuses a wrong base
or modified/unowned output. For explicit replacement of the original game
directory after exiting the game, use:

```sh
prismatix apply-patch --base path/to/v1 --patch path/to/update-v1-v2 --in-place
```

Unchanged files are hard-linked into the staging directory when supported,
with copying as a fallback. A separate
`--output` copies the complete reconstructed distribution, so it needs space
for that full output. In-place replacement also needs room for changed files
and any fallback copies. Do not bypass an ownership refusal by removing saves
or other user files from the game directory.

The bundle is **not** a standalone `patch.pdx`, is not automatically loaded
at startup, and is not signed. It detects accidental corruption and wrong
versions, not a malicious distributor who can replace the bundle and its
metadata together. An automatic update channel still needs authenticated
publisher identity, version/dependency policy, download/resume handling, and
installation coordination. A drop-in PDX overlay additionally needs explicit
override precedence and tombstone semantics; the current installer instead
assembles a complete target distribution before launch. The selected native
Packager must understand both package versions; cross-engine upgrades may
require a full installer rather than this bundle format.

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
