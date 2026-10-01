import assert from "node:assert/strict";
import test from "node:test";

import {Control, Group, Image, Radio, Scene, Slider, Text, TextureButton, Toggle, compileJsxUi} from "../src/index.js";
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

test("clipping, show/hide transitions and Slider option lower to runtime properties", () => {
  const node = compiled([
    jsx(Group, {name: "Panel", fill: true, clipContent: true, linearTransition: true,
      showTransition: {seconds: 0.2, offset: [0, -50]}, hideTransition: {seconds: 0.15}}),
    jsx(Slider, {name: "TextSpeed", position: [0, 0], size: [400, 18], option: "mspeed"}),
  ]);
  assert.deepEqual(node("Panel").runtimeProperties, {clipContent: true,
    showTransitionSeconds: 0.2, showOffsetX: 0, showOffsetY: -50,
    hideTransitionSeconds: 0.15, linearTransition: true});
  assert.equal(node("TextSpeed").runtimeProperties.optionKey, "mspeed");
});

test("runtimeProperties with a typed prop, Control sliders and skin-shaped paths are linted", () => {
  const result = compileJsxUi(jsx(Scene, {name: "Lint", width: 640, height: 360,
    children: jsxs(Group, {name: "Root", fill: true, children: [
      jsx(Group, {name: "Clip", fill: true, runtimeProperties: {clipContent: true, custom: 1}}),
      jsx(TextureButton, {name: "Save", position: [0, 0], size: [96, 47],
        normalPath: "Assets/UI/bt_save-normal.png", hoverPath: "Assets/UI/bt_save-hover.png",
        runtimeProperties: {activateSoundPath: "Assets/se/ok.ogg"}}),
      jsx(TextureButton, {name: "Skinned", skin: "Assets/UI/bt_load", position: [0, 50], size: [96, 47]}),
    ]})}));
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.diagnostics.map(({severity, code, details}) => [severity, code, details]), [
    ["warning", "PXSDKJSX1100", "nodes[1].runtimeProperties.clipContent"],
    ["warning", "PXSDKJSX1102", "nodes[2].runtimeProperties.normalPath"],
    ["warning", "PXSDKJSX1100", "nodes[2].runtimeProperties.activateSoundPath"],
  ]);
  const control = compileJsxUi(jsx(Scene, {name: "Slider", width: 640, height: 360,
    children: jsx(Control, {name: "Volume", runtimeType: "Slider", position: [0, 0], size: [200, 20]})}));
  assert.deepEqual(control.diagnostics.map(({code}) => code), ["PXSDKJSX1101"]);
});

test("visibleWhen and enabledWhen bind without one bind object replacing another", () => {
  const node = compiled([jsx(TextureButton, {name: "Gallery", skin: "Assets/UI/bt_extra",
    position: [0, 0], size: [128, 48], action: "extra.open",
    visibleWhen: "title.visible", enabledWhen: "!profileFlags.locked"})]);
  assert.deepEqual(node("Gallery").bindings, {
    visible: {path: "title.visible"},
    enabled: {path: "profileFlags.locked", formatter: "Not"},
  });
  const both = compileJsxUi(jsx(Scene, {name: "Both", width: 64, height: 64,
    children: jsx(Text, {name: "Twice", text: "", visibleWhen: "a", bind: {visible: "b"}})}));
  assert.equal(both.valid, false);
  assert.match(both.diagnostics[0]?.details ?? "", /visibleWhen and a visible binding/u);
});

test("a TextureButton tooltip shows beside the button while hover.<name> is true", () => {
  const node = compiled([jsx(TextureButton, {name: "Save", skin: "Assets/UI/mw/bt_save",
    position: [780, 776], size: [96, 47], action: "save.open",
    tooltip: jsx(Text, {name: "SaveHelp", text: "Save", position: [-100, -45], size: [300, 40]})})]);
  const group = node("SaveGroup");
  assert.deepEqual([group.layout.x, group.layout.y, group.layout.width, group.layout.height],
    [780, 776, 96, 47]);
  assert.equal(group.runtimeProperties.mouseFilter, "Ignore");
  assert.equal(node("Save").parentId, group.id);
  assert.deepEqual(node("SaveTooltip").bindings, {visible: {path: "hover.Save"}});
  assert.equal(node("SaveHelp").parentId, node("SaveTooltip").id);
});

