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

## Dynamic resource families

Static Story/UI references and catalog assets are collected automatically. If
the Player selects a file dynamically (for example, a PIMG-derived UI family
or one of several system voices), add project-relative files or directories to
the author manifest:

```json
{"resourceIncludes": ["Assets/UI/Original", "Assets/Audio/Voice/confirm.ogg"]}
```

The CLI expands directories recursively, rejects missing/empty roots and
symbolic links, and copies the files into the owned artifact as normal
dependencies. `resourceIncludes` is build-time metadata and is removed from
the canonical `project.pxproject` shipped to the Player. Direct
`compileProject` callers must provide `resolveResourceInclude(root)` returning
the file paths under each declared root; without a resolver compilation fails
instead of silently omitting media. This is for runtime assets, not TSX source
or generated UI documents. Use the asset catalog when a resource needs a
stable UUID across renames.

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

`compileStory` compiles one document and resolves only its local labels. For
`[jump scene=...]`, `[call scene=...]`, or `[choice scene=...]`, use
`compileStoryProject` with a Story index and all referenced scene sources;
single-document compilation reports `PXSTORY1215` for these commands.

For large Story indexes, `compileStoryProject({..., onProgress})` reports each
completed scene in its `parse` and `compile` phases with locale, scene ID,
source path and per-locale `current`/`total`. `compileProject({...,
onStoryProgress})` forwards those events and adds `globalCurrent`/`globalTotal`
across locales. Callbacks are synchronous; a missing or invalid scene still
advances the count, while diagnostics remain in the compile result. The CLI
uses this API for `validate`, `dev` and builds: human output is throttled, and
`--json` streams every scene as a `PXCLI1103` progress record on stderr.

## Readable Story with durable save identities

An ordinary `.pxstory` needs no manually authored id. New projects from
`prismatix create` start with an external identity map already prepared.
When save/seen identity
must survive text edits, `prismatix ids my-game --write` creates a
recoverable source backup, removes existing `[id ...]` directives, and writes
an author-only `<source>.pxids.json` sidecar. The Story keeps one short
`; @prismatix-ids` marker so a missing sidecar fails the build instead of
silently changing save anchors. The CLI loads adjacent maps automatically;
direct `compileStoryProject` callers must include each sidecar's text in
`sourceFiles` under its exact `<source>.pxids.json` path. Sidecars do not ship
in the Runtime artifact.

Node callers of the lower-level `compileProject` API can use
`await loadStorySourceFiles(projectRoot, storyIndex)` to read all indexed
`.pxstory` sources and any adjacent `.pxids.json` maps into its `sourceFiles`
input. Missing required Story files, oversized files, and paths escaping the
project root fail at load time. Browser/Editor callers can continue supplying
their own `sourceFiles` map without Node filesystem access.

The map preserves both authored ids and the exact pre-migration fallback
`sourceId` of operations that had no `[id]`. In-place prose edits, comments,
and blank lines retain those identities. For added or removed executable
operations, run `prismatix ids my-game --sync` to preview the proposed map,
then `prismatix ids my-game --sync --write` to back up and update only the
sidecars. `--sync --check` exits nonzero when an update is pending. Existing
operation ids survive unambiguous edits; deleted identities are listed so you
can review save-migration implications. The synchronizer refuses reordered,
duplicated, or otherwise ambiguous edits rather than guessing a save anchor.
Changing an operation's kind/command also needs manual review. Validate the
project and test old saves before distributing an update; stable ids alone do
not prove save compatibility. `--external` remains an explicit spelling of the
default. `--inline` is available only for legacy projects that deliberately
want `[id ...]` in Story source; it cannot be mixed with external sidecars.

## Player presentation crossfades

Set Player presentation effects in the project `settings` (seconds, from 0 to
10; `0` settles immediately):

```json
{
  "player.presentationTransitions": {
    "routePreset": "crossfade",
    "routeDuration": 0.3,
    "splashPreset": "crossfade",
    "splashDuration": 0.3,
    "titlePreset": "crossfade",
    "titleDuration": 0.5,
    "routeOverrides": [
      {"from": "title", "to": "settings", "preset": "slide-left", "duration": 0.4}
    ]
  },
  "player.splashes": [
    {"scene": "Content/UI/Publisher.pxui"},
    {"scene": "Content/UI/Studio.pxui", "transitionPreset": "crossfade",
      "transitionDuration": 0.5}
  ]
}
```

