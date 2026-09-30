import assert from "node:assert/strict";
import test from "node:test";

import {assembleImportedStoryProject, canonicalJson, compileStoryProject, externalizeStoryIds,
  serializeStoryIdentityMap} from "../src/index.js";

const story = "*start\n@guide こんにちは。\n[choice]\n- 行く -> go\n- 残る -> stay\n[endchoice]\n*go\n行こう。\n*stay\n残ろう。\n";

test("sidecars are written one entry per line and keep every identity", () => {
  const externalized = externalizeStoryIds(story, {path: "Story/main.pxstory"});
  assert.equal(externalized.valid, true, JSON.stringify(externalized.diagnostics));
  const map = externalized.map!;
  const text = serializeStoryIdentityMap(map);
  const lines = text.trimEnd().split("\n");
  assert.equal(lines[0], "{\"entries\":[");
  assert.equal(lines.at(-1), "],\"format\":\"PrismatiXStoryIdentityMap\",\"schemaRevision\":1}");
  assert.equal(lines.length, map.entries.length + 2);
  for (const [index, entry] of map.entries.entries())
    assert.equal(lines[index + 1], canonicalJson(entry, false).trimEnd() +
      (index + 1 < map.entries.length ? "," : ""));
  assert.deepEqual(JSON.parse(text), JSON.parse(canonicalJson(map)));
  assert.ok(text.length < canonicalJson(map).length * 0.75);
  assert.equal(serializeStoryIdentityMap({...map, entries: []}),
    "{\"entries\":[],\"format\":\"PrismatiXStoryIdentityMap\",\"schemaRevision\":1}\n");
});

test("compact and pretty-printed sidecars compile to the same identities", () => {
  const externalized = externalizeStoryIds(story, {path: "Story/main.pxstory"});
  const index = {format: "PrismatiXStoryIndex" as const, schemaRevision: 2 as const, id: "main",
    entryScene: "main", chapters: [{id: "c", title: "C", scenes: ["main"]}],
    scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}]};
  const compile = (sidecar: string) => compileStoryProject({storyIndex: index, locale: "en",
    sourceFiles: {"Story/main.pxstory": externalized.source!, "Story/main.pxstory.pxids.json": sidecar}});
  const compact = compile(serializeStoryIdentityMap(externalized.map!));
  const pretty = compile(canonicalJson(externalized.map!));
  const oneLine = compile(`${JSON.stringify(externalized.map!)}\n`);
  assert.equal(compact.valid, true, JSON.stringify(compact.diagnostics));
  assert.deepEqual(compact.runtimeIr, pretty.runtimeIr);
  assert.deepEqual(compact.runtimeIr, oneLine.runtimeIr);
});

test("imported projects write compact sidecars", () => {
  const result = assembleImportedStoryProject({id: "imported", entryScene: "first",
    locales: ["en"], chapters: [{id: "chapter", title: "Chapter"}],
    scenes: [{id: "first", chapter: "chapter", sources: {en: {path: "Story/en/first.pxstory",
      lines: [{source: "こんにちは。", id: "first.001"}, {source: "[end]", id: "first.002"}]}}}]});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const sidecar = result.value!.files["Story/en/first.pxstory.pxids.json"]!;
  // Header line, two entries, footer line and the final newline.
  assert.equal(sidecar.split("\n").length, 5);
  assert.doesNotMatch(sidecar, /^ {2}/mu);
});
