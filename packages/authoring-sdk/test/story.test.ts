import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";

import {canonicalJson, compileStory, parseStory, validateDocument} from "../src/index.js";
import type {CharacterDocument, ExtensionManifest, GameDocument} from "../src/index.js";

const character: CharacterDocument = {
  format: "PrismatiXCharacter",
  schemaRevision: 1,
  id: "11111111-1111-4111-8111-111111111111",
  displayName: "雪",
  aliases: ["yuki"],
  defaultExpressionId: "22222222-2222-4222-8222-222222222222",
  expressions: [{id:"22222222-2222-4222-8222-222222222222",name:"Smile",aliases:["smile"],assetId:"33333333-3333-4333-8333-333333333333"}],
};
const game: GameDocument = {
  format: "PrismatiXGame",
  schemaRevision: 1,
  variables: [{name:"affection",type:"integer",default:0,persistent:true}],
};
const extension: ExtensionManifest = {
  format: "PrismatiXExtension",
  schemaRevision: 1,
  language: "javascript",
  id: "weather",
  version: "1.0.0",
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
[show yuki expression=smile position=center]

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
  const shown = compiled.runtimeIr!.operations.find((operation) => operation.kind === "showCharacter");
  assert.equal(shown?.arguments.character, character.id);
  assert.equal(shown?.arguments.expression, character.expressions[0]!.id);
  assert.equal(shown?.arguments.sprite, `asset:${character.expressions[0]!.assetId}`);
  assert.equal(compiled.sourceMap!.mappings.length, compiled.runtimeIr!.operations.length);
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
  assert.deepEqual(JSON.parse(operation.arguments.value), {
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
