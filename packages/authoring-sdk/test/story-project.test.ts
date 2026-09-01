import assert from "node:assert/strict";
import test from "node:test";

import {compileStoryProject, validateDocument} from "../src/index.js";
import type {StoryIndexDocument} from "../src/index.js";

const storyIndex: StoryIndexDocument = {
  format: "PrismatiXStoryIndex",
  schemaRevision: 2,
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
  assert.equal(operations[0]?.arguments.target, "pxscene.entry.3ddd0e2578e95273");
  assert.ok(operations.some((operation) => operation.kind === "label" && operation.arguments.target === "pxscene.entry.3ddd0e2578e95273"));
  assert.ok(operations.some((operation) => operation.kind === "label" && operation.arguments.target === "pxscene.label.7fefb12f027437c6"));
  assert.ok(operations.some((operation) => operation.kind === "jump" && operation.arguments.target === "pxscene.label.7fefb12f027437c6"));
  assert.ok(operations.some((operation) => operation.kind === "callFragment" && operation.arguments.target === "pxscene.label.f379e5d879ebe15e"));

  const sourceUris = new Set(compiled.sourceMap!.mappings.map((mapping) => mapping.sourceUri));
  assert.ok(sourceUris.has("Story/story.pxindex"));
  assert.ok(sourceUris.has("zh-TW/ch01/scene01.pxstory"));
  assert.ok(sourceUris.has("zh-TW/ch01/scene02.pxstory"));
});

test("Story project compiler supports jumping directly to another scene entry", () => {
  const compiled = compileStoryProject({
    storyIndex: {
      ...storyIndex,
      scenes: storyIndex.scenes.map((scene) =>
        scene.id === "scene02"
          ? {...scene, requiredLabels: ["start", "aside"]}
          : scene,
      ),
    },
    locale: "zh-TW",
    sourceFiles: {
      "zh-TW/ch01/scene01.pxstory": `*start\n[jump scene=scene02]\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nArrived.\n*aside\n[return]\n`,
    },
  });

  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.ok(compiled.runtimeIr!.operations.some((operation) => operation.kind === "jump" && operation.arguments.target === "pxscene.entry.3ddd0f2578e95426"));
});

test("Story project compiler rejects duplicate scene ids even when the first descriptor cannot be loaded", () => {
  const duplicate = compileStoryProject({
    storyIndex: {
      ...storyIndex,
      entryScene: "scene02",
      scenes: [
        {id: "scene01", sources: {"ja-JP": "ja-JP/missing.pxstory"}},
        {id: "scene01", sources: {"zh-TW": "zh-TW/duplicate.pxstory"}},
        storyIndex.scenes[1]!,
      ],
    },
    locale: "zh-TW",
    sourceFiles: {
      "zh-TW/duplicate.pxstory": `*start\nDuplicate.\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nSecond scene.\n*aside\n[return]\n`,
    },
  });

  assert.equal(duplicate.valid, false);
  assert.ok(duplicate.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1301" && diagnostic.details === "scene01"));
  assert.ok(duplicate.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1302" && diagnostic.details === "scene01"));
});

test("Story project compiler hashes author-controlled scene and label ids into internal targets", () => {
  const compiled = compileStoryProject({
    storyIndex: {
      format: "PrismatiXStoryIndex",
      schemaRevision: 2,
      id: "collision-story",
      entryScene: "chapter.one",
      chapters: [{id: "chapter", title: "Chapter", scenes: ["chapter.one"]}],
      scenes: [{id: "chapter.one", sources: {"zh-TW": "zh-TW/chapter.one.pxstory"}}],
    },
    locale: "zh-TW",
    sourceFiles: {
      "zh-TW/chapter.one.pxstory": `*label.with.dots\n[jump target=label.with.dots]\n`,
    },
  });

  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  for (const operation of compiled.runtimeIr!.operations) {
    const target = operation.arguments.target;
    if (typeof target === "string" && target.startsWith("pxscene.")) {
      assert.equal(target.includes("chapter.one"), false);
      assert.equal(target.includes("label.with.dots"), false);
    }
  }
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
