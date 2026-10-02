import assert from "node:assert/strict";
import test from "node:test";

import {
  assignStableSourceIds,
  compileProject,
  normalizeProject,
  stableSourceIdReport,
  validateDocument,
} from "../src/index.js";

const assetId = "33333333-3333-4333-8333-333333333333";
const assetPath = "Assets/BG/room.webp";

test("single-locale normalization compiles Story/main.pxstory without support documents", () => {
  const input = {
    project: {id: "first-game", name: "First Game", defaultLocale: "zh-TW"},
    sourceFiles: {"Story/main.pxstory": "Hello.\n[end]\n"},
  };
  const normalized = normalizeProject(input);
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.deepEqual(normalized.value?.project.supportedLocales, ["zh-TW"]);
  assert.equal(normalized.value?.storyPath, "Story/main.pxstory");
  assert.equal(validateDocument("project", normalized.value?.project).valid, true);
  assert.equal(validateDocument("storyIndex",
    normalized.value?.documents["Story/story.pxindex"]).valid, true);
  assert.equal(validateDocument("game",
    normalized.value?.documents["Content/game.pxgame"]).valid, true);
  assert.equal(validateDocument("ui",
    normalized.value?.documents["Content/UI/Title.pxui"]).valid, true);
  assert.ok((normalized.value?.documents["Content/UI/Title.pxui"] as {
    nodes: readonly {onClick?: {id: string}}[];
  }).nodes.some((node) => node.onClick?.id === "game.start"));
  assert.equal(validateDocument("locale",
    normalized.value?.locales["zh-TW"]).valid, true);

  const compiled = compileProject(input);
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.equal(compiled.artifact?.project.defaultLocale, "zh-TW");
  assert.deepEqual(JSON.parse(compiled.artifact!.files["project.pxproject"]!).assets, []);
  assert.ok(compiled.artifact?.files["Story/story.pxindex"]?.includes(
    "Story/main.pxstory"));
  assert.ok(compiled.artifact?.files["Runtime/main.pxir"]);
});

test("Gallery references become native project asset descriptors", () => {
  const image = {id: "44444444-4444-4444-8444-444444444444",
    path: "Assets/CG/scene.png", kind: "cg"};
  const thumbnail = {id: "55555555-5555-4555-8555-555555555555",
    path: "Assets/CG/scene-thumb.png", kind: "cg"};
  const game = {format: "PrismatiXGame", schemaRevision: 1,
    variables: [], gallery: [{id: "scene", title: "Scene", image, thumbnail,
      unlockFlag: "scene"}], unlockables: []};
  const input = {project: {id: "gallery-game"},
    documents: {"Content/game.pxgame": game},
    sourceFiles: {"Story/main.pxstory": "[unlock kind=cg id=scene]\n[end]\n"},
    resourceExists: () => true};
  const compiled = compileProject(input);
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const runtimeProject = JSON.parse(compiled.artifact!.files["project.pxproject"]!);
  assert.deepEqual(runtimeProject.assets, [
    {id: image.id, source: image.path, kind: "cg"},
    {id: thumbnail.id, source: thumbnail.path, kind: "cg"},
  ]);
  assert.ok(compiled.artifact?.dependencies.some((item) =>
    item.source === image.path));

  const conflict = normalizeProject({...input, project: {id: "gallery-game",
    assets: [{id: image.id, source: "Assets/CG/other.png", kind: "cg"}]}});
  assert.ok(conflict.diagnostics.some((item) => item.code === "PXBUILD1071"));
});

