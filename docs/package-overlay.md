# PDX overlay verification contract (in progress)

The existing `create-patch` / `apply-patch` commands are the supported update
workflow. They assemble and verify a complete new distribution. A separate
**single-file content overlay** can now be built and checked:

```sh
prismatix create-overlay --base path/to/v1 --target path/to/v2 --output path/to/patch.pdx
prismatix verify-overlay --base path/to/v1 --patch path/to/patch.pdx
```

The native Packager also accepts `--create-overlay` and `--verify-overlay`
with absolute paths. Player does **not** discover or mount `patch.pdx` yet.

An overlay is an unencrypted PDX file readable by the current Archive runtime
(PDX4 or PDX5) whose virtual resources are exactly:

- `__prismatix_overlay__/manifest.json` (at most 16 KiB)
- `__prismatix_overlay__/target-manifest.json` (at most 4 MiB)
- `__prismatix_overlay__/target-content-index.json` (at most 128 MiB)
- every added or changed target resource, by its original virtual URI

The first document has `format: "PrismatiXPackageOverlay"`,
`schemaRevision: 1`, `gameId`, `basePackageFingerprint`,
`targetPackageFingerprint`, and SHA-256 fields named `baseManifestSha256`,
`baseContentIndexSha256`, `targetManifestSha256`, and
`targetContentIndexSha256`. The two target documents are copies of the
Packager-produced `Package/manifest.json` and `Package/content-index.json`.
The three `__prismatix_overlay__/` names are reserved and cannot be ordinary
package resources.

`verify-overlay` checks the base's exact manifest and index bytes,
the game's identity and engine version, target manifest/index consistency,
all added or changed resource sizes and SHA-256 hashes, and that the PDX has
no missing or unexpected entries. URIs present in the base index but absent
from the target index become VFS tombstones. Unchanged resources stay in the
base PDX even when the target's shard assignment differs. A target route,
entry Runtime IR, source map, save migration, or shader artifact omitted from
the target index is rejected.

`create-overlay` first fully verifies both distributions' PDX contents. It
rejects a changed Player or other non-content runtime file, extracts only
new or changed target resources, writes an uncompressed PDX5 using streaming
file input and a source SHA-256 check, verifies the result, and publishes a
new output without replacing an existing path. It reports verification,
extraction, and writing progress. Extraction temporarily uses disk space for
the changed resources in addition to the output PDX; a large compressed
source entry may still be buffered by the current Archive reader.
The creator also rejects a distribution whose manifest or content index changes
between preflight verification and publication. This narrows an update race;
it does not make a concurrently writable distribution trustworthy or replace
publisher authentication.

`verify-overlay` does **not** rehash unchanged base PDX payloads, verify the
target's full authored dependency graph, or authenticate a publisher. The
creator's full base/target verification is not a substitute for a signed
update channel. Player boot integration, safe drop-in discovery, rollback,
and a publisher trust policy remain unfinished. Do not assume placing this
file beside a game applies it.

For future Player auto-loading, the chosen policy is to pin a publisher public
key in the trusted base distribution and require a valid signature plus the
existing exact-base checks before mounting an overlay. The signing format,
key provisioning, and Player integration are not implemented yet. A distribution
can now pin an uncompressed SEC1 P-256 public key by passing
`--publisher-key path/to/public-key.txt` to `prismatix pack` or a distribution
`build`. The file contains one lowercase hex point (`04` followed by 128 hex
characters). The key enters `Package/manifest.json` and the package
fingerprint; overlay creation and verification reject a target that changes
it. This only records the expected publisher key. It does not sign an overlay
or make the current unsigned overlay safe to auto-load. Existing
unsigned overlays remain explicit verification artifacts, not auto-loaded
updates.
