import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, compileStoryProject, externalizeStoryIds,
  synchronizeStoryIdentityMap,
  type StoryIdentityMap, type StoryIndexDocument} from "../src/index.js";

const sourcePath = "Story/en/main.pxstory";
const mapPath = `${sourcePath}.pxids.json`;
const storyIndex: StoryIndexDocument = {
  format: "PrismatiXStoryIndex", schemaRevision: 2, id: "story", entryScene: "main",
  chapters: [{id: "chapter", title: "Chapter", scenes: ["main"]}],
  scenes: [{id: "main", identityNamespace: "legacy:main",
    sources: {en: sourcePath}}],
};

function compile(source: string, map?: StoryIdentityMap) {
  return compileStoryProject({storyIndex, locale: "en", sourceFiles: {
    [sourcePath]: source,
    ...(map === undefined ? {} : {[mapPath]: JSON.stringify(map)}),
  }});
}

test("external identity maps preserve inline operation ids without cluttering Story", () => {
  const original = "*start\n[id legacy.greeting]\nHello.\n[id legacy.end]\n[end]\n";
  const before = compile(original);
  const converted = externalizeStoryIds(original, {path: sourcePath});
  assert.equal(converted.valid, true, JSON.stringify(converted.diagnostics));
  assert.equal(converted.assigned, 1); // The label had no inline id.
  assert.equal(converted.source, "; @prismatix-ids\n*start\nHello.\n[end]\n");
  assert.deepEqual(converted.map!.entries.map(entry => entry.id).slice(1),
    ["legacy.greeting", "legacy.end"]);
  const after = compile(converted.source!, converted.map);
  assert.equal(before.valid, true, JSON.stringify(before.diagnostics));
  assert.equal(after.valid, true, JSON.stringify(after.diagnostics));
  const identities = (result: typeof before) => result.runtimeIr!.operations
    .map(operation => [operation.sourceId, operation.operationId]);
  assert.deepEqual(identities(after), identities(before));
  assert.equal(after.sourceMap!.mappings.find(mapping =>
    mapping.operationId === after.runtimeIr!.operations.find(operation =>
      operation.text === "Hello.")?.operationId)?.startLine, 3);
});

test("external ids survive prose edits and non-executable inserted lines", () => {
  const converted = externalizeStoryIds("*start\nHello.\n[end]\n", {path: sourcePath});
  const changed = "; @prismatix-ids\n; new comment\n*start\n\nGood morning.\n[end]\n";
  const before = compile(converted.source!, converted.map);
  const after = compile(changed, converted.map);
  assert.equal(after.valid, true, JSON.stringify(after.diagnostics));
  assert.equal(after.runtimeIr!.operations.find(operation => operation.kind === "narration")?.operationId,
    before.runtimeIr!.operations.find(operation => operation.kind === "narration")?.operationId);
});

test("external ids fail closed for inserted or reordered executable operations", () => {
  const converted = externalizeStoryIds("*start\nFirst.\nSecond.\n[end]\n", {path: sourcePath});
  const added = compile("*start\nFirst.\nNew.\nSecond.\n[end]\n", converted.map);
  assert.equal(added.valid, false);
  assert.ok(added.diagnostics.some(diagnostic => diagnostic.code === "PXSTORY1405"));
  const moved = compile("*start\nSecond.\nFirst.\n[end]\n", converted.map);
  assert.equal(moved.valid, false);
  assert.ok(moved.diagnostics.some(diagnostic => diagnostic.code === "PXSTORY1409"));
});

test("sidecar sync preserves old operation ids across distinct insertions and edits", () => {
  const converted = externalizeStoryIds("*start\nFirst.\nSecond.\n[end]\n", {path: sourcePath});
  const before = compile(converted.source!, converted.map);
  const changed = "; @prismatix-ids\n*start\nFirst revised.\nSecond.\nNew line.\n[end]\n";
  const synced = synchronizeStoryIdentityMap(changed, converted.map,
    {path: sourcePath});
  assert.equal(synced.valid, true, JSON.stringify(synced.diagnostics));
  assert.equal(synced.preserved, 4);
  assert.equal(synced.added, 1);
  assert.deepEqual(synced.removed, []);
  const after = compile(changed, synced.map);
  assert.equal(after.valid, true, JSON.stringify(after.diagnostics));
  const ids = (result: typeof before) => result.runtimeIr!.operations.map(operation => operation.operationId);
  assert.deepEqual(ids(after).filter(id => ids(before).includes(id)), ids(before));
  assert.equal(synchronizeStoryIdentityMap(changed, synced.map, {path: sourcePath}).added, 0);
});

test("sidecar sync reports deletions for save migration review", () => {
  const converted = externalizeStoryIds("*start\nFirst.\nSecond.\n[end]\n", {path: sourcePath});
  const changed = "; @prismatix-ids\n*start\nFirst.\n[end]\n";
  const synced = synchronizeStoryIdentityMap(changed, converted.map, {path: sourcePath});
  assert.equal(synced.valid, true, JSON.stringify(synced.diagnostics));
  assert.equal(synced.preserved, 3);
  assert.equal(synced.added, 0);
  assert.deepEqual(synced.removed, [`source:${converted.map!.entries[2]!.sourceId}`]);
  assert.equal(compile(changed, synced.map).valid, true);
});

test("sidecar sync rejects reordered, duplicated, or overlapping ambiguous edits", () => {
  const converted = externalizeStoryIds("*start\nFirst.\nSecond.\n[end]\n", {path: sourcePath});
  for (const [changed, code] of [
    ["; @prismatix-ids\n*start\nSecond.\nFirst.\n[end]\n", "PXSTORY1413"],
    ["; @prismatix-ids\n*start\nFirst.\nFirst.\nSecond.\n[end]\n", "PXSTORY1416"],
    ["; @prismatix-ids\n*start\nChanged.\nInserted.\nSecond.\n[end]\n", "PXSTORY1414"],
  ] as const) {
    const result = synchronizeStoryIdentityMap(changed, converted.map, {path: sourcePath});
    assert.equal(result.valid, false, changed);
    assert.ok(result.diagnostics.some(diagnostic => diagnostic.code === code),
      JSON.stringify(result.diagnostics));
  }
});

test("declared external identities fail closed when the sidecar is missing", () => {
  const converted = externalizeStoryIds("*start\nHello.\n[end]\n", {path: sourcePath});
  const missing = compile(converted.source!);
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some(diagnostic => diagnostic.code === "PXSTORY1411"));
});

test("project compiler uses external ids but does not distribute the sidecar", () => {
  const source = "*start\nHello.\n[end]\n";
  const converted = externalizeStoryIds(source, {path: "Story/main.pxstory"});
  const result = compileProject({sourceFiles: {
    "Story/main.pxstory": source,
    "Story/main.pxstory.pxids.json": JSON.stringify(converted.map),
  }});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.artifact!.files["Story/main.pxstory.pxids.json"], undefined);
  assert.ok(result.artifact!.dependencies.every(dependency =>
    dependency.logicalPath !== "Story/main.pxstory.pxids.json"));
  assert.ok(result.artifact!.files["Runtime/main.pxir"]);
});