test("Player UI data is validated and preserved in the canonical project", () => {
  const rows = [{key: "confirm-0", rect: {x: 20, y: 30, width: 388, height: 35}}];
  const project = {id: "placed-ui", settings: {"player.uiData": {
    "settings.confirmations": rows,
  }}};
  const normalized = normalizeProject({project});
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.deepEqual((normalized.value?.project.settings?.["player.uiData"] as
    Record<string, unknown>)["settings.confirmations"], rows);
  const compiled = compileProject({project,
    sourceFiles: {"Story/main.pxstory": "Hello.\n[end]\n"}});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(JSON.parse(compiled.artifact!.files["project.pxproject"]!).settings[
    "player.uiData"]["settings.confirmations"], rows);

  const invalidPath = normalizeProject({project: {id: "bad-ui-data", settings: {
    "player.uiData": {"settings..rows": rows},
  }}});
  assert.equal(invalidPath.valid, false);
  let tooDeep: unknown = "leaf";
  for (let depth = 0; depth < 34; depth += 1) tooDeep = [tooDeep];
  const invalidDepth = normalizeProject({project: {id: "deep-ui-data", settings: {
    "player.uiData": {"settings.rows": tooDeep},
  }}});
  assert.equal(invalidDepth.valid, false);
  assert.ok(invalidDepth.diagnostics.some((value) => value.code === "PXBUILD1059"));
});

test("resourceIncludes add dynamic media without leaking author roots into runtime project", () => {
  const input = {
    project: {id: "dynamic-media", resourceIncludes: ["Assets/UI/Original", "Assets/Audio/pick.ogg"]},
    sourceFiles: {"Story/main.pxstory": "Hello.\n[end]\n"},
  };
  const normalized = normalizeProject(input);
  assert.equal(normalized.valid, true, JSON.stringify(normalized.diagnostics));
  assert.deepEqual(normalized.value?.resourceIncludes, input.project.resourceIncludes);
  assert.equal("resourceIncludes" in normalized.value!.project, false);
  const files: Readonly<Record<string, readonly string[]>> = {
    "Assets/UI/Original": ["Assets/UI/Original/a.png", "Assets/UI/Original/sub/b.png"],
    "Assets/Audio/pick.ogg": ["Assets/Audio/pick.ogg"],
  };
  const compiled = compileProject({...input,
    resolveResourceInclude: (root) => files[root], resourceExists: () => true});
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  assert.deepEqual(compiled.artifact!.dependencies.map((value) => value.logicalPath),
    ["Assets/Audio/pick.ogg", "Assets/UI/Original/a.png", "Assets/UI/Original/sub/b.png"]);
  assert.doesNotMatch(compiled.artifact!.files["project.pxproject"]!, /resourceIncludes/u);

  const unresolved = compileProject(input);
  assert.ok(unresolved.diagnostics.some((value) => value.code === "PXBUILD1052"));
  const escaped = compileProject({...input,
    resolveResourceInclude: () => ["Assets/elsewhere.png"]});
  assert.ok(escaped.diagnostics.some((value) => value.code === "PXBUILD1053"));
  const malformed = normalizeProject({...input,
    project: {...input.project, resourceIncludes: ["Assets/../outside"]}});
  assert.ok(malformed.diagnostics.some((value) => value.code === "PXBUILD1051"));
  const collision = compileProject({...input,
    project: {...input.project, resourceIncludes: ["Story/main.pxstory"]},
    resolveResourceInclude: () => ["Story/main.pxstory"]});
  assert.ok(collision.diagnostics.some((value) => value.code === "PXBUILD1055"));
  const authoredSource = compileProject({...input,
    project: {...input.project, resourceIncludes: ["Assets/UI"]},
    resolveResourceInclude: () => ["Assets/UI/Save.tsx"]});
  assert.ok(authoredSource.diagnostics.some((value) => value.code === "PXBUILD1055"));
  const canonicalCollision = compileProject({...input,
    project: {...input.project, resourceIncludes: ["Content/Localization/en-US.json"]},
    resolveResourceInclude: () => ["Content/Localization/en-US.json"]});
  assert.ok(canonicalCollision.diagnostics.some((value) => value.code === "PXBUILD1054"));
});

