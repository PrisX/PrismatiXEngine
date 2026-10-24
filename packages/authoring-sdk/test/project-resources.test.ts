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
    id: "project-resources",
    name: "Project Resources",
    version: "0.2.0",
    contentVersion: "test-v1",
    saveVersion: 1,
    resolution: {width: 1280, height: 720},
    entry: {story: "scene01", ui: "title"},
    defaultLocale: "en-US",
    supportedLocales: ["en-US"],
    storyIndex: "Story/story.pxindex",
    gameCatalog: "Content/game.pxgame",
    extensions: [],
    uiEntryPoints: {title: "Content/UI/Title.pxui"},
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
  const result = validateProjectCharacterResources(project(), {[source]: authored}, {
    characterSources: {[source]: JSON.stringify(authored)},
  });
  assert.equal(result.valid, false);
  const wrongName = result.diagnostics.find((item) => item.code === "PXSDKRES2004");
  assert.equal(wrongName?.span?.path, source);
  assert.equal(JSON.stringify(authored).slice(wrongName.span.start.offset,
    wrongName.span.end.offset), '"Snow"');
});

test("project-aware character validation rejects missing or incompatible expression assets", () => {
  const missing = validateProjectCharacterResources(
    {...project(), assets: []},
    {[source]: character()},
    {characterSources: {[source]: JSON.stringify(character())}},
  );
  assert.equal(missing.valid, false);
  assert.equal(missing.diagnostics.find((item) => item.code === "PXSDKRES2006")?.span?.path,
    source);

  const incompatibleProject = {...project(),
    assets: [{id: spriteId, kind: "audio", source: "Assets/voice.ogg"}]};
  const incompatible = validateProjectCharacterResources(
    incompatibleProject,
    {[source]: character()},
    {projectSource: {path: "prismatix.json", text: JSON.stringify(incompatibleProject)}},
  );
  assert.equal(incompatible.valid, false);
  assert.equal(incompatible.diagnostics.find((item) => item.code === "PXSDKRES2007")?.span?.path,
    "prismatix.json");
});

test("project-aware character validation can verify Runtime asset existence", () => {
  const authoredProject = project();
  const result = validateProjectCharacterResources(authoredProject, {[source]: character()}, {
    assetExists: () => false,
    projectSource: {path: "prismatix.json", text: JSON.stringify(authoredProject)},
  });
  assert.equal(result.valid, false);
  assert.equal(result.diagnostics.find((item) => item.code === "PXSDKRES2008")?.span?.path,
    "prismatix.json");
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
  }, {characterSources: {[source]: JSON.stringify(character()),
    [secondSource]: JSON.stringify(second)}});
  assert.equal(result.valid, false);
  assert.equal(result.diagnostics.find((item) => item.code === "PXSDKRES2005")?.span?.path,
    secondSource);
  assert.equal(result.diagnostics.find((item) => item.code === "PXSDKRES2009")?.span?.path,
    secondSource);
});

test("character nameStyle colours are checked and its font and plate must exist", () => {
  const styled = {...character(), nameColor: "#8FD6DC", nameStyle: {
    color: "#E0688AFF", gradient: ["#FFFFFF", "#FFC6D6", "horizontal"], outlineColor: "#4E3A52",
    outlineSize: 3, shadow: {color: "#00000080", x: 2, y: 2, blur: 2},
    font: "Content/Fonts/Name.ttf", bold: true, italic: 8, plate: "Content/UI/Plates/shirone.png"}};
  const present = new Set(["Assets/yuki.png", "Content/Fonts/Name.ttf", "Content/UI/Plates/shirone.png"]);
  const valid = validateProjectCharacterResources(project(), {[source]: styled},
    {assetExists: (path) => present.has(path) || !path.startsWith("Content/")});
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));

  const missing = validateProjectCharacterResources(project(), {[source]: styled}, {
    assetExists: (path) => !path.startsWith("Content/"),
    characterSources: {[source]: JSON.stringify(styled)},
  });
  assert.deepEqual(missing.diagnostics.map((item) => item.code), ["PXSDKRES2012", "PXSDKRES2013"]);
  const plate = missing.diagnostics[1]!;
  assert.equal(JSON.stringify(styled).slice(plate.span!.start.offset, plate.span!.end.offset),
    '"Content/UI/Plates/shirone.png"');

  for (const nameStyle of [{color: "pink"}, {gradient: ["#FFFFFF"]}, {gradient: ["#FFFFFF", "#000000", "diagonal"]},
    {outlineSize: 13}, {shadow: {x: 2}}, {shadow: {color: "#000000", blur: 9}}, {bold: 5}, {italic: 30},
    {plate: "../outside.png"}, {size: 3}]) {
    const result = validateProjectCharacterResources(project(), {[source]: {...character(), nameStyle}});
    assert.equal(result.valid, false, JSON.stringify(nameStyle));
  }
});
