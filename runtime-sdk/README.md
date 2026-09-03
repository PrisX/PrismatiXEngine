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

// A packaged native shader/post effect. Parameters are at most eight vec4s.
ctx.stage.customEffect("dream-tone", 0.65, [[0.8, 0.2, 0, 0]]);
```

`flash` and `fade` are available on every graphics tier. `blur`, `vignette`, `color-grade`, and packaged shader effects require the GPU-effects tier, matching the native Stage behavior. Stage state—including camera, transforms, background-rule progress, and post-effect values—continues to use the existing save/checkpoint path.

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

An awaited screen effect is a normal engine-owned suspension point, so JavaScript commands and Actions remain cooperative with the runtime. Full-screen playback is transient across route/state restoration; the restored screen and ordinary Stage/Timeline/save data remain authoritative.
