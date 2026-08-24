import assert from "node:assert/strict";
import test from "node:test";

import {parseStory, validateStoryLocalizationStructure} from "../src/index.js";

test("localized Stories may reflow dialogue while preserving control structure", () => {
  const reference = parseStory(`*start\n@yuki\n早安。\n[jump target=end]\n*end\n`, "Story/zh-TW/scene.pxstory");
  const localized = parseStory(`*start\n@yuki\nGood morning.\nHow are you?\n[jump target=end]\n*end\n`, "Story/en-US/scene.pxstory");
  assert.deepEqual(validateStoryLocalizationStructure(reference, localized), []);
});

test("localized Stories reject missing labels and changed branch targets", () => {
  const reference = parseStory(`*start\n[choice id=greeting text="A" goto=end]\n[choice.wait]\n*end\n`, "Story/zh-TW/scene.pxstory");
  const localized = parseStory(`*start\n[choice id=greeting text="A" goto=other]\n[choice.wait]\n*other\n`, "Story/en-US/scene.pxstory");
  const diagnostics = validateStoryLocalizationStructure(reference, localized);
  assert.ok(diagnostics.some((item) => item.code === "PXLOC1002"));
});
