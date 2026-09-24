import assert from "node:assert/strict";
import test from "node:test";

import {defineMotion, inlineMotion, parseDocument, serializeMotion,
  validateDocument} from "../src/index.js";

const input = {
  id: "m2-reveal",
  canvas: {width: 1920, height: 1080},
  duration: 1.5,
  layers: [
    {id: "body", image: "Assets/Motion/body.png", width: 400, height: 200,
      x: 960, y: 540, anchorX: 0.5, anchorY: 0.5},
    {id: "mark", parent: "body", image: "Assets/Motion/mark.png",
      width: 120, height: 120, x: 160, y: 40},
  ],
  tracks: [
    {layer: "body", property: "opacity" as const,
      keys: [{time: 0, value: 0}, {time: 0.5, value: 1, curve: "easeOut" as const}]},
    {layer: "mark", property: "visible" as const,
      keys: [{time: 0, value: false}, {time: 0.25, value: true}]},
  ],
};

test("motion helper emits canonical, safe layered animation document", () => {
  const result = defineMotion(input);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.format, "PrismatiXMotion");
  assert.equal(result.value?.scaleMode, "fit");
  assert.equal(result.value?.layers[0]?.opacity, 1);
  assert.equal(result.value?.tracks[1]?.keys[0]?.curve, "step");
  const serialized = serializeMotion(input);
  assert.equal(serialized.valid, true, JSON.stringify(serialized.diagnostics));
  assert.equal(parseDocument("motion", serialized.value!).valid, true);
});

test("tick-based motion uses revision 10 and inline composition normalizes clocks", () => {
  const outer = {
    id: "tick-outer", canvas: {width: 320, height: 180}, duration: 120,
    tickRate: 60, layers: [{id: "anchor", kind: "group" as const,
      timeOffset: -15}],
    markers: [{time: 30, audio: "Assets/Audio/start.ogg", bus: "se" as const}],
  };
  const child = {
    id: "tick-child", canvas: {width: 100, height: 60}, duration: 12,
    tickRate: 24,
    layers: [{id: "image", image: "Assets/Motion/image.png", width: 100, height: 60}],
    tracks: [{layer: "image", property: "x" as const, keys: [
      {time: 0, value: 0}, {time: 12, value: 100},
    ]}],
    markers: [{time: 6, audio: "Assets/Audio/pop.ogg", bus: "se" as const}],
  };
  const authored = defineMotion(outer);
  assert.equal(authored.valid, true, JSON.stringify(authored.diagnostics));
  assert.equal(authored.value?.schemaRevision, 10);
  assert.equal(authored.value?.duration, 120);
  assert.equal(parseDocument("motion", serializeMotion(outer).value!).valid, true);
  const composed = inlineMotion(outer, child, {id: "nested", at: 0.5});
  assert.equal(composed.valid, true, JSON.stringify(composed.diagnostics));
  assert.equal(composed.value?.tickRate, undefined);
  assert.equal(composed.value?.duration, 2);
  assert.equal(composed.value?.layers[0]?.timeOffset, -0.25);
  assert.equal(composed.value?.layers[1]?.timeOffset, -0.5);
  assert.equal(composed.value?.tracks[1]?.keys[1]?.time, 0.5);
  assert.deepEqual(composed.value?.markers?.map((marker) => marker.time), [0.5, 0.75]);
  assert.equal(defineMotion({...outer, tickRate: 2000}).valid, false);
  assert.equal(inlineMotion({...outer, tickRate: 2000}, child, {id: "bad"}).valid, false);
  assert.equal(defineMotion({...outer, duration: 86401 * 60}).valid, false);
  assert.equal(defineMotion({...outer, layers: [{...outer.layers[0]!, timeOffset: 86401 * 60}]}).valid, false);
  assert.equal(defineMotion({...child, tracks: [{...child.tracks[0]!, keys: [
    {time: 0, value: 0}, {time: 86401 * 24, value: 100},
  ]}]}).valid, false);
  assert.equal(validateDocument("motion", {...authored.value,
    schemaRevision: 9, tickRate: undefined, duration: 86401}).valid, false);
  assert.equal(validateDocument("motion", {...authored.value, schemaRevision: 9}).valid, false);
  assert.equal(validateDocument("motion", {...authored.value, tickRate: undefined}).valid, false);
});

