import assert from "node:assert/strict";
import test from "node:test";

import {compileProject} from "../src/index.js";

const rootId = "11111111-1111-4111-8111-111111111111";
const uiId = "22222222-2222-4222-8222-222222222222";
const project = {
  format: "PrismatiXProject",
  schemaRevision: 2,
  id: "commercial-sample",
  name: "Commercial Sample",
  version: "0.2.0",
  contentVersion: "chapter-1",
  saveVersion: 1,
  resolution: {width: 1280, height: 720},
  entry: {story: "scene01", ui: "title"},
  defaultLocale: "zh-TW",
  supportedLocales: ["zh-TW"],
  storyIndex: "Story/story.pxindex",
  gameCatalog: "Content/game.pxgame",
  extensions: [],
  uiEntryPoints: {title: "Content/UI/Title.pxui"},
  assets: [],
};
const storyIndex = {
  format: "PrismatiXStoryIndex",
  schemaRevision: 2,
  id: "main",
  entryScene: "scene01",
  chapters: [{id: "chapter01", title: "Chapter 1", scenes: ["scene01"]}],
  scenes: [{id: "scene01", sources: {"zh-TW": "Story/zh-TW/scene01.pxstory"}}],
};
const game = {
  format: "PrismatiXGame",
  schemaRevision: 2,
  variables: [
    {name: "route", type: "string", default: "common", scope: "session"},
    {name: "allClear", type: "boolean", default: false, scope: "profile"},
  ],
  gallery: [],
  unlockables: [],
};
const ui = {
  format: "PrismatiXUIScene",
  schemaRevision: 2,
  id: uiId,
  revision: 1,
  name: "Title",
  width: 1280,
  height: 720,
  rootId,
  nodes: [{
    id: rootId, parentId: null, order: 0, kind: "control", runtimeType: "Control",
    name: "Root", visible: true, locked: false,
    layout: {mode: "free", x: 0, y: 0, width: 1280, height: 720,
      anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0,
      pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed"},
    runtimeProperties: {}, bindings: {},
  }],
  theme: [],
  behaviorGraph: {nodes: [], links: [], groups: []},
  behaviorTriggers: [],
  visualStateGroups: [],
};
const {behaviorGraph: _behaviorGraph, behaviorTriggers: _behaviorTriggers,
  visualStateGroups: _visualStateGroups, ...componentContent} = ui;
const component = {...componentContent, format: "PrismatiXUIComponent",
  componentInterface: {properties: [], signals: [], slots: []}};

test("compileProject produces one deterministic RuntimeIR BuildArtifact", () => {
  const result = compileProject({
    project: {...project, uiComponents: [{id: uiId, name: "Panel", source: "Content/UI/Panel.pxuicomponent"}]},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      "Content/UI/Panel.pxuicomponent": component,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2, locale: "zh-TW", strings: {"ui.start": "開始"}}},
    committedRevision: 7,
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.ok(result.artifact);
  assert.equal(result.artifact.engineVersion, "0.2.0");
  assert.equal(result.artifact.runtimeIr.documentId, "commercial-sample");
  assert.equal(result.artifact.runtimeIr.committedRevision, 7);
  assert.ok(result.artifact.files["Runtime/main.pxir"]?.includes("PrismatiXRuntimeIR"));
  assert.ok(result.artifact.files["Runtime/main.pxmap"]?.includes("sourceUri"));
  assert.ok(result.artifact.files["Runtime/Locales/zh-TW/main.pxir"]?.includes("PrismatiXRuntimeIR"));
  assert.equal(result.artifact.localeArtifacts["zh-TW"]?.runtimeIrPath,
    "Runtime/Locales/zh-TW/main.pxir");
  assert.ok(result.artifact.files["Content/Localization/zh-TW.json"]?.includes('"strings"'));
  assert.ok(result.artifact.files["Content/UI/Panel.pxuicomponent"]?.includes("PrismatiXUIComponent"));
});

test("compileProject rejects UI component interfaces that the native Runtime cannot instantiate", () => {
  const source = "Content/UI/Panel.pxuicomponent";
  const exposedComponent = {
    ...component,
    nodes: [{...component.nodes[0], runtimeProperties: {markup: "Panel"}}],
    componentInterface: {
      properties: [{id: "caption", displayName: "Caption", nodeId: rootId,
        property: "runtimeProperties.markup", valueType: "string",
        defaultValue: "Panel"}],
      signals: [],
      slots: [],
    },
  };
  const compile = (candidate: object) => compileProject({
    project: {...project, uiComponents: [{id: uiId, name: "Panel", source}]},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      [source]: candidate,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
  });

  const valid = compile(exposedComponent);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));

  const missingPath = compile({...exposedComponent, componentInterface: {
    ...exposedComponent.componentInterface,
    properties: [{...exposedComponent.componentInterface.properties[0],
      property: "markup"}],
  }});
  assert.equal(missingPath.valid, false);
  assert.equal(missingPath.artifact, undefined);
  assert.ok(missingPath.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSDKUICOMP1105" && diagnostic.path === source));

  const wrongDefault = compile({...exposedComponent, componentInterface: {
    ...exposedComponent.componentInterface,
    properties: [{...exposedComponent.componentInterface.properties[0],
      defaultValue: 42}],
  }});
  assert.equal(wrongDefault.valid, false);
  assert.ok(wrongDefault.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSDKUICOMP1105"));
});

test("compileProject publishes aligned RuntimeIR and source maps for every Story locale", () => {
  const bilingualIndex = {
    ...storyIndex,
    scenes: [{id: "scene01", sources: {
      "zh-TW": "Story/zh-TW/scene01.pxstory",
      "ja-JP": "Story/ja-JP/scene01.pxstory",
    }}],
  };
  const compile = (japanese: string) => compileProject({
    project: {...project, supportedLocales: ["zh-TW", "ja-JP"]},
    documents: {
      "Story/story.pxindex": bilingualIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
    },
    sourceFiles: {
      "Story/zh-TW/scene01.pxstory": "*start\n@hero\n早安。\n[choice text=留下 goto=end]\n*end\n[end]\n",
      "Story/ja-JP/scene01.pxstory": japanese,
    },
    locales: {
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "zh-TW", strings: {}},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "ja-JP", strings: {}},
    },
  });

  const valid = compile("*start\n@hero\nおはよう。\n[choice text=残る goto=end]\n*end\n[end]\n");
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  const base = valid.artifact!.localeArtifacts["zh-TW"]!;
  const localized = valid.artifact!.localeArtifacts["ja-JP"]!;
  assert.deepEqual(localized.runtimeIr.operations.map((operation) => operation.operationId),
    base.runtimeIr.operations.map((operation) => operation.operationId));
  assert.deepEqual(localized.runtimeIr.operations.map((operation) => operation.sourceId),
    base.runtimeIr.operations.map((operation) => operation.sourceId));
  assert.notEqual(localized.runtimeIr.operations.find((operation) =>
    operation.kind === "dialogue")?.arguments.text,
  base.runtimeIr.operations.find((operation) => operation.kind === "dialogue")?.arguments.text);
  assert.ok(valid.artifact!.files["Runtime/Locales/ja-JP/main.pxmap"]?.includes(
    "Story/ja-JP/scene01.pxstory"));

  const structurallyDifferent = compile("*start\n@hero\nおはよう。\n[set route=branch]\n[choice text=残る goto=end]\n*end\n[end]\n");
  assert.equal(structurallyDifferent.valid, false);
  assert.ok(structurallyDifferent.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1030" || diagnostic.code === "PXBUILD1031"));
});

