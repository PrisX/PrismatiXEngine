# Data-driven placed UI items

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
runtime host still require that host to validate its registrations.
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