test("Toggle and Radio lower to state buttons bound to options or UI data", () => {
  const node = compiled([
    jsx(Toggle, {name: "DockLock", option: "dock", position: [1864, 783], size: [41, 38],
      on: "Assets/UI/mw/bt_lock", off: {base: "Assets/UI/mw/bt_unlock", states: ["hover"]}}),
    jsx(Toggle, {name: "MuteBGM", checked: "settings.muted.bgm", action: "set.mute.bgm",
      position: [0, 0], size: [80, 32], on: "Assets/UI/muted", off: "Assets/UI/unmuted"}),
    jsx(Radio, {name: "FontSerif", option: "font", value: 2, position: [900, 300], size: [34, 34],
      skin: "Assets/UI/radio", selected: "Assets/UI/radio-on"}),
  ]);
  assert.deepEqual([node("DockLock").layout.x, node("DockLock").layout.width], [1864, 41]);
  assert.deepEqual(node("DockLockOff").bindings, {visible: {path: "options.dock.is1", formatter: "Not"}});
  assert.deepEqual(node("DockLockOff").onClick, {id: "option.set", arguments: {argument: "dock=1"}});
  assert.deepEqual(node("DockLockOn").bindings, {visible: {path: "options.dock.is1"}});
  assert.deepEqual(node("DockLockOn").onClick, {id: "option.set", arguments: {argument: "dock=0"}});
  assert.equal(node("DockLockOn").accessibilityRole, "checkbox");
  assert.deepEqual(node("MuteBGMOn").onClick, {id: "set.mute.bgm", arguments: {}});
  assert.deepEqual(node("MuteBGMOff").bindings, {visible: {path: "settings.muted.bgm", formatter: "Not"}});
  assert.deepEqual(node("FontSerifButton").onClick, {id: "option.set", arguments: {argument: "font=2"}});
  assert.deepEqual(node("FontSerifSelected").bindings, {visible: {path: "options.font.is2"}});
  assert.equal(node("FontSerifSelected").onClick, undefined);
  assert.equal(node("FontSerifSelected").runtimeProperties.normalPath, "Assets/UI/radio-on-normal.png");
});

test("typed action builders produce the Player's action ids and argument keys", async () => {
  const {actions} = await import("../src/index.js");
  assert.deepEqual(actions.save.slot("$key"), {id: "save.slot", arguments: {slot: "$key"}});
  assert.deepEqual(actions.load.slot(3), {id: "load.slot", arguments: {slot: 3}});
  assert.deepEqual(actions.option.set("dock", 0), {id: "option.set", arguments: {argument: "dock=0"}});
  assert.deepEqual(actions.setting.volume("bgm", 80), {id: "set.bgm.value", arguments: {value: 80}});
  assert.deepEqual(actions.screen.close, {id: "overlay.close", arguments: {}});
  assert.throws(() => actions.option.set("bad key", 1), /option.set/u);
  assert.throws(() => actions.save.slot(-1), /slot/u);
  const node = compiled([jsx(TextureButton, {name: "Quick", skin: "Assets/UI/qsave",
    position: [0, 0], size: [96, 47], action: actions.save.quick})]);
  assert.deepEqual(node("Quick").onClick, {id: "save.quick", arguments: {}});
});

test("Image region selects an atlas cell", () => {
  const node = compiled([jsx(Image, {name: "VoicePortrait", path: "Assets/conf/char.png",
    region: [208, 192, 208, 96], position: [0, 0], size: [208, 96], scaleMode: "Original"})]);
  assert.deepEqual(node("VoicePortrait").runtimeProperties.region,
    {type: "rect", x: 208, y: 192, width: 208, height: 96});
  assert.throws(() => jsx(Image, {name: "Bad", path: "a.png", region: [0, 0, -1, 4]}), /non-negative/u);
});
