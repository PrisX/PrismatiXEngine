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
  const curved: MotionTrack = {layer: "logo", property: "x", keys: [{
    time: 0, value: 10, curve: "cubicBezier",
    bezier: {x1: 0.2, y1: 0.8, x2: 0.8, y2: 1},
  }]};
  assert.deepEqual([numeric.property, visible.property, tinted.property,
    curved.property], ["opacity", "visible", "tint", "x"]);

  // These compile-time checks keep TS authoring ahead of JSON/native validation.
  // @ts-expect-error opacity keys are numeric, not RGB objects
  const wrongOpacity: MotionTrack = {layer: "logo", property: "opacity", keys: [{time: 0, value: {r: 1, g: 1, b: 1}}]};
  // @ts-expect-error tint keys require all three RGB channels
  const wrongTint: MotionTrack = {layer: "logo", property: "tint", keys: [{time: 0, value: 1}]};
  // @ts-expect-error visibility keys are boolean, not numeric
  const wrongVisible: MotionTrack = {layer: "logo", property: "visible", keys: [{time: 0, value: 1}]};
  // @ts-expect-error atlas frame keys are numeric, not boolean
  const wrongFrame: MotionTrack = {layer: "logo", property: "frame", keys: [{time: 0, value: true}]};
  // @ts-expect-error visible tracks cannot interpolate
  const curvedVisible: MotionTrack = {layer: "logo", property: "visible", keys: [{time: 0, value: true, curve: "easeOut"}]};
  // @ts-expect-error atlas frames cannot interpolate
  const curvedFrame: MotionTrack = {layer: "logo", property: "frame", keys: [{time: 0, value: 1, curve: "linear"}]};
  // @ts-expect-error cubic timing requires its control points
  const missingBezier: MotionTrack = {layer: "logo", property: "x", keys: [{time: 0, value: 1, curve: "cubicBezier"}]};
  // @ts-expect-error non-cubic timing cannot silently carry unused points
  const unusedBezier: MotionTrack = {layer: "logo", property: "x", keys: [{time: 0, value: 1, curve: "linear", bezier: {x1: 0, y1: 0, x2: 1, y2: 1}}]};
  void [wrongOpacity, wrongTint, wrongVisible, wrongFrame,
    curvedVisible, curvedFrame, missingBezier, unusedBezier];

  if (false) {
    // The main authoring entry point must reject the same error, not only an
    // explicitly annotated MotionTrack variable.
    // @ts-expect-error an opacity track cannot have a boolean key value
    defineMotion({id: "invalid", canvas: {width: 1, height: 1}, duration: 1, layers: [{id: "logo", image: "logo.png", width: 1, height: 1}], tracks: [{layer: "logo", property: "opacity", keys: [{time: 0, value: true}]}]});
  }
});
