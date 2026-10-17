import assert from "node:assert/strict";
import test from "node:test";

import {compileProject, ENGINE_VERSION, satisfiesEngineVersion, StoryProjectCompilationCache} from "../src/index.js";

const rootId = "11111111-1111-4111-8111-111111111111";
const uiId = "22222222-2222-4222-8222-222222222222";
const project = {
  format: "PrismatiXProject",
  schemaRevision: 1,
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
  schemaRevision: 1,
  id: "main",
  entryScene: "scene01",
  chapters: [{id: "chapter01", title: "Chapter 1", scenes: ["scene01"]}],
  scenes: [{id: "scene01", sources: {"zh-TW": "Story/zh-TW/scene01.pxstory"}}],
};
const game = {
  format: "PrismatiXGame",
  schemaRevision: 1,
  variables: [
    {name: "route", type: "string", default: "common", scope: "session"},
    {name: "allClear", type: "boolean", default: false, scope: "profile"},
  ],
  gallery: [],
  unlockables: [],
};
const ui = {
  format: "PrismatiXUIScene",
  schemaRevision: 1,
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

test("project host Action contract checks authored UI before publication", () => {
  const actionContract = {format: "PrismatiXActionContract", schemaRevision: 1,
    actions: [{id: "host.open", safety: {previewSafe: true,
      deterministic: true, seekSafe: true, rollbackSafe: true},
    parameters: [{name: "target", type: "string", required: true}]}]};
  const authorProject = {...project, settings: {"authoring.actionContract": actionContract}};
  const compile = (target: unknown) => compileProject({project: authorProject,
    documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game,
      "Content/UI/Title.pxui": {...ui, nodes: [{...ui.nodes[0], onClick: {
        id: "host.open", arguments: {target},
      }}]}},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  const valid = compile("settings");
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.deepEqual(valid.artifact?.project.settings?.["authoring.actionContract"],
    actionContract);
  assert.ok(compile(7).diagnostics.some((item) => item.code === "PXBUILD1062"));
  const unsupported = compileProject({project: {...project, settings: {
    "authoring.actionContract": {...actionContract, actions: [{
      ...actionContract.actions[0], parameters: [{name: "target", type: "null"}],
    }]},
  }}, documents: {"Story/story.pxindex": storyIndex,
    "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
  sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
    locale: "zh-TW", strings: {}}}});
  assert.equal(unsupported.valid, false);
  assert.ok(unsupported.diagnostics.some((item) => item.code === "PXSDKJSON1004"));
});

test("project compilation checks authored input action routes", () => {
  const compile = (route: string) => compileProject({project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": {...game, inputActions: [
        {id: "screen.open.settings", context: "game", keys: ["F8"], route},
      ]},
      "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  const valid = compile("settings");
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  const missing = compile("not-a-route");
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((item) => item.code === "PXBUILD1061"));
});

