# Shipping guide

Packaging and maintaining a released game: patches and overlays, save compatibility, importing an existing game, and the Android build.

- [Signed PDX content overlays](#signed-pdx-content-overlays)
- [Save migration anchors](#save-migration-anchors)
- [Augmenting a published artifact](#augmenting-a-published-artifact)
- [Importing stories from other formats](#importing-stories-from-other-formats)
- [Resolving assets while importing Story](#resolving-assets-while-importing-story)
- [Android (experimental)](#android-experimental)

## Signed PDX content overlays

`create-patch` / `apply-patch` still reconstruct and verify a complete new
distribution. The content-only PDX overlay is a smaller, exact-base update.
The Player scans bounded `.pdx` candidates in `Package/updates` and applies
the unique signed, exact-base update chain. Filenames do not control order.

### Publishing a signed update

```sh
prismatix overlay-keygen --private path/to/private-key.txt --public path/to/public-key.txt
prismatix pack game --artifact path/to/artifact --publisher-key path/to/public-key.txt --output path/to/v1
prismatix pack game --artifact path/to/updated-artifact --publisher-key path/to/public-key.txt --previous-package path/to/v1 --output path/to/v2
prismatix create-overlay --base path/to/v1 --target path/to/v2 --output path/to/patch.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --patch path/to/patch.pdx --require-signature
```

For a content-only update, the complete v2 distribution is optional. A
published artifact can supply the target inventory directly:

```sh
prismatix create-overlay --base path/to/v1 --artifact path/to/updated-artifact --output path/to/patch.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --patch path/to/patch.pdx --require-signature
```

To see the exact number of changed/removed resources and changed raw bytes
before allocating patch disk space, use the same artifact with `--check`:

```sh
prismatix create-overlay --base path/to/v1 --artifact path/to/updated-artifact --check --signing-key path/to/private-key.txt
```

`--check` fully hashes the current artifact inputs, verifies the base PDX and
publisher key, and writes no overlay or staging PDX. It accepts `--updates-dir`
and `--optional-id` for a chained or optional patch. The reported raw bytes
are not the final PDX size; the source can also change after the check, so
the publishing command repeats verification and streaming checks.
Base PDX shards are verified concurrently, up to four by default; set
`PRISMATIX_PACKAGE_VERIFY_WORKERS` to an integer from 1 to 8 to tune this for
the storage device. Encrypted bases stay single-worker because older
encrypted-and-compressed entries can require a whole-entry buffer. Each entry
still has its CRC and indexed SHA-256 checked.
Parallel verification reduces elapsed time without making the check
incremental or trusting unverified cached bytes.
On Windows, direct artifact overlay planning also overlaps the full artifact
hash with base verification while holding read-only handles on base PDX
shards through publication. It holds the base static files and selected
Player/runtime inputs read-only before their complete hash comparison, then
checks that their file names have not changed before publishing. This avoids
a second full runtime-file hash without accepting in-place edits. Other
platforms keep the two stages sequential and rehash runtime inputs at the
publication boundary.

This path fully checks the artifact inputs, verifies the complete base
distribution, confirms the selected Player and runtime libraries match the
base, pins the base manifest used while planning, and streams only changed
resources into the patch. After the full input check, it re-resolves artifact
paths only for resources whose bytes will enter the new PDX; unchanged entries
are supplied by the verified base. It does not write a
second multi-gigabyte distribution. The target manifest and content index
are computed by the same Packager planner used for a complete package; a
later v2 built from that exact artifact with `--previous-package v1`, the
same Player, encryption setting, publisher key, and shard count has matching
target documents. The artifact must be a CLI-published directory with intact
ownership metadata. Build it with `prismatix build --artifact-only`, or
publish changes with `prismatix augment`.

The overlay writer streams changed file and PDX-stream resources without
buffering an entire entry. Resources of at least 64 KiB use bounded zstd
compression, except streaming video formats. It still checks the planned
source size and SHA-256 while writing, then independently verifies the
published PDX. The compression choice can affect the final patch size, so
`--check` reports raw changed bytes rather than a promised download size.
On an 8 GB, 31,756-resource corpus, replacing one 256 MiB WAV produced a
67 MB signed overlay (about 75% smaller than the uncompressed writer) with a
sampled Packager peak working set near 600 MB; the Player loaded the signed
patch without route errors. Treat this as a single warm-cache measurement,
not a size or time guarantee.

To publish `patch2.pdx` without a complete v2, put the signed `patch.pdx`
in a directory of prior updates and use its verified target as the next
base. The new output must be outside the installed base and artifact:

```sh
prismatix create-overlay --base path/to/v1 --updates-dir path/to/prior-updates --artifact path/to/artifact-v3 --output path/to/patch2.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --updates-dir path/to/prior-updates --patch path/to/patch2.pdx --require-signature
```

The same command works for later main updates: `--updates-dir` must contain
the complete, unique signed chain preceding the new patch. It rejects
damaged or unrelated main update candidates. Once verified, install the new
patch alongside the prior updates. To publish `adult.pdx` against the latest
main update, use the same `--updates-dir` and add `--optional-id adult`; it
remains independently enabled by the player. Updating `adult.pdx` after a
new main patch requires a new optional pack signed against that latest main
version. `verify-overlay --updates-dir` also works after the patch is installed:
it excludes the `--patch` file while resolving its preceding chain.

The two distributions must have the same game ID, Engine version, publisher
public key, and non-content runtime files, but distinct package fingerprints.
`--previous-package` makes v2 a standalone distribution that carries compatible
save identity and legacy key readers from v1. The CLI verifies v1 before
using it and leaves it untouched. Subsequent standalone versions should name
their immediate predecessor the same way. The project's committed
`.prismatix-release/lineage.json` carries the same save identity when the
previous distribution is not available (see
[same-version releases](#same-version-releases)). For incompatible Story state, advance
the content/save version and provide an explicit migration.
The public key is a lowercase, uncompressed SEC1 P-256 point in
`Package/manifest.json`. It also affects the package fingerprint. Keep the
private key outside the project and distribution, restrict access to it, and
back it up securely. `overlay-keygen` refuses to overwrite either output.

After verification, install the PDX as `v1/Package/updates/patch.pdx` and
restart Player. Place later patches in the same updates directory; Player
discovers their dependency order from
the signed base and target documents. The Player checks the pinned publisher key, signature, exact
base manifest and index bytes, target metadata, and every changed resource
before mounting it. It uses the base PDX archives and their original
decryption key, then mounts the overlay at higher VFS priority and applies
tombstones for removed resources. Route, Runtime IR, source map, content
version, and save migration settings come from the signed target manifest.
When content and save versions stay the same, Player also accepts a save from
an earlier fingerprint in the verified update chain, then checks its Runtime
document and exact execution anchor before restoring it. An in-place package
replacement also carries a bounded list of earlier same-version fingerprints
in `persistence.compatibleSaveFingerprints`; Player combines that list with
the signed main update chain. Legacy key readers from the installed base and
signed main target remain available across the chain, including when an
optional pack is enabled. An unrelated
same-version fingerprint is still rejected. If an update changes save or
Story state incompatibly, advance the content/save version and ship an
explicit save migration in the signed target.
Invalid or stale candidates are ignored. If two valid updates target the same
version, Player rejects the ambiguous chain and starts the base package.
It also retries the base package if Runtime initialization of an accepted
overlay fails. The installed update file is never modified by Player.
During chain discovery, Player reads each PDX's small manifest once and uses
its claimed base fingerprint to select candidates for the current version.
That claim is only a routing hint: selected candidates still undergo full
signature, exact-document, and changed-resource verification before mounting.

### Publishing an optional pack

Build a complete optional target distribution from the latest main version.
Keep its game ID, Engine version, publisher key, content version, save version,
and persistence identity the same. The optional target can add or replace
resources and routes. For example, after v2 is the latest main version:

```sh
prismatix create-overlay --base path/to/v2 --target path/to/v2-with-adult --output path/to/adult.pdx --signing-key path/to/private-key.txt --optional-id adult
```

Install `adult.pdx` beside the main updates in `Package/updates`. It remains
off by default. From the installed distribution, run
`PrismatiXPlayer --optional-pack adult on` or
`PrismatiXPlayer --optional-pack adult off`, then restart Player. The choice
is atomically stored in that game's per-user data directory as
`optional-pack.txt`, so
reinstalling the game does not reset it. Only one optional pack can be enabled
at a time. The enabled pack must be signed for the final main update version;
an older optional pack is ignored after a new main update until the publisher
ships a compatible replacement. The Player uses the main update fingerprint
for save identity while the optional pack is enabled. Saves inside content
that is absent when the pack is off may still need a migration or the pack to
be enabled again.

### Format and verification

An overlay is an unencrypted PDX5 file whose virtual resources are exactly:

- `__prismatix_overlay__/manifest.json` (at most 16 KiB)
- `__prismatix_overlay__/target-manifest.json` (at most 4 MiB)
- `__prismatix_overlay__/target-content-index.json` (at most 128 MiB)
- `__prismatix_overlay__/signature.bin` (exactly 64 bytes, signed revisions 2 and 3)
- every added or changed target resource, by its original virtual URI

The overlay manifest has `format: "PrismatiXPackageOverlay"`, a
`schemaRevision` of 1 for unsigned updates, 2 for signed updates, or 3 for
signed optional packs. Revision 3 adds `kind: "optional"` and a bounded
lowercase `optionalId`. All revisions contain `gameId`,
`basePackageFingerprint`, `targetPackageFingerprint`, and SHA-256
fields named `baseManifestSha256`, `baseContentIndexSha256`,
`targetManifestSha256`, and `targetContentIndexSha256`. The two target
documents copy the Packager-produced `Package/manifest.json` and
`Package/content-index.json`. The reserved overlay prefix cannot be an
ordinary package resource.

Revisions 2 and 3 sign the SHA-256 digest of the exact overlay manifest bytes,
prefixed with the ASCII bytes `PrismatiXPackageOverlaySignatureV2` or
`PrismatiXPackageOverlaySignatureV3`, respectively, and a NUL byte.
`signature.bin` contains a raw P-256 ECDSA SHA-256 signature in
`r || s` form. The signed manifest hashes the exact base and target
documents; the target index hashes every changed payload. Verification
rejects missing, extra, mismatched, or damaged entries. Unchanged resources
remain in the base PDX even if the target changes their shard assignment.

`prismatix verify-overlay` checks every revision and verifies signatures
whenever present. Add `--require-signature` for the Player trust policy; it
rejects revision 1. Unsigned overlays remain available for explicit
inspection and are never loaded automatically.

The distribution-to-distribution form of `create-overlay` fully verifies both
input distributions, rejects changed
Player/runtime files, streams changed content directly from target archives
into an uncompressed PDX5,
re-verifies its output, and publishes without replacing an existing file.
It rechecks manifest and index bytes before publication. The staged PDX needs
space for the final patch, but changed resources no longer need separate
extracted files. Each streamed entry is checked against the target index size
and SHA-256 while writing. Unencrypted compressed PDX entries decode
with a capped zstd history window while streaming, and complete size/CRC
verification at end of stream. Legacy whole-entry encrypted and compressed
entries still buffer the decoded entry; encrypted chunked entries stream.
On Windows, publication moves the verified PDX within the output volume without
the replace-existing flag, so it also works on volumes without hard-link
support. A competing output created during the build is preserved. Other
platforms currently publish with a no-replace hard link.

### Current limits

Only one optional pack can be enabled at a time, and switching requires a
Player restart. There is no in-game toggle UI yet. A signed update chain may
contain at most 64 regular PDX candidates in a
dedicated directory, and ambiguous branches fail closed. Publishing from a
prior signed chain still rechecks all artifact inputs and verifies the complete
installed base. It has not been measured on the 8 GB project and does not
provide block-level incremental authoring or packaging.

Verification does not rehash unchanged base PDX payloads or independently
validate the target's entire authored dependency graph. A local process that
can concurrently rewrite distribution files can still race verification and
mounting; the base distribution and its Player must be protected by the
normal installation boundary. Two full approximately 7.488 GiB Senren
distributions have produced a signed 5.41 MiB overlay that Player mounted
through 22 UI routes. A real v29 same-version encrypted autosave was loaded
by a separate v31 standalone package. These checks do not cover a large
changed compressed media entry, disk-use peak, or a real cross-version save
migration.

## Save migration anchors

### Same-version releases

A release that keeps `contentVersion` and `saveVersion` needs no migration,
but its package fingerprint still changes with its content. The Player accepts
a save from an earlier fingerprint only when the current package lists that
fingerprint in `persistence.compatibleSaveFingerprints`. Packager builds the
list from the distribution it replaces and from the project's
`.prismatix-release/lineage.json`, which the CLI updates after each pack.
Commit that file with each release. It records the latest package fingerprint,
its content/save version, the earlier same-version fingerprints, and the legacy
persistence readers. These are the values the shipped `Package/manifest.json`
already contains in plain text. Up to 64 same-version fingerprints are kept; Packager fails
with `PXPKG1428` beyond that, which means it is time to advance the version.
When the version changes, the list starts over and the declared migrations
decide which old saves still load.

The Player reports these load failures separately:

| Code | Meaning |
| --- | --- |
| `PXSAVE6115` | The save belongs to a different game ID. |
| `PXSAVE6111` | Same content/save version, but the save's package fingerprint is not current or listed as compatible. The package was probably built without the previous release's lineage. |

To recover from `PXSAVE6111` after a release has shipped, restore the lineage
file from version control, or pack again with `--previous-package` naming the
shipped distribution. The new package then accepts both fingerprints.

### Anchors

When a release changes `contentVersion` or `saveVersion`, declare each migration
step in `prismatix.json` and provide its `.pxsave-migration` document. A step
can also rename variables or routes and change the Runtime IR path. Test it
against copies of saves from the released Player before shipping.

The `anchor` field controls the saved execution position:

| Policy | Behavior |
| --- | --- |
| `preserve` | Keep the saved document, source, and operation IDs. |
| `map` | Require an exact mapping for the saved anchor; reject an unlisted anchor. |
| `map-or-preserve` | Map listed anchors and keep an unlisted anchor's IDs. |

Use `map-or-preserve` when a large Story keeps most identities but moves a few
operations. It avoids listing every unchanged anchor in a file limited to
100,000 mappings and 4 MiB by the native migration reader. For example:

```json
{
  "format": "PrismatiXSaveMigration",
  "schemaRevision": 2,
  "id": "chapter-1-to-2",
  "from": {"contentVersion": "chapter-1", "saveVersion": 1},
  "to": {"contentVersion": "chapter-2", "saveVersion": 2},
  "anchor": {
    "policy": "map-or-preserve",
    "mappings": [{
      "from": {"runtimeDocumentId": "old-document", "sourceId": "old-line", "operationId": "old-op"},
      "to": {"runtimeDocumentId": "new-document", "sourceId": "new-line", "operationId": "new-op"}
    }]
  },
  "operations": []
}
```

`prismatix ids --sync` helps review added and removed Story identities but
does not generate or verify a migration. The Authoring SDK can compare compiled
Runtime IR from two releases and require a replacement for each old anchor
that has disappeared:

```ts
import {planSaveAnchorPolicy} from "@prismatix/authoring-sdk";

const plan = planSaveAnchorPolicy(previousRuntimeIr, nextRuntimeIr, [
  {fromOperationId: "old-op", toOperationId: "new-op"},
]);
if (!plan.valid) throw new Error(JSON.stringify(plan.diagnostics));
const migrationAnchor = plan.value!.anchor;
```

Use `migrationAnchor` as the document's `anchor` field. The inputs are the
canonical Runtime IR documents; operation IDs identify commands within each
document. The helper retains exact old anchors, maps explicitly reviewed
replacements, and reports missing or duplicate identities. Compare every
supported locale's Runtime IR and test real old saves: the helper cannot tell
whether a surviving ID or replacement still means the same thing. The Player
checks that the resulting document and exact source/operation pair exist
before restoring a slot. It rejects an unchanged anchor that was removed.
Existence alone cannot prove the operation has the same narrative meaning or
that variables, resources, and routes still make sense. Validate representative
old slots at each changed position in the packaged Player.

`map-or-preserve` requires a Player with this policy implemented. Earlier
Players do not understand it. A content overlay cannot replace the Player
executable, so an already released older Player needs a new base distribution
with the updated runtime before it can use this policy.

## Augmenting a published artifact

Prefer declaring ordinary runtime files with `resourceIncludes` in the source
project. Use `prismatix augment` only when a post-build tool genuinely creates
or replaces files after the canonical artifact build. Never edit an artifact
or `.prismatix-output.json` directly.

Create a manifest outside the artifact, for example `artifact-augment.json`:

```json
{
  "format": "PrismatiXArtifactAugmentation",
  "schemaRevision": 1,
  "owner": "my-importer",
  "files": [
    {"source": "Generated/bonus.png", "target": "Assets/bonus.png", "mode": "add"}
  ]
}
```

`source` is relative to the author project; `target` is relative to the
published artifact. Both must be safe paths to regular files, without symlinks.
An `add` target must not exist. To replace an owned artifact file, use
`"mode":"replace"` and supply its current 64-character SHA-256 from
`.prismatix-output.json` as `expectedFingerprint`. A replacement fails if a
newer build changed that file. A different augmentation owner cannot replace
another owner's file. The CLI also rejects duplicate targets and its reserved
ownership files.

```bash
prismatix build my-game --artifact-only --output my-game/.prismatix/artifact
prismatix augment my-game --artifact my-game/.prismatix/artifact --manifest my-game/artifact-augment.json
prismatix pack my-game --artifact my-game/.prismatix/artifact
```

The operation checks the old artifact's complete inventory, takes the same
publication lock as `build`, stages unchanged files using hard links when the
filesystem allows, and publishes a new complete ownership inventory atomically.
It writes `.prismatix-augmentations.json` into the artifact to track target
owners. Failure keeps the old artifact; no partially augmented output is
published. Rebuilding the same artifact path is allowed afterward, but a fresh
build replaces the overlay, so rerun `augment` if it is still needed. Because
ownership verification and fingerprinting read the complete artifact, this is
not a block-level incremental build or a way to avoid hashing large media.

`augment` validates publication safety, not whether a replacement UI scene or
project document is semantically correct. `pack --artifact` performs the
native package validation, including file hashes, canonical routes and
contracts. Test the resulting Player before shipping.

## Importing stories from other formats

### Artemis games

`prismatix import artemis <extracted-game> [project]` converts an extracted
Artemis game: scenes, save-ID sidecars and the story index, characters and
layered portraits, the gallery (`Content/game.pxgame`), the `characters` and
`storyAssets` fields of `prismatix.json`, and `docs/conversion-report.json`.
Defaults such as fade times, screen size and font sizes come from the game's
own `list_windows.tbl`. Anything that only this game does (act presets,
scrambled movie names, replay scenes, chapter rules, key bindings, asset roots)
goes in `artemis-import.json` beside `prismatix.json`, which the command
validates.

Re-running the import keeps the save IDs already in the sidecars, so saves made
with an earlier import still load. The output uses the short Story forms; pass
`--long-forms` to spell every argument by name. The importer does not copy the
original media into the project.

### Writing your own importer

The Authoring SDK provides the output side of a story importer. A format
adapter decodes its own source files and translates their commands into
PrismatiX Story lines. The SDK checks those lines, assigns the adapter's
explicit execution IDs to an adjacent `.pxids.json` sidecar, and assembles a
multi-scene, multi-locale `story.pxindex`. It does not decode KAG, SCN, PSB, or
another game's proprietary format by itself.

```ts
import {assembleImportedStoryProject} from "@prismatix/authoring-sdk";

const imported = assembleImportedStoryProject({
  id: "example-story",
  entryScene: "intro",
  locales: ["en"],
  chapters: [{id: "main", title: "Main"}],
  scenes: [{
    id: "intro", chapter: "main", requiredLabels: ["start"],
    sources: {en: {
      path: "Story/en/intro.pxstory",
      lines: [
        {source: "*start", id: "original.intro.start"},
        {source: "Hello.", id: "original.intro.greeting"},
        {source: "[end]", id: "original.intro.end"},
      ],
    }},
  }],
});
if (!imported.valid) throw new Error(JSON.stringify(imported.diagnostics));
// Publish imported.value.files together, then compile the project.
```

An adapter can use `serializeStoryCommand` for named JSON arguments and
`createImportedResourceResolver` for unambiguous media lookup. It should
decode one source scene at a time rather than retain all raw source data. The
assembler takes the translated scenes and returns canonical index, Story,
and sidecar text without writing files. Write the returned files as one
reviewable set; a failed import returns no set to publish.

Every executable line must have a stable ID assigned from the original
source. Blank lines, comments, speaker declarations, and `choice.wait` do not
take IDs. The SDK rejects duplicate IDs within a shared identity namespace,
different ID or operation-kind sequences across locales, missing locales,
unsafe or colliding paths, and source or sidecar files above 16 MiB. A later
`compileStoryProject` or `compileProject` must still check command meaning,
resources, labels, and cross-scene jumps. The assembler does not infer how
two versions of a story should map old saves; use the [save migration
tools](#save-migration-anchors) for that review.

For an existing importer whose labels previously received generated
`sourceId` values, `buildImportedStorySource(path, lines,
{legacyLabelSourceIds: {documentId}})` can recreate that ID seed while still
requiring explicit IDs on dialogue and commands. Use the exact `documentId`
from the old import. When a sidecar already exists, reconcile the new source
against it with `synchronizeAlignedStoryIdentityMap` or
`synchronizeStoryIdentityMap` before publishing: prior edits may have preserved
label IDs that a fresh generation would change. New importers should assign
explicit IDs to labels too.

The Senren port's SCN/PSB decoder and original-command translation remain
project-specific. Its SCN adapter now uses `buildImportedStorySource` per
scene and keeps the existing sidecar reconciliation. Its project index and
media resolution still use port-specific code. The SDK already provides the
general index assembler and an ambiguity-reporting media resolver; replacing
the port's older code is project migration work because its existing edited
sidecars must remain stable and its media fallback chooses among original
assets using game-specific rules. The SDK does not need those rules to support
other importers. This does not make the SDK a general KAG or PSB input decoder.

## Resolving assets while importing Story

An importer can enumerate media once and use the Authoring SDK to match names
from an external script format to project-relative resource paths. Pass only
paths that belong to the resource category being imported:

```ts
import {createImportedResourceResolver} from "@prismatix/authoring-sdk";

const indexed = createImportedResourceResolver([
  "Assets/Characters/hero.png",
  "Assets/UI/hero.png",
  "Assets/BG/school.png",
]);
if (!indexed.valid || !indexed.value)
  throw new Error(JSON.stringify(indexed.diagnostics));

const image = indexed.value.resolve("hero.png");
if (image.kind === "ambiguous")
  throw new Error(`Choose an exact path: ${image.candidates.join(", ")}`);
if (image.kind !== "found") throw new Error(`Missing or unsafe image: ${image.query}`);
// Write image.path into a Story resource argument.
```

Exact project-relative paths win. A bare name is case-insensitive and matches
the filename stem; if the name includes an extension, that extension must also
match. Multiple candidates produce `ambiguous` with deterministic, sorted
paths. The resolver never selects one by file size, directory order, or locale.
It rejects unsafe index paths, and a query with a directory never falls back to
another directory's basename. If two source paths differ only by case, even an
exact lookup is ambiguous: this avoids a project that works on one filesystem
and opens a different asset on another.

The input is an array of **already enumerated** project-relative paths. The
resolver does not walk folders, decode external formats, create asset UUIDs, or
register assets in the package. Continue to use Story static resource
references, `resourceIncludes`, or the project asset catalog for build-time
dependencies. Keep the source format decoder in its importer and report every
missing or ambiguous lookup before writing generated Story files.

To write a command after resolving its resources, use
`serializeStoryCommand(name, arguments)`. It quotes strings and encodes JSON
values so the Story parser can read them back:

```ts
import {serializeStoryCommand} from "@prismatix/authoring-sdk";

const line = serializeStoryCommand("show", {
  character: "hero", asset: "Assets/Characters/hero smile.png",
  transition: "auto",
});
if (!line.valid || !line.value)
  throw new Error(JSON.stringify(line.diagnostics));
// line.value can be appended to a .pxstory file.
```

This helper checks that the line is syntactically parseable; project
compilation still checks whether the command, resource, character, and flow
targets are valid. `[if ...]` uses an expression grammar and must be authored
separately. To keep save identities stable across reimports, follow the Story
sidecar workflow in the Authoring SDK README; neither helper writes files or
silently updates a published identity map.

## Android (experimental)

> **Status: builds, not yet run on a device.** The arm64 Player library,
> the Gradle project and `prismatix pack --target android` have produced
> debug and signed release APKs and an AAB from the `minimal` starter on a
> Windows host. Nothing has been installed on a phone or emulator yet, so
> runtime behaviour on Android (rendering, audio, lifecycle, touch,
> text-to-speech) is unverified. Please update this page after the first
> device run.

This page covers phases A0–A2 of the Android plan in the
2026-09-30 release review.

### What is verified

| Area | State | Where |
| --- | --- | --- |
| arm64-android vcpkg dependencies | All 27 target ports build from a Windows host (NDK r27d) | `vcpkg/triplets/arm64-android-prismatix.cmake` |
| `libmain.so` | Links; stripped 19.6 MB; needs only FFmpeg and system libraries; 16 KB pages | `android-arm64` preset |
| Gradle project | `assembleDebug`, `assembleRelease`, `bundleRelease` succeed | `packages/cli/templates/android` |
| `prismatix pack --target android` | Builds the APK (and AAB when signing is configured) | [below](#build-an-apk) |
| Reading the package from the APK | Tested on desktop through SDL only | `Engine/IO/FileSource.h` |
| Background, foreground, low memory, terminate | Rules unit-tested on Windows only | `Engine/Platform/AppLifecycle.h`, `Engine/Runtime.cpp` |
| Touch gestures and `platform.touch` | Unit-tested with synthetic finger events | `Engine/Platform/TouchGestures.h`, [Touch input](#touch-input) |
| Text-to-speech (self-voicing) | Compiles; JNI to `android.speech.tts.TextToSpeech` | `Engine/Accessibility/SpeechService.cpp`, `PrismatiXSpeech.java` |
| TalkBack (screen reader tree) | Not supported | `Engine/Accessibility` |
| Play Asset Delivery | Not started | phase A3 |

### Prerequisites (host)

- Android SDK with `platforms;android-35` and `build-tools;35.0.0`, and
  `ANDROID_HOME` pointing at it. The template turns off AGP's automatic
  SDK downloads, so a missing package fails the build by name.
- Android NDK r27 or newer; set `ANDROID_NDK_HOME`.
- JDK 17 or newer in `JAVA_HOME`, and Gradle 8.9 or newer (tested with
  JDK 21 and Gradle 8.11.1). The first Gradle run downloads the Android
  Gradle Plugin from Google's Maven repository into `GRADLE_USER_HOME`.
- vcpkg (`VCPKG_ROOT`; Visual Studio's bundled copy works) and the desktop
  build of this repository: the Android build runs the host `shadercross`
  to compile built-in shaders.

### Build `libmain.so`

```sh
cmake --preset android-arm64
cmake --build --preset android-arm64
```

The preset chain-loads the NDK toolchain into vcpkg with the overlay
triplet `arm64-android-prismatix`: API 26, release-only, everything static
except FFmpeg, which is LGPL and ships as replaceable shared libraries.
The preset assumes a Windows host (`VCPKG_HOST_TRIPLET=x64-windows` and the
`shadercross.exe` path); on macOS or Linux, override both. To keep the
large Android trees off the system drive, add
`-B <dir> -DVCPKG_INSTALLED_DIR=<dir>/vcpkg_installed`.

Each Player directory is self-contained:

- `android/shipped/arm64-v8a/`: stripped `libmain.so`, the FFmpeg
  `lib*.so` files and `prismatix-player.json`. Pass this folder to the CLI.
- `android/shipped/symbols/arm64-v8a/libmain.so`: unstripped, for
  `ndk-stack`.
- `android/qa/...`: the QA Player with the QA IPC server. The CLI
  refuses to ship it.

Notes from the first build:

- NDK r27's libc++ 18 needs `-fexperimental-library` for `std::jthread`
  and `std::stop_token`, and has no floating-point `std::from_chars`
  (the engine uses `px::ParseFiniteDouble`).
- vcpkg's `harfbuzz.pc` asks for `-lc++` (the shared C++ runtime); the
  build drops it because `libc++` is linked statically.
- Configure fails if the template's SDL Java glue
  (`app/src/main/java/org/libsdl/app`, copied from the SDL source release)
  differs from the native SDL version. When vcpkg updates SDL, copy the
  matching release's `android-project/app/src/main/java/org/` over it.

### Build an APK

Add the Android settings to `prismatix.json`:

```json
"distribution": {
  "android": {
    "applicationId": "com.example.firstlight",
    "versionCode": 3,
    "versionName": "1.2.0",
    "label": "First Light",
    "icon": "Assets/Branding/icon-512.png",
    "keystore": {
      "path": "../keys/upload.jks",
      "alias": "upload",
      "storePasswordEnv": "FIRSTLIGHT_STORE_PASSWORD",
      "keyPasswordEnv": "FIRSTLIGHT_KEY_PASSWORD"
    }
  }
}
```

Only `applicationId` is required. `versionName` defaults to the project
`version`, `label` to the project name, and the orientation follows the
resolution (landscape unless taller than wide). `icon` is a square PNG
(48–4096 px). The keystore names environment variables; passwords never go
in the file, and a `*pass*` key in `keystore` is rejected.

```sh
prismatix pack my-game --target android \
  --android-player out/build/android-arm64/android/shipped/arm64-v8a \
  [--artifact <built artifact>] [--gradle <gradle or its install dir>] \
  [--android-sdk <dir>] [--output <dir>] [--debug]
```

- Without a keystore (or with `--debug`) the result is a debug-signed
  `dist/<id>-android/<id>-debug.apk` for testing.
- With a keystore it is a signed `<id>.apk` plus `<id>.aab` for Play.
- `<output>/build/package` holds the packed game (reused when unchanged) and
  `<output>/build/gradle` the Gradle project (Gradle's caches survive
  between runs). The game goes into `assets/` stored uncompressed, laid out
  as on desktop (`Package/…`, `Content*.pdx`, `Notices/`); the desktop
  Player and its libraries are left out.

To do the same by hand, copy the template, put the `.so` files in
`app/src/main/jniLibs/arm64-v8a/`, the packed game in
`app/src/main/assets/`, optionally write `prismatix.properties` (keys:
`applicationId`, `versionCode`, `versionName`, `screenOrientation`,
`keystore`, `keyAlias`, `storePasswordEnv`, `keyPasswordEnv`), and run
`gradle assembleDebug`.

Install and watch the log on a device:

```sh
adb install -r dist/<id>-android/<id>-debug.apk
adb logcat -s SDL SDL/APP PrismatiX
```

#### How the Player finds the game

`io::PackageFileSource()` is `SdlFileSource()` on Android. The Player opens
`Package/manifest.json`, every `*.pdx` shard and any VFS directory mount
through `SDL_IOFromFile`, which tries the app's internal storage first and
then the APK's `assets/`. Files that a later download places at the same
relative path in internal storage would be picked up first; nothing uses
this yet. Saves, settings, the profile and logs use `SDL_GetPrefPath`.
Signed single-file updates (`Package/updates`) are scanned with
`std::filesystem`, so they do not apply on Android yet. Keep APK assets
under about 1–2 GiB for side-loading; larger games need Play Asset
Delivery (phase A3).

#### Lifecycle

The Runtime registers an SDL event watch. These rules are unit-tested on
Windows; they have not run on a phone.

- **Leaving the foreground:** the Player saves first, then all media pauses.
  The save goes to the autosave slot if a game is in progress, followed by
  the settings and profile, and the Player waits for the writes. Nothing is
  rendered until the app returns.
- **Returning:** only the audio tracks and videos the lifecycle paused
  resume. The frame clock restarts, and text density is recomputed for the
  new surface size.
- **Low memory:** drops textures not drawn in the last frame, font files no
  open font uses, and unreferenced decoded audio.
- **Texture budget:** resident textures are trimmed, least recently drawn
  first, to 192 MiB on Android and iOS (512 MiB on desktop); a texture drawn
  in the last frame always stays. `"player.textureBudgetMiB": {"mobile": 256}`
  in the project settings changes it (32-16384, `desktop` likewise).
  `distribution.transcode` shrinks the APK's media but not this figure:
  textures are decoded to RGBA either way.
- **Terminating:** saves again only if the game ran since the last
  background save, then quits.
- `SDL_EVENT_RENDER_TARGETS_RESET` and `SDL_EVENT_RENDER_DEVICE_RESET`
  recreate the renderer's offscreen targets. A device reset also reloads
  file textures.

### Touch input

Fingers are turned into gestures. Each gesture fires the same command the
mouse, keyboard or gamepad already fires:

| Gesture | Same as | Default effect |
| --- | --- | --- |
| Tap | Left click | Advance the text, or activate the control under the finger |
| Long press (0.5 s) | `toggle-ui` (H) | Hide or show the message window |
| Two-finger tap | `menu` (Escape, gamepad B) | Whatever the project binds to `menu` in `inputActions` |
| Swipe up | `backlog` (B, wheel up) | Open the backlog |
| Drag | Held left button | Sliders and other draggable controls |

A tap acts when the finger lifts, so a swipe or long press never advances
the text as well. SDL's synthetic mouse copies of touches are ignored.
Mouse, keyboard and gamepad input is unchanged.

`platform.touch` is Player UI data (boolean), so TSX can show
touch-friendly variants:

```tsx
<Group name="TouchToolbar" visibleWhen="platform.touch" ... />
<Group name="HoverDock" visibleWhen="!platform.touch" ... />
```

The project setting `player.touchProfile` controls both:

| Value | Gestures | `platform.touch` |
| --- | --- | --- |
| `"auto"` (default) | on | `true` while the last pointer input was a finger |
| `"on"` | on | always `true`: preview touch layouts on desktop |
| `"off"` | off; the first finger acts as a plain mouse pointer | always `false` |

### Verification checklist for the first device run

1. The APK installs, the title screen appears, and logcat shows
   `Mounted archive 'Content.pdx' (… source=sdl)`.
2. A full branch of the `minimal` template plays to the end.
3. Pressing Home during dialogue saves to the autosave slot, audio stops,
   and returning resumes audio and the text.
4. Swiping the app away from recents, then relaunching, lets you load the
   autosave.
5. Video plays, pauses when backgrounded, and resumes.
6. Every gesture in the touch table works, and taps land on the control
   under the finger even when the game is letterboxed.
7. With self-voicing on, each dialogue line is spoken.

Record what broke and how you fixed it here.
