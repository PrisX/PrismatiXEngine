import assert from "node:assert/strict";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {builtinScreenTsx, ejectBuiltinUi} from "../src/eject.js";

const layout = (x: number, y: number, width: number, height: number, fill = false) => ({
  mode: "free", x, y, width, height, anchorX: 0, anchorY: 0,
  anchorRight: fill ? 1 : 0, anchorBottom: fill ? 1 : 0, pivotX: 0, pivotY: 0, margin: 0,
  alignment: fill ? "fill" : "start", sizeRule: fill ? "fill" : "fixed"});
const appearance = {backgroundColor: "#00000000", textColor: "#FFFFFFFF", opacity: 1, disabledOpacity: 0.5};
const screen = {
  format: "PrismatiXUIScene", name: "PrismatiX Builtin Settings", width: 1280, height: 720,
  nodes: [
    {id: "r", parentId: null, order: 0, kind: "group", name: "SettingsRoot", visible: true, locked: false,
      layout: layout(0, 0, 1280, 720, true), appearance: {...appearance, backgroundColor: "#0B0E14F5"},
      runtimeProperties: {}, bindings: {}},
    {id: "a", parentId: "r", order: 4, kind: "label", runtimeType: "Label", name: "Heading", visible: true,
      locked: false, layout: layout(72, 40, 800, 48), text: "設定", appearance,
      runtimeProperties: {fontSize: 34}, bindings: {visible: {path: "settings.page1", formatter: "Not"}}},
    {id: "b", parentId: "r", order: 2, kind: "button", runtimeType: "CheckBox", name: "Fullscreen",
      visible: true, locked: false, layout: layout(96, 430, 350, 44), text: "全螢幕", appearance,
      runtimeProperties: {fontSize: 20}, bindings: {}},
    {id: "c", parentId: "r", order: 9, kind: "button", runtimeType: "Button", name: "Close", visible: true,
      locked: false, layout: layout(1088, 644, 120, 42), text: "完成", appearance,
      onClick: {id: "overlay.close", arguments: {}}, runtimeProperties: {focusMode: "All", fontSize: 18},
      bindings: {}},
  ],
};

test("a built-in screen becomes scaled TSX that keeps names, order and bindings", () => {
  const tsx = builtinScreenTsx("settings", screen, 1920, 1080);
  assert.match(tsx, /^import \{Button, Control, Group, Scene, Text\} from "@prismatix\/authoring-sdk";/u);
  assert.match(tsx, /const width = 1920;\nconst height = 1080;/u);
  // Children follow the built-in sibling order: CheckBox (2), Label (4), Button (9).
  const order = ["Fullscreen", "Heading", "Close"].map((name) => tsx.indexOf(`name="${name}"`));
  assert.deepEqual([...order].sort((left, right) => left - right), order);
  assert.match(tsx, /<Control name="Fullscreen"\s+kind="button"\s+runtimeType="CheckBox"/u);
  assert.match(tsx, /runtimeProperties=\{\{fontSize: sf\(20\)\}\}/u);
  assert.match(tsx, /<Text name="Heading"[^>]*fontSize=\{sf\(34\)\}[^>]*bind=\{\{"visible": "!settings.page1"\}\}/su);
  assert.match(tsx, /<Button name="Close"[^>]*text="完成"[^>]*action="overlay.close"[^>]*focusMode="All"/su);
  assert.match(tsx, /position=\{\[sx\(1088\), sy\(644\)\]\}/u);
});

test("eject writes Content/UI and registers the route, refusing to overwrite without --force", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-eject-"));
  const builtin = join(root, "builtin");
  try {
    await mkdir(builtin);
    await writeFile(join(builtin, "settings.pxui"), JSON.stringify(screen));
    await writeFile(join(root, "prismatix.json"), JSON.stringify({name: "Game", id: "game",
      resolution: {width: 1280, height: 720}, uiEntryPoints: {title: "Content/UI/Title.tsx"}}));
    assert.deepEqual(await ejectBuiltinUi(root, ["settings"], builtin, false),
      {created: ["Content/UI/Settings.tsx"], registered: ["settings"]});
    const manifest = JSON.parse(await readFile(join(root, "prismatix.json"), "utf8")) as
      {uiEntryPoints: Record<string, string>};
    assert.deepEqual(manifest.uiEntryPoints, {title: "Content/UI/Title.tsx", settings: "Content/UI/Settings.tsx"});
    await assert.rejects(ejectBuiltinUi(root, ["settings"], builtin, false), /--force/u);
    assert.deepEqual(await ejectBuiltinUi(root, ["settings"], builtin, true),
      {created: ["Content/UI/Settings.tsx"], registered: []});
    await assert.rejects(ejectBuiltinUi(root, ["credits"], builtin, false), /No built-in screen named credits/u);
  } finally { await rm(root, {recursive: true, force: true}); }
});
