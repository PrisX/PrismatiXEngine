# Story author cookbook

These examples use the public Story compiler in `packages/authoring-sdk/src/story.ts`, not guessed spellings of runtime JavaScript methods. Start from a generated project and run `prismatix validate my-game` after changing declarations. Asset examples require the referenced file to exist; an explicit catalog entry is needed only when you want to pin a stable asset identity yourself.

## Rewrite, branch and preserve identities

```pxstory
*start
@guide
今天想去哪裡呢？

[choice text="看看星星" goto=stars]
[choice text="聽個故事" goto=story]
[choice.wait]
*stars
第一顆星亮起了。
[jump target=finish]
*story
很久很久以前，有一個小小的願望。
*finish
[end]
```

An empty line ends speaker scope. Labels use `*name`; choices use `text` and `goto`, jumps use `target`. A generated project's adjacent `.pxids.json` keeps save identities without `[id ...]` in the prose. Prose rewrites retain their identities; for added or removed executable lines, review `prismatix ids <project> --sync` and then run `--sync --write`. Removing an old operation requires `--accept-removals` when writing the sidecar, after reviewing old saves and migration. Do not delete the Story's `; @prismatix-ids` marker or the sidecar. Changing routes, resources or variable schemas still requires a deliberate migration test against old saves. Identity stability does not by itself prove compatibility.

## Short forms for long scripts

Every short form below compiles to the same operations as its long form, so
save identities and the Runtime IR do not depend on which spelling you use.

```pxstory
@直哉「うっぷ……少し食い過ぎたか……」
@guide 今天想去哪裡呢？
@guide[smile left voice=g0042] 一起去吧！
あれめんどくさいんだよなぁ。\
他で返せたら、そっちでチャラにしたいものだ。
[show guide smile right]
[bg street rule=083r duration=1000ms vague=40]
[bgm bgm004]
```

- `@name text` speaks one line and returns to narration; `@name` on its own
  line still starts a block that ends at the next empty line. Text may follow
  the name directly when it opens with `「`, `『`, `（`, `(` or `“`.
- A name that is not an identifier (`@直哉`) is a display name, like
  `@"直哉"`; only identifier names are checked against character aliases.
- `@alias[...]` options apply to that line only: a bare word is an expression
  alias or a position (`left`/`center`/`right`), and `voice=` plays with the
  line. Expression and position require a registered character alias.
