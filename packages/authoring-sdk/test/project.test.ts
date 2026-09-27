import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, StoryProjectCompilationCache} from "../src/index.js";

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

test("project compilation checks authored input action routes", () => {
  const compile = (route: string) => compileProject({project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": {...game, inputActions: [
        {id: "screen.open.settings", context: "game", keys: ["F8"], route},
      ]},
      "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
  });
  const valid = compile("settings");
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  const missing = compile("not-a-route");
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((item) => item.code === "PXBUILD1061"));
});

test("project compilation reports unresolved key-binding buttons before Player starts", () => {
  const compile = (action: unknown, asComponent = false) => {
    const bound = {...ui.nodes[0], id: "33333333-3333-4333-8333-333333333333",
      parentId: rootId, order: 1, kind: "button", runtimeType: "TextureButton",
      name: "Shortcut", runtimeProperties: {bindingAction: action}};
    const authored = {...ui, nodes: [...ui.nodes, bound]};
    return compileProject({project: asComponent
      ? {...project, uiComponents: [{id: uiId, source: "Content/UI/Keys.pxuicomponent"}]}
      : project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": {...game, inputActions: [
        {id: "photo-mode", context: "game", keys: ["P"]},
      ]},
      "Content/UI/Title.pxui": asComponent ? ui : authored,
      ...(asComponent ? {"Content/UI/Keys.pxuicomponent": {
        ...componentContent, nodes: authored.nodes, format: "PrismatiXUIComponent",
        componentInterface: {properties: [], signals: [], slots: []},
      }} : {})},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}}});
  };
  assert.equal(compile("photo-mode").valid, true);
  assert.equal(compile("quick-save").valid, true);
  const typo = compile("photo-mod");
  assert.equal(typo.valid, true, "a host may register its own action later");
  assert.deepEqual(typo.diagnostics.filter((item) => item.code === "PXBUILD1072")
    .map((item) => [item.path, item.details]), [[
      "Content/UI/Title.pxui", "nodes[1].runtimeProperties.bindingAction: photo-mod",
    ]]);
  const componentTypo = compile("photo-mod", true);
  assert.equal(componentTypo.valid, true, JSON.stringify(componentTypo.diagnostics));
  assert.equal(componentTypo.diagnostics.find((item) => item.code === "PXBUILD1072")?.path,
    "Content/UI/Keys.pxuicomponent");
  for (const invalid of ["", 12]) {
    const result = compile(invalid);
    assert.equal(result.valid, false);
    assert.ok(result.diagnostics.some((item) => item.code === "PXBUILD1071"));
  }
});

test("project compilation checks keyboard defaults against a native key registry", () => {
  const compile = (actionKey: string, legacyKey: string) => compileProject({project,
    inputKeyRegistry: {format: "PrismatiXInputKeyRegistry", schemaRevision: 1,
      keys: ["Enter", "Escape", "F8"]},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": {...game,
        inputActions: [{id: "quick-save", context: "game", keys: [actionKey]}],
        inputBindings: [{key: legacyKey, command: "screen.open", argument: "settings"}]},
      "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
  });
  assert.equal(compile("F8", "Enter").valid, true);
  const invalid = compile("F88", "Escap");
  assert.equal(invalid.valid, false);
  assert.deepEqual(invalid.diagnostics.filter((item) => item.code === "PXBUILD1068")
    .map((item) => item.details), ["inputActions.quick-save: F88", "inputBindings[0]: Escap"]);
  assert.ok(invalid.diagnostics.every((item) => item.code !== "PXBUILD1068" ||
    item.path === "Content/game.pxgame"));
  assert.match(invalid.diagnostics.find((item) => item.details?.endsWith("F88"))?.hint ?? "", /F8/u);
  assert.match(invalid.diagnostics.find((item) => item.details?.endsWith("Escap"))?.hint ?? "", /Escape/u);
});