test("compileProject fails closed when cross-document identity is inconsistent", () => {
  const result = compileProject({
    project: {...project, entry: {story: "missing", ui: "title"}},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2, locale: "ja-JP", strings: {}}},
  });
  assert.equal(result.valid, false);
  assert.equal(result.artifact, undefined);
  assert.ok(result.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1005"));
  assert.ok(result.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1006"));
});

test("compileProject validates and publishes an explicit save migration chain", () => {
  const descriptor = {
    id: "chapter-0-to-1",
    from: {contentVersion: "chapter-0", saveVersion: 1},
    to: {contentVersion: "chapter-1", saveVersion: 1},
    asset: "Content/Migrations/chapter-0.pxsave-migration",
  };
  const migration = {
    format: "PrismatiXSaveMigration",
    schemaRevision: 2,
    id: descriptor.id,
    from: descriptor.from,
    to: descriptor.to,
    anchor: {policy: "preserve"},
    operations: [{op: "renameVariable", from: "oldRoute", to: "route"}],
  };
  const result = compileProject({
    project: {...project, saveMigrations: [descriptor]},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      [descriptor.asset]: migration,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2, locale: "zh-TW", strings: {}}},
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.ok(result.artifact?.files[descriptor.asset]?.includes("PrismatiXSaveMigration"));

  const broken = compileProject({
    project: {...project, saveMigrations: [{...descriptor, to: {contentVersion: "orphan", saveVersion: 1}}]},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      [descriptor.asset]: {...migration, to: {contentVersion: "orphan", saveVersion: 1}},
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2, locale: "zh-TW", strings: {}}},
  });
  assert.equal(broken.valid, false);
  assert.ok(broken.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1010"));
});

