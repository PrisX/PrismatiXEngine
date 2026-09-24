# Authoring layered motion

`.pxmotion` is a UTF-8 JSON resource, not a TypeScript program. It contains a
logical canvas, image layers, and keyed tracks. TypeScript authors can use
`defineMotion` or `serializeMotion` from `@prismatix/authoring-sdk` to create and
validate that JSON; a TSX UI references the saved resource with `<Motion>`.

Revision 1 supports parented image layers and keyed position, scale,
rotation, opacity, and visibility. Revision 2 adds atlas frames: pixel
rectangles in one layer image selected by integer, `step`-curve `frame` keys.
The helper chooses revision 2 when either `frames` or a `frame` track is used.
Revision 3 adds timed `se`/`voice` audio markers. The helper selects revision 3
when `markers` are present. Older revision-1/2 files remain supported.

```ts
import {writeFile} from "node:fs/promises";
import {serializeMotion} from "@prismatix/authoring-sdk";

const motion = serializeMotion({
  id: "logo-reveal",
  canvas: {width: 1280, height: 720},
  duration: 1,
  layers: [{
    id: "logo",
    image: "Assets/UI/logo-atlas.png",
    x: 480, y: 300, width: 320, height: 120,
    frames: [
      {x: 0, y: 0, width: 320, height: 120},
      {x: 320, y: 0, width: 320, height: 120},
    ],
  }],
  tracks: [{
    layer: "logo", property: "frame",
    keys: [{time: 0, value: 0}, {time: 0.5, value: 1}],
  }],
  markers: [{id: "logo-pop", time: 0.5,
    audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
if (!motion.valid) throw new Error(JSON.stringify(motion.diagnostics));
await writeFile("Content/Motion/logo-reveal.pxmotion", motion.value!, "utf8");
```

```tsx
import {Motion, Scene} from "@prismatix/authoring-sdk";

export default <Scene name="Title" width={1280} height={720}>
  <Motion name="Logo" path="Content/Motion/logo-reveal.pxmotion"
    position={[0, 0]} size={[1280, 720]} autoplay />
</Scene>;
```

The Authoring SDK inserts an invisible, full-scene structural root when the
scene contains a leaf or multiple top-level children. An explicit `<Group>`
still works and is preserved as authored.

`canvas` and layer `x`/`y`/`width`/`height` use logical UI coordinates;
`frames` use pixels of the source image. A `frame` track is discrete, so
interpolating curves and indices outside the frame array are rejected. Before
the first key, frame 0 is selected. The image is packaged and preloaded once,
not reloaded at every key. Keep rectangles inside the actual atlas dimensions:
the contract bounds coordinates but cannot infer the image's size; the
renderer clips an out-of-image region.

Audio marker IDs are unique within a clip; times must be ordered and fall
between zero and `duration`. `bus` is `se` or `voice`. `compileProject`
includes marker audio in the package dependency graph, while visual preloading
still requests only layer images. Normal playback emits each crossed marker in
order, including on loops. An explicit `Seek` does not play skipped audio; an
extremely large frame jump has a bounded event budget rather than replaying an
unbounded backlog. A UI `<Motion>` and splash `MotionRect` dispatch these events
through the Player audio service.

This is not an `.mtn` importer or a full M2 Cheeseware reproduction. Image
masks, blur, blend modes, and Story playback are still missing;
the source work on those features is tracked in
[layered motion architecture](architecture/layered-motion-authoring.md).
