import assert from "node:assert/strict";
import test from "node:test";

import {isProjectEdit} from "../src/dev-watch.js";

test("dev counts source edits and ignores build output", () => {
  assert.equal(isProjectEdit("Story/main.pxstory"), true);
  assert.equal(isProjectEdit("Content\\UI\\HUD.tsx"), true);
  assert.equal(isProjectEdit(Buffer.from("prismatix.json")), true);
  for (const ignored of [".prismatix\\dev\\1\\project.pxproject", ".git/index",
    "node_modules/x/index.js", "dist\\game.pxpak"])
    assert.equal(isProjectEdit(ignored), false, ignored);
});

test("dev ignores watcher events without a file name", () => {
  // Windows emits these for the project root itself; treating them as edits
  // rebuilt the Preview right after launch and skipped the title screen.
  assert.equal(isProjectEdit(null), false);
  assert.equal(isProjectEdit(undefined), false);
  assert.equal(isProjectEdit(""), false);
});
