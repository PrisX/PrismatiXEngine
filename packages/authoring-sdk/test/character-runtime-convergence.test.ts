import assert from "node:assert/strict";
import test from "node:test";

import {validateDocument} from "../src/index.js";
import type {JsonValue} from "../src/index.js";

test("project character descriptors enforce the Runtime loader contract", () => {
  const id = "11111111-1111-4111-8111-111111111111";
  const base = {
    format:"PrismatiXProject",
    schemaRevision:2,
    id:"character-convergence",name:"Character Convergence",version:"0.2.0",
    contentVersion:"test-v1",saveVersion:1,
    resolution:{width:1280,height:720},entry:{story:"scene01",ui:"title"},
    defaultLocale:"en-US",supportedLocales:["en-US"],
    storyIndex:"Story/story.pxindex",gameCatalog:"Content/game.pxgame",
    extensions:[],uiEntryPoints:{title:"Content/UI/Title.pxui"},
    assets:[],
    characters:[{id,displayName:"Yuki",source:`Characters/${id}.pxcharacter`}],
  } satisfies JsonValue;
  assert.equal(validateDocument("project", base).valid, true);

  const missingName = {
    ...base,
    characters:[{id,source:`Characters/${id}.pxcharacter`}],
  } satisfies JsonValue;
  const missingNameResult = validateDocument("project", missingName);
  assert.equal(missingNameResult.valid, false);
  assert.ok(missingNameResult.diagnostics.some((item) => item.code === "PXSDKSEM1111"));

  const wrongSource = {
    ...base,
    characters:[{id,displayName:"Yuki",source:"Characters/yuki.pxcharacter"}],
  } satisfies JsonValue;
  const wrongSourceResult = validateDocument("project", wrongSource);
  assert.equal(wrongSourceResult.valid, false);
  assert.ok(wrongSourceResult.diagnostics.some((item) => item.code === "PXSDKSEM1112"));

  const duplicateSource = {
    ...base,
    characters:[
      {id,displayName:"Yuki",source:`Characters/${id}.pxcharacter`},
      {id:"22222222-2222-4222-8222-222222222222",displayName:"Yuki 2",source:`characters/${id}.pxcharacter`},
    ],
  } satisfies JsonValue;
  const duplicateSourceResult = validateDocument("project", duplicateSource);
  assert.equal(duplicateSourceResult.valid, false);
  assert.ok(duplicateSourceResult.diagnostics.some((item) => item.code === "PXSDKSEM1110"));
});

test("character semantics reject documents the Runtime loader would reject", () => {
  const characterId = "11111111-1111-4111-8111-111111111111";
  const expressionId = "22222222-2222-4222-8222-222222222222";
  const assetId = "33333333-3333-4333-8333-333333333333";
  const valid = {
    format:"PrismatiXCharacter",
    schemaRevision:2,
    id:characterId,
    displayName:"Yuki",
    aliases:["yuki"],
    defaultExpressionId:expressionId,
    expressions:[{id:expressionId,name:"Smile",aliases:["smile"],assetId}],
  } satisfies JsonValue;
  assert.equal(validateDocument("character", valid).valid, true);

  const missingDefault = {...valid, defaultExpressionId:null} satisfies JsonValue;
  const missingDefaultResult = validateDocument("character", missingDefault);
  assert.equal(missingDefaultResult.valid, false);
  assert.ok(missingDefaultResult.diagnostics.some((item) => item.code === "PXSDKSEM1118"));

  const duplicateNames = {
    ...valid,
    expressions:[
      {id:expressionId,name:"Smile",aliases:["smile"],assetId},
      {id:"44444444-4444-4444-8444-444444444444",name:"smile",aliases:["happy"],assetId},
    ],
  } satisfies JsonValue;
  const duplicateNamesResult = validateDocument("character", duplicateNames);
  assert.equal(duplicateNamesResult.valid, false);
  assert.ok(duplicateNamesResult.diagnostics.some((item) => item.code === "PXSDKSEM1114"));

  const ambiguousCharacterLookup = {...valid, aliases:[characterId]} satisfies JsonValue;
  const ambiguousCharacterLookupResult = validateDocument("character", ambiguousCharacterLookup);
  assert.equal(ambiguousCharacterLookupResult.valid, false);
  assert.ok(ambiguousCharacterLookupResult.diagnostics.some((item) => item.code === "PXSDKSEM1116"));

  const ambiguousExpressionLookup = {
    ...valid,
    expressions:[{id:expressionId,name:"Smile",aliases:[expressionId],assetId}],
  } satisfies JsonValue;
  const ambiguousExpressionLookupResult = validateDocument("character", ambiguousExpressionLookup);
  assert.equal(ambiguousExpressionLookupResult.valid, false);
  assert.ok(ambiguousExpressionLookupResult.diagnostics.some((item) => item.code === "PXSDKSEM1117"));
});
