# Story author cookbook

These examples use the public Story compiler in `packages/authoring-sdk/src/story.ts`, not guessed spellings of runtime JavaScript methods. Start from a generated project and run `prismatix validate my-game` after changing declarations. Asset examples require the referenced file to exist; a catalog entry is needed when you want a stable asset identity or a character expression references it.

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

An empty line ends speaker scope. Labels use `*name`; choices use `text` and `goto`, jumps use `target`. A generated project's adjacent `.pxids.json` keeps save identities without `[id ...]` in the prose. Prose rewrites retain their identities; for added or removed executable lines, review `prismatix ids <project> --sync` and then run `--sync --write`. Do not delete the Story's `; @prismatix-ids` marker or the sidecar. Changing routes, resources or variable schemas still requires a deliberate migration test against old saves. Identity stability does not by itself prove compatibility.

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

To add an expression, add an asset descriptor and an expression in `Characters/guide.pxcharacter`, then select its alias with `expression=...`. Positions are `left`, `center`, `right` (also 1/2/3). The move duration is milliseconds in the native session implementation.

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
[bgm Assets/evening.wav]
[se Assets/bell.wav]
[voice Assets/greeting.wav]
@guide
歡迎回來。
```

These Story audio commands lower an asset reference only. Do not copy JavaScript volume/fade/bus options into them and assume those options take effect. Use the typed runtime API in an extension when you need its richer audio controls. A missing reference is a build diagnostic, while decode/device failures need native testing with real media.

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

Create the actual clip with typed authoring contracts or adapt the existing sample's Timeline. Story's `timeline` accepts the clip reference; it does not expose every runtime playback option. See [the React-free TSX example](examples/jsx-ui-authoring.tsx), [custom effects](custom-effects.md), and [the runtime SDK](../runtime-sdk/README.md) for richer authored UI and presentation APIs.

## Localization, voice and release updates

Declare locale resources and `fontChain` in `Content/Localization/<locale>.json`. The starter already includes its Traditional Chinese font. Validate every locale's images/voices and test real text rendering. The Player's live language change requires aligned document/operation identities and types; splitting or merging translated dialogue is not automatically a valid live-switch mapping. Keep topology aligned or redesign the scene/update and test its save anchors. Do not silently omit missing voice to make validation pass.

For an update, retain the previous packaged game and isolated test saves. Increment the intended content/save versions, provide the project's explicit save-migration chain, test dialogue moves and changed resource names/variable schemas, then launch the new packaged Player against copies of those saves. Never experiment on real player saves.

## Save, replay and JavaScript boundaries

Preview Title → Start → HUD exposes Save/Load for session-only checkpoints. Rebuilding or closing Preview clears those slots. Packaged Player Save/Load persists player saves on disk; use Player to test reopening and release migrations.

The runtime has checkpoint state for VM/Stage, routes/UI, backlog and Timeline; pending JavaScript commands and Actions have separate replay journals. Extension state must use a versioned synchronous JSON provider. Arbitrary closures, globals, promises or the entire QuickJS heap are not saved. Use the actual provider API shown in [runtime-sdk/README.md](../runtime-sdk/README.md), including its migration callback.

Checkpoint restore can re-enter command/Action code to reconstruct a suspended continuation. Engine operations are checked against the journal; divergent replay can fail. Keep persistent state in providers, and do not rely on external side effects being exactly-once across load/rollback. Provider capture/restore/migrate callbacks must be deterministic and must not call engine operations.

Full-screen transition playback is transient: loading cancels it and restores the incoming Stage/UI state. Timeline playback has serialized state and await handling; this is not a promise to serialize GPU frames. Audio/video decoder positions and voice replay require the native journey tests for the specific boundary; do not promise sample-exact restoration merely because an asset and logical state are present. See the [audit](releases/0.2.0-audit.md) for tested versus untested restore combinations.

Runtime extensions run in QuickJS, without Node filesystem/process/network modules or unrestricted time/random/eval APIs. Installing a Node package does not make it a runtime dependency. TSX authoring is evaluated during the build in Node and is a separate trust boundary: only build projects and dependencies you trust. Runtime sandbox limits do not make arbitrary build-time project code safe.

