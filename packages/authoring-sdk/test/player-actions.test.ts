import assert from "node:assert/strict";
import test from "node:test";

import {Button, Control, Scene, actions, compileJsxUi, playerActions,
  type UiAction} from "../src/index.js";
import {jsx} from "../src/jsx-runtime.js";

test("the SDK's Player action table matches the C++ contract", async (t) => {
  const {readFile} = await import("node:fs/promises");
  let header: string;
  try {
    header = await readFile(new URL(
      "../../../../PrismatiXEngine/Engine/UI/Actions/PlayerActionContract.h", import.meta.url), "utf8");
  } catch {
    t.skip("engine sources are not beside this package");
    return;
  }
  const types: Record<string, string> = {Integer: "integer", Boolean: "boolean", Text: "string"};
  const native: Record<string, unknown> = {};
  for (const match of header.matchAll(
    /detail::Plain\("([^"]+)", "([^"]+)"\)|detail::With\("([^"]+)", "([^"]+)", detail::(\w+), "([^"]+)"\)|\{"([^"]+)", (?:"([^"]*)"|\{\}), detail::(\w+), "([^"]+)", PlayerActionOwner::(\w+)\}/gu)) {
    if (match[1] !== undefined) native[match[1]] = {category: match[2]};
    else if (match[3] !== undefined)
      native[match[3]] = {argument: {key: match[4], type: types[match[5]!]}, category: match[6]};
    else native[match[7]!] = {
      ...(match[9] === "None" ? {} : {argument: {key: match[8], type: types[match[9]!]}}),
      category: match[10], owner: match[11]!.toLowerCase(),
    };
  }
  assert.ok(Object.keys(native).length > 100);
  assert.deepEqual(playerActions, native);
});

function compile(action: UiAction) {
  return compileJsxUi(jsx(Scene, {name: "Actions", width: 800, height: 600,
    children: jsx(Control, {name: "Root", children: jsx(Button, {name: "Go", action})})}),
  {kind: "scene"});
}

test("every typed action builder produces a valid Player action", () => {
  const built: UiAction[] = [];
  const walk = (value: unknown) => {
    if (typeof value === "object" && value !== null) {
      if ("id" in value) built.push(value as UiAction);
      else Object.values(value).forEach(walk);
    }
  };
  walk(actions);
  built.push(actions.screen.settingsPage(2), actions.screen.open("chapter3"),
    actions.save.slot(3), actions.load.slot(1), actions.save.page(2), actions.load.page(1),
    actions.save.delete(2), actions.save.copy(1), actions.save.move(4),
    actions.save.comment(5),
    actions.choice.select(0), actions.backlog.voice(4), actions.backlog.rollback(4),
    actions.backlog.scroll(-3), actions.gallery.view("cg01"), actions.replay.launch("start01"),
    actions.option.set("dock", 1), actions.setting.volume("bgm", 64),
    actions.setting.volumeUp("se"), actions.setting.volumeDown("voice"),
    actions.setting.toggleMute("master"), actions.setting.textSpeed(30),
    actions.setting.fullscreen(true), actions.setting.skipReadOnly(false),
    actions.setting.language("ja-JP"));
  assert.ok(built.length > 30);
  for (const action of built) {
    assert.ok(action.id in playerActions, action.id);
    const result = compile(action);
    assert.equal(result.valid, true, `${action.id}: ${JSON.stringify(result.diagnostics)}`);
  }
});

test("a misspelled Player action and a wrong argument are reported", () => {
  const typo = compile({id: "set.bgm.vaule", arguments: {value: 3}});
  assert.equal(typo.valid, false);
  assert.ok(typo.diagnostics.some((item) => item.code === "PXSDKUI1026" &&
    item.details?.includes("set.bgm.vaule")));
  const wrong = compile({id: "set.bgm.value", arguments: {volume: 3}});
  assert.ok(wrong.diagnostics.some((item) => item.code === "PXSDKUI1025" &&
    item.details?.includes("takes arguments {value: integer}")));
  // Ids outside the Player's namespaces belong to extensions.
  assert.equal(compile({id: "myGame.openMap", arguments: {}}).valid, true);
});
