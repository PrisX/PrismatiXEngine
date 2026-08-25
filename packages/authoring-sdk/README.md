# PrismatiX Authoring SDK

`@prismatix/authoring-sdk` is the frontend-neutral TypeScript tooling layer for PrismatiX projects. It is used at authoring and build time; the shipped Player does not require Node.js.

The public entrypoint provides:

- strict, bounded validation and deterministic serialization for the canonical PrismatiX JSON formats;
- `.pxstory` parsing with source spans, diagnostics, and recovery;
- project-aware Story compilation to versioned Runtime IR and source maps;
- character alias, expression, variable, resource, label, and extension-command validation;
- structural Story localization validation;
- UI Visual State and shared property-capability validation;
- explicit migration entrypoints and generated contract hashes.

```ts
import {compileStory, parseStory} from "@prismatix/authoring-sdk";

const story = parseStory(source, "Story/zh-TW/chapter01.pxstory");
const result = compileStory(story, {
  documentId: "chapter01.scene01",
  characters,
  game,
  extensions,
});
```

## Resource identity policy

PrismatiX authoring is **path-first**. Authors may use ordinary project-relative paths such as `Assets/BG/classroom.webp`; the asset catalog is not a virtual filesystem and ordinary assets do not need a UUID merely to be referenced.

Stable asset identities remain available for resources that benefit from rename/move stability or are referenced by identity-bearing authored objects such as character expressions. Those references compile to the canonical `asset:<uuid>` Runtime token and are resolved against `project.pxproject` before VM execution. The Runtime keeps both the stable UUID and the resolved last-known path.

```ts
import {assetToken} from "@prismatix/authoring-sdk";

const stableSprite = assetToken("33333333-3333-4333-8333-333333333333");
// asset:33333333-3333-4333-8333-333333333333
```

This deliberately keeps the authoring surface friendly without removing the existing stable-identity layer used by character resources, runtime asset resolution, packaging, and rename-safe references.

Run `npm run check` at the repository root to verify schemas, generated contract freshness, the TypeScript build, package exports, and contract tests.
