import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";

import {canonicalJson, compileStory, parseStory, validateDocument} from "../src/index.js";
import type {CharacterDocument, ExtensionManifest, GameDocument} from "../src/index.js";

const character: CharacterDocument = {
  format: "PrismatiXCharacter",
  schemaRevision: 2,
  id: "11111111-1111-4111-8111-111111111111",
  displayName: "雪",
  displayNames: {"ja-JP": "ゆき"},
  aliases: ["yuki"],
  defaultExpressionId: "22222222-2222-4222-8222-222222222222",
  expressions: [{id:"22222222-2222-4222-8222-222222222222",name:"Smile",aliases:["smile"],assetId:"33333333-3333-4333-8333-333333333333"}],
};

test("Story dialogue uses the authored locale name without changing actor identity", () => {
  const document = parseStory("@yuki\nおはよう。\n", "Story/ja-JP/intro.pxstory");
  const localized = compileStory(document, {documentId: "intro", locale: "ja-JP",
    characters: [character]});
  const fallback = compileStory(document, {documentId: "intro", locale: "zh-TW",
    characters: [character]});
  assert.equal(localized.valid, true, JSON.stringify(localized.diagnostics));
  assert.equal(localized.runtimeIr?.operations[0]?.arguments.speaker, "ゆき");
  assert.equal(fallback.runtimeIr?.operations[0]?.arguments.speaker, "雪");
  assert.equal(localized.runtimeIr?.operations[0]?.arguments.character, character.id);
  assert.equal(localized.runtimeIr?.operations[0]?.operationId,
    fallback.runtimeIr?.operations[0]?.operationId);
});

test("quoted Story speaker keeps visible name separate without inventing a character", () => {
  const document = parseStory('@"村雨"\n早安，主人。\n',
    "Story/zh-TW/intro.pxstory");
  const result = compileStory(document, {documentId: "intro", characters: [character]});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.equal(result.runtimeIr?.operations[0]?.kind, "dialogue");
  assert.equal(result.runtimeIr?.operations[0]?.arguments.speaker, "村雨");
  assert.equal(result.runtimeIr?.operations[0]?.arguments.character, "");
  assert.equal(result.runtimeIr?.operations[0]?.arguments.text, "早安，主人。");
  const malformed = parseStory('@"村雨\n早安。\n');
  assert.ok(malformed.diagnostics.some((item) => item.code === "PXSTORY1015"));
});

test("hide all clears actors with one readable operation and optional instant transition", () => {
  const result = compileStory(parseStory(
    "[hide all]\n[hide all transition=false]\n[hide yuki transition=false]\n",
    "Story/clear.pxstory"), {documentId: "clear", characters: [character]});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.runtimeIr?.operations.map(({kind, arguments: args}) => [kind, args]), [
    ["hideCharacters", {}],
    ["hideCharacters", {transition:"false"}],
    ["hideCharacter", {character:character.id, transition:"false"}],
  ]);
  const invalid = compileStory(parseStory("[hide all transition=maybe]\n"),
    {documentId: "invalid-clear"});
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((item) => item.code === "PXSTORY1145"));
  const namedAll = compileStory(parseStory("[hide character=all]\n"),
    {documentId: "named-all", characters: [{...character, aliases:["all"]}]});
  assert.equal(namedAll.valid, true, JSON.stringify(namedAll.diagnostics));
  assert.deepEqual(namedAll.runtimeIr?.operations.map(({kind, arguments: args}) => [kind, args]), [
    ["hideCharacter", {character:character.id}],
  ]);
});
const game: GameDocument = {
  format: "PrismatiXGame",
  schemaRevision: 2,
  variables: [{name:"affection",type:"integer",default:0,scope:"profile"}],
};
const extension: ExtensionManifest = {
  format: "PrismatiXExtension",
  schemaRevision: 2,
  language: "javascript",
  id: "weather",
  version: "1.0.0",
  requiredEngineVersion: "^0.2.0",
  entry: "weather.js",
  capabilities: ["runtime"],
  commands: [
    {id:"weather",parameters:[{name:"type",type:"string",required:true,enum:["rain","snow"]},{name:"strength",type:"number",required:true,range:{minimum:0,maximum:1}}]},
    {id:"stage.compose",parameters:[
      {name:"offset",type:"vec2",required:true},
      {name:"bounds",type:"rect",required:true},
      {name:"tint",type:"color",required:true},
      {name:"tags",type:"array",required:true},
      {name:"metadata",type:"object",required:true},
      {name:"asset",type:"resource",required:true},
    ]},
  ],
  actions: [],
};

