# Authoring input actions

Put named actions in the project's `Content/game.pxgame` document. This is
versioned JSON, but the `GameInputAction` TypeScript type can be used by
generators. Unlike the older `inputBindings` command/key pairs, one entry
declares the action identity, input context, default keyboard keys, wheel gesture, and an
optional Player route or UI command together:

```json
{
  "format": "PrismatiXGame",
  "schemaRevision": 3,
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

`uiAction` requires `schemaRevision: 3`; revision 2 remains valid for projects
without it. An authored UI shortcut calls the same Player UI command handler
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
keyboard and wheel choices override authored defaults. Reset restores the
authored keyboard defaults. Gamepad bindings are left intact.
The order in `keys` is preserved for UI captions and saved settings (for
example, `F2 / S` stays in that order rather than scancode order).
Wheel `direction` is `up`, `down`, or `none`; `none` disables an engine default.
The optional `threshold` is accumulated within one frame (0..100); omitting it
fires on any movement. The UI still receives its independent raw Scroll event.
The equivalent typed authoring example is
[input-actions.ts](examples/input-actions.ts).

The project compiler checks route names, duplicate action IDs, and collisions
with legacy `screen.open` bindings. Native builds export
`PrismatiXInputKeyRegistry.json` from `Input::KeyCode` next to the Runtime tools.
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

`bindingAction` must name an action registered by the game or host. Player
provides the input service and saves a successful rebind to `Save/config.dat`
in its [per-game user-data directory](player-user-data.md);
other hosts must attach `UIContext::SetInputBindingService(input, onChanged)`
and persist the updated keyboard or wheel bindings themselves. The Runtime SDK exposes
`ctx.input.addKeyBinding(action, key)` as the same atomic append operation;
it returns `limit-reached` if the save-compatible maximum is reached. This
button can append, replace one selected key, or replace the whole keyboard set.
Native `Input::ReplaceKeyBinding` replaces one key atomically while preserving
the other keys and their order. Native `Input::RemoveKeyBinding`
and Runtime SDK `ctx.input.removeKeyBinding(action, key)` remove one key while
keeping the others in order and leaving wheel/gamepad bindings intact. They
return `false` for an unknown action, invalid key, or key that is not bound.
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
game actions register. Older revision-2 settings without `wheelBindings`
continue to use authored defaults. A native host using `GameSettings` can wire
both devices to its own settings path:

```cpp
px::Input input;
px::progress::GameSettings settings;
const auto load = settings.LoadWithInputBindings(configPath, input, &saveKey);
const bool settingsWritable =
    load != px::progress::PersistLoadStatus::Failed;
uiContext.SetInputBindingService(&input, [&] {
    if (settingsWritable)
        (void)settings.SaveWithInputBindings(configPath, input, &saveKey);
});
```

`configPath` is the host's per-user settings path, and `saveKey` is its
persistence key. The callback runs after a successful UI rebind. Register
authored actions and defaults before loading when possible; saved actions
registered later still receive their overrides. A missing file leaves defaults
active on a fresh Input. An invalid file changes neither settings nor input.
Keep writes disabled so the host cannot overwrite the user's bytes. Hosts with a
different store can use `CaptureInputBindings` and `ApplyInputBindings` directly.
Existing `inputBindings` documents remain readable for migration.

Project compilation checks `bindingAction` in UI scenes and components. An
empty or non-string value is an error (`PXBUILD1071`). A name absent from both
`game.inputActions` and the built-in Input actions produces `PXBUILD1072`
with the UI node location; it is a warning because a custom host may register
the action later. A `wheelThreshold` outside 0–100, or one that is not a finite
number, is an error (`PXBUILD1073`) even when no native UI registry is selected.
For Player projects, treat the unknown-action warning as a likely typo and
exercise the binding in a packaged Player before release.
