import {defineLayeredPortrait, serializeLayeredPortrait}
  from "@prismatix/authoring-sdk";

const input = {width: 691, height: 1450, layers: [
  {path: "Assets/Characters/Layers/body.png", width: 691, height: 1450,
    role: "base" as const},
  {path: "Assets/Characters/Layers/face-smile.png", x: 302, y: 153,
    width: 174, height: 74, opacity: 217, role: "expression" as const},
]};
const defined = defineLayeredPortrait(input);
const serialized = serializeLayeredPortrait(input);
if (!defined.valid || !serialized.valid || serialized.value === undefined ||
    defined.value?.layers[0]?.opacity !== 255 ||
    defined.value.schemaRevision !== 2)
  throw new Error("Layered portrait authoring example failed");
const document = JSON.parse(serialized.value) as {format?: string};
if (document.format !== "PrismatiXLayeredPortrait")
  throw new Error("Layered portrait contract format was not serialized");
process.stdout.write("Layered portrait authoring documentation example passed\n");
