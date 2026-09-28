import assert from "node:assert/strict";
import test from "node:test";

import {Group, ItemsGrid, Scene, Slider, Text, TextureButton, compileJsxUi,
  validatePlayerUiContract} from "../src/index.js";
import {jsx, jsxs, type UiElement} from "../src/jsx-runtime.js";

function document(children: readonly UiElement[]) {
  const result = compileJsxUi(jsx(Scene, {name: "Route", width: 640, height: 360,
    children: jsxs(Group, {name: "Root", fill: true, children})}));
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  return result.value!;
}

const settings = {"player.saveLayout": {pages: 3, slotsPerPage: 8, firstSlot: 1}};

test("reserved Player control names must keep the type the Player casts to", () => {
  const config = document([
    jsx(Slider, {name: "Master", position: [0, 0], size: [200, 20]}),
    jsx(Text, {name: "BGM", text: "", position: [0, 30], size: [200, 20]}),
  ]);
  const diagnostics = validatePlayerUiContract({uiEntryPoints: {settings: "Content/UI/Config.tsx"}},
    new Map([["Content/UI/Config.pxui", config as never]]));
  assert.deepEqual(diagnostics.map(({code, details}) => [code, details?.split(":")[1]]),
    [["PXBUILD1101", "BGM"]]);
  // The same name is unrestricted on routes where the Player never looks it up.
  assert.deepEqual(validatePlayerUiContract({uiEntryPoints: {extra: "Content/UI/Config.tsx"}},
    new Map([["Content/UI/Config.pxui", config as never]])), []);
});

test("bindings under Player-owned roots must name published data", () => {
  const save = document([
    jsx(TextureButton, {name: "Page3", skin: "Assets/page", position: [0, 0], size: [40, 40],
      action: {id: "saveLoad.page", arguments: {argument: "3"}}, bind: {enabled: "!saveLoad.page3"}}),
    jsx(TextureButton, {name: "Page4", skin: "Assets/page", position: [50, 0], size: [40, 40],
      action: {id: "saveLoad.page", arguments: {argument: "4"}}, bind: {enabled: "!saveLoad.page4"}}),
    jsx(Text, {name: "Typo", text: "", position: [0, 50], size: [100, 20],
      bind: {visible: "saveLoad.confirm.visable"}}),
    jsx(Text, {name: "Mine", text: "", position: [0, 80], size: [100, 20],
      bind: {text: "settings.custom.label"}}),
    jsx(ItemsGrid, {name: "Slots", position: [0, 100], size: [400, 200],
      items: {path: "saveLoad.slots", key: "key"}, columns: 2, itemExtent: [200, 50],
      children: jsx(Group, {name: "Cell", size: [200, 50], children: [
        jsx(Text, {name: "SlotTitle", text: "", position: [0, 0], size: [100, 20],
          bind: {text: "item.title"}}),
        jsx(Text, {name: "SlotDate", text: "", position: [0, 20], size: [100, 20],
          bind: {text: "item.subtitel"}}),
      ]})}),
  ]);
  const diagnostics = validatePlayerUiContract({uiEntryPoints: {save: "Content/UI/Save.tsx"},
    settings: {...settings, "player.uiData": {"settings.custom": {label: ""}}}},
  new Map([["Content/UI/Save.pxui", save as never]]));
  assert.deepEqual(diagnostics.map(({code, message}) => [code, message]), [
    ["PXBUILD1100", "The Player publishes no UI data at saveLoad.page4; did you mean saveLoad.page1?"],
    ["PXBUILD1100", "The Player publishes no UI data at saveLoad.confirm.visable; did you mean saveLoad.confirm.visible?"],
    ["PXBUILD1102", "saveLoad.slots items have no field subtitel; did you mean subtitle?"],
  ]);
});
