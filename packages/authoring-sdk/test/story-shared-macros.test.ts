import assert from "node:assert/strict";
import test from "node:test";

import {applyStoryIdentityMap, compileProject, compileStory, createStoryMacroLibrary,
  externalizeStoryIds, normalizeProject, parseStory} from "../src/index.js";
import type {CharacterDocument} from "../src/index.js";

const yuki: CharacterDocument = {
  format: "PrismatiXCharacter", schemaRevision: 1,
  id: "11111111-1111-4111-8111-111111111111", displayName: "雪", aliases: ["yuki"],
  defaultExpressionId: "22222222-2222-4222-8222-222222222222",
  expressions: [{id: "22222222-2222-4222-8222-222222222222", name: "Smile", aliases: ["smile"],
    assetId: "33333333-3333-4333-8333-333333333333"}],
};

const macroPath = "Story/_macros.pxstory";
const macroFile = [
  "; Scene changes shared by every chapter.",
  "[macro scene_change bg rule=wipe]",
  "[hide all transition=false]",
  "[bg %bg transition=false]",
  "[transition 800ms rule=%rule]",
  "[endmacro]",
  "",
  "[macro greet who]",
  "@%who こんにちは。",
  "[endmacro]",
  "",
].join("\n");

function library(source = macroFile) {
  const built = createStoryMacroLibrary([{path: macroPath, source}]);
  assert.deepEqual(built.diagnostics, []);
  return built.library;
}

test("storyMacros definitions expand in any Story like local macros", () => {
  const scene = "[scene_change Assets/park.png rule=Assets/rule.png]\n[greet yuki]\n";
  const result = compileStory(parseStory(scene, "Story/day1.pxstory"),
    {documentId: "day1", characters: [yuki], storyMacros: library()});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const operations = result.runtimeIr!.operations;
  assert.deepEqual(operations.map(({kind}) => kind),
    ["hideCharacters", "background", "transition", "dialogue"]);
  assert.equal(operations[1]!.arguments.asset, "Assets/park.png");
  assert.equal(operations[2]!.arguments.rule, "Assets/rule.png");
  // Identities are the call's plus the body line, and the source map points
  // at the call in the scene, not into the macro file.
  const call = parseStory(scene, "Story/day1.pxstory").nodes[0]!.id;
  assert.deepEqual(operations.slice(0, 3).map(({sourceId}) => sourceId),
    [`${call}/m0`, `${call}/m1`, `${call}/m2`]);
  for (const mapping of result.sourceMap!.mappings.slice(0, 3)) {
    assert.equal(mapping.sourceUri, "Story/day1.pxstory");
    assert.equal(mapping.startLine, 1);
  }
});

test("editing a shared macro file keeps the save identities of its calls", () => {
  const scene = "[greet yuki]\n";
  const compile = (source: string) => compileStory(parseStory(scene, "Story/day1.pxstory"),
    {documentId: "day1", characters: [yuki], storyMacros: library(source)}).runtimeIr!.operations;
  const before = compile(macroFile);
  const after = compile(`; A new comment and a new macro first.\n[macro other]\nx\n[endmacro]\n${
    macroFile.replace("こんにちは。", "こんばんは。")}`);
  assert.equal(after[0]!.arguments.text, "こんばんは。");
  assert.deepEqual(after.map(({operationId}) => operationId), before.map(({operationId}) => operationId));
});

test("shared macro diagnostics point at the call and name the definition", () => {
  const scene = "\n[greet nobody]\n[scene_change]\n";
  const result = compileStory(parseStory(scene, "Story/day1.pxstory"),
    {documentId: "day1", characters: [yuki], storyMacros: library()});
  const unknown = result.diagnostics.find(({code}) => code === "PXSTORY1102");
  assert.equal(unknown?.path, "Story/day1.pxstory");
  assert.equal(unknown?.span?.start.line, 2);
  assert.match(unknown?.details ?? "", /in macro greet \(Story\/_macros\.pxstory:8\)/u);
  const missing = result.diagnostics.find(({code}) => code === "PXSTORY1195");
  assert.equal(missing?.span?.start.line, 3);
  assert.match(missing?.details ?? "", /Story\/_macros\.pxstory:2/u);
});