test("compileProject publishes only validated custom-effect source inputs", () => {
  const descriptor = {id: "dream-tone", source: "Effects/dream-tone.pxeffect"};
  const effect = {
    format: "PrismatiXEffect",
    schemaRevision: 2,
    id: descriptor.id,
    targetLayer: "stage",
    shader: "Effects/dream-tone.frag.hlsl",
    uniforms: [{name: "amount", type: "number", slot: 0, default: 0.5,
      minimum: 0, maximum: 1}],
  };
  const shader = "// Packager performs compilation, reflection, and fixed-binding validation.\n";
  const compile = (projectOverrides: object = {}, effectOverrides: object = {},
    sourceFiles: Readonly<Record<string, string>> = {
      "Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n",
      [effect.shader]: shader,
    }) => compileProject({
      project: {...project, graphicsTier: "gpu-effects", effects: [descriptor],
        ...projectOverrides},
      documents: {
        "Story/story.pxindex": storyIndex,
        "Content/game.pxgame": game,
        "Content/UI/Title.pxui": ui,
        [descriptor.source]: {...effect, ...effectOverrides},
      },
      sourceFiles,
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "zh-TW", strings: {}}},
    });

  const valid = compile();
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.ok(valid.artifact?.files[descriptor.source]?.includes("PrismatiXEffect"));
  assert.equal(valid.artifact?.files[effect.shader], shader);

  const basicTier = compile({graphicsTier: "basic"});
  assert.equal(basicTier.valid, false);
  assert.ok(basicTier.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1020"));

  const missingShader = compile({}, {}, {
    "Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n",
  });
  assert.equal(missingShader.valid, false);
  assert.ok(missingShader.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1022"));

  const invalidDefault = compile({}, {uniforms: [
    {name: "tint", type: "color", slot: 0, default: [1, 0, 2, 1]},
  ]});
  assert.equal(invalidDefault.valid, false);
  assert.ok(invalidDefault.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1023"));

  const defaultOutsideRange = compile({}, {uniforms: [
    {name: "amount", type: "number", slot: 0, default: 2,
      minimum: 0, maximum: 1},
  ]});
  assert.equal(defaultOutsideRange.valid, false);
  assert.ok(defaultOutsideRange.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1023"));
});

test("compileProject rejects unknown and unsupported-tier Story effects before packaging", () => {
  const compile = (story: string, projectOverrides: object = {}) => compileProject({
    project: {...project, ...projectOverrides},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": story},
    locales: {"zh-TW": {format:"PrismatiXLocale",schemaRevision:2,
      locale:"zh-TW",strings:{}}},
  });

  const unknown = compile("[effect missing-effect]\n[end]\n");
  assert.equal(unknown.valid, false);
  assert.ok(unknown.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1024" && diagnostic.sourceId !== undefined));

  const wrongTier = compile("[effect blur]\n[end]\n");
  assert.equal(wrongTier.valid, false);
  assert.ok(wrongTier.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1025"));

  const gpu = compile("[effect blur]\n[camera pan]\n[end]\n",
    {graphicsTier:"gpu-effects"});
  assert.equal(gpu.valid, true, JSON.stringify(gpu.diagnostics));
});