const source = `; Chapter 1

*start
[bg classroom]
[show yuki expression=smile position=center x=-12 y=8 scale=0.67]

@yuki
早安。
今天也請多指教。

[set affection=10]
[weather type=rain strength=0.8]
[choice text="早安。" goto=normal]
[choice text="妳今天來得真早。" goto=tease]
[choice.wait]

*normal
@yuki
嗯！
[jump target=continue]

*tease
@yuki
偶爾啦……

*continue
`;

test("line-oriented parser retains source spans without indentation semantics", () => {
  const plain = parseStory(source, "Story/zh-TW/ch01.pxstory");
  const indented = parseStory(source.split("\n").map((line) => line.length === 0 ? line : `    ${line}`).join("\n"), "Story/zh-TW/ch01.pxstory");
  assert.equal(plain.diagnostics.length, 0);
  assert.deepEqual(plain.nodes.map((node) => [node.kind, node.name, node.text]), indented.nodes.map((node) => [node.kind, node.name, node.text]));
  const firstDialogue = plain.nodes.find((node) => node.kind === "dialogue");
  assert.equal(firstDialogue?.span.start.line, 8);
  assert.equal(firstDialogue?.span.start.column, 1);
});

test("blank lines end speaker scope so the next text paragraph is narration", () => {
  const parsed = parseStory(`@yuki\n第一句。\n第二句。\n\n這是旁白。\n@yuki\n又是對話。\n`, "Story/paragraphs.pxstory");
  assert.equal(parsed.diagnostics.length, 0);
  assert.deepEqual(
    parsed.nodes.filter((node) => node.kind === "dialogue" || node.kind === "narration").map((node) => [node.kind, node.name, node.text]),
    [
      ["dialogue", "yuki", "第一句。"],
      ["dialogue", "yuki", "第二句。"],
      ["narration", undefined, "這是旁白。"],
      ["dialogue", "yuki", "又是對話。"],
    ],
  );
});

test("chapter authors a durable in-story presentation title", () => {
  const compiled = compileStory(parseStory("[id chapter-one]\n[chapter title=\"CHAPTER 1-1\"]\nDialogue.\n", "chapter.pxstory"),
    {documentId: "chapter-fixture"});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const chapter = compiled.runtimeIr!.operations.find((operation) => operation.kind === "scene");
  assert.equal(chapter?.arguments.title, "CHAPTER 1-1");
  const shifted = compileStory(parseStory("; inserted note\n[id chapter-one]\n[chapter title=\"CHAPTER 1-1\"]\nDialogue.\n", "chapter.pxstory"),
    {documentId: "chapter-fixture"});
  assert.equal(chapter?.operationId,
    shifted.runtimeIr!.operations.find((operation) => operation.kind === "scene")?.operationId);
  const missing = compileStory(parseStory("[chapter]\n", "missing.pxstory"),
    {documentId: "chapter-fixture"});
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1144"));
});

test("single-document Story compilation points cross-scene authors to the project API", () => {
  for (const command of ["jump", "call", "choice"] as const) {
    const syntax = command === "choice"
      ? '[choice text="Continue" scene=next target=start]'
      : `[${command} scene=next target=start]`;
    const compiled = compileStory(parseStory(`*start\n${syntax}\n`, "Story/one.pxstory"),
      {documentId: "one"});
    assert.equal(compiled.valid, false);
    assert.ok(compiled.diagnostics.some((item) => item.code === "PXSTORY1215" &&
      item.message.includes("compileStoryProject")), JSON.stringify(compiled.diagnostics));
  }
});

