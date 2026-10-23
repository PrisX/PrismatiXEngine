// Studio extensions (docs/studio-extensions.md): schemaRevision 2 manifests
// with a Studio side, their diagnostics, packages and what a game sees.

import assert from "node:assert/strict";
import test from "node:test";

import {
  compileProject,
  extensionSides,
  gameExtensionManifest,
  hasGameSide,
  isJsonPointer,
  parseContributionId,
  storyCommandSchema,
  validateDocument,
  validateExtensionPackage,
  validateRegistryDocument,
  writeExtensionPackage,
  type ExtensionManifest,
  type GameExtensionManifest,
} from "../src/index.js";

const safety = {previewSafe: true, deterministic: true, seekSafe: true, rollbackSafe: true} as const;
const studio = {
  entry: "dist/studio.js", requiredStudioVersion: ">=0.1.0", permissions: ["project.read"],
  contributes: {
    assetTypes: [{id: "live2d.model", displayName: "Live2D model", patterns: ["*.model3.json"]}],
    inspectors: [{assetType: "live2d.model", displayName: "Live2D preview"}],
    parameterOptions: [{id: "motions", displayName: "Motions"}],
  },
} as const;
const gameSide = {language: "javascript", entry: "main.js", capabilities: ["runtime"], safety,
  commands: [{id: "live2d.motion", parameters: [
    {name: "model", type: "resource", resourceFilter: "*.model3.json"},
    {name: "motion", type: "string", optionsFrom: {parameter: "model", pointer: "/FileReferences/Motions"}},
    {name: "group", type: "token", optionsFrom: {provider: "owen/live2d:motions"}},
  ]}], actions: []} as const;

function manifest(fields: Record<string, unknown>): Record<string, unknown> {
  return {format: "PrismatiXExtension", schemaRevision: 2, id: "live2d", version: "1.0.0",
    requiredEngineVersion: ">=0.1.0 <0.2.0", ...fields};
}

function codes(value: unknown): string[] {
  return validateDocument("extension", value, "live2d.pxextension").diagnostics.map((item) => item.code);
}

test("schemaRevision 2 manifests: Studio-only and both sides", () => {
  const studioOnly = manifest({studio});
  const both = manifest({...gameSide, studio});
  for (const value of [studioOnly, both]) {
    const result = validateDocument<ExtensionManifest>("extension", value);
    assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  }
  assert.deepEqual(extensionSides(studioOnly as unknown as ExtensionManifest), ["studio"]);
  assert.deepEqual(extensionSides(both as unknown as ExtensionManifest), ["game", "studio"]);
  assert.equal(hasGameSide(studioOnly as unknown as ExtensionManifest), false);
  // A Studio-only manifest may still name its language and list empty game arrays.
  assert.deepEqual(codes(manifest({studio, language: "typescript", commands: [], actions: [], capabilities: []})), []);
  // Revision 2 with an entry is revision 1 plus studio: the game fields stay required.
  assert.ok(codes(manifest({entry: "main.js", studio})).includes("PXSDKJSON1004"));
  // Revision 1 manifests are unchanged; one that names its own provider needs a Studio side.
  assert.deepEqual(codes({...manifest(gameSide), schemaRevision: 1}), ["PXSDKSEM1137"]);
  assert.deepEqual(codes({...manifest({...gameSide, commands: []}), schemaRevision: 1}), []);
  assert.ok(codes({...manifest({studio}), schemaRevision: 1}).includes("PXSDKJSON1004"));
  assert.ok(codes({...manifest({...gameSide, studio}), schemaRevision: 3}).includes("PXSDKJSON1004"));
});

test("manifest shape errors say what to change", () => {
  const revisionOne = validateDocument("extension", {...manifest({...gameSide, studio}), schemaRevision: 1});
  assert.equal(revisionOne.diagnostics[0]?.code, "PXSDKSEM1126");
  assert.match(revisionOne.diagnostics[0]!.message, /schemaRevision.*2/u);
  assert.ok(!revisionOne.diagnostics.some((item) => /"then"|boolean schema/u.test(item.details ?? "")),
    JSON.stringify(revisionOne.diagnostics));
  assert.deepEqual(codes(manifest({})), ["PXSDKSEM1127"]);
  const gameFields = validateDocument("extension", manifest({studio, modules: ["lib.js"], safety, platforms: ["web"],
    native: {abi: 1, services: ["a.b"], libraries: {"win32-x64": {path: "native/win32-x64/a.dll"}}},
    commands: [{id: "x", parameters: []}], actions: [{id: "y", parameters: []}], capabilities: ["runtime"]}));
  assert.deepEqual(gameFields.diagnostics.map((item) => [item.code, item.details]).sort(), [
    ["PXSDKSEM1128", "actions"], ["PXSDKSEM1128", "capabilities"], ["PXSDKSEM1128", "commands"],
    ["PXSDKSEM1128", "modules"], ["PXSDKSEM1128", "native"], ["PXSDKSEM1128", "platforms"], ["PXSDKSEM1128", "safety"]]);
});