test("compileProject validates extension UI Actions and packages typed resource arguments", () => {
  const extensionPath = "Content/Extensions/music.pxextension";
  const extension = {format: "PrismatiXExtension", schemaRevision: 2,
    language: "javascript", id: "music-extension", version: "1.0.0",
    requiredEngineVersion: "^0.2.0", entry: "music.js",
    capabilities: ["ui", "audio"],
    safety: {previewSafe: true, deterministic: true, seekSafe: true,
      rollbackSafe: true}, commands: [],
    actions: [{id: "music.play", parameters: [
      {name: "media", type: "resource", required: true},
      {name: "volume", type: "number", range: {minimum: 0, maximum: 1}},
    ]}],
  };
  const compile = (arguments_: Record<string, unknown>, manifest: unknown = extension) => compileProject({
    project: {...project, extensions: [extensionPath]},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": {...ui, nodes: [{...ui.nodes[0],
        onClick: {id: "music.play", arguments: arguments_}}]},
      [extensionPath]: manifest},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
  });
  const valid = compile({media: {type: "resource", value: "Assets/Audio/theme.ogg"},
    volume: 1});
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.ok(valid.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/theme.ogg"));
  const extensionless = compile({media: {type: "resource", value: "Assets/Audio/theme"}});
  assert.equal(extensionless.valid, true, JSON.stringify(extensionless.diagnostics));
  assert.ok(extensionless.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/theme"));

  const invalid = compile({media: "Assets/Audio/theme.ogg", volume: 2});
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1062"));
  assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1064"));

  const defaulted = compile({volume: 0.5}, {...extension, actions: [{
    id: "music.play", parameters: [
      {name: "media", type: "resource", required: true,
        default: "Assets/Audio/default.ogg"},
      {name: "volume", type: "number", range: {minimum: 0, maximum: 1}},
    ],
  }]});
  assert.equal(defaulted.valid, true, JSON.stringify(defaulted.diagnostics));
  assert.ok(defaulted.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/default.ogg"));
});

test("UI-only project edits reuse Story compilation but publish fresh UI and revision", () => {
  const storyCompilationCache = new StoryProjectCompilationCache();
  const input = {
    project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {"ui.start": "開始"}}},
    storyCompilationCache,
  };
  const first = compileProject({...input, committedRevision: 1});
  assert.equal(first.valid, true, JSON.stringify(first.diagnostics));
  const progress: Array<{cached?: boolean}> = [];
  const second = compileProject({...input, committedRevision: 2,
    documents: {...input.documents, "Content/UI/Title.pxui": {...ui, name: "New title"}},
    onStoryProgress: (event) => progress.push(event)});
  assert.equal(second.valid, true, JSON.stringify(second.diagnostics));
  assert.ok(progress.length > 0 && progress.every((event) => event.cached === true));
  assert.equal(second.artifact?.runtimeIr.committedRevision, 2);
  assert.deepEqual(second.artifact?.runtimeIr.operations, first.artifact?.runtimeIr.operations);
  assert.notEqual(second.artifact?.files["Content/UI/Title.pxui"],
    first.artifact?.files["Content/UI/Title.pxui"]);
});

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

test("compileProject packages both halves of an authored intro-loop BGM", () => {
  const result = compileProject({project, documents: {
    "Story/story.pxindex": storyIndex,
    "Content/game.pxgame": game,
    "Content/UI/Title.pxui": ui,
  }, sourceFiles: {"Story/zh-TW/scene01.pxstory":
    "[bgm Assets/Audio/theme-loop.ogg intro=Assets/Audio/theme-intro.ogg fade=0.5s]\n[end]\n"},
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
    locale: "zh-TW", strings: {}}}});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.artifact?.dependencies
    .filter((dependency) => dependency.source.startsWith("Assets/Audio/theme-"))
    .map((dependency) => dependency.source).sort(), [
      "Assets/Audio/theme-intro.ogg", "Assets/Audio/theme-loop.ogg",
    ]);
});

test("compileProject packages nested SpriteText glyph images", () => {
  const spriteUi = {...ui, nodes: [ui.nodes[0]!, {
    ...ui.nodes[0]!, id: "33333333-3333-4333-8333-333333333333",
    parentId: rootId, kind: "image", runtimeType: "SpriteText",
    name: "ChapterNumber", runtimeProperties: {text: "1-", advance: 24, glyphs: [
      {character: "1", path: "Assets/Glyph/one.png", x: 0, y: 2, width: 18, height: 30},
      {character: "-", path: "Assets/Glyph/dash.png", x: 1, y: 3, width: 19, height: 10},
    ]},
  }]};
  const result = compileProject({project, documents: {
    "Story/story.pxindex": storyIndex,
    "Content/game.pxgame": game,
    "Content/UI/Title.pxui": spriteUi,
  }, sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
    locale: "zh-TW", strings: {}}}});
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.artifact?.dependencies
    .filter((dependency) => dependency.source.startsWith("Assets/Glyph/"))
    .map((dependency) => dependency.source).sort(),
    ["Assets/Glyph/dash.png", "Assets/Glyph/one.png"]);
});

