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
Revision 4 adds per-layer `blendMode`: `normal` (default), `additive`, or
`multiply`. The helper selects revision 4 when a layer declares the property;
older revisions reject it instead of silently drawing with the wrong blend.
Revision 5 adds a `cubicBezier` timing curve for numeric tracks. Its control
points specify normalized time/value coordinates; the helper selects revision
5 when a key uses it. Earlier revisions and existing clips remain supported.
Revision 6 adds `mask` on an image layer. It names another animated layer whose
sampled alpha clips the content layer; the source layer is not painted by
itself. The helper selects revision 6 when any layer declares a mask. Missing,
self-referential and nested mask references are rejected. This first slice
requires normal blending on both the content and mask source; combining a mask
with additive/multiply is rejected instead of producing a misleading image.
Revision 7 adds non-drawing `kind: "group"` layers and per-layer `timeOffset` /
`timeScale`. A child samples its tracks at `parentLocalTime * timeScale +
timeOffset`; its transform and opacity still inherit from the parent. Group
layers need no image or dimensions and create no packaged texture dependency.
The helper selects revision 7 when these fields are used. Older files retain
their original shared-clock behavior. In revision 7, track key times use the
target layer's local seconds and may exceed the outer clip's `duration` (up to
86,400 seconds); earlier revisions still require keys within `duration`.
Audio markers always use the outer clip clock and remain within its duration.
Revision 8 adds multiplicative RGB `tint` on image or group layers and a
three-channel `tint` track. The helper selects revision 8 when either is
authored; older files cannot silently contain tint fields. A group's tint is
multiplied into its descendants, independently of `opacity`.
Revision 9 adds per-layer `inheritRotation` and `inheritScale` switches, both
defaulting to `true`. A child can keep the parent's affine translation while
dropping its authored rotation, scale/stretch (including reflection and shear),
or both. The sampler tracks authored rotation separately so a negative scale
does not become a fake 180-degree angle. `inlineMotion` accepts the same
switches for its inserted group. Older revisions reject these fields.
Revision 10 adds an explicit `tickRate` (ticks per second). When it is present,
`duration`, layer `timeOffset`, track-key `time`, and audio-marker `time` are
all authored in ticks. The Native loader converts them to seconds before
playback; `defineMotion` selects revision 10, which requires `tickRate`.
Earlier revisions still use seconds and reject the field. The allowed
`tickRate` is 0.001–1000; converted duration and local key/offset limits
remain one day. This avoids independent, error-prone division of every
timestamp when importing a source timeline. It does **not** infer a source
format's tick rate or validate that source animation semantics match.
Revision 11 lets image-layer `anchorX` and `anchorY` extend from -16 to 16
image-size units, including a pivot outside the image rectangle. `defineMotion`
selects it when an anchor is outside [0, 1]; earlier revisions keep their
original [0, 1] bounds. Revision 11 may use seconds or an explicit `tickRate`.
An external source's origin coordinates still require a verified mapping to
these normalized anchors before claiming a faithful import.

