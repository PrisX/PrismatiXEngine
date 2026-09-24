import assert from "node:assert/strict";
import test from "node:test";

import {defineMotion} from "../src/index.js";
import type {MotionTrack} from "../src/index.js";

test("motion track property determines its authoring key type", () => {
  const numeric: MotionTrack = {layer: "logo", property: "opacity",
    keys: [{time: 0, value: 0}, {time: 1, value: 1}]};
  const visible: MotionTrack = {layer: "logo", property: "visible",
    keys: [{time: 0, value: false}]};
  const tinted: MotionTrack = {layer: "logo", property: "tint",
    keys: [{time: 0, value: {r: 1, g: 0.5, b: 1}}]};
  assert.deepEqual([numeric.property, visible.property, tinted.property],
    ["opacity", "visible", "tint"]);

  // These compile-time checks keep TS authoring ahead of JSON/native validation.
  // @ts-expect-error opacity keys are numeric, not RGB objects
  const wrongOpacity: MotionTrack = {layer: "logo", property: "opacity", keys: [{time: 0, value: {r: 1, g: 1, b: 1}}]};
  // @ts-expect-error tint keys require all three RGB channels
  const wrongTint: MotionTrack = {layer: "logo", property: "tint", keys: [{time: 0, value: 1}]};
  // @ts-expect-error visibility keys are boolean, not numeric
  const wrongVisible: MotionTrack = {layer: "logo", property: "visible", keys: [{time: 0, value: 1}]};
  // @ts-expect-error atlas frame keys are numeric, not boolean
  const wrongFrame: MotionTrack = {layer: "logo", property: "frame", keys: [{time: 0, value: true}]};
  void [wrongOpacity, wrongTint, wrongVisible, wrongFrame];

  if (false) {
    // The main authoring entry point must reject the same error, not only an
    // explicitly annotated MotionTrack variable.
    // @ts-expect-error an opacity track cannot have a boolean key value
    defineMotion({id: "invalid", canvas: {width: 1, height: 1}, duration: 1, layers: [{id: "logo", image: "logo.png", width: 1, height: 1}], tracks: [{layer: "logo", property: "opacity", keys: [{time: 0, value: true}]}]});
  }
});
