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

test("dialog kinds and voice profiles come from the project's settings", () => {
  const hud = document([
    jsx(Text, {name: "Dialogue", text: "", position: [0, 120], size: [200, 20]}),
    jsx(Group, {name: "ConfirmDialog", position: [0, 0], size: [200, 100], children: [
      jsx(Text, {name: "AskSave", text: "", position: [0, 0], size: [100, 20],
        bind: {visible: "dialog.kind.qsave"}}),
      jsx(Text, {name: "AskExit", text: "", position: [0, 30], size: [100, 20],
        bind: {visible: "dialog.kind.exit"}}),
      jsx(Text, {name: "Muted", text: "", position: [0, 60], size: [100, 20],
        bind: {visible: "settings.profileMuted.hikari"}}),
    ]}),
  ]);
  const check = (projectSettings: Record<string, never> | object) => validatePlayerUiContract(
    {uiEntryPoints: {hud: "Content/UI/HUD.tsx"}, settings: projectSettings as never},
    new Map([["Content/UI/HUD.pxui", hud as never]]))
    .map(({code, message}) => [code, message.split(";")[0]]);
  assert.deepEqual(check({
    "player.confirmations": {quickSave: {kind: "qsave"}, exit: {}, typo: {kind: "ignored"}},
    "player.voiceProfiles": {ids: ["hikari"]},
  }), []);
  // Without declarations the dialog never opens and nothing is published.
  assert.deepEqual(check({}), [
    ["PXBUILD1103", "ConfirmDialog is never shown because player.confirmations lists no operation"],
    ["PXBUILD1100", "The Player publishes no UI data at dialog.kind.qsave"],
    ["PXBUILD1100", "The Player publishes no UI data at dialog.kind.exit"],
    ["PXBUILD1100", "The Player publishes no UI data at settings.profileMuted.hikari"],
  ]);
});

test("duplicate control names are reported, as errors when the Player looks the name up", () => {
  const hud = document([
    jsx(Text, {name: "Dialogue", text: "", position: [0, 0], size: [200, 20]}),
    jsx(Text, {name: "Dialogue", text: "", position: [0, 30], size: [200, 20]}),
    jsx(Text, {name: "DockLock", text: "", position: [0, 60], size: [40, 20]}),
    jsx(Text, {name: "DockLock", text: "", position: [0, 90], size: [40, 20]}),
  ]);
  const diagnostics = validatePlayerUiContract({uiEntryPoints: {hud: "Content/UI/HUD.tsx"}},
    new Map([["Content/UI/HUD.pxui", hud as never]]));
  assert.deepEqual(diagnostics.map(({severity, code, details}) => [severity, code, details]), [
    ["error", "PXBUILD1106", "nodes[2].name:Dialogue"],
    ["warning", "PXBUILD1107", "nodes[4].name:DockLock"],
  ]);
});

test("a HUD without a Dialogue control is reported", () => {
  const hud = document([jsx(Text, {name: "Body", text: "", position: [0, 0], size: [200, 20]})]);
  const diagnostics = validatePlayerUiContract({uiEntryPoints: {hud: "Content/UI/HUD.tsx"}},
    new Map([["Content/UI/HUD.pxui", hud as never]]));
  assert.deepEqual(diagnostics.map(({code}) => code), ["PXBUILD1108"]);
  assert.deepEqual(validatePlayerUiContract({uiEntryPoints: {extra: "Content/UI/HUD.tsx"}},
    new Map([["Content/UI/HUD.pxui", hud as never]])), []);
});

test("the contract names every control the C++ Player looks up by a literal name", async (t) => {
  const {readFile} = await import("node:fs/promises");
  const {playerLookupNames} = await import("../src/index.js");
  const root = new URL("../../../../PrismatiXEngine/", import.meta.url);
  const sources: string[] = [];
  for (const file of ["Engine/UI/GalgameUI.cpp", "Applications/Player/PlayerApp.cpp"]) {
    try { sources.push(await readFile(new URL(file, root), "utf8")); }
    catch { t.skip("engine sources are not beside this package"); return; }
  }
  const {names, prefixes} = playerLookupNames();
  const looked = new Set<string>();
  for (const source of sources)
    for (const match of source.matchAll(/Named(?:<[^>]*>)?\s*\([^;"]*?"([A-Za-z_]+)"/gu))
      looked.add(match[1]!);
  const missing = [...looked].filter((name) =>
    !names.has(name) && !prefixes.some((prefix) => name.startsWith(prefix)));
  assert.deepEqual(missing, []);
});

test("textKey binds locale strings, and an unknown key is reported", () => {
  const title = document([
    jsx(Text, {name: "Dialogue", text: "", position: [0, 0], size: [200, 20]}),
    jsx(Text, {name: "Error", text: "Cannot load", textKey: "save.incompatible",
      position: [0, 30], size: [200, 20]}),
    jsx(Text, {name: "Typo", text: "", textKey: "save.incompatable", position: [0, 60], size: [200, 20]}),
  ]);
  const error = title.nodes.find((node) => node.name === "Error")!;
  assert.deepEqual(error.bindings, {text: {path: "strings.save.incompatible"}});
  assert.equal(error.text, "Cannot load");
  const diagnostics = validatePlayerUiContract({uiEntryPoints: {hud: "Content/UI/HUD.tsx"},
    localeStrings: ["save.incompatible"]}, new Map([["Content/UI/HUD.pxui", title as never]]));
  assert.deepEqual(diagnostics.map(({code, message}) => [code, message]), [
    ["PXBUILD1100", "The Player publishes no UI data at strings.save.incompatable; did you mean strings.save.incompatible?"],
  ]);
});

test("save management, settings preview and the SlotComment field are published data", () => {
  const save = document([
    jsx(Text, {name: "Picking", text: "", position: [0, 0], size: [100, 20],
      bind: {visible: "saveLoad.pending.visible", text: "saveLoad.pending.slot"}}),
    jsx(Text, {name: "Editing", text: "", position: [0, 30], size: [100, 20],
      bind: {visible: "saveLoad.comment.visible", text: "saveLoad.slot3.comment"}}),
    jsx(Text, {name: "Sample", text: "", position: [0, 300], size: [100, 20],
      bind: {visible: "settings.previewWaiting", text: "settings.preview"}}),
    jsx(ItemsGrid, {name: "Slots", position: [0, 60], size: [400, 200],
      items: {path: "saveLoad.slots", key: "key"}, columns: 2, itemExtent: [200, 50],
      children: jsx(Group, {name: "Cell", size: [200, 50], children: [
        jsx(Text, {name: "Note", text: "", position: [0, 0], size: [200, 20],
          bind: {text: "item.comment", visible: "!item.pending"}})]})}),
    jsx(Text, {name: "SlotComment", text: "", position: [0, 270], size: [100, 20]}),
  ]);
  const diagnostics = validatePlayerUiContract({uiEntryPoints: {save: "Content/UI/Save.tsx"},
    settings: {"player.confirmations": {deleteSave: {kind: "erase"}, moveSave: {}}}},
  new Map([["Content/UI/Save.pxui", save as never]]));
  assert.deepEqual(diagnostics.map(({code, details}) => [code, details?.split(":")[1]]),
    [["PXBUILD1101", "SlotComment"]]);
});