test("player.navigation names a keyword or a screen the Player can show", () => {
  const compile = (navigation: unknown) => compileProject({
    project: {...project, uiEntryPoints: {...project.uiEntryPoints, credits: "Content/UI/Title.pxui"},
      settings: {"player.navigation": navigation}},
    documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  for (const navigation of [{}, {afterStoryEnd: "stay"}, {afterStoryEnd: "title"},
    {afterStoryEnd: "quit"}, {afterStoryEnd: "credits"}, {gameRightClick: "hideHud"},
    {gameRightClick: "none"}, {gameRightClick: "settings"}, {gameRightClick: "credits"}]) {
    const result = compile(navigation);
    assert.equal(result.valid, true, `${JSON.stringify(navigation)}: ${JSON.stringify(result.diagnostics)}`);
    assert.deepEqual(result.artifact?.project.settings?.["player.navigation"], navigation);
  }
  for (const navigation of [{afterStoryEnd: "missing"}, {afterStoryEnd: "hud"},
    {gameRightClick: "title"}, {gameRightClick: "video"}]) {
    const result = compile(navigation);
    assert.equal(result.valid, false, JSON.stringify(navigation));
    assert.ok(result.diagnostics.some((item) => item.code === "PXBUILD1111"),
      `${JSON.stringify(navigation)}: ${JSON.stringify(result.diagnostics)}`);
  }
  // The schema rejects other shapes before the route check runs.
  for (const navigation of [{afterStoryEnd: ""}, {afterStoryEnd: 1}, {onEnd: "title"}, "title"]) {
    const result = compile(navigation);
    assert.equal(result.valid, false, JSON.stringify(navigation));
  }
});

test("cross-document diagnostics locate the exact authored JSON values", () => {
  const authoredProject = {...project, name: "測試專案", settings: {
    "player.presentationTransitions": {routePreset: "crossfade", routeDuration: 0.3,
      splashPreset: "fade", splashDuration: 0.3,
      titlePreset: "fade", titleDuration: 0.3, routeOverrides: [
      {from: "title", to: "missing-route", preset: "fade", duration: 0.3},
    ]},
    "player.splashes": [{scene: "Content/UI/Missing.pxui",
      minimumDuration: 0, skipAllowedAfter: 0.5}],
  }};
  const authoredGame = {...game, inputActions: [
    {id: "screen.open.extra", context: "game", keys: ["F8"], route: "missing-action-route"},
  ]};
  const projectText = JSON.stringify(authoredProject, null, 2);
  const gameText = JSON.stringify(authoredGame, null, 2);
  const compile = (manifestText: string, catalogText: string,
    catalog: unknown = authoredGame) => compileProject({
    project: authoredProject,
    projectSource: {path: "prismatix.json", text: manifestText},
    documentSources: {"Content/game.pxgame": catalogText},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": catalog, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  const gameResult = compile(projectText, gameText);
  const projectResult = compile(projectText, JSON.stringify(game), game);
  assert.equal(gameResult.valid, false);
  assert.equal(projectResult.valid, false);
  const at = (result: ReturnType<typeof compile>, code: string,
    path: string, text: string, token: string) => {
    const item = result.diagnostics.find((diagnostic) => diagnostic.code === code);
    assert.equal(item?.span?.path, path, code);
    const offset = text.indexOf(`"${token}"`);
    assert.ok(offset >= 0);
    assert.equal(item.span!.start.offset,
      Buffer.byteLength(text.slice(0, offset), "utf8"), code);
  };
  at(gameResult, "PXBUILD1061", "Content/game.pxgame", gameText, "missing-action-route");
  at(projectResult, "PXBUILD1057", "prismatix.json", projectText, "missing-route");
  at(projectResult, "PXBUILD1055", "prismatix.json", projectText, "Content/UI/Missing.pxui");
  const staleProject = compile(projectText.replace("missing-route", "settings"),
    JSON.stringify(game), game);
  const staleGame = compile(projectText,
    gameText.replace("missing-action-route", "settings"));
  assert.equal(staleProject.diagnostics.find((item) => item.code === "PXBUILD1057")?.span,
    undefined);
  assert.equal(staleGame.diagnostics.find((item) => item.code === "PXBUILD1061")?.span,
    undefined);
});

test("missing authored includes and UI entries point back to their declarations", () => {
  const included = {...project, resourceIncludes: ["Assets/Missing"]};
  const includeResult = compileProject({project: included,
    projectSource: {path: "prismatix.json", text: JSON.stringify(included)},
    resolveResourceInclude: () => [],
  });
  assert.equal(includeResult.diagnostics.find((item) => item.code === "PXBUILD1052")?.span?.path,
    "prismatix.json");
  const missingUi = {...project, uiEntryPoints: {title: "Content/UI/Missing.pxui"}};
  const uiResult = compileProject({project: missingUi,
    projectSource: {path: "prismatix.json", text: JSON.stringify(missingUi)},
    documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  assert.equal(uiResult.diagnostics.find((item) => item.code === "PXBUILD1002")?.span?.path,
    "prismatix.json");
});

test("project compilation checks declared Player UI data binding paths", () => {
  const result = compileProject({project: {...project,
    settings: {"player.uiData": {screen: {caption: "Ready"}}}},
  documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game,
    "Content/UI/Title.pxui": {...ui, nodes: [{...ui.nodes[0],
      bindings: {text: {path: "screen.missing"}}}]}},
  sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
    locale: "zh-TW", strings: {}}}});
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXBUILD1086" &&
    item.path === "Content/UI/Title.pxui"));
});

test("project compilation rejects a missing host-declared UI path", () => {
  const result = compileProject({project: {...project, settings: {
    "authoring.uiContract": {format: "PrismatiXUiContract", schemaRevision: 1,
      roots: ["dialogue"], values: {dialogue: "object", "dialogue.caption": "string"}},
  }}, documents: {"Story/story.pxindex": storyIndex,
    "Content/game.pxgame": game,
    "Content/UI/Title.pxui": {...ui, nodes: [{...ui.nodes[0],
      bindings: {text: {path: "dialogue.typo"}}}]}},
  sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
    locale: "zh-TW", strings: {}}}});
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXBUILD1094" &&
    item.path === "Content/UI/Title.pxui"));
});

test("project compilation reports unresolved key-binding buttons before Player starts", () => {
  const compile = (action: unknown, asComponent = false,
    extraProperties: Record<string, unknown> = {}) => {
    const bound = {...ui.nodes[0], id: "33333333-3333-4333-8333-333333333333",
      parentId: rootId, order: 1, kind: "button", runtimeType: "TextureButton",
      name: "Shortcut", runtimeProperties: {bindingAction: action,
        ...extraProperties}};
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
  assert.equal(compile("backlog", false, {bindingDevice: "wheel",
    wheelThreshold: 1.5}).valid, true);
  for (const invalid of [-1, 101, "fast"]) {
    const result = compile("backlog", false, {wheelThreshold: invalid});
    assert.equal(result.valid, false);
    assert.ok(result.diagnostics.some((item) => item.code === "PXBUILD1073" &&
      item.path === "Content/UI/Title.pxui" &&
      item.details === "nodes[1].runtimeProperties.wheelThreshold"));
  }
  const invalidComponent = compile("backlog", true, {wheelThreshold: 101});
  assert.ok(invalidComponent.diagnostics.some((item) => item.code === "PXBUILD1073" &&
    item.path === "Content/UI/Keys.pxuicomponent"));
});

test("project compilation checks keyboard defaults against a native key registry", () => {
  const compile = (actionKey: string, legacyKey: string,
    sourceOverride?: string) => {
    const catalog = {...game,
        inputActions: [{id: "quick-save", context: "game", keys: [actionKey]}],
        inputBindings: [{key: legacyKey, command: "screen.open", argument: "settings"}]};
    return compileProject({project,
      inputKeyRegistry: {format: "PrismatiXInputKeyRegistry", schemaRevision: 1,
        keys: ["Enter", "Escape", "F8"]},
      documents: {"Story/story.pxindex": storyIndex,
        "Content/game.pxgame": catalog, "Content/UI/Title.pxui": ui},
      documentSources: {"Content/game.pxgame": sourceOverride ??
        JSON.stringify(catalog, null, 2)},
      sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
        locale: "zh-TW", strings: {}}},
    });
  };
  assert.equal(compile("F8", "Enter").valid, true);
  const invalid = compile("F88", "Escap");
  assert.equal(invalid.valid, false);
  assert.deepEqual(invalid.diagnostics.filter((item) => item.code === "PXBUILD1068")
    .map((item) => item.details), ["inputActions.quick-save: F88", "inputBindings[0]: Escap"]);
  assert.ok(invalid.diagnostics.every((item) => item.code !== "PXBUILD1068" ||
    item.path === "Content/game.pxgame"));
  assert.match(invalid.diagnostics.find((item) => item.details?.endsWith("F88"))?.hint ?? "", /F8/u);
  assert.match(invalid.diagnostics.find((item) => item.details?.endsWith("Escap"))?.hint ?? "", /Escape/u);
  for (const [key, token] of [["F88", '"F88"'], ["Escap", '"Escap"']] as const) {
    const item = invalid.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1068" &&
      diagnostic.details?.endsWith(key));
    assert.equal(item?.span?.path, "Content/game.pxgame");
    assert.equal(item.span.end.offset - item.span.start.offset, Buffer.byteLength(token));
  }
  const stale = compile("F88", "Escap", JSON.stringify({...game, inputActions: [], inputBindings: []}));
  assert.ok(stale.diagnostics.filter((item) => item.code === "PXBUILD1068")
    .every((item) => item.span === undefined));
});

