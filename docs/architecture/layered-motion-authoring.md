# Layered motion authoring proposal

Status: image-layer, timing, blend, alpha-mask, and inline nested-transform slices plus TSX UI mounting implemented; advanced composition
and Story playback below remain a proposal. This is not an `.mtn` parser or a claim that the
original M2/petal animations are reproduced.

The versioned `PrismatiXMotion` revision-1 through revision-10 `.pxmotion` contract has an
Authoring SDK `defineMotion`/`serializeMotion` helper, matching native parser,
unique layer/track and hierarchy checks, path checks, deterministic keyed
sampling, and a `Renderer2D` image-layer drawing adapter. A `MotionRect`
control can now play the descriptor in TSX-authored UI, splash scenes,
Player routes and native/WASM previews. `MotionSampler::Seek`
reuses its scratch storage and binary-searches ordered keys; its parent-world
transform and inherited opacity are sampled deterministically, with revision-7
child-local time offsets and scales. `fit` is the default
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
masks beyond one alpha-mask source per content layer, blur, specialized blend equations, save/load playback binding, and
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
Revision 4 adds layer-local normal/additive/multiply blending. Authoring and
native parsing reject the field in older revisions; a framebuffer test draws
overlapping layers from one atlas and verifies both additive and multiply
pixels. This is not an alpha-mask implementation or M2 conformance fixture.
Revision 5 adds bounded cubic Bézier timing controls for numeric property
tracks. The authoring helper chooses it automatically, older revisions reject
the controls, and native `Seek` solves the timing cubic deterministically.
The original `.mtn` custom timing curves still require semantic comparison
before a converter may claim equivalence.
Revision 6 adds a layer `mask` reference. The referenced layer is sampled on
the same clock and rendered only into reusable alpha coverage targets; its
alpha multiplies the content layer. SDK and native validation reject missing,
self-referential, nested or non-normal-blend masks. GPU D3D12 and basic D3D11
framebuffer tests check full, empty, half-alpha, and moving coverage. This is
not an M2 conformance result: external nested clip references, blur, particles, and
source-specific blend behavior still block a faithful converter.
Revision 7 adds non-drawing transform groups and child-local time remapping.
The Authoring helper selects it automatically; the native sampler computes
each layer's local time in hierarchy order before sampling property tracks.
The GPU framebuffer test verifies that a delayed child moves at the expected
time without painting its group, and the project compiler omits groups from
image dependencies. This allows an offline `.mtn` importer to flatten some
referenced submotions under groups, but does not interpret external references,
spatial paths, particles, mesh deformations, or source-specific mask flags.
`inlineMotion` now performs that offline rebasing for already-authored
non-looping clips, including mask references and audio-marker times. Revision-7
track keys are bounded by the local clock's 86,400-second limit instead of the
outer duration; audio markers remain on the outer clock. This is composition
of known `.pxmotion` inputs, not an `.mtn` parser or conformance result.
The helper also accepts an empty outer layer list as a composition starting
point and rejects temporal overflow unless truncation is explicitly requested.
Revision 8 adds a three-channel animated RGB tint and inherited modulation
through transform groups. Its native sampler and framebuffer test verify that
color changes reach the pixels without changing alpha. This maps the ordinary
color modulation seen in M2's `m2logo` source, but not its source-specific
mask flags, blur, embedded-motion extraction, or complete color equations.
In the decoded M2 source, `src/bure/icon1` and `src/bure2/icon1` resolve to
280×112 indexed images. Their names alone do not imply a runtime blur effect;
the need for per-layer blur in this particular clip is unproven until the
source semantics and rendered frames are compared.
Revision 9 lets a child retain the parent's affine translation while opting
out of inherited rotation and/or scale/stretch. Native sampling tests cover
all four combinations and reflected parents. This fills an Engine capability
gap revealed by nonuniform `inheritMask` values in the Senren M2 source; the
bitfield-to-rule mapping and rendered-frame equivalence are still unproven.
Revision 10 accepts an explicit ticks-per-second rate for authored duration,
layer offsets, track keys, and audio markers. Native playback normalizes all
four to seconds; `inlineMotion` can compose clips with different rates after
validating each source, while retaining the outer rate in the result. It also
accepts `atTicks` for a nested start without manual division. The M2 source
rate is still unverified, and this does not resolve source-specific paths or
composition rules.

Revision-1 sampling uses a layer's authored property until its first key;
thereafter each key's curve determines interpolation toward the next key.
Visibility keys are discrete. Layers with the same `z` paint in authored order.

## Existing foundation and the gap

`PrismatiXTimeline` has typed tracks, easing, markers and nested clips;
`AnimationClip` and `TimelinePlayer` provide deterministic sampling. The
implemented `.pxmotion` resource now binds image/group layers to tracks,
atlas frames, alpha masks, tint, blend modes, audio markers and dependencies;
TSX `<Motion>` can display it. The port's M2 animation still uses a substitute
UI animation. Missing work includes source-specific `.mtn` conversion with
rendered-frame conformance, richer curve/compositing semantics, and direct
Story/Stage playback with preloading and save/load behavior.

## Target `.pxmotion` document

The file is a reusable composition, currently referenceable from a UI node
or splash; transition and Story presentation remain targets. Revisions 1–9
author time in seconds; revision 10 can author it in declared ticks per
second. Native playback uses seconds. Coordinates are in a declared logical
canvas, independent of window scaling.

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
and durable playback remain necessary before authors can use it end-to-end.
Basic/GPU framebuffer coverage now exists for normal/additive/multiply blending
and one alpha-mask source; software/WASM parity and M2 source-image conformance
remain open. The Senren test fixtures should capture M2 and the HUD petal
at fixed timestamps (0, 0.25, 0.5, 1.0 seconds), compare composition geometry,
verify no blank transition frame, and exercise skip, reduced motion, save/load
and non-16:9 scaling. Until those tests pass, the port must label its M2 and
petal effects as approximations.
