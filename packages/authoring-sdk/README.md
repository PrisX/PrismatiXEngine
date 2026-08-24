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

Run `npm run check` at the repository root to verify schemas, generated contract freshness, the TypeScript build, package exports, and contract tests.
