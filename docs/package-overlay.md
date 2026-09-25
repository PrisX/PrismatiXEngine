# PDX overlay verification contract (in progress)

The existing `create-patch` / `apply-patch` commands are the supported update
workflow. They assemble and verify a complete new distribution. This document
describes the separate **single-file overlay** groundwork; Player does not
discover or mount `patch.pdx` yet, and no supported command creates one.

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

`PrismatiXPackager --verify-overlay <absolute-base-distribution>
<absolute-overlay-pdx>` checks the base's exact manifest and index bytes,
the game's identity and engine version, target manifest/index consistency,
all added or changed resource sizes and SHA-256 hashes, and that the PDX has
no missing or unexpected entries. URIs present in the base index but absent
from the target index become VFS tombstones. Unchanged resources stay in the
base PDX even when the target's shard assignment differs. A target route,
entry Runtime IR, source map, save migration, or shader artifact omitted from
the target index is rejected.

This verification does **not** rehash unchanged base PDX payloads, verify
the target's full authored dependency graph, or authenticate a publisher.
It is not a safe basis for downloading and automatically executing arbitrary
patches. A production creator must fully verify both distributions and reject
runtime binary changes that a content overlay cannot deliver. Player boot
integration, atomic installation, rollback, and a signed trust policy remain
unfinished.
