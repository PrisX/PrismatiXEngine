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
