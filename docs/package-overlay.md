# Signed PDX content overlays

`create-patch` / `apply-patch` still reconstruct and verify a complete new
distribution. The content-only PDX overlay is a smaller, exact-base update.
The Player scans bounded `.pdx` candidates in `Package/updates` and applies
the unique signed, exact-base update chain. Filenames do not control order.

## Publishing a signed update

```sh
prismatix overlay-keygen --private path/to/private-key.txt --public path/to/public-key.txt
prismatix pack game --artifact path/to/artifact --publisher-key path/to/public-key.txt --output path/to/v1
prismatix pack game --artifact path/to/updated-artifact --publisher-key path/to/public-key.txt --output path/to/v2
prismatix create-overlay --base path/to/v1 --target path/to/v2 --output path/to/patch.pdx --signing-key path/to/private-key.txt
prismatix verify-overlay --base path/to/v1 --patch path/to/patch.pdx --require-signature
```

The two distributions must have the same game ID, Engine version, publisher
public key, and non-content runtime files, but distinct package fingerprints.
The public key is a lowercase, uncompressed SEC1 P-256 point in
`Package/manifest.json`. It also affects the package fingerprint. Keep the
private key outside the project and distribution, restrict access to it, and
back it up securely. `overlay-keygen` refuses to overwrite either output.

After verification, install the PDX as `v1/Package/updates/patch.pdx` and
restart Player. For a second update, build a complete v3 distribution and
create `patch2.pdx` from v2 to v3 with the same publisher key. Place both
files in the updates directory; Player discovers their dependency order from
the signed base and target documents. The Player checks the pinned publisher key, signature, exact
base manifest and index bytes, target metadata, and every changed resource
before mounting it. It uses the base PDX archives and their original
decryption key, then mounts the overlay at higher VFS priority and applies
tombstones for removed resources. Route, Runtime IR, source map, content
version, and save migration settings come from the signed target manifest.
Invalid or stale candidates are ignored. If two valid updates target the same
version, Player rejects the ambiguous chain and starts the base package.
It also retries the base package if Runtime initialization of an accepted
overlay fails. The installed update file is never modified by Player.

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
is stored in that game's per-user data directory as `optional-pack.txt`, so
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

`create-overlay` fully verifies both input distributions, rejects changed
Player/runtime files, streams changed content into an uncompressed PDX5,
re-verifies its output, and publishes without replacing an existing file.
It rechecks manifest and index bytes before publication. Temporary disk space
is needed for extracted changes; a large compressed source entry may still
be buffered by the current Archive reader.

## Current limits

Only one optional pack can be enabled at a time, and switching requires a
Player restart. There is no in-game toggle UI yet. A signed update chain may
contain at most 64 regular PDX candidates in a
dedicated directory, and ambiguous branches fail closed. A later update must
be authored from its preceding complete distribution; installed overlay files
are not valid `create-overlay` input distributions.

Verification does not rehash unchanged base PDX payloads or independently
validate the target's entire authored dependency graph. A local process that
can concurrently rewrite distribution files can still race verification and
mounting; the base distribution and its Player must be protected by the
normal installation boundary. Large two-version overlay benchmarks and
existing-save update acceptance remain outstanding.
