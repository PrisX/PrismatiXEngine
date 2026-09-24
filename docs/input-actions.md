# Authoring input actions

Put named actions in the project's `Content/game.pxgame` document. This is
versioned JSON, but the `GameInputAction` TypeScript type can be used by
generators. Unlike the older `inputBindings` command/key pairs, one entry
declares the action identity, input context, default keyboard keys, wheel gesture, and an
optional Player route together:

```json
{
  "format": "PrismatiXGame",
  "schemaRevision": 2,
  "variables": [],
  "gallery": [],
  "unlockables": [],
  "inputActions": [
    {"id": "quick-save", "context": "game", "keys": ["F8"]},
    {"id": "screen.open.gallery", "context": "game", "keys": ["G"],
     "route": "gallery"},
    {"id": "backlog", "context": "game", "wheel": {"direction": "up", "threshold": 1}},
    {"id": "advance", "context": "game", "wheel": {"direction": "down"}},
    {"id": "photo-mode", "context": "game", "keys": ["P"]}
  ]
}
```

The Player registers each action in the native input system. The built-in
`quick-save` action is therefore remapped to F8 without a Player-specific
configuration branch. A `route` opens a registered screen while gameplay is
active; it requires `context: "game"`. An action without a route can be read
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
with legacy `screen.open` bindings. The native loader also rejects malformed
entries; unknown SDL key names stop Player startup with a diagnostic instead
of silently disabling the shortcut. `prismatix validate` currently validates
key strings structurally, not against the selected platform's SDL key-name
table, so a distribution smoke test is still needed for uncommon key names.

This does not yet provide a UI widget for key capture, a general mapping from
named actions to UI Actions, a player-facing wheel remapping widget, or portable
persistence for non-Player hosts. Those are separate remaining input-contract
tasks. Existing `inputBindings` documents remain readable for migration.
