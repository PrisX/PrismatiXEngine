# Layered motion authoring proposal

Status: first image-layer slice and TSX UI mounting implemented; advanced composition
and Story playback below remain a proposal. This is not an `.mtn` parser or a claim that the
original M2/petal animations are reproduced.

The versioned `PrismatiXMotion` revision-1/2/3 `.pxmotion` contract has an
Authoring SDK `defineMotion`/`serializeMotion` helper, matching native parser,
unique layer/track and hierarchy checks, path checks, deterministic keyed
sampling, and a `Renderer2D` image-layer drawing adapter. A `MotionRect`
control can now play the descriptor in TSX-authored UI, splash scenes,
Player routes and native/WASM previews. `MotionSampler::Seek`
reuses its scratch storage and binary-searches ordered keys; its parent-world
transform and inherited opacity are sampled at one time. `fit` is the default
canvas scaling mode to avoid image squashing. Native and TypeScript contract
tests cover this slice. `compileProject` validates reachable descriptors
and collects their image dependencies. Splash playback now resolves static UI
image paths (including `.pxmotion` layers), queues a texture preload batch for
the following page while the current page is visible, and waits before exit
until that batch finishes. The Player also queues the configured start route's
static UI images during boot and holds the final splash until its batch is
ready. This avoids presenting a partially decoded next splash/title page,
but is not a general UI/Story preload handshake. Player routes now also gather
mounted UI image paths before fading in, and bound image paths are queued again
after each UI update; `TextureRect` retains its prior resident image until a
replacement is ready. Arbitrary custom drawing, Story playback dependencies,
texture residency under budget pressure, and M2-specific framebuffer
conformance remain open. Story motion commands,
masks, blur, blend modes, save/load playback binding, and
M2 conformance fixtures have not been added. The renderer adapter has not yet
passed an M2-specific framebuffer comparison. Revision 2 adds
per-layer atlas rectangles and discrete `frame` keys; the Authoring helper
selects it automatically when those fields are present. The native sampler
selects a source rectangle without changing textures at each key, and a
two-colour atlas framebuffer test checks the rendered output. Revision-1
descriptors remain readable and cannot silently contain revision-2 fields.
Revision 3 adds ordered `se`/`voice` audio markers: the Authoring SDK validates
them and includes their audio paths in package dependencies, and `MotionRect`
emits crossed events during normal playback to the Player audio service (also
for splash scenes). Seeking does not replay skipped cues. Script markers,
Story integration, and audio-predecode scheduling remain open.

Revision-1 sampling uses a layer's authored property until its first key;
thereafter each key's curve determines interpolation toward the next key.
Visibility keys are discrete. Layers with the same `z` paint in authored order.

## Existing foundation and the gap

`PrismatiXTimeline` already has typed tracks, easing, markers and nested clips;
`AnimationClip` and `TimelinePlayer` provide deterministic sampling. UI scenes
can position image nodes. What is missing is a declarative visual composition
that binds tracks to layers and expresses image sequence, masks, blend modes,
and dependency preloading. The port currently reconstructs these effects with
generated UI files and Player-specific presentation code.

## Target `.pxmotion` document

The file is a reusable composition, referenceable from a UI node, splash,
transition or Story command. Time is in seconds; coordinates are in a declared
logical canvas, independent of window scaling.

- `canvas`: width, height, scale mode and transparent background.
- `layers`: stable IDs, z-order, image/atlas resource, anchor, transform,
  opacity, tint, optional blend mode and optional mask layer. A layer can be a
  nested composition. Every referenced asset is a packager dependency.
- `tracks`: target layer/property plus typed keyframes and curve. Properties
  include position, scale, rotation, opacity, source rectangle/frame index,
  mask progress, tint and blend weight. Discrete properties use `step`.
- `markers`: time-stamped sound/voice/script events with once-per-iteration
  semantics. Markers do not execute during a seek unless requested.
- `playback`: duration, loop/ping-pong, speed, pre-roll and named exit point.
- `reducedMotion`: author-supplied still frame or abbreviated clip.

For example, a petal is one transparent texture layer with a four-second
loop: position follows a keyed Bezier path, rotation increases, opacity fades
at both ends, and scale varies slightly. M2-style logo reveals can combine an
atlas frame track with an animated mask and a short sound marker. Neither
example requires importing or emulating `.mtn` itself.

## Runtime and SDK contract

1. Authoring SDK validates layer IDs, property types, time ordering, atlas
   frame bounds, mask cycles and resource paths; compiler emits an immutable
   motion graph and recursively lists dependencies.
2. Renderer evaluates every visible layer at one shared clock sample, then
   composites to an off-screen target. Mask and blend semantics are explicit;
   no first-frame flash while decoding or switching scenes.
3. Playback state (clip ID, time, iteration, marker cursor) is serializable for
   save/load and preview scrubbing. Multiple instances share assets but not
   clocks. Asset preloading completes before a transition starts.
4. UI/Story expose `play`, `stop`, `await` and `crossfade` without a game-specific
   Player switch. Input-driven skips land on the authored exit state and fire
   only markers declared `onSkip`.

## Incremental implementation and acceptance

The image-layer/transform sampling primitive, atlas frame track, audio markers,
descriptor dependency packaging and TSX UI mounting are in place; Story mounting
and durable playback remain necessary before authors can use it end-to-end. Then add
GPU/CPU mask parity and blend modes. The Senren test fixtures should capture M2 and the HUD petal
at fixed timestamps (0, 0.25, 0.5, 1.0 seconds), compare composition geometry,
verify no blank transition frame, and exercise skip, reduced motion, save/load
and non-16:9 scaling. Until those tests pass, the port must label its M2 and
petal effects as approximations.
