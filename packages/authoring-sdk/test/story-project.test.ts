import assert from "node:assert/strict";
import test from "node:test";

import {compileStoryProject, StoryProjectCompilationCache, validateDocument} from "../src/index.js";
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

test("Story compilation cache reuses only identical inputs and refreshes the revision", () => {
  const cache = new StoryProjectCompilationCache();
  const sourceFiles = {
    "zh-TW/ch01/scene01.pxstory": "*start\nFirst line.\n[jump scene=scene02 target=start]\n",
    "zh-TW/ch01/scene02.pxstory": "*start\nSecond line.\n[end]\n*aside\n[end]\n",
  };
  const first = cache.compile({storyIndex, locale: "zh-TW", sourceFiles, committedRevision: 1});
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  const progress: Array<{phase: string; sceneId: string; cached?: boolean}> = [];
  const second = cache.compile({storyIndex: {...storyIndex}, locale: "zh-TW",
    sourceFiles: {...sourceFiles, "unrelated.pxstory": "changed"}, committedRevision: 2,
    onProgress: (event) => progress.push(event)});
  assert.equal(second.valid, true, JSON.stringify(second.diagnostics));
  assert.equal(second.runtimeIr?.committedRevision, 2);
  assert.equal(first.runtimeIr?.committedRevision, 1);
  assert.deepEqual(second.runtimeIr?.operations, first.runtimeIr?.operations);
  assert.equal(progress.length, storyIndex.scenes.length * 2);
  assert.ok(progress.every((event) => event.cached === true));

  progress.length = 0;
  const edited = cache.compile({storyIndex, locale: "zh-TW", committedRevision: 3,
    sourceFiles: {...sourceFiles, "zh-TW/ch01/scene01.pxstory":
      "*start\nEdited line.\n[jump scene=scene02 target=start]\n"},
    onProgress: (event) => progress.push(event)});
  assert.equal(edited.valid, true, JSON.stringify(edited.diagnostics));
  assert.ok(edited.runtimeIr?.operations.some((operation) => operation.text === "Edited line."));
  assert.deepEqual(progress.filter((event) => event.phase === "compile")
    .map((event) => [event.sceneId, event.cached === true]),
    [["scene01", false], ["scene02", true]]);
  assert.deepEqual(progress.filter((event) => event.phase === "parse")
    .map((event) => [event.sceneId, event.cached === true]),
    [["scene01", false], ["scene02", true]]);
  const uncachedEdited = compileStoryProject({storyIndex, locale: "zh-TW",
    committedRevision: 3, sourceFiles: {...sourceFiles,
      "zh-TW/ch01/scene01.pxstory":
        "*start\nEdited line.\n[jump scene=scene02 target=start]\n"}});
  assert.deepEqual(edited.runtimeIr, uncachedEdited.runtimeIr);
  assert.deepEqual(edited.sourceMap, uncachedEdited.sourceMap);
  assert.deepEqual(edited.resourceDependencies, uncachedEdited.resourceDependencies);

  progress.length = 0;
  const badSidecar = cache.compile({storyIndex, locale: "zh-TW", committedRevision: 4,
    sourceFiles: {...sourceFiles,
      "zh-TW/ch01/scene01.pxstory.pxids.json": "{}"},
    onProgress: (event) => progress.push(event)});
  assert.equal(badSidecar.valid, false);
  assert.ok(badSidecar.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1403"));
  assert.deepEqual(progress.filter((event) => event.phase === "compile")
    .map((event) => [event.sceneId, event.cached === true]),
    [["scene01", false], ["scene02", true]]);
  assert.deepEqual(progress.filter((event) => event.phase === "parse")
    .map((event) => [event.sceneId, event.cached === true]),
    [["scene01", false], ["scene02", true]]);

  progress.length = 0;
  const changedLabels = cache.compile({storyIndex, locale: "zh-TW",
    sourceFiles: {...sourceFiles, "zh-TW/ch01/scene02.pxstory":
      "*other\nSecond line.\n[end]\n*aside\n[end]\n"},
    onProgress: (event) => progress.push(event)});
  assert.equal(changedLabels.valid, false);
  assert.ok(changedLabels.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSTORY1305" || diagnostic.code === "PXSTORY1307"));
  assert.ok(progress.filter((event) => event.phase === "compile")
    .every((event) => event.cached !== true),
  "a changed cross-scene label set invalidates every dependent compile result");
  assert.ok(progress.filter((event) => event.phase === "parse")
    .every((event) => event.cached !== true),
  "scenes are reparsed when changed labels require recompilation");

  progress.length = 0;
  const changedInputs = cache.compile({storyIndex, locale: "zh-TW", sourceFiles,
    characters: [], onProgress: (event) => progress.push(event)});
  assert.equal(changedInputs.valid, true, JSON.stringify(changedInputs.diagnostics));
  assert.ok(progress.filter((event) => event.phase === "compile")
    .every((event) => event.cached !== true),
  "changed shared compiler inputs invalidate all scene results");
  assert.ok(progress.filter((event) => event.phase === "parse")
    .every((event) => event.cached !== true),
  "scenes are reparsed when changed inputs require recompilation");
});

test("Story cache compiles the source snapshot captured before progress callbacks", () => {
  const sourceFiles = {
    "zh-TW/ch01/scene01.pxstory": "*start\nFirst line.\n[end]\n",
    "zh-TW/ch01/scene02.pxstory": "*start\nSecond line.\n[end]\n*aside\n[end]\n",
  };
  const cache = new StoryProjectCompilationCache();
  const first = cache.compile({storyIndex, locale: "zh-TW", sourceFiles,
    onProgress: (event) => {
      if (event.phase === "parse" && event.sceneId === "scene01")
        sourceFiles["zh-TW/ch01/scene02.pxstory"] =
          "*start\nChanged during progress.\n[end]\n*aside\n[end]\n";
    }});
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  assert.ok(first.runtimeIr?.operations.some((operation) =>
    operation.text === "Second line."));
  assert.ok(!first.runtimeIr?.operations.some((operation) =>
    operation.text === "Changed during progress."));
  const second = cache.compile({storyIndex, locale: "zh-TW", sourceFiles});
  assert.equal(second.valid, true, JSON.stringify(second.diagnostics));
  assert.ok(second.runtimeIr?.operations.some((operation) =>
    operation.text === "Changed during progress."));
});

test("Story project compiler resolves cross-scene jump/call targets into one Runtime IR program", () => {
  const progress: Array<{phase: string; sceneId: string; current: number;
    total: number; sourcePath?: string}> = [];
  const compiled = compileStoryProject({
    storyIndex,
    locale: "zh-TW",
    committedRevision: 4,
    sourceFiles: {
      "zh-TW/ch01/scene01.pxstory": `*start\nOpening.\n[jump scene=scene02 target=start]\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nSecond scene.\n[call target=aside]\n[end]\n\n*aside\nAside.\n[return]\n`,
    },
    onProgress: (event) => progress.push(event),
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
  assert.deepEqual(progress.map(({phase, sceneId, current, total}) =>
    [phase, sceneId, current, total]), [
    ["parse", "scene01", 1, 2], ["parse", "scene02", 2, 2],
    ["compile", "scene01", 1, 2], ["compile", "scene02", 2, 2],
  ]);
  assert.equal(progress[0]?.sourcePath, "zh-TW/ch01/scene01.pxstory");
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

test("Story scene identityNamespace preserves explicit operation anchors after splitting a source", () => {
  const oldIndex: StoryIndexDocument = {
    format: "PrismatiXStoryIndex", schemaRevision: 2, id: "migration",
    entryScene: "main", chapters: [{id: "all", title: "All", scenes: ["main"]}],
    scenes: [{id: "main", sources: {"zh-TW": "old.pxstory"}}],
  };
  const newIndex: StoryIndexDocument = {
    ...oldIndex, entryScene: "opening",
    chapters: [{id: "opening", title: "Opening", scenes: ["opening"]}],
    scenes: [{id: "opening", identityNamespace: "migration:main",
      sources: {"zh-TW": "opening.pxstory"}}],
  };
  const source = "*start\n[id line-1]\nFirst line.\n[end]\n";
  const oldResult = compileStoryProject({storyIndex: oldIndex, locale: "zh-TW",
    sourceFiles: {"old.pxstory": source}});
  const newResult = compileStoryProject({storyIndex: newIndex, locale: "zh-TW",
    sourceFiles: {"opening.pxstory": source}});
  assert.equal(oldResult.valid, true, JSON.stringify(oldResult.diagnostics));
  assert.equal(newResult.valid, true, JSON.stringify(newResult.diagnostics));
  assert.equal(validateDocument("storyIndex", newIndex).valid, true);
  const anchor = (result: typeof oldResult) => result.runtimeIr!.operations
    .find((operation) => operation.text === "First line.");
  assert.equal(anchor(oldResult)?.operationId, anchor(newResult)?.operationId);
  assert.equal(anchor(oldResult)?.sourceId, anchor(newResult)?.sourceId);
  assert.equal(newResult.sourceMap!.mappings.find((mapping) =>
    mapping.operationId === anchor(newResult)?.operationId)?.sourceUri, "opening.pxstory");
});

test("shared Story identityNamespace rejects duplicate authored ids", () => {
  const index: StoryIndexDocument = {
    ...storyIndex,
    scenes: storyIndex.scenes.map((scene) => ({...scene, identityNamespace: "legacy:main"})),
  };
  const compiled = compileStoryProject({storyIndex: index, locale: "zh-TW", sourceFiles: {
    "zh-TW/ch01/scene01.pxstory": "*start\n[id duplicate]\nFirst.\n[end]\n",
    "zh-TW/ch01/scene02.pxstory": "*start\n[id duplicate]\nSecond.\n*aside\n[end]\n",
  }});
  assert.equal(compiled.valid, false);
  assert.ok(compiled.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1308"));
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
  const progress: string[] = [];
  const missingSource = compileStoryProject({
    storyIndex,
    locale: "ja-JP",
    sourceFiles: {},
    onProgress: (event) => progress.push(`${event.phase}:${event.current}/${event.total}`),
  });
  assert.equal(missingSource.valid, false);
  assert.ok(missingSource.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1302"));
  assert.ok(missingSource.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1306"));
  assert.deepEqual(progress, ["parse:1/2", "parse:2/2", "compile:1/2", "compile:2/2"]);

  const unresolved = compileStoryProject({
    storyIndex,
    locale: "zh-TW",
    sourceFiles: {
      "zh-TW/ch01/scene01.pxstory": `*start\n[jump scene=scene02 target=missing]\n`,
      "zh-TW/ch01/scene02.pxstory": `*start\nSecond scene.\n`,
    },
  });
  assert.equal(unresolved.valid, false);
  assert.ok(unresolved.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSTORY1305" &&
    diagnostic.span?.path === "zh-TW/ch01/scene01.pxstory" &&
    diagnostic.span.start.line === 2 && diagnostic.span.start.column === 1));
  assert.ok(unresolved.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1307" && diagnostic.details === "aside"));

  const missingScene = compileStoryProject({storyIndex, locale: "zh-TW", sourceFiles: {
    "zh-TW/ch01/scene01.pxstory": "*start\n[jump scene=absent]\n",
    "zh-TW/ch01/scene02.pxstory": "*start\n[end]\n*aside\n[end]\n",
  }});
  assert.ok(missingScene.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSTORY1304" &&
    diagnostic.span?.path === "zh-TW/ch01/scene01.pxstory" &&
    diagnostic.span.start.line === 2 && diagnostic.span.start.column === 1));
});