test("macro files hold only definitions and names stay unique", () => {
  const extra = createStoryMacroLibrary([{path: macroPath, source: `${macroFile}こんにちは。\n[bg x]\n`}]);
  assert.deepEqual(extra.diagnostics.map(({code, span}) => [code, span?.start.line]),
    [["PXSTORY1196", 11], ["PXSTORY1196", 12]]);
  const twice = createStoryMacroLibrary([{path: macroPath, source: macroFile},
    {path: "Story/_more.pxstory", source: "[macro greet]\nx\n[endmacro]\n"}]);
  assert.deepEqual(twice.diagnostics.map(({code, path}) => [code, path]),
    [["PXSTORY1191", "Story/_more.pxstory"]]);
  assert.match(twice.diagnostics[0]!.message, /already defined at Story\/_macros\.pxstory:8/u);
  const reserved = createStoryMacroLibrary([{path: macroPath, source: "[macro bg]\nx\n[endmacro]\n"}]);
  assert.deepEqual(reserved.diagnostics.map(({code}) => code), ["PXSTORY1191"]);
  const local = compileStory(parseStory("[macro greet]\nx\n[endmacro]\n", "Story/a.pxstory"),
    {documentId: "a", storyMacros: library()});
  assert.deepEqual(local.diagnostics.map(({code}) => code), ["PXSTORY1191"]);
});

test("identity sidecars pin shared macro calls like any other operation", () => {
  const scene = "; @prismatix-ids\n[greet yuki]\n";
  const plain = "[greet yuki]\n";
  const externalized = externalizeStoryIds(plain, {path: "Story/a.pxstory"});
  assert.equal(externalized.valid, true, JSON.stringify(externalized.diagnostics));
  assert.equal(externalized.map!.entries.length, 1);
  const pinned = applyStoryIdentityMap(parseStory(scene, "Story/a.pxstory"), externalized.map);
  const result = compileStory(pinned, {documentId: "a", characters: [yuki], storyMacros: library()});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.runtimeIr!.operations[0]!.sourceId,
    `${externalized.map!.entries[0]!.sourceId}/m0`);
});

test("storyMacros is an author-only project field compiled for every scene", () => {
  const normalized = normalizeProject({project: {name: "Macros", storyMacros: [macroPath]},
    sourceFiles: {"Story/main.pxstory": "[greet guide]\n"}});
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.deepEqual(normalized.value?.storyMacros, [macroPath]);
  assert.equal("storyMacros" in (normalized.value?.project ?? {}), false);
  for (const storyMacros of [[], ["../x.pxstory"], ["Story/a.txt"], [macroPath, macroPath], macroPath]) {
    const invalid = normalizeProject({project: {name: "Macros", storyMacros}});
    assert.deepEqual(invalid.diagnostics.map(({code}) => code), ["PXBUILD1120"], JSON.stringify(storyMacros));
  }

  const compiled = compileProject({project: {name: "Macros", storyMacros: [macroPath]},
    sourceFiles: {"Story/main.pxstory": "[greet guide]\n[greet 直哉]\n",
      [macroPath]: macroFile}});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const dialogue = compiled.artifact!.runtimeIr.operations.filter(({kind}) => kind === "dialogue");
  assert.deepEqual(dialogue.map(({arguments: args}) => args.speaker), ["guide", "直哉"]);
  assert.equal(JSON.stringify(compiled.artifact!.project).includes("storyMacros"), false);

  const missing = compileProject({project: {name: "Macros", storyMacros: [macroPath]},
    sourceFiles: {"Story/main.pxstory": "こんにちは。\n"}});
  assert.deepEqual(missing.diagnostics.map(({code}) => code), ["PXBUILD1121"]);
});
