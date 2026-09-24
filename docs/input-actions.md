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
its engine default. Saved user keyboard choices override authored defaults;
Reset restores the authored defaults. Gamepad and wheel bindings are left intact.
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

This does not yet provide a ready-made rebinding UI widget, a player-facing
wheel remapping widget, or portable persistence for non-Player hosts. Those
are separate remaining input-contract tasks. Existing `inputBindings`
documents remain readable for migration.
