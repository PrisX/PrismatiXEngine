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

test("portrait expression roles must be complete and known", () => {
  const input = {width: 320, height: 600, layers: [
    {path: "Assets/Characters/body.png", width: 320, height: 600,
      role: "base" as const},
    {path: "Assets/Characters/face.png", width: 90, height: 60,
      role: "expression" as const},
  ]};
  const defined = defineLayeredPortrait(input);
  assert.equal(defined.valid, true, JSON.stringify(defined.diagnostics));
  assert.equal(defined.value?.schemaRevision, 1);
  assert.equal(defineLayeredPortrait({...input, layers: [input.layers[0]!,
    {path: "Assets/Characters/face.png", width: 90, height: 60}]}).valid, false);
  assert.equal(validateDocument("portrait", {...defined.value,
    layers: [{...defined.value!.layers[0]!, role: "outfit"},
      defined.value!.layers[1]!]}).valid, false);
});

test("expression layers carry blink and mouth frames; base layers cannot", () => {
  const input = {width: 320, height: 600, layers: [
    {path: "Assets/Characters/body.png", width: 320, height: 600,
      role: "base" as const},
    {path: "Assets/Characters/face.png", x: 100, y: 80, width: 90, height: 60,
      role: "expression" as const,
      blink: [{path: "Assets/Characters/eyes-half.png", width: 90, height: 30},
        {path: "Assets/Characters/eyes-shut.png", width: 90, height: 30}],
      mouth: [{path: "Assets/Characters/mouth-open.png", x: 130, y: 120,
        width: 30, height: 20}]},
  ]};
  const defined = defineLayeredPortrait(input);
  assert.equal(defined.valid, true, JSON.stringify(defined.diagnostics));
  assert.deepEqual(defined.value?.layers[1]?.blink?.[0],
    {path: "Assets/Characters/eyes-half.png", width: 90, height: 30});
  assert.deepEqual(defined.value?.layers[1]?.mouth?.[0],
    {path: "Assets/Characters/mouth-open.png", x: 130, y: 120, width: 30, height: 20});
  const onBase = defineLayeredPortrait({...input, layers: [
    {...input.layers[0]!, blink: input.layers[1]!.blink!}, input.layers[1]!]});
  assert.ok(onBase.diagnostics.some((diagnostic) => diagnostic.code === "PXSDKSEM1351"));
  assert.equal(defineLayeredPortrait({...input, layers: [input.layers[0]!,
    {...input.layers[1]!, mouth: []}]}).valid, false);
  assert.equal(defineLayeredPortrait({...input, layers: [input.layers[0]!,
    {...input.layers[1]!, blink: [{path: "Assets/Characters/x.png", width: 0, height: 4}]}]}).valid,
    false);
});
