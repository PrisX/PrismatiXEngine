# Save migration anchors

## Same-version releases

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

## Anchors

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
