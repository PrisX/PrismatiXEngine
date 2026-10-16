# Authoring guide

Everything about writing a PrismatiX game: the project, Story scripts, characters, TSX UI, input, motion and custom effects.

- [Build your first PrismatiX game](#build-your-first-prismatix-game)
- [Story author cookbook](#story-author-cookbook)
- [Authoring input actions](#authoring-input-actions)
- [Layered portrait authoring](#layered-portrait-authoring)
- [Authoring layered motion](#authoring-layered-motion)
- [Layered motion authoring proposal](#layered-motion-authoring-proposal)
- [Sprite lettering without one control per glyph](#sprite-lettering-without-one-control-per-glyph)
- [Data-driven placed UI items](#data-driven-placed-ui-items)
- [Modal dialogs in TSX UI](#modal-dialogs-in-tsx-ui)
- [Host Action authoring contract](#host-action-authoring-contract)
- [Packaged Player user data](#packaged-player-user-data)
- [Custom render effects](#custom-render-effects)
- [Testing with journeys](#testing-with-journeys)

## Build your first PrismatiX game

Install Node.js and npm first. CI is configured for Node.js 24.14.0; this candidate's local Windows journey was verified with Node.js 24.2.0. Other versions have not been revalidated for this candidate. Node.js is an author prerequisite, not installed by the commands below. The platform CLI package must contain the matching native runtime; authors do not need C++, CMake, vcpkg, an engine checkout or a separate runtime download.

**Release status:** the commands below describe the intended published-package interface. Current evidence is from an isolated candidate registry, not public npm publication.

### Three commands

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

Edit `my-game/Story/main.pxstory` and save. The generated Story has no inline `[id]` directives: `create` puts stable save identities in the adjacent `.pxids.json` sidecar. Keep its `; @prismatix-ids` marker and the sidecar together. `dev` watches before its first build, so fixing initial syntax errors can start Preview without rerunning the command. In one `dev` session, unchanged Story inputs reuse their compiled program when only UI or other non-Story inputs change; Story text, identity sidecars, index, character/game/extension declarations, locale resource aliases, and asset catalog changes invalidate that reuse. On later revisions, `dev` content-checks media against the prior owned revision and hard-links unchanged files where supported, falling back to source copies. It still loads and scans dependencies, verifies source content and published ownership, fingerprints the new artifact, and restarts Preview. Each successful edit prepares an immutable revision and replacement Preview, waits for apply/play acknowledgements, then closes the prior Preview. **The current policy restarts the scene after a successful edit.** It does not promise to preserve dialogue position. To work deep inside a scene, start Preview there and keep it there across edits with `--at`: `prismatix dev my-game --at chapter2:120` (a line), `--at chapter2:after_school` (a label) or `--at Story/zh-TW/chapter2.pxstory:120`. Preview replays the Story up to that point, so backgrounds, characters and variables are as they would be; a position behind a choice is reached by trying the first few choice paths. Errors retain the previous playable revision. Preview starts at Title; Start opens the story HUD. Its Save/Load slots hold session-only checkpoints, cleared on rebuild or Host close. Packaged Player Save/Load uses persistent player saves.

When the bundled Runtime includes `share/PrismatiX/PrismatiXUiTypeRegistry.json`,
`validate`, `dev`, `build`, `pack`, and `run` also verify UI runtime property
names, types, and authored string enum choices before launching the Player.
Numeric `range` metadata is an editor hint, so it is not treated as a universal
runtime limit. The native wheel binding threshold is an explicit 0–100
contract and is checked independently. A selected `--runtime` prefix or
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

### Editing Story in VS Code

The `vscode-pxstory` extension in `packages/vscode-pxstory` highlights
`.pxstory` files and runs a language server built on the same Authoring SDK
as `validate`: errors appear as you type (with the SDK's codes and spans),
completion offers commands, characters and expressions, `storyAssets` short
names and labels (including `scene:label` in other scenes), and
go-to-definition and hover work on labels, macros, characters and images.
**PrismatiX: Validate Project** runs the project's own CLI, and **PrismatiX:
Preview From Here** starts `prismatix dev` at the cursor's scene and line
(`--at <scene>:<line>`; a CLI without that flag is reported, not ignored).
It is not on the Marketplace yet; build and install the `.vsix` as described
in [its README](../packages/vscode-pxstory/README.md).

### Daily work and distribution

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

For a release candidate, `release` validates, builds, packs and verifies every
archive in one step, then reminds you to commit `.prismatix-release/lineage.json`
so later releases keep loading players' saves. Put the settings every build of
the project should use in `prismatix.json` instead of repeating flags:

```json
"distribution": {"executableName": "first-light", "icon": "Assets/icon.ico",
  "output": "release/First Light", "compression": "fast", "archiveShards": 4}
```

```bash
prismatix release my-game
```

Command-line options still win over these defaults. `"linkMedia": true`
hard-links the project's media into the build artifact instead of copying it
when both are on the same volume, so a multi-GiB game is not stored twice
(the artifact then shares those files with the project).

`"transcode"` makes distributions smaller: `build` and `pack` (not `dev`)
store PNG as WebP and Ogg Vorbis/WAV as Opus or Vorbis under the same paths,
which the Player recognises by content, so Story, UI and saves are unchanged.
Alpha is kept exactly; a file that would grow or fails to decode ships as
authored. Encodings are cached in `.prismatix/transcode-cache`, so only new or
edited media is encoded again, and `--no-transcode` packs the originals:

```json
"transcode": {"images": {"format": "webp", "quality": 90},
  "audio": {"format": "opus", "bitrate": 64},
  "exclude": ["Assets/Music/**"]}
```

`quality` is 1-100 or `"lossless"`, `effort` (0-6, default 4) trades encode
time for size, Opus `bitrate` is in kbit/s (default 96), and Vorbis takes
`quality` from -1 to 10 instead. Engine contributors can
pass a local build directory as `--runtime out/build/x64-release/PrismatiXEngine`;
a CLI linked from an engine checkout finds that checkout's build by itself
(`PRISMATIX_NO_CHECKOUT_RUNTIME=1` turns this off), and `create` run from a
checkout links the new project to it with `file:` dependencies, so it works
offline.
Scaffold the files you would otherwise write by hand, and see any command's
options with `--help`:

```bash
prismatix add character kei --from Assets/Portraits/kei --name 圭
prismatix add scene chapter2 --chapter chapter-1 --title "放學後"
prismatix eject ui hud
prismatix release --help
```

`add character` makes `normal.png` (or `default`/`neutral`) the default
expression and, without `--from`, registers a character with no sprite, such
as a first-person protagonist who still needs a name.

`add scene` lists the scene in a chapter of the story index and ends it with
`[jump scene=next]`. `scene=next` (in `[jump]`, `[call]` and choice options)
is the scene after this one in reading order: the chapters in order, each
chapter's scenes in order, then scenes no chapter lists. After the last
scene, `[jump scene=next]` ends the story; a choice or `[call]` there is
`PXSTORY1309`. Reordering scenes in the index therefore reorders the story
without editing jumps. A scene that runs off its end without a jump or
`[end]` still stops there: the compiler never continues into the next scene
on its own, so one of several route endings in a chapter cannot run into the
next by accident. A scene really named `next` is reached by that name.

`prismatix create my-game --template adv` starts with the HUD, Save, Load,
Settings and Backlog already ejected this way. `eject ui <route>` (or `all`) copies a stock Player screen (`title`, `hud`,
`backlog`, `save`, `load`, `gallery`, `settings`, `video`) into
`Content/UI/<Route>.tsx`, scaled to the project's resolution, and registers it
in `uiEntryPoints`. It lowers to exactly the built-in screen, so you start from
a working HUD or Save screen with every control name and binding the Player
needs, and restyle from there.

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

`build` and `dev` keep one compressed Story scene compilation cache at
`my-game/.prismatix/cache/story-scenes-v1.json.gz`. A new CLI process can reuse
unchanged scenes; editing one scene recompiles it while retaining independent
scenes. Exact Story and sidecar text, shared compiler inputs, and a fingerprint
of the installed compiler determine reuse. A damaged or stale cache is ignored.
The file is rewritten only when successful compilation changes cached scenes,
and is limited to 128 MiB compressed (512 MiB before compression). Deleting it
only makes the next build compile Story again. This does not skip authored text
reads, artifact integrity checks, or the Packager's content verification.

To distribute an artifact that has already been built, pass it to `pack`,
`build`, or `run` with `--artifact`. This skips author-source compilation and
the intermediate artifact copy; the native Packager still checks every input's
size and SHA-256 before accepting it. The artifact must be an intact CLI-owned
build directory with its `.prismatix-output.json` marker. Changes to Story or
UI source are **not** included until you build or otherwise update the artifact.

```bash
prismatix pack my-game --artifact my-game/.prismatix/artifact
```

Every save records the fingerprint of the package that wrote it. When a new
package keeps the same `contentVersion` and `saveVersion` but its content
changed, the Player loads an older save only if the new package lists that
save's fingerprint as compatible. Otherwise it rejects the save with
`PXSAVE6111`. After each successful `pack`, `build`, or `run`, the CLI records
the fingerprint of the package it wrote, and the fingerprints that package
accepts, in `.prismatix-release/lineage.json` in the project. The next pack
passes that record to Packager, whatever `--output` it uses. **Commit this
file.** With it, packing to a new folder, packing on a clean CI machine, or
deleting an old `dist` folder no longer breaks existing saves. The file changes
only when the package fingerprint does.

When a pack finds no lineage file, no previous distribution in `--output`, and
no `--previous-package`, it warns with `PXCLI1222` that saves from an
already-shipped release with the same versions will be rejected. The warning
does not appear when a declared save migration leads into the current version.
For a first release, commit the lineage file that the pack writes.
`PXCLI1223` warns that the version changed since the recorded release but no
`saveMigrations` entry starts from the recorded version. A lineage file that
names another game ID stops the pack. See [save migrations](shipping.md#save-migration-anchors).

To publish a standalone new version in a different directory while retaining
compatible saves from an earlier distribution, name that intact distribution:

```bash
prismatix pack my-game --artifact my-game/.prismatix/artifact --previous-package release/v1 --output release/v2
```

`--previous-package` also works with distribution `build` and `run`. The CLI
verifies the old package, checks its game ID and publisher key, and passes its
save identity and reusable archive data to Packager. The old directory stays
untouched; the new output must be absent or empty. Repacking into the same
output directory already carries this history without the option. Packager
combines the save identity it finds in the previous distribution with the
project's lineage file. Compatible
save identity does not make changed Story state compatible: advance the
content/save version and provide a migration when needed.

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
[`augment` command](shipping.md#augmenting-a-published-artifact) before `pack --artifact`;
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

Use `prismatix pack <project> --artifact <artifact> --check` to run that
native request and Story/UI/resource-contract preflight without writing a
distribution. Without `--artifact`, the CLI compiles a temporary artifact
first. The command reports a distinct `validated` result, never `packed`.
Media fingerprints intentionally deferred by normal Packager are also
deferred by `--check`; a successful preflight is not a verified PDX or a
guarantee that all media bytes will remain unchanged during packaging.
Add `--verify-media` to `pack --check` when you want SHA-256 verification of
every current artifact input without producing PDX. The `validated` event
reports `deferredMediaCount` (zero in this mode). Even full-input preflight
does not replace `verify-package` for a completed distribution.

Full-input verification, including direct artifact-to-overlay planning, hashes
independent files with up to four bounded workers and reports the
`validate-hash` phase. `PRISMATIX_PACKAGE_HASH_WORKERS=1` forces sequential
verification; values 1–8 are accepted for measuring a particular storage
device. Every input still gets its current bytes checked against its declared
SHA-256; this is not a persistent verification cache.

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

If only the Player executable, its copied adjacent libraries, or installed
release notices change, `pack --artifact` can instead refresh those runtime
files without invoking Packager or rewriting PDX shards. This requires a
previous package with version-2 CLI provenance, identical content options and
Packager binaries, a fully owned output, and verified artifact bytes. The CLI
reports `PXCLI1217` (`refresh-runtime`) when it uses this path. Changed Story,
UI, media, Packager, or package options still use normal packaging. Older
provenance needs one normal pack before runtime-only refresh is available.

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
installation coordination. Native `VFS::MountArchive` now accepts an explicit
`ArchiveMountPriority::Overlay` for trusted callers: the newest overlay wins
over earlier archives, while directory mounts still win over all archives.
An overlay may now supply a bounded list of tombstone URIs to hide resources
from older PDX files. Invalid, duplicate, or conflicting tombstones reject the
entire mount, and an unreadable overlay entry cannot silently fall back to an
older copy. These are low-level VFS semantics. Player accepts a signed,
exact-base content update chain from `Package/updates`; it ignores unsigned
or incompatible files and starts the base package if a valid chain is
ambiguous. The complete
distribution `create-patch` / `apply-patch` installer remains available. The selected native
Packager must understand both package versions; cross-engine upgrades may
require a full installer rather than this bundle format.

For SDK developers, the [single-file content overlay
contract](shipping.md#signed-pdx-content-overlays) now has `prismatix create-overlay` and
`prismatix verify-overlay`. The creator fully checks both complete input
distributions and rejects changed Player/runtime files. The verifier checks
the candidate PDX against exact base and target metadata and all changed
payloads. Generate a publisher key pair, pin its public key in both
distributions, sign the overlay, and use `--require-signature` before
installing it in `Package/updates` as a `.pdx` file. Multiple signed updates
are applied in exact-base dependency order. An independently enabled optional
pack can be produced with `create-overlay --optional-id <id>` and toggled with
`PrismatiXPlayer --optional-pack <id> on|off` after installation.

Never choose Story, Assets, another source input or an unrelated existing folder as output. `pack` is an explicit distribution command; `run` packages and launches. Quote paths containing spaces.

Output replacement verifies the previous build's inventory. If you have played a distribution and it now contains saves/logs, or edited its files, use a new output directory; do not delete player data to bypass the guard. Test migration with a copy of the old saves before distributing an update.
When replacing an owned output at the same content/save version, Packager carries its former save fingerprint and any legacy key readers through CLI staging. A fresh, unrelated output directory has no previous release to inherit from; distribute it through a signed exact-base overlay or use an explicit versioned save migration when old saves must be accepted.

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
`--sync --write` to update only the sidecars with a backup. When old anchors
were removed, the write is refused until you review their effect on old saves;
then pass `--sync --write --accept-removals` to acknowledge those removals.
This flag does not migrate saves or prove compatibility. Reorders and ambiguous duplicate lines require manual identity
review. For a rewrite immediately beside an insertion, sync the rewrite first,
then insert and sync again; a combined edit cannot always be safely matched
because the sidecar stores fingerprints, not the old prose. Test existing saves
before shipping a revised story. For explicit cross-version anchor changes,
see [save migration anchors](shipping.md#save-migration-anchors), including the sparse
`map-or-preserve` policy for large Stories.

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

### Diagnostics and troubleshooting

The terminal shows readable diagnostics by default. Pass `--json` for stable NDJSON codes, paths and source spans on stderr; preserve stderr for editor integrations. `PXSTORY1199` identifies an unknown Story command. Unknown options and invalid typed values fail. `dev --once` tests initial apply/play and shutdown, not an interactive editing session.

Normal discovery uses the matching `runtime/<platform>-<architecture>/` bundle inside the CLI package. A missing or incompatible bundle is an installation/release problem. Do not install C++ build tools as an author workaround. Explicit `--runtime`, per-tool options and `PRISMATIX_*` overrides are developer diagnostics. Manifest validation cannot attest to the compiler ABI of an arbitrary executable supplied as an override.

For registry or proxy errors during create, fix npm's registry/proxy configuration and retry. Do not use sudo or disable security controls. A nonempty or read-only destination is rejected. After abrupt termination, inspect a leftover create lock before removing its empty lock directory and retrying.

### Verify a runtime bundle release

Native build instructions are in the [contributor section](../README.md#engine-contributors). Release engineers run:

```bash
node scripts/phase1-clean-install.mjs --mock --keep
node scripts/phase1-clean-install.mjs --runtime /path/to/installed-runtime --keep
```

Here `--runtime` supplies binaries for the candidate npm package; it is never passed to the simulated author CLI. The harness serves candidate packages through a loopback registry, installs into a temporary global prefix, creates the project, watches edit/error/repair, closes the registry, reopens offline, builds and launches Player. Failed workspaces are retained; `--keep` also retains success evidence. Mock proves orchestration only. Native acknowledgements and launch do not prove visual interaction, audio or glyph correctness.

Windows x64, Linux x64 and macOS x64/arm64 are intended desktop targets. Each exact platform/architecture needs its own candidate evidence. WASM Preview is separate from full Web game distribution; mobile, console and full Web shipping are outside this desktop release.

Continue with the [cookbook](#story-author-cookbook), [placed UI item authoring](#data-driven-placed-ui-items), [layered portrait authoring](#layered-portrait-authoring), [custom effects](#custom-render-effects), [runtime extension contracts](../runtime-sdk/README.md) and [release compatibility](releases/delivery-contract.md).

## Story author cookbook

These examples use the public Story compiler in `packages/authoring-sdk/src/story.ts`, not guessed spellings of runtime JavaScript methods. Start from a generated project and run `prismatix validate my-game` after changing declarations. Asset examples require the referenced file to exist; an explicit catalog entry is needed only when you want to pin a stable asset identity yourself.

### Rewrite, branch and preserve identities

```pxstory
*start
@guide
今天想去哪裡呢？

[choice]
- 看看星星 -> stars
- 聽個故事 -> story
[endchoice]
*stars
第一顆星亮起了。
[jump finish]
*story
很久很久以前，有一個小小的願望。
*finish
[end]
```

An empty line ends speaker scope. Labels use `*name`; each `- text -> label` line is one choice (the long form is `[choice text="看看星星" goto=stars]`), and `[jump finish]` is short for `[jump target=finish]`. A generated project's adjacent `.pxids.json` keeps save identities without `[id ...]` in the prose. Prose rewrites retain their identities; for added or removed executable lines, review `prismatix ids <project> --sync` and then run `--sync --write`. Removing an old operation requires `--accept-removals` when writing the sidecar, after reviewing old saves and migration. Do not delete the Story's `; @prismatix-ids` marker or the sidecar. The CLI and importers write the sidecar as canonical JSON with one operation per line, so a changed operation is one line in a diff; older pretty-printed or single-line sidecars remain valid and keep their layout until the CLI next rewrites them. The identity values never depend on the layout. Changing routes, resources or variable schemas still requires a deliberate migration test against old saves. Identity stability does not by itself prove compatibility.

### Short forms for long scripts

Every short form below compiles to the same operations as its long form, so
save identities and the Runtime IR do not depend on which spelling you use.

```pxstory
@直哉「うっぷ……少し食い過ぎたか……」
@guide 今天想去哪裡呢？
@guide[smile left voice=g0042] 一起去吧！
あれめんどくさいんだよなぁ。\
他で返せたら、そっちでチャラにしたいものだ。
[show guide smile right]
[bg street rule=083r duration=1000ms vague=40]
[bgm bgm004]
```

- `@name text` speaks one line and returns to narration; `@name` on its own
  line still starts a block that ends at the next empty line. Text may follow
  the name directly when it opens with `「`, `『`, `（`, `(` or `“`.
- A name that is not an identifier (`@直哉`) is a display name: it speaks as
  the registered character whose `displayName` it is, otherwise as a plain
  label like `@"直哉"`. Character commands (`[show 直哉 ...]`,
  `[act 直哉 nod]`, `[hide 直哉]`) accept the same display names. A quoted
  `@"..."` is always a plain label.
- `@alias[...]` options apply to that line only: a bare word is an expression
  alias or a position (`left`/`center`/`right`), and `voice=` plays with the
  line. Expression and position require a registered character alias.
- A prose line ending in `\` continues on the next line; both lines are shown
  in one advance with a line break. Write `\\` for a literal trailing
  backslash.
- An expression is addressed by its name or any of its aliases
  (`[show kei smile]` for an expression named `smile`).
- Command arguments are separated by ASCII spaces only, so a CJK title keeps
  its ideographic space, and `[chapter 第一章 天台]` takes every word as the
  chapter title (shown in the HUD notice and in save slots).
- In `[show alias ...]`, words after the alias are the expression or the
  position in either order. Showing a visible character again without a
  position keeps its slot, offset and scale, so expression changes no longer
  snap it back to the center. Giving a position starts a fresh placement:
  omitted `x`, `y`, `scale` and `rotate` return to their defaults.
- `rotate=` turns a character about its centre in degrees (`[show aka z2
  left rotate=-90]` lays it down, as Artemis `fg rotate=` does); an expression
  change keeps the turn. `[move alias rotate=0 duration=1s]` animates it. The
  turn and a turn in progress are saved. A non-numeric value is
  `PXSTORY1155`.

Short resource names come from author-only search roots in `prismatix.json`:

```json
"storyAssets": {
  "bg": ["Assets/bg", "Assets/cg"],
  "rule": ["Assets/rule"],
  "sprite": ["Assets/Portraits"],
  "voice": ["Assets/voice"],
  "bgm": ["Assets/bgm"],
  "se": ["Assets/se"]
}
```

Kinds are `bg`, `rule`, `sprite`, `voice`, `bgm`, `se`, `ambience` and `video`.
A name is looked up among the files under these roots (and among
`resourceIncludes` and cataloged `assets`); only the files Story actually
names are packaged, so a root needs no matching `resourceIncludes` entry.
Roots are tried in order; inside
one root an exact relative path wins over a path without its extension, then a
file name, then a file name without its extension (`kei/k001` or `k001` for
`Assets/voice/kei/k001.ogg`). Several matches at the same level are an error
(`PXSTORY1161`) listing every candidate rather than a guess, and a bare name
with no match is `PXSTORY1160` instead of a missing project-root file. Values
containing `/` or an extension that match no root still work as ordinary
project paths.

On the imported 352-scene *Sakura no Uta* port, rewriting `[say]`/`[narrate]`,
separate `[voice]` lines and full media paths into these forms removes about
35% of the script's characters without changing any operation.

#### Inline text styles

Dialogue and narration text may change font, size and colour mid-line, next
to `[ruby=...]` and `[br]`:

```pxstory
@直哉「[size=150%]ぎゃあああ[/size]」
[b]Stop[/b], she said, [color=#C04040]quietly[/color].
[font=Assets/Fonts/hand.otf][i]a note in the margin[/i][/font]
@guide [size=75%]（小声で）[/size][ruby=きおく]記憶[/ruby]がない。
```

| Tag | Effect |
| --- | --- |
| `[size=150%]` | percent of the enclosing size, 10%–400%; nested percentages multiply |
| `[size=40]` | absolute size in logical pixels, 1–400 |
| `[color=#RRGGBB]`, `[color=#RRGGBBAA]` | text colour; the control's outline is kept |
| `[font=path]` | a font file by its project-relative path; the build packages it |
| `[b]`, `[i]` | bold and italic, synthesized when the font has no such face |

Each tag ends at its `[/size]`, `[/color]`, `[/font]`, `[/b]` or `[/i]`, or at
the end of the line when left open (`PXSTORY1182`). Closing tags need not nest
strictly: `[/b]` ends the most recent `[b]`. Runs on one line share a
baseline, and a ruby reading is drawn at half its base's size in the base's
colour. The tags take no typewriter step, so revealed text never shows half a
tag; the backlog and saves keep the markup, so a loaded game shows the same
styles. A line may open with a text tag and still be prose.

Three more tags only time the typewriter:

```pxstory
@直哉「待って[w 500ms]……[speed 50%]ゆっくり[/speed]、[nowait]一気に！[/nowait]」
[w 1s]間を置いてから話す。
```

| Tag | Effect |
| --- | --- |
| `[w 500ms]`, `[w 1.5s]` | pause before the next character, 0–60s (`ms` when no unit) |
| `[speed 50%]`…`[/speed]` | reveal at 10%–1000% of the player's text speed; nested tags use the innermost |
| `[nowait]`…`[/nowait]` | show the enclosed text at once |

The Player removes them from the text it shows, so the backlog, NVL pages
and saves hold the plain line, and a click still shows the whole line at
once. Instant text speed ignores them. A malformed pause or speed is
`PXSTORY1185`; an unclosed `[speed]` or `[nowait]` lasts to the end of the
line (`PXSTORY1182`). A line may open with one of them and still be prose.
They are not yet supported inside a ruby base (`PXSTORY1184`).

The Player hides any other bracketed text in dialogue; `prismatix validate`
warns about it (`PXSTORY1180`) and about style tags inside a ruby base, which
are shown as written (`PXSTORY1184`); put them around `[ruby]` instead. An
invalid size or colour is `PXSTORY1181` and a font path outside the project is
`PXSTORY1183`. Styled and ruby lines wrap at Unicode line-break opportunities
with the same 禁則 as plain text, and are laid out left to right;
right-to-left scripts keep to unstyled text.

#### Story macros

A macro names a block of Story lines that repeats with small changes. It is
expanded where it is called when the Story compiles, so it needs no
JavaScript (use a `.pxextension` command when the Player itself must do
something new):

```pxstory
[macro scream who line volume=150%]
[se se9022]
[shake 1320ms 6 target=window]
@%who [size=%volume]%line[/size]
[endmacro]

[scream 直哉 「ぎゃああああ」]
[scream who=藍 line=「うるさい！」 volume=200%]
```

- `[macro name ...]` lists parameters: a bare word is required, `name=value`
  has a default. The body runs to the next line that is exactly
  `[endmacro]`; a missing one is `PXSTORY1190`.
- A call passes values in order or by name. In the body `%param` is replaced
  by the value as written and `%%` by `%`; any other `%` is left alone. A value
  used inside a command argument should be quoted there if it may contain
  spaces (`[say speaker=%who text="%line"]`).
- A macro may call other macros; a call cycle is `PXSTORY1194`. Its name may
  not be a built-in command, an extension command or an inline text tag
  (`PXSTORY1191`), parameters must be unique identifiers (`PXSTORY1192`), and
  a call with a missing, extra or unknown value is `PXSTORY1195`. A body cannot
  hold labels, `[id]` or `[macro]` (`PXSTORY1193`): every call would repeat
  them.
- Definitions are local to their Story file and may come after their calls,
  unless they live in a shared macro file (below).
- Diagnostics and the source map point at the call; the diagnostic details
  name the macro, where it is defined and, for a syntax error inside the
  expanded body, its body line.
- Each expanded operation takes the call's identity plus its line in the body
  (`<call>/m0`, `<call>/m1`, ...). An `[id]` before the call or the file's
  `.pxids.json` entry for it pins the whole expansion, so saves and read-text
  marks survive edits elsewhere. Changing a macro's body renumbers the lines
  of every call, like editing those lines in place.

Macros every scene repeats belong in shared macro files, listed in
`prismatix.json` (author-only, like `storyAssets`):

```json
"storyMacros": ["Story/_macros.pxstory"]
```

```pxstory
; Story/_macros.pxstory
[macro scene_change bg rule=083r time=1000ms]
[image.hide all]
[hud visible=false]
[bg black rule=%rule duration=%time]
[wait %time]
[hide all transition=false]
[bg %bg transition=false]
[transition %time rule=%rule]
[endmacro]
```

```pxstory
; Story/day1.pxstory
[scene_change bg006a]
[scene_change bg012 rule=wipe-left]
```

- Every Story in the project may call these macros. A shared macro file holds
  only `[macro]` definitions, comments and blank lines; anything else is
  `PXSTORY1196`, because it would never run. The file emits no operations
  and is not a scene.
- Names are unique across all shared files and may not be redefined by a
  Story's own `[macro]` (`PXSTORY1191`, naming the first definition).
- Calls behave exactly like calls to local macros: identities come from the
  call, so editing or moving the shared file keeps saves valid, and the
  diagnostics point at the call with the shared file and line in the
  details (`in macro scene_change (Story/_macros.pxstory:2)`).
- `storyMacros` lists 1-64 distinct project-relative `.pxstory` files
  (`PXBUILD1120`); a listed file that does not exist is `PXBUILD1121`.
  `compileStory` and `compileStoryProject` callers pass the result of
  `createStoryMacroLibrary(files)` as `storyMacros`.

#### Full-screen (NVL) text

`[nvl]` switches from the message window to a full-screen page: each line
that follows is added to the page under the previous ones instead of
replacing them. `[nvl.clear]` starts a new page, and `[adv]` returns to the
message window and discards the page.

```pxstory
[nvl]
桜の季節が、また巡ってきた。
@直哉 「……ああ」
[nvl.clear]
新しい頁。
[adv]
```

- The HUD needs a `RichTextLabel` named `NVLText`. The Player shows it (or a
  group named `NVLPanel` around it) in NVL mode and hides `Dialogue`,
  `Speaker` and `MessageWindow` (or a group named `ADVPanel`); `dialogue.nvl`
  is bindable UI data for anything else that should follow the mode.
- A speaker's line appears as `【name】text`; lines are separated by an empty
  line, the newest one types out like ADV text, and inline styles and ruby
  work as in the message window.
- A line that would overflow `NVLText` at its font size starts a new page by
  itself, so long passages need no manual `[nvl.clear]`.
- Every line also goes to the backlog as usual. The page and the mode are
  saved, restored after loading and rolled back with the line they belong to.

#### Staff roll

`[credits]` scrolls a staff roll from below the screen to above it, then the
Story continues:

```pxstory
[hud visible=false]
[bg black]
[credits duration=60s skippable=false size=32 color=#FFFFFF]
# シナリオ
Owen Liu

# 原画
[image logo]
[endcredits]
[hud visible=true]
```

- Each line of the block is one centred item: text, a `#` heading (1.5x
  the size), `[image name]` (a `storyAssets.bg` name or a path; images wider
  than the screen are scaled down), or an empty line as a gap. `;` lines are
  comments; leading and trailing empty lines are dropped. Text is drawn as
  written: inline tags are not read inside a roll.
- `duration` is the whole scroll, 1s–600s (default 30s). With the default
  `skippable=true` a click or skip ends the roll; `skippable=false` ignores
  them until it has finished. `size` is 8–200 pixels (default 32), `color`
  is `#RRGGBB(AA)` (default white) and `font` a project font file (default:
  the locale's first `fontChain` font).
- The roll is drawn over the Stage, under the HUD and message window, so
  hide the HUD first as above and choose the background behind it.
- The whole block is one operation and one `.pxids.json` entry; a locale may
  change its lines and images. A save made during the roll resumes after it.
- Errors: a missing `[endcredits]` (or one without a block) is
  `PXSTORY1270`, a bad option `PXSTORY1271`, and an unsupported or empty
  body `PXSTORY1272`.

There is no `[input var=...]` name-entry command yet. It needs a Player
text-entry screen (keyboard, IME and gamepad input), a new waiting state that
saves and rollback understand, and string variables written from the Player,
all of which belong to the Player UI rather than the Story compiler. Until
then, let a TSX screen collect the name and set a `profile` or `session`
variable through the runtime SDK, and use `{name}` substitution in text.

#### Speaker name colours

`"nameColor": "#F4BFD0"` in a `.pxcharacter` colours that character's name in
the HUD's `Speaker` label while they speak; other lines keep the label's own
colour. Lines already read are drawn halfway to grey (`dimReadText`), which
stays legible on light and dark message windows.

The Player publishes booleans for HUD details that would otherwise need
code: `dialogue.hasSpeaker` (a name plate that hides during narration),
`dialogue.waiting` (the line is complete and waits for a click: bind a
click-wait glyph's `visible` to it), and `hud.auto` / `hud.skip`.

Wrapped text breaks at Unicode line-break opportunities with 禁則: closing
punctuation and small kana never start a line and opening brackets never end
one. The message window decides the breaks on the whole line, so characters
do not jump to the next line while the typewriter reveals them.

`validate` and `build` warn (`PXCLI1060`) about characters that no font of a
locale's `fontChain` can draw, listing each with its first use: Story text
(dialogue, speakers, choices, chapter titles), UI text (TSX `text`, button
captions and `TextField` placeholders, checked in every locale, since UI text
shows whatever the language) and the locale's own `strings`. A node's own
`font` and `player.uiFont` are tried first; `SpriteText` draws image glyphs
and is skipped. Add a covering font to the chain (for example a Japanese font
for 凪 in a Traditional Chinese project).

#### Message-window face

An expression may name a face image, shown inside the message window while
that character speaks. Give it in the `.pxcharacter` as `facePath` (lowered
to a cataloged `faceAssetId`, like `assetPath`):

```json
{"name": "微笑", "aliases": ["smile"], "assetPath": "Assets/guide.png",
 "facePath": "Assets/Faces/guide-smile.png"}
```

The HUD shows it in an image control named `Face` (hidden when there is no
face and in NVL mode); `dialogue.face` holds the path for other bindings.

```pxstory
@guide 立ち絵がないので、既定の表情の顔。
[show guide sad center]
@guide 顔は立ち絵に合わせて sad になる。
@guide[face=smile] この一行だけ smile の顔。
@guide[face=none] この一行は顔なし。
```

- Without `face=` the face follows the expression shown on stage (the one
  whose image the character is showing), else the character's default
  expression. Expressions that share one image resolve to the first of them.
- `face=` takes an expression alias for that line only, or `none`. An unknown
  expression is `PXSTORY1114`; one without a face is warned about
  (`PXSTORY1163`) and shows none.
- A `player.options` entry with `"binds": "messageFace"` chooses when faces
  appear, as the Artemis `mwface` setting does: 0 never, 1 always (the default
  without such an option), 2 only while the speaker is not on stage.
- The face is part of the line's dialogue state, so saves, loads and rollback
  show the same face.

### Characters, backgrounds and sound

The starter registers `guide`, its `smile` expression and an original background. Reuse these without hand-writing another UUID:

```pxstory
[bg window]
[show guide expression=smile position=left x=0 y=0 scale=0.8]
@guide
這裡就是新的背景。
[move guide position=right duration=400 wait=true]
[hide guide]
```

Use `[hide all]` to fade out every character without listing its aliases.
`[hide all transition=false]` removes them immediately. Neither form clears
the background or independent stage layers. The same operation is available
to runtime scripts as `Engine.ClearCharacters(false)` or through the typed
runtime SDK as `ctx.stage.clearCharacters(false)`.

`[bg window]` works because the starter's `prismatix.json` declares
`"storyAssets": {"bg": ["Assets"]}`; `[bg Assets/window.png]` is the same
operation.

Characters and image layers are hidden with the same verb:

| Hides | Short form | Long form |
| --- | --- | --- |
| one character | `[hide guide]` | `[hide character=guide transition=true]` |
| every character | `[hide all]` | — |
| one `[image]` layer | `[hide image=cg1 400ms]` | `[image.hide cg1 400ms]` |
| every image layer | `[hide image=all]` | `[image.hide all]` |

`[hide image=...]` takes only a duration (`PXSTORY1245` otherwise) and
compiles to exactly the `[image.hide]` operation.

Character actions move a visible character relative to where it stands and
always return it to rest, so they never change its slot or saved placement:

```pxstory
[act guide nod]
[act guide jump size=30 time=250ms loop=2]
[act guide float loop=-1]
@guide[act=hop] わっ！
[act guide stop]
```

Presets are `hop`, `jump`, `startle`, `yawn`, `bow`, `nod`, `nods`,
`tremble`, `shake-head`, `sway`, `float`, `wiggle`, `dip`, `lift`,
`nudge-right`, `nudge-left`, `bob` and `waver`. `size` is pixels (percent
for `startle`/`wiggle`, degrees for `sway`), `time` is one segment (at most
600s), `loop=-1` repeats until `[act alias stop]`, and `ease` is `linear`,
`in`, `out` or `inout`. A looping action is saved and resumes after loading.
The same presets play on the background and image layers (see below).
`wiggle` stretches in anti-phase like Artemis ウゴウゴ: while the width
shrinks by `size` percent the height grows by the same amount, and back.

`[se.stop fade=500ms]` stops every playing sound effect, `[se.stop channel=2]`
only channel 2 (see below), and `[voice.stop]` stops the current voice.
`[shake 300ms 5 target=window]` shakes only the message window layer instead
of the Stage.

With `dir=` a shake follows the Artemis `quake` curve instead: the view jumps
`amplitude` pixels one way, then swings across `count` times (default 10),
each swing 0.85 times the last and rounded up to whole pixels, and settles.
`dir=h` moves horizontally, `dir=v` vertically, and `dir=r` vertically on that
curve and horizontally at twice its rate. Each swing lasts
`duration / (count + 0.5)`, the first half as long, so the Artemis default
(size 6, 60 ms per swing, 10 swings) is:

```pxstory
[shake 630ms 6 dir=r]
[shake 330ms 4 dir=h count=5 target=window]
```

A running shake of either kind is saved with the Stage. A bad `dir` or a
`count` outside 1–100 (or without `dir`) is `PXSTORY1154`.

`[auto on]` makes the Story read itself until `[auto off]`, like Artemis
`autoplay`: `speed=` sets milliseconds per character (0–1000) and `wait=` the
pause after each line (at most 60 s); either left out keeps the player's
setting. `lock=true` also ignores the player: clicks, keys, the HUD menus,
backlog, rollback and skip do nothing until `[auto off]`. Without a lock a
click advances at once and reading continues. `[skip.stop]` ends skipping,
and `[skip.disable]` refuses skip (Ctrl included) until `[skip.enable]`.
A choice ends an auto section, as it would otherwise wait for a click the
lock refuses.
The auto section and the skip lock are saved with the game. Invalid values
are `PXSTORY1156`.

```pxstory
[auto on speed=20 wait=2500ms lock=true]
……静かな夜だった。
[auto off]
```

`[hud visible=false]` hides the message window and HUD until the next line
shows them again; `[hud visible=true]` shows them at once. Both fade over
200 ms unless `time=` says otherwise (`[hud visible=false time=1s]`, at most
10 s, `time=0` switches instantly, as Artemis `msgoff`/`msgon time=`). The
Story does not wait for the fade, and later automatic fades use 200 ms again.

`bgm`, `ambience`, `se` and `voice` accept `volume=0..100`, the level of that
one cue in percent of its bus (the Config volume stays in charge).

Sound effects without a channel overlap freely and are forgotten by saves.
`channel=1..64` gives a cue its own channel, as Artemis `se id=` does: a new
cue on a channel replaces the one playing there, the cue's `volume` applies to
that channel only, and a channel may `loop=true` and `fade` in. A looping
channel is saved with its position and resumes after loading or rollback;
one-shot channel cues are transient like any sound effect. While skipping,
one-shot cues are dropped but loops still start.

```pxstory
[se rain channel=2 loop=true fade=500ms volume=40]
[se door channel=1 volume=60]
; the knock cuts the door off
[se knock channel=1]
; the rain only, then everything (pool and channels)
[se.stop channel=2 fade=1s]
[se.stop]
```

A channel outside 1–64 is `PXSTORY1177`; `loop` and `fade` on an `se` without
a channel stay errors (`PXSTORY1147`, `PXSTORY1146`). The ambience bus remains
for a single background loop that follows the ambience volume setting.

`[flash color=af4154 alpha=0.6 duration=300ms loop=2]` washes the screen with
a colour: it rises to `alpha` and falls back, `loop` times, each taking
`duration`. It is skipped while fast-forwarding.

#### Framing, image layers and whole-screen transitions

A background can be framed instead of filling the screen. `x`/`y` place the
image's top-left corner as if it were unscaled, `zoom` and `rotate` apply
about `anchor` (image pixels; the image centre when omitted), and the
`from-*` values start an eased move over `duration`. Repeating `[bg]` with
the same image pans or zooms it; omitted framing values are kept:

```pxstory
[bg street-wide x=-800 y=-473 zoom=0.9 anchor=1920,1126]
[bg street-wide x=-200 duration=4s ease=inOutSine]
```

`[image]` places a named layer with the same framing, plus `alpha` (0–1) and
`z` (below 0 is behind characters, 0 and above in front). A new layer with a
`duration` fades in; `[image.hide name]` or `[image.hide all]` fades out and
removes it:

```pxstory
[image cg1 ev_park_01 x=-300 y=-616 zoom=0.35 anchor=960,947 z=10 duration=400ms]
[image.hide cg1 400ms]
```

`[act]` plays the character presets on the background and on image layers
too: `bg` is the background and `image=` names a layer placed by `[image]`:

```pxstory
[act bg shake-head size=10 time=100ms loop=-1]
[image smoke fog x=-768 zoom=1.5 anchor=1728,540 z=11]
[act image=smoke float size=100 time=5s loop=-1]
[act image=smoke stop]
```

The image keeps its framing. Movement is in image pixels, so it follows the
image's `zoom`; `sway`, `startle` and `wiggle` turn and scale about the
image's `anchor` (its centre when unanchored), and a background that fills the
screen turns about the screen centre. Another image in the layer, or a new
background, ends the action; reframing the same image keeps it running. A
looping action is saved with its layer and resumes after loading. Only one
action runs per target: a new `[act]` replaces the running one from its
current pose. `[act character=bg nod]` still reaches a character aliased
`bg`. An `image=` name that no `[image]` in the same Story shows is a warning
(PXSTORY1176), since another Story may have placed that layer.

`[transition]` reveals every Stage change since the last displayed frame at
once, as a crossfade or through a grayscale rule image. Stage the changes
without their own fades, then transition; the Story waits unless `wait=false`,
and a click or skip moves on:

```pxstory
[hide all transition=false]
[bg classroom transition=false]
[show guide smile center transition=false]
[transition 1s rule=wipe-left vague=40]
```

Rule images resolve from `storyAssets.rule`. `[act window hop size=5]` plays
any character action preset on the message window.

#### Changing only the face of a layered character

When a character's expressions are role-tagged `.pxportrait` descriptors
(see [layered portraits](#layered-portrait-authoring)), each expression is a body
(`base` layers) plus a face (`expression` layers), and an alias such as
`z1e0700_e0022` names both. `[face]` changes only the face and keeps the
body, pose, position, offset, scale and turn on stage:

```pxstory
[show 藍 z1e0700_e0022 center]
@藍「……」
[face 藍 e0023]
@藍「えへへ」
[face 藍 z1e0000_e0001]
```

- The face is the file name, without extension, of the expression layer
  image (`e0023` for `.../e0023.png`; several layers join with `+`), or an
  expression alias whose face to use.
- The compiler lists, for every body of that character, the expression that
  shows this face; the Player picks the one whose `base` layers match the
  portrait on stage and switches with `transition=auto` (only the face
  crossfades). `transition=true|false` works as for `[show]`.
- A character without layered expressions is `PXSTORY1261`; a face no
  expression shows is `PXSTORY1262`; `[face]` without a character and a face
  is `PXSTORY1260`. At run time a body with no such face, or a character not
  on stage, leaves the image unchanged and logs warning `PXVN6102`.
- The message-window face option `@藍[face=...]` is a different feature: it
  picks the face image shown inside the message window for one line.

To add an expression, add a source image and an expression with `assetPath` in `Characters/guide.pxcharacter`, then select its alias with `expression=...`. The Authoring project compiler generates canonical character, expression, and asset UUIDs when omitted; no catalog entry is needed for that image. For example, `{"name":"Happy","aliases":["happy"],"assetPath":"Assets/guide-happy.png"}` is enough inside `expressions`. TypeScript generators can use the exported `CharacterSourceDocument` type; the stricter `CharacterDocument` type describes the published canonical file. Validate this shorthand with `compileProject` or `prismatix validate`, not the low-level `validateDocument("character")` canonical checker. The generated IDs derive from the project, character source path, expression's first alias, and image path. Keep those stable across save-compatible releases, or pin `id`/`assetId` explicitly before renaming; a file move is not automatically a save migration. Positions are `left`, `center`, `right` (also 1/2/3). The move duration is milliseconds in the native session implementation.

For a dynamically selected family of UI/media files, put the containing
directory in the project's `resourceIncludes`, for example
`"resourceIncludes": ["Assets/UI/Original"]`. UI controls can still use
ordinary paths; do not also list every file in `assets` unless it needs a
stable catalog identity. `prismatix validate` reports `catalogIncludeOverlap`
so large duplicate inventories are visible. Overlap is not automatically an
error: an ID referenced by a gallery or character may still be necessary.

After registering suitable decodable media, the compiler accepts:

```pxstory
[bg Assets/garden.png]
[bgm Assets/evening-loop.wav intro=Assets/evening-intro.wav fade=250ms]
[ambience Assets/rain.wav loop=false fade=100ms]
[se Assets/bell.wav]
[voice Assets/greeting.wav]
@guide
歡迎回來。
```

`bgm` and `ambience` accept `fade` (milliseconds by default, or a `ms`/`s`
suffix) and `loop=true|false`. `bgm intro=...` plays the intro once, then
loops its main asset; it cannot be combined with `loop=false`. Both BGM files
are collected for packaging. `bgm.stop` and `ambience.stop` also accept `fade`.
Unsupported options on `voice` and on `se` without `channel=`, invalid timing, and unsafe paths are
authoring errors instead of silently changing playback. Use the typed runtime
API in an extension for volume and multi-track control. A missing reference is
a build diagnostic, while decode/device failures need native testing with real
media.

For startup screens and title voices, declare all random candidates in the
project's `settings` instead of duplicating them in `resourceIncludes`:

```json
"settings": {
  "player.titleVoiceVariants": ["Assets/Voice/title-a.ogg", "Assets/Voice/title-b.ogg"],
  "player.splashes": [{
    "scene": "Content/UI/Logo.pxui",
    "audioVariants": ["Assets/Voice/logo-a.ogg", "Assets/Voice/logo-b.ogg"],
    "audioDelay": 1,
    "minimumDuration": 4
  }]
}
```

The splash scene must be listed in `uiEntryPoints`; for a TSX-authored route,
use its `.tsx` source path in both places and the CLI publishes the compiled
`.pxui` path. The compiler collects
*every* candidate for packaging and reports missing media when the project
resolver is available; the Player chooses one available candidate at runtime.
`audio` remains available for a single splash sound. The project schema checks
these known settings, including paths and timing, before a distribution build.

### Variables and conditional flow

Declare a variable in Story with `[var]`; no JSON is needed:

```pxstory
[var affection=0]
[var met_aoi=false scope=profile]
[var nickname="直哉"]
[var ratio=0 type=number]
```

- `name=default` gives the name and default value. The type follows the
  default (`0` integer, `0.5` number, `true`/`false` boolean, anything else
  string) unless `type=boolean|integer|number|string` says otherwise; `scope`
  is `session` (the default) or `profile`.
- A declaration emits no operation and takes no entry in the `.pxids.json`
  sidecar. The build merges every Story's declarations into the compiled
  `game.pxgame`, so the Player, saves and UI bindings see an ordinary catalog
  variable. `[set]`, `[add]`, `[if]` and choice conditions check names and
  types against it.
- Declaring the same variable again (in another scene, for example) is fine
  when it agrees; a different type, scope or default, including one that
  disagrees with the `.pxgame` file, is `PXSTORY1251` naming the first
  declaration. A malformed `[var]` is `PXSTORY1250`, and `[var]` inside a
  macro is `PXSTORY1252` (declarations are read before macros expand).
  `scope` and `type` cannot be variable names in `[var]`.

A hand-written catalog still works and may be combined with `[var]`, for
example by setting `"gameCatalog":"Content/game.pxgame"` in the project and
creating:

```json
{"format":"PrismatiXGame","schemaRevision":1,"variables":[{"name":"affection","type":"integer","default":0,"scope":"session"}],"gallery":[],"unlockables":[]}
```

```pxstory
[set affection=1]
[if affection > 0]
小光記得這個約定。
[else]
故事才剛開始。
[endif]
```

Use `session` for story state that belongs to a checkpoint; use `profile` intentionally for cross-session progression. Do not treat profile unlocks or preferences as ordinary rollback variables. The compiler checks declared variable names and assignment types.

#### Asking for a name: `[input]`

```pxstory
[var hero="直哉" type=string scope=profile]
[input var=hero prompt="名前を入力してください" default=直哉 max=8]
@hero 俺は{hero}だ。
```

The Player shows the HUD's text input panel, waits, and stores the trimmed text
(or the default, when the field is left empty) in the string variable; `{hero}`
in a line shows it. `max` counts characters (1-256, default 32). The built-in
HUD has the panel. In your own HUD add one, bound to the Player's data:

```tsx
<Group name="NamePanel" bind={{visible: "input.visible"}} position={[460, 380]} size={[1000, 260]}>
  <Text name="NamePrompt" bind={{text: "input.prompt"}} position={[40, 30]} size={[920, 50]} />
  <TextField name="TextInput" position={[40, 100]} size={[920, 64]} />
  <Button name="NameOk" text="OK" action={actions.input.accept} position={[800, 190]} size={[160, 50]} />
</Group>
```

The field must be named `TextInput`. Enter accepts as well. While the prompt is
open the Player ignores other UI actions and clicks. Preview has no prompt and
stores the default.

#### `[elseif]`, conditional choices and choice blocks

```pxstory
[if affection > 5]
「ずっと一緒にいたい」
[elseif affection > 2]
「また明日ね」
[else]
「……じゃあ」
[endif]

[choice]
- 一起看看星星 -> stars
- 告白する -> confess if affection > 3
- 次の日へ -> scene=day2
[endchoice]
```

- `[elseif expr]` may repeat between `[if]` and an optional `[else]`. It
  compiles to `[else]` plus a nested `[if]`, and the one `[endif]` closes
  them all, so older Players run it unchanged. `[elseif]` without an open
  `[if]` is `PXSTORY1243`; after `[else]` it is `PXSTORY1244`; a bad
  expression is `PXSTORY1133` as for `[if]`.
- `[choice text="告白する" goto=confess if="affection > 3"]` shows the option
  only while the expression is true when the choices appear. The others keep
  their order; if every option is hidden, the Story continues after the
  choices instead of waiting. Saves keep the options that were offered.
- A `[choice]` block lists one option per line as `- text -> label`. After
  the label, `scene=id` jumps to another scene (`-> scene=day2` alone jumps
  to its start, `-> scene=next` to the following scene) and `if <expression>`
  runs to the end of the line. Quote the
  text when it contains `->` (`- "A -> B" -> label`). Blank lines and `;`
  comments are allowed; `[choice.wait]` is not needed. Each option compiles
  to exactly the operation of the equivalent `[choice text=... goto=...]`
  line and is its own entry in the `.pxids.json` sidecar.
- Block mistakes: a line that is not an option is `PXSTORY1240`, a missing
  `[endchoice]` (or one without a block) is `PXSTORY1241`, and an empty block
  is `PXSTORY1242`.

### UI actions and Timeline

The template's title button contains this actual Action descriptor:

```json
{"id":"load.open","arguments":{}}
```

`game.start`, `save.open`, `load.open`, `backlog.open` and `settings.open` are built-in Player actions used by the default UI. Keep scene `onClick` Actions distinct from Story UI operations:

```pxstory
[ui route=save operation=push]
```

#### The Player's actions

Every stock Player action, with the argument it takes, is listed once in
`Engine/UI/Actions/PlayerActionContract.h`. Build actions with the typed
`actions` builders instead of spelling ids:

```tsx
import {actions} from "@prismatix/authoring-sdk";

<TextureButton name="Save" skin="ui/save" action={actions.screen.save} />
<TextureButton name="Extras" skin="ui/extra" action={actions.screen.open("chapter3")} />
<TextureButton name="Slot" skin="ui/slot" action={actions.save.slot("$key")} />
```

`validate` rejects an id in a Player namespace that the Player does not have
(`PXSDKUI1026`, e.g. `set.bgm.vaule`) and an argument with the wrong name or
type (`PXSDKUI1025` says what the action takes). `screen.open` opens any route
declared in `uiEntryPoints`.

Extension-declared Actions use the parameter names and types in their `.pxextension` manifest; give them their own namespace, because the Player's namespaces (`game.`, `set.`, `music.`, ...) are reserved. For a `resource` parameter, wrap a path so it reaches the Runtime as a ResourceRef, not an ordinary string:

```json
{"id":"jukebox.play","arguments":{"media":{"type":"resource","value":"Assets/Audio/theme.ogg"},"volume":0.8}}
```

The same typed UI argument bridge accepts `color`, `uuid`, `token`, `vec2`, `rect`, arrays and objects. `prismatix validate` checks extension-declared Action arguments (required names, types, enum choices and ranges) and includes static resource paths, including manifest resource defaults, in the artifact. Actions registered only by a Player/host at runtime have no authoring manifest to check and remain runtime-validated. A bare string path is still a string, so it cannot satisfy a declared `resource` parameter.

A registered `.pxtimeline` can be played using its resource reference:

```pxstory
[timeline Animations/greeting.pxtimeline]
```

Create the actual clip with typed authoring contracts or adapt the existing sample's Timeline. Story's `timeline` accepts the clip reference; it does not expose every runtime playback option. For image-layer animations in UI, see [`.pxmotion` authoring](#authoring-layered-motion); for image-based lettering without a node per possible character, see [SpriteText](#sprite-lettering-without-one-control-per-glyph). For keyboard shortcuts, see [named input actions](#authoring-input-actions). See also [the React-free TSX example](examples/jsx-ui-authoring.tsx), [custom effects](#custom-render-effects), and [the runtime SDK](../runtime-sdk/README.md) for richer authored UI and presentation APIs.

#### Panels without textures

Any group, button or image can be a rounded, bordered, gradient or shadowed
panel; the engine draws it behind the control's content:

```tsx
<Group name="Card" position={[120, 180]} size={[820, 420]}
  style={{gradient: ["#FFFFFFCC", "#F2F5FDB8"], radius: 22, border: [1.5, "#FFFFFFF0"],
    shadow: {color: "#2A3A7A2E", blur: 26, y: 8}}} />
<Button name="Start" text="開始遊戲" font="Content/Fonts/NotoSerifTC-Bold.ttf"
  style={{background: "#FFFFFF10", hoverBackground: "#E9CF942E", radius: 21,
    border: [1, "#FFFFFF33"], hoverBorderColor: "#E9CF94FF", hoverTextColor: "#F6E6BEFF"}} />
```

`radius`, `border`, `gradient` (`[from, to]`, or `[from, to, "horizontal"]`) and
`shadow` are shorthands for the appearance fields `cornerRadius`,
`borderWidth`/`borderColor`, `backgroundColor`/`gradientColor`/`gradientDirection`
and `shadowColor`/`shadowBlur`/`shadowOffsetX`/`shadowOffsetY`;
`hoverBackgroundColor`, `hoverBorderColor` and `hoverTextColor` style the
pointer-over state. A transparent circle with only a large `shadow` makes a
soft ambient glow. The stock Player screens are built this way. `Button`,
`TextureButton`, `Text` and `RichTextLabel` take `font` for their caption.

A prop that TypeScript accepts but no lowering uses is warned about
(`PXSDKJSX1103`) instead of being dropped silently.

#### Typed TSX props instead of runtimeProperties

Prefer the typed props below to `runtimeProperties`; TypeScript checks them,
and `prismatix validate` warns about the old spelling at the authored line
(`PXSDKJSX1100`), about `<Control runtimeType="Slider">` (`PXSDKJSX1101`) and
about four hand-written state images that `skin` would derive (`PXSDKJSX1102`):

```tsx
<TextureButton name="Save" skin="Assets/UI/mw/bt_save" disabledPath="Assets/UI/mw/bt_save-off.png"
  position={[780, 776]} size={[96, 47]} action="save.open" />
<Slider name="TextSpeed" option="mspeed" position={[919, 300]} size={[400, 18]}
  railPath="Assets/UI/conf/rail.png" fillRailToValue />
<Group name="Dialog" fill clipContent linearTransition
  showTransition={{seconds: 0.2, offset: [0, -50]}} hideTransition={{seconds: 0.2}} />
```

`<Image path="Assets/UI/conf/char.png" region={[208, 192, 208, 96]} />` shows
one cell of an atlas (in image pixels) as if it were a whole image of that
size, so sprite sheets need neither pre-cut files nor a clipping group with a
negative offset.

`visibleWhen="profileFlags.cleared"` and `enabledWhen="!saveLoad.page1"` bind
one condition each, so a helper can pass either without one `bind` object
replacing the other. `tooltip={<Image path="Assets/UI/help-save.png"
position={[-100, -45]} size={[313, 43]} />}` on a named `TextureButton` shows
the element (positioned relative to the button) while the pointer is over it;
the build declares the `hover.<name>` data it needs, so `player.uiData` needs
no `hover.*` entries.

Two-state and one-of-many controls need no pair of hand-bound buttons:

```tsx
import {Radio, Toggle, actions} from "@prismatix/authoring-sdk";

<Toggle name="DockLock" option="dock" on="Assets/UI/mw/bt_lock" off="Assets/UI/mw/bt_unlock"
  position={[1864, 783]} size={[41, 38]} />
<Toggle name="MuteBGM" checked="settings.muted.bgm" action={actions.setting.toggleMute("bgm")}
  on="Assets/UI/conf/mute-on" off="Assets/UI/conf/mute-off" position={[1405, 256]} size={[80, 32]} />
<Radio name="FontSerif" option="font" value={2} skin="Assets/UI/conf/radio34"
  selected="Assets/UI/conf/radio34-on" position={[913, 354]} size={[34, 34]} />
<TextureButton name="Slot" skin="Assets/UI/save/slot" action={actions.save.slot("$key")} />
```

`Toggle` lowers to `<name>On` / `<name>Off` buttons and `Radio` to
`<name>Button` / `<name>Selected`, bound to `options.<key>.is<value>` or the
`checked` path. `actions` builds every stock Player action with the right id
and argument key (`actions.option.set("dock", 0)`, `actions.setting.volume("bgm",
80)`, `actions.screen.close`, ...), so a typo is a TypeScript error rather than
a button that does nothing.

`skin="stem"` uses `stem-normal.png`, `stem-hover.png` (also for pressed) and,
with `skin={{base: "stem", states: ["hover", "pressed", "disabled"]}}`, the
other states; an explicit `*Path` wins. Button sounds need no per-button
props: `player.uiActivateSound` plays for every button and
`player.uiCancelSound` for `overlay.close`, `route.back` and `*.cancel`
actions; `activateSoundPath=` overrides one button.

#### Where choices appear

The HUD's `ItemsPlaced` over `dialogue.choices` decides where choice bars go:

```tsx
<ItemsPlaced name="ChoiceList" items={{path: "dialogue.choices", key: "key", placement: "rect"}}
  choiceLayout={{area: [0, 497, 1920, 86], pitch: 60,
    rows: {2: [-1, 1], 3: [-1.5, 0, 1.5], 4: [-2, -0.6666, 0.6666, 2]}}}>
  ...one choice template...
</ItemsPlaced>
```

`area` is the middle bar's rectangle. Each choice moves down by `pitch` times its
offset; `rows` sets the offsets for a choice count, and other counts sit one
pitch apart around the middle. The toolbar that appears when the pointer comes
near is controlled the same way: put an invisible control named `DockArea` in
the HUD. The older `player.choiceLayout` and `player.dockArea` settings still
work, and `validate` suggests moving them.

#### Screens that change with progress

Title buttons that unlock as the player clears routes are rules over the
global profile's flags, not code:

```json
"player.profileUiData": {
  "title.extraOpen": {"any": ["clear_a", "clear_b"]},
  "title.ending": {"cases": [
    {"when": {"all": ["clear_a", "clear_b"]}, "value": 2},
    {"when": "clear_a", "value": 1}
  ], "default": 0}
},
"player.titleBgm": [
  {"when": "clear_true_end", "path": "Assets/Music/title_after.ogg"},
  {"path": "Assets/Music/title.ogg"}
]
```

A condition is a flag name (true when the profile variable is `true` or `1`),
or `all`, `any` or `not` of conditions. A plain condition publishes a boolean
at its key (`bind={{visible: "title.extraOpen"}}`). A `cases` rule publishes the
first matching value as an integer, plus `title.ending.is0`, `.is1`, `.is2`
booleans. `player.titleBgm` plays the first rule that matches. `validate`
checks the rules and warns about a `player.*` key the Player does not read.

#### Player options and confirmation dialogs

`player.options` in `prismatix.json` declares small integer settings the Player
stores with the player's preferences. Each entry has a `default` and either
`min`/`max` or a list of `choices`. An option does nothing on its own; one that
declares `binds` drives a Player behaviour with `value * scale + offset`
(`scale` defaults to 1, `offset` to 0):

```json
"player.options": {
  "textSpeed": {"default": 80, "min": 0, "max": 100,
                "binds": "textSpeed", "scale": -1, "offset": 100},
  "autoSpeed": {"default": 50, "min": 0, "max": 100,
                "binds": "autoWait", "scale": -60, "offset": 6600},
  "windowAlpha": {"default": 70, "min": 0, "max": 100, "binds": "messageWindowOpacity"},
  "font": {"default": 1, "choices": [1, 2, 3], "binds": "font"},
  "askQuickSave": {"default": 0, "choices": [0, 1]}
}
```

Here slider 100 shows text at once (0 ms per character) and each point below
adds a millisecond; auto-reading waits 600 ms at 100 and 6.6 s at 0.

| `binds` | The Player receives |
| --- | --- |
| `textSpeed` | milliseconds per character, 0–120 (0 shows the line at once) |
| `autoWait` | auto-reading wait in milliseconds, 0–60000 |
| `skipUnread` | non-zero: skip also skips unread text |
| `messageWindowOpacity` | message window opacity in percent, 0–100 |
| `dimReadText` | non-zero: grey out text already read |
| `uiEffects` | 0: show screens without transitions |
| `font` | 1-based index into `player.fonts`; other values keep the theme font |
| `keepSkipAfterChoice`, `keepAutoAfterChoice` | non-zero: resume skip / auto-reading after a choice |
| `cursorHideDelay` | hide an idle cursor after this many milliseconds; 0 never hides it |
| `dialogCursorWarp` | non-zero: move the cursor onto the control named `DialogYes` when a dialog opens |
| `dockPinned` | 0: the HUD toolbar (`hud.dockShown`) shows only while the pointer is over the HUD control named `DockArea` or a choice is up |
| `messageFace` | 0: never show message-window faces; 2: only while the speaker is off stage; otherwise always |

Behaviours no option binds keep their defaults (the toolbar is pinned, the
cursor never hides). Two options binding one behaviour is warned about
(`PXBUILD1105`); whichever the player set last wins. A misspelled `binds` is a
validation error.

`<Slider option="autoSpeed" />` edits an option and needs no host code. `{id: "option.set", arguments: {argument: "font=2"}}`
sets one, and `options.<name>` (text) and `options.<name>.is<value>` (boolean)
are bindable UI data.

While the settings screen is open, `settings.preview` types a sample line at
the current text speed, holds it for the auto-advance wait (at least 0.3 s)
with `settings.previewWaiting` true, and starts again; moving either slider
restarts it. Bind a label to it to let players try their settings:

```tsx
<Text name="Preview" bind={{text: "settings.preview"}} position={[120, 820]} size={[900, 48]} />
<Text name="PreviewGlyph" text="▼" visibleWhen="settings.previewWaiting" position={[1030, 832]} size={[24, 24]} />
```

The locale string `player.settings.preview` replaces the sample line. The
stock settings screen shows it under the sliders.

`player.confirmations` lists the Player operations that ask before they run.
Each names the `kind` its dialog shows (the operation's own name when left out)
and, optionally, the option that stores "do not ask again":

```json
"player.confirmations": {
  "quickSave": {"kind": "qsave", "dontAskOption": "askQuickSave"},
  "save": {"kind": "save"},
  "overwrite": {"kind": "save2"},
  "exit": {}
}
```

Operations: `quickSave`, `quickLoad`, `save` (into an empty slot), `overwrite`
(an occupied slot), `load`, `returnToTitle`, `exit`, `backlogJump`,
`previousChoice`, `nextChoice`, `resetSettings`, `deleteSave`, and `copySave` /
`moveSave` (asked only when the target slot holds a save). Several operations may share
a kind. An operation asks only while the mounted route has a control named
`ConfirmDialog` and its `dontAskOption` is 0; unlisted operations run at once.
The dialog reads `dialog.visible`, `dialog.skipNext` and
`dialog.kind.<kind>` (one boolean per declared kind) and answers with
`dialog.accept`, `dialog.cancel` and `dialog.toggleSkip`; accepting with
`dialog.skipNext` set stores 1 in the `dontAskOption`. `prismatix validate`
rejects a `dontAskOption` that `player.options` does not declare
(`PXBUILD1104`), a binding to an undeclared `dialog.kind.<kind>`
(`PXBUILD1100`) and warns about a `ConfirmDialog` when nothing is listed
(`PXBUILD1103`).

#### Managing saves

Save and load screens can delete, copy and move saves and let the player
write a comment on a slot:

```tsx
<ItemsGrid name="Slots" items={{path: "saveLoad.slots", key: "key"}} ...>
  <Group name="Cell" size={[420, 120]}>
    <TextureButton name="SlotButton" skin="Assets/UI/save/slot" action={actions.save.slot("$key")} />
    <Text name="Note" bind={{text: "item.comment", visible: "item.occupied"}} ... />
    <TextureButton name="Copy" skin="Assets/UI/save/copy" action={actions.save.copy("$key")} ... />
    <TextureButton name="Move" skin="Assets/UI/save/move" action={actions.save.move("$key")} ... />
    <TextureButton name="Erase" skin="Assets/UI/save/erase" action={actions.save.delete("$key")} ... />
    <TextureButton name="Comment" skin="Assets/UI/save/note" action={actions.save.comment("$key")} ... />
  </Group>
</ItemsGrid>
<Group name="Picking" visibleWhen="saveLoad.pending.visible" ...>
  <Text text="Choose where to copy it" visibleWhen="saveLoad.pending.copy" ... />
  <Button text="Cancel" action={actions.saveLoad.cancelPending} ... />
</Group>
<Group name="CommentEditor" visibleWhen="saveLoad.comment.visible" ...>
  <TextField name="SlotComment" placeholder="A note on this save" ... />
  <Button text="OK" action={actions.saveLoad.acceptComment} ... />
  <Button text="Cancel" action={actions.saveLoad.cancelComment} ... />
</Group>
```

- `save.delete` removes the save (ask first with `deleteSave`).
- `save.copy` / `save.move` pick the source; the next slot the player
  activates is the target, whether through its ordinary `save.slot` /
  `load.slot` button or the same action. While a copy or move waits, every
  slot except the autosave accepts it, on both screens and across pages;
  `item.pending` marks the source, and `saveLoad.pending.visible`, `.copy`,
  `.move` and `.slot` describe the wait. Choosing the source again,
  `saveLoad.pending.cancel` or leaving the screen drops it. A copy replaces
  the target; a move onto another save swaps the two. The autosave can be
  copied but not moved.
- `save.comment` fills the screen's `TextField` named `SlotComment` with the
  slot's comment and focuses it; Enter or `saveLoad.comment.accept` stores up
  to 64 characters (empty removes the comment). `saveLoad.comment.visible`
  and `.slot` show the editor. The comment is `item.comment` (and
  `saveLoad.slot<N>.comment`); saving into a slot starts it without one.

A failed operation shows `saveLoad.error.visible`.

Any control can fade out when hidden instead of vanishing:
`showTransition={{seconds: 0.2, offset: [0, -50]}}`,
`hideTransition={{seconds: 0.2, offset: [0, 50]}}` and `linearTransition`
slide and fade it.

#### Audio defaults and per-character voice volume

A fresh install (and `settings.reset`) starts from the engine's volumes unless
the project sets its own, as percentages the settings screen shows:

```json
"player.defaultVolumes": {"master": 100, "bgm": 80, "se": 100, "voice": 100},
"player.voiceProfiles": {
  "ids": ["hikari", "guide", "other"],
  "fallbackByFilePrefix": {"npc_": "other"}
}
```

A voice file belongs to the profile named by its folder
(`Assets/Voice/hikari/001.ogg`); files in other folders use the first matching
file-name prefix. A settings screen binds `settings.profileMuted.<id>` and uses
`Slider`s named `VoiceProfile_<id>` with `VoiceProfileValue_<id>` labels; a
muted bus or profile keeps its level and its number reads `mute`.

`Text` accepts `font="Assets/Fonts/Number.otf"` for a label that must not use
the theme font, such as the digits beside a slider.

#### Executable name and icon

`prismatix pack` names the game's executable after `distribution`, which is
read only when packaging and never shipped as content:

```json
"distribution": {"executableName": "hikari", "icon": "Assets/Branding/hikari.ico"}
```

The platform extension is kept (`hikari.exe` on Windows). On Windows the
`.ico` becomes the executable's only icon, which Explorer, the taskbar and the
game window use. Changing either value rebuilds the package; the in-place
runtime refresh is skipped for branded executables.

### Localization, voice and release updates

UI text follows the language too: `textKey` binds a label to the current
locale's `strings` entry and updates it when the player switches language.

```tsx
<Text name="LoadError" textKey="save.incompatible" text="This save cannot be loaded." />
```

```json
{"format": "PrismatiXLocale", "schemaRevision": 1, "locale": "ja-JP",
 "strings": {"save.incompatible": "このセーブデータは読み込めません。"}}
```

The Player publishes every locale string as `strings.<key>` UI data; `text`
is shown until the locale loads. `prismatix validate` reports a key that no
locale defines (`PXBUILD1100`).

Declare locale resources and `fontChain` in `Content/Localization/<locale>.json`. The starter already includes its Traditional Chinese font. Validate every locale's images/voices and test real text rendering. The Player's live language change requires aligned document/operation identities and types; splitting or merging translated dialogue is not automatically a valid live-switch mapping. Keep topology aligned or redesign the scene/update and test its save anchors. Do not silently omit missing voice to make validation pass.

For an update, retain the previous packaged game and isolated test saves. Increment the intended content/save versions, provide the project's explicit save-migration chain, test dialogue moves and changed resource names/variable schemas, then launch the new packaged Player against copies of those saves. Never experiment on real player saves.

### Save, replay and JavaScript boundaries

Preview Title → Start → HUD exposes Save/Load for session-only checkpoints. Rebuilding or closing Preview clears those slots. Packaged Player Save/Load persists player saves on disk; use Player to test reopening and release migrations.

The runtime has checkpoint state for VM/Stage, routes/UI, backlog and Timeline; pending JavaScript commands and Actions have separate replay journals. Extension state must use a versioned synchronous JSON provider. Arbitrary closures, globals, promises or the entire QuickJS heap are not saved. Use the actual provider API shown in [runtime-sdk/README.md](../runtime-sdk/README.md), including its migration callback.

Checkpoint restore can re-enter command/Action code to reconstruct a suspended continuation. Engine operations are checked against the journal; divergent replay can fail. Keep persistent state in providers, and do not rely on external side effects being exactly-once across load/rollback. Provider capture/restore/migrate callbacks must be deterministic and must not call engine operations.

`ctx.stage.animate(target, spec)` tweens a character, an image layer, `$bg`
or `$camera` (`x`, `y`, `scale`, `alpha`; `rotation` in degrees for characters
and image layers) over `duration` milliseconds with `ease`. `loop` plays that
many passes (`-1` until another tween replaces it) and `yoyo: true` runs every
other pass back, so `{rotation: 8, duration: 1000, loop: -1, yoyo: true}`
rocks a layer. Running tweens, their pass and direction are Stage state:
saves, loads and rollback resume them where they were.

Full-screen transition playback is transient: loading cancels it and restores the incoming Stage/UI state. Timeline playback has serialized state and await handling; this is not a promise to serialize GPU frames. Audio/video decoder positions and voice replay require the native journey tests for the specific boundary; do not promise sample-exact restoration merely because an asset and logical state are present. Test the specific restore boundaries your game depends on with the native journey tests.

Runtime extensions run in QuickJS, without Node filesystem/process/network modules or unrestricted time/random/eval APIs. Installing a Node package does not make it a runtime dependency. TSX authoring is evaluated during the build in Node and is a separate trust boundary: only build projects and dependencies you trust. Runtime sandbox limits do not make arbitrary build-time project code safe.

## Authoring input actions

Put named actions in the project's `Content/game.pxgame` document. This is
versioned JSON, but the `GameInputAction` TypeScript type can be used by
generators. Unlike `inputBindings` command/key pairs, one entry
declares the action identity, input context, default keyboard keys, wheel gesture, and an
optional Player route or UI command together:

```json
{
  "format": "PrismatiXGame",
  "schemaRevision": 1,
  "variables": [],
  "gallery": [],
  "unlockables": [],
  "inputActions": [
    {"id": "quick-save", "context": "game", "keys": ["F8"]},
    {"id": "screen.open.gallery", "context": "game", "keys": ["G"],
     "route": "gallery"},
    {"id": "backlog", "context": "game", "wheel": {"direction": "up", "threshold": 1}},
    {"id": "advance", "context": "game", "wheel": {"direction": "down"}},
    {"id": "photo-mode", "context": "game", "keys": ["P"]},
    {"id": "settings.keys", "context": "ui", "keys": ["F2"],
     "uiAction": {"command": "settings.page7"}}
  ]
}
```

An authored `uiAction` shortcut calls the same Player UI command handler
as a button, with an optional string `argument`. It is active only in the
declared input context (`ui` while in menus, `game` during gameplay), and is
suppressed during splash, preload, video, and full-screen CG presentation.
One action may have a `route` or a `uiAction`, not both. A misspelled command
cannot be checked against every host extension at build time; an unhandled
Player command emits `PXPLAYER5023` when pressed. Authors should still
exercise the resulting shortcut in a packaged Player test.

The Player registers each action in the native input system. The built-in
`quick-save` action is therefore remapped to F8 without a Player-specific
configuration branch. The built-in `save.open` action opens the save screen
from gameplay with F6 by default; unlike the former raw F6 check, it respects
input contexts, can be rebound, and returns to F6 on Reset. A `route` opens a
registered screen while gameplay is active; it requires `context: "game"`.
An action without a route or UI command can be read
by an extension or host using the named-action input API. `keys` may be
omitted: a new custom action starts unbound, while an engine built-in keeps
its engine default. Set `keys: []` explicitly to remove a built-in's default
keyboard keys without removing its gamepad or wheel binding. Saved user
keyboard and wheel choices override authored defaults. Reset restores the
authored keyboard defaults. Gamepad bindings are left intact.
The order in `keys` is preserved for UI captions and saved settings (for
example, `F2 / S` stays in that order rather than scancode order).
Wheel `direction` is `up`, `down`, or `none`; `none` disables an engine default.
The optional `threshold` is accumulated within one frame (0..100); omitting it
fires on any movement. The UI still receives its independent raw Scroll event.
The equivalent typed authoring example is
[input-actions.ts](examples/input-actions.ts).

The project compiler checks route names, duplicate action IDs, and collisions
with legacy `screen.open` bindings. Native builds export
`PrismatiXInputKeyRegistry.json` from `Input::KeyCode` next to the Runtime tools.
With `--runtime` (or a Packager selected for a fresh distribution build),
`prismatix validate` and `build` check both `inputActions[].keys` and legacy
`inputBindings[].key` against that table before Player starts. A misspelled key
is `PXBUILD1068`; a runtime without the manifest emits `PXCLI1043`, so it is
clear that only structural validation ran. Native builds that have the exporter
but lack a current manifest fail rather than silently skipping this check.
The manifest lists canonical SDL names and the historical `Enter` alias; use
one of those spellings for portable authored defaults. A distribution smoke
test is still needed to verify actual keyboard behavior and user remapping.

Runtime extensions can now start a one-shot physical key capture with
`ctx.input.beginKeyCapture()`, poll `takeCapturedKey()`, and pass the resulting
scancode to `rebindKey()`. The captured key is withheld from ordinary actions
and text input; `cancelKeyCapture()` should be called when the settings page
closes. See the [Runtime SDK example](../runtime-sdk/README.md) for conflict
handling. Escape is returned as an ordinary key so the author's UI decides
whether it means cancel or a binding.

For keyboard settings, the Authoring SDK also provides `KeyBindingButton`.
It uses the normal `TextureButton` skins and text styling, but displays the
current key(s) for `bindingAction`, captures the next physical key on click,
and reports a conflicting key in the caption without changing the binding.
Escape cancels by default; the prompt, unbound caption, conflict caption,
Escape behavior, and shared-binding policy are authorable props. Set
`deleteClearsBinding` to let Delete during capture clear only that action's
keyboard keys; gamepad and wheel bindings remain intact. It is opt-in so a
game that uses Delete as a normal shortcut can still bind it. Set
`addCapturedBinding` to append a captured key while keeping the existing
ordered keys instead of replacing them. Conflict checks apply to the new key;
32 keys per action is the persisted-map limit. With `selectableBindingKeys`,
the selected key appears in brackets. Page Up/Down changes the selection while
the button is focused; the pointer wheel does so while hovered. Delete removes
just the selected key while focused.
Capturing a new key replaces the selected key in place unless
`addCapturedBinding` is set. `confirmSharedBinding` holds a conflicting key for
four seconds and requires another activation before sharing it with a second
action. The confirmation caption is configurable with `bindingConfirmText`.
For example:

```tsx
import {KeyBindingButton, Scene} from "@prismatix/authoring-sdk";

export default <Scene name="Keys" width={800} height={600}>
  <KeyBindingButton name="PhotoModeKey" bindingAction="photo-mode"
    x={280} y={180} width={240} height={56}
    normalPath="Assets/UI/key.png" bindingPrompt="Press a key"
    selectableBindingKeys confirmSharedBinding
    bindingConfirmText="Key in use; press again to share" />
</Scene>;
```

`bindingAction` must name an action registered by the game or host. Player
provides the input service and saves a successful rebind to `Save/config.dat`
in its [per-game user-data directory](#packaged-player-user-data);
other hosts must attach `UIContext::SetInputBindingService(input, onChanged)`
and persist the updated keyboard or wheel bindings themselves. The Runtime SDK exposes
`ctx.input.addKeyBinding(action, key)` as the same atomic append operation;
it returns `limit-reached` if the save-compatible maximum is reached. This
button can append, replace one selected key, or replace the whole keyboard set.
Native `Input::ReplaceKeyBinding` replaces one key atomically while preserving
the other keys and their order. Native `Input::RemoveKeyBinding`
and Runtime SDK `ctx.input.removeKeyBinding(action, key)` remove one key while
keeping the others in order and leaving wheel/gamepad bindings intact. They
return `false` for an unknown action, invalid key, or key that is not bound.
Wheel bindings can also be edited at runtime:

```ts
ctx.input.setWheelBinding("backlog", "up", 1.5);
ctx.input.setWheelBinding("advance", "none");
const binding = ctx.input.wheelBinding("backlog");
```

For a stock settings control, use `WheelBindingButton`:

```tsx
import {Scene, WheelBindingButton} from "@prismatix/authoring-sdk";

export default <Scene name="Wheel Settings" width={800} height={600}>
  <WheelBindingButton name="BacklogWheel" bindingAction="backlog"
    x={280} y={180} width={240} height={56}
    normalPath="Assets/UI/wheel.png" wheelThreshold={1.5}
    wheelUpText="Scroll up" wheelDownText="Scroll down"
    deleteClearsBinding />
</Scene>;
```

Clicking the control captures the next physical scroll direction. That gesture
does not activate the old action or scroll the underlying UI. Escape cancels
capture by default; clicking again also cancels. With `deleteClearsBinding`,
Delete during capture clears only the wheel binding. `wheelThreshold` accepts
0–100 and defaults to zero, which triggers on any nonzero movement. The button
displays the bound direction and asks the host to persist successful changes.

`wheelBinding` returns `null` for an unknown action. `setWheelBinding` returns
`false` for an unknown action or invalid threshold. Player saves these choices
alongside keyboard bindings in per-user `config.dat` and restores them before
game actions register. A native host using `GameSettings` can wire
both devices to its own settings path:

```cpp
px::Input input;
px::progress::GameSettings settings;
const auto load = settings.LoadWithInputBindings(configPath, input, &saveKey);
const bool settingsWritable =
    load != px::progress::PersistLoadStatus::Failed;
uiContext.SetInputBindingService(&input, [&] {
    if (settingsWritable)
        (void)settings.SaveWithInputBindings(configPath, input, &saveKey);
});
```

`configPath` is the host's per-user settings path, and `saveKey` is its
persistence key. The callback runs after a successful UI rebind. Register
authored actions and defaults before loading when possible; saved actions
registered later still receive their overrides. A missing file leaves defaults
active on a fresh Input. An invalid file changes neither settings nor input.
Keep writes disabled so the host cannot overwrite the user's bytes. Hosts with a
different store can use `CaptureInputBindings` and `ApplyInputBindings` directly.

Project compilation checks `bindingAction` in UI scenes and components. An
empty or non-string value is an error (`PXBUILD1071`). A name absent from both
`game.inputActions` and the built-in Input actions produces `PXBUILD1072`
with the UI node location; it is a warning because a custom host may register
the action later. A `wheelThreshold` outside 0–100, or one that is not a finite
number, is an error (`PXBUILD1073`) even when no native UI registry is selected.
For Player projects, treat the unknown-action warning as a likely typo and
exercise the binding in a packaged Player before release.

## Layered portrait authoring

`.pxportrait` is a public, versioned resource contract for compositing shared
transparent texture layers. Reuse the same body and clothing PNGs across face
variants instead of exporting a complete image for each expression.

```ts
import {serializeLayeredPortrait} from "@prismatix/authoring-sdk";

const authored = serializeLayeredPortrait({
  width: 691,
  height: 1450,
  layers: [
    {path: "Assets/Characters/Layers/body.png", width: 691, height: 1450,
      role: "base"},
    {path: "Assets/Characters/Layers/face-smile.png", x: 302, y: 153,
      width: 174, height: 74, opacity: 217, role: "expression"},
  ],
});
if (!authored.valid) throw new Error(JSON.stringify(authored.diagnostics));
// Write authored.value as UTF-8 to Assets/Characters/Smile.pxportrait.
```

Layers are drawn in array order on the portrait's unscaled canvas. `x` and
`y` default to zero; `opacity` defaults to 255. Width and height are required
for the canvas and every layer, and must be positive. Paths are safe
project-relative texture paths; a layer cannot reference another `.pxportrait`.
`defineLayeredPortrait` returns the typed validated document when an importer
needs to inspect or transform it before serialization.

Mark every layer `base` or
`expression`: `base` includes body, dress, pose, and overlays that must not
change during a facial transition. A partial role table is rejected.

Reference the saved descriptor in Story just like a sprite:

```pxstory
[show character=hero asset=Assets/Characters/Smile.pxportrait position=center transition=auto]
```

With `transition=auto`, a new actor uses the ordinary entrance fade. An
existing actor with the same portrait simply updates position; a replacement
crossfades only if both role-tagged descriptors have the same canvas, layer
count, roles, and all `base` layers are identical while at least one
`expression` layer differs. Otherwise it switches immediately. `true` and
`false` retain their explicit behavior, including on portraits without roles. This makes a changed outfit or pose safe without per-story-line
transition flags; it does not infer semantic roles from filenames.

### Blinking and lip sync

An `expression` layer can carry `blink` and `mouth` frame lists. Each frame
is a texture drawn over the expression, in canvas coordinates (`x`/`y`
default to the layer's own):

```ts
{path: "Assets/Characters/Layers/face-smile.png", x: 302, y: 153,
  width: 174, height: 74, role: "expression",
  blink: [
    {path: "Assets/Characters/Layers/eyes-half.png", width: 174, height: 40},
    {path: "Assets/Characters/Layers/eyes-shut.png", width: 174, height: 40},
  ],
  mouth: [
    {path: "Assets/Characters/Layers/mouth-ajar.png", x: 372, y: 205, width: 34, height: 18},
    {path: "Assets/Characters/Layers/mouth-open.png", x: 372, y: 205, width: 34, height: 18},
  ]}
```

- **Blink** frames go from half-closed to closed. Every 2.5–6 seconds (at
  random, per character) the Stage plays them in order and back, 50 ms each.
- **Mouth** frames go from slightly to fully open. While a line's voice
  plays, the character named by the line (`hero: ...` or `char=`) opens its
  mouth with the voice's loudness; quiet passages show the expression's own
  closed mouth. Other characters on stage keep their mouths shut.

Frames are drawn only on the expression currently shown (an expression that
is fading out keeps its still face), stop while skipping, and are not saved:
a loaded game simply starts a new blink timer. Frames need the layer to be
an `expression` layer (`PXSDKSEM1351`), and their textures are packaged with
the descriptor.

When a character's expressions are role-tagged descriptors, `[face alias
e0023]` changes only the face: the compiler finds, for each set of `base`
layers, the expression whose `expression` layer file is named `e0023`, and
the Player picks the one matching the body on stage. See the Story
cookbook's "Changing only the face of a layered character".

The Authoring project compiler validates reachable descriptors and includes
their layer textures from Story, UI action arguments, locale resources,
explicit includes, or asset-catalog references. A UI `TextureRect` can use a
`.pxportrait` as its `path` (including JSX `<Image path="...pxportrait" />`)
or use an `assetId` / `texture` resource reference to the descriptor.
It fits the portrait canvas into the control and draws ordered layers with
their authored opacity; `scaleMode` and alignment work as for ordinary images.
The Player and preview hosts load the descriptor through their UI text asset
loader and preload its layer textures before showing a new scene. Dynamic
`path` changes also resolve the descriptor, retaining the previous image if
the new layer textures are still loading. Missing or malformed descriptors
fail before packaging or UI installation.

Direct `compileProject` callers must provide the descriptor text in
`sourceFiles` or through `resourceText`; the CLI reads it from the project
directory. This format does not yet offer an Editor inspector, per-layer
animation, or dynamic dress/pose/face selection within a single descriptor.

## Authoring layered motion

For a simple camera pan or zoom, write it directly in Story:

```text
[camera x=120 y=-30 zoom=1.2 duration=800ms ease=outCubic]
[camera x=0 y=0 zoom=1 duration=600ms]
```

Coordinates and zoom are absolute; omitted values retain their current value.
The default duration is 600 ms, and `wait=true` is the default so the next
Story operation starts after the move. Use `duration=0` to snap immediately or
`wait=false` to let the next operation run while the camera moves. A `.pxmotion`
resource is useful for composed, multi-track motion.

`.pxmotion` is a UTF-8 JSON resource, not a TypeScript program. It contains a
logical canvas, image layers, and keyed tracks. TypeScript authors can use
`defineMotion` or `serializeMotion` from `@prismatix/authoring-sdk` to create and
validate that JSON; a TSX UI references the saved resource with `<Motion>`.

The contract is `schemaRevision` 1. It supports parented image layers and keyed
position, scale, rotation, opacity, and visibility. Atlas frames are pixel
rectangles in one layer image selected by integer, `step`-curve `frame` keys.
Timed `se`/`voice` audio markers fire when ordinary playback crosses them.
Each layer may declare `blendMode`: `normal` (default), `additive`, or
`multiply`. A key may use a `cubicBezier` timing curve for numeric tracks; its
control points specify normalized time/value coordinates.
A `mask` on an image layer names another animated layer whose sampled alpha
clips the content layer; the source layer is not painted by itself. Missing,
self-referential and nested mask references are rejected. A mask requires
normal blending on both the content and mask source; combining a mask
with additive/multiply is rejected instead of producing a misleading image.
Non-drawing `kind: "group"` layers and per-layer `timeOffset` / `timeScale`
give a child local time: it samples its tracks at `parentLocalTime * timeScale +
timeOffset`; its transform and opacity still inherit from the parent. Group
layers need no image or dimensions and create no packaged texture dependency.
Track key times use the target layer's local seconds and may exceed the outer
clip's `duration` (up to 86,400 seconds). Audio markers always use the outer
clip clock and remain within its duration.
Multiplicative RGB `tint` applies on image or group layers and as a
three-channel `tint` track. A group's tint is multiplied into its descendants,
independently of `opacity`.
Per-layer `inheritRotation` and `inheritScale` switches, both
defaulting to `true`, let a child keep the parent's affine translation while
dropping its authored rotation, scale/stretch (including reflection and shear),
or both. The sampler tracks authored rotation separately so a negative scale
does not become a fake 180-degree angle. `inlineMotion` accepts the same
switches for its inserted group.
An explicit `tickRate` (ticks per second, 0.001-1000) is optional. When it is
present, `duration`, layer `timeOffset`, track-key `time`, and audio-marker
`time` are all authored in ticks and the native loader converts them to seconds
before playback; without it they are seconds. Converted duration and local
key/offset limits remain one day. This avoids independent, error-prone division
of every timestamp when importing a source timeline. It does **not** infer a
source format's tick rate or validate that source animation semantics match.
Image-layer `anchorX` and `anchorY` extend from -16 to 16
image-size units, including a pivot outside the image rectangle.
An external source's origin coordinates still require a verified mapping to
these normalized anchors before claiming a faithful import.

In TypeScript, `MotionTrack` is property-discriminated: `visible` keys take
booleans, `tint` keys take `{r, g, b}`, and position/scale/rotation/opacity/
atlas-frame keys take numbers. `visible` and `frame` keys only allow `step`;
`cubicBezier` keys require control points, which other curves reject. The
editor and `tsc` catch these structural mistakes before building the game.
Runtime schema and semantic checks still validate external JSON, numeric
ranges, frame indices, key times, and layer references.

```ts
import {writeFile} from "node:fs/promises";
import {serializeMotion} from "@prismatix/authoring-sdk";

const motion = serializeMotion({
  id: "logo-reveal",
  canvas: {width: 1280, height: 720},
  duration: 1,
  layers: [{
    id: "logo",
    image: "Assets/UI/logo-atlas.png",
    blendMode: "additive",
    x: 480, y: 300, width: 320, height: 120,
    frames: [
      {x: 0, y: 0, width: 320, height: 120},
      {x: 320, y: 0, width: 320, height: 120},
    ],
  }],
  tracks: [{
    layer: "logo", property: "frame",
    keys: [{time: 0, value: 0}, {time: 0.5, value: 1}],
  }],
  markers: [{time: 0.5,
    audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
if (!motion.valid) throw new Error(JSON.stringify(motion.diagnostics));
await writeFile("Content/Motion/logo-reveal.pxmotion", motion.value!, "utf8");
```

For a source whose rate is independently verified as 60 ticks per second,
the same authoring API can keep its original time coordinates:

```ts
const tickMotion = serializeMotion({
  id: "tick-reveal", canvas: {width: 1280, height: 720},
  tickRate: 60, duration: 90, // 1.5 seconds
  layers: [{id: "logo", image: "Assets/UI/logo.png",
    width: 320, height: 120, timeOffset: -15}],
  tracks: [{layer: "logo", property: "opacity", keys: [
    {time: 0, value: 0}, {time: 60, value: 1},
  ]}],
  markers: [{time: 30, audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
```

`inlineMotion` accepts clips with different `tickRate` values and checks each
original document first. It keeps the **outer** clip's tick rate in the
composed result, so repeated nesting does not require a new unit conversion;
seconds-based outer clips remain seconds-based. Its `at` option is always in
the parent's local **seconds**; `atTicks` is an alternative in the outer
clip's authored ticks and requires `tickRate`. Supplying both is an error.
`speed` is unitless. The M2 `.mtn` tick rate has not yet been verified here,
so the value 60 above is an example, not a claim about M2.

For example, `anchorY: -1 / 3` places the pivot one pixel above a three-pixel-
high image; the layer's `x` and `y` specify where that pivot is placed. This
describes the `.pxmotion` transform only, not a conversion rule for `.mtn`.
`withPixelPivot` performs that geometry conversion when an author knows the
intended pivot in image pixels, and rejects invalid dimensions or conflicting
normalized anchors:

```ts
import {withPixelPivot} from "@prismatix/authoring-sdk";

const glyph = withPixelPivot({
  id: "glyph", image: "Assets/Motion/glyph.png",
  width: 16, height: 3, x: 500, y: 300,
}, 8, -1);
// glyph.anchorX === 0.5; glyph.anchorY === -1 / 3
```

The helper is authoring-only: saved `.pxmotion` retains canonical normalized
anchors. It does not know whether another format's `origin` uses this pivot
convention.

```tsx
import {Motion, Scene} from "@prismatix/authoring-sdk";

export default <Scene name="Title" width={1280} height={720}>
  <Motion name="Logo" path="Content/Motion/logo-reveal.pxmotion"
    position={[0, 0]} size={[1280, 720]} autoplay />
</Scene>;
```

The Authoring SDK inserts an invisible, full-scene structural root when the
scene contains a leaf or multiple top-level children. An explicit `<Group>`
still works and is preserved as authored.

`canvas` and layer `x`/`y`/`width`/`height` use logical UI coordinates;
`frames` use pixels of the source image. A `frame` track is discrete, so
interpolating curves and indices outside the frame array are rejected. Before
the first key, frame 0 is selected. The image is packaged and preloaded once,
not reloaded at every key. Keep rectangles inside the actual atlas dimensions:
the contract bounds coordinates but cannot infer the image's size; the
renderer clips an out-of-image region.

Audio marker IDs are unique within a clip; times must be ordered and fall
between zero and `duration`. `bus` is `se` or `voice`. `compileProject`
includes marker audio in the package dependency graph, while visual preloading
still requests only layer images. Normal playback emits each crossed marker in
order, including on loops. An explicit `Seek` does not play skipped audio; an
extremely large frame jump has a bounded event budget rather than replaying an
unbounded backlog. A UI `<Motion>` and splash `MotionRect` dispatch these events
through the Player audio service. The TypeScript helper generates marker IDs
when omitted; specify an ID only when a tool or editor needs a stable name.
Native hosts register one `UIContext::SetMotionAudioSink` callback, rather than
walking every `MotionRect` to attach a callback.

Blend modes are scoped per draw, so two layers can share one atlas without
leaking the previous layer's mode. `additive` adds source light to what is
already drawn; `multiply` darkens the overlap. Neither mode is an alpha mask.

For an animated reveal, author a normal image layer and a separate mask image
layer. Key the mask's transform or opacity on the same timeline:

```ts
layers: [
  {id: "logo", image: "Assets/Motion/logo.png", width: 640, height: 220,
    mask: "reveal"},
  {id: "reveal", image: "Assets/Motion/reveal-alpha.png", width: 640,
    height: 220},
],
tracks: [{layer: "reveal", property: "x", keys: [
  {time: 0, value: -640}, {time: 1, value: 0},
]}],
```

Mask images use their alpha channel, not brightness. Their transform, atlas
frame, inherited visibility and opacity are sampled with the content at the
same seek position. Both image files are validated as package dependencies.
The renderer reuses two logical-size offscreen targets per Renderer2D; it does
not allocate targets for every frame, but each masked draw adds offscreen
passes. The SDL GPU (D3D12) and basic (D3D11) framebuffer tests cover opaque,
transparent, half-alpha and moving masks. Software and WASM parity, mask
performance in the full game, and M2-specific frame matching are not yet
verified.

To place an animated subpart under a parent transform and delay its own
timeline, write a group and parent an image layer to it:

```ts
layers: [
  {id: "logo-group", kind: "group", x: 320, y: 180,
    timeOffset: -0.25, timeScale: 2},
  {id: "letter", parent: "logo-group", image: "Assets/Motion/letter.png",
    width: 120, height: 80},
],
tracks: [{layer: "letter", property: "opacity", keys: [
  {time: 0, value: 0}, {time: 0.5, value: 1},
]}],
```

At the clip's 0.5-second mark, `letter` is sampled at local time 0.75. A
group can also have transform tracks. This represents an inline nested motion
hierarchy, not a reference to another `.pxmotion` resource; an importer must
flatten external `.mtn` subclips and diagnose unsupported source fields.

For offline composition, `inlineMotion` performs the ID, parent, mask, track,
and audio-marker rebasing and adds a transform group for you:

```ts
import {inlineMotion, serializeMotion} from "@prismatix/authoring-sdk";

const combined = inlineMotion(outerMotion, letterMotion, {
  id: "letter-intro", parent: "logo-group", at: 0.25, speed: 2,
  x: 120, y: 30,
});
if (!combined.valid) throw new Error(JSON.stringify(combined.diagnostics));
const json = serializeMotion(combined.value!);
```

`outerMotion` may start with `layers: []`; the final combined resource is still
validated as a normal motion document. The child must be non-looping. It is
hidden before `at`, begins at child-local time zero, and holds its final pose
after its last key. `at` uses the named parent's local clock; audio markers
are converted to the outer clock. The helper rejects a clip that would start
before or finish after the outer duration, since that would otherwise silently
truncate animation. Set `allowTruncate: true` only when intentional; markers
outside the outer interval are then discarded, and a clip wholly outside is
still rejected. The child canvas stays in its original logical units;
explicitly scale it when embedding a different-sized canvas. This helper does
not read or convert `.mtn`, resolve external motion references, or imply M2
visual equivalence.

For a non-linear key interval, put the timing curve on the first key:

```ts
{time: 0, value: 0, curve: "cubicBezier",
  bezier: {x1: 0.1, y1: 0.8, x2: 0.4, y2: 0.9}}
```

The next key supplies the interval's destination value. `x1` and `x2` must
be in `[0, 1]`; `y1` and `y2` may be in `[-8, 8]` to permit overshoot. Boolean
visibility and atlas-frame tracks remain discrete `step` tracks. The sampler
solves the timing cubic for each seek, so playback and preview scrubbing use
the same pose. This supports a class of `.mtn` timing curves, but does not
interpret `.mtn` files automatically.

An animated source color can be expressed without splitting it into three
separate tracks:

```ts
layers: [
  {id: "group", kind: "group", tint: {r: 1, g: 0.5, b: 1}},
  {id: "logo", parent: "group", image: "Assets/Motion/logo.png",
    width: 320, height: 120},
],
tracks: [{layer: "logo", property: "tint", keys: [
  {time: 0, value: {r: 1, g: 1, b: 1}},
  {time: 1, value: {r: 1, g: 0, b: 0}},
]}],
```

Each channel is in `[0, 1]`. Values interpolate with the numeric-track
easing curves, with overshoot clamped to valid channels. This covers ordinary
RGB modulation, not M2's entire color-transform or blur semantics.

For a sublayer that should follow a rotating parent's position and zoom but
not its angle, set `inheritRotation: false`:

```ts
layers: [
  {id: "parent", kind: "group", x: 320, y: 180,
    rotation: 90, scaleX: 2, scaleY: 2},
  {id: "letter", parent: "parent", image: "Assets/Motion/letter.png",
    width: 80, height: 80, x: 40, inheritRotation: false},
],
```

This is an Engine-authored inheritance rule, not a proven mapping of every
FreeMote `inheritMask` flag. Source angle, zoom, flip, stencil, and mesh
semantics still require a per-frame comparison before importing M2 clips.

This is not an `.mtn` importer or a full M2 Cheeseware reproduction. Blur,
external nested motion references, spatial path curves, particles, more
specialized blend equations, and Story playback are still missing;
the source work on those features is tracked in
[layered motion architecture](#layered-motion-authoring-proposal).
The decoded M2 source groups named `bure` and `bure2` are indexed images,
not evidence of a shader blur command. Per-layer blur remains a general
capability gap, but M2-specific necessity must be established by source
semantics and rendered-frame comparison rather than those names.

Story's existing `[timeline Content/Motion/intro.pxanim await=true speed=1.25]`
command now exposes the Runtime's wait and playback-rate controls, so a scene
can pause until a `.pxanim` finishes without a JavaScript workaround. This
command plays `.pxanim`/`.pxtimeline` animation assets. For a full-canvas
`.pxmotion` composition in the Stage, use:

```text
[motion Content/Motion/intro.pxmotion await=true speed=1.25]
[motion.stop]
```

Stage motion appears above Stage actors and below dialogue/UI. The Runtime
checks that the descriptor and referenced images/audio exist, preloads images
before advancing, and plays audio markers on the SE or voice bus. `await=true`
pauses Story until a non-looping motion completes; awaiting a looping motion is
rejected. Starting another motion replaces the current composition, and
`[motion.stop]` removes it. Save/load restores the descriptor, playback speed,
sampled position and pending wait without replaying earlier markers. TSX
`<Motion>` remains available for motion inside UI layouts.

## Layered motion authoring proposal

Status: image-layer, timing, blend, alpha-mask, and inline nested-transform slices plus TSX UI mounting implemented; advanced composition
and Story playback below remain a proposal. This is not an `.mtn` parser or a claim that the
original M2/petal animations are reproduced.

The versioned `PrismatiXMotion` revision-1 `.pxmotion` contract has an
Authoring SDK `defineMotion`/`serializeMotion` helper, matching native parser,
unique layer/track and hierarchy checks, path checks, deterministic keyed
sampling, and a `Renderer2D` image-layer drawing adapter. A `MotionRect`
control can now play the descriptor in TSX-authored UI, splash scenes,
Player routes and native/WASM previews. `MotionSampler::Seek`
reuses its scratch storage and binary-searches ordered keys; its parent-world
transform and inherited opacity are sampled deterministically, with
child-local time offsets and scales. `fit` is the default
canvas scaling mode to avoid image squashing. Native and TypeScript contract
tests cover this slice. `compileProject` validates reachable descriptors
and collects their image dependencies. Splash playback now resolves static UI
image paths (including `.pxmotion` layers), queues a texture preload batch for
the following page while the current page is visible, and waits before exit
until that batch finishes. The Player also queues the configured start route's
static UI images during boot and holds the final splash until its batch is
ready. This avoids presenting a partially decoded next splash/title page,
but is not a general UI/Story preload handshake. Player routes now also gather
mounted UI image paths before fading in, and bound image paths are queued again
after each UI update; `TextureRect` retains its prior resident image until a
replacement is ready. Arbitrary custom drawing, Story playback dependencies,
texture residency under budget pressure, and M2-specific framebuffer
conformance remain open. Story motion commands,
masks beyond one alpha-mask source per content layer, blur, specialized blend equations, save/load playback binding, and
M2 conformance fixtures have not been added. The renderer adapter has not yet
passed an M2-specific framebuffer comparison. Layers carry
per-layer atlas rectangles and discrete `frame` keys. The native sampler
selects a source rectangle without changing textures at each key, and a
two-colour atlas framebuffer test checks the rendered output.
Ordered `se`/`voice` audio markers: the Authoring SDK validates
them and includes their audio paths in package dependencies, and `MotionRect`
emits crossed events during normal playback to the Player audio service (also
for splash scenes). Seeking does not replay skipped cues. Script markers,
Story integration, and audio-predecode scheduling remain open.
Layers have local normal/additive/multiply blending; a framebuffer test draws
overlapping layers from one atlas and verifies both additive and multiply
pixels. This is not an alpha-mask implementation or M2 conformance fixture.
Numeric property tracks accept bounded cubic Bézier timing controls, and
native `Seek` solves the timing cubic deterministically.
The original `.mtn` custom timing curves still require semantic comparison
before a converter may claim equivalence.
A layer may carry a `mask` reference. The referenced layer is sampled on
the same clock and rendered only into reusable alpha coverage targets; its
alpha multiplies the content layer. SDK and native validation reject missing,
self-referential, nested or non-normal-blend masks. GPU D3D12 and basic D3D11
framebuffer tests check full, empty, half-alpha, and moving coverage. This is
not an M2 conformance result: external nested clip references, blur, particles, and
source-specific blend behavior still block a faithful converter.
Non-drawing transform groups and child-local time remapping are part of the contract.
The native sampler computes
each layer's local time in hierarchy order before sampling property tracks.
The GPU framebuffer test verifies that a delayed child moves at the expected
time without painting its group, and the project compiler omits groups from
image dependencies. This allows an offline `.mtn` importer to flatten some
referenced submotions under groups, but does not interpret external references,
spatial paths, particles, mesh deformations, or source-specific mask flags.
`inlineMotion` now performs that offline rebasing for already-authored
non-looping clips, including mask references and audio-marker times. Track
keys are bounded by the local clock's 86,400-second limit instead of the
outer duration; audio markers remain on the outer clock. This is composition
of known `.pxmotion` inputs, not an `.mtn` parser or conformance result.
The helper also accepts an empty outer layer list as a composition starting
point and rejects temporal overflow unless truncation is explicitly requested.
A three-channel animated RGB tint and inherited modulation
through transform groups. Its native sampler and framebuffer test verify that
color changes reach the pixels without changing alpha. This maps the ordinary
color modulation seen in M2's `m2logo` source, but not its source-specific
mask flags, blur, embedded-motion extraction, or complete color equations.
In the decoded M2 source, `src/bure/icon1` and `src/bure2/icon1` resolve to
280×112 indexed images. Their names alone do not imply a runtime blur effect;
the need for per-layer blur in this particular clip is unproven until the
source semantics and rendered frames are compared.
A child may retain the parent's affine translation while opting
out of inherited rotation and/or scale/stretch. Native sampling tests cover
all four combinations and reflected parents. This fills an Engine capability
gap revealed by nonuniform `inheritMask` values in the Senren M2 source; the
bitfield-to-rule mapping and rendered-frame equivalence are still unproven.
An optional explicit ticks-per-second rate applies to authored duration,
layer offsets, track keys, and audio markers. Native playback normalizes all
four to seconds; `inlineMotion` can compose clips with different rates after
validating each source, while retaining the outer rate in the result. It also
accepts `atTicks` for a nested start without manual division. The M2 source
rate is still unverified, and this does not resolve source-specific paths or
composition rules.
Normalized layer anchors may lie outside the image rectangle,
bounded to [-16, 16] on each axis; a clip uses either seconds or an explicit
tick clock. This gives an
importer a way to represent external image pivots, but source-origin mapping
and rendered-frame equivalence still require separate validation.

Sampling uses a layer's authored property until its first key;
thereafter each key's curve determines interpolation toward the next key.
Visibility keys are discrete. Layers with the same `z` paint in authored order.

### Existing foundation and the gap

`PrismatiXTimeline` has typed tracks, easing, markers and nested clips;
`AnimationClip` and `TimelinePlayer` provide deterministic sampling. The
implemented `.pxmotion` resource now binds image/group layers to tracks,
atlas frames, alpha masks, tint, blend modes, audio markers and dependencies;
TSX `<Motion>` can display it. The port's M2 animation still uses a substitute
UI animation. Missing work includes source-specific `.mtn` conversion with
rendered-frame conformance, richer curve/compositing semantics, and direct
Story/Stage playback with preloading and save/load behavior.

### Target `.pxmotion` document

The file is a reusable composition, currently referenceable from a UI node
or splash; transition and Story presentation remain targets. Authored time is in
seconds, or in ticks when the document declares a tick rate. Native playback uses seconds. Coordinates are in a declared logical
canvas, independent of window scaling.

- `canvas`: width, height, scale mode and transparent background.
- `layers`: stable IDs, z-order, image/atlas resource, anchor, transform,
  opacity, tint, optional blend mode and optional mask layer. A layer can be a
  nested composition. Every referenced asset is a packager dependency.
- `tracks`: target layer/property plus typed keyframes and curve. Properties
  include position, scale, rotation, opacity, source rectangle/frame index,
  mask progress, tint and blend weight. Discrete properties use `step`.
- `markers`: time-stamped sound/voice/script events with once-per-iteration
  semantics. Markers do not execute during a seek unless requested.
- `playback`: duration, loop/ping-pong, speed, pre-roll and named exit point.
- `reducedMotion`: author-supplied still frame or abbreviated clip.

For example, a petal is one transparent texture layer with a four-second
loop: position follows a keyed Bezier path, rotation increases, opacity fades
at both ends, and scale varies slightly. M2-style logo reveals can combine an
atlas frame track with an animated mask and a short sound marker. Neither
example requires importing or emulating `.mtn` itself.

### Runtime and SDK contract

1. Authoring SDK validates layer IDs, property types, time ordering, atlas
   frame bounds, mask cycles and resource paths; compiler emits an immutable
   motion graph and recursively lists dependencies.
2. Renderer evaluates every visible layer at one shared clock sample, then
   composites to an off-screen target. Mask and blend semantics are explicit;
   no first-frame flash while decoding or switching scenes.
3. Playback state (clip ID, time, iteration, marker cursor) is serializable for
   save/load and preview scrubbing. Multiple instances share assets but not
   clocks. Asset preloading completes before a transition starts.
4. UI/Story expose `play`, `stop`, `await` and `crossfade` without a game-specific
   Player switch. Input-driven skips land on the authored exit state and fire
   only markers declared `onSkip`.

### Incremental implementation and acceptance

The image-layer/transform sampling primitive, atlas frame track, audio markers,
descriptor dependency packaging and TSX UI mounting are in place; Story mounting
and durable playback remain necessary before authors can use it end-to-end.
Basic/GPU framebuffer coverage now exists for normal/additive/multiply blending
and one alpha-mask source; software/WASM parity and M2 source-image conformance
remain open. The Senren test fixtures should capture M2 and the HUD petal
at fixed timestamps (0, 0.25, 0.5, 1.0 seconds), compare composition geometry,
verify no blank transition frame, and exercise skip, reduced motion, save/load
and non-16:9 scaling. Until those tests pass, the port must label its M2 and
petal effects as approximations.

## Sprite lettering without one control per glyph

`SpriteText` displays short text using a declared set of image glyphs. It is a
typed TSX UI control; no separate descriptor format is required.

```tsx
import {Scene, SpriteText} from "@prismatix/authoring-sdk";

const digits = [
  {character: "1", path: "Assets/UI/digit-1.png",
   x: 0, y: 0, width: 18, height: 30},
  {character: "-", path: "Assets/UI/dash.png",
   x: 0, y: 8, width: 19, height: 10},
];

export default <Scene name="Chapter" width={420} height={143}>
  <SpriteText name="ChapterNumber" glyphs={digits} advance={24}
    size={[420, 143]} bind={{text: "chapter.number"}} />
</Scene>;
```

Each `character` is one Unicode grapheme. `x` and `y` are offsets within the
control, and `width`/`height` are the rendered dimensions. `advance` moves the
next glyph's origin by a fixed logical-pixel distance; zero uses each glyph's
width. Missing characters are left blank. Playback renders at most 64 graphemes
per control, so this is for badges, counters and decorative labels, not long
dialogue or general text shaping.

Authoring validates unique glyphs, bounds and safe image paths. Project builds
package every glyph image, even if it is not in the initial text. Native UI
preloading likewise includes the full glyph set before showing the page.
`text` is bindable and can change without rebuilding the UI tree. Glyphs are
separate image resources; atlas source rectangles and kerning are not yet part
of this control.

## Data-driven placed UI items

`ItemsGrid` and `ItemsList` are for regular layouts. When one authored item
template must appear at irregular source-defined coordinates, use
`ItemsPlaced`. It retains the same stable-key, `item.<field>` binding, and
`$index` / `$key` Action behavior without generating a separate authored node
for every row.

```tsx
import {Control, ItemsPlaced, Scene, TextureButton} from "@prismatix/authoring-sdk";

export default <Scene name="Settings" width={1920} height={1080}>
  <Control name="Root" fill>
    <ItemsPlaced name="ConfirmationRows" x={950} y={200}
      width={500} height={700}
      items={{path: "settings.confirmations", key: "key", placement: "rect"}}>
      <Control name="ConfirmationTemplate" width={388} height={35}>
        <TextureButton name="On" x={0} y={0} width={184} height={34}
          normalPath="Assets/UI/on.png" />
        <TextureButton name="Off" x={204} y={0} width={184} height={34}
          normalPath="Assets/UI/off.png" />
      </Control>
    </ItemsPlaced>
  </Control>
</Scene>;
```

For a packaged Player, define initial UI data in the project manifest's
`settings["player.uiData"]` map. The key is the same ViewModel path used in
`items.path`; values may be arrays, objects, or scalar JSON values. The Player
registers them before mounting the first UI route. Other hosts can register
equivalent values with `GalgameUI::DefineUiData`.

```json
{
  "settings": {
    "player.uiData": {
      "settings.confirmations": [
        {"key":"confirm-voice","rect":{"x":0,"y":0,"width":388,"height":35}},
        {"key":"confirm-exit","rect":{"x":0,"y":48,"width":388,"height":35}}
      ]
    }
  }
}
```

Each item needs a unique nonempty string or integer `key`
and a `rect` object with finite numeric `x`, `y`, `width`, and `height`.
Coordinates are local to `ItemsPlaced`; width and height must be positive.
When the item's source path resolves to an initial array declared in
`player.uiData`, the SDK checks every stable key and placement, and every `item.<field>`
binding before packaging. With a selected native UI TypeRegistry it also
checks binding output types, including the built-in `Not`, `Percent`, and
`BoolToVisibility` formatters. Paths and custom formatters supplied only by a
runtime host can be declared in `settings["authoring.uiContract"]` so the SDK
can check them before packaging:

```json
{
  "settings": {
    "authoring.uiContract": {
      "format": "PrismatiXUiContract",
      "schemaRevision": 1,
      "roots": ["settings.confirmations"],
      "values": {
        "settings.confirmations": "array",
        "settings.confirmations[].key": "string",
        "settings.confirmations[].rect.x": "number",
        "settings.confirmations[].rect.y": "number",
        "settings.confirmations[].rect.width": "number",
        "settings.confirmations[].rect.height": "number",
        "settings.confirmations[].caption": "string"
      },
      "formatters": {
        "CaptionCase": {"input": "string", "output": "string"}
      }
    }
  }
}
```

`roots` lists the exact ViewModel paths the host registers, and each root
must have a type in `values`. Other `values` entries declare fields beneath
those roots. Use `[]`
for an array item's type and the item's nested fields; the UI binding itself
continues to use `item.caption`. Declared item sources must be arrays, stable
keys must be strings or integers, and an `ItemsPlaced` rectangle needs numeric
`x`, `y`, `width`, and `height` declarations. A declared ViewModel root with a
missing binding path or undeclared custom formatter fails validation. The SDK
checks formatter input and output against the selected native UI TypeRegistry.
The host must register matching runtime ViewModel values and formatter
implementations. `GalgameUI::RegisterUiFormatter` exposes custom formatter
registration, and `GalgameUI::ValidateUiContract` checks registered roots,
current values (including populated array items), and formatter signatures.
Stock Player runs this check when it loads `project.pxproject`, after defining
`player.uiData` and before mounting an authored route. The check retains a
successful contract. A host can then call `GalgameUI::UpdateUiData(path, value)`
for later changes, including an array that was empty at boot. It validates
declared item fields in a candidate ViewModel before notifying mounted UI;
a contract-invalid update keeps the previous value. A declared root must be registered
as an exact ViewModel property. Other hosts should run the check after
registering their own data and formatters, then use `UpdateUiData` for changes.
Paths outside declared roots remain the host's responsibility.

For a non-Player host, register values and formatters before validating the
authored contract, and validate before mounting routes. This example uses the
`settings.confirmations` contract above and assumes `project` is the parsed
`project.pxproject` JSON:

```cpp
px::Status PrepareAuthoredUi(px::ui::GalgameUI& ui,
                             const nlohmann::json& project) {
    auto status = ui.DefineUiData(
        "settings.confirmations", px::Variant(px::VariantArray{}));
    if (!status) return status;
    status = ui.RegisterUiFormatter({
        "CaptionCase", px::VariantType::String, px::VariantType::String,
        [](const px::Variant& value) -> px::Result<px::Variant> {
            // Replace this pass-through body with the host's caption format.
            return px::Result<px::Variant>::Success(value.Clone());
        }});
    if (!status) return status;
    return ui.ValidateUiContract(
        project.at("settings").at("authoring.uiContract").dump());
}

px::Status RefreshConfirmations(px::ui::GalgameUI& ui,
                                px::VariantArray rows) {
    return ui.UpdateUiData("settings.confirmations",
                           px::Variant(std::move(rows)));
}
```

The host supplies each row's declared `key`, `rect`, and `caption` fields.
`UpdateUiData` checks a candidate value before notifying mounted UI, so a
bad late row does not replace the last valid array. The host must also
register its Action commands and their handlers before an authored screen
uses them; the UI contract above describes values and formatter types, not
Action implementations. For typed host Actions, see the
[host Action authoring contract](#host-action-authoring-contract).

Resource paths supplied only through dynamic item bindings still need explicit
asset inclusion (for example `resourceIncludes`) until the compiler can infer
those runtime dependencies.

The view realizes every item; unlike `ItemsGrid` and `ItemsList`, it does not
scroll or virtualize a long collection. Reordering an array or changing a
rectangle retains the control identity for an unchanged key. An invalid
update leaves the previous visible item snapshot in place and emits a
diagnostic. As with the regular repeaters, one structural child is the item
template, nested item sources are not supported, and item bindings must use
`item.<field>`.

## Modal dialogs in TSX UI

`<Modal>` creates a full-screen backdrop and a blocking panel while native UI
input keeps focus inside the visible dialog. Its children use coordinates
relative to `panel`; the backdrop may use an image or the default translucent
color. `backdropAction` is optional: without it, clicks outside the panel are
still consumed, but do not dismiss the dialog.

```tsx
import {Button, Modal, Scene, Text} from "@prismatix/authoring-sdk";

export default <Scene name="Settings" width={1280} height={720}>
  <Modal name="Confirm" fill visible={false}
    bind={{visible: "confirm.open"}}
    panel={{x: 390, y: 240, width: 500, height: 240}}
    backdropAction="confirm.cancel">
    <Text name="Prompt" x={20} y={24} width={460} height={60}>
      Save these settings?
    </Text>
    <Button name="Yes" x={20} y={170} width={120} height={40}
      action="confirm.accept">Yes</Button>
  </Modal>
</Scene>;
```

When `visible` becomes true, pointer events outside the dialog cannot reach
the underlying screen, and keyboard/gamepad focus moves to the first focusable
child. Hiding the dialog restores the previous focus if that control still
exists. The last visible modal in render order owns capture; a nested modal
can temporarily supersede its parent. The panel itself blocks backdrop clicks
within its rectangle. This is an in-scene dialog; route-level modal navigation
uses the separate UI router API.

## Host Action authoring contract

An application embedding `GalgameUI` can expose its own Actions to authored
UI. Declare their signatures in `prismatix.json` so `compileProject` checks
button clicks, component signals, and Behavior Graph Action arguments before
publishing the artifact:

```json
{
  "settings": {
    "authoring.actionContract": {
      "format": "PrismatiXActionContract",
      "schemaRevision": 1,
      "actions": [{
        "id": "host.open",
        "parameters": [{"name": "target", "type": "string", "required": true}],
        "safety": {
          "previewSafe": true,
          "deterministic": true,
          "seekSafe": true,
          "rollbackSafe": true
        }
      }]
    }
  }
}
```

An authored button can then use `{"id":"host.open","arguments":{"target":"settings"}}`
as its `onClick`. Supported parameter types are `boolean`, `integer`,
`number`, `string`, and `resource`. Action and parameter names must be unique;
an Action ID cannot also be declared by an extension. The SDK rejects missing,
unknown, or mistyped static arguments and checks dynamic bindings against the
declared parameter names. Other Actions remain available through their existing
providers.

The host registers a typed handler and validates the packaged contract before
mounting authored routes. This example assumes `project` is the parsed
`project.pxproject` JSON:

```cpp
px::Status PrepareHostActions(px::ui::GalgameUI& ui,
                              const nlohmann::json& project) {
    px::ui::ActionDescriptor descriptor;
    descriptor.id = "host.open";
    descriptor.arguments = {{.name = "target", .type = px::VariantType::String,
                             .required = true}};
    descriptor.previewSafe = true;
    descriptor.deterministic = true;
    descriptor.seekSafe = true;
    descriptor.rollbackSafe = true;
    auto status = ui.RegisterHostAction(std::move(descriptor),
        [](const px::ui::ActionInvocation& invocation) {
            const auto* target = invocation.arguments.at("target")
                .TryGet<std::string>();
            return OpenHostScreen(*target); // Replace with the host's operation.
        });
    if (!status) return status;
    return ui.ValidateActionContract(
        project.at("settings").at("authoring.actionContract").dump());
}
```

`ValidateActionContract` requires an exact registered handler, parameter
signature, and four matching safety flags for every declared Action. A generic
`RegisterActionCommand` fallback does not satisfy this check. A failed check
must stop that host from mounting the authored UI; it reports `PXUI2824`.
The catalog removes the host descriptors when `GalgameUI` is destroyed.
Preview dispatch also applies the native Action safety gate, so a handler
marked destructive in Preview or non-deterministic is blocked there.

Stock Player does not register application-specific handlers from this
declaration. A project using these Actions must provide a host integration
that registers them. The contract describes the host interface; it cannot
implement an Action by itself.

## Packaged Player user data

The packaged Player treats its installation directory as game content, not as
a writable save directory. It keeps `Save/config.dat`, `Save/profile.dat`,
`Save/save_<slot>.pxsav`, `Save/autosave.pxsav`, and `logs/PrismatiXPlayer.log`
under a per-user directory chosen by SDL's `SDL_GetPrefPath`. The organization
is `PrismatiXEngine`; the application directory is `Game ` followed by the
SHA-256 of the package's stable `gameId`. It does not change when content is
repacked or the installation is moved.

To choose another parent directory, set `PRISMATIX_PLAYER_DATA_ROOT` to an
**absolute** path before starting Player. Player still creates a separate
`Game <SHA-256(gameId)>` child within that parent. This is useful for isolated
tests or portable deployments; put it outside an artifact/distribution output
if you want the ownership-safe CLI to update that output in place. An invalid
or unwritable data location fails startup rather than silently falling back to
the package or launcher working directory. Developer launches without an
adjacent package keep their existing relative `Save/` and `logs/` behavior.

On the first launch with this storage policy, Player copies recognized files
from the older `<package>/Save/` directory to the new location. It recognizes
settings, profile, numeric slots, autosave, and their `.legacy*.bak` key
migration backups. Existing destination files win; nothing in the package is
overwritten or deleted. A marker in the new user-data directory prevents a
later launch from restoring stale old saves after a player deletes or changes
them. A failed or unsafe import stops startup and retains the old files for
recovery. Persistence file path strings are interpreted as UTF-8, including
Windows paths with non-ASCII user names.

The old portable `Save/` and `logs/` directories remain beside an installation
that was already run with an older Player. Their bytes are preserved, so an
in-place `prismatix pack` may still reject that *old* distribution as modified.
After verifying the imported saves, move those legacy directories to a backup
outside the distribution before repacking. New packages no longer create them
by default. `PrismatiXPlayer --inspect-save <path> --package <Package/manifest.json>`
continues to accept an explicit save path.

## Custom render effects

PrismatiX custom effects extend the existing native Stage compositor. They are
offline assets, not runtime shader injection: the Packager validates a bounded
`.pxeffect` descriptor, compiles its HLSL fragment entry point to SPIR-V,
reflects the fixed resource layout, and emits only fingerprinted native release
artifacts before removing the HLSL source from the Player archive. DXC-capable
Windows/Linux packaging emits SPIR-V, DXIL, and MSL; macOS uses the pinned
glslang HLSL frontend plus ShaderCross/SPIRV-Cross and emits SPIR-V and MSL.

### Descriptor

```json
{
  "format": "PrismatiXEffect",
  "schemaRevision": 1,
  "id": "dream-tone",
  "targetLayer": "stage",
  "shader": "Effects/dream-tone.frag.hlsl",
  "uniforms": [
    {
      "name": "amount",
      "type": "number",
      "slot": 0,
      "default": 0.5,
      "minimum": 0,
      "maximum": 1
    }
  ]
}
```

The `targetLayer` is `stage`, `node` or `transition`; the shader receives a
controlled viewport input. An effect may
declare at most eight uniquely named uniforms in unique slots.
Supported types are `number`, `vec2`, and `color`; defaults must be finite and
inside the declared range. Colors also remain in normalized `[0, 1]` space.

### Fixed HLSL interface

```hlsl
cbuffer PrismatiXEffectContext : register(b0, space3) {
  float2 texelSize;
  float progress;
  float randomSeed;
  float4 parameters[8];
  float4 viewport; // logical x, y, width, height
};
Texture2D stageTexture : register(t0, space2);
SamplerState stageSampler : register(s0, space2);
```

Stage and node effects bind their source at `t0`/`s0`. Transition effects bind
the immutable outgoing frame at `t0`/`s0` and incoming frame at `t1`/`s1`.
Reflection must report exactly one sampler for Stage/node or two for transition,
one uniform buffer, one output, and no storage textures or buffers. The runtime
provides only those textures, normalized progress, logical viewport and texel
size, deterministic seed, and declared parameter slots. Raw renderer objects,
GPU pointers, filesystem, network, wall-clock, and unrestricted randomness are
never exposed.

### JavaScript

```ts
ctx.stage.customEffect("dream-tone", 0.65, {amount: 0.8});
ctx.stage.nodeEffect("portrait", "character-outline", 1, {width: 2});
ctx.stage.clearCustomEffect();
ctx.stage.clearNodeEffect("portrait");

const transition = ctx.effects.play("ink-wipe", 0.8, {softness: 0.12});
await ctx.effects.wait(transition);
```

Named values are checked against the packaged schema before native uniform
updates. The positional `vec4[]` form remains available for compatibility with
older extensions. Per-frame progress sampling and rendering stay native; Stage
effect state, parameters, and deterministic seed participate in save, seek, and
rollback capture.

### Capability and fallback

All three custom-effect targets require `graphicsTier: "gpu-effects"`. Native
Player and Native Preview select a fingerprint-valid packaged artifact accepted
by the SDL GPU device and reject the package if none is compatible. Package
manifests carry one to three unique release formats and always retain SPIR-V for
reflection plus MSL for Apple targets; DXIL is emitted only where the build-time
DXC backend exists. Shader compilers remain build/Packager dependencies and are
never copied into the Player distribution.

The WASM Preview currently has no custom GPU-effect backend and fails the GPU
tier explicitly instead of silently rendering a different image. Use basic-tier
built-in effects where a software/WASM fallback is required.

## Testing with journeys

`prismatix test` plays a packaged game with scripts and fails when one
throws, so a build can be checked screen by screen without clicking through
it. Pack with the QA Player, which answers the test interface (the shipped
Player has none of it):

```bash
prismatix pack . --output dist/qa --player <runtime>/PrismatiXPlayerQA.exe
prismatix test --package dist/qa
```

Journeys are `tests/journeys/*.ts` (or paths given on the command line).
Each gets a fresh Player with its own user data:

```ts
import type {PrismatixTestClient} from "@prismatix/cli/test";

// The default limit is 180 s; a long tour sets its own (--timeout overrides).
export const timeoutMs = 600_000;

export default async function tour(px: PrismatixTestClient) {
  await px.waitFor({appState: "title", idle: true});
  await px.jump("chapter2:after_school");
  // Click through lines until one matches, in one request.
  await px.advanceUntil({textContains: "放學後"});
  await px.screenshot("after-school.png");
  await px.advanceUntil({choices: true});
  await px.choose(0);
  await px.save(1);  // the thumbnail is the next Stage frame, without the HUD
}
```

- `px.advanceUntil(condition, {timeoutMs, maxLines})` takes the same
  conditions as `px.waitFor`. The Player clicks a few lines (and timed waits)
  per frame and checks the condition after each, so it never clicks past the
  line it looks for. It fails at a choice the condition does not ask for,
  when the story ends, and after `maxLines` (5000) lines; it returns the
  state with `advancedLines`.
- `px.save(slot)` keeps the state at the request and writes it once the next
  Stage frame is drawn, as the Save screen does, so the slot shows a real
  thumbnail. Inside `px.batch([...])`, which runs in one frame, `save` writes
  at once and without a thumbnail.
- Screenshots with relative paths go to `.prismatix/test-results/<journey>`.
  On failure the report shows the tail of the Player log and keeps the user
  data.
