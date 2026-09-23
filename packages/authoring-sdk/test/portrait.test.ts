import assert from "node:assert/strict";
import test from "node:test";

import {defineLayeredPortrait, serializeLayeredPortrait, validateDocument}
  from "../src/index.js";

test("typed layered portrait authoring supplies compositor defaults and canonical JSON", () => {
  const input = {width: 691, height: 1450, layers: [
    {path: "Assets/Characters/Layers/body.png", width: 691, height: 1450},
    {path: "Assets/Characters/Layers/face.png", x: 302, y: 153,
      width: 174, height: 74, opacity: 217},
  ]};
  const defined = defineLayeredPortrait(input);
  assert.equal(defined.valid, true, JSON.stringify(defined.diagnostics));
  assert.deepEqual(defined.value?.layers[0], {
    path: "Assets/Characters/Layers/body.png", x: 0, y: 0,
    width: 691, height: 1450, opacity: 255,
  });
  const serialized = serializeLayeredPortrait(input);
  assert.equal(serialized.valid, true, JSON.stringify(serialized.diagnostics));
  assert.equal(validateDocument("portrait", JSON.parse(serialized.value!)).valid,
    true);
});

test("portrait contract rejects layers that Stage would skip or cannot load", () => {
  const base = {width: 320, height: 600, layers: [
    {path: "Assets/Characters/Layers/body.png", width: 320, height: 600},
  ]};
  assert.equal(defineLayeredPortrait({...base, width: 0}).valid, false);
  assert.equal(defineLayeredPortrait({...base, layers: []}).valid, false);
  assert.equal(defineLayeredPortrait({...base, layers: [{...base.layers[0]!,
    width: 0}]}).valid, false);
  assert.equal(defineLayeredPortrait({...base, layers: [{...base.layers[0]!,
    opacity: 256}]}).valid, false);
  assert.equal(defineLayeredPortrait({...base, layers: [{...base.layers[0]!,
    path: "../outside.png"}]}).valid, false);
  const nested = defineLayeredPortrait({...base, layers: [{...base.layers[0]!,
    path: "Assets/Characters/other.pxportrait"}]});
  assert.equal(nested.valid, false);
  assert.ok(nested.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSDKSEM1350"));
});
