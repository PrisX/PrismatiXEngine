# Save migration anchors

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
does not generate or verify a migration. Inspect the actual IDs in the old and
new Runtime IR and map each moved save position deliberately. The Player
checks that the resulting document and exact source/operation pair exist
before restoring a slot. It rejects an unchanged anchor that was removed.
Existence alone cannot prove the operation has the same narrative meaning or
that variables, resources, and routes still make sense. Validate representative
old slots at each changed position in the packaged Player.

`map-or-preserve` requires a Player with this policy implemented. Earlier
Players do not understand it; ship the current Player with the updated content.