`routePreset`/`routeDuration` apply to Player menu routes and confirmed load
presentation; `splashPreset`/`splashDuration` apply between splash screens;
`titlePreset`/`titleDuration` apply from the final splash to the title. A splash
entry can override the effect or duration used when entering that screen.
`routeOverrides` selects an exact outgoing/incoming Player route pair; unknown
routes and duplicate pairs fail validation. Available built-in presets are
`none`, `fade`, `crossfade`, `slide-left`, and `slide-right`. `fade` passes
through the clear colour, while `crossfade` overlays the incoming frame on
the outgoing frame. Omitted settings preserve previous Player defaults
(including the 0.5-second transition into the second splash). Custom shader
transitions and a fully declarative route graph remain separate authoring gaps.

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

`TextureButton` accepts `normalPath`, `hoverPath`, `pressedPath`,
`disabledPath`, and `scaleMode` directly. `RichTextLabel` accepts ruby/markup
through `markup` (or text children), plus `vertical` and `verticalRows`. Both
can use the common `focusMode`, `hoverSoundPath`, and `activateSoundPath`
props inherited by UI nodes. `Image` accepts `path`, `scaleMode`,
`lockAspectRatio`, and `opacity` directly; `Text` and `RichTextLabel` accept
`fontSize`, wrapping, outline size/color, and text alignment. `Button` and
`TextureButton` accept caption font size and alignment. An outline color string
such as `"#2D171DFF"` is lowered to the native Color value. These typed props
override an identically named entry in
`runtimeProperties`, which remains available for less common native properties.

Bindings can address array items and object fields, for example
`bind={{text: "saveLoad.slots[0].title"}}`. The native ViewModel must define
`saveLoad.slots` as an array containing an object with a string `title` before
the UI is installed; replacing that array refreshes subscribed item bindings.
Indexes are zero-based and cannot have leading zeroes. TSX may generate a fixed
set of controls with `Array.from`, but this is not yet a runtime keyed repeater:
changing the array length does not create or remove controls. A replacement
that removes an actively bound item or changes its bound value type is rejected
instead of leaving stale UI visible.

When a native UI TypeRegistry manifest is available, use
`validateUiDocument(ui, registry)` to check writable/bindable properties and
their declared `valueType` before packaging. For example, a Color property
must receive `{type: "color", value: "#2D171DFF"}`, not the plain string
`"#2D171DFF"`. `validateDocument("ui", ui)` checks the canonical document
but has no native property metadata, so it cannot catch that type mismatch.
`compileProject({..., uiTypeRegistry: registry})` applies the same check to all
authored UI routes and reusable components.

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

For a multilingual Story, keep one stable character alias and optionally give
its `.pxcharacter` document localized speaker names:

```json
{"displayName":"村雨","displayNames":{"ja-JP":"ムラサメ","en-US":"Murasame"},"aliases":["murasame"]}
```

The other required character fields are omitted from this excerpt. `@murasame`
then emits the locale's `speaker` text while retaining the same `character` ID
and save/seen operation IDs. Missing locales use `displayName`. The project
compiler allows translated speaker text but still rejects changes to actor
identity or control flow across locales. This affects authored Story dialogue;
it does not localize arbitrary native/JavaScript calls that bypass Story.

For a displayed speaker with no character resource, quote the name after `@`:

```pxstory
@"村雨"
早安，主人。
```

This emits a `dialogue` operation with separate `speaker` and `text` fields and
an empty character identity. Each locale may translate the quoted speaker.
Unlike `@murasame`, it does not select a portrait, voice directory, or other
character-catalog resource. Existing narration cannot be converted to dialogue
in a shipped game without reviewing save/seen identity migration.

## Project-aware character resources

Layered expression sprites have a typed, validated `.pxportrait` authoring
contract. See [layered portrait authoring](../../docs/portrait-authoring.md)
for the helper, coordinate semantics, and dependency rules.

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
