import assert from "node:assert/strict";
import test from "node:test";

import {Group, Scene, Slider, Text, TextureButton, compileJsxUi} from "../src/index.js";
import {jsx, jsxs, type UiElement} from "../src/jsx-runtime.js";

function compiled(children: readonly UiElement[]) {
  const result = compileJsxUi(jsx(Scene, {name: "Shorthand", width: 640, height: 360,
    children: jsxs(Group, {name: "Root", fill: true, children})}));
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  return (name: string) => result.value!.nodes.find((node) => node.name === name)!;
}

test("TextureButton skin derives state images and explicit paths win", () => {
  const node = compiled([
    jsx(TextureButton, {name: "Plain", skin: "Assets/UI/save/bt_save",
      position: [0, 0], size: [96, 47], action: "save.open"}),
    jsx(TextureButton, {name: "Full", position: [0, 50], size: [96, 47],
      skin: {base: "Assets/UI/mw/bt_auto", states: ["hover", "pressed", "disabled"], extension: ".webp"},
      disabledPath: "Assets/UI/mw/locked.webp", action: "mode.auto"}),
  ]);
  const plain = node("Plain").runtimeProperties;
  assert.equal(plain.normalPath, "Assets/UI/save/bt_save-normal.png");
  assert.equal(plain.hoverPath, "Assets/UI/save/bt_save-hover.png");
  assert.equal(plain.pressedPath, "Assets/UI/save/bt_save-hover.png");
  assert.equal("disabledPath" in plain, false);
  const full = node("Full").runtimeProperties;
  assert.equal(full.pressedPath, "Assets/UI/mw/bt_auto-pressed.webp");
  assert.equal(full.disabledPath, "Assets/UI/mw/locked.webp");
  assert.equal("skin" in full, false);
});

test("a leading ! in bind shorthand applies the Not formatter", () => {
  const node = compiled([jsx(TextureButton, {name: "Page", skin: "Assets/UI/page",
    position: [0, 0], size: [41, 47], action: "saveLoad.page",
    bind: {enabled: "!saveLoad.page1", visible: "saveLoad.visible"}})]);
  assert.deepEqual(node("Page").bindings, {
    enabled: {path: "saveLoad.page1", formatter: "Not"},
    visible: {path: "saveLoad.visible"},
  });
});

test("Slider is a typed native control with skin and range props", () => {
  const node = compiled([jsx(Slider, {name: "Master", position: [919, 213], size: [400, 18],
    minimum: 0, maximum: 100, step: 1, railPath: "Assets/UI/rail.png",
    railHoverPath: "Assets/UI/rail-hover.png", knobPath: "Assets/UI/knob.png",
    railHeight: 18, knobSize: 20, accessibilityLabel: "Master volume"})]);
  const slider = node("Master");
  assert.equal(slider.runtimeType, "Slider");
  assert.equal(slider.accessibilityRole, "slider");
  assert.deepEqual(slider.runtimeProperties, {minimum: 0, maximum: 100, step: 1,
    railPath: "Assets/UI/rail.png", railHoverPath: "Assets/UI/rail-hover.png",
    knobPath: "Assets/UI/knob.png", railHeight: 18, knobSize: 20});
  assert.equal(slider.layout.sizeRule, "fixed");
});

test("Text font and letterSpacing lower to Label runtime properties", () => {
  const node = compiled([jsx(Text, {name: "Value", text: "100", position: [0, 0], size: [90, 32],
    fontSize: 19, font: "Assets/Fonts/Number.otf", letterSpacing: 4.5})]);
  const label = node("Value").runtimeProperties;
  assert.equal(label.fontPath, "Assets/Fonts/Number.otf");
  assert.equal(label.letterSpacing, 4.5);
  assert.equal("font" in label, false);
});
