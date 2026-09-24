import assert from "node:assert/strict";
import test from "node:test";

import {defineMotion, parseDocument, serializeMotion, validateDocument} from "../src/index.js";

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
});