test("compileProject validates extension UI Actions and packages typed resource arguments", () => {
  const extensionPath = "Content/Extensions/music.pxextension";
  const extension = {format: "PrismatiXExtension", schemaRevision: 1,
    language: "javascript", id: "music-extension", version: "1.0.0",
    requiredEngineVersion: "^0.2.0", entry: "music.js",
    capabilities: ["ui", "audio"],
    safety: {previewSafe: true, deterministic: true, seekSafe: true,
      rollbackSafe: true}, commands: [],
    actions: [{id: "jukebox.play", parameters: [
      {name: "media", type: "resource", required: true},
      {name: "volume", type: "number", range: {minimum: 0, maximum: 1}},
    ]}],
  };
  const uiWithAction = (arguments_: Record<string, unknown>) =>
    ({...ui, nodes: [{...ui.nodes[0],
      onClick: {id: "jukebox.play", arguments: arguments_}}]});
  const compile = (arguments_: Record<string, unknown>, manifest: unknown = extension) => compileProject({
    project: {...project, extensions: [extensionPath]},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": uiWithAction(arguments_),
      [extensionPath]: manifest},
    documentSources: {"Content/UI/Title.pxui":
      JSON.stringify(uiWithAction(arguments_), null, 2),
      [extensionPath]: JSON.stringify(manifest, null, 2)},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  const valid = compile({media: {type: "resource", value: "Assets/Audio/theme.ogg"},
    volume: 1});
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.ok(valid.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/theme.ogg"));
  const unsupportedEngine = compile({media: "Assets/Audio/theme.ogg"},
    {...extension, requiredEngineVersion: ">=9.0.0"});
  assert.equal(unsupportedEngine.diagnostics.find((item) => item.code === "PXBUILD1013")?.span?.path,
    extensionPath);
  // Extensions are checked against the SDK's own engine version, not a fixed one.
  const exactEngine = compile({media: "Assets/Audio/theme.ogg"},
    {...extension, requiredEngineVersion: `=${ENGINE_VERSION}`});
  assert.ok(!exactEngine.diagnostics.some((item) => item.code === "PXBUILD1013"),
    JSON.stringify(exactEngine.diagnostics));
  const extensionless = compile({media: {type: "resource", value: "Assets/Audio/theme"}});
  assert.equal(extensionless.valid, true, JSON.stringify(extensionless.diagnostics));
  assert.ok(extensionless.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/theme"));

  const invalid = compile({media: "Assets/Audio/theme.ogg", volume: 2});
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1062"));
  assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1064"));
  const wrongMedia = invalid.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1062" && diagnostic.details?.endsWith(".media"));
  assert.ok(wrongMedia?.span);
  const invalidSource = JSON.stringify(uiWithAction({media: "Assets/Audio/theme.ogg",
    volume: 2}), null, 2);
  assert.equal(invalidSource.slice(wrongMedia.span.start.offset, wrongMedia.span.end.offset),
    '"Assets/Audio/theme.ogg"');

  const defaulted = compile({volume: 0.5}, {...extension, actions: [{
    id: "jukebox.play", parameters: [
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {"ui.start": "開始"}}},
    committedRevision: 7,
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.ok(result.artifact);
  assert.equal(result.artifact.engineVersion, ENGINE_VERSION);
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
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {}},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 1, locale: "ja-JP", strings: {}},
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
      onClick: {id: "jukebox.play", arguments: {
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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

test("missing UI dependencies point to their authored values only for matching source", () => {
  const authored = {...ui, nodes: [{...ui.nodes[0],
    runtimeType: "TextureButton",
    runtimeProperties: {normalPath: "Assets/UI/missing.png"},
    onClick: {id: "jukebox.play", arguments: {
      argument: "Assets/Audio/missing.ogg"}},
  }]};
  const source = JSON.stringify(authored, null, 2);
  const compile = (text: string) => compileProject({project,
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": authored},
    documentSources: {"Content/UI/Title.pxui": text},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    resourceExists: (path) => !path.includes("/missing."),
  });
  const result = compile(source);
  assert.equal(result.valid, false);
  const missing = result.diagnostics.filter((item) => item.code === "PXBUILD1044");
  assert.equal(missing.length, 2, JSON.stringify(result.diagnostics));
  for (const diagnostic of missing) {
    assert.equal(diagnostic.span?.path, "Content/UI/Title.pxui");
    assert.ok(diagnostic.span && diagnostic.span.start.line > 1);
    assert.equal(source.slice(diagnostic.span.start.offset, diagnostic.span.end.offset),
      JSON.stringify(diagnostic.path));
  }
  const stale = compile(source.replace("Assets/UI/missing.png", "Assets/UI/other.png"));
  assert.equal(stale.valid, false);
  assert.ok(stale.diagnostics.filter((item) => item.code === "PXBUILD1044")
    .every((item) => item.span === undefined));
});

test("missing Player setting audio points to the source project field", () => {
  const authoredProject = {...project, settings: {
    "player.titleBgm": "Assets/Audio/missing-title.ogg"}};
  const source = JSON.stringify(authoredProject, null, 2);
  const result = compileProject({project: authoredProject,
    documentSources: {"project.pxproject": source},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    resourceExists: (path) => path !== "Assets/Audio/missing-title.ogg",
  });
  assert.equal(result.valid, false);
  const missing = result.diagnostics.find((item) => item.code === "PXBUILD1044" &&
    item.path === "Assets/Audio/missing-title.ogg");
  assert.equal(missing?.span?.path, "project.pxproject");
  assert.ok(missing?.span && missing.span.start.line > 1);
  assert.equal(source.slice(missing.span.start.offset, missing.span.end.offset),
    '"Assets/Audio/missing-title.ogg"');
});

test("missing catalog media points to its authored asset row", () => {
  const authoredProject = {...project, assets: [{
    id: "33333333-3333-4333-8333-333333333333", kind: "cg",
    source: "Assets/CG/missing.png"}]};
  const source = JSON.stringify(authoredProject, null, 2);
  const result = compileProject({project: authoredProject,
    documentSources: {"project.pxproject": source},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    resourceExists: (path) => path !== "Assets/CG/missing.png",
  });
  assert.equal(result.valid, false);
  const missing = result.diagnostics.find((item) => item.code === "PXBUILD1044" &&
    item.path === "Assets/CG/missing.png");
  assert.equal(missing?.span?.path, "project.pxproject");
  assert.ok(missing?.span && missing.span.start.line > 1);
  assert.equal(source.slice(missing.span.start.offset, missing.span.end.offset),
    '"Assets/CG/missing.png"');
});

test("missing Story media keeps its authored command location through scene cache", () => {
  const source = "Opening.\n[bgm Assets/Audio/missing-theme.ogg]\n[end]\n";
  const cache = new StoryProjectCompilationCache();
  const compile = (storyCompilationCache: StoryProjectCompilationCache) =>
    compileProject({project,
      documents: {"Story/story.pxindex": storyIndex,
        "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
      sourceFiles: {"Story/zh-TW/scene01.pxstory": source},
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
        locale: "zh-TW", strings: {}}},
      resourceExists: (path) => path !== "Assets/Audio/missing-theme.ogg",
      storyCompilationCache,
    });
  const location = (result: ReturnType<typeof compileProject>) => {
    assert.equal(result.valid, false);
    const missing = result.diagnostics.find((item) => item.code === "PXBUILD1044" &&
      item.path === "Assets/Audio/missing-theme.ogg");
    assert.equal(missing?.span?.path, "Story/zh-TW/scene01.pxstory");
    assert.equal(missing?.span?.start.line, 2);
    assert.ok(missing?.span);
    assert.equal(source.slice(missing.span.start.offset, missing.span.end.offset),
      "Assets/Audio/missing-theme.ogg");
    return missing.span;
  };
  const first = location(compile(cache));
  assert.deepEqual(location(compile(cache)), first);
  const restored = new StoryProjectCompilationCache();
  restored.importScenes(cache.exportScenes());
  assert.deepEqual(location(compile(restored)), first);
  const damaged = structuredClone(cache.exportScenes()) as {
    locales: Array<{
      scenes: Array<{
        result: {resourceLocations: Array<{span: {path: string}}>};
      }>;
    }>;
  };
  damaged.locales[0]!.scenes[0]!.result.resourceLocations[0]!.span.path =
    "Story/another-scene.pxstory";
  assert.throws(() => new StoryProjectCompilationCache().importScenes(damaged),
    /Invalid Story scene cache entry/u);
});

test("compileProject packages both halves of an authored intro-loop BGM", () => {
  const result = compileProject({project, documents: {
    "Story/story.pxindex": storyIndex,
    "Content/game.pxgame": game,
    "Content/UI/Title.pxui": ui,
  }, sourceFiles: {"Story/zh-TW/scene01.pxstory":
    "[bgm Assets/Audio/theme-loop.ogg intro=Assets/Audio/theme-intro.ogg fade=0.5s]\n[end]\n"},
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
  locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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

test("compileProject packages every Player setting path and rejects missing ones", () => {
  const settings = {
    "player.uiFont": "Assets/Fonts/ui.otf",
    "player.fonts": ["Assets/Fonts/serif.otf", "Assets/Fonts/sans.otf"],
    "player.musicTracks": ["Assets/Music/one.ogg", "Assets/Music/two.ogg"],
    "player.titleBgmAfter": {flag: "cleared", path: "Assets/Music/after.ogg"},
  };
  const compile = (resourceExists?: (path: string) => boolean) => compileProject({
    project: {...project, settings},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    ...(resourceExists === undefined ? {} : {resourceExists}),
  });
  const valid = compile();
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  const packaged = new Set(valid.artifact?.dependencies.map((dependency) => dependency.source));
  for (const path of ["Assets/Fonts/ui.otf", "Assets/Fonts/serif.otf", "Assets/Fonts/sans.otf",
    "Assets/Music/one.ogg", "Assets/Music/two.ogg", "Assets/Music/after.ogg"])
    assert.ok(packaged.has(path), path);
  for (const missingPath of ["Assets/Fonts/ui.otf", "Assets/Fonts/sans.otf",
    "Assets/Music/two.ogg", "Assets/Music/after.ogg"]) {
    const missing = compile((path) => path !== missingPath);
    assert.equal(missing.valid, false, missingPath);
    assert.ok(missing.diagnostics.some((diagnostic) => diagnostic.code === "PXBUILD1044" &&
      diagnostic.path === missingPath), missingPath);
  }
});

test("compileProject checks Player option bindings and confirmations", () => {
  const compile = (settings: unknown) => compileProject({
    project: {...project, settings},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  const settings = {
    "player.options": {
      speed: {default: 50, min: 0, max: 100, binds: "textSpeed", scale: -1, offset: 100},
      askSave: {default: 0, choices: [0, 1]},
    },
    "player.confirmations": {quickSave: {kind: "qs", dontAskOption: "askSave"}, exit: {}},
  };
  const valid = compile(settings);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  for (const invalid of [
    {"player.options": {speed: {default: 50, binds: "textSpeeed"}}},
    {"player.options": {speed: {default: 50, scale: "2"}}},
    {"player.options": {"bad-name": {default: 0}}},
    {"player.confirmations": {quickSaev: {}}},
    {"player.confirmations": {exit: {kind: "has space"}}},
  ]) assert.equal(compile(invalid).valid, false, JSON.stringify(invalid));
  const undeclared = compile({...settings,
    "player.confirmations": {exit: {dontAskOption: "askExit"}}});
  assert.equal(undeclared.valid, false);
  assert.ok(undeclared.diagnostics.some((item) => item.code === "PXBUILD1104"));
  const twice = compile({"player.options": {
    a: {default: 1, choices: [0, 1], binds: "dockPinned"},
    b: {default: 1, choices: [0, 1], binds: "dockPinned"}}});
  assert.equal(twice.valid, true, JSON.stringify(twice.diagnostics));
  assert.deepEqual(twice.diagnostics.filter((item) => item.code === "PXBUILD1105")
    .map((item) => item.severity), ["warning"]);
});

test("compileProject checks profile rules, flags replaced settings and unknown Player keys", () => {
  const compile = (settings: unknown, resourceExists?: (path: string) => boolean) => compileProject({
    project: {...project, settings},
    documents: {"Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game, "Content/UI/Title.pxui": ui},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    ...(resourceExists === undefined ? {} : {resourceExists}),
  });
  const settings = {
    "player.titleBgm": [{when: {all: ["clear_a", {not: "clear_b"}]}, path: "Assets/Music/after.ogg"},
      {path: "Assets/Music/one.ogg"}],
    "player.profileUiData": {
      "title.extraOpen": {any: ["clear_a", "clear_b"]},
      "title.ending": {cases: [{when: "clear_b", value: 2}, {when: "clear_a", value: 1}], default: 0},
    },
  };
  const valid = compile(settings);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.deepEqual(valid.diagnostics.filter((item) => item.severity === "warning"), []);
  const packaged = new Set(valid.artifact?.dependencies.map((dependency) => dependency.source));
  assert.ok(packaged.has("Assets/Music/after.ogg"));
  const missing = compile(settings, (path) => path !== "Assets/Music/after.ogg");
  assert.ok(missing.diagnostics.some((item) => item.code === "PXBUILD1044" &&
    item.path === "Assets/Music/after.ogg"), JSON.stringify(missing.diagnostics));
  for (const invalid of [
    {"player.profileUiData": {"title.x": {every: ["a"]}}},
    {"player.profileUiData": {"title.x": {cases: [{when: "a"}]}}},
    {"player.titleBgm": [{when: "a"}]},
    {"player.saveLayout": {pages: 0}},
  ]) assert.equal(compile(invalid).valid, false, JSON.stringify(invalid));
  const old = compile({"player.titleBgmAfter": {flag: "clear_a", path: "Assets/Music/after.ogg"},
    "player.choiceLayout": {x: 0, y: 400, width: 1280, height: 60, halfPitch: 40},
    "player.titelBgm": "Assets/Music/one.ogg"});
  assert.equal(old.valid, true, JSON.stringify(old.diagnostics));
  assert.deepEqual(old.diagnostics.map(({code, severity}) => [code, severity]).sort(), [
    ["PXBUILD1109", "warning"], ["PXBUILD1109", "warning"], ["PXBUILD1110", "warning"]]);
  assert.match(old.diagnostics.find((item) => item.code === "PXBUILD1110")!.message,
    /did you mean player\.titleBgm\?/u);
});

test("every player.* key the C++ Player reads is in the project schema", async (t) => {
  const {readFile} = await import("node:fs/promises");
  const {contractSchemas} = await import("../src/generated/contracts.js");
  let source: string;
  try {
    source = await readFile(new URL(
      "../../../../PrismatiXEngine/Applications/Player/PlayerSettings.cpp", import.meta.url), "utf8");
  } catch {
    t.skip("engine sources are not beside this package");
    return;
  }
  const read = new Set([...source.matchAll(/find\("(player\.[A-Za-z]+)"\)/gu)].map((match) => match[1]!));
  for (const match of source.matchAll(/"(player\.ui[A-Za-z]+Sound)"/gu)) read.add(match[1]!);
  const schema = contractSchemas["project.schema.json"] as {
    properties: {settings: {properties: Record<string, unknown>}};
  };
  const declared = new Set(Object.keys(schema.properties.settings.properties)
    .filter((key) => key.startsWith("player.")));
  assert.deepEqual([...read].sort(), [...declared].sort());
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    resourceText: (path) => path === descriptorPath
      ? JSON.stringify(descriptor) : undefined,
  });
  assert.equal(catalogOnly.valid, true, JSON.stringify(catalogOnly.diagnostics));
  assert.ok(catalogOnly.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Characters/Layers/face.png"));

  const animated = compile(JSON.stringify({...descriptor, layers: [
    {...descriptor.layers[0], role: "base"},
    {...descriptor.layers[1], role: "expression",
      blink: [{path: "Assets/Characters/Layers/eyes-shut.png", width: 174, height: 40}],
      mouth: [{path: "Assets/Characters/Layers/mouth-open.png", x: 360, y: 200,
        width: 40, height: 20}]}]}));
  assert.equal(animated.valid, true, JSON.stringify(animated.diagnostics));
  for (const path of ["Assets/Characters/Layers/eyes-shut.png",
    "Assets/Characters/Layers/mouth-open.png"])
    assert.ok(animated.artifact?.dependencies.some((dependency) =>
      dependency.source === path), `${path} (a face frame) is packaged`);

  const invalid = compile(JSON.stringify({...descriptor, height: 0}));
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1045" && diagnostic.path === descriptorPath));
  const missing = compile(undefined);
  assert.equal(missing.valid, false);
  assert.ok(missing.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1045" && diagnostic.path === descriptorPath));
  const portraitSource = JSON.stringify(descriptor, null, 2);
  const missingLayer = compile(portraitSource,
    (path) => path !== "Assets/Characters/Layers/face.png");
  assert.equal(missingLayer.valid, false);
  const missingLayerDiagnostic = missingLayer.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1044" &&
    diagnostic.path === "Assets/Characters/Layers/face.png");
  assert.equal(missingLayerDiagnostic?.span?.path, descriptorPath);
  assert.ok(missingLayerDiagnostic?.span && missingLayerDiagnostic.span.start.line > 1);
  assert.equal(portraitSource.slice(missingLayerDiagnostic.span.start.offset,
    missingLayerDiagnostic.span.end.offset),
  '"Assets/Characters/Layers/face.png"');
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
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
        locale: "zh-TW", strings: {}}},
      resourceText: (path) => path === descriptorPath
        ? JSON.stringify(motion, null, 2) : undefined,
      ...(resourceExists === undefined ? {} : {resourceExists}),
    });
  const valid = compile(descriptor);
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  for (const path of [descriptorPath, "Assets/Motion/back.png", "Assets/Motion/mark.png"])
    assert.ok(valid.artifact?.dependencies.some((dependency) => dependency.source === path));
  const atlas = compile({...descriptor, schemaRevision: 1,
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
  const masked = compile({...descriptor, schemaRevision: 1,
    layers: [{...descriptor.layers[0], mask: "mark"}, descriptor.layers[1]]});
  assert.equal(masked.valid, true, JSON.stringify(masked.diagnostics));
  assert.ok(masked.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Motion/mark.png"),
  "mask-only sources must still be packaged");
  const nested = compile({...descriptor, schemaRevision: 1,
    layers: [{id: "subclip", kind: "group", timeOffset: -0.25, timeScale: 2},
      {...descriptor.layers[1], parent: "subclip"}]});
  assert.equal(nested.valid, true, JSON.stringify(nested.diagnostics));
  assert.ok(nested.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Motion/mark.png"));
  assert.equal(nested.artifact?.dependencies.some((dependency) =>
    dependency.source === ""), false,
  "non-drawing transform groups do not create empty image dependencies");
  const missingMask = compile({...descriptor, schemaRevision: 1,
    layers: [{...descriptor.layers[0], mask: "mark"}, descriptor.layers[1]]},
  (path) => path !== "Assets/Motion/mark.png");
  assert.equal(missingMask.valid, false);
  assert.ok(missingMask.diagnostics.some((diagnostic) =>
    diagnostic.code === "PXBUILD1044"));
  const sound = compile({...descriptor, schemaRevision: 1, markers: [
    {id: "hit", time: 0.5, audio: "Assets/Audio/hit.ogg", bus: "se"},
  ]});
  assert.equal(sound.valid, true, JSON.stringify(sound.diagnostics));
  assert.ok(sound.artifact?.dependencies.some((dependency) =>
    dependency.source === "Assets/Audio/hit.ogg"));
  const missingSound = compile({...descriptor, schemaRevision: 1, markers: [
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
  const missingImageDiagnostic = missingImage.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1044" && diagnostic.path === "Assets/Motion/mark.png");
  assert.equal(missingImageDiagnostic?.span?.path, descriptorPath);
  assert.ok(missingImageDiagnostic?.span && missingImageDiagnostic.span.start.line > 1);
  const motionSource = JSON.stringify(descriptor, null, 2);
  assert.equal(motionSource.slice(missingImageDiagnostic.span.start.offset,
    missingImageDiagnostic.span.end.offset), '"Assets/Motion/mark.png"');
});

test("compileProject checks every authored UI property against a supplied native registry", () => {
  const registry = {controls: [{runtimeType: "Control", properties: [
    {id: "modulate", valueType: "color", writable: true, bindable: true, animatable: true},
    {id: "bindingDevice", valueType: "string", writable: true, bindable: true,
      animatable: false, enumChoices: ["keyboard", "wheel"]},
  ]}]};
  const compile = (authoredColor: unknown, bindingDevice?: string) => compileProject({
    project,
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": {...ui, nodes: [{...ui.nodes[0],
        runtimeProperties: {modulate: authoredColor,
          ...(bindingDevice === undefined ? {} : {bindingDevice})}}]},
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
    uiTypeRegistry: registry,
  });
  assert.equal(compile({type: "color", value: "#2D171DFF"}).valid, true);
  const invalid = compile("#2D171DFF");
  assert.equal(invalid.valid, false);
  assert.ok(invalid.diagnostics.some((item) => item.code === "PXSDKSEM1324" &&
    item.path === "Content/UI/Title.pxui"));
  const invalidEnum = compile({type: "color", value: "#2D171DFF"}, "mouse");
  assert.equal(invalidEnum.valid, false);
  assert.ok(invalidEnum.diagnostics.some((item) => item.code === "PXSDKSEM1326" &&
    item.path === "Content/UI/Title.pxui" && item.details === `${rootId}.bindingDevice`));
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
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
  const hero = {format: "PrismatiXCharacter", schemaRevision: 1,
    id: heroId, displayName: "英雄", displayNames: {"ja-JP": "勇者"},
    aliases: ["hero"], expressions: []};
  const rivalId = "44444444-4444-4444-8444-444444444444";
  const rivalSource = `Characters/${rivalId}.pxcharacter`;
  const rival = {format: "PrismatiXCharacter", schemaRevision: 1,
    id: rivalId, displayName: "對手", aliases: ["rival"], expressions: []};
  const localizedExtension = {
    format: "PrismatiXExtension", schemaRevision: 1, language: "javascript",
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
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
        locale: "zh-TW", strings: {}, resources: {
          "opening.background": "Assets/zh-TW/opening.webp",
          "opening.voice": "Assets/zh-TW/opening.ogg",
        }},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 1,
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

  const wrongCue = compile("*start\n[bg asset=opening.background]\n[voice asset=opening.voice]\n[localized.cue media=opening.voice cue=changed]\n@hero\nおはよう。\n[choice text=残る goto=end]\n*end\n[end]\n");
  assert.equal(wrongCue.valid, false);
  assert.ok(wrongCue.diagnostics.some((diagnostic) =>
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
      "zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
        locale: "zh-TW", strings: {}},
      "ja-JP": {format: "PrismatiXLocale", schemaRevision: 1,
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
  const authoredProject = {...project, entry: {story: "missing", ui: "title"}};
  const authoredLocale = {format: "PrismatiXLocale", schemaRevision: 1, locale: "ja-JP", strings: {}};
  const result = compileProject({
    project: authoredProject,
    projectSource: {path: "prismatix.json", text: JSON.stringify(authoredProject)},
    documentSources: {"Content/Localization/zh-TW.json": JSON.stringify(authoredLocale)},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": authoredLocale},
  });
  assert.equal(result.valid, false);
  assert.equal(result.artifact, undefined);
  assert.equal(result.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1005")?.span?.path,
    "prismatix.json");
  assert.equal(result.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1006")?.span?.path,
    "Content/Localization/zh-TW.json");
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
    schemaRevision: 1,
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
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {}}},
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.ok(result.artifact?.files[descriptor.asset]?.includes("PrismatiXSaveMigration"));

  const oversized = compileProject({
    project: {...project, saveMigrations: [descriptor]},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      [descriptor.asset]: {...migration, operations: Array.from({length: 5}, (_, index) => ({
        op: "setVariable", name: `var${index}`, value: "x".repeat(1024 * 1024),
      }))},
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {}}},
  });
  assert.equal(oversized.valid, false);
  assert.equal(oversized.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1074")?.path,
    descriptor.asset);

  const mismatched = {...migration, id: "wrong-migration-id"};
  const mismatch = compileProject({
    project: {...project, saveMigrations: [descriptor]},
    documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui, [descriptor.asset]: mismatched},
    documentSources: {[descriptor.asset]: JSON.stringify(mismatched)},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
      locale: "zh-TW", strings: {}}},
  });
  assert.equal(mismatch.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1007")?.span?.path, descriptor.asset);

  const brokenProject = {...project, saveMigrations: [
    {...descriptor, to: {contentVersion: "orphan", saveVersion: 1}}]};
  const broken = compileProject({
    project: brokenProject,
    projectSource: {path: "prismatix.json", text: JSON.stringify(brokenProject)},
    documents: {
      "Story/story.pxindex": storyIndex,
      "Content/game.pxgame": game,
      "Content/UI/Title.pxui": ui,
      [descriptor.asset]: {...migration, to: {contentVersion: "orphan", saveVersion: 1}},
    },
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {}}},
  });
  assert.equal(broken.valid, false);
  assert.equal(broken.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1010")?.span?.path,
    "prismatix.json");
});

test("compileProject publishes only validated custom-effect source inputs", () => {
  const descriptor = {id: "dream-tone", source: "Effects/dream-tone.pxeffect"};
  const effect = {
    format: "PrismatiXEffect",
    schemaRevision: 1,
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
    }) => {
    const authoredProject = {...project, graphicsTier: "gpu-effects", effects: [descriptor],
      ...projectOverrides};
    const authoredEffect = {...effect, ...effectOverrides};
    return compileProject({
      project: authoredProject,
      projectSource: {path: "prismatix.json", text: JSON.stringify(authoredProject)},
      documents: {
        "Story/story.pxindex": storyIndex,
        "Content/game.pxgame": game,
        "Content/UI/Title.pxui": ui,
        [descriptor.source]: authoredEffect,
      },
      documentSources: {[descriptor.source]: JSON.stringify(authoredEffect)},
      sourceFiles,
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1,
        locale: "zh-TW", strings: {}}},
    });
  };

  const valid = compile();
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  assert.ok(valid.artifact?.files[descriptor.source]?.includes("PrismatiXEffect"));
  assert.equal(valid.artifact?.files[effect.shader], shader);

  const transition = compile({}, {
    schemaRevision: 1,
    targetLayer: "transition",
  });
  assert.equal(transition.valid, true, JSON.stringify(transition.diagnostics));

  const nodeTarget = compile({}, {targetLayer: "node"});
  assert.equal(nodeTarget.valid, true, JSON.stringify(nodeTarget.diagnostics));

  const basicTier = compile({graphicsTier: "basic"});
  assert.equal(basicTier.valid, false);
  assert.equal(basicTier.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1020")?.span?.path, "prismatix.json");

  const missingShader = compile({}, {}, {
    "Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n",
  });
  assert.equal(missingShader.valid, false);
  assert.equal(missingShader.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1022")?.span?.path, descriptor.source);

  const invalidDefault = compile({}, {uniforms: [
    {name: "tint", type: "color", slot: 0, default: [1, 0, 2, 1]},
  ]});
  assert.equal(invalidDefault.valid, false);
  assert.equal(invalidDefault.diagnostics.find((diagnostic) =>
    diagnostic.code === "PXBUILD1023")?.span?.path, descriptor.source);

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
    locales: {"zh-TW": {format:"PrismatiXLocale",schemaRevision:1,
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

  // The diagnostics point at the Story line, and a sepia tone names itself.
  const located = compile("Opening.\n[tone sepia]\n[effect missing-effect]\n[end]\n");
  const tone = located.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1025");
  assert.equal(tone?.span?.start.line, 2, JSON.stringify(located.diagnostics));
  assert.match(tone?.message ?? "", /tone sepia/u);
  const missing = located.diagnostics.find((diagnostic) => diagnostic.code === "PXBUILD1024");
  assert.equal(missing?.span?.start.line, 3);
  assert.match(missing?.message ?? "", /missing-effect/u);

  // [camera x= y= zoom=] (the documented pan and zoom) is not an effect preset.
  const camera = compile("[camera x=120 y=-30 zoom=1.2 duration=800ms]\n[camera x=0 y=0 zoom=1]\n[end]\n");
  assert.equal(camera.valid, true, JSON.stringify(camera.diagnostics));

  const gpu = compile("[effect blur]\n[camera pan]\n[end]\n",
    {graphicsTier:"gpu-effects"});
  assert.equal(gpu.valid, true, JSON.stringify(gpu.diagnostics));
});

test("Story plays project effects: [effect id duration uniforms], [effect none] and [transition effect=]", () => {
  const stage = {format: "PrismatiXEffect", schemaRevision: 1, id: "dream", targetLayer: "stage",
    shader: "Effects/dream.frag.hlsl", uniforms: [
      {name: "amount", type: "number", slot: 0, default: 0.5, minimum: 0, maximum: 1},
      {name: "tint", type: "color", slot: 1, default: [1, 1, 1, 1]}]};
  const wipe = {format: "PrismatiXEffect", schemaRevision: 1, id: "ink-wipe", targetLayer: "transition",
    shader: "Effects/ink-wipe.frag.hlsl", uniforms: [
      {name: "softness", type: "number", slot: 0, default: 0.1, minimum: 0, maximum: 1}]};
  const compile = (story: string) => {
    const authoredProject = {...project, graphicsTier: "gpu-effects", effects: [
      {id: "dream", source: "Effects/dream.pxeffect"}, {id: "ink-wipe", source: "Effects/ink-wipe.pxeffect"}]};
    return compileProject({
      project: authoredProject,
      documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game, "Content/UI/Title.pxui": ui,
        "Effects/dream.pxeffect": stage, "Effects/ink-wipe.pxeffect": wipe},
      sourceFiles: {"Story/zh-TW/scene01.pxstory": story, [stage.shader]: "// stage\n", [wipe.shader]: "// wipe\n"},
      locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {}}},
    });
  };
  const valid = compile("[effect dream 1s amount=0.8 tint=#FF8000]\n[effect none]\n" +
    "[transition 2s effect=ink-wipe softness=0.25]\n[end]\n");
  assert.equal(valid.valid, true, JSON.stringify(valid.diagnostics));
  const operations = valid.artifact!.runtimeIr.operations;
  const effect = operations.find((operation) => operation.kind === "effect" && operation.arguments.value === "dream")!;
  assert.equal(effect.arguments.duration, "1s");
  assert.deepEqual(JSON.parse(effect.arguments.params!), {amount: [0.8], tint: [1, 0.502, 0, 1]});
  assert.ok(operations.some((operation) => operation.kind === "effect" && operation.arguments.value === "none"));
  const transition = operations.find((operation) => operation.kind === "transition")!;
  assert.equal(transition.arguments.effect, "ink-wipe");
  assert.deepEqual(JSON.parse(transition.arguments.params!), {softness: [0.25]});

  const codes = (story: string) => compile(story).diagnostics.map((diagnostic) => diagnostic.code);
  // [effect] plays Stage effects; a transition effect belongs to [transition].
  assert.ok(codes("[effect ink-wipe]\n[end]\n").includes("PXBUILD1026"));
  assert.ok(codes("[transition 1s effect=dream]\n[end]\n").includes("PXBUILD1026"));
  assert.ok(codes("[transition 1s effect=missing]\n[end]\n").includes("PXBUILD1026"));
  // Uniform names, component counts and ranges come from the descriptor.
  assert.ok(codes("[effect dream strength=1]\n[end]\n").includes("PXBUILD1027"));
  assert.ok(codes("[effect dream amount=2]\n[end]\n").includes("PXBUILD1027"));
  assert.ok(codes("[effect dream amount=0.1,0.2]\n[end]\n").includes("PXBUILD1027"));
  assert.ok(codes("[effect dream amount=bright]\n[end]\n").includes("PXSTORY1280"));
  assert.ok(codes("[transition 1s effect=ink-wipe rule=wipe]\n[end]\n").includes("PXSTORY1282"));
  // Without effect=, [transition] keeps its closed argument list.
  assert.ok(codes("[transition 1s softness=0.2]\n[end]\n").includes("PXSTORY1017"));
});

test("compileProject declares every hover.<name> a UI binds as Player UI data", () => {
  const hoverUi = {...ui, nodes: ui.nodes.map((node) =>
    ({...node, bindings: {visible: {path: "hover.StartButton"}}}))};
  const compile = (settings?: Record<string, unknown>) => compileProject({
    project: {...project, ...(settings === undefined ? {} : {settings})},
    documents: {"Story/story.pxindex": storyIndex, "Content/game.pxgame": game,
      "Content/UI/Title.pxui": hoverUi},
    sourceFiles: {"Story/zh-TW/scene01.pxstory": "Opening.\n[end]\n"},
    locales: {"zh-TW": {format: "PrismatiXLocale", schemaRevision: 1, locale: "zh-TW", strings: {}}},
  });
  const result = compile();
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.artifact?.project.settings?.["player.uiData"], {"hover.StartButton": false});
  const authored = compile({"player.uiData": {"hover.StartButton": true, "mine": 1}});
  assert.deepEqual(authored.artifact?.project.settings?.["player.uiData"],
    {"hover.StartButton": true, "mine": 1});
});

test("extension engine ranges compare against the running engine version", () => {
  assert.equal(satisfiesEngineVersion(`^${ENGINE_VERSION}`), true);
  assert.equal(satisfiesEngineVersion(`>${ENGINE_VERSION}`), false);
  assert.equal(satisfiesEngineVersion(">=0.3.0 <0.4.0", "0.3.2"), true);
  assert.equal(satisfiesEngineVersion("^0.2.0", "0.3.0"), false);
  assert.equal(satisfiesEngineVersion("~1.4.0 || ^2.0.0", "2.7.1"), true);
  assert.equal(satisfiesEngineVersion("^0.3.0", "0.3.0-beta.1"), true);
  assert.equal(satisfiesEngineVersion("soon"), undefined);
  assert.equal(satisfiesEngineVersion("*", "not-a-version"), undefined);
});
