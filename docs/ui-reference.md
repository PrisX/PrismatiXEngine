# UI reference

Screens are TSX files (`Content/UI/*.tsx`) built with
`@prismatix/authoring-sdk`. `prismatix eject ui <route>` copies a stock screen
to start from (see [Getting started](getting-started.md#add-characters-scenes-and-screens)).
The [examples](examples/jsx-ui-authoring.tsx) show a complete scene.

- [Player actions](#player-actions)
- [Controls and styling](#controls-and-styling)
- [Data the Player publishes](#data-the-player-publishes)
- [Settings and options](#settings-and-options)
- [Saving and loading](#saving-and-loading)
- [Sound settings](#sound-settings)
- [Language](#language)
- [Startup screens](#startup-screens)
- [Game flow](#game-flow)
- [Input actions and key bindings](#input-actions-and-key-bindings)
- [Sprite lettering](#sprite-lettering)
- [Placed items](#placed-items)
- [Modal dialogs](#modal-dialogs)
- [Host Actions](#host-actions)
- [Custom render effects](#custom-render-effects)

## Player actions

Buttons run Player actions (`save.open`, `set.bgm.value`, ...). Build them with
the typed `actions` builders instead of spelling ids:

```tsx
import {actions} from "@prismatix/authoring-sdk";

<TextureButton name="Save" skin="ui/save" action={actions.screen.save} />
<TextureButton name="Extras" skin="ui/extra" action={actions.screen.open("chapter3")} />
<TextureButton name="Slot" skin="ui/slot" action={actions.save.slot("$key")} />
```

`validate` rejects an id in a Player namespace that the Player does not have
(`PXSDKUI1026`, e.g. `set.bgm.vaule`) and an argument with the wrong name or
type (`PXSDKUI1025` says what the action takes). `screen.open` opens any route
declared in `uiEntryPoints`.

Extension-declared Actions use the parameter names and types in their `.pxextension` manifest; give them their own namespace, because the Player's namespaces (`game.`, `set.`, `music.`, ...) are reserved. For a `resource` parameter, wrap a path so it reaches the Runtime as a ResourceRef, not an ordinary string:

```json
{"id":"jukebox.play","arguments":{"media":{"type":"resource","value":"Assets/Audio/theme.ogg"},"volume":0.8}}
```

The same typed UI argument bridge accepts `color`, `uuid`, `token`, `vec2`, `rect`, arrays and objects. `prismatix validate` checks extension-declared Action arguments (required names, types, enum choices and ranges) and includes static resource paths, including manifest resource defaults, in the artifact. Actions registered only by a Player/host at runtime have no authoring manifest to check and remain runtime-validated. A bare string path is still a string, so it cannot satisfy a declared `resource` parameter.

`prismatix extension create my-fx --action open` scaffolds such an extension in TypeScript (`--lang js` for plain JavaScript): the manifest, a `main.ts` handler stub `defineAction("myFx.open", (args, context) => ...)` typed from the manifest, and the registration in `prismatix.json`. `prismatix extension add-action my-fx close` declares another. The handler's `context` carries `preview`, `scene`, `node`, `signal` and `route`; the manifest's `reentry` (`allow`, `ignoreWhileRunning`, `restart`) decides what a second press does while it runs. Every build bundles the TypeScript for the Player and reports type errors as `PXCLIEXT2001` warnings (see [runtime-sdk/README.md](../runtime-sdk/README.md)).

## Controls and styling

### Panels without textures

Any group, button or image can be a rounded, bordered, gradient or shadowed
panel; the engine draws it behind the control's content:

```tsx
<Group name="Card" position={[120, 180]} size={[820, 420]}
  style={{gradient: ["#FFFFFFCC", "#F2F5FDB8"], radius: 22, border: [1.5, "#FFFFFFF0"],
    shadow: {color: "#2A3A7A2E", blur: 26, y: 8}}} />
<Button name="Start" text="開始遊戲" font="Content/Fonts/NotoSerifTC-Bold.ttf"
  style={{background: "#FFFFFF10", hoverBackground: "#E9CF942E", radius: 21,
    border: [1, "#FFFFFF33"], hoverBorderColor: "#E9CF94FF", hoverTextColor: "#F6E6BEFF"}} />
```

`radius`, `border`, `gradient` (`[from, to]`, or `[from, to, "horizontal"]`) and
`shadow` are shorthands for the appearance fields `cornerRadius`,
`borderWidth`/`borderColor`, `backgroundColor`/`gradientColor`/`gradientDirection`
and `shadowColor`/`shadowBlur`/`shadowOffsetX`/`shadowOffsetY`;
`hoverBackgroundColor`, `hoverBorderColor` and `hoverTextColor` style the
pointer-over state. A transparent circle with only a large `shadow` makes a
soft ambient glow. The stock Player screens are built this way. `Button`,
`TextureButton`, `Text` and `RichTextLabel` take `font` for their caption.

A prop that TypeScript accepts but no lowering uses is warned about
(`PXSDKJSX1103`) instead of being dropped silently.

### Typed props

Prefer the typed props below to `runtimeProperties`; TypeScript checks them,
and `prismatix validate` warns about the old spelling at the authored line
(`PXSDKJSX1100`), about `<Control runtimeType="Slider">` (`PXSDKJSX1101`) and
about four hand-written state images that `skin` would derive (`PXSDKJSX1102`):

```tsx
<TextureButton name="Save" skin="Assets/UI/mw/bt_save" disabledPath="Assets/UI/mw/bt_save-off.png"
  position={[780, 776]} size={[96, 47]} action="save.open" />
<Slider name="TextSpeed" option="mspeed" position={[919, 300]} size={[400, 18]}
  railPath="Assets/UI/conf/rail.png" fillRailToValue />
<Group name="Dialog" fill clipContent linearTransition
  showTransition={{seconds: 0.2, offset: [0, -50]}} hideTransition={{seconds: 0.2}} />
```

`<Image path="Assets/UI/conf/char.png" region={[208, 192, 208, 96]} />` shows
one cell of an atlas (in image pixels) as if it were a whole image of that
size, so sprite sheets need neither pre-cut files nor a clipping group with a
negative offset.

`slice` draws an image as a nine-slice, so one frame, panel or button skin fits
any size: the image pixels it names at the left, top, right and bottom keep
their size at the corners and stretch along one axis at the sides, and the
middle fills the rest. `sliceScale` sets the drawn size of that border
relative to the image (`0.5` for art made at twice the layout's resolution),
and `sliceCenter={false}` leaves the middle empty, for a frame drawn around
other controls. On a `TextureButton` every state image is sliced the same way:

```tsx
<Image name="WindowFrame" path="Assets/UI/window.png" slice={[96, 80, 96, 80]} sliceScale={0.5}
  position={[160, 790]} size={[1600, 260]} />
<TextureButton name="Choice" skin="Assets/UI/choice" slice={[60, 0, 60, 0]} sliceScale={0.5}
  text="一起去看星星" position={[480, 500]} size={[960, 84]} action={actions.choice.select(0)} />
```

A sliced image stretches by definition, so it takes no `region`, `scaleMode` or
`lockAspectRatio`; `validate` reports a negative margin or a `sliceScale`
outside 0.01–16.

A caption is laid out in the whole button and centred on its letters, so on
art whose face is not the middle of the image (a drop shadow or glow below it)
the text looks low. `contentPadding` insets the caption's area in layout
pixels, one number for every side or `[left, top, right, bottom]`, for every
alignment of every `Button`, `TextureButton`, `KeyBindingButton` and
`WheelBindingButton`. Centred text moves by half the difference of top and
bottom, so a face with 8px of shadow under it takes:

```tsx
<TextureButton name="Start" skin="Assets/UI/start" contentPadding={[0, 0, 0, 8]}
  text="開始遊戲" position={[760, 600]} size={[400, 88]} action="game.start" />
```

The default is no padding. Each side is 0–2048; `validate` reports a negative
or larger value. Narrow buttons still keep a small side inset inside the
padding so their caption is not clipped.

`visibleWhen="profileFlags.cleared"` and `enabledWhen="!saveLoad.page1"` bind
one condition each, so a helper can pass either without one `bind` object
replacing the other. `tooltip={<Image path="Assets/UI/help-save.png"
position={[-100, -45]} size={[313, 43]} />}` on a named `TextureButton` shows
the element (positioned relative to the button) while the pointer is over it;
the build declares the `hover.<name>` data it needs, so `player.uiData` needs
no `hover.*` entries.

Two-state and one-of-many controls need no pair of hand-bound buttons:

```tsx
import {Radio, Toggle, actions} from "@prismatix/authoring-sdk";

<Toggle name="DockLock" option="dock" on="Assets/UI/mw/bt_lock" off="Assets/UI/mw/bt_unlock"
  position={[1864, 783]} size={[41, 38]} />
<Toggle name="MuteBGM" checked="settings.muted.bgm" action={actions.setting.toggleMute("bgm")}
  on="Assets/UI/conf/mute-on" off="Assets/UI/conf/mute-off" position={[1405, 256]} size={[80, 32]} />
<Radio name="FontSerif" option="font" value={2} skin="Assets/UI/conf/radio34"
  selected="Assets/UI/conf/radio34-on" position={[913, 354]} size={[34, 34]} />
<TextureButton name="Slot" skin="Assets/UI/save/slot" action={actions.save.slot("$key")} />
```

`Toggle` lowers to `<name>On` / `<name>Off` buttons and `Radio` to
`<name>Button` / `<name>Selected`, bound to `options.<key>.is<value>` or the
`checked` path. `actions` builds every stock Player action with the right id
and argument key (`actions.option.set("dock", 0)`, `actions.setting.volume("bgm",
80)`, `actions.screen.close`, ...), so a typo is a TypeScript error rather than
a button that does nothing.

`skin="stem"` uses `stem-normal.png`, `stem-hover.png` (also for pressed) and,
with `skin={{base: "stem", states: ["hover", "pressed", "disabled"]}}`, the
other states; an explicit `*Path` wins. Button sounds need no per-button
props: `player.uiActivateSound` plays for every button and
`player.uiCancelSound` for `overlay.close`, `route.back` and `*.cancel`
actions; `activateSoundPath=` overrides one button.

Any control can also sound as it appears: `showSound` plays when its show
transition starts, after its `delay` and any parent `stagger` and once its
parents have begun to show (at once without a transition), and `hideSound`
as it starts to hide. They play on the SE channel at the player's SE volume;
`variants` picks one file at random each time (and replaces `path`),
`volume` (0 to 1) scales it and `delay` (show only, up to 10 s) waits that
long after the control begins to show. On a screen's root that plays a sound
seconds after the screen opens without holding the screen back (a show
transition's `delay` would keep the whole screen unseen); a staggered list of
cards sounds card by card. A re-layout does not play it again; showing the
control again does:

```tsx
<Group name="Root" fill showSound={{path: "Assets/Audio/se/chime.ogg", delay: 1.5}}>
  <Group name="Cards" stagger={0.12}>
    <Image name="Card1" path="Assets/UI/card.png" position={[200, 300]} size={[240, 320]}
      showTransition={{seconds: 0.3, delay: 0.4, offset: [0, 40]}}
      showSound={{variants: ["Assets/Audio/se/flip-a.ogg", "Assets/Audio/se/flip-b.ogg"], volume: 0.7}} />
  </Group>
</Group>
```

The compiler packages every file (variants included) and reports missing ones.

Any control can fade out when hidden instead of vanishing:
`showTransition={{seconds: 0.2, offset: [0, -50]}}`,
`hideTransition={{seconds: 0.2, offset: [0, 50]}}` and `linearTransition`
slide and fade it (`translation` and the slide move the whole control, its
panel and children with it). `ease` picks the slide curve: `quad` (default), `linear`,
`cubic`, or `back`, which overshoots and settles on show and pulls back before
leaving on hide. `offset: {y: -50}` or `{x: 40}` sets one axis and keeps the
Runtime default of the other: without an offset a control slides up 8px as it
appears and does not slide as it hides. A toolbar that springs up from the
bottom edge:

```tsx
<Group name="HudDock" fill bind={{visible: "hud.dockShown"}}
  showTransition={{seconds: 0.45, offset: [0, 96], ease: "back"}}
  hideTransition={{seconds: 0.22, offset: [0, 96], ease: "cubic"}} />
```

A transition can also wait, wipe and scale. `delay` keeps a control unseen
for that many seconds before it shows (it applies to a screen opening, too);
`wipe: "left" | "right" | "up" | "down"` reveals the control from that edge
(and on hide takes it away towards it), clipping its panel and children;
`scale` is the size it grows from (show) or ends at (hide); `fade: false`
keeps it opaque, for a pure wipe or slide. `stagger` on a parent starts each
visible child that many seconds after the previous one, and `hoverScale`
eases a control to that scale under the pointer:

```tsx
<Group name="Menu" stagger={0.06}>
  <Button name="Start" text="開始" hoverScale={1.05}
    showTransition={{seconds: 0.35, delay: 0.2, offset: [-40, 0], ease: "cubic"}} />
  <Button name="Load" text="讀取" hoverScale={1.05}
    showTransition={{seconds: 0.35, delay: 0.2, offset: [-40, 0], ease: "cubic"}} />
</Group>
```

The Player makes the items of a `ListView` or `GridView` itself (save slots,
backlog lines), so the view carries their animation: `itemShowTransition`
takes the same fields and plays on every item when the view fills (when the
screen opens or its data is replaced; items scrolled into view later appear
at once), `stagger` spaces them out in order and `itemHoverScale` sets their
hover scale. Save slots that wipe in from the left one after another:

```tsx
<Control name="Slots" kind="grid" runtimeType="GridView"
  itemShowTransition={{seconds: 0.5, wipe: "left", offset: [0, 0], ease: "cubic"}}
  stagger={0.08} itemHoverScale={1.03}
  runtimeProperties={{columns: 3, itemExtent: {type: "vec2", x: 564, y: 150}}} />
```

## Data the Player publishes

Controls bind to UI data with `bind={{text: "..."}}`, `visibleWhen` and
`enabledWhen`. Besides the screens' own data (save slots, backlog entries,
settings values), the Player publishes booleans for HUD details that would
otherwise need code: `dialogue.hasSpeaker` (a name plate that hides during narration),
`dialogue.waiting` (the line is complete and waits for a click: bind a
click-wait glyph's `visible` to it), and `hud.auto` / `hud.skip`.

### Where choices appear

The HUD's `ItemsPlaced` over `dialogue.choices` decides where choice bars go:

```tsx
<ItemsPlaced name="ChoiceList" items={{path: "dialogue.choices", key: "key", placement: "rect"}}
  choiceLayout={{area: [0, 497, 1920, 86], pitch: 60,
    rows: {2: [-1, 1], 3: [-1.5, 0, 1.5], 4: [-2, -0.6666, 0.6666, 2]}}}>
  ...one choice template...
</ItemsPlaced>
```

`area` is the middle bar's rectangle. Each choice moves down by `pitch` times its
offset; `rows` sets the offsets for a choice count, and other counts sit one
pitch apart around the middle. The toolbar that appears when the pointer comes
near is controlled the same way: put an invisible control named `DockArea` in
the HUD. The older `player.choiceLayout` and `player.dockArea` settings still
work, and `validate` suggests moving them.

### Screens that change with progress

Title buttons that unlock as the player clears routes are rules over the
global profile's flags, not code:

```json
"player.profileUiData": {
  "title.extraOpen": {"any": ["clear_a", "clear_b"]},
  "title.ending": {"cases": [
    {"when": {"all": ["clear_a", "clear_b"]}, "value": 2},
    {"when": "clear_a", "value": 1}
  ], "default": 0}
},
"player.titleBgm": [
  {"when": "clear_true_end", "path": "Assets/Music/title_after.ogg"},
  {"path": "Assets/Music/title.ogg"}
]
```

A condition is a flag name (true when the profile variable is `true` or `1`),
or `all`, `any` or `not` of conditions. A plain condition publishes a boolean
at its key (`bind={{visible: "title.extraOpen"}}`). A `cases` rule publishes the
first matching value as an integer, plus `title.ending.is0`, `.is1`, `.is2`
booleans. `player.titleBgm` plays the first rule that matches. `validate`
checks the rules and warns about a `player.*` key the Player does not read.

## Settings and options

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
| `dockPinned` | 0: the HUD toolbar (`hud.dockShown`) shows only while the pointer is over the HUD control named `DockArea` or a choice is up |
| `messageFace` | 0: never show message-window faces; 2: only while the speaker is off stage; otherwise always |

Behaviours no option binds keep their defaults (the toolbar is pinned, the
cursor never hides). Two options binding one behaviour is warned about
(`PXBUILD1105`); whichever the player set last wins. A misspelled `binds` is a
validation error.

`<Slider option="autoSpeed" />` edits an option and needs no host code. `{id: "option.set", arguments: {argument: "font=2"}}`
sets one, and `options.<name>` (text) and `options.<name>.is<value>` (boolean)
are bindable UI data.

While the settings screen is open, `settings.preview` types a sample line at
the current text speed, holds it for the auto-advance wait (at least 0.3 s)
with `settings.previewWaiting` true, and starts again; moving either slider
restarts it. Bind a label to it to let players try their settings:

```tsx
<Text name="Preview" bind={{text: "settings.preview"}} position={[120, 820]} size={[900, 48]} />
<Text name="PreviewGlyph" text="▼" visibleWhen="settings.previewWaiting" position={[1030, 832]} size={[24, 24]} />
```

The locale string `player.settings.preview` replaces the sample line. The
stock settings screen shows it under the sliders.

### Confirmation dialogs

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
`previousChoice`, `nextChoice`, `resetSettings`, `deleteSave`, and `copySave` /
`moveSave` (asked only when the target slot holds a save). Several operations may share
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

## Saving and loading

Save and load screens can delete, copy and move saves and let the player
write a comment on a slot:

```tsx
<ItemsGrid name="Slots" items={{path: "saveLoad.slots", key: "key"}} ...>
  <Group name="Cell" size={[420, 120]}>
    <TextureButton name="SlotButton" skin="Assets/UI/save/slot" action={actions.save.slot("$key")} />
    <Text name="Note" bind={{text: "item.comment", visible: "item.occupied"}} ... />
    <TextureButton name="Copy" skin="Assets/UI/save/copy" action={actions.save.copy("$key")} ... />
    <TextureButton name="Move" skin="Assets/UI/save/move" action={actions.save.move("$key")} ... />
    <TextureButton name="Erase" skin="Assets/UI/save/erase" action={actions.save.delete("$key")} ... />
    <TextureButton name="Comment" skin="Assets/UI/save/note" action={actions.save.comment("$key")} ... />
  </Group>
</ItemsGrid>
<Group name="Picking" visibleWhen="saveLoad.pending.visible" ...>
  <Text text="Choose where to copy it" visibleWhen="saveLoad.pending.copy" ... />
  <Button text="Cancel" action={actions.saveLoad.cancelPending} ... />
</Group>
<Group name="CommentEditor" visibleWhen="saveLoad.comment.visible" ...>
  <TextField name="SlotComment" placeholder="A note on this save" ... />
  <Button text="OK" action={actions.saveLoad.acceptComment} ... />
  <Button text="Cancel" action={actions.saveLoad.cancelComment} ... />
</Group>
```

- `save.delete` removes the save (ask first with `deleteSave`).
- `save.copy` / `save.move` pick the source; the next slot the player
  activates is the target, whether through its ordinary `save.slot` /
  `load.slot` button or the same action. While a copy or move waits, every
  slot except the autosave accepts it, on both screens and across pages;
  `item.pending` marks the source, and `saveLoad.pending.visible`, `.copy`,
  `.move` and `.slot` describe the wait. Choosing the source again,
  `saveLoad.pending.cancel` or leaving the screen drops it. A copy replaces
  the target; a move onto another save swaps the two. The autosave can be
  copied but not moved.
- `save.comment` fills the screen's `TextField` named `SlotComment` with the
  slot's comment and focuses it; Enter or `saveLoad.comment.accept` stores up
  to 64 characters (empty removes the comment). `saveLoad.comment.visible`
  and `.slot` show the editor. The comment is `item.comment` (and
  `saveLoad.slot<N>.comment`); saving into a slot starts it without one.

A failed operation shows `saveLoad.error.visible`.

## Sound settings

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

## Language

UI text follows the language too: `textKey` binds a label to the current
locale's `strings` entry and updates it when the player switches language.

```tsx
<Text name="LoadError" textKey="save.incompatible" text="This save cannot be loaded." />
```

```json
{"format": "PrismatiXLocale", "schemaRevision": 1, "locale": "ja-JP",
 "strings": {"save.incompatible": "このセーブデータは読み込めません。"}}
```

The Player publishes every locale string as `strings.<key>` UI data; `text`
is shown until the locale loads. `prismatix validate` reports a key that no
locale defines (`PXBUILD1100`).

Declare locale resources and `fontChain` in `Content/Localization/<locale>.json`. The starter already includes its Traditional Chinese font. Validate every locale's images/voices and test real text rendering. The Player's live language change requires aligned document/operation identities and types; splitting or merging translated dialogue is not automatically a valid live-switch mapping. Keep topology aligned or redesign the scene/update and test its save anchors. Do not silently omit missing voice to make validation pass.

## Startup screens

A new project (`prismatix create`, with or without `--empty`) opens with a
"Made with PrismatiX" screen: the `splash` route, `Content/UI/Splash.tsx`
(laid out for the project's resolution, with the PrismatiX Studio layout
`Splash.studio.json` beside it), listed first in `player.splashes`. It fades
the logo in and out over its `enter` and `exit` animations in about three
seconds, and a click or a key skips it after 0.4 s. Edit it like any screen,
point the entry's `scene` at your own logo screen, or delete the entry (and,
if you like, the route and `Content/UI/PrismatiX/`) to start at the title.

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

A splash sound plays `audioDelay` seconds into the startup screen; inside any
other screen, put `showSound={{path, delay}}` on its root (see
[typed props](#typed-props)).

The screen's own `animations` play with it: the entry state as it is
installed, then the state named by `enterAnimation` (default `enter`) as it
appears and the one named by `exitAnimation` (default `exit`) before it leaves.
An empty name (`"enterAnimation": ""`) plays nothing there; a name the screen
has no state for warns (PXBOOT1104/PXBOOT1105).

The splash scene must be listed in `uiEntryPoints`; for a TSX-authored route,
use its `.tsx` source path in both places and the CLI publishes the compiled
`.pxui` path. The compiler collects
*every* candidate for packaging and reports missing media when the project
resolver is available; the Player chooses one available candidate at runtime.
`audio` remains available for a single splash sound. The project schema checks
these known settings, including paths and timing, before a distribution build.

## Game flow

How the stock Player moves between screens, and what decides each step:

1. **Launch**: each `player.splashes` entry in order (see
   [Startup screens](#startup-screens)), while the title's images load.
2. **Title**: the route `entry.ui` names (`title` unless the project says
   otherwise). Its buttons decide what comes next.
3. **Buttons**: `game.start` starts the Story at its entry scene (the story
   index's `entryScene`); `replay.launch` starts it at a named scene;
   `game.continue` loads the newest save, or opens `load` when there is none;
   `<route>.open` and `screen.open` open a screen over the current one;
   `overlay.close` returns to the screen underneath; `title.open` returns to
   the title (asking first when `player.confirmations.returnToTitle` is set);
   `app.quit` quits (asking first when `player.confirmations.exit` is set).
4. **In the game**: the `hud` route shows the Story. Keys open screens
   through `inputActions` in `Content/game.pxgame` (B opens the backlog and F6
   the save screen by default; give the built-in `menu` action, Escape, a
   `route` to open a menu with it). A key's or gamepad button's `route` opens
   its screen; a right click does what `player.navigation.gameRightClick`
   says. Escape or a right click closes an open menu.
5. **The end**: when the Story reaches `[end]` or runs off its last line,
   `player.navigation.afterStoryEnd` decides what follows. The Story can also
   leave earlier with `[ui route=... operation=replace]`.

`player.navigation` sets what no button decides. Every field left out keeps
the Player's own behaviour:

```json
"settings": {
  "player.navigation": {
    "afterStoryEnd": "credits",
    "gameRightClick": "menu"
  }
}
```

| Field | Values | Default |
| --- | --- | --- |
| `afterStoryEnd` | `stay`, `title` (as `title.open`, without asking), `quit`, or a route shown in place of the HUD | `stay`: the last line stays on screen |
| `gameRightClick` | `hideHud`, `none`, or a route opened as a menu (not `title`) | `hideHud`: a right click hides or shows the message window |

A route here must be one the Player can show: a `uiEntryPoints` route or a
stock screen, never `hud`, `game` or `video`. `prismatix validate` reports
anything else as `PXBUILD1111`. An after-ending screen (staff roll, thanks)
leads back with a `title.open` button; a right-click menu closes with
`overlay.close`, Escape or another right click.

## Input actions and key bindings

Put named actions in the project's `Content/game.pxgame` document. This is
versioned JSON, but the `GameInputAction` TypeScript type can be used by
generators. Unlike `inputBindings` command/key pairs, one entry
declares the action identity, input context, default keyboard keys, wheel gesture, and an
optional Player route or UI command together:

```json
{
  "format": "PrismatiXGame",
  "schemaRevision": 1,
  "variables": [],
  "gallery": [],
  "unlockables": [],
  "inputActions": [
    {"id": "quick-save", "context": "game", "keys": ["F8"]},
    {"id": "screen.open.gallery", "context": "game", "keys": ["G"],
     "route": "gallery"},
    {"id": "backlog", "context": "game", "wheel": {"direction": "up", "threshold": 1}},
    {"id": "advance", "context": "game", "wheel": {"direction": "down"}},
    {"id": "photo-mode", "context": "game", "keys": ["P"]},
    {"id": "settings.keys", "context": "ui", "keys": ["F2"],
     "uiAction": {"command": "settings.page7"}}
  ]
}
```

An authored `uiAction` shortcut calls the same Player UI command handler
as a button, with an optional string `argument`. It is active only in the
declared input context (`ui` while in menus, `game` during gameplay), and is
suppressed during splash, preload, video, and full-screen CG presentation.
One action may have a `route` or a `uiAction`, not both. A misspelled command
cannot be checked against every host extension at build time; an unhandled
Player command emits `PXPLAYER5023` when pressed. Authors should still
exercise the resulting shortcut in a packaged Player test.

The Player registers each action in the native input system. The built-in
`quick-save` action is therefore remapped to F8 without a Player-specific
configuration branch. The built-in `save.open` action opens the save screen
from gameplay with F6 by default; unlike the former raw F6 check, it respects
input contexts, can be rebound, and returns to F6 on Reset. A `route` opens a
registered screen while gameplay is active; it requires `context: "game"`.
An action without a route or UI command can be read
by an extension or host using the named-action input API. `keys` may be
omitted: a new custom action starts unbound, while an engine built-in keeps
its engine default. Set `keys: []` explicitly to remove a built-in's default
keyboard keys without removing its gamepad or wheel binding. Saved user
keyboard and wheel choices override authored defaults. A `route` answers the
action's keys and gamepad buttons, not a mouse button that also fires it (a
right click fires `menu`; see [Game flow](#game-flow)). Reset restores the
authored keyboard defaults. Gamepad bindings are left intact.
The order in `keys` is preserved for UI captions and saved settings (for
example, `F2 / S` stays in that order rather than scancode order).
Wheel `direction` is `up`, `down`, or `none`; `none` disables an engine default.
The optional `threshold` is accumulated within one frame (0..100); omitting it
fires on any movement. The UI still receives its independent raw Scroll event.
The equivalent typed authoring example is
[input-actions.ts](examples/input-actions.ts).

The project compiler checks route names, duplicate action IDs, and collisions
with legacy `screen.open` bindings.
With `--runtime` (or a Packager selected for a fresh distribution build),
`prismatix validate` and `build` check both `inputActions[].keys` and legacy
`inputBindings[].key` against that table before Player starts. A misspelled key
is `PXBUILD1068`; a runtime without the manifest emits `PXCLI1043`, so it is
clear that only structural validation ran. Native builds that have the exporter
but lack a current manifest fail rather than silently skipping this check.
The manifest lists canonical SDL names and the historical `Enter` alias; use
one of those spellings for portable authored defaults. A distribution smoke
test is still needed to verify actual keyboard behavior and user remapping.

Runtime extensions can now start a one-shot physical key capture with
`ctx.input.beginKeyCapture()`, poll `takeCapturedKey()`, and pass the resulting
scancode to `rebindKey()`. The captured key is withheld from ordinary actions
and text input; `cancelKeyCapture()` should be called when the settings page
closes. See the [Runtime SDK example](../runtime-sdk/README.md) for conflict
handling. Escape is returned as an ordinary key so the author's UI decides
whether it means cancel or a binding.

For keyboard settings, the Authoring SDK also provides `KeyBindingButton`.
It uses the normal `TextureButton` skins and text styling, but displays the
current key(s) for `bindingAction`, captures the next physical key on click,
and reports a conflicting key in the caption without changing the binding.
Escape cancels by default; the prompt, unbound caption, conflict caption,
Escape behavior, and shared-binding policy are authorable props. Set
`deleteClearsBinding` to let Delete during capture clear only that action's
keyboard keys; gamepad and wheel bindings remain intact. It is opt-in so a
game that uses Delete as a normal shortcut can still bind it. Set
`addCapturedBinding` to append a captured key while keeping the existing
ordered keys instead of replacing them. Conflict checks apply to the new key;
32 keys per action is the persisted-map limit. With `selectableBindingKeys`,
the selected key appears in brackets. Page Up/Down changes the selection while
the button is focused; the pointer wheel does so while hovered. Delete removes
just the selected key while focused.
Capturing a new key replaces the selected key in place unless
`addCapturedBinding` is set. `confirmSharedBinding` holds a conflicting key for
four seconds and requires another activation before sharing it with a second
action. The confirmation caption is configurable with `bindingConfirmText`.
For example:

```tsx
import {KeyBindingButton, Scene} from "@prismatix/authoring-sdk";

export default <Scene name="Keys" width={800} height={600}>
  <KeyBindingButton name="PhotoModeKey" bindingAction="photo-mode"
    x={280} y={180} width={240} height={56}
    normalPath="Assets/UI/key.png" bindingPrompt="Press a key"
    selectableBindingKeys confirmSharedBinding
    bindingConfirmText="Key in use; press again to share" />
</Scene>;
```

`bindingAction` must name an action registered by the game or host. The
Player saves a successful rebind with the player's settings in its
[user-data directory](shipping.md#packaged-player-user-data). The Runtime SDK exposes
`ctx.input.addKeyBinding(action, key)` as the same atomic append operation;
it returns `limit-reached` if the save-compatible maximum is reached. This
button can append, replace one selected key, or replace the whole keyboard set.
Wheel bindings can also be edited at runtime:

```ts
ctx.input.setWheelBinding("backlog", "up", 1.5);
ctx.input.setWheelBinding("advance", "none");
const binding = ctx.input.wheelBinding("backlog");
```

For a stock settings control, use `WheelBindingButton`:

```tsx
import {Scene, WheelBindingButton} from "@prismatix/authoring-sdk";

export default <Scene name="Wheel Settings" width={800} height={600}>
  <WheelBindingButton name="BacklogWheel" bindingAction="backlog"
    x={280} y={180} width={240} height={56}
    normalPath="Assets/UI/wheel.png" wheelThreshold={1.5}
    wheelUpText="Scroll up" wheelDownText="Scroll down"
    deleteClearsBinding />
</Scene>;
```

Clicking the control captures the next physical scroll direction. That gesture
does not activate the old action or scroll the underlying UI. Escape cancels
capture by default; clicking again also cancels. With `deleteClearsBinding`,
Delete during capture clears only the wheel binding. `wheelThreshold` accepts
0–100 and defaults to zero, which triggers on any nonzero movement. The button
displays the bound direction and asks the host to persist successful changes.


`wheelBinding` returns `null` for an unknown action. `setWheelBinding` returns
`false` for an unknown action or invalid threshold. Player saves these choices
alongside keyboard bindings in per-user `config.dat` and restores them before
game actions register.

Project compilation checks `bindingAction` in UI scenes and components. An
empty or non-string value is an error (`PXBUILD1071`). A name absent from both
`game.inputActions` and the built-in Input actions produces `PXBUILD1072`
with the UI node location; it is a warning because a custom host may register
the action later. A `wheelThreshold` outside 0–100, or one that is not a finite
number, is an error (`PXBUILD1073`) even when no native UI registry is selected.
For Player projects, treat the unknown-action warning as a likely typo and
exercise the binding in a packaged Player before release.

## Sprite lettering

`SpriteText` displays short text using a declared set of image glyphs. It is a
typed TSX UI control; no separate descriptor format is required.

```tsx
import {Scene, SpriteText} from "@prismatix/authoring-sdk";

const digits = [
  {character: "1", path: "Assets/UI/digit-1.png",
   x: 0, y: 0, width: 18, height: 30},
  {character: "-", path: "Assets/UI/dash.png",
   x: 0, y: 8, width: 19, height: 10},
];

export default <Scene name="Chapter" width={420} height={143}>
  <SpriteText name="ChapterNumber" glyphs={digits} advance={24}
    size={[420, 143]} bind={{text: "chapter.number"}} />
</Scene>;
```

Each `character` is one Unicode grapheme. `x` and `y` are offsets within the
control, and `width`/`height` are the rendered dimensions. `advance` moves the
next glyph's origin by a fixed logical-pixel distance; zero uses each glyph's
width. Missing characters are left blank. Playback renders at most 64 graphemes
per control, so this is for badges, counters and decorative labels, not long
dialogue or general text shaping.

Authoring validates unique glyphs, bounds and safe image paths. Project builds
package every glyph image, even if it is not in the initial text. Native UI
preloading likewise includes the full glyph set before showing the page.
`text` is bindable and can change without rebuilding the UI tree. Glyphs are
separate image resources; atlas source rectangles and kerning are not yet part
of this control.

## Placed items

`ItemsGrid` and `ItemsList` are for regular layouts. When one authored item
template must appear at irregular source-defined coordinates, use
`ItemsPlaced`. It retains the same stable-key, `item.<field>` binding, and
`$index` / `$key` Action behavior without generating a separate authored node
for every row.

```tsx
import {Control, ItemsPlaced, Scene, TextureButton} from "@prismatix/authoring-sdk";

export default <Scene name="Settings" width={1920} height={1080}>
  <Control name="Root" fill>
    <ItemsPlaced name="ConfirmationRows" x={950} y={200}
      width={500} height={700}
      items={{path: "settings.confirmations", key: "key", placement: "rect"}}>
      <Control name="ConfirmationTemplate" width={388} height={35}>
        <TextureButton name="On" x={0} y={0} width={184} height={34}
          normalPath="Assets/UI/on.png" />
        <TextureButton name="Off" x={204} y={0} width={184} height={34}
          normalPath="Assets/UI/off.png" />
      </Control>
    </ItemsPlaced>
  </Control>
</Scene>;
```

For a packaged Player, define initial UI data in the project manifest's
`settings["player.uiData"]` map. The key is the same ViewModel path used in
`items.path`; values may be arrays, objects, or scalar JSON values. The Player
registers them before mounting the first UI route. Other hosts can register
equivalent values with `GalgameUI::DefineUiData`.

```json
{
  "settings": {
    "player.uiData": {
      "settings.confirmations": [
        {"key":"confirm-voice","rect":{"x":0,"y":0,"width":388,"height":35}},
        {"key":"confirm-exit","rect":{"x":0,"y":48,"width":388,"height":35}}
      ]
    }
  }
}
```

Each item needs a unique nonempty string or integer `key`
and a `rect` object with finite numeric `x`, `y`, `width`, and `height`.
Coordinates are local to `ItemsPlaced`; width and height must be positive.
When the item's source path resolves to an initial array declared in
`player.uiData`, the SDK checks every stable key and placement, and every `item.<field>`
binding before packaging. With a selected native UI TypeRegistry it also
checks binding output types, including the built-in `Not`, `Percent`, and
`BoolToVisibility` formatters. Paths and custom formatters supplied only by a
runtime host can be declared in `settings["authoring.uiContract"]` so the SDK
can check them before packaging:

```json
{
  "settings": {
    "authoring.uiContract": {
      "format": "PrismatiXUiContract",
      "schemaRevision": 1,
      "roots": ["settings.confirmations"],
      "values": {
        "settings.confirmations": "array",
        "settings.confirmations[].key": "string",
        "settings.confirmations[].rect.x": "number",
        "settings.confirmations[].rect.y": "number",
        "settings.confirmations[].rect.width": "number",
        "settings.confirmations[].rect.height": "number",
        "settings.confirmations[].caption": "string"
      },
      "formatters": {
        "CaptionCase": {"input": "string", "output": "string"}
      }
    }
  }
}
```

`roots` lists the exact ViewModel paths the host registers, and each root
must have a type in `values`. Other `values` entries declare fields beneath
those roots. Use `[]`
for an array item's type and the item's nested fields; the UI binding itself
continues to use `item.caption`. Declared item sources must be arrays, stable
keys must be strings or integers, and an `ItemsPlaced` rectangle needs numeric
`x`, `y`, `width`, and `height` declarations. A declared ViewModel root with a
missing binding path or undeclared custom formatter fails validation. The SDK
checks formatter input and output against the selected native UI TypeRegistry.
The host must register matching runtime ViewModel values and formatter
implementations. `GalgameUI::RegisterUiFormatter` exposes custom formatter
registration, and `GalgameUI::ValidateUiContract` checks registered roots,
current values (including populated array items), and formatter signatures.
Stock Player runs this check when it loads `project.pxproject`, after defining
`player.uiData` and before mounting an authored route. The check retains a
successful contract. A host can then call `GalgameUI::UpdateUiData(path, value)`
for later changes, including an array that was empty at boot. It validates
declared item fields in a candidate ViewModel before notifying mounted UI;
a contract-invalid update keeps the previous value. A declared root must be registered
as an exact ViewModel property. Other hosts should run the check after
registering their own data and formatters, then use `UpdateUiData` for changes.
Paths outside declared roots remain the host's responsibility.

Resource paths supplied only through dynamic item bindings still need explicit
asset inclusion (for example `resourceIncludes`) until the compiler can infer
those runtime dependencies.

The view realizes every item; unlike `ItemsGrid` and `ItemsList`, it does not
scroll or virtualize a long collection. Reordering an array or changing a
rectangle retains the control identity for an unchanged key. An invalid
update leaves the previous visible item snapshot in place and emits a
diagnostic. As with the regular repeaters, one structural child is the item
template, nested item sources are not supported, and item bindings must use
`item.<field>`.

## Modal dialogs

`<Modal>` creates a full-screen backdrop and a blocking panel while native UI
input keeps focus inside the visible dialog. Its children use coordinates
relative to `panel`; the backdrop may use an image or the default translucent
color. `backdropAction` is optional: without it, clicks outside the panel are
still consumed, but do not dismiss the dialog.

```tsx
import {Button, Modal, Scene, Text} from "@prismatix/authoring-sdk";

export default <Scene name="Settings" width={1280} height={720}>
  <Modal name="Confirm" fill visible={false}
    bind={{visible: "confirm.open"}}
    panel={{x: 390, y: 240, width: 500, height: 240}}
    backdropAction="confirm.cancel">
    <Text name="Prompt" x={20} y={24} width={460} height={60}>
      Save these settings?
    </Text>
    <Button name="Yes" x={20} y={170} width={120} height={40}
      action="confirm.accept">Yes</Button>
  </Modal>
</Scene>;
```

When `visible` becomes true, pointer events outside the dialog cannot reach
the underlying screen, and keyboard/gamepad focus moves to the first focusable
child. Hiding the dialog restores the previous focus if that control still
exists. The last visible modal in render order owns capture; a nested modal
can temporarily supersede its parent. The panel itself blocks backdrop clicks
within its rectangle. This is an in-scene dialog; route-level modal navigation
uses the separate UI router API.

## Host Actions

An application embedding `GalgameUI` can expose its own Actions to authored
UI. Declare their signatures in `prismatix.json` so `compileProject` checks
button clicks, component signals, and Behavior Graph Action arguments before
publishing the artifact:

```json
{
  "settings": {
    "authoring.actionContract": {
      "format": "PrismatiXActionContract",
      "schemaRevision": 1,
      "actions": [{
        "id": "host.open",
        "parameters": [{"name": "target", "type": "string", "required": true}],
        "safety": {
          "previewSafe": true,
          "deterministic": true,
          "seekSafe": true,
          "rollbackSafe": true
        }
      }]
    }
  }
}
```

An authored button can then use `{"id":"host.open","arguments":{"target":"settings"}}`
as its `onClick`. Supported parameter types are `boolean`, `integer`,
`number`, `string`, and `resource`. Action and parameter names must be unique;
an Action ID cannot also be declared by an extension. The SDK rejects missing,
unknown, or mistyped static arguments and checks dynamic bindings against the
declared parameter names. Other Actions remain available through their existing
providers.

The host registers a typed handler and validates the packaged contract before
mounting authored routes. This example assumes `project` is the parsed
`project.pxproject` JSON:

```cpp
px::Status PrepareHostActions(px::ui::GalgameUI& ui,
                              const nlohmann::json& project) {
    px::ui::ActionDescriptor descriptor;
    descriptor.id = "host.open";
    descriptor.arguments = {{.name = "target", .type = px::VariantType::String,
                             .required = true}};
    descriptor.previewSafe = true;
    descriptor.deterministic = true;
    descriptor.seekSafe = true;
    descriptor.rollbackSafe = true;
    auto status = ui.RegisterHostAction(std::move(descriptor),
        [](const px::ui::ActionInvocation& invocation) {
            const auto* target = invocation.arguments.at("target")
                .TryGet<std::string>();
            return OpenHostScreen(*target); // Replace with the host's operation.
        });
    if (!status) return status;
    return ui.ValidateActionContract(
        project.at("settings").at("authoring.actionContract").dump());
}
```

`ValidateActionContract` requires an exact registered handler, parameter
signature, and four matching safety flags for every declared Action. A generic
`RegisterActionCommand` fallback does not satisfy this check. A failed check
must stop that host from mounting the authored UI; it reports `PXUI2824`.
The catalog removes the host descriptors when `GalgameUI` is destroyed.
Preview dispatch also applies the native Action safety gate, so a handler
marked destructive in Preview or non-deterministic is blocked there.

Stock Player does not register application-specific handlers from this
declaration. A project using these Actions must provide a host integration
that registers them. The contract describes the host interface; it cannot
implement an Action by itself.

## Custom render effects

A `.pxeffect` adds an HLSL fragment shader to the Stage, a Stage node or a
transition. The Packager compiles it for every target when the game is packed;
the HLSL source is not shipped.

### Descriptor

```json
{
  "format": "PrismatiXEffect",
  "schemaRevision": 1,
  "id": "dream-tone",
  "targetLayer": "stage",
  "shader": "Effects/dream-tone.frag.hlsl",
  "uniforms": [
    {
      "name": "amount",
      "type": "number",
      "slot": 0,
      "default": 0.5,
      "minimum": 0,
      "maximum": 1
    }
  ]
}
```

The `targetLayer` is `stage`, `node` or `transition`; the shader receives a
controlled viewport input. An effect may
declare at most eight uniquely named uniforms in unique slots.
Supported types are `number`, `vec2`, and `color`; defaults must be finite and
inside the declared range. Colors also remain in normalized `[0, 1]` space.

### Fixed HLSL interface

```hlsl
cbuffer PrismatiXEffectContext : register(b0, space3) {
  float2 texelSize;
  float progress;
  float randomSeed;
  float4 parameters[8];
  float4 viewport; // logical x, y, width, height
};
Texture2D stageTexture : register(t0, space2);
SamplerState stageSampler : register(s0, space2);
```

Stage and node effects bind their source at `t0`/`s0`. Transition effects bind
the immutable outgoing frame at `t0`/`s0` and incoming frame at `t1`/`s1`.
Reflection must report exactly one sampler for Stage/node or two for transition,
one uniform buffer, one output, and no storage textures or buffers. The runtime
provides only those textures, normalized progress, logical viewport and texel
size, deterministic seed, and declared parameter slots. Raw renderer objects,
GPU pointers, filesystem, network, wall-clock, and unrestricted randomness are
never exposed.

### Story

`[effect dream-tone 1s amount=0.8]`, `[effect none]` and
`[transition 1s effect=ink-wipe softness=0.2]` play Stage and transition
effects from a Story line; see
[whole-screen transitions](story-reference.md#framing-image-layers-and-whole-screen-transitions).

### JavaScript

```ts
ctx.stage.customEffect("dream-tone", 0.65, {amount: 0.8});
ctx.stage.nodeEffect("portrait", "character-outline", 1, {width: 2});
ctx.stage.clearCustomEffect();
ctx.stage.clearNodeEffect("portrait");

const transition = ctx.effects.play("ink-wipe", 0.8, {softness: 0.12});
await ctx.effects.wait(transition);
```

Named values are checked against the packaged schema before native uniform
updates. The positional `vec4[]` form remains available for compatibility with
older extensions. Per-frame progress sampling and rendering stay native; Stage
effect state, parameters, and deterministic seed participate in save, seek, and
rollback capture.

### Capability and fallback

Custom effects require `graphicsTier: "gpu-effects"`; a device without a
compatible shader format rejects the package rather than drawing a different
image.

The WASM Preview currently has no custom GPU-effect backend and fails the GPU
tier explicitly instead of silently rendering a different image. Use basic-tier
built-in effects where a software/WASM fallback is required.
