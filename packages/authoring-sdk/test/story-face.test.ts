import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, compileStory, createStoryPortraitIndex, parseStory} from "../src/index.js";
import type {CharacterDocument, LayeredPortraitDocument} from "../src/index.js";

function uuid(n: number): string {
  return `00000000-0000-4000-8000-${String(n).padStart(12, "0")}`;
}

function portrait(body: string, face: string): LayeredPortraitDocument {
  return {format: "PrismatiXLayeredPortrait", schemaRevision: 2, width: 1920, height: 1080, layers: [
    {path: `Assets/fg/aix/${body}.png`, x: 517, y: 151, width: 952, height: 1764, opacity: 255, role: "base"},
    {path: `Assets/fg/aix/${face}.png`, x: 818, y: 240, width: 224, height: 204, opacity: 255, role: "expression"},
  ]};
}

// 藍: bodies z1e0000 and z1e0700, faces e0022 and e0023 (not every pair).
const combos = [["z1e0000", "e0022"], ["z1e0000", "e0023"], ["z1e0700", "e0022"], ["z1e0700", "e0023"],
  ["z2e0100", "e0022"]] as const;
const aix: CharacterDocument = {
  format: "PrismatiXCharacter", schemaRevision: 2, id: uuid(1), displayName: "藍", aliases: ["aix"],
  defaultExpressionId: uuid(100),
  expressions: combos.map(([body, face], index) => ({id: uuid(100 + index), name: `藍 ${body}_${face}`,
    aliases: [`${body}_${face}`], assetId: uuid(200 + index)})),
};
const resourceAssets = combos.map(([body, face], index) => ({id: uuid(200 + index),
  source: `Assets/Portraits/aix/${body}_${face}.pxportrait`, kind: "character"}));
const portraits = createStoryPortraitIndex(Object.fromEntries(combos.map(([body, face]) =>
  [`Assets/Portraits/aix/${body}_${face}.pxportrait`, portrait(body, face)])));

function compile(source: string, withPortraits = true) {
  return compileStory(parseStory(source, "Story/face.pxstory"), {documentId: "face",
    characters: [aix], resourceAssets, ...(withPortraits ? {portraits} : {})});
}

test("[face] lists, per body, the expression that shows the face", () => {
  const result = compile("[show 藍 z1e0700_e0022 center]\n[face 藍 e0023]\n[face aix z1e0000_e0022 transition=false]\n");
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const [, byStem, byExpression] = result.runtimeIr!.operations;
  assert.equal(byStem!.kind, "showCharacter");
  assert.equal(byStem!.arguments.character, aix.id);
  assert.deepEqual(JSON.parse(byStem!.arguments.faces!), [
    {base: "Assets/fg/aix/z1e0000.png", sprite: `asset:${uuid(201)}`, expression: uuid(101)},
    {base: "Assets/fg/aix/z1e0700.png", sprite: `asset:${uuid(203)}`, expression: uuid(103)},
  ]);
  // Naming an expression uses its face on whatever body is on stage.
  assert.deepEqual(JSON.parse(byExpression!.arguments.faces!).map((entry: {sprite: string}) => entry.sprite),
    [`asset:${uuid(200)}`, `asset:${uuid(202)}`, `asset:${uuid(204)}`]);
  assert.equal(byExpression!.arguments.transition, "false");
  assert.ok(result.resourceDependencies!.includes("Assets/Portraits/aix/z2e0100_e0022.pxportrait"));
});

test("[face] mistakes are reported", () => {
  const codes = (source: string, withPortraits = true) =>
    compile(`${source}\n`, withPortraits).diagnostics.map(({code}) => code);
  assert.deepEqual(codes("[face 藍]"), ["PXSTORY1260"]);
  assert.deepEqual(codes("[face nobody e0023]"), ["PXSTORY1113"]);
  assert.deepEqual(codes("[face 藍 e9999]"), ["PXSTORY1262"]);
  assert.deepEqual(codes("[face 藍 e0023 transition=slow]"), ["PXSTORY1145"]);
  assert.deepEqual(codes("[face 藍 e0023]", false), ["PXSTORY1261"]);
});

test("project builds give [face] the character expression portraits", () => {
  const sourceFiles: Record<string, string> = {"Story/main.pxstory":
    "[show aix z1e0000_e0022 center]\n[face aix e0023]\nこんにちは。\n"};
  const read: string[] = [];
  const project = {name: "Face", assets: resourceAssets, characters: [{alias: "aix",
    displayName: "藍", source: "Characters/aix.pxcharacter"}]};
  const input = {project, documents: {"Characters/aix.pxcharacter": aix}, sourceFiles,
    resourceText: (path: string) => {
      read.push(path);
      const match = combos.find(([body, face]) => path.endsWith(`${body}_${face}.pxportrait`));
      return match === undefined ? undefined : JSON.stringify(portrait(match[0], match[1]));
    }};
  const compiled = compileProject(input);
  assert.equal(compiled.valid, true,
    JSON.stringify(compiled.diagnostics));
  const face = compiled.artifact?.runtimeIr.operations.find(({arguments: args}) => args.faces !== undefined);
  assert.equal(JSON.parse(face!.arguments.faces!).length, 2);
  assert.ok(read.includes("Assets/Portraits/aix/z1e0700_e0023.pxportrait"));
  const plain = compileProject({...input, sourceFiles: {"Story/main.pxstory": "[show aix z1e0000_e0022]\n"}});
  assert.ok(plain.valid, JSON.stringify(plain.diagnostics));
});
