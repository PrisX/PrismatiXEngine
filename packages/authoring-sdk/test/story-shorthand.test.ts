import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, compileStory, createStoryAssetIndex, normalizeProject,
  parseStory} from "../src/index.js";
import type {CharacterDocument} from "../src/index.js";

const yuki: CharacterDocument = {
  format: "PrismatiXCharacter",
  schemaRevision: 2,
  id: "11111111-1111-4111-8111-111111111111",
  displayName: "雪",
  aliases: ["yuki"],
  defaultExpressionId: "22222222-2222-4222-8222-222222222222",
  expressions: [
    {id:"22222222-2222-4222-8222-222222222222",name:"Smile",aliases:["smile"],assetId:"33333333-3333-4333-8333-333333333333"},
    {id:"44444444-4444-4444-8444-444444444444",name:"Angry",aliases:["angry"],assetId:"55555555-5555-4555-8555-555555555555"},
  ],
};

const assets = createStoryAssetIndex({
  bg: ["Assets/bg", "Assets/cg"],
  rule: ["Assets/rule"],
  voice: ["Assets/voice"],
  bgm: ["Assets/bgm"],
}, [
  "Assets/bg/school.png", "Assets/cg/ev01.png", "Assets/cg/school.png",
  "Assets/rule/wipe.png", "Assets/voice/yuki/y001.ogg",
  "Assets/voice/kei/k001.ogg", "Assets/voice/kei/dup.ogg", "Assets/voice/yuki/dup.ogg",
  "Assets/bgm/theme.ogg", "Assets/bgm/theme.wav",
]);

function operations(source: string, context: Partial<Parameters<typeof compileStory>[1]> = {}) {
  const result = compileStory(parseStory(source, "Story/short.pxstory"),
    {documentId: "short", ...context});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  return result.runtimeIr!.operations;
}

function codes(source: string, context: Partial<Parameters<typeof compileStory>[1]> = {}) {
  const document = parseStory(source, "Story/short.pxstory");
  return [...document.diagnostics,
    ...compileStory(document, {documentId: "short", ...context}).diagnostics]
    .map((item) => item.code);
}

test("inline speaker lines speak once and return to narration", () => {
  const ops = operations("@yuki おはよう。\n@直哉「うっぷ……」\n@\"Old Man\" (coughs) Hello.\n風が吹く。\n",
    {characters: [yuki]});
  assert.deepEqual(ops.map(({kind, arguments: args}) => [kind, args.speaker ?? "", args.character ?? "", args.text]), [
    ["dialogue", "雪", yuki.id, "おはよう。"],
    ["dialogue", "直哉", "", "「うっぷ……」"],
    ["dialogue", "Old Man", "", "(coughs) Hello."],
    ["narration", "", "", "風が吹く。"],
  ]);
});

test("display-name speaker blocks need no quotes or character catalog", () => {
  const ops = operations("@直哉\n「一行目」\n「二行目」\n\n地の文。\n", {characters: [yuki]});
  assert.deepEqual(ops.map(({kind, arguments: args}) => [kind, args.speaker ?? ""]), [
    ["dialogue", "直哉"], ["dialogue", "直哉"], ["narration", ""],
  ]);
  assert.ok(codes("@yuko おはよう。\n", {characters: [yuki]}).includes("PXSTORY1102"));
});

test("trailing backslash keeps one advance across several source lines", () => {
  const document = parseStory("あれめんどくさいんだよなぁ。\\\n  他で返せたら、そっちでチャラにしたいものだ。\n@直哉「一行目\\\n二行目」\n末尾\\\\\n",
    "Story/multiline.pxstory");
  assert.deepEqual(document.diagnostics, []);
  const [first, second, third] = document.nodes;
  assert.equal(first?.text, "あれめんどくさいんだよなぁ。\n他で返せたら、そっちでチャラにしたいものだ。");
  assert.equal(first?.span.start.line, 1);
  assert.equal(first?.span.end.line, 2);
  assert.equal(second?.kind, "dialogue");
  assert.equal(second?.text, "「一行目\n二行目」");
  assert.equal(third?.text, "末尾\\\\", "an escaped backslash does not continue");
});