test("motion validation rejects cycles, duplicate tracks, unsafe paths, and wrong key types", () => {
  const valid = defineMotion(input).value!;
  const layers = valid.layers.map((layer) => ({...layer}));
  assert.equal(validateDocument("motion", {...valid, layers: [
    {...layers[0], parent: "mark"}, layers[1],
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...valid, tracks: [
    ...valid.tracks, valid.tracks[0],
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...valid, layers: [
    {...layers[0], image: "Assets//body.png"}, layers[1],
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...valid, tracks: [
    valid.tracks[0], {...valid.tracks[1], keys: [{time: 0, value: 1, curve: "step"}]},
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...valid, tracks: [
    {...valid.tracks[0], keys: [{time: 0, value: 2, curve: "linear"}]}, valid.tracks[1],
  ]}).valid, false);
});

test("atlas frame tracks use revision 2 and reject fractional, interpolated or missing frames", () => {
  const atlas = defineMotion({
    id: "logo-frames", canvas: {width: 320, height: 120}, duration: 1,
    layers: [{id: "logo", image: "Assets/Motion/logo-atlas.png", width: 120,
      height: 40, frames: [
        {x: 0, y: 0, width: 120, height: 40},
        {x: 120, y: 0, width: 120, height: 40},
      ]}],
    tracks: [{layer: "logo", property: "frame", keys: [
      {time: 0, value: 0}, {time: 0.5, value: 1},
    ]}],
  });
  assert.equal(atlas.valid, true, JSON.stringify(atlas.diagnostics));
  assert.equal(atlas.value?.schemaRevision, 2);
  assert.equal(atlas.value?.tracks[0]?.keys[0]?.curve, "step");
  const document = atlas.value!;
  assert.equal(validateDocument("motion", {...document, schemaRevision: 1}).valid, false);
  for (const value of [1.5, 2, -1])
    assert.equal(validateDocument("motion", {...document, tracks: [{
      ...document.tracks[0]!, keys: [{time: 0, value, curve: "step"}],
    }]}).valid, false);
  assert.equal(validateDocument("motion", {...document, tracks: [{
    ...document.tracks[0]!, keys: [{time: 0, value: 1, curve: "linear"}],
  }]}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [{
    ...document.layers[0]!, frames: undefined,
  }]}).valid, false);
});

test("sound markers use revision 3 with ordered, named audio dependencies", () => {
  const marked = defineMotion({...input, markers: [
    {time: 0, audio: "Assets/Audio/reveal.ogg", bus: "se"},
    {id: "voice", time: 0.5, audio: "Assets/Audio/voice.ogg", bus: "voice"},
  ]});
  assert.equal(marked.valid, true, JSON.stringify(marked.diagnostics));
  assert.equal(marked.value?.schemaRevision, 3);
  assert.deepEqual(marked.value?.markers?.map((marker) => marker.id),
    ["marker-1", "voice"]);
  const collision = defineMotion({...input, markers: [
    {id: "marker-2", time: 0, audio: "Assets/Audio/reveal.ogg", bus: "se"},
    {time: 0.5, audio: "Assets/Audio/voice.ogg", bus: "voice"},
  ]});
  assert.equal(collision.valid, true, JSON.stringify(collision.diagnostics));
  assert.deepEqual(collision.value?.markers?.map((marker) => marker.id),
    ["marker-2", "marker-2-2"]);
  assert.equal(parseDocument("motion", serializeMotion({...input,
    markers: marked.value!.markers!}).value!).valid, true);
  const document = marked.value!;
  assert.equal(validateDocument("motion", {...document, schemaRevision: 2}).valid, false);
  assert.equal(validateDocument("motion", {...document, markers: [
    document.markers![1], document.markers![0],
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, markers: [
    document.markers![0], {...document.markers![1], id: "marker-1"},
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, markers: [
    {...document.markers![0], audio: "../outside.ogg"},
  ]}).valid, false);
});

test("layer blend modes select revision 4 and reject older or unknown modes", () => {
  const result = defineMotion({...input, layers: [
    {...input.layers[0]!, blendMode: "normal" as const},
    {...input.layers[1]!, blendMode: "additive" as const},
  ]});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.schemaRevision, 4);
  assert.equal(result.value?.layers[1]?.blendMode, "additive");
  const document = result.value!;
  assert.equal(validateDocument("motion", {...document, schemaRevision: 3}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, blendMode: "screen"}, document.layers[1]!,
  ]}).valid, false);
  const withSound = defineMotion({...input, layers: [
    {...input.layers[0]!, blendMode: "multiply" as const}, input.layers[1]!,
  ], markers: [{time: 0.2, audio: "Assets/Audio/pop.ogg", bus: "se" as const}]});
  assert.equal(withSound.valid, true, JSON.stringify(withSound.diagnostics));
  assert.equal(withSound.value?.schemaRevision, 4);
  assert.equal(withSound.value?.markers?.length, 1);
});

test("cubic timing curves select revision 5 and require bounded control points", () => {
  const key = {time: 0, value: 0, curve: "cubicBezier" as const,
    bezier: {x1: 0.1, y1: 0.8, x2: 0.4, y2: 0.9}};
  const result = defineMotion({...input, tracks: [{layer: "body", property: "opacity" as const,
    keys: [key, {time: 1, value: 1}]}]});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.schemaRevision, 5);
  assert.deepEqual(result.value?.tracks[0]?.keys[0]?.bezier, key.bezier);
  const document = result.value!;
  assert.equal(parseDocument("motion", serializeMotion({...input,
    tracks: [{layer: "body", property: "opacity", keys: [key, {time: 1, value: 1}]}],
  }).value!).valid, true);
  assert.equal(validateDocument("motion", {...document, schemaRevision: 4}).valid, false);
  assert.equal(validateDocument("motion", {...document, tracks: [{...document.tracks[0]!,
    keys: [{time: 0, value: 0, curve: "cubicBezier"}, {time: 1, value: 1, curve: "linear"}],
  }]}).valid, false);
  assert.equal(validateDocument("motion", {...document, tracks: [{...document.tracks[0]!,
    keys: [{...key, curve: "linear"}, {time: 1, value: 1, curve: "linear"}],
  }]}).valid, false);
  assert.equal(validateDocument("motion", {...document, tracks: [{...document.tracks[0]!,
    keys: [{...key, bezier: {...key.bezier, x1: 1.1}}, {time: 1, value: 1, curve: "linear"}],
  }]}).valid, false);
});