- A prose line ending in `\` continues on the next line; both lines are shown
  in one advance with a line break. Write `\\` for a literal trailing
  backslash.
- In `[show alias ...]`, words after the alias are the expression or the
  position in either order. Showing a visible character again without a
  position keeps its slot, offset and scale, so expression changes no longer
  snap it back to the center.

Short resource names come from author-only search roots in `prismatix.json`:

```json
"storyAssets": {
  "bg": ["Assets/bg", "Assets/cg"],
  "rule": ["Assets/rule"],
  "sprite": ["Assets/Portraits"],
  "voice": ["Assets/voice"],
  "bgm": ["Assets/bgm"],
  "se": ["Assets/se"]
}
```

Kinds are `bg`, `rule`, `sprite`, `voice`, `bgm`, `se`, `ambience` and `video`.
A name is looked up only among files the build already packages
(`resourceIncludes` and cataloged `assets`). Roots are tried in order; inside
one root an exact relative path wins over a path without its extension, then a
file name, then a file name without its extension (`kei/k001` or `k001` for
`Assets/voice/kei/k001.ogg`). Several matches at the same level are an error
(`PXSTORY1161`) listing every candidate rather than a guess, and a bare name
with no match is `PXSTORY1160` instead of a missing project-root file. Values
containing `/` or an extension that match no root still work as ordinary
project paths.

On the imported 352-scene *Sakura no Uta* port, rewriting `[say]`/`[narrate]`,
separate `[voice]` lines and full media paths into these forms removes about
35% of the script's characters without changing any operation.

## Characters, backgrounds and sound

The starter registers `guide`, its `smile` expression and an original background. Reuse these without hand-writing another UUID:

```pxstory
[bg Assets/window.png]
[show guide expression=smile position=left x=0 y=0 scale=0.8]
@guide
這裡就是新的背景。
[move guide position=right duration=400 wait=true]
[hide guide]
```

Use `[hide all]` to fade out every character without listing its aliases.
`[hide all transition=false]` removes them immediately. Neither form clears
the background or independent stage layers. The same operation is available
to runtime scripts as `Engine.ClearCharacters(false)` or through the typed
runtime SDK as `ctx.stage.clearCharacters(false)`.

To add an expression, add a source image and an expression with `assetPath` in `Characters/guide.pxcharacter`, then select its alias with `expression=...`. The Authoring project compiler generates canonical character, expression, and asset UUIDs when omitted; no catalog entry is needed for that image. For example, `{"name":"Happy","aliases":["happy"],"assetPath":"Assets/guide-happy.png"}` is enough inside `expressions`. TypeScript generators can use the exported `CharacterSourceDocument` type; the stricter `CharacterDocument` type describes the published canonical file. Validate this shorthand with `compileProject` or `prismatix validate`, not the low-level `validateDocument("character")` canonical checker. The generated IDs derive from the project, character source path, expression's first alias, and image path. Keep those stable across save-compatible releases, or pin `id`/`assetId` explicitly before renaming; a file move is not automatically a save migration. Positions are `left`, `center`, `right` (also 1/2/3). The move duration is milliseconds in the native session implementation.

For a dynamically selected family of UI/media files, put the containing
directory in the project's `resourceIncludes`, for example
`"resourceIncludes": ["Assets/UI/Original"]`. UI controls can still use
ordinary paths; do not also list every file in `assets` unless it needs a
stable catalog identity. `prismatix validate` reports `catalogIncludeOverlap`
so large duplicate inventories are visible. Overlap is not automatically an
error: an ID referenced by a gallery or character may still be necessary.

After registering suitable decodable media, the compiler accepts:

```pxstory
[bg Assets/garden.png]
[bgm Assets/evening-loop.wav intro=Assets/evening-intro.wav fade=250ms]
[ambience Assets/rain.wav loop=false fade=100ms]
[se Assets/bell.wav]
[voice Assets/greeting.wav]
@guide
歡迎回來。
```

`bgm` and `ambience` accept `fade` (milliseconds by default, or a `ms`/`s`
suffix) and `loop=true|false`. `bgm intro=...` plays the intro once, then
loops its main asset; it cannot be combined with `loop=false`. Both BGM files
are collected for packaging. `bgm.stop` and `ambience.stop` also accept `fade`.
Unsupported options on `voice` and `se`, invalid timing, and unsafe paths are
authoring errors instead of silently changing playback. Use the typed runtime
API in an extension for volume and multi-track control. A missing reference is
a build diagnostic, while decode/device failures need native testing with real
media.

For startup screens and title voices, declare all random candidates in the
project's `settings` instead of duplicating them in `resourceIncludes`:

```json
"settings": {
  "player.titleVoiceVariants": ["Assets/Voice/title-a.ogg", "Assets/Voice/title-b.ogg"],
  "player.splashes": [{
    "scene": "Content/UI/Logo.pxui",
    "audioVariants": ["Assets/Voice/logo-a.ogg", "Assets/Voice/logo-b.ogg"],
    "audioDelay": 1,
    "minimumDuration": 4
  }]
}
```

The splash scene must be listed in `uiEntryPoints`; for a TSX-authored route,
use its `.tsx` source path in both places and the CLI publishes the compiled
`.pxui` path. The compiler collects
*every* candidate for packaging and reports missing media when the project
resolver is available; the Player chooses one available candidate at runtime.
`audio` remains available for a single splash sound. The project schema checks
these known settings, including paths and timing, before a distribution build.

## Variables and conditional flow

Declare a catalog, for example by setting `"gameCatalog":"Content/game.pxgame"` in the project and creating:

```json
{"format":"PrismatiXGame","schemaRevision":2,"variables":[{"name":"affection","type":"integer","default":0,"scope":"session"}],"gallery":[],"unlockables":[]}
```

```pxstory
[set affection=1]
[if affection > 0]
小光記得這個約定。
[else]
故事才剛開始。
[endif]
```

Use `session` for story state that belongs to a checkpoint; use `profile` intentionally for cross-session progression. Do not treat profile unlocks or preferences as ordinary rollback variables. The compiler checks declared variable names and assignment types.

## UI actions and Timeline

The template's title button contains this actual Action descriptor:

```json
{"id":"load.open","arguments":{}}
```

`game.start`, `save.open`, `load.open`, `backlog.open` and `settings.open` are built-in Player actions used by the default UI. Keep scene `onClick` Actions distinct from Story UI operations:

```pxstory
[ui route=save operation=push]
```

Extension-declared Actions use the parameter names and types in their `.pxextension` manifest. For a `resource` parameter, wrap a path so it reaches the Runtime as a ResourceRef, not an ordinary string:

```json
{"id":"music.play","arguments":{"media":{"type":"resource","value":"Assets/Audio/theme.ogg"},"volume":0.8}}
```

The same typed UI argument bridge accepts `color`, `uuid`, `token`, `vec2`, `rect`, arrays and objects. `prismatix validate` checks extension-declared Action arguments (required names, types, enum choices and ranges) and includes static resource paths, including manifest resource defaults, in the artifact. Actions registered only by a Player/host at runtime have no authoring manifest to check and remain runtime-validated. A bare string path is still a string, so it cannot satisfy a declared `resource` parameter.

A registered `.pxtimeline` can be played using its resource reference:

```pxstory
[timeline Animations/greeting.pxtimeline]
```

Create the actual clip with typed authoring contracts or adapt the existing sample's Timeline. Story's `timeline` accepts the clip reference; it does not expose every runtime playback option. For image-layer animations in UI, see [`.pxmotion` authoring](motion-authoring.md); for image-based lettering without a node per possible character, see [SpriteText](sprite-text-authoring.md). For keyboard shortcuts, see [named input actions](input-actions.md). See also [the React-free TSX example](examples/jsx-ui-authoring.tsx), [custom effects](custom-effects.md), and [the runtime SDK](../runtime-sdk/README.md) for richer authored UI and presentation APIs.

## Localization, voice and release updates

Declare locale resources and `fontChain` in `Content/Localization/<locale>.json`. The starter already includes its Traditional Chinese font. Validate every locale's images/voices and test real text rendering. The Player's live language change requires aligned document/operation identities and types; splitting or merging translated dialogue is not automatically a valid live-switch mapping. Keep topology aligned or redesign the scene/update and test its save anchors. Do not silently omit missing voice to make validation pass.

For an update, retain the previous packaged game and isolated test saves. Increment the intended content/save versions, provide the project's explicit save-migration chain, test dialogue moves and changed resource names/variable schemas, then launch the new packaged Player against copies of those saves. Never experiment on real player saves.

## Save, replay and JavaScript boundaries

Preview Title → Start → HUD exposes Save/Load for session-only checkpoints. Rebuilding or closing Preview clears those slots. Packaged Player Save/Load persists player saves on disk; use Player to test reopening and release migrations.

The runtime has checkpoint state for VM/Stage, routes/UI, backlog and Timeline; pending JavaScript commands and Actions have separate replay journals. Extension state must use a versioned synchronous JSON provider. Arbitrary closures, globals, promises or the entire QuickJS heap are not saved. Use the actual provider API shown in [runtime-sdk/README.md](../runtime-sdk/README.md), including its migration callback.

Checkpoint restore can re-enter command/Action code to reconstruct a suspended continuation. Engine operations are checked against the journal; divergent replay can fail. Keep persistent state in providers, and do not rely on external side effects being exactly-once across load/rollback. Provider capture/restore/migrate callbacks must be deterministic and must not call engine operations.

Full-screen transition playback is transient: loading cancels it and restores the incoming Stage/UI state. Timeline playback has serialized state and await handling; this is not a promise to serialize GPU frames. Audio/video decoder positions and voice replay require the native journey tests for the specific boundary; do not promise sample-exact restoration merely because an asset and logical state are present. See the [audit](releases/0.2.0-audit.md) for tested versus untested restore combinations.

Runtime extensions run in QuickJS, without Node filesystem/process/network modules or unrestricted time/random/eval APIs. Installing a Node package does not make it a runtime dependency. TSX authoring is evaluated during the build in Node and is a separate trust boundary: only build projects and dependencies you trust. Runtime sandbox limits do not make arbitrary build-time project code safe.

