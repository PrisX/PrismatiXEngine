# Signed PDX content overlays

`create-patch` / `apply-patch` still reconstruct and verify a complete new
distribution. The content-only PDX overlay is a smaller, exact-base update.
The Player scans bounded `.pdx` candidates in `Package/updates` and applies
the unique signed, exact-base update chain. Filenames do not control order.

## Publishing a signed update

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

This path fully checks the artifact inputs, verifies the complete base
distribution, confirms the selected Player and runtime libraries match the
base, pins the base manifest used while planning, and streams only changed
resources into the patch. It does not write a
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
The 2026-09-27 [large-media acceptance](SDK_LARGE_MEDIA_OVERLAY_ACCEPTANCE.md)
records an 8 GB corpus run, process memory, sampled disk use, signature
verification, and Player loading.

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
their immediate predecessor the same way. For incompatible Story state, advance
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

## Publishing an optional pack

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

## Format and verification

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

## Current limits

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
