# @prismatix/runtime

TypeScript declarations and a thin ergonomic facade for PrismatiX JavaScript extensions.

The shipped game still runs the native C++ Runtime with its embedded JavaScript engine. This package is authoring/build-time tooling only; it does not add Node.js or a browser runtime to the Player.

```ts
import {createPrismatiXContext, defineCommand} from "@prismatix/runtime";

const ctx = createPrismatiXContext();

defineCommand<{amount: number}>("game.raiseAffection", async ({amount}) => {
  const current = Number(ctx.variables.get("affection") ?? 0);
  ctx.variables.set("affection", current + amount);
  await ctx.time.wait(0.1);
});
```

For code that prefers the host API directly, importing the package's types also declares the existing `Engine`, `px`, and `DisplayMode` globals.

## Build-time bundling

The embedded JavaScript host executes extension entry scripts; it is not a Node.js module loader and does not resolve npm packages at runtime. TypeScript extensions may import `@prismatix/runtime` while authoring, but the project build step must transpile/bundle those imports into the JavaScript shipped with the game.

In other words:

- authoring/build machine: TypeScript + `@prismatix/runtime` imports are fine;
- packaged extension: ship bundled JavaScript with no unresolved npm imports;
- native Player / WASM Preview: execute that JavaScript through the same embedded ScriptHost API.

The facade deliberately wraps the existing host instead of defining a second scripting runtime. Native Player and WASM Preview therefore keep identical ScriptHost semantics.

## Production runtime services

Long-running media and persistent state are exposed as typed engine services. Video
handles are thenable and also expose explicit lifecycle control; awaiting one uses
the native scheduler rather than blocking the update thread.

```ts
ctx.audio.playVoice("Content/Voice/chapter-01/001.ogg");
ctx.audio.stopVoice();

const opening = ctx.video.play("Content/Video/opening.mp4", {
  volume: 0.8,
  skippable: true,
});
opening.pause();
opening.resume();
await opening; // stop(), skip(), status(), error(), and token() are also available

ctx.saves.save(3);
ctx.saves.autosave();
const slot = ctx.saves.query(3);
const occupied = ctx.saves.list().filter((entry) => entry.exists);
ctx.saves.load(slot.slot);
```

Native playback keeps FFmpeg demux/decode on its worker and feeds bounded audio
and video frame queues. Production PDX5 packages store video as independently
authenticated 256 KiB records, so AVIO seek/read decrypts only the requested
record; encrypted MP4/H.264/AAC playback does not materialize the whole file.
PDX4 packages remain readable. WASM Preview reports native-video capability as
unavailable instead of silently substituting a different decoder.

Keyboard, controller, and pointer bindings converge on logical actions, so
extensions do not need device-specific branches:

```ts
if (ctx.input.actionPressed("advance")) {
  // advance dialogue
}
if (ctx.input.actionDown("toggle-skip")) {
  // held logical action
}
```

Extension-owned state participates in save, seek, and rollback through a
versioned, deterministic JSON provider. Capture, restore, and migration are
synchronous and may not call back into engine operations.

```ts
let state = {affection: 0};
ctx.state.registerProvider("game.relationships", 2, {
  capture: () => state,
  restore: (saved) => { state = {...saved}; },
  migrate: (saved, fromVersion) => fromVersion === 1
    ? {affection: Number((saved as {score?: number}).score ?? 0)}
    : saved as typeof state,
});
```

The embedded sandbox remains intentionally capability-limited: these services do
not expose Node.js, host filesystem, network, process, wall-clock, dynamic code
evaluation, or unrestricted randomness.

## Stage capability parity

The stable facade exposes the presentation properties that are serializable by the native Stage. Existing calls remain valid; the additional options only opt into the richer transform and compositor paths.

