# Authoring keyboard actions

Put named actions in the project's `Content/game.pxgame` document. This is
versioned JSON, but the `GameInputAction` TypeScript type can be used by
generators. Unlike the older `inputBindings` command/key pairs, one entry
declares the action identity, input context, default keyboard keys, and an
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
Reset restores the authored defaults. Gamepad bindings are left intact.
The equivalent typed authoring example is
[input-actions.ts](examples/input-actions.ts).

The project compiler checks route names, duplicate action IDs, and collisions
with legacy `screen.open` bindings. The native loader also rejects malformed
entries; unknown SDL key names stop Player startup with a diagnostic instead
of silently disabling the shortcut. `prismatix validate` currently validates
key strings structurally, not against the selected platform's SDL key-name
table, so a distribution smoke test is still needed for uncommon key names.

This does not yet provide a UI widget for key capture, a general mapping from
named actions to UI Actions, remappable mouse-wheel gestures, or portable
persistence for non-Player hosts. Those are separate remaining input-contract
tasks. Existing `inputBindings` documents remain readable for migration.
