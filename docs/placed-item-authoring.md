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

For a non-Player host, register values and formatters before validating the
authored contract, and validate before mounting routes. This example uses the
`settings.confirmations` contract above and assumes `project` is the parsed
`project.pxproject` JSON:

```cpp
px::Status PrepareAuthoredUi(px::ui::GalgameUI& ui,
                             const nlohmann::json& project) {
    auto status = ui.DefineUiData(
        "settings.confirmations", px::Variant(px::VariantArray{}));
    if (!status) return status;
    status = ui.RegisterUiFormatter({
        "CaptionCase", px::VariantType::String, px::VariantType::String,
        [](const px::Variant& value) -> px::Result<px::Variant> {
            // Replace this pass-through body with the host's caption format.
            return px::Result<px::Variant>::Success(value.Clone());
        }});
    if (!status) return status;
    return ui.ValidateUiContract(
        project.at("settings").at("authoring.uiContract").dump());
}

px::Status RefreshConfirmations(px::ui::GalgameUI& ui,
                                px::VariantArray rows) {
    return ui.UpdateUiData("settings.confirmations",
                           px::Variant(std::move(rows)));
}
```

The host supplies each row's declared `key`, `rect`, and `caption` fields.
`UpdateUiData` checks a candidate value before notifying mounted UI, so a
bad late row does not replace the last valid array. The host must also
register its Action commands and their handlers before an authored screen
uses them; the UI contract above describes values and formatter types, not
Action implementations. For typed host Actions, see the
[host Action authoring contract](host-action-authoring.md).

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
