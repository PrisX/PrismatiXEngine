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

test("compileProject reports completed Story scenes across locales", () => {
  const progress: Array<{phase: string; locale: string; current: number;
    total: number; globalCurrent: number; globalTotal: number}> = [];
  const result = compileProject({
    project: {...project, supportedLocales: ["ja-JP", "zh-TW"]},
    documents: {
      "Story/story.pxindex": {...storyIndex, scenes: [{id: "scene01", sources: {
        "zh-TW": "Story/zh-TW/scene01.pxstory",
        "ja-JP": "Story/ja-JP/scene01.pxstory",
      }}]},
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n",
      "Story/ja-JP/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 2, locale: "zh-TW", strings: {}},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 2, locale: "ja-JP", strings: {}},
    },
    onStoryProgress: (event) => progress.push(event),
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(progress.map(({phase, locale, current, total, globalCurrent, globalTotal}) =>
    [phase, locale, current, total, globalCurrent, globalTotal]), [
    ["parse", "zh-TW", 1, 1, 1, 2], ["compile", "zh-TW", 1, 1, 1, 2],
    ["parse", "ja-JP", 1, 1, 2, 2], ["compile", "ja-JP", 1, 1, 2, 2],
  ]);
});

test("compileProject collects UI runtime texture dependencies", () => {
  const textureUi = {
    ...ui,
    nodes: [{
      ...ui.nodes[0],
      runtimeType: "TextureButton",
      onClick: {id: "music.play", arguments: {
        argument: "Assets/Audio/theme.wav",
        label: "chapter/route.without-a-resource-extension/",
      }},
      runtimeProperties: {
        normalPath: "Assets/UI/start.png",
        hoverPath: "Assets/UI/start-hover.png",
        pressedPath: "Assets/UI/start-pressed.png",
        disabledPath: "Assets/UI/start-disabled.png",
        hoverSoundPath: "Assets/Audio/hover.ogg",
        activateSoundPath: "Assets/Audio/activate.ogg",
      },
    }],
  };
  const result = compileProject({
    project,
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": textureUi,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.artifact?.dependencies
    .filter((dependency) => dependency.source.startsWith("Assets/UI/"))
    .map((dependency) => dependency.source).sort(), [
      "Assets/UI/start-disabled.png",
      "Assets/UI/start-hover.png",
      "Assets/UI/start-pressed.png",
      "Assets/UI/start.png",
    ]);
  assert.ok(result.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/theme.wav"),
  "path-shaped UI action arguments must be included in the artifact");
  assert.ok(result.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/hover.ogg"));
  assert.ok(result.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/activate.ogg"));
});

test("compileProject validates splash voice variants and packages every possible choice", () => {
  const settings = {
    "player.titleVoiceVariants": ["Assets/Voice/title-a.ogg", "Assets/Voice/title-b.ogg"],
    "player.splashes": [{scene: "Content/UI/Title.pxui", audioDelay: 1,
      minimumDuration: 4, audioVariants: ["Assets/Voice/logo-a.ogg", "Assets/Voice/logo-b.ogg"]}],
  };
  const compile = (values: unknown, resourceExists?: (path: string) => boolean) => compileProject({
    project: {...project, settings: values},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
    ...(resourceExists === undefined ? {} : {resourceExists}),
  });
  const valid = compile(settings);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.deepEqual(valid.artifact?.dependencies
    .filter((dependency) => dependency.source.startsWith("Assets/Voice/"))
    .map((dependency) => dependency.source).sort(), [
      "Assets/Voice/logo-a.ogg", "Assets/Voice/logo-b.ogg",
      "Assets/Voice/title-a.ogg", "Assets/Voice/title-b.ogg",
    ]);
  const missing = compile(settings, (path) => path !== "Assets/Voice/logo-b.ogg");
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1044" &&
    diagnostic.path === "Assets/Voice/logo-b.ogg"));
  for (const invalidSettings of [
    {...settings, "player.titleVoiceVariants": ["Assets/Voice/title-a.ogg", 42]},
    {...settings, "player.titleVoiceVariants": []},
    {...settings, "player.splashes": [{scene: "Content/UI/Title.pxui",
      audioVariants: ["../outside.ogg"]}]},
    {...settings, "player.splashes": [{scene: "Content/UI/Title.pxui",
      audioVariants: []}]},
  ]) assert.equal(compile(invalidSettings).valid, false);
  const uncompiledScene = compile({...settings, "player.splashes": [{
    scene: "Content/UI/Uncompiled.pxui"}]});
  assert.ok(uncompiledScene.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1055"));
  const impossibleSkip = compile({...settings, "player.splashes": [{
    scene: "Content/UI/Title.pxui", minimumDuration: 0, skipAllowedAfter: 0.5}]});
  assert.ok(impossibleSkip.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1056"));
});

test("compileProject validates portraits referenced outside Story and collects every layer", () => {
  const descriptorPath = "Assets/Characters/Hero.pxportrait";
  const descriptor = {format: "PrismatiXLayeredPortrait", schemaRevision: 1,
    width: 691, height: 1450, layers: [
      {path: "Assets/Characters/Layers/body.png", width: 691, height: 1450},
      {path: "Assets/Characters/Layers/face.png", x: 302, y: 153,
        width: 174, height: 74, opacity: 217},
    ]};
  const withPortraitUi = {...ui, nodes: [{...ui.nodes[0],
    onClick: {id: "show.portrait", arguments: {argument: descriptorPath}}}]};
  const compile = (text: string | undefined,
    resourceExists?: (path: string) => boolean) => compileProject({
    project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": withPortraitUi},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
    resourceText: (path) => path === descriptorPath ? text : undefined,
    ...(resourceExists === undefined ? {} : {resourceExists}),
  });
  const valid = compile(JSON.stringify(descriptor));
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  for (const path of [descriptorPath, "Assets/Characters/Layers/body.png",
    "Assets/Characters/Layers/face.png"])
    assert.ok(valid.artifact?.dependencies.some((dependency) =>
      dependency.source === path), `${path} is packaged`);

  const catalogOnly = compileProject({
    project: {...project, assets: [{id: "33333333-3333-4333-8333-333333333333",
      kind: "character", source: descriptorPath}]},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
    resourceText: (path) => path === descriptorPath
      ? JSON.stringify(descriptor) : undefined,
  });
  assert.equal(catalogOnly.valid, true, JSON.stringify(catalogOnly.diagnostics));
  assert.ok(catalogOnly.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Characters/Layers/face.png"));

  const invalid = compile(JSON.stringify({...descriptor, height: 0}));
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1045" && diagnostic.path === descriptorPath));
  const missing = compile(undefined);
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1045" && diagnostic.path === descriptorPath));
  const missingLayer = compile(JSON.stringify(descriptor),
    (path) => path !== "Assets/Characters/Layers/face.png");
  assert.equal(missingLayer.valid, false);
  assert.ok(missingLayer.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1044" &&
    diagnostic.path === "Assets/Characters/Layers/face.png"));
});

test("compileProject checks every authored UI property against a supplied native registry", () => {
  const registry = {controls: [{runtimeType: "Control", properties: [
    {id: "modulate", valueType: "color", writable: true, bindable: true, animatable: true},
  ]}]};
  const compile = (authoredColor: unknown) => compileProject({
    project,
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": {...ui, nodes: [{...ui.nodes[0],
        runtimeProperties: {modulate: authoredColor}}]},
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
    uiTypeRegistry: registry,
  });
  assert.equal(compile({type: "color", value: "#2D171DFF"}).valid, true);
  const invalid = compile("#2D171DFF");
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((item) => item.code === "PXSDKSEM1324" &&
    item.path === "Content/UI/Title.pxui"));
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
  const extensionPath = "Content/Extensions/localized.pxextension";
  const heroId = "33333333-3333-4333-8333-333333333333";
  const heroSource = `Characters/${heroId}.pxcharacter`;
  const hero = {format: "PrismatiXCharacter", schemaRevision: 2,
    id: heroId, displayName: "英雄", displayNames: {"ja-JP": "勇者"},
    aliases: ["hero"], expressions: []};
  const rivalId = "44444444-4444-4444-8444-444444444444";
  const rivalSource = `Characters/${rivalId}.pxcharacter`;
  const rival = {format: "PrismatiXCharacter", schemaRevision: 2,
    id: rivalId, displayName: "對手", aliases: ["rival"], expressions: []};
  const localizedExtension = {
    format: "PrismatiXExtension", schemaRevision: 2, language: "javascript",
    id: "localized-media", version: "1.0.0",
    requiredEngineVersion: ">=0.2.0 <0.3.0", entry: "localized.js",
    capabilities: ["runtime"],
    safety: {previewSafe: true, deterministic: true, seekSafe: true,
      rollbackSafe: true},
    commands: [{id: "localized.cue", displayName: "Localized cue",
      rollback: "reversible", capabilities: ["runtime"], parameters: [
        {name: "media", type: "resource", required: true},
        {name: "cue", type: "string", required: true},
      ]}],
    actions: [],
  };
  const bilingualIndex = {
    ...storyIndex,
    scenes: [{id: "scene01", sources: {
      "zh-TW": "Story/zh-TW/scene01.pxstory",
      "ja-JP": "Story/ja-JP/scene01.pxstory",
    }}],
  };
  const compile = (japanese: string) => compileProject({
    project: {...project, supportedLocales: ["zh-TW", "ja-JP"],
      extensions: [extensionPath],
      characters: [{id: heroId, displayName: "英雄", source: heroSource},
        {id: rivalId, displayName: "對手", source: rivalSource}]},
    documents: {
      "Story/story.pxindex": bilingualIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      [extensionPath]: localizedExtension,
      [heroSource]: hero,
      [rivalSource]: rival,
    },
    sourceFiles: {
      "Story/zh-TW/scene01.pxstory": "*start\n[bg asset=opening.background]\n[voice asset=opening.voice]\n[localized.cue media=opening.voice cue=opening]\n@hero\n早安。\n[choice text=留下 goto=end]\n*end\n[end]\n",
      "Story/ja-JP/scene01.pxstory": japanese,
      "Content/Extensions/localized.js": "export {};",
      "Assets/zh-TW/opening.webp": "localized-image-zh",
      "Assets/ja-JP/opening.webp": "localized-image-ja",
      "Assets/zh-TW/opening.ogg": "localized-voice-zh",
      "Assets/ja-JP/opening.ogg": "localized-voice-ja",
    },
    locales: {
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "zh-TW", strings: {}, resources: {
          "opening.background": "Assets/zh-TW/opening.webp",
          "opening.voice": "Assets/zh-TW/opening.ogg",
        }},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "ja-JP", strings: {}, resources: {
          "opening.background": "Assets/ja-JP/opening.webp",
          "opening.voice": "Assets/ja-JP/opening.ogg",
        }},
    },
  });

  const valid = compile("*start\n[bg asset=opening.background]\n[voice asset=opening.voice]\n[localized.cue media=opening.voice cue=opening]\n@hero\nおはよう。\n[choice text=残る goto=end]\n*end\n[end]\n");
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
  assert.equal(base.runtimeIr.operations.find((operation) =>
    operation.kind === "dialogue")?.arguments.speaker, "英雄");
  assert.equal(localized.runtimeIr.operations.find((operation) =>
    operation.kind === "dialogue")?.arguments.speaker, "勇者");
  assert.equal(localized.runtimeIr.operations.find((operation) =>
    operation.kind === "dialogue")?.arguments.character, heroId);
  assert.equal(base.runtimeIr.operations.find((operation) =>
    operation.kind === "background")?.arguments.asset,
  "Assets/zh-TW/opening.webp");
  assert.equal(localized.runtimeIr.operations.find((operation) =>
    operation.kind === "background")?.arguments.asset,
  "Assets/ja-JP/opening.webp");
  assert.equal(localized.runtimeIr.operations.find((operation) =>
    operation.kind === "voice")?.arguments.asset,
  "Assets/ja-JP/opening.ogg");
  const localizedCue = localized.runtimeIr.operations.find((operation) =>
    operation.kind === "customNode")?.arguments.value;
  assert.equal(JSON.parse(localizedCue ?? "{}").media,
    "Assets/ja-JP/opening.ogg");
  assert.ok(valid.artifact!.dependencies.some((dependency) =>
    dependency.source === "Assets/ja-JP/opening.ogg"));
  assert.ok(valid.artifact!.files["Runtime/Locales/ja-JP/main.pxmap"]?.includes(
    "Story/ja-JP/scene01.pxstory"));

  const wrongActor = compile("*start\n[bg asset=opening.background]\n[voice asset=opening.voice]\n[localized.cue media=opening.voice cue=opening]\n@rival\nおはよう。\n[choice text=残る goto=end]\n*end\n[end]\n");
  assert.equal(wrongActor.valid, false);
  assert.ok(wrongActor.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1031"));

  const structurallyDifferent = compile("*start\n[bg asset=opening.background]\n[voice asset=opening.voice]\n[localized.cue media=opening.voice cue=opening]\n@hero\nおはよう。\n[set route=branch]\n[choice text=残る goto=end]\n*end\n[end]\n");
  assert.equal(structurallyDifferent.valid, false);
  assert.ok(structurallyDifferent.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1030" || diagnostic.code === "PXBUILD1031"));
});

test("localized literal Story speakers keep dialogue identity and presentation separate", () => {
  const bilingualIndex = {...storyIndex, scenes: [{id: "scene01", sources: {
    "zh-TW": "Story/zh-TW/scene01.pxstory",
    "ja-JP": "Story/ja-JP/scene01.pxstory",
  }}]};
  const result = compileProject({
    project: {...project, supportedLocales: ["zh-TW", "ja-JP"]},
    documents: {"Story/story.pxindex": bilingualIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {
      "Story/zh-TW/scene01.pxstory": '*start\n@"村雨"\n早安。\n[end]\n',
      "Story/ja-JP/scene01.pxstory": '*start\n@"ムラサメ"\nおはよう。\n[end]\n',
    },
    locales: {
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "zh-TW", strings: {}},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "ja-JP", strings: {}},
    },
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const base = result.artifact!.localeArtifacts["zh-TW"]!.runtimeIr.operations
    .find((operation) => operation.kind === "dialogue")!;
  const translated = result.artifact!.localeArtifacts["ja-JP"]!.runtimeIr.operations
    .find((operation) => operation.kind === "dialogue")!;
  assert.equal(base.arguments.speaker, "村雨");
  assert.equal(translated.arguments.speaker, "ムラサメ");
  assert.equal(translated.arguments.character, "");
  assert.equal(translated.operationId, base.operationId);
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

  const transition = compile({}, {
    schemaRevision: 3,
    targetLayer: "transition",
  });
  assert.equal(transition.valid, true, JSON.stringify(transition.diagnostics));

  const legacyWrongTarget = compile({}, {targetLayer: "node"});
  assert.equal(legacyWrongTarget.valid, false);
  assert.ok(legacyWrongTarget.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXSDKJSON1004"));

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
