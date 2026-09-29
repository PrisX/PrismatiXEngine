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
- A name that is not an identifier (`@直哉`) is a display name: it speaks as
  the registered character whose `displayName` it is, otherwise as a plain
  label like `@"直哉"`. Character commands (`[show 直哉 ...]`,
  `[act 直哉 nod]`, `[hide 直哉]`) accept the same display names. A quoted
  `@"..."` is always a plain label.
- `@alias[...]` options apply to that line only: a bare word is an expression
  alias or a position (`left`/`center`/`right`), and `voice=` plays with the
  line. Expression and position require a registered character alias.
- A prose line ending in `\` continues on the next line; both lines are shown
  in one advance with a line break. Write `\\` for a literal trailing
  backslash.
- In `[show alias ...]`, words after the alias are the expression or the
  position in either order. Showing a visible character again without a
  position keeps its slot, offset and scale, so expression changes no longer
  snap it back to the center. Giving a position starts a fresh placement:
  omitted `x`, `y`, `scale` and `rotate` return to their defaults.
- `rotate=` turns a character about its centre in degrees (`[show aka z2
  left rotate=-90]` lays it down, as Artemis `fg rotate=` does); an expression
  change keeps the turn. `[move alias rotate=0 duration=1s]` animates it. The
  turn and a turn in progress are saved. A non-numeric value is
  `PXSTORY1155`.

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

### Inline text styles

Dialogue and narration text may change font, size and colour mid-line, next
to `[ruby=...]` and `[br]`:

```pxstory
@直哉「[size=150%]ぎゃあああ[/size]」
[b]Stop[/b], she said, [color=#C04040]quietly[/color].
[font=Assets/Fonts/hand.otf][i]a note in the margin[/i][/font]
@guide [size=75%]（小声で）[/size][ruby=きおく]記憶[/ruby]がない。
```

| Tag | Effect |
| --- | --- |
| `[size=150%]` | percent of the enclosing size, 10%–400%; nested percentages multiply |
| `[size=40]` | absolute size in logical pixels, 1–400 |
| `[color=#RRGGBB]`, `[color=#RRGGBBAA]` | text colour; the control's outline is kept |
| `[font=path]` | a font file by its project-relative path; the build packages it |
| `[b]`, `[i]` | bold and italic, synthesized when the font has no such face |

Each tag ends at its `[/size]`, `[/color]`, `[/font]`, `[/b]` or `[/i]`, or at
the end of the line when left open (`PXSTORY1182`). Closing tags need not nest
strictly: `[/b]` ends the most recent `[b]`. Runs on one line share a
baseline, and a ruby reading is drawn at half its base's size in the base's
colour. The tags take no typewriter step, so revealed text never shows half a
tag; the backlog and saves keep the markup, so a loaded game shows the same
styles. A line may open with a text tag and still be prose.

The Player hides any other bracketed text in dialogue; `prismatix validate`
warns about it (`PXSTORY1180`) and about style tags inside a ruby base, which
are shown as written (`PXSTORY1184`); put them around `[ruby]` instead. An
invalid size or colour is `PXSTORY1181` and a font path outside the project is
`PXSTORY1183`. Styled lines wrap at Unicode line-break opportunities and are
laid out left to right; right-to-left scripts keep to unstyled text.

### Story macros

A macro names a block of Story lines that repeats with small changes. It is
expanded where it is called when the Story compiles, so it needs no
JavaScript (use a `.pxextension` command when the Player itself must do
something new):

```pxstory
[macro scream who line volume=150%]
[se se9022]
[shake 1320ms 6 target=window]
@%who [size=%volume]%line[/size]
[endmacro]

[scream 直哉 「ぎゃああああ」]
[scream who=藍 line=「うるさい！」 volume=200%]
```

- `[macro name ...]` lists parameters: a bare word is required, `name=value`
  has a default. The body runs to the next line that is exactly
  `[endmacro]`; a missing one is `PXSTORY1190`.
- A call passes values in order or by name. In the body `%param` is replaced
  by the value as written and `%%` by `%`; any other `%` is left alone. A value
  used inside a command argument should be quoted there if it may contain
  spaces (`[say speaker=%who text="%line"]`).
- A macro may call other macros; a call cycle is `PXSTORY1194`. Its name may
  not be a built-in command, an extension command or an inline text tag
  (`PXSTORY1191`), parameters must be unique identifiers (`PXSTORY1192`), and
  a call with a missing, extra or unknown value is `PXSTORY1195`. A body cannot
  hold labels, `[id]` or `[macro]` (`PXSTORY1193`): every call would repeat
  them.
- Definitions are local to their Story file and may come after their calls.
- Diagnostics and the source map point at the call; a syntax error inside the
  expanded body names the macro and its body line in the diagnostic details.
- Each expanded operation takes the call's identity plus its line in the body
  (`<call>/m0`, `<call>/m1`, ...). An `[id]` before the call or the file's
  `.pxids.json` entry for it pins the whole expansion, so saves and read-text
  marks survive edits elsewhere. Changing a macro's body renumbers the lines
  of every call, like editing those lines in place.

### Full-screen (NVL) text

`[nvl]` switches from the message window to a full-screen page: each line
that follows is added to the page under the previous ones instead of
replacing them. `[nvl.clear]` starts a new page, and `[adv]` returns to the
message window and discards the page.

```pxstory
[nvl]
桜の季節が、また巡ってきた。
@直哉 「……ああ」
[nvl.clear]
新しい頁。
[adv]
```

- The HUD needs a `RichTextLabel` named `NVLText`. The Player shows it (or a
  group named `NVLPanel` around it) in NVL mode and hides `Dialogue`,
  `Speaker` and `MessageWindow` (or a group named `ADVPanel`); `dialogue.nvl`
  is bindable UI data for anything else that should follow the mode.
- A speaker's line appears as `【name】text`; lines are separated by an empty
  line, the newest one types out like ADV text, and inline styles and ruby
  work as in the message window.
- A line that would overflow `NVLText` at its font size starts a new page by
  itself, so long passages need no manual `[nvl.clear]`.
- Every line also goes to the backlog as usual. The page and the mode are
  saved, restored after loading and rolled back with the line they belong to.

### Message-window face

An expression may name a face image, shown inside the message window while
that character speaks. Give it in the `.pxcharacter` as `facePath` (lowered
to a cataloged `faceAssetId`, like `assetPath`):

```json
{"name": "微笑", "aliases": ["smile"], "assetPath": "Assets/guide.png",
 "facePath": "Assets/Faces/guide-smile.png"}
```

The HUD shows it in an image control named `Face` (hidden when there is no
face and in NVL mode); `dialogue.face` holds the path for other bindings.

```pxstory
@guide 立ち絵がないので、既定の表情の顔。
[show guide sad center]
@guide 顔は立ち絵に合わせて sad になる。
@guide[face=smile] この一行だけ smile の顔。
@guide[face=none] この一行は顔なし。
```

- Without `face=` the face follows the expression shown on stage (the one
  whose image the character is showing), else the character's default
  expression. Expressions that share one image resolve to the first of them.
- `face=` takes an expression alias for that line only, or `none`. An unknown
  expression is `PXSTORY1114`; one without a face is warned about
  (`PXSTORY1163`) and shows none.
- A `player.options` entry with `"binds": "messageFace"` chooses when faces
  appear, as the Artemis `mwface` setting does: 0 never, 1 always (the default
  without such an option), 2 only while the speaker is not on stage.
- The face is part of the line's dialogue state, so saves, loads and rollback
  show the same face.

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

Character actions move a visible character relative to where it stands and
always return it to rest, so they never change its slot or saved placement:

```pxstory
[act guide nod]
[act guide jump size=30 time=250ms loop=2]
[act guide float loop=-1]
@guide[act=hop] わっ！
[act guide stop]
```

Presets are `hop`, `jump`, `startle`, `yawn`, `bow`, `nod`, `nods`,
`tremble`, `shake-head`, `sway`, `float`, `wiggle`, `dip`, `lift`,
`nudge-right`, `nudge-left`, `bob` and `waver`. `size` is pixels (percent
for `startle`/`wiggle`, degrees for `sway`), `time` is one segment (at most
600s), `loop=-1` repeats until `[act alias stop]`, and `ease` is `linear`,
`in`, `out` or `inout`. A looping action is saved and resumes after loading.
The same presets play on the background and image layers (see below).
`wiggle` stretches in anti-phase like Artemis ウゴウゴ: while the width
shrinks by `size` percent the height grows by the same amount, and back.

`[se.stop fade=500ms]` stops every playing sound effect, `[se.stop channel=2]`
only channel 2 (see below), and `[voice.stop]` stops the current voice.
`[shake 300ms 5 target=window]` shakes only the message window layer instead
of the Stage.

With `dir=` a shake follows the Artemis `quake` curve instead: the view jumps
`amplitude` pixels one way, then swings across `count` times (default 10),
each swing 0.85 times the last and rounded up to whole pixels, and settles.
`dir=h` moves horizontally, `dir=v` vertically, and `dir=r` vertically on that
curve and horizontally at twice its rate. Each swing lasts
`duration / (count + 0.5)`, the first half as long, so the Artemis default
(size 6, 60 ms per swing, 10 swings) is:

```pxstory
[shake 630ms 6 dir=r]
[shake 330ms 4 dir=h count=5 target=window]
```

A running shake of either kind is saved with the Stage. A bad `dir` or a
`count` outside 1–100 (or without `dir`) is `PXSTORY1154`.

`[auto on]` makes the Story read itself until `[auto off]`, like Artemis
`autoplay`: `speed=` sets milliseconds per character (0–1000) and `wait=` the
pause after each line (at most 60 s); either left out keeps the player's
setting. `lock=true` also ignores the player: clicks, keys, the HUD menus,
backlog, rollback and skip do nothing until `[auto off]`. Without a lock a
click advances at once and reading continues. `[skip.stop]` ends skipping,
and `[skip.disable]` refuses skip (Ctrl included) until `[skip.enable]`.
A choice ends an auto section, as it would otherwise wait for a click the
lock refuses.
The auto section and the skip lock are saved with the game. Invalid values
are `PXSTORY1156`.

```pxstory
[auto on speed=20 wait=2500ms lock=true]
……静かな夜だった。
[auto off]
```

`[hud visible=false]` hides the message window and HUD until the next line
shows them again; `[hud visible=true]` shows them at once. Both fade over
200 ms unless `time=` says otherwise (`[hud visible=false time=1s]`, at most
10 s, `time=0` switches instantly, as Artemis `msgoff`/`msgon time=`). The
Story does not wait for the fade, and later automatic fades use 200 ms again.

`bgm`, `ambience`, `se` and `voice` accept `volume=0..100`, the level of that
one cue in percent of its bus (the Config volume stays in charge).

Sound effects without a channel overlap freely and are forgotten by saves.
`channel=1..64` gives a cue its own channel, as Artemis `se id=` does: a new
cue on a channel replaces the one playing there, the cue's `volume` applies to
that channel only, and a channel may `loop=true` and `fade` in. A looping
channel is saved with its position and resumes after loading or rollback;
one-shot channel cues are transient like any sound effect. While skipping,
one-shot cues are dropped but loops still start.

```pxstory
[se rain channel=2 loop=true fade=500ms volume=40]
[se door channel=1 volume=60]
; the knock cuts the door off
[se knock channel=1]
; the rain only, then everything (pool and channels)
[se.stop channel=2 fade=1s]
[se.stop]
```

A channel outside 1–64 is `PXSTORY1177`; `loop` and `fade` on an `se` without
a channel stay errors (`PXSTORY1147`, `PXSTORY1146`). The ambience bus remains
for a single background loop that follows the ambience volume setting.

`[flash color=af4154 alpha=0.6 duration=300ms loop=2]` washes the screen with
a colour: it rises to `alpha` and falls back, `loop` times, each taking
`duration`. It is skipped while fast-forwarding.

### Framing, image layers and whole-screen transitions

A background can be framed instead of filling the screen. `x`/`y` place the
image's top-left corner as if it were unscaled, `zoom` and `rotate` apply
about `anchor` (image pixels; the image centre when omitted), and the
`from-*` values start an eased move over `duration`. Repeating `[bg]` with
the same image pans or zooms it; omitted framing values are kept:

```pxstory
[bg street-wide x=-800 y=-473 zoom=0.9 anchor=1920,1126]
[bg street-wide x=-200 duration=4s ease=inOutSine]
```

`[image]` places a named layer with the same framing, plus `alpha` (0–1) and
`z` (below 0 is behind characters, 0 and above in front). A new layer with a
`duration` fades in; `[image.hide name]` or `[image.hide all]` fades out and
removes it:

```pxstory
[image cg1 ev_park_01 x=-300 y=-616 zoom=0.35 anchor=960,947 z=10 duration=400ms]
[image.hide cg1 400ms]
```

`[act]` plays the character presets on the background and on image layers
too: `bg` is the background and `image=` names a layer placed by `[image]`:

```pxstory
[act bg shake-head size=10 time=100ms loop=-1]
[image smoke fog x=-768 zoom=1.5 anchor=1728,540 z=11]
[act image=smoke float size=100 time=5s loop=-1]
[act image=smoke stop]
```

The image keeps its framing. Movement is in image pixels, so it follows the
image's `zoom`; `sway`, `startle` and `wiggle` turn and scale about the
image's `anchor` (its centre when unanchored), and a background that fills the
screen turns about the screen centre. Another image in the layer, or a new
background, ends the action; reframing the same image keeps it running. A
looping action is saved with its layer and resumes after loading. Only one
action runs per target: a new `[act]` replaces the running one from its
current pose. `[act character=bg nod]` still reaches a character aliased
`bg`. An `image=` name that no `[image]` in the same Story shows is a warning
(PXSTORY1176), since another Story may have placed that layer.

`[transition]` reveals every Stage change since the last displayed frame at
once, as a crossfade or through a grayscale rule image. Stage the changes
without their own fades, then transition; the Story waits unless `wait=false`,
and a click or skip moves on:

```pxstory
[hide all transition=false]
[bg classroom transition=false]
[show guide smile center transition=false]
[transition 1s rule=wipe-left vague=40]
```

Rule images resolve from `storyAssets.rule`. `[act window hop size=5]` plays
any character action preset on the message window.

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
Unsupported options on `voice` and on `se` without `channel=`, invalid timing, and unsafe paths are
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

### Player options and confirmation dialogs

`player.options` in `prismatix.json` declares small integer settings the Player
stores with the player's preferences. Each entry has a `default` and either
`min`/`max` or a list of `choices`. An option does nothing on its own; one that
declares `binds` drives a Player behaviour with `value * scale + offset`
(`scale` defaults to 1, `offset` to 0):

```json
"player.options": {
  "textSpeed": {"default": 80, "min": 0, "max": 100,
                "binds": "textSpeed", "scale": -1, "offset": 100},
  "autoSpeed": {"default": 50, "min": 0, "max": 100,
                "binds": "autoWait", "scale": -60, "offset": 6600},
  "windowAlpha": {"default": 70, "min": 0, "max": 100, "binds": "messageWindowOpacity"},
  "font": {"default": 1, "choices": [1, 2, 3], "binds": "font"},
  "askQuickSave": {"default": 0, "choices": [0, 1]}
}
```

Here slider 100 shows text at once (0 ms per character) and each point below
adds a millisecond; auto-reading waits 600 ms at 100 and 6.6 s at 0.

| `binds` | The Player receives |
| --- | --- |
| `textSpeed` | milliseconds per character, 0–120 (0 shows the line at once) |
| `autoWait` | auto-reading wait in milliseconds, 0–60000 |
| `skipUnread` | non-zero: skip also skips unread text |
| `messageWindowOpacity` | message window opacity in percent, 0–100 |
| `dimReadText` | non-zero: grey out text already read |
| `uiEffects` | 0: show screens without transitions |
| `font` | 1-based index into `player.fonts`; other values keep the theme font |
| `keepSkipAfterChoice`, `keepAutoAfterChoice` | non-zero: resume skip / auto-reading after a choice |
| `cursorHideDelay` | hide an idle cursor after this many milliseconds; 0 never hides it |
| `dialogCursorWarp` | non-zero: move the cursor onto the control named `DialogYes` when a dialog opens |
| `dockPinned` | 0: the HUD toolbar (`hud.dockShown`) shows only while the pointer is in `player.dockArea` or a choice is up |
| `messageFace` | 0: never show message-window faces; 2: only while the speaker is off stage; otherwise always |

Behaviours no option binds keep their defaults (the toolbar is pinned, the
cursor never hides). Two options binding one behaviour is warned about
(`PXBUILD1105`); whichever the player set last wins. A misspelled `binds` is a
validation error.

A `Slider` with `runtimeProperties={{optionKey: "autoSpeed"}}` edits an option
and needs no host code. `{id: "option.set", arguments: {argument: "font=2"}}`
sets one, and `options.<name>` (text) and `options.<name>.is<value>` (boolean)
are bindable UI data.

`player.confirmations` lists the Player operations that ask before they run.
Each names the `kind` its dialog shows (the operation's own name when left out)
and, optionally, the option that stores "do not ask again":

```json
"player.confirmations": {
  "quickSave": {"kind": "qsave", "dontAskOption": "askQuickSave"},
  "save": {"kind": "save"},
  "overwrite": {"kind": "save2"},
  "exit": {}
}
```

Operations: `quickSave`, `quickLoad`, `save` (into an empty slot), `overwrite`
(an occupied slot), `load`, `returnToTitle`, `exit`, `backlogJump`,
`previousChoice`, `nextChoice` and `resetSettings`. Several operations may share
a kind. An operation asks only while the mounted route has a control named
`ConfirmDialog` and its `dontAskOption` is 0; unlisted operations run at once.
The dialog reads `dialog.visible`, `dialog.skipNext` and
`dialog.kind.<kind>` (one boolean per declared kind) and answers with
`dialog.accept`, `dialog.cancel` and `dialog.toggleSkip`; accepting with
`dialog.skipNext` set stores 1 in the `dontAskOption`. `prismatix validate`
rejects a `dontAskOption` that `player.options` does not declare
(`PXBUILD1104`), a binding to an undeclared `dialog.kind.<kind>`
(`PXBUILD1100`) and warns about a `ConfirmDialog` when nothing is listed
(`PXBUILD1103`).

Any control can fade out when hidden instead of vanishing:
`showTransitionSeconds`, `hideTransitionSeconds`, `showOffsetY`,
`hideOffsetY` and `linearTransition` are ordinary runtime properties.

### Audio defaults and per-character voice volume

A fresh install (and `settings.reset`) starts from the engine's volumes unless
the project sets its own, as percentages the settings screen shows:

```json
"player.defaultVolumes": {"master": 100, "bgm": 80, "se": 100, "voice": 100},
"player.voiceProfiles": {
  "ids": ["hikari", "guide", "other"],
  "fallbackByFilePrefix": {"npc_": "other"}
}
```

A voice file belongs to the profile named by its folder
(`Assets/Voice/hikari/001.ogg`); files in other folders use the first matching
file-name prefix. A settings screen binds `settings.profileMuted.<id>` and uses
`Slider`s named `VoiceProfile_<id>` with `VoiceProfileValue_<id>` labels; a
muted bus or profile keeps its level and its number reads `mute`.

`Text` accepts `font="Assets/Fonts/Number.otf"` for a label that must not use
the theme font, such as the digits beside a slider.

### Executable name and icon

`prismatix pack` names the game's executable after `distribution`, which is
read only when packaging and never shipped as content:

```json
"distribution": {"executableName": "hikari", "icon": "Assets/Branding/hikari.ico"}
```

The platform extension is kept (`hikari.exe` on Windows). On Windows the
`.ico` becomes the executable's only icon, which Explorer, the taskbar and the
game window use. Changing either value rebuilds the package; the in-place
runtime refresh is skipped for branded executables.

## Localization, voice and release updates

Declare locale resources and `fontChain` in `Content/Localization/<locale>.json`. The starter already includes its Traditional Chinese font. Validate every locale's images/voices and test real text rendering. The Player's live language change requires aligned document/operation identities and types; splitting or merging translated dialogue is not automatically a valid live-switch mapping. Keep topology aligned or redesign the scene/update and test its save anchors. Do not silently omit missing voice to make validation pass.

For an update, retain the previous packaged game and isolated test saves. Increment the intended content/save versions, provide the project's explicit save-migration chain, test dialogue moves and changed resource names/variable schemas, then launch the new packaged Player against copies of those saves. Never experiment on real player saves.

## Save, replay and JavaScript boundaries

Preview Title → Start → HUD exposes Save/Load for session-only checkpoints. Rebuilding or closing Preview clears those slots. Packaged Player Save/Load persists player saves on disk; use Player to test reopening and release migrations.

The runtime has checkpoint state for VM/Stage, routes/UI, backlog and Timeline; pending JavaScript commands and Actions have separate replay journals. Extension state must use a versioned synchronous JSON provider. Arbitrary closures, globals, promises or the entire QuickJS heap are not saved. Use the actual provider API shown in [runtime-sdk/README.md](../runtime-sdk/README.md), including its migration callback.

Checkpoint restore can re-enter command/Action code to reconstruct a suspended continuation. Engine operations are checked against the journal; divergent replay can fail. Keep persistent state in providers, and do not rely on external side effects being exactly-once across load/rollback. Provider capture/restore/migrate callbacks must be deterministic and must not call engine operations.

Full-screen transition playback is transient: loading cancels it and restores the incoming Stage/UI state. Timeline playback has serialized state and await handling; this is not a promise to serialize GPU frames. Audio/video decoder positions and voice replay require the native journey tests for the specific boundary; do not promise sample-exact restoration merely because an asset and logical state are present. Test the specific restore boundaries your game depends on with the native journey tests.

Runtime extensions run in QuickJS, without Node filesystem/process/network modules or unrestricted time/random/eval APIs. Installing a Node package does not make it a runtime dependency. TSX authoring is evaluated during the build in Node and is a separate trust boundary: only build projects and dependencies you trust. Runtime sandbox limits do not make arbitrary build-time project code safe.

