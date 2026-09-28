import assert from "node:assert/strict";
import test from "node:test";

import {buildImportedStorySource, compileStoryProject, parseStory,
  serializeStoryCommand} from "../src/index.js";

test("imported Story command round-trips quoted and structured values", () => {
  const values = {
    text: '她說「a=b ]」 and "yes"',
    asset: "Assets/UI/a b.png",
    payload: {items: [1, true, null], label: "日本語"},
    speed: 1.25,
  };
  const serialized = serializeStoryCommand("stage.compose", values);
  assert.equal(serialized.valid, true, JSON.stringify(serialized.diagnostics));
  const parsed = parseStory(`${serialized.value}\n`, "Story/imported.pxstory");
  assert.equal(parsed.diagnostics.length, 0);
  assert.equal(parsed.nodes[0]?.name, "stage.compose");
  assert.deepEqual(Object.fromEntries(parsed.nodes[0]?.arguments?.map((argument) =>
    [argument.name, argument.value]) ?? []), values);
  const special = JSON.parse('{"payload":{"__proto__":{"name":"kept"}}}') as
    {payload: {"__proto__": {name: string}}};
  const encoded = serializeStoryCommand("stage.compose", special);
  assert.equal(encoded.valid, true);
  assert.deepEqual(parseStory(encoded.value!).nodes[0]?.arguments?.[0]?.value,
    special.payload);
});

test("imported Story command rejects names and values the parser cannot preserve", () => {
  assert.equal(serializeStoryCommand("if", {value: true}).diagnostics[0]?.code,
    "PXIMPORT1002");
  assert.equal(serializeStoryCommand("bad command").valid, false);
  assert.equal(serializeStoryCommand("show", {"bad key": "x"}).diagnostics[0]?.code,
    "PXIMPORT1003");
  assert.equal(serializeStoryCommand("wait", {duration: Number.POSITIVE_INFINITY})
    .diagnostics[0]?.code, "PXIMPORT1004");
  assert.equal(serializeStoryCommand("show", null as never).diagnostics[0]?.code,
    "PXIMPORT1004");
});

test("imported Story lines produce a sidecar with stable save identities", () => {
  const lines = [
    {source: "; external chapter title"},
    {source: "*start", id: "original.start"},
    {source: "Opening.", id: "original.opening"},
    {source: "[end]", id: "original.end"},
  ];
  const compile = (path: string) => {
    const built = buildImportedStorySource(path, lines);
    assert.equal(built.valid, true, JSON.stringify(built.diagnostics));
    assert.equal(built.value?.source.startsWith("; @prismatix-ids\n"), true);
    assert.deepEqual(built.value?.identityMap.entries.map((entry) => entry.id),
      ["original.start", "original.opening", "original.end"]);
    const compiled = compileStoryProject({
      storyIndex: {format: "PrismatiXStoryIndex", schemaRevision: 2,
        id: "imported", entryScene: "main",
        chapters: [{id: "chapter", title: "Chapter", scenes: ["main"]}],
        scenes: [{id: "main", identityNamespace: "imported:main",
          sources: {en: path}}]},
      locale: "en",
      sourceFiles: {[path]: built.value!.source,
        [`${path}.pxids.json`]: JSON.stringify(built.value!.identityMap)},
    });
    assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
    return compiled.runtimeIr!.operations.map((operation) =>
      [operation.sourceId, operation.operationId]);
  };
  assert.deepEqual(compile("Story/en/old.pxstory"),
    compile("Story/en/moved.pxstory"));
});

test("imported Story lines reject missing, duplicate, and injected IDs", () => {
  const path = "Story/en/main.pxstory";
  assert.equal(buildImportedStorySource(path, [
    {source: "*start", id: "start"}, {source: "Text."},
  ]).diagnostics[0]?.code, "PXIMPORT1009");
  assert.equal(buildImportedStorySource(path, [
    {source: "*start", id: "same"}, {source: "Text.", id: "same"},
  ]).valid, false);
  assert.equal(buildImportedStorySource(path, [
    {source: "[id hidden]"}, {source: "Text.", id: "text"},
  ]).diagnostics[0]?.code, "PXIMPORT1008");
  assert.equal(buildImportedStorySource(path, [
    {source: "Text.\n[end]", id: "text"},
  ]).diagnostics[0]?.code, "PXIMPORT1006");
  assert.equal(buildImportedStorySource("../outside.pxstory", [
    {source: "Text.", id: "text"},
  ]).diagnostics[0]?.code, "PXIMPORT1005");
});

test("imported speaker and choice-wait lines do not need execution IDs", () => {
  const result = buildImportedStorySource("Story/zh-TW/main.pxstory", [
    {source: "*start", id: "start"},
    {source: '@"雪"'},
    {source: "你好。", id: "greeting"},
    {source: "[choice.wait]"},
    {source: "[end]", id: "end"},
  ]);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.value?.identityMap.entries.map((entry) => entry.id),
    ["start", "greeting", "end"]);
});