test("Story compiler emits only executable semantic operations and source maps", () => {
  const parsed = parseStory(source, "Story/zh-TW/ch01.pxstory");
  const compiled = compileStory(parsed, {
    documentId: "chapter01.scene01",
    committedRevision: 7,
    characters: [character],
    game,
    extensions: [extension],
    resources: {classroom:"Assets/Images/classroom.png"},
  });
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.ok(compiled.runtimeIr);
  assert.ok(compiled.sourceMap);
  assert.equal(validateDocument("runtimeIr", compiled.runtimeIr).valid, true);
  assert.equal(validateDocument("sourceMap", compiled.sourceMap).valid, true);
  assert.equal(compiled.runtimeIr!.operations.some((operation) => ["sequence", "group", "document", "choice-wrapper"].includes(operation.kind)), false);
  assert.equal(compiled.runtimeIr!.operations.some((operation) => operation.kind === "customNode" && operation.arguments.type === "weather"), true);
  assert.deepEqual(
    compiled.runtimeIr!.operations.find((operation) => operation.kind === "setVariable")?.arguments,
    {name:"affection",value:"10",scope:"profile",type:"integer"},
  );
  const shown = compiled.runtimeIr!.operations.find((operation) => operation.kind === "showCharacter");
  assert.equal(shown?.arguments.character, character.id);
  assert.equal(shown?.arguments.expression, character.expressions[0]!.id);
  assert.equal(shown?.arguments.sprite, `asset:${character.expressions[0]!.assetId}`);
  assert.equal(shown?.arguments.position, "2");
  assert.equal(shown?.arguments.x, "-12");
  assert.equal(shown?.arguments.y, "8");
  assert.equal(shown?.arguments.scale, "0.67");
  assert.equal(compiled.sourceMap!.mappings.length, compiled.runtimeIr!.operations.length);
});

test("durable audio Story commands lower to typed Runtime IR operations", () => {
  const compiled = compileStory(
    parseStory([
      "[bgm Assets/Audio/theme.ogg intro=Assets/Audio/theme-intro.ogg fade=250ms]",
      "[ambience Assets/Audio/rain.ogg loop=false fade=100ms]",
      "[ambience.stop fade=80ms]",
      "[bgm.stop fade=120ms]",
    ].join("\n"), "Story/audio.pxstory"),
    {documentId: "audio"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => [operation.kind, operation.arguments]), [
    ["bgm", {asset:"Assets/Audio/theme.ogg",fade:"250ms",intro:"Assets/Audio/theme-intro.ogg"}],
    ["ambience", {asset:"Assets/Audio/rain.ogg",fade:"100ms",loop:"false"}],
    ["stopAmbience", {fade:"80ms"}],
    ["stopBgm", {fade:"120ms"}],
  ]);
  assert.deepEqual(compiled.resourceDependencies, [
    "Assets/Audio/rain.ogg", "Assets/Audio/theme-intro.ogg", "Assets/Audio/theme.ogg",
  ]);
});

test("Story audio options reject unsupported or invalid playback requests", () => {
  const invalid = [
    ["[bgm Assets/Audio/theme.ogg intro=../escape.ogg]", "PXSTORY1111"],
    ["[bgm Assets/Audio/theme.ogg intro=Assets/Audio/intro.ogg loop=false]", "PXSTORY1148"],
    ["[ambience Assets/Audio/rain.ogg loop=maybe]", "PXSTORY1147"],
    ["[voice Assets/Audio/line.ogg fade=250ms]", "PXSTORY1146"],
    ["[se Assets/Audio/click.ogg loop=true]", "PXSTORY1147"],
    ["[bgm.stop fade=999999999999999999ms]", "PXSTORY1146"],
    ["[ambience.stop fade=soon]", "PXSTORY1146"],
  ] as const;
  for (const [source, code] of invalid) {
    const compiled = compileStory(parseStory(source, "Story/audio-invalid.pxstory"),
      {documentId: "audio-invalid"});
    assert.equal(compiled.valid, false, source);
    assert.ok(compiled.diagnostics.some((diagnostic) => diagnostic.code === code),
      `${source}: ${JSON.stringify(compiled.diagnostics)}`);
  }
});

test("unlock lowers to a typed Runtime IR operation", () => {
  const compiled = compileStory(
    parseStory("[unlock kind=cg id=ev101a]\n", "Story/unlock.pxstory"),
    {documentId: "unlock"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => [operation.kind, operation.arguments]), [
    ["unlock", {kind:"cg", id:"ev101a"}],
  ]);
});

