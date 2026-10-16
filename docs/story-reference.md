# Story reference

Everything a `.pxstory` script can say, how scenes fit together, and the
assets the Stage shows. Start with [Getting started](getting-started.md) if
you have not made a project yet. Run `prismatix validate my-game` after
changing declarations; diagnostics name the line and the code (`PXSTORY…`).

- [Scripts, labels and choices](#scripts-labels-and-choices)
- [Keeping saves working while you edit](#keeping-saves-working-while-you-edit)
- [Short forms and asset names](#short-forms-and-asset-names)
- [Text](#text): styles, ruby, typewriter timing, NVL pages, staff roll, faces
- [Macros](#macros)
- [Characters, backgrounds and sound](#characters-backgrounds-and-sound)
- [Variables and conditions](#variables-and-conditions)
- [Scenes and the story index](#scenes-and-the-story-index)
- [Importing a writers' draft](#importing-a-writers-draft)
- [Layered portraits](#layered-portraits)
- [Camera and layered motion](#camera-and-layered-motion)
- [Scripts, saves and rollback](#scripts-saves-and-rollback)

## Scripts, labels and choices

```pxstory
*start
@guide
今天想去哪裡呢？

[choice]
- 看看星星 -> stars
- 聽個故事 -> story
[endchoice]
*stars
第一顆星亮起了。
[jump finish]
*story
很久很久以前，有一個小小的願望。
*finish
[end]
```

An empty line ends speaker scope. Labels use `*name`; each `- text -> label` line is one choice (the long form is `[choice text="看看星星" goto=stars]`), and `[jump finish]` is short for `[jump target=finish]`.

A generated project's adjacent `.pxids.json` keeps save identities without `[id ...]` in the prose. Prose rewrites retain their identities; for added or removed executable lines, review `prismatix ids <project> --sync` and then run `--sync --write`. Removing an old operation requires `--accept-removals` when writing the sidecar, after reviewing old saves and migration. Do not delete the Story's `; @prismatix-ids` marker or the sidecar. Changing routes, resources or variable schemas still requires a deliberate migration test against old saves. Identity stability does not by itself prove compatibility.

## Keeping saves working while you edit

`create` already prepares an identity sidecar for the starter Story. For an
older or separately authored Story with save/seen history, run
`prismatix ids my-game --write` once to move line-by-line `[id]`
anchors into adjacent `.pxstory.pxids.json` files while keeping the prose
readable. External sidecars are the default; `--inline` is an explicit legacy
mode. The command backs up original Story sources. `prismatix ids my-game
--check` then audits coverage. Do not delete a sidecar: the source marker makes
the compiler reject a missing one rather than silently changing save ids.
After adding or removing executable Story lines, preview the identity update
with `prismatix ids my-game --sync`. The JSON report includes each new
operation's Story path, one-based line and short source preview, plus each
removed anchor's original operation position and identity. Check these against
your edit; if the preserved/added/removed operations match your intent, use
`--sync --write` to update only the sidecars with a backup. When old anchors
were removed, the write is refused until you review their effect on old saves;
then pass `--sync --write --accept-removals` to acknowledge those removals.
This flag does not migrate saves or prove compatibility. Reorders and ambiguous duplicate lines require manual identity
review. For a rewrite immediately beside an insertion, sync the rewrite first,
then insert and sync again; a combined edit cannot always be safely matched
because the sidecar stores fingerprints, not the old prose. Test existing saves
before shipping a revised story. For explicit cross-version anchor changes,
see [save migration anchors](shipping.md#save-migration-anchors), including the sparse
`map-or-preserve` policy for large Stories.

## Short forms and asset names

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
- An expression is addressed by its name or any of its aliases
  (`[show kei smile]` for an expression named `smile`).
- Command arguments are separated by ASCII spaces only, so a CJK title keeps
  its ideographic space, and `[chapter 第一章 天台]` takes every word as the
  chapter title (shown in the HUD notice and in save slots).
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
A name is looked up among the files under these roots (and among
`resourceIncludes` and cataloged `assets`); only the files Story actually
names are packaged, so a root needs no matching `resourceIncludes` entry.
Roots are tried in order; inside
one root an exact relative path wins over a path without its extension, then a
file name, then a file name without its extension (`kei/k001` or `k001` for
`Assets/voice/kei/k001.ogg`). Several matches at the same level are an error
(`PXSTORY1161`) listing every candidate rather than a guess, and a bare name
with no match is `PXSTORY1160` instead of a missing project-root file. Values
containing `/` or an extension that match no root still work as ordinary
project paths.

## Text

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

Three more tags only time the typewriter:

```pxstory
@直哉「待って[w 500ms]……[speed 50%]ゆっくり[/speed]、[nowait]一気に！[/nowait]」
[w 1s]間を置いてから話す。
```

| Tag | Effect |
| --- | --- |
| `[w 500ms]`, `[w 1.5s]` | pause before the next character, 0–60s (`ms` when no unit) |
| `[speed 50%]`…`[/speed]` | reveal at 10%–1000% of the player's text speed; nested tags use the innermost |
| `[nowait]`…`[/nowait]` | show the enclosed text at once |

The Player removes them from the text it shows, so the backlog, NVL pages
and saves hold the plain line, and a click still shows the whole line at
once. Instant text speed ignores them. A malformed pause or speed is
`PXSTORY1185`; an unclosed `[speed]` or `[nowait]` lasts to the end of the
line (`PXSTORY1182`). A line may open with one of them and still be prose.
They are not yet supported inside a ruby base (`PXSTORY1184`).

The Player hides any other bracketed text in dialogue; `prismatix validate`
warns about it (`PXSTORY1180`) and about style tags inside a ruby base, which
are shown as written (`PXSTORY1184`); put them around `[ruby]` instead. An
invalid size or colour is `PXSTORY1181` and a font path outside the project is
`PXSTORY1183`. Styled and ruby lines wrap at Unicode line-break opportunities
with the same 禁則 as plain text, and are laid out left to right;
right-to-left scripts keep to unstyled text.


### Text wrapping and missing glyphs

Wrapped text breaks at Unicode line-break opportunities with 禁則: closing
punctuation and small kana never start a line and opening brackets never end
one. The message window decides the breaks on the whole line, so characters
do not jump to the next line while the typewriter reveals them.

`validate` and `build` warn (`PXCLI1060`) about characters that no font of a
locale's `fontChain` can draw, listing each with its first use: Story text
(dialogue, speakers, choices, chapter titles), UI text (TSX `text`, button
captions and `TextField` placeholders, checked in every locale, since UI text
shows whatever the language) and the locale's own `strings`. A node's own
`font` and `player.uiFont` are tried first; `SpriteText` draws image glyphs
and is skipped. Add a covering font to the chain (for example a Japanese font
for 凪 in a Traditional Chinese project).

### Speaker name colours

`"nameColor": "#F4BFD0"` in a `.pxcharacter` colours that character's name in
the HUD's `Speaker` label while they speak; other lines keep the label's own
colour. Lines already read are drawn halfway to grey (`dimReadText`), which
stays legible on light and dark message windows.

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


### Staff roll

`[credits]` scrolls a staff roll from below the screen to above it, then the
Story continues:

```pxstory
[hud visible=false]
[bg black]
[credits duration=60s skippable=false size=32 color=#FFFFFF]
# シナリオ
Owen Liu

# 原画
[image logo]
[endcredits]
[hud visible=true]
```

- Each line of the block is one centred item: text, a `#` heading (1.5x
  the size), `[image name]` (a `storyAssets.bg` name or a path; images wider
  than the screen are scaled down), or an empty line as a gap. `;` lines are
  comments; leading and trailing empty lines are dropped. Text is drawn as
  written: inline tags are not read inside a roll.
- `duration` is the whole scroll, 1s–600s (default 30s). With the default
  `skippable=true` a click or skip ends the roll; `skippable=false` ignores
  them until it has finished. `size` is 8–200 pixels (default 32), `color`
  is `#RRGGBB(AA)` (default white) and `font` a project font file (default:
  the locale's first `fontChain` font).
- The roll is drawn over the Stage, under the HUD and message window, so
  hide the HUD first as above and choose the background behind it.
- The whole block is one operation and one `.pxids.json` entry; a locale may
  change its lines and images. A save made during the roll resumes after it.
- Errors: a missing `[endcredits]` (or one without a block) is
  `PXSTORY1270`, a bad option `PXSTORY1271`, and an unsupported or empty
  body `PXSTORY1272`.


## Macros

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
- Definitions are local to their Story file and may come after their calls,
  unless they live in a shared macro file (below).
- Diagnostics and the source map point at the call; the diagnostic details
  name the macro, where it is defined and, for a syntax error inside the
  expanded body, its body line.
- Each expanded operation takes the call's identity plus its line in the body
  (`<call>/m0`, `<call>/m1`, ...). An `[id]` before the call or the file's
  `.pxids.json` entry for it pins the whole expansion, so saves and read-text
  marks survive edits elsewhere. Changing a macro's body renumbers the lines
  of every call, like editing those lines in place.

Macros every scene repeats belong in shared macro files, listed in
`prismatix.json` (author-only, like `storyAssets`):

```json
"storyMacros": ["Story/_macros.pxstory"]
```

```pxstory
; Story/_macros.pxstory
[macro scene_change bg rule=083r time=1000ms]
[image.hide all]
[hud visible=false]
[bg black rule=%rule duration=%time]
[wait %time]
[hide all transition=false]
[bg %bg transition=false]
[transition %time rule=%rule]
[endmacro]
```

```pxstory
; Story/day1.pxstory
[scene_change bg006a]
[scene_change bg012 rule=wipe-left]
```

- Every Story in the project may call these macros. A shared macro file holds
  only `[macro]` definitions, comments and blank lines; anything else is
  `PXSTORY1196`, because it would never run. The file emits no operations
  and is not a scene.
- Names are unique across all shared files and may not be redefined by a
  Story's own `[macro]` (`PXSTORY1191`, naming the first definition).
- Calls behave exactly like calls to local macros: identities come from the
  call, so editing or moving the shared file keeps saves valid, and the
  diagnostics point at the call with the shared file and line in the
  details (`in macro scene_change (Story/_macros.pxstory:2)`).
- `storyMacros` lists 1-64 distinct project-relative `.pxstory` files
  (`PXBUILD1120`); a listed file that does not exist is `PXBUILD1121`.
  `compileStory` and `compileStoryProject` callers pass the result of
  `createStoryMacroLibrary(files)` as `storyMacros`.

## Characters, backgrounds and sound

The starter registers `guide`, its `smile` expression and an original background. Reuse these without hand-writing another UUID:

```pxstory
[bg window]
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

`[bg window]` works because the starter's `prismatix.json` declares
`"storyAssets": {"bg": ["Assets"]}`; `[bg Assets/window.png]` is the same
operation.

Characters and image layers are hidden with the same verb:

| Hides | Short form | Long form |
| --- | --- | --- |
| one character | `[hide guide]` | `[hide character=guide transition=true]` |
| every character | `[hide all]` | — |
| one `[image]` layer | `[hide image=cg1 400ms]` | `[image.hide cg1 400ms]` |
| every image layer | `[hide image=all]` | `[image.hide all]` |

`[hide image=...]` takes only a duration (`PXSTORY1245` otherwise) and
compiles to exactly the `[image.hide]` operation.

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

### Changing only the face of a layered character

When a character's expressions are role-tagged `.pxportrait` descriptors
(see [layered portraits](#layered-portraits)), each expression is a body
(`base` layers) plus a face (`expression` layers), and an alias such as
`z1e0700_e0022` names both. `[face]` changes only the face and keeps the
body, pose, position, offset, scale and turn on stage:

```pxstory
[show 藍 z1e0700_e0022 center]
@藍「……」
[face 藍 e0023]
@藍「えへへ」
[face 藍 z1e0000_e0001]
```

- The face is the file name, without extension, of the expression layer
  image (`e0023` for `.../e0023.png`; several layers join with `+`), or an
  expression alias whose face to use.
- The compiler lists, for every body of that character, the expression that
  shows this face; the Player picks the one whose `base` layers match the
  portrait on stage and switches with `transition=auto` (only the face
  crossfades). `transition=true|false` works as for `[show]`.
- A character without layered expressions is `PXSTORY1261`; a face no
  expression shows is `PXSTORY1262`; `[face]` without a character and a face
  is `PXSTORY1260`. At run time a body with no such face, or a character not
  on stage, leaves the image unchanged and logs warning `PXVN6102`.
- The message-window face option `@藍[face=...]` is a different feature: it
  picks the face image shown inside the message window for one line.

### Expressions and image files

To add an expression, add a source image and an expression with `assetPath` in `Characters/guide.pxcharacter`, then select its alias with `expression=...`. The Authoring project compiler generates canonical character, expression, and asset UUIDs when omitted; no catalog entry is needed for that image. For example, `{"name":"Happy","aliases":["happy"],"assetPath":"Assets/guide-happy.png"}` is enough inside `expressions`. TypeScript generators can use the exported `CharacterSourceDocument` type; the stricter `CharacterDocument` type describes the published canonical file. Validate this shorthand with `compileProject` or `prismatix validate`, not the low-level `validateDocument("character")` canonical checker. The generated IDs follow the character file, the expression's first alias and its image path: keep those stable across save-compatible releases, or pin `id`/`assetId` explicitly before renaming; a file move is not automatically a save migration. Positions are `left`, `center`, `right` (also 1/2/3).

For a dynamically selected family of UI/media files, put the containing
directory in the project's `resourceIncludes`, for example
`"resourceIncludes": ["Assets/UI/Original"]`. UI controls can still use
ordinary paths; do not also list every file in `assets` unless it needs a
stable catalog identity. `prismatix validate` reports `catalogIncludeOverlap`
so large duplicate inventories are visible.

### Music, sound and voice

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
authoring errors instead of silently changing playback. A missing reference is a build diagnostic.

### Opening UI routes and timelines

`[ui route=save operation=push]` opens a Player route from the Story;
buttons use [Player actions](ui-reference.md#player-actions) instead.

A registered `.pxtimeline` plays with its resource reference, and
`[timeline Content/Motion/intro.pxanim await=true speed=1.25]` waits for it and
sets its playback rate:

```pxstory
[timeline Animations/greeting.pxtimeline]
```

## Variables and conditions

Declare a variable in Story with `[var]`; no JSON is needed:

```pxstory
[var affection=0]
[var met_aoi=false scope=profile]
[var nickname="直哉"]
[var ratio=0 type=number]
```

- `name=default` gives the name and default value. The type follows the
  default (`0` integer, `0.5` number, `true`/`false` boolean, anything else
  string) unless `type=boolean|integer|number|string` says otherwise; `scope`
  is `session` (the default) or `profile`.
- A declaration emits no operation and takes no entry in the `.pxids.json`
  sidecar. The build merges every Story's declarations into the compiled
  `game.pxgame`, so the Player, saves and UI bindings see an ordinary catalog
  variable. `[set]`, `[add]`, `[if]` and choice conditions check names and
  types against it.
- Declaring the same variable again (in another scene, for example) is fine
  when it agrees; a different type, scope or default, including one that
  disagrees with the `.pxgame` file, is `PXSTORY1251` naming the first
  declaration. A malformed `[var]` is `PXSTORY1250`, and `[var]` inside a
  macro is `PXSTORY1252` (declarations are read before macros expand).
  `scope` and `type` cannot be variable names in `[var]`.

A hand-written catalog still works and may be combined with `[var]`, for
example by setting `"gameCatalog":"Content/game.pxgame"` in the project and
creating:

```json
{"format":"PrismatiXGame","schemaRevision":1,"variables":[{"name":"affection","type":"integer","default":0,"scope":"session"}],"gallery":[],"unlockables":[]}
```

```pxstory
[set affection=1]
[if affection > 0]
小光記得這個約定。
[else]
故事才剛開始。
[endif]
```

Use `session` for story state that belongs to a checkpoint; use `profile` intentionally for cross-session progression. Do not treat profile unlocks or preferences as ordinary rollback variables.

### Asking for a name: `[input]`

```pxstory
[var hero="直哉" type=string scope=profile]
[input var=hero prompt="名前を入力してください" default=直哉 max=8]
@hero 俺は{hero}だ。
```

The Player shows the HUD's text input panel, waits, and stores the trimmed text
(or the default, when the field is left empty) in the string variable; `{hero}`
in a line shows it. `max` counts characters (1-256, default 32). The built-in
HUD has the panel. In your own HUD add one, bound to the Player's data:

```tsx
<Group name="NamePanel" bind={{visible: "input.visible"}} position={[460, 380]} size={[1000, 260]}>
  <Text name="NamePrompt" bind={{text: "input.prompt"}} position={[40, 30]} size={[920, 50]} />
  <TextField name="TextInput" position={[40, 100]} size={[920, 64]} />
  <Button name="NameOk" text="OK" action={actions.input.accept} position={[800, 190]} size={[160, 50]} />
</Group>
```

The field must be named `TextInput`. Enter accepts as well. While the prompt is
open the Player ignores other UI actions and clicks. Preview has no prompt and
stores the default.

### `[elseif]`, conditional choices and choice blocks

```pxstory
[if affection > 5]
「ずっと一緒にいたい」
[elseif affection > 2]
「また明日ね」
[else]
「……じゃあ」
[endif]

[choice]
- 一起看看星星 -> stars
- 告白する -> confess if affection > 3
- 次の日へ -> scene=day2
[endchoice]
```

- `[elseif expr]` may repeat between `[if]` and an optional `[else]`. It
  compiles to `[else]` plus a nested `[if]`, and the one `[endif]` closes
  them all, so older Players run it unchanged. `[elseif]` without an open
  `[if]` is `PXSTORY1243`; after `[else]` it is `PXSTORY1244`; a bad
  expression is `PXSTORY1133` as for `[if]`.
- `[choice text="告白する" goto=confess if="affection > 3"]` shows the option
  only while the expression is true when the choices appear. The others keep
  their order; if every option is hidden, the Story continues after the
  choices instead of waiting. Saves keep the options that were offered.
- A `[choice]` block lists one option per line as `- text -> label`. After
  the label, `scene=id` jumps to another scene (`-> scene=day2` alone jumps
  to its start, `-> scene=next` to the following scene) and `if <expression>`
  runs to the end of the line. Quote the
  text when it contains `->` (`- "A -> B" -> label`). Blank lines and `;`
  comments are allowed; `[choice.wait]` is not needed. Each option compiles
  to exactly the operation of the equivalent `[choice text=... goto=...]`
  line and is its own entry in the `.pxids.json` sidecar.
- Block mistakes: a line that is not an option is `PXSTORY1240`, a missing
  `[endchoice]` (or one without a block) is `PXSTORY1241`, and an empty block
  is `PXSTORY1242`.

## Scenes and the story index

A larger story is split into scenes listed in `Story/story.pxindex`, grouped
into chapters. `prismatix add scene <id> --chapter <chapter> --title <title>`
writes a scene for every locale and lists it.

`add scene` ends a new scene with
`[jump scene=next]`. `scene=next` (in `[jump]`, `[call]` and choice options)
is the scene after this one in reading order: the chapters in order, each
chapter's scenes in order, then scenes no chapter lists. After the last
scene, `[jump scene=next]` ends the story; a choice or `[call]` there is
`PXSTORY1309`. Reordering scenes in the index therefore reorders the story
without editing jumps. A scene that runs off its end without a jump or
`[end]` still stops there: the compiler never continues into the next scene
on its own, so one of several route endings in a chapter cannot run into the
next by accident. A scene really named `next` is reached by that name.

## Importing a writers' draft

Writers often draft in Markdown (HackMD, Google Docs exports): a heading per
scene, the speaker's name before what they say, struck-out passages that were
cut. `prismatix import screenplay` turns such a draft into scenes, and stays
re-runnable after the writers edit it:

```bash
prismatix import screenplay docs/source/chapter1.md
prismatix import screenplay https://hackmd.io/<note>/download
```

Settings and staging live in `screenplay-import.json` beside `prismatix.json`
(or `--config <file>`):

```json
{
  "chapter": {"id": "chapter1", "title": "第一章"},
  "speakers": {"黑澤 凪": "nagi", "千明 栞": "shiori"},
  "narrators": ["系統"],
  "entryLabel": "top",
  "scenes": [
    {"id": "ch1_01_funeral", "title": "教堂葬禮", "chapterTitle": "第一章　教堂葬禮",
     "start": ["[bg church duration=2s]", "[bgm requiem fade=2s]"],
     "cues": [
       {"at": "「不能哭啊」", "commands": ["[show father normal center]"]},
       {"after": "我們要不要交換書來看?", "commands": ["[choice]", "- 好啊 -> swap_yes", "[endchoice]"]}
     ],
     "end": ["[w 1s]"]}
  ]
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `sceneHeading` | `^#{1,3}\s+(.+?)\s*$` | a line that starts a scene; group 1 is its heading (the scene title unless `title` is set) |
| `speakerTag` | `^\[text speaker="([^"]*)"\]\s*$` | a line naming who speaks the following lines |
| `speakerLine` | none | a line holding a name and what they say, such as `^(\S{1,8})：(.+)$` |
| `speakers` | `{}` | names → character aliases; other names stay literal (`@"路人"`), without a trailing `（…）` note |
| `narrators` | `[]` | names whose lines are narration |
| `cut` | `["~~", "~~"]` | text between the two markers is dropped, across lines; `null` keeps it |
| `letter` | `"` | a line holding only this opens and closes a letter, shown as an NVL page; a blank line in it turns the page; `null` turns this off |
| `scenes` | ids `scene01`, … | staging per scene in draft order (the count must match); `id`, `title`, `chapterTitle` (`[chapter]`), `start`, `cues`, `end` |
| `idPrefix` | `scene` | ids when `scenes` names none |
| `entryLabel` | none | a label at the top of every scene, for journeys and jumps |
| `locale` | the project's default | where scenes are written: `Story/<locale>/<id>.pxstory` |

A cue's `at` inserts its Story lines before the first line containing that
text, `after` right after it; cues are taken in draft order, and one the
draft no longer contains is reported. Prose is escaped so it is always text
(`[`, `{` and a leading `@`, `;` or `*` become their fullwidth forms), and a
line opened by `*` (or wrapped in `_..._`) is shown in italics. Each scene
ends with `[jump scene=next]`. The chapter's scenes replace their earlier
import in the story index; other chapters stay. On a re-import, operations on
unchanged lines keep their save identities even when lines were added or
rewritten around them, so saves still resume; a scene whose edits cannot be
matched is reported and gets fresh identities.

## Layered portraits

`.pxportrait` is a public, versioned resource contract for compositing shared
transparent texture layers. Reuse the same body and clothing PNGs across face
variants instead of exporting a complete image for each expression.

```ts
import {serializeLayeredPortrait} from "@prismatix/authoring-sdk";

const authored = serializeLayeredPortrait({
  width: 691,
  height: 1450,
  layers: [
    {path: "Assets/Characters/Layers/body.png", width: 691, height: 1450,
      role: "base"},
    {path: "Assets/Characters/Layers/face-smile.png", x: 302, y: 153,
      width: 174, height: 74, opacity: 217, role: "expression"},
  ],
});
if (!authored.valid) throw new Error(JSON.stringify(authored.diagnostics));
// Write authored.value as UTF-8 to Assets/Characters/Smile.pxportrait.
```

Layers are drawn in array order on the portrait's unscaled canvas. `x` and
`y` default to zero; `opacity` defaults to 255. Width and height are required
for the canvas and every layer, and must be positive. Paths are safe
project-relative texture paths; a layer cannot reference another `.pxportrait`.
`defineLayeredPortrait` returns the typed validated document when an importer
needs to inspect or transform it before serialization.

Mark every layer `base` or
`expression`: `base` includes body, dress, pose, and overlays that must not
change during a facial transition. A partial role table is rejected.

Reference the saved descriptor in Story just like a sprite:

```pxstory
[show character=hero asset=Assets/Characters/Smile.pxportrait position=center transition=auto]
```

With `transition=auto`, a new actor uses the ordinary entrance fade. An
existing actor with the same portrait simply updates position; a replacement
crossfades only if both role-tagged descriptors have the same canvas, layer
count, roles, and all `base` layers are identical while at least one
`expression` layer differs. Otherwise it switches immediately. `true` and
`false` retain their explicit behavior, including on portraits without roles. This makes a changed outfit or pose safe without per-story-line
transition flags; it does not infer semantic roles from filenames.

### Blinking and lip sync

An `expression` layer can carry `blink` and `mouth` frame lists. Each frame
is a texture drawn over the expression, in canvas coordinates (`x`/`y`
default to the layer's own):

```ts
{path: "Assets/Characters/Layers/face-smile.png", x: 302, y: 153,
  width: 174, height: 74, role: "expression",
  blink: [
    {path: "Assets/Characters/Layers/eyes-half.png", width: 174, height: 40},
    {path: "Assets/Characters/Layers/eyes-shut.png", width: 174, height: 40},
  ],
  mouth: [
    {path: "Assets/Characters/Layers/mouth-ajar.png", x: 372, y: 205, width: 34, height: 18},
    {path: "Assets/Characters/Layers/mouth-open.png", x: 372, y: 205, width: 34, height: 18},
  ]}
```

- **Blink** frames go from half-closed to closed. Every 2.5–6 seconds (at
  random, per character) the Stage plays them in order and back, 50 ms each.
- **Mouth** frames go from slightly to fully open. While a line's voice
  plays, the character named by the line (`hero: ...` or `char=`) opens its
  mouth with the voice's loudness; quiet passages show the expression's own
  closed mouth. Other characters on stage keep their mouths shut.

Frames are drawn only on the expression currently shown (an expression that
is fading out keeps its still face), stop while skipping, and are not saved:
a loaded game simply starts a new blink timer. Frames need the layer to be
an `expression` layer (`PXSDKSEM1351`), and their textures are packaged with
the descriptor.

When a character's expressions are role-tagged descriptors, `[face alias
e0023]` changes only the face: the compiler finds, for each set of `base`
layers, the expression whose `expression` layer file is named `e0023`, and
the Player picks the one matching the body on stage. See [changing only the face](#changing-only-the-face-of-a-layered-character).

The Authoring project compiler validates reachable descriptors and includes
their layer textures from Story, UI action arguments, locale resources,
explicit includes, or asset-catalog references. A UI `TextureRect` can use a
`.pxportrait` as its `path` (including JSX `<Image path="...pxportrait" />`)
or use an `assetId` / `texture` resource reference to the descriptor.
It fits the portrait canvas into the control and draws ordered layers with
their authored opacity; `scaleMode` and alignment work as for ordinary images.
The Player and preview hosts load the descriptor through their UI text asset
loader and preload its layer textures before showing a new scene. Dynamic
`path` changes also resolve the descriptor, retaining the previous image if
the new layer textures are still loading. Missing or malformed descriptors
fail before packaging or UI installation.

## Camera and layered motion

For a simple camera pan or zoom, write it directly in Story:

```text
[camera x=120 y=-30 zoom=1.2 duration=800ms ease=outCubic]
[camera x=0 y=0 zoom=1 duration=600ms]
```

Coordinates and zoom are absolute; omitted values retain their current value.
The default duration is 600 ms, and `wait=true` is the default so the next
Story operation starts after the move. Use `duration=0` to snap immediately or
`wait=false` to let the next operation run while the camera moves. A `.pxmotion`
resource is useful for composed, multi-track motion.

`.pxmotion` is a UTF-8 JSON resource, not a TypeScript program. It contains a
logical canvas, image layers, and keyed tracks. TypeScript authors can use
`defineMotion` or `serializeMotion` from `@prismatix/authoring-sdk` to create and
validate that JSON; a TSX UI references the saved resource with `<Motion>`.

Each layer may be an image (optionally an atlas `frames` list selected by a
`frame` track) or a non-drawing `group`. Tracks key position, scale, rotation,
opacity, visibility, atlas frame and an RGB `tint`; numeric keys may use a
`cubicBezier` timing curve. Layers can be parented, blended (`normal`,
`additive`, `multiply`), masked by another layer's alpha, given their own
clock (`timeOffset`, `timeScale`) and told not to inherit their parent's
rotation or scale (`inheritRotation`, `inheritScale`). Timed `se`/`voice` audio
markers play when playback crosses them. Times are seconds, or ticks when the
document declares a `tickRate`.

In TypeScript, `MotionTrack` is property-discriminated: `visible` keys take
booleans, `tint` keys take `{r, g, b}`, and position/scale/rotation/opacity/
atlas-frame keys take numbers. `visible` and `frame` keys only allow `step`;
`cubicBezier` keys require control points, which other curves reject. The
editor and `tsc` catch these structural mistakes before building the game.
Runtime schema and semantic checks still validate external JSON, numeric
ranges, frame indices, key times, and layer references.

```ts
import {writeFile} from "node:fs/promises";
import {serializeMotion} from "@prismatix/authoring-sdk";

const motion = serializeMotion({
  id: "logo-reveal",
  canvas: {width: 1280, height: 720},
  duration: 1,
  layers: [{
    id: "logo",
    image: "Assets/UI/logo-atlas.png",
    blendMode: "additive",
    x: 480, y: 300, width: 320, height: 120,
    frames: [
      {x: 0, y: 0, width: 320, height: 120},
      {x: 320, y: 0, width: 320, height: 120},
    ],
  }],
  tracks: [{
    layer: "logo", property: "frame",
    keys: [{time: 0, value: 0}, {time: 0.5, value: 1}],
  }],
  markers: [{time: 0.5,
    audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
if (!motion.valid) throw new Error(JSON.stringify(motion.diagnostics));
await writeFile("Content/Motion/logo-reveal.pxmotion", motion.value!, "utf8");
```

For a source whose rate is independently verified as 60 ticks per second,
the same authoring API can keep its original time coordinates:

```ts
const tickMotion = serializeMotion({
  id: "tick-reveal", canvas: {width: 1280, height: 720},
  tickRate: 60, duration: 90, // 1.5 seconds
  layers: [{id: "logo", image: "Assets/UI/logo.png",
    width: 320, height: 120, timeOffset: -15}],
  tracks: [{layer: "logo", property: "opacity", keys: [
    {time: 0, value: 0}, {time: 60, value: 1},
  ]}],
  markers: [{time: 30, audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
```

`inlineMotion` composes clips, also with different tick rates; the result
keeps the outer clip's rate. Its `at` option is in the parent's local seconds,
`atTicks` in the outer clip's ticks.

An image `anchorX`/`anchorY` may lie outside the image (-16 to 16 image
sizes). `withPixelPivot` sets them from a pivot in image pixels:

```ts
import {withPixelPivot} from "@prismatix/authoring-sdk";

const glyph = withPixelPivot({
  id: "glyph", image: "Assets/Motion/glyph.png",
  width: 16, height: 3, x: 500, y: 300,
}, 8, -1);
// glyph.anchorX === 0.5; glyph.anchorY === -1 / 3
```

```tsx
import {Motion, Scene} from "@prismatix/authoring-sdk";

export default <Scene name="Title" width={1280} height={720}>
  <Motion name="Logo" path="Content/Motion/logo-reveal.pxmotion"
    position={[0, 0]} size={[1280, 720]} autoplay />
</Scene>;
```

The Authoring SDK inserts an invisible, full-scene structural root when the
scene contains a leaf or multiple top-level children. An explicit `<Group>`
still works and is preserved as authored.

`canvas` and layer `x`/`y`/`width`/`height` use logical UI coordinates;
`frames` use pixels of the source image. A `frame` track is discrete, so
interpolating curves and indices outside the frame array are rejected. Before
the first key, frame 0 is selected. Keep rectangles inside the atlas image.

Audio marker IDs are unique within a clip; times must be ordered and fall
between zero and `duration`. `bus` is `se` or `voice`. `compileProject`
includes marker audio in the package dependency graph, while visual preloading
still requests only layer images. Marker IDs are generated when omitted.

Blend modes are scoped per draw, so two layers can share one atlas without
leaking the previous layer's mode. `additive` adds source light to what is
already drawn; `multiply` darkens the overlap. Neither mode is an alpha mask.

For an animated reveal, author a normal image layer and a separate mask image
layer. Key the mask's transform or opacity on the same timeline:

```ts
layers: [
  {id: "logo", image: "Assets/Motion/logo.png", width: 640, height: 220,
    mask: "reveal"},
  {id: "reveal", image: "Assets/Motion/reveal-alpha.png", width: 640,
    height: 220},
],
tracks: [{layer: "reveal", property: "x", keys: [
  {time: 0, value: -640}, {time: 1, value: 0},
]}],
```

Mask images use their alpha channel, not brightness. Their transform, atlas
frame, inherited visibility and opacity are sampled with the content at the
same seek position. Both image files are validated as package dependencies.

To place an animated subpart under a parent transform and delay its own
timeline, write a group and parent an image layer to it:

```ts
layers: [
  {id: "logo-group", kind: "group", x: 320, y: 180,
    timeOffset: -0.25, timeScale: 2},
  {id: "letter", parent: "logo-group", image: "Assets/Motion/letter.png",
    width: 120, height: 80},
],
tracks: [{layer: "letter", property: "opacity", keys: [
  {time: 0, value: 0}, {time: 0.5, value: 1},
]}],
```

At the clip's 0.5-second mark, `letter` is sampled at local time 0.75. A
group can also have transform tracks.

For offline composition, `inlineMotion` performs the ID, parent, mask, track,
and audio-marker rebasing and adds a transform group for you:

```ts
import {inlineMotion, serializeMotion} from "@prismatix/authoring-sdk";

const combined = inlineMotion(outerMotion, letterMotion, {
  id: "letter-intro", parent: "logo-group", at: 0.25, speed: 2,
  x: 120, y: 30,
});
if (!combined.valid) throw new Error(JSON.stringify(combined.diagnostics));
const json = serializeMotion(combined.value!);
```

`outerMotion` may start with `layers: []`; the final combined resource is still
validated as a normal motion document. The child must be non-looping. It is
hidden before `at`, begins at child-local time zero, and holds its final pose
after its last key. `at` uses the named parent's local clock; audio markers
are converted to the outer clock. The helper rejects a clip that would start
before or finish after the outer duration, since that would otherwise silently
truncate animation. Set `allowTruncate: true` only when intentional; markers
outside the outer interval are then discarded, and a clip wholly outside is
still rejected. The child canvas stays in its original logical units;
explicitly scale it when embedding a different-sized canvas.

For a non-linear key interval, put the timing curve on the first key:

```ts
{time: 0, value: 0, curve: "cubicBezier",
  bezier: {x1: 0.1, y1: 0.8, x2: 0.4, y2: 0.9}}
```

The next key supplies the interval's destination value. `x1` and `x2` must
be in `[0, 1]`; `y1` and `y2` may be in `[-8, 8]` to permit overshoot. Boolean
visibility and atlas-frame tracks remain discrete `step` tracks.

An animated source color can be expressed without splitting it into three
separate tracks:

```ts
layers: [
  {id: "group", kind: "group", tint: {r: 1, g: 0.5, b: 1}},
  {id: "logo", parent: "group", image: "Assets/Motion/logo.png",
    width: 320, height: 120},
],
tracks: [{layer: "logo", property: "tint", keys: [
  {time: 0, value: {r: 1, g: 1, b: 1}},
  {time: 1, value: {r: 1, g: 0, b: 0}},
]}],
```

Each channel is in `[0, 1]`. Values interpolate with the numeric-track
easing curves, with overshoot clamped to valid channels.

For a sublayer that should follow a rotating parent's position and zoom but
not its angle, set `inheritRotation: false`:

```ts
layers: [
  {id: "parent", kind: "group", x: 320, y: 180,
    rotation: 90, scaleX: 2, scaleY: 2},
  {id: "letter", parent: "parent", image: "Assets/Motion/letter.png",
    width: 80, height: 80, x: 40, inheritRotation: false},
],
```

For a full-canvas composition on the Stage:

```text
[motion Content/Motion/intro.pxmotion await=true speed=1.25]
[motion.stop]
```

Stage motion appears above Stage actors and below dialogue/UI. The Runtime
checks that the descriptor and referenced images/audio exist, preloads images
before advancing, and plays audio markers on the SE or voice bus. `await=true`
pauses Story until a non-looping motion completes; awaiting a looping motion is
rejected. Starting another motion replaces the current composition, and
`[motion.stop]` removes it. Save/load restores the descriptor, playback speed,
sampled position and pending wait without replaying earlier markers. TSX
`<Motion>` remains available for motion inside UI layouts.

## Scripts, saves and rollback

The runtime has checkpoint state for VM/Stage, routes/UI, backlog and Timeline; pending JavaScript commands and Actions have separate replay journals. Extension state must use a versioned synchronous JSON provider. Arbitrary closures, globals, promises or the entire QuickJS heap are not saved. See the provider API in [runtime-sdk/README.md](../runtime-sdk/README.md), including its migration callback.

Checkpoint restore can re-enter command/Action code to reconstruct a suspended continuation. Keep persistent state in providers, and do not rely on external side effects being exactly-once across load/rollback. Provider capture/restore/migrate callbacks must be deterministic and must not call engine operations.

`ctx.stage.animate(target, spec)` tweens a character, an image layer, `$bg`
or `$camera` (`x`, `y`, `scale`, `alpha`; `rotation` in degrees for characters
and image layers) over `duration` milliseconds with `ease`. `loop` plays that
many passes (`-1` until another tween replaces it) and `yoyo: true` runs every
other pass back, so `{rotation: 8, duration: 1000, loop: -1, yoyo: true}`
rocks a layer. Running tweens, their pass and direction are Stage state:
saves, loads and rollback resume them where they were.

Runtime extensions run in QuickJS, without Node filesystem/process/network modules or unrestricted time/random/eval APIs. Installing a Node package does not make it a runtime dependency. TSX authoring is evaluated during the build in Node and is a separate trust boundary: only build projects and dependencies you trust. Runtime sandbox limits do not make arbitrary build-time project code safe.
