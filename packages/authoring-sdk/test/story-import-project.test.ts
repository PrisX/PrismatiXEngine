import assert from "node:assert/strict";
import test from "node:test";

import {assembleImportedStoryProject, compileStoryProject} from "../src/index.js";
import type {ImportedStoryProjectInput, ImportedStoryScene} from "../src/index.js";

function scene(id: string, lines: ImportedStoryScene["sources"][string]["lines"]): ImportedStoryScene {
  return {id, chapter: "chapter", identityNamespace: "imported:all",
    requiredLabels: ["start"], sources: {
      en: {path: `Story/en/${id}.pxstory`, lines},
      ja: {path: `Story/ja/${id}.pxstory`, lines},
    }};
}

const first = scene("first", [
  {source: "*start", id: "first.start"},
  {source: "Opening.", id: "first.line"},
  {source: "[jump scene=second target=start]", id: "first.jump"},
]);
const second = scene("second", [
  {source: "*start", id: "second.start"},
  {source: "Ending.", id: "second.line"},
  {source: "[end]", id: "second.end"},
]);
const input: ImportedStoryProjectInput = {id: "imported", entryScene: "first",
  locales: ["en", "ja"], chapters: [{id: "chapter", title: "Chapter"}],
  scenes: [first, second]};

test("format adapters can assemble aligned multi-scene Story inputs", () => {
  const result = assembleImportedStoryProject(input);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.value?.storyIndex.chapters[0]?.scenes, ["first", "second"]);
  assert.ok(result.value?.files["Story/en/first.pxstory.pxids.json"]);
  assert.ok(result.value?.files["Story/story.pxindex"]);
  for (const locale of input.locales) {
    const compiled = compileStoryProject({storyIndex: result.value!.storyIndex,
      locale, sourceFiles: result.value!.files});
    assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  }
});

test("import assembly fails closed on locale topology and path collisions", () => {
  assert.equal(assembleImportedStoryProject({...input, indexPath: "../outside.pxindex"})
    .diagnostics[0]?.code, "PXIMPORT1018");
  const missing = assembleImportedStoryProject({...input, scenes: [
    {...first, sources: {en: first.sources.en!}}, second,
  ]});
  assert.equal(missing.value, undefined);
  assert.ok(missing.diagnostics.some((item) => item.code === "PXIMPORT1014"));

  const differentIds = assembleImportedStoryProject({...input, scenes: [
    {...first, sources: {...first.sources, ja: {...first.sources.ja!, lines: [
      {source: "*start", id: "first.start"},
      {source: "はじめ。", id: "other.line"},
      {source: "[jump scene=second target=start]", id: "first.jump"},
    ]}}}, second,
  ]});
  assert.ok(differentIds.diagnostics.some((item) => item.code === "PXIMPORT1010"));

  const collision = assembleImportedStoryProject({...input, scenes: [first,
    {...second, sources: {...second.sources,
      ja: {path: "Story/EN/FIRST.pxstory", lines: second.sources.ja!.lines}}},
  ]});
  assert.ok(collision.diagnostics.some((item) => item.code === "PXIMPORT1016"));

  const duplicateId = assembleImportedStoryProject({...input, scenes: [first,
    {...second, sources: {
      en: {...second.sources.en!, lines: [
        {source: "*start", id: "first.start"}, {source: "[end]", id: "second.end"}]},
      ja: {...second.sources.ja!, lines: [
        {source: "*start", id: "first.start"}, {source: "[end]", id: "second.end"}]},
    }},
  ]});
  assert.ok(duplicateId.diagnostics.some((item) => item.code === "PXIMPORT1015"));
});