test("alpha mask references select revision 6 and reject unsupported composition", () => {
  const result = defineMotion({...input, layers: [
    {...input.layers[0]!, mask: "mark"}, input.layers[1]!,
  ]});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.schemaRevision, 6);
  const document = result.value!;
  assert.equal(parseDocument("motion", serializeMotion({...input, layers: [
    {...input.layers[0]!, mask: "mark"}, input.layers[1]!,
  ]}).value!).valid, true);
  assert.equal(validateDocument("motion", {...document, schemaRevision: 5}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, mask: "unknown"}, document.layers[1]!,
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    document.layers[0]!, {...document.layers[1]!, mask: "body"},
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, blendMode: "multiply"}, document.layers[1]!,
  ]}).valid, false);
});

test("transform groups and child-local time select revision 7", () => {
  const source = {
    id: "nested-logo", canvas: {width: 640, height: 360}, duration: 1,
    layers: [
      {id: "subclip", kind: "group" as const, x: 250, y: 130,
        opacity: 0.5, timeOffset: -0.25, timeScale: 2},
      {id: "logo", parent: "subclip", image: "Assets/Motion/logo.png",
        width: 100, height: 100},
    ],
    tracks: [{layer: "logo", property: "x" as const, keys: [
      {time: 0, value: 0}, {time: 1, value: 100},
    ]}],
  };
  const result = defineMotion(source);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.schemaRevision, 7);
  assert.equal(result.value?.layers[0]?.kind, "group");
  assert.equal(parseDocument("motion", serializeMotion(source).value!).valid, true);
  const document = result.value!;
  assert.equal(validateDocument("motion", {...document, schemaRevision: 6}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, image: "Assets/Motion/logo.png"}, document.layers[1]!,
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, mask: "logo"}, document.layers[1]!,
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, timeScale: 1001}, document.layers[1]!,
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    document.layers[0]!, {...document.layers[1]!, mask: "subclip"},
  ]}).valid, false);
  const longerLocalTrack = {...document, tracks: [{...document.tracks[0]!, keys: [
    document.tracks[0]!.keys[0]!, {time: 2, value: 100, curve: "linear" as const},
  ]}]};
  assert.equal(validateDocument("motion", longerLocalTrack).valid, true);
  assert.equal(validateDocument("motion", {...longerLocalTrack,
    tracks: longerLocalTrack.tracks.map((track) => ({...track, layer: "logo"})),
    layers: [{id: "logo", image: "Assets/Motion/logo.png", width: 100,
      height: 100}], schemaRevision: 6}).valid, false);
});