test("video lowers to a typed Runtime IR operation and resource dependency", () => {
  const compiled = compileStory(
    parseStory("[video asset=Assets/Video/op.mp4 skippable=true]\n", "Story/video.pxstory"),
    {documentId: "video"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => [operation.kind, operation.arguments]), [
    ["video", {asset:"Assets/Video/op.mp4", skippable:"true"}],
  ]);
  assert.deepEqual(compiled.resourceDependencies, ["Assets/Video/op.mp4"]);
});

test("background transition rules lower with timing and both resources", () => {
  const compiled = compileStory(
    parseStory("[bg asset=Assets/Backgrounds/shrine.png rule=Content/Images/Rules/rule_7.png duration=1500ms vague=64]\n", "Story/background-rule.pxstory"),
    {documentId: "background-rule"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => [operation.kind, operation.arguments]), [
    ["background", {
      asset:"Assets/Backgrounds/shrine.png",
      rule:"Content/Images/Rules/rule_7.png",
      duration:"1500ms",
      vague:"64",
    }],
  ]);
  assert.deepEqual(compiled.resourceDependencies, [
    "Assets/Backgrounds/shrine.png", "Content/Images/Rules/rule_7.png",
  ]);
});

test("shake lowers to an explicit Runtime IR operation", () => {
  const compiled = compileStory(
    parseStory("[shake duration=250ms amplitude=15]\n", "Story/shake.pxstory"),
    {documentId: "shake"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => [operation.kind, operation.arguments]), [
    ["shake", {duration:"250ms", amplitude:"15"}],
  ]);
});

test("Story catalog assignments lower declared scope and scalar type explicitly", () => {
  const catalog: GameDocument = {
    format: "PrismatiXGame",
    schemaRevision: 2,
    variables: [
      {name:"route",type:"string",default:"common",scope:"session"},
      {name:"endingUnlocked",type:"boolean",default:false,scope:"profile"},
      {name:"affection",type:"number",default:0,scope:"profile"},
    ],
  };
  const compiled = compileStory(
    parseStory('[set route="rin"]\n[set endingUnlocked=true]\n[set affection=5]\n', "Story/catalog-variables.pxstory"),
    {documentId:"catalog-variables",game:catalog},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => operation.arguments), [
    {name:"route",value:"rin",scope:"session",type:"string"},
    {name:"endingUnlocked",value:"true",scope:"profile",type:"boolean"},
    {name:"affection",value:"5",scope:"profile",type:"number"},
  ]);
});

test("if expressions preserve comparison operators and quoted equals through Story parsing", () => {
  const catalog: GameDocument = {format: "PrismatiXGame", schemaRevision: 2, variables: [
    {name: "affection", type: "integer", default: 0, scope: "session"},
    {name: "route", type: "string", default: "a=b", scope: "session"},
  ]};
  const cases = [
    ["affection >= 1", "greater-equal"], ["affection<=1", "less-equal"],
    ["affection != 1", "not-equal"], ["affection==1", "equal"],
    ['route == "a=b"', "equal"], ['route!="a=b"', "not-equal"],
  ] as const;
  for (const [expression, expectedOperator] of cases) {
    const source = `  [if ${expression}]\nYes.\n[else]\nNo.\n[endif]\n`;
    const parsed = parseStory(source, "Story/comparison.pxstory");
    const compiled = compileStory(parsed, {documentId: "comparison", game: catalog});
    assert.equal(compiled.valid, true, `${expression}: ${JSON.stringify(compiled.diagnostics)}`);
    const condition = compiled.runtimeIr?.operations.find((operation) => operation.kind === "condition");
    assert.equal(JSON.parse(String(condition?.arguments.expression)).op, expectedOperator);
    const argument = parsed.nodes[0]?.arguments?.[0];
    assert.equal(argument?.span.start.offset, 6);
    assert.equal(argument?.span.start.column, 7);
    assert.equal(source.slice(argument!.span.start.offset, argument!.span.end.offset), argument?.raw);
  }
  const assignment = parseStory('[set route="a=b"]\n', "Story/assignment.pxstory");
  assert.equal(assignment.nodes[0]?.arguments?.[0]?.name, "route");
  assert.equal(assignment.nodes[0]?.arguments?.[0]?.value, "a=b");
  assert.equal(assignment.nodes[0]?.arguments?.[0]?.span.start.offset, 11);
});