test("the studio object: entry, permissions, range and contributions", () => {
  const with_ = (fields: Record<string, unknown>) => codes(manifest({studio: {...studio, ...fields}}));
  assert.ok(with_({entry: "studio.ts"}).includes("PXSDKJSON1004"), "the Studio entry is built JavaScript");
  assert.ok(with_({entry: "../studio.js"}).includes("PXSDKJSON1004"));
  assert.ok(with_({permissions: ["network"]}).includes("PXSDKJSON1004"), "v1 has project.read and project.write only");
  assert.ok(with_({permissions: ["project.read", "project.read"]}).includes("PXSDKJSON1004"));
  assert.deepEqual(with_({permissions: ["project.read", "project.write"]}), []);
  assert.ok(codes(manifest({studio: {entry: "studio.js"}})).includes("PXSDKJSON1004"), "requiredStudioVersion is required");
  assert.deepEqual(with_({requiredStudioVersion: "soon"}), ["PXSDKSEM1129"]);
  const contributes = (value: Record<string, unknown>) => with_({contributes: {...studio.contributes, ...value}});
  assert.ok(contributes({assetTypes: [{id: "a", displayName: "A", patterns: []}]}).includes("PXSDKJSON1004"));
  assert.ok(contributes({assetTypes: [{id: "a", displayName: "A", patterns: ["models/*.json"]}]}).includes("PXSDKJSON1004"),
    "patterns match file names, not paths");
  assert.ok(contributes({parameterOptions: Array.from({length: 65}, (_, index) => ({id: `p${index}`}))}).includes("PXSDKJSON1004"));
  assert.deepEqual(contributes({assetTypes: [...studio.contributes.assetTypes, ...studio.contributes.assetTypes]}),
    ["PXSDKSEM1130"]);
  assert.deepEqual(contributes({parameterOptions: [{id: "motions"}, {id: "motions"}]}), ["PXSDKSEM1130"]);
  assert.deepEqual(contributes({inspectors: [{assetType: "spine.skeleton"}]}), ["PXSDKSEM1131"]);
  assert.deepEqual(contributes({inspectors: [{assetType: "image"}, {assetType: "audio"}, {assetType: "text"}]}), [],
    "built-in kinds need no declaration");
  assert.deepEqual(contributes({inspectors: [{assetType: "live2d.model"}, {assetType: "live2d.model"}]}), ["PXSDKSEM1132"]);
});

test("optionsFrom and resourceFilter lists on parameters", () => {
  assert.deepEqual(codes(manifest({...gameSide, studio})), []);
  const parameters = (list: unknown[], extra: Record<string, unknown> = {}) =>
    codes(manifest({...gameSide, studio, ...extra, commands: [{id: "live2d.motion", parameters: list}]}));
  const model = {name: "model", type: "resource"};
  assert.deepEqual(parameters([{...model, resourceFilter: "image/*, *.model3.json,audio/ogg"}]), []);
  assert.deepEqual(parameters([{...model, resourceFilter: "models/*.json"}]), ["PXSDKSEM1138"]);
  assert.deepEqual(parameters([{name: "model", type: "string", resourceFilter: "*.model3.json"}]), ["PXSDKSEM1139"],
    "the Player refuses a resourceFilter on a command's string parameter");
  assert.deepEqual(parameters([model, {name: "motion", type: "integer", optionsFrom: {parameter: "model", pointer: ""}}]),
    ["PXSDKSEM1133"]);
  assert.deepEqual(parameters([model, {name: "motion", type: "string", enum: ["a"], optionsFrom: {parameter: "model", pointer: ""}}]),
    ["PXSDKSEM1133"]);
  assert.deepEqual(parameters([{name: "motion", type: "string", optionsFrom: {parameter: "model", pointer: ""}}]), ["PXSDKSEM1134"]);
  assert.deepEqual(parameters([{name: "model", type: "string"}, {name: "motion", type: "string",
    optionsFrom: {parameter: "model", pointer: ""}}]), ["PXSDKSEM1134"]);
  assert.deepEqual(parameters([model, {name: "motion", type: "string", optionsFrom: {parameter: "model", pointer: "Motions"}}]),
    ["PXSDKSEM1135"]);
  assert.deepEqual(parameters([model, {name: "motion", type: "string", optionsFrom: {parameter: "model", pointer: "/a~2"}}]),
    ["PXSDKSEM1135"]);
  assert.deepEqual(parameters([{name: "group", type: "token", optionsFrom: {provider: "motions"}}]), ["PXSDKSEM1136"]);
  assert.deepEqual(parameters([{name: "group", type: "token", optionsFrom: {provider: "live2d:expressions"}}]), ["PXSDKSEM1137"]);
  assert.deepEqual(parameters([{name: "group", type: "token", optionsFrom: {provider: "owen/other:motions"}}]), [],
    "another extension's provider is resolved by Studio");
  assert.ok(parameters([{name: "group", type: "token", optionsFrom: {provider: "a:b", parameter: "model", pointer: ""}}])
    .includes("PXSDKJSON1004"));
  // Both revisions accept them, Actions too.
  assert.deepEqual(codes({...manifest({...gameSide, commands: [], actions: [{id: "live2d.pick", parameters: [model,
    {name: "motion", type: "string", optionsFrom: {parameter: "model", pointer: "/FileReferences/Motions", take: "keys"}}]}],
    capabilities: ["runtime", "ui"]}), schemaRevision: 1}), []);
  const schema = storyCommandSchema(gameSide.commands as unknown as GameExtensionManifest["commands"]);
  assert.deepEqual(schema.at(-1)?.parameters.find((parameter) => parameter.name === "motion")?.optionsFrom,
    {parameter: "model", pointer: "/FileReferences/Motions"}, "editors receive optionsFrom with the command schema");
  assert.equal(isJsonPointer(""), true);
  assert.equal(isJsonPointer("/a~0b/~1/0"), true);
  assert.equal(isJsonPointer("a"), false);
  assert.deepEqual(parseContributionId("owen/live2d:motions"), {extension: "owen/live2d", id: "motions"});
  assert.equal(parseContributionId("owen/live2d"), undefined);
});