```ts
const ctx = createPrismatiXContext();

ctx.stage.backgroundRule(
  "Content/Backgrounds/night.png",
  "Content/Rules/soft-wipe.png",
  {durationMilliseconds: 700, vague: 48},
);
ctx.stage.layer("petals", "Content/Images/petals.png", {
  x: 80, y: 32, scaleX: 1.15, scaleY: 0.8, rotation: -8,
  alpha: 220, z: 2,
});
ctx.stage.camera({x: -24, y: 10, zoom: 1.08});
ctx.stage.screenEffect("vignette", 0.4);

// A packaged native shader/post effect. Names and ranges come from .pxeffect.
ctx.stage.customEffect("dream-tone", 0.65, {
  amount: 0.8,
  tint: [1, 0.8, 0.65, 1],
});
ctx.stage.nodeEffect("petals", "petal-glow", 0.75, {strength: 0.4});

ctx.stage.group("weather");
ctx.stage.particles("snow", "snow", {
  parent: "weather", seed: 42, rate: 90, maxParticles: 600,
  texture: "Content/Particles/snow-atlas.png",
  atlas: {columns: 4, rows: 2, frameCount: 8},
  spawnShape: "box",
  position: {x: [0, 1], y: [-0.1, 0]},
  velocity: {x: [-8, 8], y: [35, 75]},
  acceleration: {y: 4},
  lifetime: [5, 9], rotation: [0, 360], angularVelocity: [-30, 30],
  scale: [0.5, 1.2], initialOpacity: [0.7, 1],
  scaleOverLifetime: [{time: 0, value: 0.5}, {time: 1, value: 1}],
  opacityOverLifetime: [{time: 0, value: 0}, {time: 0.1, value: 1}, {time: 1, value: 0}],
  gravity: 3, wind: 2, variation: 0.25,
  burst: 40, loop: true, duration: 8,
});
```

Named values are checked by the native runtime against the packaged `number`,
`vec2`, or `color` declaration before a GPU uniform is updated. The older
positional `vec4[]` form remains accepted for existing extensions.

`flash` and `fade` are available on every graphics tier. `blur`, `vignette`, `color-grade`, and packaged shader effects require the GPU-effects tier, matching the native Stage behavior. Stage nodes (`Group`, compatibility image layers, and characters) inherit transform, visibility, opacity, and the nearest group effect from their parent. Particle presets (`rain`, `snow`, `sakura`, and `dust`/`light`) remain available; advanced emitters use the same native fixed-step, engine-seeded simulation and one geometry batch per texture/emitter. Hierarchy, node effects, complete emitter specifications and clocks, camera, transforms, background-rule progress, and post-effect values all use the existing save/checkpoint path.

## Screen and route transitions

Built-in screen effects are `none`, `fade`, `crossfade`, `slide-left`, and `slide-right`. A route call may select one directly, or a game may register a default for an outgoing/incoming route pair:

```ts
ctx.ui.setTransition("title", "hud", {
  preset: "crossfade",
  durationSeconds: 0.45,
});

ctx.ui.showModal("gallery", {
  preset: "slide-left",
  durationSeconds: 0.35,
});
```

The transition runtime captures the last complete screen as `outgoing`, renders the new screen as `incoming`, and keeps both native render textures alive for the duration. Route transitions and directly played effects use the same registry, compositor, and lifecycle.

Custom transitions are declarative. The factory runs once and receives opaque symbolic references for the two textures, normalized progress, and the logical viewport. JavaScript describes the plan; Renderer2D performs the per-frame work:

```ts
import {
  createPrismatiXContext,
  defineScreenTransition,
} from "@prismatix/runtime";

const ctx = createPrismatiXContext();

defineScreenTransition("game.tile-flip", ({
  outgoing, incoming, progress, viewport,
}) => ({
  operator: "tiles",
  columns: 10,
  rows: 6,
  stagger: 0.42,
  order: "row-major",
  outgoing,
  incoming,
  progress,
  viewport,
}));

ctx.ui.setTransition("title", "hud", {
  preset: "game.tile-flip",
  durationSeconds: 0.9,
});
```

The symbolic texture references are intentionally opaque: extension code cannot access or retain unsafe renderer pointers. The current native operators perform blend, slide, and tile composition without issuing a JavaScript draw call per frame or per tile.

Direct playback exposes the complete effect lifecycle:

```ts
const handle = ctx.effects.play("game.tile-flip", 0.9);
if (ctx.effects.isPlaying(handle)) {
  await ctx.effects.wait(handle);
}

// Alternative terminal paths:
ctx.effects.stop(handle);   // settle and report "stopped"
ctx.effects.cancel(handle); // abort and report "cancelled"
ctx.effects.status(handle);
```

Packaged schema-revision-3 `.pxeffect` transitions use the same lifecycle and
may take validated named parameters:

```ts
const handle = ctx.effects.play("ink-wipe", 0.9, {softness: 0.12});
await ctx.effects.wait(handle);
```

An awaited screen effect is a normal engine-owned suspension point, so JavaScript commands and Actions remain cooperative with the runtime. Full-screen playback is transient across route/state restoration; the restored screen and ordinary Stage/Timeline/save data remain authoritative.