In TypeScript, `MotionTrack` is property-discriminated: `visible` keys take
booleans, `tint` keys take `{r, g, b}`, and position/scale/rotation/opacity/
atlas-frame keys take numbers. `visible` and `frame` keys only allow `step`;
`cubicBezier` keys require control points, which other curves reject. The
editor and `tsc` catch these structural mistakes before building the game.
Runtime schema and semantic checks still validate external JSON, numeric
ranges, frame indices, key times, and layer references.

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
    blendMode: "additive",
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
  markers: [{time: 0.5,
    audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
if (!motion.valid) throw new Error(JSON.stringify(motion.diagnostics));
await writeFile("Content/Motion/logo-reveal.pxmotion", motion.value!, "utf8");
```

For a source whose rate is independently verified as 60 ticks per second,
the same authoring API can keep its original time coordinates:

```ts
const tickMotion = serializeMotion({
  id: "tick-reveal", canvas: {width: 1280, height: 720},
  tickRate: 60, duration: 90, // 1.5 seconds
  layers: [{id: "logo", image: "Assets/UI/logo.png",
    width: 320, height: 120, timeOffset: -15}],
  tracks: [{layer: "logo", property: "opacity", keys: [
    {time: 0, value: 0}, {time: 60, value: 1},
  ]}],
  markers: [{time: 30, audio: "Assets/Audio/logo-pop.ogg", bus: "se"}],
});
```

`inlineMotion` accepts clips with different `tickRate` values and checks each
original document first. It keeps the **outer** clip's tick rate in the
composed result, so repeated nesting does not require a new unit conversion;
seconds-based outer clips remain seconds-based. Its `at` option is always in
the parent's local **seconds**; `atTicks` is an alternative in the outer
clip's authored ticks and requires `tickRate`. Supplying both is an error.
`speed` is unitless. The M2 `.mtn` tick rate has not yet been verified here,
so the value 60 above is an example, not a claim about M2.

For example, `anchorY: -1 / 3` places the pivot one pixel above a three-pixel-
high image; the layer's `x` and `y` specify where that pivot is placed. This
describes the `.pxmotion` transform only, not a conversion rule for `.mtn`.

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
through the Player audio service. The TypeScript helper generates marker IDs
when omitted; specify an ID only when a tool or editor needs a stable name.
Native hosts register one `UIContext::SetMotionAudioSink` callback, rather than
walking every `MotionRect` to attach a callback.

Blend modes are scoped per draw, so two layers can share one atlas without
leaking the previous layer's mode. `additive` adds source light to what is
already drawn; `multiply` darkens the overlap. Neither mode is an alpha mask.

For an animated reveal, author a normal image layer and a separate mask image
layer. Key the mask's transform or opacity on the same timeline:

```ts
layers: [
  {id: "logo", image: "Assets/Motion/logo.png", width: 640, height: 220,
    mask: "reveal"},
  {id: "reveal", image: "Assets/Motion/reveal-alpha.png", width: 640,
    height: 220},
],
tracks: [{layer: "reveal", property: "x", keys: [
  {time: 0, value: -640}, {time: 1, value: 0},
]}],
```

Mask images use their alpha channel, not brightness. Their transform, atlas
frame, inherited visibility and opacity are sampled with the content at the
same seek position. Both image files are validated as package dependencies.
The renderer reuses two logical-size offscreen targets per Renderer2D; it does
not allocate targets for every frame, but each masked draw adds offscreen
passes. The SDL GPU (D3D12) and basic (D3D11) framebuffer tests cover opaque,
transparent, half-alpha and moving masks. Software and WASM parity, mask
performance in the full game, and M2-specific frame matching are not yet
verified.

To place an animated subpart under a parent transform and delay its own
timeline, write a group and parent an image layer to it:

```ts
layers: [
  {id: "logo-group", kind: "group", x: 320, y: 180,
    timeOffset: -0.25, timeScale: 2},
  {id: "letter", parent: "logo-group", image: "Assets/Motion/letter.png",
    width: 120, height: 80},
],
tracks: [{layer: "letter", property: "opacity", keys: [
  {time: 0, value: 0}, {time: 0.5, value: 1},
]}],
```

At the clip's 0.5-second mark, `letter` is sampled at local time 0.75. A
group can also have transform tracks. This represents an inline nested motion
hierarchy, not a reference to another `.pxmotion` resource; an importer must
flatten external `.mtn` subclips and diagnose unsupported source fields.

For offline composition, `inlineMotion` performs the ID, parent, mask, track,
and audio-marker rebasing and adds a revision-7 group for you:

```ts
import {inlineMotion, serializeMotion} from "@prismatix/authoring-sdk";

const combined = inlineMotion(outerMotion, letterMotion, {
  id: "letter-intro", parent: "logo-group", at: 0.25, speed: 2,
  x: 120, y: 30,
});
if (!combined.valid) throw new Error(JSON.stringify(combined.diagnostics));
const json = serializeMotion(combined.value!);
```

`outerMotion` may start with `layers: []`; the final combined resource is still
validated as a normal motion document. The child must be non-looping. It is
hidden before `at`, begins at child-local time zero, and holds its final pose
after its last key. `at` uses the named parent's local clock; audio markers
are converted to the outer clock. The helper rejects a clip that would start
before or finish after the outer duration, since that would otherwise silently
truncate animation. Set `allowTruncate: true` only when intentional; markers
outside the outer interval are then discarded, and a clip wholly outside is
still rejected. The child canvas stays in its original logical units;
explicitly scale it when embedding a different-sized canvas. This helper does
not read or convert `.mtn`, resolve external motion references, or imply M2
visual equivalence.

For a non-linear key interval, put the timing curve on the first key:

```ts
{time: 0, value: 0, curve: "cubicBezier",
  bezier: {x1: 0.1, y1: 0.8, x2: 0.4, y2: 0.9}}
```

The next key supplies the interval's destination value. `x1` and `x2` must
be in `[0, 1]`; `y1` and `y2` may be in `[-8, 8]` to permit overshoot. Boolean
visibility and atlas-frame tracks remain discrete `step` tracks. The sampler
solves the timing cubic for each seek, so playback and preview scrubbing use
the same pose. This supports a class of `.mtn` timing curves, but does not
interpret `.mtn` files automatically.

An animated source color can be expressed without splitting it into three
separate tracks:

```ts
layers: [
  {id: "group", kind: "group", tint: {r: 1, g: 0.5, b: 1}},
  {id: "logo", parent: "group", image: "Assets/Motion/logo.png",
    width: 320, height: 120},
],
tracks: [{layer: "logo", property: "tint", keys: [
  {time: 0, value: {r: 1, g: 1, b: 1}},
  {time: 1, value: {r: 1, g: 0, b: 0}},
]}],
```

Each channel is in `[0, 1]`. Values interpolate with the numeric-track
easing curves, with overshoot clamped to valid channels. This covers ordinary
RGB modulation, not M2's entire color-transform or blur semantics.

For a sublayer that should follow a rotating parent's position and zoom but
not its angle, set `inheritRotation: false`:

```ts
layers: [
  {id: "parent", kind: "group", x: 320, y: 180,
    rotation: 90, scaleX: 2, scaleY: 2},
  {id: "letter", parent: "parent", image: "Assets/Motion/letter.png",
    width: 80, height: 80, x: 40, inheritRotation: false},
],
```

This is an Engine-authored inheritance rule, not a proven mapping of every
FreeMote `inheritMask` flag. Source angle, zoom, flip, stencil, and mesh
semantics still require a per-frame comparison before importing M2 clips.

This is not an `.mtn` importer or a full M2 Cheeseware reproduction. Blur,
external nested motion references, spatial path curves, particles, more
specialized blend equations, and Story playback are still missing;
the source work on those features is tracked in
[layered motion architecture](architecture/layered-motion-authoring.md).
The decoded M2 source groups named `bure` and `bure2` are indexed images,
not evidence of a shader blur command. Per-layer blur remains a general
capability gap, but M2-specific necessity must be established by source
semantics and rendered-frame comparison rather than those names.

Story's existing `[timeline Content/Motion/intro.pxanim await=true speed=1.25]`
command now exposes the Runtime's wait and playback-rate controls, so a scene
can pause until a `.pxanim` finishes without a JavaScript workaround. This
command plays `.pxanim`/`.pxtimeline` animation assets; it does **not** mount a
`.pxmotion` visual composition into the Stage. Use TSX `<Motion>` for the latter
until Story presentation, preloading and save/load state have a complete
contract.
