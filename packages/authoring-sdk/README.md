# PrismatiX Authoring SDK

`@prismatix/authoring-sdk` is the frontend-neutral TypeScript tooling layer for PrismatiX projects. It is used at authoring and build time; the shipped Player does not require Node.js.

The public entrypoint provides:

- strict, bounded validation and deterministic serialization for the canonical PrismatiX JSON formats;
- `.pxstory` parsing with source spans, diagnostics, and recovery;
- project-aware Story compilation to versioned Runtime IR and source maps;
- character alias, expression, variable, resource, label, and extension-command validation;
- project-aware character/resource graph validation matching the native CharacterResources loader;
- structural Story localization validation;
- UI Visual State and shared property-capability validation;
- a React-free JSX runtime and typed UI components that lower to the same canonical UI contracts;
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

## JSX UI authoring

JSX and TSX are build-time syntax only. `Scene`, `VBox`, `HBox`, `Text`, `Image`,
`Button`, and the other typed authoring components produce an in-memory tree;
`compileJsxUi()` lowers that tree to `PrismatiXUIScene` JSON and immediately runs
the existing contract and semantic validator. Functional components are ordinary
functions and do not require React.

```tsx
import {Button, Scene, Text, VBox} from "@prismatix/authoring-sdk";

export default (
  <Scene name="Title" width={1280} height={720}>
    <VBox name="Root" fill style={{token: "menu", background: "#16182a"}}>
      <Text name="Heading" bind={{text: "locale.ui.title"}}>Title</Text>
      <Button name="Start" size={[240, 64]} action="game.start">Start</Button>
    </VBox>
  </Scene>
);
```

Use `jsx: "react-jsx"` and
`jsxImportSource: "@prismatix/authoring-sdk"` in `tsconfig.json`. A project may
point `uiEntryPoints` at `Content/UI/Title.tsx`; `prismatix validate/build/pack`
evaluate it during authoring, normalize the packaged route to
`Content/UI/Title.pxui`, and publish only canonical JSON. Existing `.pxui` and
`.pxuicomponent` JSON sources remain supported without changes. Explicit UUIDs
are an advanced escape hatch; normally the source path plus scene name and each
node's `name`, `stableId`, or JSX `key` provide stable, deterministic UUID seeds.
Duplicate sibling names are deterministically disambiguated. Direct layout props
such as `position`, `size`, `anchor`, `align`, and `fill`, plus CSS-like `style`,
`bind`, and `action` aliases, lower to the unchanged canonical fields.

Local `.jsx`/`.tsx` modules may be imported normally, so authoring components can
live in separate files. `prismatix validate` and `build` construct a real strict
TypeScript program for each authoring module graph and report syntactic and
semantic diagnostics before build-time evaluation. The emitted temporary modules
are deleted afterward and never enter Runtime artifacts.

## Character placement

Story character placement has three readable anchors (`left`, `center`, and `right`) plus
pixel offsets and scale. Offsets are relative to the selected anchor, so a pair can share
the center anchor while sitting close together, while another scene can keep one actor in
the center and one at the side.

```pxstory
[show character=illya position=center x=-150 y=180 scale=0.82]
[show character=miyu position=center x=150 y=180 scale=0.82]

; Move an existing actor to the right anchor and adjust the close-up smoothly.
[move character=miyu position=right x=-80 y=140 scale=0.9 duration=450ms ease=easeInOut wait=true]
```

`x`, `y`, and `scale` remain fully author-controlled. Positive `y` moves a bottom-anchored
sprite downward, which is useful for conventional upper-body visual-novel framing.

## Project-aware character resources

Standalone schema validation cannot prove that a `.pxproject` character descriptor and its `.pxcharacter` document agree, or that expression asset UUIDs exist with a Runtime-compatible kind. Use `validateProjectCharacterResources()` once the project provider has loaded those documents.

```ts
import {validateProjectCharacterResources} from "@prismatix/authoring-sdk";

const validation = validateProjectCharacterResources(
  project,
  {
    "Characters/11111111-1111-4111-8111-111111111111.pxcharacter": yuki,
  },
  {assetExists: (path) => vfs.exists(path)},
);
```

The validator keeps IO outside the SDK and checks descriptor/document identity, global expression identities, runtime lookup ambiguity, expression asset kind/identity, optional asset existence, and the same character alias limits enforced by the native Runtime.

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
