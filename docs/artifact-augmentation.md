# Augmenting a published artifact

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
