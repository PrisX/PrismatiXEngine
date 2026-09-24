# Layered portrait authoring

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

Role-tagged layers produce revision 2. Mark every layer `base` or
`expression`: `base` includes body, dress, pose, and overlays that must not
change during a facial transition. Untagged revision-1 descriptors continue
to load unchanged. A partial role table is rejected.

Reference the saved descriptor in Story just like a sprite:

```pxstory
[show character=hero asset=Assets/Characters/Smile.pxportrait position=center transition=auto]
```

With `transition=auto`, a new actor uses the ordinary entrance fade. An
existing actor with the same portrait simply updates position; a replacement
crossfades only if both revision-2 descriptors have the same canvas, layer
count, roles, and all `base` layers are identical while at least one
`expression` layer differs. Otherwise it switches immediately. `true` and
`false` retain their original explicit behavior, including on revision-1
portraits. This makes a changed outfit or pose safe without per-story-line
transition flags; it does not infer semantic roles from filenames.

The Authoring project compiler validates reachable descriptors and includes
their layer textures from Story, UI action arguments, locale resources,
explicit includes, or asset-catalog references. A UI texture control does not
render `.pxportrait` directly; the Stage does. Missing or malformed descriptors
fail before packaging. Direct `compileProject` callers must provide the
descriptor text in `sourceFiles` or
through `resourceText`; the CLI reads it from the project directory. The
Runtime Stage composes the layers; this format does not yet offer an Editor
inspector, per-layer animation, or dynamic dress/pose/face selection within a
single descriptor. Those remain separate authoring/runtime work.