test("path-first project compilation resolves all resource spellings and collects inputs", () => {
  const story = [
    `[bg asset=${assetPath}]`,
    `[bg asset=${assetId}]`,
    `[bg asset=asset:${assetId.toUpperCase()}]`,
    "[se asset=Assets/Audio/chime.wav]",
    "[end]",
    "",
  ].join("\n");
  const input = {
    project: {
      id: "resources", name: "Resources",
      assets: [{id: assetId, kind: "background", source: assetPath}],
    },
    sourceFiles: {"Story/main.pxstory": story},
  };
  const compiled = compileProject(input);
  assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));
  const backgrounds = compiled.artifact?.runtimeIr.operations
    .filter((operation) => operation.kind === "background") ?? [];
  assert.deepEqual(backgrounds.map((operation) => operation.arguments.asset), [
    assetPath,
    `asset:${assetId}`,
    `asset:${assetId}`,
  ]);
  assert.deepEqual(compiled.artifact?.dependencies.map((dependency) => ({
    logicalPath: dependency.logicalPath,
    source: dependency.source,
  })), [
    {logicalPath: "Assets/Audio/chime.wav", source: "Assets/Audio/chime.wav"},
    {logicalPath: assetPath, source: assetPath},
  ]);

  const missing = compileProject({...input,
    resourceExists: (path: string) => path !== "Assets/Audio/chime.wav"});
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((item) => item.code === "PXBUILD1044" &&
    item.path === "Assets/Audio/chime.wav"));
});

test("compileProject runs the production character graph validator on readable paths", () => {
  const characterId = "11111111-1111-4111-8111-111111111111";
  const expressionId = "22222222-2222-4222-8222-222222222222";
  const spriteId = "44444444-4444-4444-8444-444444444444";
  const source = "Characters/yuki.pxcharacter";
  const descriptor = {id: characterId, displayName: "Yuki", source};
  const character = {
    format: "PrismatiXCharacter", schemaRevision: 1,
    id: characterId, displayName: "Yuki", aliases: ["yuki"],
    defaultExpressionId: expressionId,
    expressions: [{id: expressionId, name: "Smile", aliases: ["smile"],
      assetId: spriteId}],
  };
  const base = {
    project: {
      id: "characters", name: "Characters",
      characters: [descriptor],
      assets: [{id: spriteId, kind: "character",
        source: "Assets/Characters/yuki-smile.webp"}],
    },
    documents: {[source]: character},
    sourceFiles: {"Story/main.pxstory": "[end]\n"},
  };
  const valid = compileProject(base);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.ok(valid.artifact?.files[source]?.includes("PrismatiXCharacter"));

  const drift = compileProject({...base, documents: {
    [source]: {...character, id: "55555555-5555-4555-8555-555555555555"},
  }});
  assert.equal(drift.valid, false);
  assert.ok(drift.diagnostics.some((item) => item.code === "PXSDKRES2003"));
});

test("stable source id assignment is idempotent and reports durable anchors", () => {
  const source = "[id opening]\n*start\n@hero\nHello.\n[end]\n";
  const before = stableSourceIdReport(source, {
    path: "Story/main.pxstory", documentId: "first-game:main",
  });
  assert.equal(before.valid, true, JSON.stringify(before.diagnostics));
  assert.equal(before.total, 3);
  assert.equal(before.anchored, 1);
  assert.equal(before.fallback, 2);

  const assigned = assignStableSourceIds(source, {
    path: "Story/main.pxstory", documentId: "first-game:main",
  });
  assert.equal(assigned.valid, true, JSON.stringify(assigned.diagnostics));
  assert.equal(assigned.assigned, 2);
  assert.equal(assigned.report.anchored, assigned.report.total);
  assert.ok(assigned.source);

  const second = assignStableSourceIds(assigned.source!, {
    path: "Story/main.pxstory", documentId: "first-game:main",
  });
  assert.equal(second.assigned, 0);
  assert.equal(second.source, assigned.source);

  const edited = stableSourceIdReport(assigned.source!.replace("Hello.", "Changed prose."), {
    path: "Story/renamed.pxstory", documentId: "first-game:main",
  });
  const originalDialogue = assigned.report.entries.find((entry) =>
    entry.kind === "dialogue")?.sourceId;
  const editedDialogue = edited.entries.find((entry) =>
    entry.kind === "dialogue")?.sourceId;
  assert.equal(editedDialogue, originalDialogue);
});
