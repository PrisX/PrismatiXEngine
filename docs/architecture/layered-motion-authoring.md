# Layered motion authoring proposal

Status: design only. This is a native PrismatiX authoring capability, not an
`.mtn` parser or a claim that the original M2/petal animations are reproduced.

## Existing foundation and the gap

`PrismatiXTimeline` already has typed tracks, easing, markers and nested clips;
`AnimationClip` and `TimelinePlayer` provide deterministic sampling. UI scenes
can position image nodes. What is missing is a declarative visual composition
that binds tracks to layers and expresses image sequence, masks, blend modes,
and dependency preloading. The port currently reconstructs these effects with
generated UI files and Player-specific presentation code.

## Proposed `.pxmotion` document

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

First add image layers with transform/opacity tracks and deterministic nested
playback. Then add atlas frames and sound markers. Add GPU/CPU mask parity and
blend modes last. The Senren test fixtures should capture M2 and the HUD petal
at fixed timestamps (0, 0.25, 0.5, 1.0 seconds), compare composition geometry,
verify no blank transition frame, and exercise skip, reduced motion, save/load
and non-16:9 scaling. Until those tests pass, the port must label its M2 and
petal effects as approximations.
