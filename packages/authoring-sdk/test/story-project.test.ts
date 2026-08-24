import assert from "node:assert/strict";
import test from "node:test";

import {compileStoryProject, validateDocument} from "../src/index.js";
import type {StoryIndexDocument} from "../src/index.js";

const storyIndex: StoryIndexDocument = {
  format: "PrismatiXStoryIndex",
  schemaRevision: 1,
  id: "main-story",
  entryScene: "scene01",
  chapters: [{id: "chapter01", title: "Chapter 1", scenes: ["scene01", "scene02"]}],
  scenes: [
    {id: "scene01", sources: {"zh-TW": "zh-TW/ch01/scene01.pxstory"}, requiredLabels: ["start"]},
    {id: "scene02", sources: {"zh-TW": "zh-TW/ch01/scene02.pxstory"}, requiredLabels: ["start", "aside"]},
  ],
};

test("Story project compiler resolves cross-scene jump/call targets into one Runtime IR program", () => {
  const compiled = compileStoryProject({
    storyIndex,
    locale: "zh-TW",
    committedRevision: 4,
    sourceFiles: {
      "zh-TW/ch01/scene01.pxstory": `*start\nOpening.\n[jump scene=scene02 target=start]\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nSecond scene.\n[call target=aside]\n[end]\n\n*aside\nAside.\n[return]\n`,
    },
  });

  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.ok(compiled.runtimeIr);
  assert.ok(compiled.sourceMap);
  assert.equal(validateDocument("runtimeIr", compiled.runtimeIr).valid, true);
  assert.equal(validateDocument("sourceMap", compiled.sourceMap).valid, true);
  assert.equal(compiled.runtimeIr!.documentId, "main-story");
  assert.equal(compiled.runtimeIr!.committedRevision, 4);

  const operations = compiled.runtimeIr!.operations;
  assert.equal(operations[0]?.kind, "jump");
  assert.equal(operations[0]?.arguments.target, "pxscene.scene01.entry");
  assert.ok(operations.some((operation) => operation.kind === "label" && operation.arguments.target === "pxscene.scene01.entry"));
  assert.ok(operations.some((operation) => operation.kind === "label" && operation.arguments.target === "pxscene.scene02.label.start"));
  assert.ok(operations.some((operation) => operation.kind === "jump" && operation.arguments.target === "pxscene.scene02.label.start"));
  assert.ok(operations.some((operation) => operation.kind === "callFragment" && operation.arguments.target === "pxscene.scene02.label.aside"));

  const sourceUris = new Set(compiled.sourceMap!.mappings.map((mapping) => mapping.sourceUri));
  assert.ok(sourceUris.has("Story/story.pxindex"));
  assert.ok(sourceUris.has("zh-TW/ch01/scene01.pxstory"));
  assert.ok(sourceUris.has("zh-TW/ch01/scene02.pxstory"));
});

test("Story project compiler supports jumping directly to another scene entry", () => {
  const compiled = compileStoryProject({
    storyIndex: {
      ...storyIndex,
      scenes: storyIndex.scenes.map((scene) => ({...scene, requiredLabels: scene.id === "scene02" ? ["start", "aside"] : scene.requiredLabels})),
    },
    locale: "zh-TW",
    sourceFiles: {
      "zh-TW/ch01/scene01.pxstory": `*start\n[jump scene=scene02]\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nArrived.\n*aside\n[return]\n`,
    },
  });

  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.ok(compiled.runtimeIr!.operations.some((operation) => operation.kind === "jump" && operation.arguments.target === "pxscene.scene02.entry"));
});

test("Story project compiler fails closed for missing locale sources, scenes, labels, and required labels", () => {
  const missingSource = compileStoryProject({
    storyIndex,
    locale: "ja-JP",
    sourceFiles: {},
  });
  assert.equal(missingSource.valid, false);
  assert.ok(missingSource.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1302"));
  assert.ok(missingSource.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1306"));

  const unresolved = compileStoryProject({
    storyIndex,
    locale: "zh-TW",
    sourceFiles: {
      "zh-TW/ch01/scene01.pxstory": `*start\n[jump scene=scene02 target=missing]\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nSecond scene.\n`,
    },
  });
  assert.equal(unresolved.valid, false);
  assert.ok(unresolved.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1305"));
  assert.ok(unresolved.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1307" && diagnostic.details === "aside"));
});