test("compileProject validates splash voice variants and packages every possible choice", () => {
  const settings = {
    "player.titleVoiceVariants": ["Assets/Voice/title-a.ogg", "Assets/Voice/title-b.ogg"],
    "player.presentationTransitions": {routePreset: "crossfade", routeDuration: 0.3,
      splashPreset: "slide-left", splashDuration: 0.4,
      titlePreset: "fade", titleDuration: 0.5,
      routeOverrides: [{from: "title", to: "settings", preset: "slide-right",
        duration: 0.6}]},
    "player.splashes": [{scene: "Content/UI/Title.pxui", audioDelay: 1,
      minimumDuration: 4, transitionPreset: "crossfade", transitionDuration: 0.7,
      audioVariants: ["Assets/Voice/logo-a.ogg", "Assets/Voice/logo-b.ogg"]}],
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
  assert.deepEqual(valid.artifact?.project.settings?.["player.presentationTransitions"],
    settings["player.presentationTransitions"]);
  assert.equal((valid.artifact?.project.settings?.["player.splashes"] as
    Array<{transitionDuration: number}>)[0]?.transitionDuration, 0.7);
  const instant = compile({...settings,
    "player.presentationTransitions": {routePreset: "none", routeDuration: 0,
      splashPreset: "none", splashDuration: 0, titlePreset: "none",
      titleDuration: 0},
    "player.splashes": [{scene: "Content/UI/Title.pxui",
      transitionPreset: "none", transitionDuration: 0}]});
  assert.equal(instant.valid, true, JSON.stringify(instant.diagnostics));
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
    {...settings, "player.presentationTransitions": {routeDuration: -0.1}},
    {...settings, "player.presentationTransitions": {splashDuration: 11}},
    {...settings, "player.presentationTransitions": {titleDuration: "0.5"}},
    {...settings, "player.presentationTransitions": {unknownDuration: 0.2}},
    {...settings, "player.presentationTransitions": {routePreset: "tiles"}},
    {...settings, "player.presentationTransitions": {routeOverrides: [
      {from: "title", to: "settings", preset: "wipe", duration: 0.3}]}},
    {...settings, "player.splashes": [{scene: "Content/UI/Title.pxui",
      transitionDuration: -1}]},
    {...settings, "player.splashes": [{scene: "Content/UI/Title.pxui",
      transitionDuration: 11}]},
    {...settings, "player.splashes": [{scene: "Content/UI/Title.pxui",
      transitionPreset: "custom"}]},
  ]) assert.equal(compile(invalidSettings).valid, false);
  const unknownRoute = compile({...settings, "player.presentationTransitions": {
    routeOverrides: [{from: "title", to: "typo", preset: "crossfade", duration: 0.3}]}});
  assert.ok(unknownRoute.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1057"));
  const duplicatePair = compile({...settings, "player.presentationTransitions": {
    routeOverrides: [
      {from: "title", to: "settings", preset: "crossfade", duration: 0.3},
      {from: "title", to: "settings", preset: "fade", duration: 0.5}]}});
  assert.ok(duplicatePair.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1058"));
  const sameRoute = compile({...settings, "player.presentationTransitions": {
    routeOverrides: [{from: "title", to: "title", preset: "fade", duration: 0.5}]}});
  assert.ok(sameRoute.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1057"));
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

test("compileProject validates motion descriptors and packages their image layers", () => {
  const descriptorPath = "Assets/Motion/logo.pxmotion";
  const descriptor = {format: "PrismatiXMotion", schemaRevision: 1,
    id: "logo", canvas: {width: 1280, height: 720}, duration: 1, loop: false,
    layers: [{id: "back", image: "Assets/Motion/back.png", width: 640, height: 360},
      {id: "mark", parent: "back", image: "Assets/Motion/mark.png",
        width: 128, height: 128}],
    tracks: [{layer: "mark", property: "opacity", keys: [
      {time: 0, value: 0, curve: "linear"},
      {time: 1, value: 1, curve: "linear"}]}]};
  const compile = (motion: unknown, resourceExists?: (path: string) => boolean) =>
    compileProject({
      project: {...project, assets: [{id: "33333333-3333-4333-8333-333333333333",
        kind: "motion", source: descriptorPath}]},
      documents: {"Story/story.pxindex": storyIndex,
        "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
      sourceFiles: {"Story/zh-TW/scene01.pxstory":
        "[motion Assets/Motion/logo.pxmotion await=true]\n[end]\n"},
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
        locale: "zh-TW", strings: {}}},
      resourceText: (path) => path === descriptorPath ? JSON.stringify(motion) : undefined,
      ...(resourceExists === undefined ? {} : {resourceExists}),
    });
  const valid = compile(descriptor);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  for (const path of [descriptorPath, "Assets/Motion/back.png", "Assets/Motion/mark.png"])
    assert.ok(valid.artifact?.dependencies.some((dependency) => dependency.source === path));
  const atlas = compile({...descriptor, schemaRevision: 2,
    layers: [descriptor.layers[0], {...descriptor.layers[1], frames: [
      {x: 0, y: 0, width: 128, height: 128},
      {x: 128, y: 0, width: 128, height: 128},
    ]}], tracks: [{layer: "mark", property: "frame", keys: [
      {time: 0, value: 0, curve: "step"},
      {time: 0.5, value: 1, curve: "step"},
    ]}]});
  assert.equal(atlas.valid, true, JSON.stringify(atlas.diagnostics));
  assert.equal(atlas.artifact?.dependencies.filter((dependency) =>
    dependency.source === "Assets/Motion/mark.png").length, 1,
  "all atlas frames share one packaged texture");
  const masked = compile({...descriptor, schemaRevision: 6,
    layers: [{...descriptor.layers[0], mask: "mark"}, descriptor.layers[1]]});
  assert.equal(masked.valid, true, JSON.stringify(masked.diagnostics));
  assert.ok(masked.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Motion/mark.png"),
  "mask-only sources must still be packaged");
  const nested = compile({...descriptor, schemaRevision: 7,
    layers: [{id: "subclip", kind: "group", timeOffset: -0.25, timeScale: 2},
      {...descriptor.layers[1], parent: "subclip"}]});
  assert.equal(nested.valid, true, JSON.stringify(nested.diagnostics));
  assert.ok(nested.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Motion/mark.png"));
  assert.equal(nested.artifact?.dependencies.some((dependency) =>
    dependency.source === ""), false,
  "non-drawing transform groups do not create empty image dependencies");
  const missingMask = compile({...descriptor, schemaRevision: 6,
    layers: [{...descriptor.layers[0], mask: "mark"}, descriptor.layers[1]]},
  (path) => path !== "Assets/Motion/mark.png");
  assert.equal(missingMask.valid, false);
  assert.ok(missingMask.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1044"));
  const sound = compile({...descriptor, schemaRevision: 3, markers: [
    {id: "hit", time: 0.5, audio: "Assets/Audio/hit.ogg", bus: "se"},
  ]});
  assert.equal(sound.valid, true, JSON.stringify(sound.diagnostics));
  assert.ok(sound.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/hit.ogg"));
  const missingSound = compile({...descriptor, schemaRevision: 3, markers: [
    {id: "hit", time: 0.5, audio: "Assets/Audio/hit.ogg", bus: "se"},
  ]}, (path) => path !== "Assets/Audio/hit.ogg");
  assert.equal(missingSound.valid, false);
  assert.ok(missingSound.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1044"));
  const referencedFromUi = compileProject({
    project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": {...ui, nodes: [ui.nodes[0], {
        ...ui.nodes[0], id: "33333333-3333-4333-8333-333333333333",
        parentId: rootId, order: 0, kind: "image", runtimeType: "MotionRect",
        name: "Logo motion", runtimeProperties: {path: descriptorPath},
      }]}},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "zh-TW", strings: {}}},
    resourceText: (path) => path === descriptorPath ? JSON.stringify(descriptor) : undefined,
  });
  assert.equal(referencedFromUi.valid, true, JSON.stringify(referencedFromUi.diagnostics));
  for (const path of [descriptorPath, "Assets/Motion/back.png", "Assets/Motion/mark.png"])
    assert.ok(referencedFromUi.artifact?.dependencies.some((dependency) => dependency.source === path));
  const broken = compile({...descriptor, layers: [...descriptor.layers,
    {...descriptor.layers[0], id: "back"}]});
  assert.equal(broken.valid, false);
  assert.ok(broken.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1060"));
  const missingImage = compile(descriptor, (path) => path !== "Assets/Motion/mark.png");
  assert.equal(missingImage.valid, false);
  assert.ok(missingImage.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1044"));
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