test("character placement names and authored moves lower to explicit Runtime commands", () => {
  const compiled = compileStory(
    parseStory(`[show yuki position=left x=24 scale=0.8]\n[move yuki position=right x=-48 y=32 scale=1 duration=450ms ease=easeInOut wait=true]\n`, "Story/placement.pxstory"),
    {documentId:"placement", characters:[character]},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => [operation.kind, operation.arguments]), [
    ["showCharacter", {character:character.id, position:"1", x:"24", scale:"0.8"}],
    ["timeline", {mode:"move", target:character.id, position:"3", x:"-48", y:"32", scale:"1", duration:"450ms", ease:"easeInOut", wait:"true"}],
  ]);

  const invalid = compileStory(
    parseStory(`[show yuki position=somewhere]\n`, "Story/invalid-placement.pxstory"),
    {documentId:"invalid-placement", characters:[character]},
  );
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1128"));
});

test("show accepts a path-first sprite without a character catalog", () => {
  const compiled = compileStory(
    parseStory("[show character=yoshino asset=Assets/Characters/yoshino.png position=center y=24 scale=0.65]\n", "Story/direct-sprite.pxstory"),
    {documentId:"direct-sprite"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) => operation.arguments), [{
    character:"yoshino", sprite:"Assets/Characters/yoshino.png", position:"2", y:"24", scale:"0.65",
  }]);
  assert.deepEqual(compiled.resourceDependencies, ["Assets/Characters/yoshino.png"]);
});

test("effect and camera commands require presets and lower to explicit RuntimeIR kinds", () => {
  const compiled = compileStory(
    parseStory("[effect fade]\n[camera pan]\n", "Story/effects.pxstory"),
    {documentId:"effects"},
  );
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.runtimeIr?.operations.map((operation) =>
    [operation.kind, operation.arguments.value]), [
    ["effect", "fade"],
    ["camera", "pan"],
  ]);

  const missing = compileStory(
    parseStory("[effect]\n[camera]\n", "Story/missing-effects.pxstory"),
    {documentId:"missing-effects"},
  );
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((item) => item.code === "PXSTORY1126"));
  assert.ok(missing.diagnostics.some((item) => item.code === "PXSTORY1127"));
});

test("Story extension commands accept structured JSON literals and preserve typed payloads", () => {
  const parsed = parseStory(
    `[stage.compose offset=[100, 200] bounds=[0,0,1280,720] tint=[255,128,64,255] tags=["night", "rain"] metadata={"weather":"rain", "strength":0.8} asset={"path":"Assets/CG/rain.webp"}]\n`,
    "Story/structured-extension.pxstory",
  );
  assert.equal(parsed.diagnostics.length, 0, JSON.stringify(parsed.diagnostics));
  const command = parsed.nodes.find((node) => node.kind === "command");
  assert.deepEqual(command?.arguments?.find((argument) => argument.name === "offset")?.value, [100, 200]);
  assert.deepEqual(command?.arguments?.find((argument) => argument.name === "metadata")?.value, {weather:"rain",strength:0.8});

  const compiled = compileStory(parsed, {documentId:"structured-extension",extensions:[extension]});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const operation = compiled.runtimeIr?.operations.find((item) => item.kind === "customNode");
  assert.ok(operation);
  const payload = operation.arguments.value;
  if (typeof payload !== "string") {
    assert.fail("expected customNode payload to be a JSON string");
  }
  assert.deepEqual(JSON.parse(payload), {
    asset:{path:"Assets/CG/rain.webp"},
    bounds:[0,0,1280,720],
    metadata:{strength:0.8,weather:"rain"},
    offset:[100,200],
    tags:["night","rain"],
    tint:[255,128,64,255],
  });
});

