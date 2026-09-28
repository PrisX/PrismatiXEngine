# Host Action authoring contract

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