test("speaker options change expression and position before the line", () => {
  const ops = operations("[show yuki smile center]\n@yuki[angry left] もう！\n", {characters: [yuki]});
  assert.deepEqual(ops.map(({kind, arguments: args}) => [kind, args.expression ?? "", args.position ?? ""]), [
    ["showCharacter", "22222222-2222-4222-8222-222222222222", "2"],
    ["showCharacter", "44444444-4444-4444-8444-444444444444", "1"],
    ["dialogue", "", ""],
  ]);
  assert.equal(ops[1]!.sourceId, ops[2]!.sourceId);
  assert.notEqual(ops[1]!.operationId, ops[2]!.operationId);
  assert.ok(codes("@yuki[frown] hi\n", {characters: [yuki]}).includes("PXSTORY1114"));
  assert.ok(codes("@直哉[smile] hi\n").includes("PXSTORY1162"));
  assert.ok(codes("@yuki[smile]\nhi\n", {characters: [yuki]}).includes("PXSTORY1018"));
  assert.ok(codes("@yuki[mood=smile] hi\n", {characters: [yuki]}).includes("PXSTORY1017"));
});

test("an authored id pins every operation from one inline line", () => {
  const ops = operations("[id greet]\n@yuki[smile] やあ\n", {characters: [yuki]});
  assert.equal(ops.length, 2);
  assert.equal(ops[0]!.sourceId, ops[1]!.sourceId);
  assert.equal(ops[0]!.sourceId, operations("[id greet]\n@yuki[smile] 違う文\n",
    {characters: [yuki]})[0]!.sourceId);
});

test("show classifies extra positional words as expression or position", () => {
  const [shown] = operations("[show yuki left smile]\n", {characters: [yuki]});
  assert.equal(shown?.arguments.expression, "22222222-2222-4222-8222-222222222222");
  assert.equal(shown?.arguments.position, "1");
  assert.ok(codes("[show yuki smile angry]\n", {characters: [yuki]}).includes("PXSTORY1016"));
});

test("storyAssets roots resolve short names with explicit precedence", () => {
  const result = compileStory(parseStory(
    "[bg school rule=wipe]\n[bg ev01]\n@yuki[voice=y001] hi\n@kei[voice=kei/dup] yo\n[bgm theme.ogg]\n[bg Assets/cg/school.png]\n"),
  {documentId: "assets", storyAssets: assets});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const ops = result.runtimeIr!.operations;
  assert.deepEqual(ops.map(({arguments: args}) => args.asset ?? args.voice), [
    "Assets/bg/school.png", "Assets/cg/ev01.png", "Assets/voice/yuki/y001.ogg",
    "Assets/voice/kei/dup.ogg", "Assets/bgm/theme.ogg", "Assets/cg/school.png",
  ]);
  assert.equal(ops[0]!.arguments.rule, "Assets/rule/wipe.png");
  assert.ok(result.resourceDependencies?.includes("Assets/voice/yuki/y001.ogg"));
});

test("storyAssets reports ambiguous and missing short names at the argument", () => {
  const ambiguous = compileStory(parseStory("[bgm theme]\n@kei[voice=dup] yo\n"),
    {documentId: "assets", storyAssets: assets});
  assert.deepEqual(ambiguous.diagnostics.map(({code}) => code), ["PXSTORY1161", "PXSTORY1161"]);
  assert.equal(ambiguous.diagnostics[1]?.span?.start.column, 12);
  const missing = compileStory(parseStory("[bg shcool]\n"),
    {documentId: "assets", storyAssets: assets});
  assert.deepEqual(missing.diagnostics.map(({code}) => code), ["PXSTORY1160"]);
  assert.match(missing.diagnostics[0]!.message, /Assets\/bg, Assets\/cg/u);
});

test("storyAssets is an author-only manifest field", () => {
  const normalized = normalizeProject({project: {name: "Roots", storyAssets: {bg: ["Assets/bg"]}},
    sourceFiles: {"Story/main.pxstory": "[bg school]\n"}});
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.deepEqual(normalized.value?.storyAssets, {bg: ["Assets/bg"]});
  assert.equal("storyAssets" in (normalized.value?.project ?? {}), false);
  for (const storyAssets of [{background: ["Assets"]}, {bg: []}, {bg: ["../x"]}, ["Assets"]]) {
    const invalid = normalizeProject({project: {name: "Roots", storyAssets}});
    assert.deepEqual(invalid.diagnostics.map(({code}) => code), ["PXBUILD1056"]);
  }
});

test("project compile resolves storyAssets against included resources", () => {
  const result = compileProject({
    project: {name: "Roots", resourceIncludes: ["Assets"], storyAssets: {bg: ["Assets/bg"]}},
    sourceFiles: {"Story/main.pxstory": "[bg school]\nこんにちは。\n"},
    resolveResourceInclude: () => ["Assets/bg/school.png"],
    resourceExists: () => true,
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const operation = result.artifact!.runtimeIr.operations.find(({kind}) => kind === "background");
  assert.equal(operation?.arguments.asset, "Assets/bg/school.png");
});