test("Story extension structured literals fail closed for malformed JSON and typed shapes", () => {
  const malformed = parseStory(`[stage.compose offset=[100,] bounds=[0,0,1,1] tint=[0,0,0,255] tags=[] metadata={} asset="x"]\n`, "Story/malformed-json.pxstory");
  assert.ok(malformed.diagnostics.some((item) => item.code === "PXSTORY1014"));
  assert.equal(compileStory(malformed, {documentId:"malformed",extensions:[extension]}).valid, false);

  const wrongShape = compileStory(
    parseStory(`[stage.compose offset=[1,2,3] bounds=[0,0,1,1] tint=[300,0,0,255] tags={} metadata=[] asset=1]\n`, "Story/wrong-shape.pxstory"),
    {documentId:"wrong-shape",extensions:[extension]},
  );
  assert.equal(wrongShape.valid, false);
  assert.ok(wrongShape.diagnostics.filter((item) => item.code === "PXSTORY1202").length >= 5);
});

test("Story compiler fails closed for unresolved flow and extension contracts", () => {
  const parsed = parseStory(`*start\n[jump missing]\n[weather type=hail strength=2]\n[removed value=1]\n`, "Story/bad.pxstory");
  const compiled = compileStory(parsed, {documentId:"bad",extensions:[extension]});
  assert.equal(compiled.valid, false);
  assert.equal(compiled.runtimeIr, undefined);
  assert.ok(compiled.diagnostics.some((item) => item.code === "PXSTORY1118"));
  assert.ok(compiled.diagnostics.some((item) => item.code === "PXSTORY1203"));
  assert.ok(compiled.diagnostics.some((item) => item.code === "PXSTORY1205"));
  assert.ok(compiled.diagnostics.some((item) => item.code === "PXSTORY1199"));
});

test("authored Story ids survive prose edits, insertions, and source path changes", () => {
  const compile = (text: string, path: string) => compileStory(parseStory(text, path), {
    documentId: "commercial.prologue",
  });
  const first = compile(
    "Fallback line.\n[id opening.memory]\nOld localized text.\n",
    "Story/zh-TW/old.pxstory",
  );
  const second = compile(
    "Another fallback.\nInserted line.\n[id opening.memory]\n完全不同的翻譯。\n",
    "Story/zh-TW/renamed.pxstory",
  );
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  assert.equal(second.valid, true, JSON.stringify(second.diagnostics));
  const firstPinned = first.runtimeIr!.operations.at(-1)!;
  const secondPinned = second.runtimeIr!.operations.at(-1)!;
  assert.equal(firstPinned.sourceId, secondPinned.sourceId);
  assert.equal(firstPinned.operationId, secondPinned.operationId);
});

test("authored Story ids fail closed when duplicated or dangling", () => {
  const compiled = compileStory(parseStory(
    "[id duplicated]\nOne.\n[id duplicated]\nTwo.\n[id dangling]\n",
    "Story/invalid-identity.pxstory",
  ), {documentId: "identity-test"});
  assert.equal(compiled.valid, false);
  assert.ok(compiled.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1212"));
  assert.ok(compiled.diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1213"));
});

test("parser recovers after malformed commands", () => {
  const parsed = parseStory(`[bg "unterminated]\nNarration survives.\n*end\n`, "Story/recovery.pxstory");
  assert.equal(parsed.diagnostics.length, 1);
  assert.equal(parsed.nodes.some((node) => node.kind === "narration" && node.text === "Narration survives."), true);
  assert.equal(parsed.nodes.some((node) => node.kind === "label" && node.name === "end"), true);
});

test("Story compiler golden output remains cross-language stable", () => {
  const logicalPath = "Contracts/fixtures/story-compiler-golden.pxstory";
  const fixture = (name: string): string => readFileSync(new URL(`../../../../Contracts/fixtures/${name}`, import.meta.url), "utf8");
  const compiled = compileStory(parseStory(fixture("story-compiler-golden.pxstory"), logicalPath), {documentId:"contract.story-golden",committedRevision:1});
  assert.equal(compiled.valid, true);
  assert.equal(canonicalJson(compiled.runtimeIr!), fixture("story-compiler-golden.pxir"));
  assert.equal(canonicalJson(compiled.sourceMap!), fixture("story-compiler-golden.pxmap"));
});
