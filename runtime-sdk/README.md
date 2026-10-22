# @prismatix/runtime

TypeScript declarations and a thin ergonomic facade for PrismatiX JavaScript extensions.

The shipped game still runs the native C++ Runtime with its embedded JavaScript engine. This package is authoring/build-time tooling only; it does not add Node.js or a browser runtime to the Player.

Projects created with `prismatix create` already depend on it, pinned to the
same exact version as `@prismatix/cli` (`pnpm add -D @prismatix/runtime` adds it
to an older project).

```ts
import {createPrismatiXContext, defineCommand} from "@prismatix/runtime";

const ctx = createPrismatiXContext();

defineCommand("game.raiseAffection", async ({amount}) => {
  const current = Number(ctx.variables.get("affection") ?? 0);
  ctx.variables.set("affection", current + amount);
  await ctx.time.wait(0.1);
});
```

For code that prefers the host API directly, importing the package's types also declares the existing `Engine`, `px`, and `DisplayMode` globals; `"types": ["@prismatix/runtime/sandbox"]` in a tsconfig adds the sandbox `console`.

## TypeScript extensions

Start one with the CLI (PrismatiX Studio's New Extension dialog runs the same command):

```sh
prismatix extension create my-fx --command greet --action open
```

It writes `Content/Extensions/my-fx/`: the manifest `my-fx.pxextension` (`"entry": "main.ts"`), `main.ts` with a handler stub per command and Action, a `tsconfig.json`, and `prismatix-extension.d.ts`, and registers the manifest in `prismatix.json`. `--lang js` writes a plain `main.js` that uses the `Engine` global instead.

Every command that builds the game (`validate`, `build`, `pack`, `run --dev` and so Studio's Play button, `test`, the web and Android exports) bundles a `.ts` entry with esbuild into one ES module beside it (`main.ts` -> `main.js`) and publishes that, with the manifest's `entry` pointing at it. The Player never sees TypeScript. The bundle contains:

- `@prismatix/runtime` (this facade; a few KB);
- relative imports (`./lib/util` or `./lib/util.ts`) inside the project;
- pure-JavaScript npm packages from the project's `node_modules`, inlined. A Node.js built-in (`fs`, `node:path`, ...) or `@prismatix/authoring-sdk` is a build error (`PXCLIEXT1001`), as is a syntax error or an unresolved import. A package that calls `Date`, timers, `fetch` or `Math.random` bundles but fails in the sandbox, so prefer small, pure ones.

Output is deterministic (no machine paths) and cached in `.prismatix/cache/extensions` by the content hash of every input. Each build also type-checks the extension against this package, the sandbox globals (ES2023, no DOM or Node.js) and its manifest. Type errors are warnings (`PXCLIEXT2001`, with the `.ts` file and line), so a quick experiment still plays; using `Date`, `eval`, `Function` or `Math.random` is warning `PXCLIEXT2002`. `prismatix test` and `prismatix run` (so Studio's 試玩) map Player log positions in a bundle back to the `.ts` line through the cached source map.

### Typed handlers from the manifest

`prismatix-extension.d.ts` augments this package with the manifest's declarations, so the id and arguments of `defineCommand`/`defineAction` are checked without hand-written generics:

```ts
// my-fx.pxextension declares myFx.greet {name: string (required), times: integer = 1}
defineCommand("myFx.greet", ({name, times}) => {   // name: string, times: number
  ctx.log.info(name.repeat(times));
});
defineCommand("myFx.typo", () => {});               // type error: not declared
```

Parameter types map to `boolean`, `number` (integer/number), `string` (a string enum becomes a union of its values), `Vec2Value` `{x, y}`, `Rect` `{x, y, w, h}`, `Color` `{r, g, b, a}` (0-255), `ResourceValue` `{id, path}`, `string` (uuid/token), `RuntimeValue[]` and objects. A parameter that is `required` or has a `default` is always present. Run `prismatix extension types` after editing the manifest by hand (`add-command`/`add-action` refresh it themselves); builds check against fresh declarations and warn (`PXCLIEXT2003`) when the file on disk is stale. Code without the generated file can still pass explicit argument types: `defineCommand<{amount: number}>(...)`.

### Handlers, waits and cancellation

- A command whose manifest says `"await": true` holds the Story until its handler finishes; `false` lets the Story go on at once. An Action's `reentry` decides what another press does while it runs: `allow`, `ignoreWhileRunning` or `restart` (the running one is cancelled, see below, before the new one starts).
- Await only Engine waits: `ctx.time.wait`, animation, screen-effect, video and audio handles, `ctx.session.say`. Each is a saved suspension point that survives save, load and rollback by replaying the handler to the same point (keep side effects in `ctx.state`). Awaiting any other Promise is rejected by the Player.
- A handler may also be a generator that yields the matching tokens (`yield ctx.time.token(0.5)`, `yield ctx.audio.play(path).token()`).
- Every handler gets a cancellation signal as `context.abortSignal` (an Action's second argument; a command's second argument is `{abortSignal}`). See [Cancellation](#cancellation).
- The entry runs once when the game loads; register handlers, events and state there, never inside a handler (the Player rejects registration from a running handler).
- `ctx.events.on(name, handler)` returns a function that removes the listener from the Player (it is not called again, not even later in the event being delivered); `once` delivers one event and then removes itself, and so do `ctx.frame.onUpdate`, `ctx.animation.onMarker` and the `observe*` functions. `ctx.events.listenerCount(name?)` counts this extension's live listeners. A listener added while an event is delivered first hears the next one. Listeners are not saved: a load or rollback keeps the listeners that exist; the Player releases all of an extension's listeners when it reloads the extension or shuts down. On the raw host, `Engine.on` returns the listener id that `Engine.off(id)` removes. Built-in events are typed: `engine.ready`, `engine.shutdown`, `frame.update` (`delta` seconds as a string; `ctx.frame.onUpdate` gives a number), `scenario.started`, `save.loaded`, `save.written` and `timeline.marker`.
- `ctx.state.define(id, {initial, version, migrate})` returns a saved, rollback-aware value (`get`, `set`, `update`, `reset`) built on the state providers below. It needs the `persistence` capability.
- `ctx.log.info/warn/error` write to the Player log, like the sandbox `console`.
- The Player refuses a call whose capability the manifest's `capabilities` do not list: `ui` for `defineAction` and `ctx.ui` routes, `audio`, `video`, `animation`, `persistence` for `ctx.saves` and `ctx.state`, `render` for retained `ctx.renderer.upsert/remove/clear`, `input` for `ctx.input.actionPressed/actionDown`, and `runtime` for the rest. Builds warn about a call the manifest does not cover (`PXCLIEXT2004`).
- Player log lines and errors that name a bundle position (`main.js:12:5`) are shown at the TypeScript line by `prismatix test` and while `prismatix run` plays the game. With `--json`, `run` writes each extension log line (an error with its stack, a warning, `ctx.log`/`console` output) as a `run.extension` record whose `span` is the `.ts` file, line and column of the first authored frame; Studio's 試玩 console and its problems list show them there.

### Cancellation

A command or Action that is waiting is cancelled when:

| `kind` | Handler | When |
| --- | --- | --- |
| `restart` | Action | it is started again and its `reentry` is `restart` (the old run ends before the new one starts) |
| `closed` | Action | the UI scene (route) that started it is replaced or closed, including when the game starts from the title |
| `cancelled` | Action | the engine cancels it without a narrower reason (a Behavior graph or host code) |
| `skip` | command | Skip (the skip mode or held Ctrl) reaches a command that holds the Story; the Story then moves on |
| `rollback` / `load` | both | rollback or loading a save (also a Preview checkpoint) replaces the running handlers |
| `reset` | both | the host restarts them (Preview restart) |
| `reload` | both | the extension is loaded again |
| `shutdown` | both | the game closes while they run |

Cancelling one handler runs, in order and synchronously: the signal's `onabort` and its `abort` listeners (in the order they were added, each called once; `signal.aborted` is `true` and `signal.reason` is the `AbortError`), then the handler itself: the engine wait it awaits rejects with that `AbortError` (a generator gets it thrown at its `yield`, then `return()` if it yields again), so its `catch` and `finally` blocks run before the Player continues. Any engine wait started while it unwinds (`ctx.time.wait` inside `finally`) throws the same error at once. A handler that lets the `AbortError` escape ends quietly; any other error is reported as usual. A handler that finishes on its own never sees its signal abort.

```ts
import {createPrismatiXContext, defineAction, isAbortError} from "@prismatix/runtime";

const ctx = createPrismatiXContext();

defineAction("myFx.charge", async (_args, {abortSignal}) => {
  const hum = ctx.audio.play("Assets/SE/charge.ogg", {loop: true});
  abortSignal.onAbort((reason) => ctx.log.info(`charge stopped: ${reason.kind}`));
  try {
    await ctx.time.wait(3);
    ctx.ui.showModal("charged");
  } catch (error) {
    if (!isAbortError(error)) throw error;   // only the cancellation is expected here
  } finally {
    hum.stop(200);                            // runs on restart, on leaving the screen, ...
  }
});
```

The `AbortError` has `name: "AbortError"`, `code: "PXJS7301"` and `kind` from the table; `isAbortError(value)` tests for it and `signal.throwIfAborted()` rethrows it. Cancellation is deterministic: it happens at a fixed point (the press, the route change, the skip, the start of a load or rollback), and when the cancellation is requested while JavaScript of the same extension is running (an Action that changes the route it was started from), it runs as soon as that host call returns. For `load` and `rollback` the old handlers end before the saved ones are replayed, so whatever their `finally` writes (variables, `ctx.state`) is overwritten by the restored state; the replayed handlers get a fresh, unaborted signal.

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

Authored screens can render the same live VN data as the compatibility HUD,
backlog, choice list, and settings controls. Snapshot reads are synchronous;
observers receive one immediate value and then changed snapshots on the shared
`frame.update` clock. Calling the returned function stops delivery.

```ts
const stop = ctx.session.observeDialogue((dialogue) => {
  // Bind speaker/displayText/effect to any authored presentation.
  console.log(dialogue.speaker, dialogue.displayText);
});

const choices = ctx.session.choices();
if (choices.length > 0) ctx.session.selectChoice(choices[0].index);

const entry = ctx.backlog.entries().at(-1);
if (entry?.voice) ctx.backlog.replayVoice(entry.sequence);

ctx.settings.set("textScale", 1.25);
stop();
```

Backlog actions use stable `sequence` identities rather than physical array
indices, so bounded-log eviction does not retarget replay or rollback actions.

An asynchronous Story command can author one dialogue line with
`ctx.session.say()`. The line uses that Story operation's `documentId`,
`sourceId`, and `operationId` for seen state, backlog, rollback, and save
migration. Give each line its own Story command and stable sidecar ID. The
promise resolves after Advance reveals the full line and Advance is pressed
again. A saved wait resumes from the same line without replaying its voice or
adding a second backlog entry.

```ts
defineCommand<{text: string; speaker: string}>("game.narrate", async ({text, speaker}) => {
  await ctx.session.say(text, {speaker});
});
```

The command manifest must declare `await: true` and a rollback boundary.
Call `say` from an anchored Story command callback; Action handlers and
unanchored scripts cannot author dialogue.

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

For a keyboard settings screen, inspect the current scancodes and use a
readable SDL key name to replace one action's keyboard shortcut atomically:

```ts
const current = ctx.input.keyBindings("quick-save").map(ctx.input.keyName);
// Start this from a settings-screen action, then poll in its update callback.
ctx.input.beginKeyCapture();
const captured = ctx.input.takeCapturedKey();
if (captured !== null) {
  // Escape is returned as an ordinary key; this UI treats it as cancel.
  if (ctx.input.keyName(captured) !== "Escape") {
    const result = ctx.input.rebindKey("quick-save", captured);
    if (result.status === "conflict") {
      // Show result.conflicts and let the player choose another key or allow sharing.
    }
  }
}
```

Capture is one-shot. The physical key is withheld from normal actions and text
input, and remains available across frame boundaries until `takeCapturedKey()`
or `cancelKeyCapture()`; `keyCaptureActive()` reports whether it is still
waiting. A settings screen should cancel capture when it closes.

An authored game-catalog binding such as
`{"key":"G","command":"screen.open","argument":"gallery"}` becomes the
named action `screen.open.gallery`. The native Player activates it only in
the gameplay input context, and a saved override replaces its default key:

```ts
ctx.input.rebindKey("screen.open.gallery", "F8");
ctx.input.registerAction("photo.inspect");
ctx.input.assignContext("photo.inspect", "photo-overlay");
ctx.input.pushContext("photo-overlay");
// Pop the overlay when its screen closes.
ctx.input.popContext();
```

`rebindKey` leaves gamepad bindings untouched. Unknown actions and invalid key
names are reported without altering the old binding. Passing `true` as its
third argument explicitly permits a shared key. The native Player persists
keyboard overrides in its existing settings file, including actions that a
script registers later. Other hosts must persist the native Input binding map
themselves. The Player selects `game` or `ui` as its base context; extensions
can assign custom actions to a context and push/pop temporary overlays.
There is not yet a project-wide declarative action-context map or a ready-made
remapping widget; authors still provide the settings-screen action, label, and
conflict-resolution UI.

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

Packaged `.pxeffect` transitions use the same lifecycle and
may take validated named parameters:

```ts
const handle = ctx.effects.play("ink-wipe", 0.9, {softness: 0.12});
await ctx.effects.wait(handle);
```

An awaited screen effect is a normal engine-owned suspension point, so JavaScript commands and Actions remain cooperative with the runtime. Full-screen playback is transient across route/state restoration; the restored screen and ordinary Stage/Timeline/save data remain authoritative.