test("inherited RGB tint and color keys select revision 8 without changing opacity", () => {
  const tinted = defineMotion({
    id: "color-reveal", canvas: {width: 320, height: 120}, duration: 1,
    layers: [
      {id: "group", kind: "group", tint: {r: 1, g: 0.5, b: 1}},
      {id: "logo", parent: "group", image: "Assets/Motion/logo.png",
        width: 100, height: 40, opacity: 0.75},
    ],
    tracks: [{layer: "logo", property: "tint", keys: [
      {time: 0, value: {r: 1, g: 1, b: 1}},
      {time: 1, value: {r: 1, g: 0, b: 0}},
    ]}],
  });
  assert.equal(tinted.valid, true, JSON.stringify(tinted.diagnostics));
  assert.equal(tinted.value?.schemaRevision, 8);
  assert.deepEqual(tinted.value?.tracks[0]?.keys[0]?.value, {r: 1, g: 1, b: 1});
  const document = tinted.value!;
  assert.equal(validateDocument("motion", {...document, schemaRevision: 7}).valid, false);
  assert.equal(validateDocument("motion", {...document, layers: [
    {...document.layers[0]!, tint: {r: 1.2, g: 0.5, b: 1}}, document.layers[1]!,
  ]}).valid, false);
  assert.equal(validateDocument("motion", {...document, tracks: [{
    ...document.tracks[0]!, keys: [{time: 0, value: 1, curve: "linear"}],
  }]}).valid, false);
  assert.equal(validateDocument("motion", {...document, tracks: [{
    ...document.tracks[0]!, property: "opacity",
  }]}).valid, false);
});

test("selective parent transform inheritance selects revision 9", () => {
  const source = {...input, layers: [
    {...input.layers[0]!, inheritRotation: false, inheritScale: true},
    input.layers[1]!,
  ]};
  const result = defineMotion(source);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.value?.schemaRevision, 9);
  assert.equal(result.value?.layers[0]?.inheritRotation, false);
  assert.equal(result.value?.layers[0]?.inheritScale, true);
  assert.equal(parseDocument("motion", serializeMotion(source).value!).valid, true);
  const older = {...result.value!, schemaRevision: 8};
  assert.equal(validateDocument("motion", older).valid, false);
  assert.equal(validateDocument("motion", {...result.value!, layers: [
    {...result.value!.layers[0]!, inheritScale: 0}, result.value!.layers[1]!,
  ]}).valid, false);
});