test("packages carry the Studio entry in dist/", async () => {
  const encoder = new TextEncoder();
  const metadata = {format: "PrismatiXExtensionPackage", formatVersion: 1, id: "owen/live2d", version: "1.0.0",
    manifest: "live2d.pxextension", license: "MIT"};
  const pack = (value: Record<string, unknown>, scripts: string[]) => writeExtensionPackage([
    {path: "pxext.json", data: encoder.encode(JSON.stringify(metadata))},
    {path: "live2d.pxextension", data: encoder.encode(JSON.stringify(value))},
    {path: "README.md", data: encoder.encode("# Live2D\n")},
    {path: "LICENSE", data: encoder.encode("MIT\n")},
    ...scripts.map((path) => ({path, data: encoder.encode("export function activate() {}\n")}))]);
  const studioOnly = await validateExtensionPackage(pack(manifest({studio}), ["dist/studio.js"]));
  assert.equal(studioOnly.valid, true, JSON.stringify(studioOnly.diagnostics));
  assert.deepEqual(studioOnly.diagnostics, [], "the Studio entry is a referenced script");
  const both = await validateExtensionPackage(pack(manifest({...gameSide, entry: "dist/main.js", studio}),
    ["dist/main.js", "dist/studio.js"]));
  assert.equal(both.valid, true, JSON.stringify(both.diagnostics));
  const missing = await validateExtensionPackage(pack(manifest({studio}), []));
  assert.ok(missing.diagnostics.some((item) => item.code === "PXEXTPKG1010" && item.details === "dist/studio.js"));
  const outside = await validateExtensionPackage(pack(manifest({studio: {...studio, entry: "studio.js"}}), ["dist/studio.js"]));
  assert.ok(outside.diagnostics.some((item) => item.code === "PXEXTPKG1010" && item.details === "studio.js"));
  const range = await validateExtensionPackage(pack(manifest({studio: {...studio, requiredStudioVersion: "soon"}}), ["dist/studio.js"]));
  assert.ok(range.diagnostics.some((item) => item.code === "PXEXTPKG1009" && /PXSDKSEM1129|SemVer/u.test(item.details ?? "")),
    JSON.stringify(range.diagnostics));
});

test("registry documents may announce sides and the Studio range", () => {
  const result = {id: "owen/live2d", latestVersion: "1.0.0", requiredEngineVersion: "*", capabilities: [],
    sides: ["studio"], requiredStudioVersion: ">=0.1.0"};
  assert.equal(validateRegistryDocument("searchResponse", {results: [result], total: 1, page: 1, pageSize: 20}).valid, true);
  assert.equal(validateRegistryDocument("searchResponse", {results: [{...result, sides: ["editor"]}], total: 1, page: 1,
    pageSize: 20}).valid, false);
  assert.equal(validateRegistryDocument("extension", {id: "owen/live2d", capabilities: [], versions: [],
    sides: ["game", "studio"], requiredStudioVersion: ">=0.1.0"}).valid, true);
  assert.equal(validateRegistryDocument("version", {version: "1.0.0", requiredEngineVersion: "*", sha256: "a".repeat(64), size: 1,
    download: "x.pxext", manifest: manifest({studio}), yanked: false}).valid, true);
});

