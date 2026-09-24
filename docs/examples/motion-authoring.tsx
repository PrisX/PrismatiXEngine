import {Group, Motion, Scene, compileJsxUi, serializeMotion}
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

const ui = compileJsxUi(
  <Scene name="Title" width={1280} height={720}>
    <Group name="Root" fill>
      <Motion name="Logo" path="Content/Motion/logo-reveal.pxmotion"
        position={[0, 0]} size={[1280, 720]} autoplay />
    </Group>
  </Scene>, {path: "Content/UI/Title.tsx"});
if (!ui.valid || !ui.value?.nodes.some(node => node.runtimeType === "MotionRect"))
  throw new Error(JSON.stringify(ui.diagnostics));

process.stdout.write("Layered motion authoring documentation example passed\n");