test("inlined motion rebases hierarchy, masks, tracks and audio time", () => {
  const outer = {
    id: "title", canvas: {width: 640, height: 360}, duration: 2,
    layers: [{id: "anchor", kind: "group" as const,
      timeOffset: -0.2, timeScale: 2}],
    markers: [{id: "start", time: 0.2, audio: "Assets/Audio/start.ogg",
      bus: "se" as const}],
  };
  const child = {
    id: "letters", canvas: {width: 160, height: 90}, duration: 0.5,
    layers: [
      {id: "body", image: "Assets/Motion/body.png", width: 100,
        height: 80, mask: "wipe"},
      {id: "wipe", parent: "body", image: "Assets/Motion/wipe.png",
        width: 100, height: 80},
    ],
    tracks: [{layer: "wipe", property: "x" as const, keys: [
      {time: 0, value: -100}, {time: 0.5, value: 0},
    ]}],
    markers: [{time: 0.25, audio: "Assets/Audio/pop.ogg", bus: "se" as const}],
  };
  const result = inlineMotion(outer, child, {id: "logo", parent: "anchor",
    at: 0.3, speed: 2, x: 120, z: 4});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const motion = result.value!;
  assert.equal(motion.schemaRevision, 7);
  assert.equal(motion.layers[1]?.id, "logo");
  assert.equal(motion.layers[1]?.timeOffset, -0.6);
  assert.equal(motion.layers[2]?.parent, "logo");
  assert.equal(motion.layers[2]?.mask, "logo-wipe");
  assert.equal(motion.layers[2]?.z, 4);
  assert.equal(motion.layers[3]?.parent, "logo-body");
  assert.equal(motion.layers[1]?.visible, false);
  assert.equal(motion.tracks[0]?.layer, "logo");
  assert.equal(motion.tracks[0]?.property, "visible");
  assert.equal(motion.tracks[1]?.layer, "logo-wipe");
  assert.deepEqual(motion.markers?.map((marker) => [marker.id, marker.time]), [
    ["start", 0.2], ["logo-marker-1", 0.3125],
  ]);
  assert.equal(parseDocument("motion", serializeMotion(motion).value!).valid, true);
  const selective = inlineMotion(outer, child, {id: "selective", parent: "anchor",
    at: 0.3, speed: 2, inheritRotation: false, inheritScale: false});
  assert.equal(selective.valid, true, JSON.stringify(selective.diagnostics));
  assert.equal(selective.value?.schemaRevision, 9);
  assert.equal(selective.value?.layers[1]?.inheritRotation, false);
  assert.equal(selective.value?.layers[1]?.inheritScale, false);
  assert.equal(inlineMotion(outer, {...child, loop: true}, {id: "logo"}).valid, false);
  assert.equal(inlineMotion(outer, child, {id: "logo", speed: 0}).valid, false);
  assert.equal(inlineMotion(outer, child, {id: "anchor"}).valid, false);
  assert.equal(inlineMotion({...outer, layers: [{...outer.layers[0]!, timeScale: 0}]},
    child, {id: "logo", parent: "anchor"}).valid, false);
  const longChild = {
    id: "long", canvas: {width: 160, height: 90}, duration: 2,
    layers: [{id: "image", image: "Assets/Motion/long.png", width: 80,
      height: 40}],
    tracks: [{layer: "image", property: "opacity" as const, keys: [
      {time: 0, value: 0}, {time: 2, value: 1},
    ]}],
  };
  const shortOuter = {...outer, duration: 1, markers: []};
  const compressed = inlineMotion(shortOuter, longChild,
    {id: "fast", speed: 4});
  assert.equal(compressed.valid, true, JSON.stringify(compressed.diagnostics));
  assert.equal(compressed.value?.tracks[1]?.keys[1]?.time, 2);
  const blankCanvas = inlineMotion({id: "empty", canvas: {width: 160,
    height: 90}, duration: 1, layers: []}, longChild,
  {id: "first", speed: 4});
  assert.equal(blankCanvas.valid, true, JSON.stringify(blankCanvas.diagnostics));
  assert.deepEqual(blankCanvas.value?.layers.map((layer) => layer.id),
    ["first", "first-image"]);
  assert.equal(inlineMotion({id: "bad", canvas: {width: 160, height: 90},
    duration: 0, layers: []}, longChild, {id: "first"}).valid, false);
  const truncated = inlineMotion({id: "short", canvas: {width: 160,
    height: 90}, duration: 1, layers: []}, {...longChild,
    markers: [{time: 1.8, audio: "Assets/Audio/late.ogg", bus: "se" as const}]},
  {id: "cut", speed: 2});
  assert.equal(truncated.valid, true, JSON.stringify(truncated.diagnostics));
  const offEnd = inlineMotion(shortOuter, longChild, {id: "late", at: 0.8,
    speed: 2});
  assert.equal(offEnd.valid, false);
  assert.equal(offEnd.diagnostics[0]?.code, "PXMOTION2005");
  assert.equal(inlineMotion({...outer, duration: 0.3}, child,
    {id: "nested-late", parent: "anchor", at: 0.3, speed: 2})
    .diagnostics[0]?.code, "PXMOTION2005");
  const explicitlyCut = inlineMotion(shortOuter, {...longChild,
    markers: [{time: 1.8, audio: "Assets/Audio/late.ogg", bus: "se" as const}]},
  {id: "late", at: 0.8, speed: 2, allowTruncate: true});
  assert.equal(explicitlyCut.valid, true, JSON.stringify(explicitlyCut.diagnostics));
  assert.equal(explicitlyCut.value?.markers?.length ?? 0, 0);
  assert.equal(inlineMotion(shortOuter, longChild,
    {id: "unreachable", at: 2, allowTruncate: true}).valid, false);
});
