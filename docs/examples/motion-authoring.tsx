import {Motion, Scene, compileJsxUi, inlineMotion, serializeMotion}
  from "@prismatix/authoring-sdk";

const authored = serializeMotion({
  id: "logo-reveal", canvas: {width: 1280, height: 720}, duration: 1,
  layers: [{id: "logo", image: "Assets/UI/logo-atlas.png",
    x: 480, y: 300, width: 320, height: 120,
    frames: [{x: 0, y: 0, width: 320, height: 120},
      {x: 320, y: 0, width: 320, height: 120}]}],
  tracks: [{layer: "logo", property: "frame",
    keys: [{time: 0, value: 0}, {time: 0.5, value: 1}]}],
});
if (!authored.valid || authored.value === undefined)
  throw new Error(JSON.stringify(authored.diagnostics));
const descriptor = JSON.parse(authored.value) as {schemaRevision: number};
if (descriptor.schemaRevision !== 2)
  throw new Error("Atlas frame motion must use revision 2");

const masked = serializeMotion({
  id: "masked-reveal", canvas: {width: 1280, height: 720}, duration: 1,
  layers: [
    {id: "logo", image: "Assets/UI/logo.png", width: 320, height: 120,
      mask: "wipe"},
    {id: "wipe", image: "Assets/UI/logo-mask.png", width: 320, height: 120},
  ],
  tracks: [{layer: "wipe", property: "x", keys: [
    {time: 0, value: -320}, {time: 1, value: 0},
  ]}],
});
if (!masked.valid || JSON.parse(masked.value!).schemaRevision !== 6)
  throw new Error("Alpha-mask motion must validate as revision 6");

const nested = serializeMotion({
  id: "nested-logo", canvas: {width: 1280, height: 720}, duration: 1,
  layers: [
    {id: "subclip", kind: "group", x: 480, y: 300,
      timeOffset: -0.25, timeScale: 2},
    {id: "logo", parent: "subclip", image: "Assets/UI/logo.png",
      width: 320, height: 120},
  ],
  tracks: [{layer: "logo", property: "opacity", keys: [
    {time: 0, value: 0}, {time: 0.5, value: 1},
  ]}],
});
if (!nested.valid || JSON.parse(nested.value!).schemaRevision !== 7)
  throw new Error("Child-local motion must validate as revision 7");

const inlined = inlineMotion({
  id: "full-title", canvas: {width: 1280, height: 720}, duration: 1,
  layers: [{id: "title-anchor", kind: "group"}],
}, {
  id: "letter", canvas: {width: 320, height: 120}, duration: 2,
  layers: [{id: "glyph", image: "Assets/UI/logo.png", width: 320,
    height: 120}],
  tracks: [{layer: "glyph", property: "opacity", keys: [
    {time: 0, value: 0}, {time: 2, value: 1},
  ]}],
}, {id: "intro", parent: "title-anchor", at: 0.25, speed: 4,
  x: 480, y: 300});
if (!inlined.valid || inlined.value?.schemaRevision !== 7 ||
    inlined.value.tracks[1]?.keys[1]?.time !== 2)
  throw new Error(`Inline motion failed: ${JSON.stringify(inlined.diagnostics)}`);

const ui = compileJsxUi(
  <Scene name="Title" width={1280} height={720}>
    <Motion name="Logo" path="Content/Motion/logo-reveal.pxmotion"
      position={[0, 0]} size={[1280, 720]} autoplay />
  </Scene>, {path: "Content/UI/Title.tsx"});
if (!ui.valid || !ui.value?.nodes.some(node => node.runtimeType === "MotionRect"))
  throw new Error(JSON.stringify(ui.diagnostics));

process.stdout.write("Layered motion authoring documentation example passed\n");
