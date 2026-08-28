import assert from "node:assert/strict";
import test from "node:test";

import {validateProjectCharacterResources} from "../src/index.js";

const characterId = "11111111-1111-4111-8111-111111111111";
const expressionId = "22222222-2222-4222-8222-222222222222";
const spriteId = "33333333-3333-4333-8333-333333333333";
const source = `Characters/${characterId}.pxcharacter`;

function project() {
  return {
    format: "PrismatiXProject",
    schemaRevision: 1,
    assets: [
      {id: spriteId, kind: "character", source: "Assets/Characters/yuki-smile.webp"},
    ],
    characters: [
      {id: characterId, displayName: "Yuki", source},
    ],
  };
}

function character() {
  return {
    format: "PrismatiXCharacter",
    schemaRevision: 1,
    id: characterId,
    displayName: "Yuki",
    aliases: ["yuki"],
    defaultExpressionId: expressionId,
    expressions: [
      {id: expressionId, name: "Smile", aliases: ["smile"], assetId: spriteId},
    ],
  };
}

test("project-aware character validation accepts the Runtime-compatible graph", () => {
  const result = validateProjectCharacterResources(project(), {[source]: character()}, {
    assetExists: (path) => path === "Assets/Characters/yuki-smile.webp",
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
});

test("project-aware character validation rejects descriptor/document drift", () => {
  const authored = {...character(), displayName: "Snow"};
  const result = validateProjectCharacterResources(project(), {[source]: authored});
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKRES2004"));
});

test("project-aware character validation rejects missing or incompatible expression assets", () => {
  const missing = validateProjectCharacterResources(
    {...project(), assets: []},
    {[source]: character()},
  );
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((item) => item.code === "PXSDKRES2006"));

  const incompatible = validateProjectCharacterResources(
    {...project(), assets: [{id: spriteId, kind: "audio", source: "Assets/voice.ogg"}]},
    {[source]: character()},
  );
  assert.equal(incompatible.valid, false);
  assert.ok(incompatible.diagnostics.some((item) => item.code === "PXSDKRES2007"));
});

test("project-aware character validation can verify Runtime asset existence", () => {
  const result = validateProjectCharacterResources(project(), {[source]: character()}, {
    assetExists: () => false,
  });
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKRES2008"));
});

test("project-aware character validation rejects global identities and lookup collisions", () => {
  const secondId = "44444444-4444-4444-8444-444444444444";
  const secondSource = `Characters/${secondId}.pxcharacter`;
  const second = {
    ...character(),
    id: secondId,
    displayName: "Yuki 2",
    aliases: ["yuki"],
  };
  const root = {
    ...project(),
    characters: [
      {id: characterId, displayName: "Yuki", source},
      {id: secondId, displayName: "Yuki 2", source: secondSource},
    ],
  };
  const result = validateProjectCharacterResources(root, {
    [source]: character(),
    [secondSource]: second,
  });
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKRES2005"));
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKRES2009"));
});
