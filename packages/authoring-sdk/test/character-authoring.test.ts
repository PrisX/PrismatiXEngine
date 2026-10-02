import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, normalizeProject, validateDocument,
  type CharacterSourceDocument} from "../src/index.js";

const characterPath = "Characters/yuki.pxcharacter";
const imagePath = "Assets/yuki-smile.png";
const uuid = /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/u;

function compactCharacter() {
  return {format: "PrismatiXCharacter", schemaRevision: 1,
    displayName: "Yuki", aliases: ["yuki"],
    expressions: [{name: "Smile", aliases: ["smile"], assetPath: imagePath}]} satisfies CharacterSourceDocument;
}

test("path-first character expressions lower to canonical ids and an owned asset", () => {
  const author = compactCharacter();
  const input = {project: {id: "path-first-game", name: "Path First",
    characters: [{source: characterPath, displayName: "Yuki", alias: "yuki"}]},
    documents: {[characterPath]: author},
    sourceFiles: {"Story/main.pxstory": "*start\n[show yuki expression=smile]\n@yuki\nHello.\n[end]\n"},
    resourceExists: (path: string) => path === imagePath};
  const first = compileProject(input);
  const second = compileProject(input);
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  assert.equal(second.valid, true, JSON.stringify(second.diagnostics));
  const artifact = first.artifact!;
  const descriptor = artifact.project.characters![0]!;
  const asset = artifact.project.assets![0] as {id: string; source: string; kind: string};
  const character = JSON.parse(artifact.files[characterPath]!) as {
    id: string; defaultExpressionId: string;
    expressions: Array<{id: string; assetId: string; assetPath?: string}>;
  };
  assert.match(descriptor.id, uuid);
  assert.equal(character.id, descriptor.id);
  assert.match(character.expressions[0]!.id, uuid);
  assert.equal(character.defaultExpressionId, character.expressions[0]!.id);
  assert.equal(character.expressions[0]!.assetId, asset.id);
  assert.equal(character.expressions[0]!.assetPath, undefined);
  assert.deepEqual([asset.source, asset.kind], [imagePath, "character"]);
  assert.equal(validateDocument("project", artifact.project).valid, true);
  assert.equal(validateDocument("character", character).valid, true);
  assert.ok(artifact.dependencies.some((dependency) => dependency.source === imagePath));
  assert.equal(artifact.runtimeIr.operations.find((operation) =>
    operation.kind === "showCharacter")?.arguments.sprite, `asset:${asset.id}`);
  assert.equal(second.artifact!.files[characterPath], artifact.files[characterPath]);
  assert.equal(second.artifact!.project.characters![0]!.id, descriptor.id);
  assert.equal((author.expressions[0] as {assetId?: string}).assetId, undefined);
});

test("path shorthand reuses explicit catalog identities and preserves pinned ids", () => {
  const characterId = "11111111-1111-4111-8111-111111111111";
  const expressionId = "22222222-2222-4222-8222-222222222222";
  const assetId = "33333333-3333-4333-8333-333333333333";
  const normalized = normalizeProject({project: {id: "pinned-game", name: "Pinned",
    assets: [{id: assetId, kind: "character", source: imagePath}],
    characters: [{id: characterId, displayName: "Yuki", source: characterPath}]},
    documents: {[characterPath]: {format: "PrismatiXCharacter", schemaRevision: 1,
      id: characterId, displayName: "Yuki", aliases: ["yuki"],
      defaultExpressionId: "smile",
      expressions: [{id: expressionId, name: "Smile", aliases: ["smile"],
        assetPath: imagePath}]}}});
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.equal(normalized.value!.project.assets!.length, 1);
  const character = normalized.value!.documents[characterPath] as {
    defaultExpressionId: string;
    expressions: Array<{id: string; assetId: string; assetPath?: string}>;
  };
  assert.equal(character.defaultExpressionId, expressionId);
  assert.deepEqual(character.expressions[0], {id: expressionId, name: "Smile",
    aliases: ["smile"], assetId});
});

test("assetPath may pin a stable assetId without a handwritten catalog entry", () => {
  const assetId = "33333333-3333-4333-8333-333333333333";
  const normalized = normalizeProject({project: {id: "pinned-asset", name: "Pinned Asset",
    characters: [{source: characterPath, displayName: "Yuki"}]},
    documents: {[characterPath]: {format: "PrismatiXCharacter", schemaRevision: 1,
      displayName: "Yuki", aliases: ["yuki"],
      expressions: [{name: "Smile", aliases: ["smile"],
        assetId, assetPath: imagePath}]}}});
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.deepEqual(normalized.value!.project.assets, [{id: assetId,
    kind: "character", source: imagePath}]);
});

test("path-first expressions fail clearly for unsafe or conflicting assets", () => {
  for (const [expression, code] of [
    [{name: "Smile", aliases: ["smile"], assetPath: "../secret.png"}, "PXBUILD1067"],
  ] as const) {
    const result = normalizeProject({project: {id: "invalid-game", name: "Invalid",
      characters: [{source: characterPath, displayName: "Yuki"}]},
      documents: {[characterPath]: {format: "PrismatiXCharacter", schemaRevision: 1,
        displayName: "Yuki", aliases: ["yuki"], expressions: [expression]}}});
    assert.equal(result.valid, false);
    assert.ok(result.diagnostics.some((diagnostic) => diagnostic.code === code),
      JSON.stringify(result.diagnostics));
  }
  const conflict = normalizeProject({project: {id: "invalid-game", name: "Invalid",
    assets: [{id: "33333333-3333-4333-8333-333333333333", kind: "character",
      source: "Assets/other.png"}],
    characters: [{source: characterPath, displayName: "Yuki"}]},
    documents: {[characterPath]: {format: "PrismatiXCharacter", schemaRevision: 1,
      displayName: "Yuki", aliases: ["yuki"], expressions: [{name: "Smile",
        aliases: ["smile"], assetPath: imagePath,
        assetId: "33333333-3333-4333-8333-333333333333"}]}}});
  assert.equal(conflict.valid, false);
  assert.ok(conflict.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1068"));
});