const rootId = "11111111-1111-4111-8111-111111111111";
const title = {format: "PrismatiXUIScene", schemaRevision: 1, id: "22222222-2222-4222-8222-222222222222", revision: 1,
  name: "Title", width: 1280, height: 720, rootId,
  nodes: [{id: rootId, parentId: null, order: 0, kind: "control", runtimeType: "Control", name: "Root", visible: true, locked: false,
    layout: {mode: "free", x: 0, y: 0, width: 1280, height: 720, anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0,
      pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed"},
    runtimeProperties: {}, bindings: {}}],
  theme: [], behaviorGraph: {nodes: [], links: [], groups: []}, behaviorTriggers: [], visualStateGroups: []};

test("a game skips Studio-only extensions and gets the game side as revision 1", () => {
  const project = {format: "PrismatiXProject", schemaRevision: 1, id: "studio-sides", name: "Studio sides", version: "0.1.0",
    contentVersion: "1", saveVersion: 1, resolution: {width: 1280, height: 720}, entry: {story: "main", ui: "title"},
    defaultLocale: "en", supportedLocales: ["en"], storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame",
    extensions: ["Extensions/owen/tools/tools.pxextension", "Extensions/owen/live2d/live2d.pxextension"],
    uiEntryPoints: {title: "Content/UI/Title.pxui"}, assets: []};
  const both = manifest({...gameSide, studio});
  const result = compileProject({project,
    documents: {
      "Story/story.pxindex": {format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "main",
        chapters: [{id: "c", title: "C", scenes: ["main"]}], scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}]},
      "Content/game.pxgame": {format: "PrismatiXGame", schemaRevision: 1, variables: [], gallery: [], unlockables: []},
      "Content/UI/Title.pxui": title,
      "Extensions/owen/tools/tools.pxextension": manifest({id: "tools", studio: {...studio, entry: "studio.js"}}),
      "Extensions/owen/live2d/live2d.pxextension": both,
    },
    sourceFiles: {"Story/main.pxstory": "Hello.\n[live2d.motion model=\"Assets/a.model3.json\" motion=\"Idle\" group=\"g\"]\n[end]\n"},
    locales: {en: {format: "PrismatiXLocale", schemaRevision: 1, locale: "en", strings: {}}},
  });
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const artifact = result.artifact!;
  assert.deepEqual(artifact.project.extensions, ["Extensions/owen/live2d/live2d.pxextension"]);
  assert.deepEqual(JSON.parse(artifact.files["project.pxproject"]!).extensions, ["Extensions/owen/live2d/live2d.pxextension"]);
  assert.ok(!Object.keys(artifact.files).some((path) => path.startsWith("Extensions/owen/tools/")));
  assert.ok(!artifact.dependencies.some((dependency) => dependency.source.includes("tools") || dependency.source.includes("studio")),
    JSON.stringify(artifact.dependencies));
  const shipped = JSON.parse(artifact.files["Extensions/owen/live2d/live2d.pxextension"]!) as Record<string, unknown>;
  assert.equal(shipped.schemaRevision, 1, "the Player reads revision 1");
  assert.equal(shipped.studio, undefined, "the Studio side never reaches the game");
  assert.deepEqual(gameExtensionManifest(both as unknown as GameExtensionManifest), shipped);
  assert.equal(JSON.stringify(artifact.runtimeIr).includes("tools"), false);

  // A broken Studio-only manifest still fails the build: it is listed in the project.
  const broken = compileProject({project: {...project, extensions: ["Extensions/owen/tools/tools.pxextension"]},
    documents: {"Story/story.pxindex": {format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "main",
      chapters: [{id: "c", title: "C", scenes: ["main"]}], scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}]},
    "Content/game.pxgame": {format: "PrismatiXGame", schemaRevision: 1, variables: [], gallery: [], unlockables: []},
    "Content/UI/Title.pxui": title, "Extensions/owen/tools/tools.pxextension": manifest({id: "tools"})},
    sourceFiles: {"Story/main.pxstory": "Hello.\n[end]\n"},
    locales: {en: {format: "PrismatiXLocale", schemaRevision: 1, locale: "en", strings: {}}}});
  assert.ok(broken.diagnostics.some((item) => item.code === "PXSDKSEM1127"), JSON.stringify(broken.diagnostics));
});
