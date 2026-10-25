import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {writeFileSync} from "node:fs";
import {chmod, link, mkdtemp, mkdir, readFile, readdir, rm, stat, symlink, utimes, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {spawn, spawnSync} from "node:child_process";
import {gunzipSync, gzipSync} from "node:zlib";
import test from "node:test";

import {createStarter} from "../src/starter.js";
import {discoverRuntimeTool, RuntimeDiscoveryFailure} from "../src/runtime-discovery.js";

function stable(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stable).join(",")}]`;
  if (value !== null && typeof value === "object") {
    const source = value as Record<string, unknown>;
    return `{${Object.keys(source).sort().map((key) => `${JSON.stringify(key)}:${stable(source[key])}`).join(",")}}`;
  }
  return JSON.stringify(value);
}

async function write(path: string, value: string): Promise<void> {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, value, "utf8");
}
/** A stand-in native tool: discovery requires the executable bit outside Windows. */
async function writeExecutable(path: string, value: string): Promise<void> {
  await write(path, value);
  await chmod(path, 0o755);
}

function run(args: readonly string[]) {
  return spawnSync(process.execPath, [resolve("dist/src/cli.js"), ...args, "--json"], {encoding: "utf8"});
}

function runHuman(args: readonly string[]) {
  return spawnSync(process.execPath, [resolve("dist/src/cli.js"), ...args], {encoding: "utf8"});
}

async function fixture(): Promise<{root: string; project: string}> {
  const root = await mkdtemp(join(tmpdir(), "prismatix-cli-test-"));
  const project = join(root, "project.pxproject");
  await write(project, JSON.stringify({
    format: "PrismatiXProject", schemaRevision: 1, id: "cli-sample",
    name: "CLI Sample", version: "0.2.0",
    contentVersion: "chapter-1", saveVersion: 1,
    resolution: {width: 1280, height: 720}, entry: {story: "main", ui: "title"},
    defaultLocale: "en", supportedLocales: ["en"],
    storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame",
    extensions: [], uiEntryPoints: {title: "Content/UI/Title.pxui"}, assets: [],
  }));
  await write(join(root, "Story/story.pxindex"), JSON.stringify({
    format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "main",
    chapters: [{id: "chapter", title: "Chapter", scenes: ["main"]}],
    scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}],
  }));
  await write(join(root, "Story/main.pxstory"), "Hello.\n[end]\n");
  await write(join(root, "Content/game.pxgame"), JSON.stringify({
    format: "PrismatiXGame", schemaRevision: 1,
    variables: [{name: "route", type: "string", default: "common", scope: "session"}],
    gallery: [], unlockables: [],
  }));
  const id = "11111111-1111-4111-8111-111111111111";
  await write(join(root, "Content/UI/Title.pxui"), JSON.stringify({
    format: "PrismatiXUIScene", schemaRevision: 1, id, revision: 1,
    name: "Title", width: 1280, height: 720, rootId: id,
    nodes: [{id, parentId: null, order: 0, kind: "control", runtimeType: "Control",
      name: "Root", visible: true, locked: false,
      layout: {mode: "free", x: 0, y: 0, width: 1280, height: 720,
        anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0,
        pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed"},
      runtimeProperties: {}, bindings: {}}], theme: [],
    behaviorGraph: {nodes: [], links: [], groups: []}, behaviorTriggers: [], visualStateGroups: [],
  }));
  await write(join(root, "Content/Localization/en.json"), JSON.stringify({
    format: "PrismatiXLocale", schemaRevision: 1, locale: "en", strings: {},
  }));
  return {root, project};
}

test("validate reports the authored Story index position for a missing required label", async () => {
  const value = await fixture();
  try {
    const indexPath = join(value.root, "Story/story.pxindex");
    const index = JSON.parse(await readFile(indexPath, "utf8")) as {
      scenes: Array<{requiredLabels?: string[]}>;
    };
    index.scenes[0]!.requiredLabels = ["missing-anchor"];
    const source = JSON.stringify(index, null, 2);
    await write(indexPath, source);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {line: number; offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.code === "PXSTORY1307");
    assert.ok(issue?.span, validated.stderr);
    assert.equal(issue.span.path, "Story/story.pxindex");
    assert.equal(source.slice(issue.span.start.offset, issue.span.end.offset),
      '"missing-anchor"');
    assert.ok(issue.span.start.line > 1);
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate reports the authored UI binding position for missing Player data", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as
      Record<string, unknown>;
    project.settings = {"player.uiData": {screen: {title: "Ready"}}};
    await write(value.project, JSON.stringify(project));
    const uiPath = join(value.root, "Content/UI/Title.pxui");
    const ui = JSON.parse(await readFile(uiPath, "utf8")) as {
      nodes: Array<{bindings: Record<string, unknown>}>;
    };
    ui.nodes[0]!.bindings.visible = {path: "screen.missing"};
    const source = JSON.stringify(ui, null, 2);
    await write(uiPath, source);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {line: number; offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.code === "PXBUILD1086");
    assert.ok(issue?.span, validated.stderr);
    assert.equal(issue.span.path, "Content/UI/Title.pxui");
    assert.equal(source.slice(issue.span.start.offset, issue.span.end.offset),
      '"screen.missing"');
    assert.ok(issue.span.start.line > 1);
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("separate CLI builds reuse bounded Story scenes and rebuild a damaged cache", async () => {
  const value = await fixture();
  try {
    const indexPath = join(value.root, "Story/story.pxindex");
    const index = JSON.parse(await readFile(indexPath, "utf8")) as {
      chapters: Array<{scenes: string[]}>;
      scenes: Array<{id: string; sources: Record<string, string>}>;
    };
    index.chapters[0]!.scenes.push("bonus");
    index.scenes.push({id: "bonus", sources: {en: "Story/bonus.pxstory"}});
    await write(indexPath, JSON.stringify(index));
    await write(join(value.root, "Story/bonus.pxstory"), "Bonus.\n[end]\n");
    const output = join(value.root, ".prismatix", "artifact");
    const events = (stderr: string) => stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as Record<string, unknown>]; }
      catch { return []; }
    }).filter((event) => event.code === "PXCLI1103" && event.phase === "story-compile");
    const build = () => run(["build", value.project, "--artifact-only", "--output", output]);
    const first = build();
    assert.equal(first.status, 0, first.stderr);
    assert.deepEqual(events(first.stderr).map((event) => event.cached),
      [undefined, undefined]);
    const cachePath = join(value.root, ".prismatix", "cache", "story-scenes-v1.json.gz");
    const firstCache = await stat(cachePath);
    assert.ok(firstCache.size > 0);

    const second = build();
    assert.equal(second.status, 0, second.stderr);
    assert.deepEqual(events(second.stderr).map((event) => event.cached), [true, true]);
    assert.equal((await stat(cachePath)).mtimeMs, firstCache.mtimeMs,
      "a full cache hit does not recompress or rewrite the cache");

    await write(join(value.root, "Story/main.pxstory"), "Edited dialogue.\n[end]\n");
    const third = build();
    assert.equal(third.status, 0, third.stderr);
    assert.deepEqual(events(third.stderr).map((event) => event.cached),
      [undefined, true]);
    const ir = JSON.parse(await readFile(join(output, "Runtime/main.pxir"), "utf8")) as {
      operations: Array<{text: string}>;
    };
    assert.ok(ir.operations.some((operation) => operation.text === "Edited dialogue."));

    const stale = JSON.parse(gunzipSync(await readFile(cachePath)).toString("utf8")) as {
      compiler: string;
    };
    stale.compiler = "older compiler";
    await writeFile(cachePath, gzipSync(JSON.stringify(stale)));
    const staleBuild = build();
    assert.equal(staleBuild.status, 0, staleBuild.stderr);
    assert.deepEqual(events(staleBuild.stderr).map((event) => event.cached),
      [undefined, undefined]);

    await writeFile(cachePath, "damaged cache");
    const fourth = build();
    assert.equal(fourth.status, 0, fourth.stderr);
    assert.deepEqual(events(fourth.stderr).map((event) => event.cached),
      [undefined, undefined]);
    assert.ok((await stat(cachePath)).size > "damaged cache".length);
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate rejects malformed UTF-8 Story bytes before compilation", async () => {
  const value = await fixture();
  try {
    const source = join(value.root, "Story/main.pxstory");
    await writeFile(source, Buffer.from([0xc3, 0x28]));
    const result = run(["validate", value.project]);
    assert.equal(result.status, 1);
    assert.match(result.stderr, /encoded data|UTF-8/iu);
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("create --empty starts without the sample character, backgrounds or story", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-create-empty-"));
  const root = join(parent, "blank-game");
  try {
    await createStarter(root, {name: "Blank", locale: "zh-TW", empty: true, install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const project = join(root, "prismatix.json");
    const validated = run(["validate", project]);
    assert.equal(validated.status, 0, validated.stderr);
    const manifest = JSON.parse(await readFile(project, "utf8")) as {characters?: unknown; assets: Array<{kind: string}>};
    assert.equal(manifest.characters, undefined);
    assert.deepEqual(manifest.assets.map((asset) => asset.kind), ["font", "license", "license", "provenance"]);
    assert.deepEqual(await readdir(join(root, "Assets")), []);
    // The provenance still covers the font and the logo, and only them.
    const provenance = await readFile(join(root, "ASSET_PROVENANCE.md"), "utf8");
    assert.match(provenance, /^## Font$[\s\S]*NotoSansTC-Bold\.ttf[\s\S]*^## PrismatiX logo$/mu);
    assert.doesNotMatch(provenance, /street|shirone|koharu|samples only/u);
    assert.doesNotMatch(await readFile(join(root, "README.md"), "utf8"), /two characters are AI-generated/u);
    // An empty project still opens with the PrismatiX startup screen.
    assert.deepEqual((await readdir(join(root, "Content/UI/PrismatiX"))).sort(),
      ["NOTICE.md", "PrismatiXEngine_Logo.png"]);
    await assert.rejects(stat(join(root, "Characters")));
    assert.doesNotMatch(await readFile(join(root, "Content/UI/Title.tsx"), "utf8"), /Assets\//u);
    const story = await readFile(join(root, "Story/main.pxstory"), "utf8");
    assert.doesNotMatch(story, /\[bg|\[show|^@/mu);
    assert.match(story, /故事從這裡開始。/u);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("create starts with the PrismatiX splash, a Studio screen the author can edit, replace or remove", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-create-splash-"));
  const root = join(parent, "splash-game");
  try {
    await createStarter(root, {name: "Splash", locale: "zh-TW", empty: true, width: 1920, height: 1080, install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const project = join(root, "prismatix.json");
    type Manifest = {uiEntryPoints: Record<string, string>; settings?: Record<string, unknown>};
    const manifest = JSON.parse(await readFile(project, "utf8")) as Manifest;
    assert.deepEqual(manifest.uiEntryPoints, {splash: "Content/UI/Splash.tsx", title: "Content/UI/Title.tsx"});
    assert.deepEqual(manifest.settings?.["player.splashes"],
      [{scene: "Content/UI/Splash.tsx", minimumDuration: 2.8, skippable: true, skipAllowedAfter: 0.4}]);
    // The Studio owns the screen: a layout beside TSX generated from it.
    const tsx = await readFile(join(root, "Content/UI/Splash.tsx"), "utf8");
    assert.match(tsx, /^\/\/ Generated by PrismatiX Studio from Splash\.studio\.json\./u);
    assert.match(tsx, /width=\{1920\}/u);
    assert.match(tsx, /name: "enter"[\s\S]*name: "exit"/u);
    const layout = JSON.parse(await readFile(join(root, "Content/UI/Splash.studio.json"), "utf8")) as
      {format: string; scene: {width: number; height: number}};
    assert.equal(layout.format, "PrismatiXStudioLayout");
    assert.deepEqual([layout.scene.width, layout.scene.height], [1920, 1080]);
    // The engine's own logo, byte for byte, on a black backdrop.
    assert.deepEqual(await readFile(join(root, "Content/UI/PrismatiX/PrismatiXEngine_Logo.png")),
      await readFile("../../Resources/Branding/PrismatiXEngine_Logo.png"));
    assert.match(tsx, /<ColorRect name="Backdrop" fill color="#000000FF" \/>/u);
    const validated = run(["validate", project]);
    assert.equal(validated.status, 0, validated.stdout + validated.stderr);
    assert.doesNotMatch(validated.stdout + validated.stderr, /PXBUILD1055|PXBOOT/u);
    // Taken out of player.splashes it is an ordinary unused screen...
    const unlisted = {...manifest, settings: {}};
    await writeFile(project, JSON.stringify(unlisted, null, 2));
    const withoutSplash = run(["validate", project]);
    assert.equal(withoutSplash.status, 0, withoutSplash.stdout + withoutSplash.stderr);
    // ...and with its route gone too nothing refers to it.
    await writeFile(project, JSON.stringify({...unlisted, uiEntryPoints: {title: "Content/UI/Title.tsx"}}, null, 2));
    await rm(join(root, "Content/UI/Splash.tsx"));
    await rm(join(root, "Content/UI/Splash.studio.json"));
    const removed = run(["validate", project]);
    assert.equal(removed.status, 0, removed.stdout + removed.stderr);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("a project without storyIndex builds the scenes `add scene` lists in Story/story.pxindex", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-default-index-"));
  const root = join(parent, "scenes-game");
  try {
    await createStarter(root, {name: "Scenes", locale: "zh-TW", empty: true, install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const project = join(root, "prismatix.json");
    assert.equal((JSON.parse(await readFile(project, "utf8")) as {storyIndex?: string}).storyIndex, undefined);
    await write(join(root, "Story/story.pxindex"), JSON.stringify({
      format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "main",
      chapters: [{id: "chapter-1", title: "Chapter 1", scenes: ["main", "day2"]}],
      scenes: [{id: "main", sources: {"zh-TW": "Story/main.pxstory"}},
        {id: "day2", sources: {"zh-TW": "Story/zh-TW/day2.pxstory"}}],
    }));
    // A fresh Story without save identities (the starter's map is for its own text).
    await rm(join(root, "Story/main.pxstory.pxids.json"), {force: true});
    await write(join(root, "Story/main.pxstory"), "第一天。\n[jump morning scene=day2]\n");
    await write(join(root, "Story/zh-TW/day2.pxstory"), "*morning\n第二天。\n[end]\n");
    const validated = run(["validate", project]);
    assert.equal(validated.status, 0, validated.stdout + validated.stderr);
    // Both scenes are compiled (progress is reported on stderr).
    const compiled = validated.stderr.split("\n").filter((line) => line.includes("\"phase\":\"story-compile\""));
    assert.equal(compiled.length, 2, validated.stderr);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("a project without gameCatalog builds the conventional Content/game.pxgame", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-default-catalog-"));
  const root = join(parent, "catalog-game");
  try {
    await createStarter(root, {name: "Catalog", locale: "zh-TW", empty: true, install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const project = join(root, "prismatix.json");
    assert.equal((JSON.parse(await readFile(project, "utf8")) as {gameCatalog?: string}).gameCatalog, undefined);
    const catalog = (route: string) => JSON.stringify({
      format: "PrismatiXGame", schemaRevision: 1, variables: [], gallery: [], unlockables: [],
      inputActions: [{id: "menu", context: "game", route}],
    });
    // A route the catalog names is checked, so the file was read...
    await write(join(root, "Content/game.pxgame"), catalog("no-such-screen"));
    const rejected = run(["validate", project]);
    assert.equal(rejected.status, 1, rejected.stdout + rejected.stderr);
    assert.match(rejected.stdout + rejected.stderr, /PXBUILD1061/u);
    // ...and a real one validates.
    await write(join(root, "Content/game.pxgame"), catalog("title"));
    const validated = run(["validate", project]);
    assert.equal(validated.status, 0, validated.stdout + validated.stderr);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("create scaffolds a canonical playable starter without author-supplied UUIDs", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-create-"));
  const root = join(parent, "hello-game");
  await createStarter(root, {name: "Hello Game", locale: "zh-TW", install: async (directory) => {
    await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
    await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
    await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
  }}); // Actual CLI/npm installation is covered by phase1-clean-install.

  const project = join(root, "prismatix.json");
  const validated = run(["validate", project]);
  assert.equal(validated.status, 0, validated.stderr);
  const ui = await readFile(join(root, "Content/UI/Title.tsx"), "utf8");
  assert.match(ui, /const title = "Hello Game";/u);
  assert.doesNotMatch(ui, /\{\{[A-Z_]+\}\}/u);
  const manifest = JSON.parse(await readFile(project, "utf8")) as Record<string, unknown>;
  assert.deepEqual(Object.keys(manifest).sort(),
    ["assets", "characters", "defaultLocale", "id", "name", "resolution", "settings", "storyAssets", "uiEntryPoints"]);
  assert.deepEqual((manifest.characters as Array<{alias: string; id?: string}>)
    .map((character) => [character.alias, character.id]), [["shirone", undefined], ["koharu", undefined]]);
  assert.equal((manifest.assets as Array<{source: string}>).some((asset) =>
    asset.source === "Assets/Characters/shirone/normal.png"), false);
  const authorCharacter = JSON.parse(await readFile(join(root,
    "Characters/shirone.pxcharacter"), "utf8")) as {
    id?: string; nameColor: string; stage: {scale: number};
    expressions: Array<{id?: string; assetId?: string; assetPath?: string}>;
  };
  assert.equal(authorCharacter.id, undefined);
  assert.equal(authorCharacter.expressions[0]!.id, undefined);
  assert.equal(authorCharacter.expressions[0]!.assetId, undefined);
  assert.equal(authorCharacter.expressions[0]!.assetPath, "Assets/Characters/shirone/normal.png");
  // The 1472-pixel portrait stands at about 95% of the default 720 lines.
  assert.equal(authorCharacter.stage.scale, 0.46);
  assert.equal(authorCharacter.nameColor, "#8FD6DC");
  const artifact = join(root, "artifact");
  const built = run(["build", project, "--artifact-only", "--output", artifact]);
  assert.equal(built.status, 0, built.stderr);
  const canonicalCharacter = JSON.parse(await readFile(join(artifact,
    "Characters/shirone.pxcharacter"), "utf8")) as {
    id: string; expressions: Array<{id: string; assetId: string; assetPath?: string}>;
  };
  assert.match(canonicalCharacter.id, /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/u);
  assert.match(canonicalCharacter.expressions[0]!.assetId,
    /^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/u);
  assert.equal(canonicalCharacter.expressions[0]!.assetPath, undefined);
  assert.equal((await stat(join(artifact, "Assets/Characters/shirone/normal.png"))).isFile(), true);
  assert.equal((await stat(join(root, "Content/Localization/zh-TW.json"))).isFile(), true);
  assert.deepEqual((await readdir(join(root, "Assets"), {recursive: true})).map((path) => path.replaceAll("\\", "/")).sort(),
    ["Backgrounds", "Backgrounds/street.jpg", "Characters", "Characters/koharu", "Characters/koharu/normal.png",
      "Characters/shirone", "Characters/shirone/normal.png"]);
  const story = await readFile(join(root, "Story/main.pxstory"), "utf8");
  assert.match(story, /^; @prismatix-ids\r?\n/u);
  assert.doesNotMatch(story, /\[id\s/u);
  assert.match(story, /\[bg street duration=1s\]/u);
  assert.equal((await stat(join(root, "Story/main.pxstory.pxids.json"))).isFile(), true);
});

test("runtime discovery accepts an installed prefix manifest and reports attempted sources", async () => {
  const prefix = await mkdtemp(join(tmpdir(), "prismatix-runtime-prefix-"));
  const bin = join(prefix, "bin");
  const manifest = join(prefix, "share", "PrismatiX", "runtime-manifest.json");
  await writeExecutable(join(bin, process.platform === "win32" ? "PrismatiXPreviewHost.exe" : "PrismatiXPreviewHost"), "fixture");
  await write(manifest, JSON.stringify({format: "PrismatiXRuntimeManifest", schemaRevision: 1,
    version: "0.1.0", target: `${process.platform}-${process.arch}`, binDir: "../../bin", tools: {
      previewHost: process.platform === "win32" ? "PrismatiXPreviewHost.exe" : "PrismatiXPreviewHost",
    }}));
  const discovered = await discoverRuntimeTool("previewHost", {runtime: prefix, environment: {}});
  assert.equal(discovered.path, resolve(bin,
    process.platform === "win32" ? "PrismatiXPreviewHost.exe" : "PrismatiXPreviewHost"));
  await assert.rejects(discoverRuntimeTool("player", {runtime: prefix, environment: {}}),
    (error: unknown) => error instanceof RuntimeDiscoveryFailure &&
      error.attempts.some((attempt) => attempt.includes("runtime-manifest.json")));
});

test("validate and build reject native UI property type mismatches before packaging", async () => {
  const value = await fixture();
  const uiPath = join(value.root, "Content/UI/Title.pxui");
  const ui = JSON.parse(await readFile(uiPath, "utf8")) as {
    nodes: Array<{runtimeProperties: Record<string, unknown>}>
  };
  ui.nodes[0]!.runtimeProperties.modulate = "#2D171DFF";
  const wrongColorSource = JSON.stringify(ui, null, 2);
  await write(uiPath, wrongColorSource);
  const registryPath = join(value.root, "PrismatiXUiTypeRegistry.json");
  await write(registryPath, JSON.stringify({
    format: "PrismatiXUiTypeRegistry", schemaRevision: 1,
    controls: [{runtimeType: "Control", properties: [
      {id: "modulate", valueType: "color",
        writable: true, bindable: true, animatable: true},
      {id: "visibility", valueType: "string", writable: true,
        bindable: true, animatable: false,
        enumChoices: ["Visible", "Hidden", "Collapsed"]},
    ]}],
  }));
  const invalid = run(["validate", value.project, "--ui-registry", registryPath]);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /PXSDKSEM1324/u);
  const wrongColor = invalid.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
    try { return [JSON.parse(line) as {code: string; span?: {start: {offset: number};
      end: {offset: number}}}]; } catch { return []; }
  }).find((entry) => entry.code === "PXSDKSEM1324");
  assert.ok(wrongColor?.span, invalid.stderr);
  assert.equal(wrongColorSource.slice(wrongColor.span.start.offset,
    wrongColor.span.end.offset), '"#2D171DFF"');
  const rejectedBuild = run(["build", value.project, "--artifact-only", "--output",
    join(value.root, "invalid-build"), "--ui-registry", registryPath]);
  assert.equal(rejectedBuild.status, 1);
  assert.match(rejectedBuild.stderr, /PXSDKSEM1324/u);

  ui.nodes[0]!.runtimeProperties.modulate = {type: "color", value: "#2D171DFF"};
  await write(uiPath, JSON.stringify(ui));
  const valid = run(["validate", value.project, "--ui-registry", registryPath]);
  assert.equal(valid.status, 0, valid.stderr);
  ui.nodes[0]!.runtimeProperties.visibility = "Invisible";
  await write(uiPath, JSON.stringify(ui));
  const invalidEnum = run(["validate", value.project, "--ui-registry", registryPath]);
  assert.equal(invalidEnum.status, 1);
  assert.match(invalidEnum.stderr, /PXSDKSEM1326/u);
  ui.nodes[0]!.runtimeProperties.visibility = "Hidden";
  await write(uiPath, JSON.stringify(ui));
  const validEnum = run(["validate", value.project, "--ui-registry", registryPath]);
  assert.equal(validEnum.status, 0, validEnum.stderr);
  ui.nodes[0]!.runtimeProperties.wheelThreshold = 101;
  const wrongWheelSource = JSON.stringify(ui, null, 2);
  await write(uiPath, wrongWheelSource);
  const invalidWheel = run(["validate", value.project]);
  assert.equal(invalidWheel.status, 1);
  assert.match(invalidWheel.stderr, /PXBUILD1073/u);
  const wrongWheel = invalidWheel.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
    try { return [JSON.parse(line) as {code: string; span?: {start: {offset: number};
      end: {offset: number}}}]; } catch { return []; }
  }).find((entry) => entry.code === "PXBUILD1073");
  assert.ok(wrongWheel?.span, invalidWheel.stderr);
  assert.equal(wrongWheelSource.slice(wrongWheel.span.start.offset,
    wrongWheel.span.end.offset), "101");
});

test("validate rejects native-invalid UI bindings and typed values before packaging", async () => {
  const value = await fixture();
  const uiPath = join(value.root, "Content/UI/Title.pxui");
  const ui = JSON.parse(await readFile(uiPath, "utf8")) as {
    nodes: Array<{bindings: Record<string, unknown>;
      runtimeProperties: Record<string, unknown>}>
  };
  ui.nodes[0]!.bindings.visible = {path: "saveLoad.slots[00].enabled"};
  await write(uiPath, JSON.stringify(ui));
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 1);
  assert.match(validated.stderr, /Content\/UI\/Title\.pxui/u);
  const built = run(["build", value.project, "--artifact-only",
    "--output", join(value.root, "artifact")]);
  assert.equal(built.status, 1);
  assert.equal((await readdir(value.root)).includes("artifact"), false);
  ui.nodes[0]!.bindings = {};
  ui.nodes[0]!.runtimeProperties.translation = {type:"vec2",x:0,y:"bottom"};
  await write(uiPath, JSON.stringify(ui));
  const invalidValue = run(["validate", value.project]);
  assert.equal(invalidValue.status, 1);
  assert.match(invalidValue.stderr, /PXSDKSEM1325/u);
  assert.match(invalidValue.stderr, /runtimeProperties\.translation/u);
});

test("validate and artifact build use the selected local Runtime UI registry", async () => {
  const value = await fixture();
  const uiPath = join(value.root, "Content/UI/Title.pxui");
  const ui = JSON.parse(await readFile(uiPath, "utf8")) as {
    nodes: Array<{runtimeProperties: Record<string, unknown>}>
  };
  ui.nodes[0]!.runtimeProperties.modulate = "#2D171DFF";
  await write(uiPath, JSON.stringify(ui));
  const runtime = join(value.root, "native-build");
  const registry = join(runtime, "PrismatiXUiTypeRegistry.json");
  await write(registry, JSON.stringify({format: "PrismatiXUiTypeRegistry", schemaRevision: 1,
    controls: [{runtimeType: "Control", properties: [{id: "modulate", valueType: "color",
      writable: true, bindable: true, animatable: true}]}]}));
  const invalid = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /PXSDKSEM1324/u);
  const rejectedBuild = run(["build", value.project, "--artifact-only", "--output",
    join(value.root, "invalid-build"), "--runtime", runtime]);
  assert.equal(rejectedBuild.status, 1);
  assert.match(rejectedBuild.stderr, /PXSDKSEM1324/u);

  const missing = join(value.root, "missing-registry");
  await mkdir(missing);
  const unverified = run(["validate", value.project, "--runtime", missing]);
  assert.equal(unverified.status, 1);
  assert.match(unverified.stderr, /UI TypeRegistry manifest was not found/u);

  const exporter = join(runtime, process.platform === "win32"
    ? "PrismatiXUiTypeManifest.exe" : "PrismatiXUiTypeManifest");
  await write(exporter, "fixture");
  await utimes(registry, new Date(0), new Date(0));
  const stale = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(stale.status, 1);
  assert.match(stale.stderr, /older than its native exporter/u);
});

test("validate and build reject keyboard names absent from the selected native Runtime", async () => {
  const value = await fixture();
  const gamePath = join(value.root, "Content/game.pxgame");
  const game = JSON.parse(await readFile(gamePath, "utf8")) as Record<string, unknown>;
  game.inputActions = [{id: "quick-save", context: "game", keys: ["F88"]}];
  game.inputBindings = [{key: "Escape", command: "screen.open", argument: "settings"}];
  await write(gamePath, JSON.stringify(game));
  const runtime = join(value.root, "native-build");
  await write(join(runtime, "PrismatiXUiTypeRegistry.json"), JSON.stringify({
    format: "PrismatiXUiTypeRegistry", schemaRevision: 1, controls: [],
  }));
  const keyManifest = join(runtime, "PrismatiXInputKeyRegistry.json");
  await write(keyManifest, JSON.stringify({format: "PrismatiXInputKeyRegistry",
    schemaRevision: 1, keys: ["Enter", "Escape", "F8"]}));
  const invalid = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /PXBUILD1068/u);
  assert.match(invalid.stderr, /F88/u);
  const rejectedBuild = run(["build", value.project, "--artifact-only", "--output",
    join(value.root, "invalid-build"), "--runtime", runtime]);
  assert.equal(rejectedBuild.status, 1);
  assert.match(rejectedBuild.stderr, /PXBUILD1068/u);

  game.inputActions = [{id: "quick-save", context: "game", keys: ["F8"]}];
  await write(gamePath, JSON.stringify(game));
  const valid = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(valid.status, 0, valid.stderr);
  assert.doesNotMatch(valid.stderr, /PXCLI1043/u);

  await rm(keyManifest);
  const oldRuntime = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(oldRuntime.status, 0, oldRuntime.stderr);
  assert.match(oldRuntime.stderr, /PXCLI1043/u);
  const exporter = join(runtime, process.platform === "win32"
    ? "PrismatiXInputKeyManifest.exe" : "PrismatiXInputKeyManifest");
  await write(exporter, "fixture");
  const missing = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(missing.status, 1);
  assert.match(missing.stderr, /Input key manifest was not found/u);
  await write(keyManifest, JSON.stringify({format: "PrismatiXInputKeyRegistry",
    schemaRevision: 1, keys: ["Enter", "Escape", "F8"]}));
  await utimes(keyManifest, new Date(0), new Date(0));
  const stale = run(["validate", value.project, "--runtime", runtime]);
  assert.equal(stale.status, 1);
  assert.match(stale.stderr, /Input key manifest is older than its native exporter/u);
});

test("pack checks keyboard defaults against its selected Packager's native key manifest", async () => {
  const value = await fixture();
  const gamePath = join(value.root, "Content/game.pxgame");
  const game = JSON.parse(await readFile(gamePath, "utf8")) as Record<string, unknown>;
  game.inputActions = [{id: "quick-save", context: "game", keys: ["F88"]}];
  await write(gamePath, JSON.stringify(game));
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-native-keys-"));
  const packager = join(tools, "mock-packager.mjs");
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await write(packager, "process.exit(0);\n");
  await writeExecutable(player, "fixture");
  await write(join(tools, "PrismatiXInputKeyRegistry.json"), JSON.stringify({
    format: "PrismatiXInputKeyRegistry", schemaRevision: 1, keys: ["F8"],
  }));
  const packed = run(["pack", value.project, "--packager", packager, "--player", player,
    "--output", join(value.root, "package")]);
  assert.equal(packed.status, 1);
  assert.match(packed.stderr, /PXBUILD1068/u);
  assert.doesNotMatch(packed.stderr, /PXCLI1043/u);
});

test("dev --once streams PreviewHost protocol output and exits after a clean shutdown", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-preview-"));
  const preview = join(tools, "mock-preview.mjs");
  await write(preview, `
    import {createInterface} from "node:readline";
    const input = createInterface({input: process.stdin});
    input.on("line", (line) => {
      const request = JSON.parse(line);
      const types = {hello: "ready", applyRuntimeIr: "runtimeIrApplied",
        patchRuntimeIr: "runtimeIrApplied", play: "playAccepted", shutdown: "shutdownAccepted"};
      const type = types[request.type];
      if (!type) process.exit(71);
      process.stdout.write(JSON.stringify({...request, type, runtimeIrSchemaRevision: 1, uiSchemaRevision: 1,
        ...(type === "runtimeIrApplied" ? {applyMode: "fullRuntimeRestart", appliedRevision: request.revision} : {})}) + "\\n");
      if (request.type === "shutdown") { input.close(); process.exitCode = 0; }
    });
  `);
  const previewed = run(["dev", value.root, "--preview-host", preview, "--once"]);
  assert.equal(previewed.status, 0, previewed.stderr);
  assert.match(previewed.stdout, /"type":"ready"/u);
  assert.match(previewed.stdout, /"type":"runtimeIrApplied"/u);
  assert.match(previewed.stdout, /"type":"playAccepted"/u);
  assert.match(previewed.stdout, /"type":"shutdownAccepted"/u);
  assert.match(previewed.stderr, /PXCLI1602/u);
  const event = previewed.stderr.split(/\r?\n/u).filter(Boolean).map((line) => JSON.parse(line)).find((value) => value.code === "PXCLI1602");
  assert.equal((await stat(join(event.output, "Runtime", "main.pxir"))).isFile(), true);
});

test("dev --json streams diagnostics and rebuild/Preview lifecycle as NDJSON records", async () => {
  const value = await fixture();
  const preview = join(value.root, "mock-preview.mjs");
  await write(preview, `
    import {createInterface} from "node:readline";
    const input = createInterface({input: process.stdin});
    input.on("line", (line) => {
      const request = JSON.parse(line);
      const type = {hello: "ready", applyRuntimeIr: "runtimeIrApplied", play: "playAccepted",
        shutdown: "shutdownAccepted"}[request.type];
      process.stdout.write(JSON.stringify({...request, type, runtimeIrSchemaRevision: 1, uiSchemaRevision: 1,
        applyMode: "fullRuntimeRestart", appliedRevision: request.revision}) + "\\n");
      if (request.type === "shutdown") { input.close(); process.exit(0); }
    });
  `);
  const records = (stderr: string) => stderr.split(/\r?\n/u).filter(Boolean)
    .map((line) => JSON.parse(line) as Record<string, unknown>);
  const lifecycle = (items: Record<string, unknown>[]) => items
    .filter((item) => String(item.event).startsWith("dev."))
    .map((item) => `${String(item.event)}:${String(item.phase ?? "")}`);

  const ok = run(["dev", value.root, "--preview-host", preview, "--once"]);
  assert.equal(ok.status, 0, ok.stderr);
  const okRecords = records(ok.stderr);
  // Every stderr line is one JSON record with an event, a severity and a code.
  for (const item of okRecords) {
    assert.equal(typeof item.event, "string", JSON.stringify(item));
    assert.equal(typeof item.severity, "string", JSON.stringify(item));
  }
  assert.deepEqual(lifecycle(okRecords), ["dev.rebuild:start", "dev.rebuild:done",
    "dev.preview:start", "dev.watching:", "dev.preview:exit"]);
  const done = okRecords.find((item) => item.event === "dev.rebuild" && item.phase === "done")!;
  assert.deepEqual([done.code, done.revision, done.valid], ["PXCLI1612", 1, true]);
  assert.equal(typeof done.durationMs, "number");
  const exit = okRecords.find((item) => item.event === "dev.preview" && item.phase === "exit")!;
  assert.deepEqual([exit.code, exit.revision, exit.requested, exit.exitCode], ["PXCLI1614", 1, true, 0]);
  assert.ok(okRecords.some((item) => item.event === "progress" && item.code === "PXCLI1103"));

  await write(join(value.root, "Story/main.pxstory"), "[nosuchcommand]\n[end]\n");
  const failed = run(["dev", value.root, "--preview-host", preview, "--once"]);
  assert.equal(failed.status, 1);
  const failedRecords = records(failed.stderr);
  const story = failedRecords.find((item) => item.code === "PXSTORY1199")!;
  assert.equal(story.event, "diagnostic");
  assert.equal(story.severity, "error");
  assert.equal((story.span as {path: string}).path, "Story/main.pxstory");
  assert.deepEqual(lifecycle(failedRecords), ["dev.rebuild:start", "dev.rebuild:done"]);
  assert.equal(failedRecords.find((item) => item.event === "dev.rebuild" && item.phase === "done")!.valid, false);

  // Without --json the human output stays plain text.
  const human = runHuman(["dev", value.root, "--preview-host", preview, "--once"]);
  assert.equal(human.status, 1);
  assert.match(human.stderr, /error PXSTORY1199/u);
  assert.doesNotMatch(human.stderr, /dev\.rebuild|"event"/u);
});

test("dev --route shows one registered UI screen instead of the title", async () => {
  const value = await fixture();
  const preview = join(value.root, "mock-preview.mjs");
  const log = join(value.root, "requests.jsonl");
  await write(preview, `
    import {createInterface} from "node:readline";
    import {appendFileSync} from "node:fs";
    const input = createInterface({input: process.stdin});
    input.on("line", (line) => {
      const request = JSON.parse(line);
      appendFileSync(${JSON.stringify(log)}, line + "\\n");
      const type = {hello: "ready", applyRuntimeIr: "runtimeIrApplied", play: "playAccepted",
        applyUiScene: "uiApplied", shutdown: "shutdownAccepted"}[request.type];
      process.stdout.write(JSON.stringify({...request, type, runtimeIrSchemaRevision: 1, uiSchemaRevision: 1,
        applyMode: "fullRuntimeRestart", appliedRevision: request.revision}) + "\\n");
      if (request.type === "shutdown") { input.close(); process.exit(0); }
    });
  `);
  const shown = run(["dev", value.root, "--preview-host", preview, "--once", "--route", "title"]);
  assert.equal(shown.status, 0, shown.stderr);
  const requests = (await readFile(log, "utf8")).trim().split(/\r?\n/u)
    .map((line) => JSON.parse(line) as Record<string, unknown>);
  assert.deepEqual(requests.map((request) => request.type),
    ["hello", "applyRuntimeIr", "play", "applyUiScene", "shutdown"]);
  assert.equal(requests[2]!.showTitle, false);
  const title = JSON.parse(await readFile(join(value.root, "Content/UI/Title.pxui"), "utf8")) as
    {id: string; revision: number};
  const apply = requests[3]!;
  assert.deepEqual([apply.sceneId, apply.revision, apply.uiPath], [title.id, title.revision, "Content/UI/Title.pxui"]);
  assert.equal((await stat(join(String(apply.projectRoot), "Content/UI/Title.pxui"))).isFile(), true);
  const route = shown.stderr.split(/\r?\n/u).filter(Boolean).map((line) => JSON.parse(line) as Record<string, unknown>)
    .find((record) => record.code === "PXCLI1615");
  assert.deepEqual([route?.event, route?.route], ["dev.route", "title"]);

  const unknown = run(["dev", value.root, "--preview-host", preview, "--once", "--route", "settings"]);
  assert.equal(unknown.status, 1);
  assert.match(unknown.stderr, /--route settings names no uiEntryPoints route; this project registers title/u);
  const both = run(["dev", value.root, "--preview-host", preview, "--once", "--route", "title", "--at", "main:1"]);
  assert.equal(both.status, 1);
  assert.match(both.stderr, /mutually exclusive/u);
});

test("dev reuses verified media and Story inputs without hiding changes", {timeout: 50_000}, async (t) => {
  const value = await fixture();
  const indexPath = join(value.root, "Story/story.pxindex");
  const index = JSON.parse(await readFile(indexPath, "utf8")) as {
    chapters: Array<{scenes: string[]}>;
    scenes: Array<{id: string; sources: Record<string, string>}>;
  };
  index.chapters[0]!.scenes.push("bonus");
  index.scenes.push({id: "bonus", sources: {en: "Story/bonus.pxstory"}});
  await write(indexPath, JSON.stringify(index));
  await write(join(value.root, "Story/bonus.pxstory"), "Bonus scene.\n[end]\n");
  const project = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  project.resourceIncludes = ["Assets/Batch"];
  await write(value.project, JSON.stringify(project));
  const mediaPath = join(value.root, "Assets/Batch/media.bin");
  await write(mediaPath, "media-alpha");
  const host = join(value.root, "mock-preview.mjs");
  const hostLog = join(value.root, ".prismatix", "cache-host.jsonl");
  await write(host, `
    import {createInterface} from "node:readline";
    import {appendFileSync} from "node:fs";
    const input = createInterface({input: process.stdin});
    input.on("line", (line) => {
      const request = JSON.parse(line);
      appendFileSync(${JSON.stringify(hostLog)}, JSON.stringify({pid: process.pid, type: request.type}) + "\\n");
      const type = {hello: "ready", applyRuntimeIr: "runtimeIrApplied",
        play: "playAccepted", shutdown: "shutdownAccepted"}[request.type];
      process.stdout.write(JSON.stringify({...request, type, runtimeIrSchemaRevision: 1,
        uiSchemaRevision: 1, appliedRevision: request.revision}) + "\\n");
      if (request.type === "shutdown") { input.close(); process.exit(0); }
    });
  `);
  const child = spawn(process.execPath, [resolve("dist/src/cli.js"), "dev", value.root,
    "--preview-host", host, "--debounce", "50", "--json"],
  {stdio: ["ignore", "pipe", "pipe"], windowsHide: true});
  let stderr = "";
  child.stderr.on("data", (chunk) => { stderr += String(chunk); });
  child.stdout.resume();
  t.after(async () => {
    if (child.exitCode === null && child.signalCode === null) child.kill("SIGTERM");
    await new Promise<void>((done) => {
      if (child.exitCode !== null || child.signalCode !== null) done();
      else child.once("exit", () => done());
    });
    try {
      const log = await readFile(hostLog, "utf8");
      const pids = new Set(log.trim().split(/\r?\n/u).filter(Boolean)
        .map((line) => Number((JSON.parse(line) as {pid: number}).pid)));
      for (const pid of pids) { try { process.kill(pid); } catch { /* host already closed */ } }
    } catch { /* host did not start */ }
    await rm(value.root, {recursive: true, force: true, maxRetries: 10, retryDelay: 100});
  });
  const events = () => stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
    try { return [JSON.parse(line) as Record<string, unknown>]; }
    catch { return []; }
  });
  const waitForRevision = async (revision: number) => {
    const deadline = Date.now() + 20_000;
    while (Date.now() < deadline) {
      const builds = events().filter((event) => event.code === "PXCLI1602");
      if (builds.length >= revision) return builds[revision - 1]!;
      if (child.exitCode !== null) break;
      await new Promise((done) => setTimeout(done, 50));
    }
    assert.fail(`dev did not publish revision ${revision}: ${stderr}`);
  };
  const first = await waitForRevision(1);
  const firstIr = JSON.parse(await readFile(join(String(first.output), "Runtime/main.pxir"), "utf8")) as {
    operations: unknown[]; committedRevision: number;
  };

  const uiPath = join(value.root, "Content/UI/Title.pxui");
  const ui = JSON.parse(await readFile(uiPath, "utf8")) as Record<string, unknown>;
  await write(uiPath, JSON.stringify({...ui, name: "Edited title"}));
  const second = await waitForRevision(2);
  const secondIr = JSON.parse(await readFile(join(String(second.output), "Runtime/main.pxir"), "utf8")) as {
    operations: unknown[]; committedRevision: number;
  };
  assert.deepEqual(secondIr.operations, firstIr.operations);
  assert.equal(secondIr.committedRevision, 2);
  const secondCopy = events().filter((event) => event.phase === "copy" && event.done)[1]!;
  assert.equal(secondCopy.reusedFiles, 1);
  assert.equal(secondCopy.reusedBytes, Buffer.byteLength("media-alpha"));
  assert.equal(await readFile(join(String(second.output), "Assets/Batch/media.bin"), "utf8"),
    "media-alpha");
  const storyEvents = events().filter((event) => event.code === "PXCLI1103");
  assert.equal(storyEvents.length, 8);
  assert.deepEqual(storyEvents.slice(4).map((event) => event.cached),
    [true, true, true, true]);

  await write(join(value.root, "Story/main.pxstory"), "Edited dialogue.\n[end]\n");
  const third = await waitForRevision(3);
  const thirdIr = JSON.parse(await readFile(join(String(third.output), "Runtime/main.pxir"), "utf8")) as {
    operations: Array<{text: string}>; committedRevision: number;
  };
  assert.ok(thirdIr.operations.some((operation) => operation.text === "Edited dialogue."));
  assert.equal(thirdIr.committedRevision, 3);
  assert.deepEqual(events().filter((event) => event.code === "PXCLI1103")
    .slice(8).map((event) => event.cached),
    [undefined, true, undefined, true]);

  await write(mediaPath, "media-bravo"); // same size; reuse must compare content
  const fourth = await waitForRevision(4);
  assert.equal(await readFile(join(String(fourth.output), "Assets/Batch/media.bin"), "utf8"),
    "media-bravo");
  const fourthCopy = events().filter((event) => event.phase === "copy" && event.done)[3]!;
  assert.equal(fourthCopy.reusedFiles, 0);
  assert.equal(fourthCopy.reusedBytes, 0);
});

test("ids defaults to readable Story and external stable identities with a backup", async () => {
  const value = await fixture();
  const checkedBefore = run(["ids", value.root, "--check"]);
  assert.equal(checkedBefore.status, 1, checkedBefore.stdout);
  const before = JSON.parse(checkedBefore.stdout) as {dryRun: boolean; fallback: number};
  assert.equal(before.dryRun, true);
  assert.ok(before.fallback > 0);

  const backup = join(value.root, "identity-backup");
  const written = run(["ids", value.root, "--write", "--backup", backup]);
  assert.equal(written.status, 0, written.stderr);
  const report = JSON.parse(written.stdout) as {
    external: boolean; dryRun: boolean; assigned: number; fallback: number;
  };
  assert.equal(report.external, true);
  assert.equal(report.dryRun, false);
  assert.ok(report.assigned > 0);
  assert.equal(report.fallback, 0);
  const sourcePath = join(value.root, "Story", "main.pxstory");
  const story = await readFile(sourcePath, "utf8");
  assert.match(story, /^; @prismatix-ids\r?\n/u);
  assert.doesNotMatch(story, /\[id /u);
  assert.equal((await stat(`${sourcePath}.pxids.json`)).isFile(), true);
  assert.equal((await stat(join(backup, "Story", "main.pxstory"))).isFile(), true);

  const checkedAfter = run(["ids", value.root, "--check"]);
  assert.equal(checkedAfter.status, 0, checkedAfter.stderr);
});

test("ids --inline keeps the legacy source rewrite explicit", async () => {
  const value = await fixture();
  const sourcePath = join(value.root, "Story", "main.pxstory");
  const written = run(["ids", value.root, "--inline", "--write", "--prefix", "legacy"]);
  assert.equal(written.status, 0, written.stderr);
  assert.match(await readFile(sourcePath, "utf8"), /\[id legacy\.[0-9a-f]+\]/u);
  assert.equal((await readdir(join(value.root, "Story"))).some(name => name.endsWith(".pxids.json")), false);
  const conflicting = run(["ids", value.root, "--inline", "--external", "--write"]);
  assert.equal(conflicting.status, 1);
  assert.match(conflicting.stderr, /mutually exclusive/u);
  assert.equal(run(["ids", value.root, "--inline", "--sync"]).status, 1);
  await write(sourcePath, "; @prismatix-ids\nHello.\n[end]\n");
  const missingSidecar = run(["ids", value.root, "--inline", "--write"]);
  assert.equal(missingSidecar.status, 1);
  assert.match(missingSidecar.stderr, /restore its sidecar/u);
  assert.equal(await readFile(sourcePath, "utf8"), "; @prismatix-ids\nHello.\n[end]\n");
});

test("ids --external moves inline anchors to a sidecar without changing compiled operation ids", async () => {
  const value = await fixture();
  const sourcePath = join(value.root, "Story", "main.pxstory");
  await write(sourcePath, "[id pin.hello]\nHello.\n[id pin.end]\n[end]\n");
  const before = join(value.root, "before-artifact");
  assert.equal(run(["build", value.project, "--artifact-only", "--output", before]).status, 0);
  const converted = run(["ids", value.project, "--write"]);
  assert.equal(converted.status, 0, converted.stderr);
  const report = JSON.parse(converted.stdout) as {external: boolean; fallback: number; backup: string};
  assert.equal(report.external, true);
  assert.equal(report.fallback, 0);
  assert.doesNotMatch(await readFile(sourcePath, "utf8"), /\[id /u);
  assert.equal((await stat(`${sourcePath}.pxids.json`)).isFile(), true);
  assert.equal((await stat(join(report.backup, "Story", "main.pxstory"))).isFile(), true);
  assert.equal(run(["ids", value.project, "--check"]).status, 0);
  const after = join(value.root, "after-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", after]);
  assert.equal(built.status, 0, built.stderr);
  const operations = async (root: string) => (JSON.parse(await readFile(join(root, "Runtime/main.pxir"), "utf8")) as {
    operations: Array<{operationId: string; text: string}>;
  }).operations.map(operation => operation.operationId);
  assert.deepEqual(await operations(after), await operations(before));
  assert.equal((await readdir(join(after, "Story"))).some(name => name.endsWith(".pxids.json")), false);
  await write(sourcePath, "Hello.\nNew operation.\n[end]\n");
  const invalid = run(["validate", value.project]);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /PXSTORY1405/u);
});

test("ids --sync previews changes, backs up only sidecars, and preserves published ids", async () => {
  const value = await fixture();
  const sourcePath = join(value.root, "Story", "main.pxstory");
  const mapPath = `${sourcePath}.pxids.json`;
  const externalized = run(["ids", value.project, "--external", "--write"]);
  assert.equal(externalized.status, 0, externalized.stderr);
  const beforeArtifact = join(value.root, "sync-before-artifact");
  assert.equal(run(["build", value.project, "--artifact-only", "--output", beforeArtifact]).status, 0);
  const originalMap = await readFile(mapPath, "utf8");
  const changedSource = "; @prismatix-ids\nHello.\nInserted.\n[end]\n";
  await write(sourcePath, changedSource);
  assert.equal(run(["validate", value.project]).status, 1);

  const preview = run(["ids", value.project, "--sync"]);
  assert.equal(preview.status, 0, preview.stderr);
  const previewReport = JSON.parse(preview.stdout) as {
    dryRun: boolean; changed: number; preserved: number; added: number;
    addedOperations: Array<{path: string; operation: number; line: number;
      kind: string; preview: string; sourceId: string}>;
    removedOperations: unknown[];
  };
  assert.equal(previewReport.dryRun, true);
  assert.equal(previewReport.changed, 1);
  assert.equal(previewReport.preserved, 2);
  assert.equal(previewReport.added, 1);
  assert.deepEqual(previewReport.addedOperations.map(({path, operation, line, kind, preview}) =>
    ({path, operation, line, kind, preview})), [{path: "Story/main.pxstory",
    operation: 2, line: 3, kind: "narration", preview: "Inserted."}]);
  assert.match(previewReport.addedOperations[0]!.sourceId, /^story-[0-9a-f]{16}$/u);
  assert.deepEqual(previewReport.removedOperations, []);
  assert.equal(await readFile(mapPath, "utf8"), originalMap);
  assert.equal(run(["ids", value.project, "--sync", "--check"]).status, 1);

  const backup = join(value.root, "sync-backup");
  const written = run(["ids", value.project, "--sync", "--write", "--backup", backup]);
  assert.equal(written.status, 0, written.stderr);
  const report = JSON.parse(written.stdout) as {backup: string; changed: number};
  assert.equal(report.backup, backup);
  assert.equal(report.changed, 1);
  assert.equal(await readFile(join(backup, "Story", "main.pxstory.pxids.json"), "utf8"), originalMap);
  assert.equal(await readFile(sourcePath, "utf8"), changedSource);
  assert.equal(run(["validate", value.project]).status, 0);
  assert.equal(run(["ids", value.project, "--sync", "--check"]).status, 0);
  const afterArtifact = join(value.root, "sync-after-artifact");
  assert.equal(run(["build", value.project, "--artifact-only", "--output", afterArtifact]).status, 0);
  const operations = async (root: string) => (JSON.parse(await readFile(join(root, "Runtime/main.pxir"), "utf8")) as {
    operations: Array<{operationId: string}>;
  }).operations.map(operation => operation.operationId);
  const beforeIds = await operations(beforeArtifact);
  const afterIds = await operations(afterArtifact);
  assert.deepEqual(afterIds.filter(id => beforeIds.includes(id)), beforeIds);

  const updatedMap = await readFile(mapPath, "utf8");
  await write(sourcePath, "; @prismatix-ids\nInserted.\nHello.\n[end]\n");
  const ambiguous = run(["ids", value.project, "--sync", "--write"]);
  assert.equal(ambiguous.status, 1);
  assert.match(ambiguous.stderr, /PXSTORY1413/u);
  assert.equal(await readFile(mapPath, "utf8"), updatedMap);

  await write(sourcePath, "; @prismatix-ids\nHello.\n[end]\n");
  const removal = run(["ids", value.project, "--sync"]);
  assert.equal(removal.status, 0, removal.stderr);
  const removalReport = JSON.parse(removal.stdout) as {removedOperations:
    Array<{path: string; oldOperation: number; identity: string; kind: string}>};
  assert.equal(removalReport.removedOperations.length, 1);
  assert.equal(removalReport.removedOperations[0]!.path, "Story/main.pxstory");
  assert.equal(removalReport.removedOperations[0]!.oldOperation, 2);
  assert.equal(removalReport.removedOperations[0]!.kind, "narration");
  assert.match(removalReport.removedOperations[0]!.identity, /^source:story-[0-9a-f]{16}$/u);
  assert.equal(await readFile(mapPath, "utf8"), updatedMap);
  assert.equal(run(["ids", value.project, "--sync", "--accept-removals"]).status, 1);
  const refusedRemoval = run(["ids", value.project, "--sync", "--write"]);
  assert.equal(refusedRemoval.status, 1);
  assert.match(refusedRemoval.stderr, /PXSTORY1418/u);
  assert.equal((JSON.parse(refusedRemoval.stdout) as {removedOperations: unknown[]})
    .removedOperations.length, 1);
  assert.equal(await readFile(mapPath, "utf8"), updatedMap);
  const removalBackup = join(value.root, "removal-backup");
  const acceptedRemoval = run(["ids", value.project, "--sync", "--write",
    "--accept-removals", "--backup", removalBackup]);
  assert.equal(acceptedRemoval.status, 0, acceptedRemoval.stderr);
  assert.equal(await readFile(join(removalBackup, "Story", "main.pxstory.pxids.json"), "utf8"),
    updatedMap);
  assert.equal(run(["validate", value.project]).status, 0);
});

test("validate and build use the same canonical project compiler", async () => {
  const value = await fixture();
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  const output = join(value.root, "artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  assert.equal((await stat(join(output, "Runtime/main.pxir"))).isFile(), true);
  assert.equal((await stat(join(output, "Runtime/main.pxmap"))).isFile(), true);
});

test("validate reads shared storyMacros files and reports a missing one", async () => {
  const value = await fixture();
  const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  await write(value.project, JSON.stringify({...manifest, storyMacros: ["Story/_macros.pxstory"]}));
  const missing = run(["validate", value.project]);
  assert.notEqual(missing.status, 0);
  assert.match(missing.stderr, /PXBUILD1121/u);
  await write(join(value.root, "Story/_macros.pxstory"), "[macro hello who]\n@%who Hello.\n[endmacro]\n");
  await write(join(value.root, "Story/main.pxstory"), "[hello guide]\n[end]\n");
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stdout + validated.stderr);
});

test("ids skips shared storyMacros files: calls are identified where they are made", async () => {
  const value = await fixture();
  const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  await write(value.project, JSON.stringify({...manifest, storyMacros: ["Story/_macros.pxstory"]}));
  const macroPath = join(value.root, "Story/_macros.pxstory");
  const macroSource = "[macro hello who]\n@%who Hello.\n[endmacro]\n";
  await write(macroPath, macroSource);
  const scenePath = join(value.root, "Story/main.pxstory");
  await write(scenePath, "[hello guide]\n[end]\n");
  const externalized = run(["ids", value.project, "--external", "--write"]);
  assert.equal(externalized.status, 0, externalized.stderr);
  assert.equal((await stat(`${scenePath}.pxids.json`)).isFile(), true);
  await assert.rejects(stat(`${macroPath}.pxids.json`));
  assert.equal(run(["ids", value.project, "--check"]).status, 0);
  await write(scenePath, "; @prismatix-ids\n[hello guide]\nAfter the call.\n[end]\n");
  const synced = run(["ids", value.project, "--sync", "--write", "--accept-removals"]);
  assert.equal(synced.status, 0, synced.stdout + synced.stderr);
  assert.doesNotMatch(synced.stderr, /PXSTORY1411/u);
  assert.equal(run(["ids", value.project, "--sync", "--check"]).status, 0);
  await assert.rejects(stat(`${macroPath}.pxids.json`));
  assert.equal(await readFile(macroPath, "utf8"), macroSource);
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stdout + validated.stderr);
  // A macro file registered after the scenes went external needs no sidecar either.
  await write(join(value.root, "Story/_more.pxstory"), "[macro bye who]\n@%who Bye.\n[endmacro]\n");
  await write(value.project, JSON.stringify({...manifest,
    storyMacros: ["Story/_macros.pxstory", "Story/_more.pxstory"]}));
  await write(scenePath, "; @prismatix-ids\n[hello guide]\nAfter the call.\n[bye guide]\n[end]\n");
  const later = run(["ids", value.project, "--sync", "--write", "--accept-removals"]);
  assert.equal(later.status, 0, later.stdout + later.stderr);
  assert.equal(run(["ids", value.project, "--check"]).status, 0);
  assert.equal(run(["validate", value.project]).status, 0);
});

test("resourceIncludes package unreferenced UI families and audio without artifact post-processing", async () => {
  const value = await fixture();
  const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  manifest.resourceIncludes = ["Assets/UI/Original", "Assets/Audio/choice.ogg"];
  await write(value.project, JSON.stringify(manifest));
  await write(join(value.root, "Assets/UI/Original", "a.png"), "first-image");
  await write(join(value.root, "Assets/UI/Original/sub", "b.png"), "second-image");
  await write(join(value.root, "Assets/Audio/choice.ogg"), "choice-sound");
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  const summary = JSON.parse(validated.stdout) as {
    resourceFiles: number; includeRoots: number; includedFiles: number;
  };
  assert.equal(summary.includeRoots, 2);
  assert.equal(summary.includedFiles, 3);
  assert.equal(summary.resourceFiles, 3);
  assert.equal(JSON.parse(validated.stdout).catalogIncludeOverlap, 0);
  const output = join(value.root, "included-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  for (const path of ["Assets/UI/Original/a.png", "Assets/UI/Original/sub/b.png",
    "Assets/Audio/choice.ogg"]) {
    assert.equal(await readFile(join(output, ...path.split("/")), "utf8"),
      await readFile(join(value.root, ...path.split("/")), "utf8"));
  }
  const canonical = JSON.parse(await readFile(join(output, "project.pxproject"), "utf8")) as Record<string, unknown>;
  assert.equal(canonical.resourceIncludes, undefined);
  const marker = JSON.parse(await readFile(join(output, ".prismatix-output.json"), "utf8")) as {
    files: Record<string, string>;
  };
  assert.match(marker.files["Assets/UI/Original/sub/b.png"]!, /^[0-9a-f]{64}$/u);

  const missingManifest = {...manifest, resourceIncludes: ["Assets/UI/Missing"]};
  await write(value.project, JSON.stringify(missingManifest));
  const missing = run(["validate", value.project]);
  assert.equal(missing.status, 1);
  assert.match(missing.stderr, /PXBUILD1052/u);
  await mkdir(join(value.root, "Assets", "UI"), {recursive: true});
  await symlink(join(value.root, "Assets", "UI", "Original"),
    join(value.root, "Assets", "UI", "Alias"), "junction");
  await write(value.project, JSON.stringify({...manifest, resourceIncludes: ["Assets/UI/Alias"]}));
  const linked = run(["validate", value.project]);
  assert.equal(linked.status, 1);
  assert.match(linked.stderr, /symbolic link/u);
});

test("bounded parallel resource copy preserves every file and rebuilds changed media", async () => {
  const value = await fixture();
  const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  manifest.resourceIncludes = ["Assets/Batch"];
  await write(value.project, JSON.stringify(manifest));
  const inputs = Array.from({length: 25}, (_, index) =>
    `Assets/Batch/group-${index % 3}/media-${index}.bin`);
  for (const [index, path] of inputs.entries())
    await write(join(value.root, ...path.split("/")), `original-${index}`);
  const output = join(value.root, "batch-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  for (const [index, path] of inputs.entries())
    assert.equal(await readFile(join(output, ...path.split("/")), "utf8"),
      `original-${index}`);
  const markerPath = join(output, ".prismatix-output.json");
  const before = JSON.parse(await readFile(markerPath, "utf8")) as {
    files: Record<string, string>;
  };
  const markerIdentity = (await stat(markerPath, {bigint: true})).ino;
  assert.equal(inputs.every((path) => /^[0-9a-f]{64}$/u.test(before.files[path] ?? "")),
    true);
  const copyEvent = (output: string) => output.split(/\r?\n/u).filter(Boolean)
    .map((line) => JSON.parse(line) as {phase?: string; done?: boolean;
      reusedFiles?: number; reusedBytes?: number})
    .find((event) => event.phase === "copy" && event.done);
  const unchanged = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(unchanged.status, 0, unchanged.stderr);
  assert.match(unchanged.stderr, /"phase":"reuse-artifact"/u,
    "an unchanged ordinary build keeps the verified artifact without staging every media file");
  assert.equal(copyEvent(unchanged.stderr), undefined);
  assert.equal((await stat(markerPath, {bigint: true})).ino, markerIdentity,
    "the unchanged build must retain the existing publication itself");
  const unownedPath = join(output, "Assets", "Unowned", "stray.bin");
  await write(unownedPath, "stray");
  const unowned = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(unowned.status, 1);
  assert.match(unowned.stderr, /Artifact directory is not owned: Assets\/Unowned/u);
  assert.match(unowned.stderr, /use --output <new-empty-directory>/u);
  assert.doesNotMatch(unowned.stderr, /"phase":"compile"/u,
    "unowned output must fail before Story compilation");
  assert.doesNotMatch(unowned.stderr, /"phase":"copy"/u,
    "unowned output must fail before copying source media");
  assert.equal(await readFile(unownedPath, "utf8"), "stray");
  await rm(join(output, "Assets", "Unowned"), {recursive: true});
  const retainedPath = join(output, ...inputs[0]!.split("/"));
  await write(retainedPath, "tampered-0");
  const refused = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(refused.status, 1,
    "a changed old artifact cannot be promoted merely because its marker matches the source");
  assert.equal(await readFile(retainedPath, "utf8"), "tampered-0",
    "failed publication preserves the existing directory");
  await write(retainedPath, "original-0");
  await write(join(value.root, ...inputs[9]!.split("/")), "modified-9");
  const rebuilt = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(rebuilt.status, 0, rebuilt.stderr);
  assert.equal(copyEvent(rebuilt.stderr)?.reusedFiles, inputs.length - 1,
    "equal-sized changed media must be recopied after a content-hash check");
  const after = JSON.parse(await readFile(markerPath, "utf8")) as {
    files: Record<string, string>;
  };
  assert.notEqual(after.files[inputs[9]!], before.files[inputs[9]!]);
  assert.equal(await readFile(join(output, ...inputs[9]!.split("/")), "utf8"),
    "modified-9");
});

test("artifact publication rejects authored text changed after compilation", async () => {
  const value = await fixture();
  try {
    const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
    manifest.resourceIncludes = ["Assets/Batch"];
    await write(value.project, JSON.stringify(manifest));
    for (let index = 0; index < 200; index += 1)
      await write(join(value.root, "Assets/Batch", `media-${index}.bin`), `media-${index}`);
    const output = join(value.root, "artifact");
    const source = join(value.root, "Story/main.pxstory");
    const initial = run(["build", value.project, "--artifact-only", "--output", output]);
    assert.equal(initial.status, 0, initial.stderr);
    const markerPath = join(output, ".prismatix-output.json");
    const oldMarker = await readFile(markerPath, "utf8");
    const oldIr = await readFile(join(output, "Runtime/main.pxir"), "utf8");
    await write(source, "Compiled line.\n[end]\n");

    const runWithPublicationMutation = async (mutate: () => void,
      phase = "publish") => {
      const child = spawn(process.execPath, [resolve("dist/src/cli.js"), "build",
        value.project, "--artifact-only", "--output", output, "--json"]);
      let stderr = "";
      let pending = "";
      let changed = false;
      child.stderr.setEncoding("utf8");
      child.stderr.on("data", (chunk: string) => {
        stderr += chunk;
        pending += chunk;
        for (let end = pending.indexOf("\n"); end >= 0; end = pending.indexOf("\n")) {
          const line = pending.slice(0, end);
          pending = pending.slice(end + 1);
          let event: {phase?: string; current?: number} | undefined;
          try { event = JSON.parse(line) as {phase?: string; current?: number}; }
          catch { continue; }
          if (!changed && event.phase === phase && event.current === 0) {
            mutate();
            changed = true;
          }
        }
      });
      const status = await new Promise<number | null>((resolveClose, reject) => {
        child.once("error", reject);
        child.once("close", resolveClose);
      });
      assert.equal(changed, true, stderr);
      assert.equal(status, 1, stderr);
      return stderr;
    };
    const changedSource = await runWithPublicationMutation(() =>
      writeFileSync(source, "Edited during publication.\n[end]\n", "utf8"));
    assert.match(changedSource, /Authored text changed during build/u);
    assert.equal(await readFile(markerPath, "utf8"), oldMarker);
    assert.equal(await readFile(join(output, "Runtime/main.pxir"), "utf8"), oldIr);

    const appearedSidecar = await runWithPublicationMutation(() =>
      writeFileSync(`${source}.pxids.json`, "{}", "utf8"));
    assert.match(appearedSidecar, /Authored text appeared during build/u);
    assert.equal(await readFile(markerPath, "utf8"), oldMarker);
    assert.equal(await readFile(join(output, "Runtime/main.pxir"), "utf8"), oldIr);

    await rm(`${source}.pxids.json`);
    const settled = run(["build", value.project, "--artifact-only", "--output", output]);
    assert.equal(settled.status, 0, settled.stderr);
    const retainedMarker = await readFile(markerPath, "utf8");
    const retainedIr = await readFile(join(output, "Runtime/main.pxir"), "utf8");
    const rejectedRetain = await runWithPublicationMutation(() =>
      writeFileSync(source, "Changed while retaining output.\n[end]\n", "utf8"),
    "verify-before-publish");
    assert.match(rejectedRetain, /Authored text changed during build/u);
    assert.equal(await readFile(markerPath, "utf8"), retainedMarker);
    assert.equal(await readFile(join(output, "Runtime/main.pxir"), "utf8"), retainedIr);
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate reports catalog entries already covered by an included media directory", async () => {
  const value = await fixture();
  const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  manifest.resourceIncludes = ["Assets/UI/Original"];
  manifest.assets = [{id: "33333333-3333-4333-8333-333333333333", kind: "ui",
    source: "Assets/UI/Original/scene.png"}];
  await write(value.project, JSON.stringify(manifest));
  await write(join(value.root, "Assets/UI/Original/scene.png"), "picture");
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  assert.equal(JSON.parse(validated.stdout).catalogIncludeOverlap, 1);
});

test("startup voice variants enter the artifact without asset ids or resourceIncludes", async () => {
  const value = await fixture();
  const manifest = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  manifest.settings = {"player.titleVoiceVariants": ["Assets/Voice/title.ogg"],
    "player.splashes": [{scene: "Content/UI/Title.pxui", minimumDuration: 2,
      audioVariants: ["Assets/Voice/logo-a.ogg", "Assets/Voice/logo-b.ogg"]}]};
  await write(value.project, JSON.stringify(manifest));
  const voices = ["title.ogg", "logo-a.ogg", "logo-b.ogg"];
  for (const voice of voices)
    await write(join(value.root, "Assets/Voice", voice), `audio:${voice}`);
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  assert.equal(JSON.parse(validated.stdout).resourceFiles, voices.length);
  const output = join(value.root, "voices-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  for (const voice of voices)
    assert.equal(await readFile(join(output, "Assets/Voice", voice), "utf8"), `audio:${voice}`);
  const marker = JSON.parse(await readFile(join(output, ".prismatix-output.json"), "utf8"));
  for (const voice of voices)
    assert.match(marker.files[`Assets/Voice/${voice}`], /^[0-9a-f]{64}$/u);
});

test("artifact build reports load, compile, copy and fingerprint progress", async () => {
  const value = await fixture();
  const output = join(value.root, ".prismatix", "progress-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  const events = built.stderr.split(/\r?\n/u).filter(Boolean).map(line => JSON.parse(line) as {
    code: string; phase?: string; current?: number; total?: number; done?: boolean;
  }).filter(event => event.code === "PXCLI1102");
  for (const phase of ["load", "compile", "copy", "fingerprint-output", "publish"])
    assert.ok(events.some(event => event.phase === phase && event.done), phase);
  const copy = events.find(event => event.phase === "copy" && event.done)!;
  assert.equal(copy.current, copy.total);
  const storyEvents = built.stderr.split(/\r?\n/u).filter(Boolean)
    .map(line => JSON.parse(line) as {code: string; phase?: string; current?: number;
      total?: number; sceneId?: string; locale?: string; done?: boolean})
    .filter(event => event.code === "PXCLI1103");
  assert.deepEqual(storyEvents.map(event => event.phase),
    ["story-parse", "story-compile"]);
  assert.ok(storyEvents.every(event => event.current === 1 && event.total === 1 &&
    event.locale === "en" && event.sceneId === "main" && event.done));
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  assert.match(validated.stderr, /"code":"PXCLI1103"/u);
  const rebuilt = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(rebuilt.status, 0, rebuilt.stderr);
  assert.doesNotMatch(rebuilt.stderr, /"phase":"verify-existing"/u);
  assert.doesNotMatch(rebuilt.stderr, /"phase":"verify-reuse-source"/u,
    "the destination integrity scan also validates linked reuse from that destination");
  assert.match(rebuilt.stderr, /"phase":"verify-before-publish"/u);
  assert.match(rebuilt.stderr, /"phase":"reuse-artifact"/u);
});

test("augment publishes declared artifact additions for pack --artifact without hand-editing ownership", async () => {
  const value = await fixture();
  const artifact = join(value.root, ".prismatix", "augment-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", artifact]);
  assert.equal(built.status, 0, built.stderr);
  await write(join(value.root, "Assets", "bonus.bin"), "bonus data");
  const manifestPath = join(value.root, "augment.json");
  await write(manifestPath, JSON.stringify({format: "PrismatiXArtifactAugmentation",
    schemaRevision: 1, owner: "bonus", files: [{source: "Assets/bonus.bin",
      target: "Assets/bonus.bin", mode: "add"}]}));
  const augmented = run(["augment", value.project, "--artifact", artifact,
    "--manifest", manifestPath]);
  assert.equal(augmented.status, 0, augmented.stderr);
  assert.equal(JSON.parse(augmented.stdout).added, 1);
  assert.equal(await readFile(join(artifact, "Assets", "bonus.bin"), "utf8"), "bonus data");
  const marker = JSON.parse(await readFile(join(artifact, ".prismatix-output.json"), "utf8"));
  assert.match(marker.files["Assets/bonus.bin"], /^[0-9a-f]{64}$/u);
  assert.equal(marker.files.Assets, "directory");
  const tools = await mkdtemp(join(tmpdir(), "prismatix-augment-pack-"));
  const mock = join(tools, "mock-packager.mjs");
  await write(mock, `
    import {readFile} from "node:fs/promises";
    const request = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
    if (!request.inputs.some(value => value.uri === "Assets/bonus.bin") ||
        !request.inputs.some(value => value.uri === ".prismatix-augmentations.json")) process.exit(42);
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
      requestId: request.requestId, augmentedInput: true}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const packed = run(["pack", value.project, "--artifact", artifact,
    "--packager", mock, "--player", player, "--output", join(value.root, "augmented-package")]);
  assert.equal(packed.status, 0, packed.stderr);
  assert.match(packed.stdout, /"augmentedInput":true/u);
  const secondBuild = run(["build", value.project, "--artifact-only", "--output", artifact]);
  assert.equal(secondBuild.status, 0, secondBuild.stderr);
});

test("TSX UI authoring lowers to canonical JSON and never enters the Runtime artifact", async () => {
  const value = await fixture();
  const project = JSON.parse(await readFile(value.project, "utf8")) as {
    uiEntryPoints: Record<string, string>;
    uiComponents?: Array<{id: string; name: string; source: string}>;
    settings?: Record<string, unknown>;
  };
  project.uiEntryPoints.title = "Content/UI/Title.tsx";
  project.settings = {"player.splashes": [{scene: "Content/UI/Title.tsx",
    minimumDuration: 2}]};
  project.uiComponents = [{id: "12121212-1212-4212-8212-121212121212",
    name: "Panel", source: "Content/UI/Panel.jsx"}];
  await write(value.project, JSON.stringify(project));
  await write(join(value.root, "Content/UI/components/MenuButton.tsx"), `
    import {Button} from "@prismatix/authoring-sdk";

    export function MenuButton({label, action}: {label: string; action: string}) {
      return <Button name={label} action={action}
        bind={{enabled: "game.menuEnabled"}}>{label}</Button>;
    }
  `);
  await write(join(value.root, "Content/UI/Title.tsx"), `
    import {Scene, Text, VBox} from "@prismatix/authoring-sdk";
    import {MenuButton} from "./components/MenuButton.tsx";

    export default (
      <Scene name="TSX Title" width={1280} height={720}>
        <VBox name="Root" fill>
          <Text name="Heading">PrismatiX JSX</Text>
          <MenuButton key="start" label="Start" action="game.start" />
        </VBox>
      </Scene>
    );
  `);
  await write(join(value.root, "Content/UI/Panel.jsx"), `
    import {Component, Text, VBox} from "@prismatix/authoring-sdk";
    export default (
      <Component id="12121212-1212-4212-8212-121212121212"
        name="Panel" width={400} height={240}>
        <VBox name="PanelRoot"><Text name="Caption">Panel</Text></VBox>
      </Component>
    );
  `);

  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  const output = join(value.root, "tsx-artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  const canonicalPath = join(output, "Content/UI/Title.pxui");
  const canonical = JSON.parse(await readFile(canonicalPath, "utf8")) as {
    format: string; nodes: Array<{name: string; id: string; onClick?: {id: string}}>;
  };
  assert.equal(canonical.format, "PrismatiXUIScene");
  assert.equal(canonical.nodes.find((node) => node.name === "Start")?.onClick?.id,
    "game.start");
  const builtProject = JSON.parse(await readFile(join(output, "project.pxproject"), "utf8")) as {
    uiEntryPoints: Record<string, string>;
    settings: {"player.splashes": Array<{scene: string}>};
  };
  assert.equal(builtProject.uiEntryPoints.title, "Content/UI/Title.pxui");
  assert.equal(builtProject.settings["player.splashes"][0]?.scene,
    "Content/UI/Title.pxui");
  await assert.rejects(stat(join(output, "Content/UI/Title.tsx")));
  assert.equal((await stat(join(output, "Content/UI/Panel.pxuicomponent"))).isFile(), true);
  await assert.rejects(stat(join(output, "Content/UI/Panel.jsx")));

  const secondOutput = join(value.root, "tsx-artifact-second");
  const rebuilt = run(["build", value.project, "--artifact-only", "--output", secondOutput]);
  assert.equal(rebuilt.status, 0, rebuilt.stderr);
  assert.equal(await readFile(join(secondOutput, "Content/UI/Title.pxui"), "utf8"),
    await readFile(canonicalPath, "utf8"));
});

test("a TSX screen compiles again only when a module it read or an import it resolved changes", async () => {
  const value = await fixture();
  const project = JSON.parse(await readFile(value.project, "utf8")) as {uiEntryPoints: Record<string, string>};
  project.uiEntryPoints.title = "Content/UI/Title.tsx";
  await write(value.project, JSON.stringify(project));
  const button = (name: string) => `
    import {Button} from "@prismatix/authoring-sdk";
    export function MenuButton({action}: {action: string}) {
      return <Button name="${name}" action={action}>Start</Button>;
    }
  `;
  await write(join(value.root, "Content/UI/components/MenuButton.tsx"), button("FromTsx"));
  await write(join(value.root, "Content/UI/Title.tsx"), `
    import {Scene, VBox} from "@prismatix/authoring-sdk";
    import {MenuButton} from "./components/MenuButton";
    export default (
      <Scene name="Cached Title" width={1280} height={720}>
        <VBox name="Root" fill><MenuButton action="game.start" /></VBox>
      </Scene>
    );
  `);
  let builds = 0;
  const buttonName = async (): Promise<string | undefined> => {
    const output = join(value.root, `cache-artifact-${builds += 1}`);
    const built = run(["build", value.project, "--artifact-only", "--output", output]);
    assert.equal(built.status, 0, built.stderr);
    const scene = JSON.parse(await readFile(join(output, "Content/UI/Title.pxui"), "utf8")) as
      {nodes: Array<{name: string; onClick?: unknown}>};
    return scene.nodes.find((node) => node.onClick !== undefined)?.name;
  };
  assert.equal(await buttonName(), "FromTsx");

  // The next build reuses the cached document: change it to see that it does.
  const cachePath = join(value.root, ".prismatix", "cache", "jsx-ui-v1.json");
  const cache = JSON.parse(await readFile(cachePath, "utf8")) as
    {entries: Record<string, {files: Array<{path: string}>; value: {nodes: Array<{name: string}>}}>};
  const entry = cache.entries["scene:Content/UI/Title.tsx"]!;
  assert.deepEqual(entry.files.map((file) => file.path).sort(),
    ["Content/UI/Title.tsx", "Content/UI/components/MenuButton.tsx"].sort());
  for (const node of entry.value.nodes) if (node.name === "FromTsx") node.name = "FromCache";
  await writeFile(cachePath, JSON.stringify(cache));
  assert.equal(await buttonName(), "FromCache");

  // An imported module changed.
  await write(join(value.root, "Content/UI/components/MenuButton.tsx"), button("Edited"));
  assert.equal(await buttonName(), "Edited");

  // The same import now resolves to a new file that TypeScript tries first.
  await write(join(value.root, "Content/UI/components/OtherButton.tsx"), button("FromTs"));
  await write(join(value.root, "Content/UI/components/MenuButton.ts"),
    `export {MenuButton} from "./OtherButton.tsx";\n`);
  assert.equal(await buttonName(), "FromTs");
});

test("ui compile prints the canonical document, element spans, identity seeds and diagnostics", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown> &
      {uiEntryPoints: Record<string, string>};
    project.uiEntryPoints.title = "Content/UI/Title.tsx";
    project.uiComponents = [{id: "12121212-1212-4212-8212-121212121212",
      name: "Panel", source: "Content/UI/Panel.jsx"}];
    await write(value.project, JSON.stringify(project));
    const button =`import {Button} from "@prismatix/authoring-sdk";

export function MenuButton({label, action}: {label: string; action: string}) {
  return <Button name={label} action={action}>{label}</Button>;
}
`;
    await write(join(value.root, "Content/UI/components/MenuButton.tsx"), button);
    const source = `import {Scene, Text, VBox} from "@prismatix/authoring-sdk";
import {MenuButton} from "./components/MenuButton.tsx";

export default (
  <Scene name="Editor Title" width={1280} height={720}>
    <VBox name="Root" fill>
      <Text name="Heading" fontSize={32}>
        PrismatiX
      </Text>
      <MenuButton key="start" label="Start" action="game.start" />
    </VBox>
  </Scene>
);
`;
    const titlePath = join(value.root, "Content/UI/Title.tsx");
    await write(titlePath, source);
    await write(join(value.root, "Content/UI/Panel.jsx"), `import {Component, Text, VBox} from "@prismatix/authoring-sdk";
export default (
  <Component id="12121212-1212-4212-8212-121212121212" name="Panel" width={400} height={240}>
    <VBox name="PanelRoot"><Text name="Caption">Panel</Text></VBox>
  </Component>
);
`);
    type Span = {path: string; start: {line: number; column: number; offset: number};
      end: {line: number; column: number; offset: number}};
    type Report = {valid: boolean; path: string; kind: string; document?: {format: string; nodes: Array<{id: string; name: string}>};
      nodes: Array<{index: number; id: string; name: string; span?: Span; identity?: {seed: string; basis: string}}>;
      diagnostics: Array<{code: string; severity: string; span?: Span}>; dependencies: string[]};

    // The project is found from the file; no --project needed.
    const compiled = run(["ui", "compile", titlePath]);
    assert.equal(compiled.status, 0, compiled.stderr);
    const report = JSON.parse(compiled.stdout) as Report;
    assert.equal(report.valid, true);
    assert.deepEqual([report.path, report.kind, report.document!.format],
      ["Content/UI/Title.tsx", "scene", "PrismatiXUIScene"]);
    assert.deepEqual(report.nodes.map((node) => node.name), ["Root", "Heading", "Start"]);
    assert.deepEqual(report.nodes.map((node) => node.id), report.document!.nodes.map((node) => node.id));
    const heading = report.nodes[1]!;
    assert.equal(heading.span!.path, "Content/UI/Title.tsx");
    assert.deepEqual([heading.span!.start.line, heading.span!.start.column, heading.span!.end.line,
      heading.span!.end.column], [7, 7, 9, 14]);
    assert.equal(source.slice(heading.span!.start.offset, heading.span!.end.offset),
      `<Text name="Heading" fontSize={32}>\n        PrismatiX\n      </Text>`);
    assert.deepEqual(heading.identity, {seed: "root/4:Root#0/7:Heading#0", basis: "name"});
    // A node rendered by an imported component points at that component's JSX.
    const start = report.nodes[2]!;
    assert.equal(start.span!.path, "Content/UI/components/MenuButton.tsx");
    assert.equal(button.slice(start.span!.start.offset, start.span!.end.offset),
      "<Button name={label} action={action}>{label}</Button>");
    assert.equal(start.identity!.basis, "key");
    assert.deepEqual(report.dependencies, ["Content/UI/Title.tsx", "Content/UI/components/MenuButton.tsx"]);
    assert.deepEqual(report.diagnostics, []);
    // The same document the build writes.
    const built = run(["build", value.project, "--artifact-only", "--output", join(value.root, "ui-artifact")]);
    assert.equal(built.status, 0, built.stderr);
    assert.deepEqual(JSON.parse(await readFile(join(value.root, "ui-artifact/Content/UI/Title.pxui"), "utf8")),
      report.document);

    // A registered component compiles as a component.
    const panel = run(["ui", "compile", "Content/UI/Panel.jsx", "--project", value.root]);
    assert.equal(panel.status, 0, panel.stderr);
    assert.equal((JSON.parse(panel.stdout) as Report).document!.format, "PrismatiXUIComponent");

    // Errors come back as diagnostics with spans, valid false and exit 1.
    await write(titlePath, source.replace("fontSize={32}", "fontSize=\"big\""));
    const failed = run(["ui", "compile", titlePath]);
    assert.equal(failed.status, 1);
    const failure = JSON.parse(failed.stdout) as Report;
    assert.equal(failure.valid, false);
    assert.equal(failure.document, undefined);
    assert.ok(failure.diagnostics.some((item) => item.severity === "error" &&
      item.span?.path === "Content/UI/Title.tsx" && item.span.start.line === 7), failed.stdout);
    assert.equal(run(["ui", "compile", join(value.root, "Story/main.pxstory")]).status, 1);
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate locates a missing TSX image at its authored JSX value", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as {
      uiEntryPoints: Record<string, string>;
    };
    project.uiEntryPoints.title = "Content/UI/Title.tsx";
    await write(value.project, JSON.stringify(project));
    const source = `import {Image, Scene, VBox} from "@prismatix/authoring-sdk";
export default <Scene name="Title" width={1280} height={720}>
  <VBox name="Root"><Image name="Missing" path="Assets/UI/missing.png" /></VBox>
</Scene>;
`;
    await write(join(value.root, "Content/UI/Title.tsx"), source);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {line: number; column: number; offset: number};
        end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.code === "PXBUILD1044");
    assert.equal(issue?.span?.path, "Content/UI/Title.tsx", validated.stderr);
    assert.equal(issue.span.start.line, 3);
    assert.equal(source.slice(issue.span.start.offset, issue.span.end.offset),
      '"Assets/UI/missing.png"');
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate locates TSX resource errors at imported component JSX values", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as {
      uiEntryPoints: Record<string, string>;
    };
    project.uiEntryPoints.title = "Content/UI/Title.tsx";
    await write(value.project, JSON.stringify(project));
    await write(join(value.root, "Content/UI/Title.tsx"),
      `import {Scene, VBox} from "@prismatix/authoring-sdk";
import {MissingImage} from "./components/MissingImage.tsx";
export default <Scene name="Title" width={1280} height={720}>
  <VBox name="Root"><MissingImage /></VBox>
</Scene>;
`);
    const component = `import {Image} from "@prismatix/authoring-sdk";
export function MissingImage() {
  return <Image name="Missing" path="Assets/UI/component-missing.png" />;
}
`;
    await write(join(value.root, "Content/UI/components/MissingImage.tsx"), component);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {line: number; offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.code === "PXBUILD1044");
    assert.equal(issue?.span?.path, "Content/UI/components/MissingImage.tsx",
      validated.stderr);
    assert.equal(issue.span.start.line, 3);
    assert.equal(component.slice(issue.span.start.offset, issue.span.end.offset),
      '"Assets/UI/component-missing.png"');
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate locates resource errors inside a static JSX prop spread", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as {
      uiEntryPoints: Record<string, string>;
    };
    project.uiEntryPoints.title = "Content/UI/Title.tsx";
    await write(value.project, JSON.stringify(project));
    const source = `import {Image, Scene, VBox} from "@prismatix/authoring-sdk";
export default <Scene name="Title" width={1280} height={720}>
  <VBox name="Root"><Image {...{name: "Missing", path: "Assets/UI/spread-missing.png"}} /></VBox>
</Scene>;
`;
    await write(join(value.root, "Content/UI/Title.tsx"), source);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.code === "PXBUILD1044");
    assert.equal(issue?.span?.path, "Content/UI/Title.tsx", validated.stderr);
    assert.equal(source.slice(issue.span.start.offset, issue.span.end.offset),
      '"Assets/UI/spread-missing.png"');
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate locates a cross-layer TSX binding error at its JSX value", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as {
      uiEntryPoints: Record<string, string>;
      settings?: Record<string, unknown>;
    };
    project.uiEntryPoints.title = "Content/UI/Title.tsx";
    project.settings = {"player.uiData": {screen: {title: "Ready"}}};
    await write(value.project, JSON.stringify(project));
    const source = `import {Scene, Text, VBox} from "@prismatix/authoring-sdk";
export default <Scene name="Title" width={1280} height={720}>
  <VBox name="Root"><Text name="Bad" bind={{text: "screen.missing"}} /></VBox>
</Scene>;
`;
    await write(join(value.root, "Content/UI/Title.tsx"), source);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {line: number; offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.span?.path ===
      "Content/UI/Title.tsx");
    assert.ok(issue?.span, validated.stderr);
    assert.equal(issue.span.start.line, 3);
    assert.equal(source.slice(issue.span.start.offset, issue.span.end.offset),
      '"screen.missing"');
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate locates a lowered TSX layout error at its JSX value", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as {
      uiEntryPoints: Record<string, string>;
    };
    project.uiEntryPoints.title = "Content/UI/Title.tsx";
    await write(value.project, JSON.stringify(project));
    const source = `import {Image, Scene, VBox} from "@prismatix/authoring-sdk";
export default <Scene name="Title" width={1280} height={720}>
  <VBox name="Root"><Image name="BadSize" width={-1} /></VBox>
</Scene>;
`;
    await write(join(value.root, "Content/UI/Title.tsx"), source);
    const validated = run(["validate", value.project]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {line: number; offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.span?.path ===
      "Content/UI/Title.tsx");
    assert.ok(issue?.span, validated.stderr);
    assert.equal(issue.span.start.line, 3);
    assert.equal(source.slice(issue.span.start.offset, issue.span.end.offset), "-1");
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate locates invalid host UI contract values in the author manifest", async () => {
  const value = await fixture();
  try {
    const project = JSON.parse(await readFile(value.project, "utf8")) as {
      name?: string;
      settings?: Record<string, unknown>;
    };
    project.name = "雪🌸";
    project.settings = {"authoring.uiContract": {
      format: "PrismatiXUiContract", schemaRevision: 1,
      roots: ["dialogue"], values: {dialogue: "object", "dialogue..bad": "string"},
    }};
    const source = JSON.stringify(project, null, 2);
    const authorPath = join(value.root, "prismatix.json");
    await write(authorPath, source);
    const validated = run(["validate", authorPath]);
    assert.equal(validated.status, 1, validated.stderr);
    const diagnostics = validated.stderr.split(/\r?\n/u).filter(Boolean).flatMap((line) => {
      try { return [JSON.parse(line) as {code: string; span?: {path: string;
        start: {offset: number}; end: {offset: number}}}]; }
      catch { return []; }
    });
    const issue = diagnostics.find((item) => item.code === "PXBUILD1090");
    assert.equal(issue?.span?.path, "prismatix.json", validated.stderr);
    assert.equal(Buffer.from(source).subarray(issue.span.start.offset,
      issue.span.end.offset).toString("utf8"), '"string"');
  } finally {
    await rm(value.root, {recursive: true, force: true});
  }
});

test("validate reports semantic TypeScript prop errors before JSX evaluation", async () => {
  const value = await fixture();
  const project = JSON.parse(await readFile(value.project, "utf8")) as {
    uiEntryPoints: Record<string, string>;
  };
  project.uiEntryPoints.title = "Content/UI/Invalid.tsx";
  await write(value.project, JSON.stringify(project));
  await write(join(value.root, "Content/UI/Invalid.tsx"), `
    import {Scene, Text, VBox} from "@prismatix/authoring-sdk";
    export default (
      <Scene name="Invalid" width={1280} height={720}>
        <VBox name="Root"><Text name="Bad" width="very wide">Bad</Text></VBox>
      </Scene>
    );
  `);

  const validated = run(["validate", value.project]);
  assert.notEqual(validated.status, 0);
  assert.match(validated.stderr, /PXCLIJSX1001/u);
  assert.match(validated.stderr, /TS2322/u);
});

test("typed image and button props reject invalid author values before native UI loading", async () => {
  const value = await fixture();
  const project = JSON.parse(await readFile(value.project, "utf8")) as {
    uiEntryPoints: Record<string, string>;
  };
  project.uiEntryPoints.title = "Content/UI/InvalidProps.tsx";
  await write(value.project, JSON.stringify(project));
  await write(join(value.root, "Content/UI/InvalidProps.tsx"), `
    import {Image, Scene, TextureButton, VBox} from "@prismatix/authoring-sdk";
    export default (
      <Scene name="Invalid" width={1280} height={720}>
        <VBox name="Root">
          <Image name="Backdrop" path={42} scaleMode="Squish" />
          <TextureButton name="Start" focusMode="Hover" action="game.start" />
        </VBox>
      </Scene>
    );
  `);
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 1);
  assert.match(validated.stderr, /PXCLIJSX1001/u);
  assert.match(validated.stderr, /TS2322/u);
});

test("pack invokes the native file-based request protocol", async () => {
  const value = await fixture();
  const authorProject = JSON.parse(await readFile(value.project, "utf8")) as {
    uiEntryPoints: Record<string, string>;
  };
  authorProject.uiEntryPoints.title = "Content/UI/Title.tsx";
  (authorProject as {resourceIncludes?: string[]}).resourceIncludes =
    ["Extras/illustration.bin"];
  await write(value.project, JSON.stringify(authorProject));
  await write(join(value.root, "Extras/illustration.bin"), "author media");
  await write(join(value.root, "Content/UI/Title.tsx"), `
    import {Scene, Text, VBox} from "@prismatix/authoring-sdk";
    console.log("PXCLI_TEST_AUTHORING_EVALUATED");
    export default (
      <Scene name="Title" width={1280} height={720}>
        <VBox name="Root"><Text name="Caption">Title</Text></VBox>
      </Scene>
    );
  `);
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-packager-"));
  const mock = join(tools, "mock-packager.mjs");
  await write(mock, `
    import {readFile} from "node:fs/promises";
    import {isAbsolute} from "node:path";
    const [flag, path] = process.argv.slice(2);
    if (flag !== "--request" || !path || !isAbsolute(path)) process.exit(41);
    const request = JSON.parse(await readFile(path, "utf8"));
    if (request.format !== "PrismatiXPackageRequest" || request.schemaRevision !== 1 ||
        request.startScript !== "Runtime/main.pxir" || !Array.isArray(request.inputs)) process.exit(42);
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
      requestId: request.requestId, fileRequest: true}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const output = join(value.root, "packaged");
  const packed = run(["pack", value.project, "--packager", mock,
    "--player", player, "--output", output]);
  assert.equal(packed.status, 0, packed.stderr);
  assert.match(packed.stderr, /PXCLI1042/u);
  assert.match(packed.stdout, /"fileRequest":true/);
  assert.match(packed.stdout, /"packed":true/);
  assert.equal(packed.stdout.match(/PXCLI_TEST_AUTHORING_EVALUATED/gu)?.length, 1,
    "pack should load and evaluate TSX authoring inputs only once");
  const overlapping = run(["pack", value.project, "--packager", mock,
    "--player", player, "--output", join(value.root, "Extras")]);
  assert.equal(overlapping.status, 1);
  assert.match(overlapping.stderr, /Unsafe output overlaps author input/u,
    "the single loaded project must still protect nonstandard resource roots");
  assert.equal(await readFile(join(value.root, "Extras/illustration.bin"), "utf8"),
    "author media");
});

test("pack carries a verified previous distribution into a separate output", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-previous-"));
  const previous = join(value.root, "previous-distribution");
  const manifestPath = join(previous, "Package", "manifest.json");
  const verifiedMarker = join(tools, "verified.txt");
  await write(manifestPath, JSON.stringify({gameId: "cli-sample"}));
  const mock = join(tools, "mock-packager.mjs");
  await write(mock, `
    import {readFile, writeFile} from "node:fs/promises";
    import {resolve} from "node:path";
    const [flag, path] = process.argv.slice(2);
    if (flag === "--verify-package") {
      if (resolve(path) !== resolve(${JSON.stringify(previous)})) process.exit(41);
      await writeFile(${JSON.stringify(verifiedMarker)}, "verified");
      process.stdout.write(JSON.stringify({event: "verified", archives: 1,
        resources: 1, rawBytes: 1}) + "\\n");
    } else if (flag === "--request") {
      const request = JSON.parse(await readFile(path, "utf8"));
      if (request.cacheSourceDir !== ${JSON.stringify(previous)} ||
          request.retireCacheSourceAfterPromotion !== false ||
          await readFile(${JSON.stringify(verifiedMarker)}, "utf8") !== "verified") process.exit(42);
      process.stdout.write(JSON.stringify({event: "completed", previousSource: true}) + "\\n");
    } else process.exit(43);
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const args = ["pack", value.project, "--packager", mock, "--player", player,
    "--previous-package", previous];
  const packed = run([...args, "--output", join(value.root, "new-distribution")]);
  assert.equal(packed.status, 0, packed.stderr);
  assert.match(packed.stdout, /"previousSource":true/u);
  assert.equal(run([...args, "--check"]).status, 2);

  const occupiedOutput = join(value.root, "occupied-distribution");
  await write(join(occupiedOutput, "foreign.txt"), "keep");
  const occupied = run([...args, "--output", occupiedOutput]);
  assert.equal(occupied.status, 1);
  assert.match(occupied.stderr, /new or empty output directory/u);
  assert.equal(await readFile(join(occupiedOutput, "foreign.txt"), "utf8"), "keep");

  await write(manifestPath, JSON.stringify({gameId: "cli-sample", publisherPublicKey: "other"}));
  const wrongPublisher = run([...args, "--output", join(value.root, "wrong-publisher-output")]);
  assert.equal(wrongPublisher.status, 1);
  assert.match(wrongPublisher.stderr, /different publisher key/u);

  await write(manifestPath, JSON.stringify({gameId: "different-game"}));
  const wrongGame = run([...args, "--output", join(value.root, "wrong-game-output")]);
  assert.equal(wrongGame.status, 1);
  assert.match(wrongGame.stderr, /different game/u);
  await write(manifestPath, JSON.stringify({gameId: "cli-sample"}));
  await write(mock, `
    import {readFile, writeFile} from "node:fs/promises";
    const [flag, path] = process.argv.slice(2);
    if (flag === "--verify-package") {
      process.stdout.write(JSON.stringify({event: "verified", archives: 1,
        resources: 1, rawBytes: 1}) + "\\n");
    } else if (flag === "--request") {
      await readFile(path, "utf8");
      await writeFile(${JSON.stringify(manifestPath)}, "{\\"gameId\\":\\"changed\\"}");
      process.stdout.write(JSON.stringify({event: "completed"}) + "\\n");
    } else process.exit(43);
  `);
  const changedOutput = join(value.root, "changed-source-output");
  const changed = run([...args, "--output", changedOutput]);
  assert.equal(changed.status, 1);
  assert.match(changed.stderr, /manifest changed while packaging/u);
  await assert.rejects(stat(changedOutput), /ENOENT/u);
});

test("pack keeps same-version saves loadable through the project's release lineage", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-lineage-"));
  const mock = join(tools, "mock-packager.mjs");
  // Mirrors how the native Packager folds a request's saveLineage into the
  // manifest's persistence block.
  await write(mock, `
    import {mkdir, readFile, writeFile} from "node:fs/promises";
    import {join} from "node:path";
    const [flag, path] = process.argv.slice(2);
    if (flag !== "--request") process.exit(43);
    const request = JSON.parse(await readFile(path, "utf8"));
    const lineage = request.saveLineage;
    const sameVersion = lineage !== undefined &&
      lineage.contentVersion === request.contentVersion &&
      lineage.saveVersion === request.saveVersion;
    await mkdir(join(request.outputDir, "Package"), {recursive: true});
    await writeFile(join(request.outputDir, "Package", "manifest.json"), JSON.stringify({
      gameId: request.gameId, packageFingerprint: process.env.PRISMATIX_TEST_FINGERPRINT,
      contentVersion: request.contentVersion, saveVersion: request.saveVersion,
      persistence: {dataId: request.gameId, keyId: "game-data-v1",
        legacyReaders: lineage?.legacyReaders ?? [],
        compatibleSaveFingerprints: sameVersion
          ? [...lineage.compatibleSaveFingerprints, lineage.packageFingerprint] : []}}));
    process.stdout.write(JSON.stringify({event: "completed",
      receivedLineage: lineage?.packageFingerprint ?? null}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const lineagePath = join(value.root, ".prismatix-release", "lineage.json");
  const [first, second, third] = ["1", "2", "3"].map(digit => digit.repeat(64));
  const pack = (output: string, fingerprint: string) => {
    process.env.PRISMATIX_TEST_FINGERPRINT = fingerprint;
    try {
      return run(["pack", value.project, "--packager", mock, "--player", player,
        "--output", join(value.root, output)]);
    } finally {
      delete process.env.PRISMATIX_TEST_FINGERPRINT;
    }
  };

  const initial = pack("release-1", first!);
  assert.equal(initial.status, 0, initial.stderr);
  assert.match(initial.stdout, /"receivedLineage":null/u);
  assert.match(initial.stderr, /PXCLI1222/u, "a pack with no known lineage warns");
  assert.match(initial.stderr, /reject every save from that release \(PXSAVE6111\)/u);
  assert.match(initial.stderr, /PXCLI1224/u);
  assert.deepEqual(JSON.parse(await readFile(lineagePath, "utf8")), {
    format: "PrismatiXReleaseLineage", schemaRevision: 1, gameId: "cli-sample",
    packageFingerprint: first, contentVersion: "chapter-1", saveVersion: 1,
    compatibleSaveFingerprints: [], legacyReaders: [],
  });

  // A new output folder, as on a clean CI machine, still accepts release-1 saves.
  const relocated = pack("release-2", second!);
  assert.equal(relocated.status, 0, relocated.stderr);
  assert.match(relocated.stdout, new RegExp(`"receivedLineage":"${first}"`, "u"));
  assert.doesNotMatch(relocated.stderr, /PXCLI122[23]/u);
  const afterRelocation = JSON.parse(await readFile(lineagePath, "utf8")) as
    {packageFingerprint: string; compatibleSaveFingerprints: string[]};
  assert.equal(afterRelocation.packageFingerprint, second);
  assert.deepEqual(afterRelocation.compatibleSaveFingerprints, [first]);

  const authorProject = JSON.parse(await readFile(value.project, "utf8")) as
    Record<string, unknown>;
  await write(value.project, JSON.stringify({...authorProject, contentVersion: "chapter-2"}));
  const bumped = pack("release-3", third!);
  assert.equal(bumped.status, 0, bumped.stderr);
  assert.match(bumped.stderr, /PXCLI1223/u,
    "a version change without a migration from the recorded release warns");
  assert.deepEqual((JSON.parse(await readFile(lineagePath, "utf8")) as
    {compatibleSaveFingerprints: string[]}).compatibleSaveFingerprints, []);

  await write(lineagePath, JSON.stringify({...afterRelocation,
    format: "PrismatiXReleaseLineage", schemaRevision: 1, gameId: "other-game",
    contentVersion: "chapter-2", saveVersion: 1, legacyReaders: []}));
  const foreign = pack("release-4", third!);
  assert.equal(foreign.status, 1);
  assert.match(foreign.stderr, /records game \\"other-game\\"/u);
});

test("pack validates UI against the selected Packager's adjacent registry", async () => {
  const value = await fixture();
  const uiPath = join(value.root, "Content/UI/Title.pxui");
  const ui = JSON.parse(await readFile(uiPath, "utf8")) as {
    nodes: Array<{runtimeProperties: Record<string, unknown>}>
  };
  ui.nodes[0]!.runtimeProperties.modulate = "#2D171DFF";
  await write(uiPath, JSON.stringify(ui));
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-native-ui-"));
  const mock = join(tools, "mock-packager.mjs");
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await write(mock, "process.exit(0);\n");
  await writeExecutable(player, "fixture");
  await write(join(tools, "PrismatiXUiTypeRegistry.json"), JSON.stringify({
    format: "PrismatiXUiTypeRegistry", schemaRevision: 1,
    controls: [{runtimeType: "Control", properties: [{id: "modulate", valueType: "color",
      writable: true, bindable: true, animatable: true}]}],
  }));
  const packed = run(["pack", value.project, "--packager", mock, "--player", player,
    "--output", join(value.root, "package")]);
  assert.equal(packed.status, 1);
  assert.match(packed.stderr, /PXSDKSEM1324/u);
  assert.doesNotMatch(packed.stderr, /PXCLI1042/u);
});

test("pack --check runs native preflight on an artifact without writing a distribution", async () => {
  const value = await fixture();
  const artifact = join(value.root, "artifact");
  assert.equal(run(["build", value.project, "--artifact-only", "--output", artifact]).status, 0);
  await write(join(value.root, "Story/main.pxstory"), "[invalid-author-source]\n");
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-package-check-"));
  const mock = join(tools, "mock-packager.mjs");
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  await write(mock, `
    import {readFile} from "node:fs/promises";
    const [flag, path, mode] = process.argv.slice(2);
    if (flag !== "--validate-request" ||
        (mode !== undefined && mode !== "--verify-media")) process.exit(41);
    const request = JSON.parse(await readFile(path, "utf8"));
    if (request.projectRoot !== ${JSON.stringify(artifact)} ||
        request.inputs.length === 0) process.exit(42);
    process.stdout.write(JSON.stringify({event: "validated",
      inputCount: request.inputs.length,
      deferredMediaCount: mode === "--verify-media" ? 0 : 2}) + "\\n");
  `);
  const output = join(value.root, "distribution");
  const args = ["pack", value.project, "--artifact", artifact,
    "--packager", mock, "--player", player, "--output", output, "--check"];
  const checked = run(args);
  assert.equal(checked.status, 0, checked.stderr);
  assert.match(checked.stdout, /"validated":true/u);
  assert.match(checked.stdout, /"packed":false/u);
  assert.match(checked.stdout, /"deferredMediaCount":2/u);
  await assert.rejects(stat(output), /ENOENT/u);
  const allInputs = run([...args, "--verify-media"]);
  assert.equal(allInputs.status, 0, allInputs.stderr);
  assert.match(allInputs.stdout, /"deferredMediaCount":0/u);
  await assert.rejects(stat(output), /ENOENT/u);
  const invalidOptions = run(args.filter(argument => argument !== "--check").concat("--verify-media"));
  assert.equal(invalidOptions.status, 2);
  assert.match(invalidOptions.stderr, /--verify-media requires pack --check/u);

  await write(mock, `
    process.stdout.write(JSON.stringify({event: "failed", code: "PXPKG1020",
      diagnostics: [{severity: "error", code: "PXPKG1020",
        message: "Native semantic check rejected UI"}]}) + "\\n");
    process.exit(1);
  `);
  const rejected = run(args);
  assert.equal(rejected.status, 1);
  assert.match(rejected.stderr, /PXPKG1020/u);
  await assert.rejects(stat(output), /ENOENT/u);
});

test("pack streams native progress and handles more than 16 MiB of progress events", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-long-progress-"));
  const mock = join(tools, "mock-packager.mjs");
  await write(mock, `
    const padding = "x".repeat(4096);
    for (let current = 0; current <= 4200; current++)
      process.stdout.write(JSON.stringify({event: "progress", phase: "archive",
        current, total: 4200, message: padding}) + "\\n");
    process.stdout.write(JSON.stringify({event: "completed"}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const packed = runHuman(["pack", value.project, "--packager", mock,
    "--player", player, "--output", join(value.root, "packaged")]);
  assert.equal(packed.status, 0, packed.stderr.slice(-2000));
  assert.match(packed.stderr, /Packaging archive: 0\/4200/u);
  assert.match(packed.stderr, /Packaging archive: 4200\/4200/u);
  assert.match(packed.stdout, /Packaged playable game/u);
});

test("changed pack still refuses a modified old distribution at promotion", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-pack-late-check-"));
  const mock = join(tools, "mock-packager.mjs");
  await write(mock, `
    import {readFile, writeFile} from "node:fs/promises";
    import {join} from "node:path";
    const request = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
    await writeFile(join(request.outputDir, "Content.pdx"), "new-content");
    process.stdout.write(JSON.stringify({event: "completed"}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const output = join(value.root, "packaged");
  const args = ["pack", value.project, "--packager", mock, "--player", player,
    "--output", output];
  const initial = run(args);
  assert.equal(initial.status, 0, initial.stderr);
  await write(join(output, "Content.pdx"), "old-content");
  const rejected = run(args);
  assert.equal(rejected.status, 1);
  assert.match(rejected.stderr, /unowned or modified output directory/u);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "old-content");
});

test("pack reuses an owned artifact without recompiling edited author sources", async () => {
  const value = await fixture();
  const artifact = join(value.root, ".prismatix", "artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", artifact]);
  assert.equal(built.status, 0, built.stderr);
  const markerPath = join(artifact, ".prismatix-output.json");
  const marker = JSON.parse(await readFile(markerPath, "utf8")) as {
    files: Record<string, string>;
  };
  delete marker.files.Runtime;
  await write(markerPath, JSON.stringify(marker));
  await write(join(value.root, "Story/main.pxstory"), "[unknown-command]\n");
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-reused-artifact-"));
  const mock = join(tools, "mock-packager.mjs");
  const output = join(value.root, "packaged");
  const publisherKey = join(tools, "publisher-key.txt");
  const publicKey = "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296" +
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
  await write(publisherKey, `${publicKey}\n`);
  await write(mock, `
    import {readFile} from "node:fs/promises";
    const request = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
    if (request.projectRoot !== ${JSON.stringify(artifact)} ||
        request.cacheSourceDir !== ${JSON.stringify(output)} ||
        request.retireCacheSourceAfterPromotion !== true ||
        request.outputDir === request.cacheSourceDir ||
        request.archiveShards !== 4 ||
        request.publisherPublicKey !== ${JSON.stringify(publicKey)} ||
        !request.inputs.some(value => value.uri === "Runtime/main.pxir") ||
        !request.inputs.some(value => value.uri === "project.pxproject")) process.exit(42);
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
      requestId: request.requestId, reusedArtifact: true}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const packed = run(["pack", value.project, "--artifact", artifact,
    "--packager", mock, "--player", player, "--output", output,
    "--archive-shards", "4", "--publisher-key", publisherKey]);
  assert.equal(packed.status, 0, packed.stderr);
  assert.match(packed.stdout, /"reusedArtifact":true/u);
  assert.match(packed.stdout, /"packed":true/u);
  const rebuilt = run(["pack", value.project, "--packager", mock,
    "--player", player, "--output", join(value.root, "other-package")]);
  assert.equal(rebuilt.status, 1);
  assert.match(rebuilt.stderr, /PXSTORY1199/u);
  const overlapping = run(["pack", value.project, "--artifact", artifact,
    "--packager", mock, "--player", player, "--output", artifact]);
  assert.equal(overlapping.status, 1);
  assert.match(overlapping.stderr, /Unsafe output overlaps author input/u);
  assert.ok((await readdir(artifact)).includes("project.pxproject"));
  const incompatible = run(["build", value.project, "--artifact-only", "--artifact", artifact]);
  assert.equal(incompatible.status, 2);
  assert.match(incompatible.stderr, /--artifact-only and --artifact cannot be combined/u);
  const invalidShards = run(["pack", value.project, "--archive-shards", "65"]);
  assert.equal(invalidShards.status, 2);
  assert.match(invalidShards.stderr, /--archive-shards must be an integer from 1 to 64/u);
  const artifactOnlyShards = run(["build", value.project, "--artifact-only",
    "--archive-shards", "4"]);
  assert.equal(artifactOnlyShards.status, 2);
  assert.match(artifactOnlyShards.stderr, /--archive-shards requires a distribution build/u);
  const artifactOnlyPublisher = run(["build", value.project, "--artifact-only",
    "--publisher-key", publisherKey]);
  assert.equal(artifactOnlyPublisher.status, 2);
  assert.match(artifactOnlyPublisher.stderr, /--publisher-key requires a distribution build/u);
});

test("pack preserves structured native diagnostics instead of replacing them with a CLI error", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-packager-failure-"));
  const mock = join(tools, "mock-packager-failure.mjs");
  await write(mock, `
    const diagnostic = {severity: "error", code: "PXPKGTEST9001",
      message: "Component property is incompatible", documentId: "ui-document",
      sourceId: "component-property", hint: "Repair the exposed path", cause: "missing path",
      span: {path: "Content/UI/Panel.pxuicomponent",
        start: {line: 12, column: 3, offset: 100},
        end: {line: 12, column: 20, offset: 117}}, retryable: false};
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "failed",
      requestId: "failure", code: diagnostic.code, message: diagnostic.message,
      retryable: false, diagnostic, diagnostics: [diagnostic]}) + "\\n");
    process.exitCode = 1;
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await writeExecutable(player, "fixture");
  const packed = run(["pack", value.project, "--packager", mock,
    "--player", player, "--output", join(value.root, "packaged")]);
  assert.equal(packed.status, 1);
  assert.match(packed.stderr, /"code":"PXPKGTEST9001"/);
  assert.match(packed.stderr, /"documentId":"ui-document"/);
  assert.match(packed.stderr, /"path":"Content\/UI\/Panel\.pxuicomponent"/);
  assert.doesNotMatch(packed.stderr, /PXCLI1201/);
});

test("build publishes every declared extension ES module", async () => {
  const value = await fixture();
  const project = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  project.extensions = ["Content/Extensions/module.pxextension"];
  await write(value.project, JSON.stringify(project));
  await write(join(value.root, "Content/Extensions/module.pxextension"), JSON.stringify({
    format: "PrismatiXExtension", schemaRevision: 1, language: "javascript",
    id: "module", version: "1.0.0", requiredEngineVersion: "^0.1.0",
    entry: "entry.js", modules: ["lib/helper.js"], capabilities: ["runtime"],
    commands: [], actions: [],
  }));
  await write(join(value.root, "Content/Extensions/entry.js"),
    "import {value} from './lib/helper.js'; void value;\n");
  await write(join(value.root, "Content/Extensions/lib/helper.js"),
    "export const value = 1;\n");

  const output = join(value.root, "artifact-modules");
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  assert.equal((await stat(join(output, "Content/Extensions/entry.js"))).isFile(), true);
  assert.equal((await stat(join(output, "Content/Extensions/lib/helper.js"))).isFile(), true);
});

test("inspect-save cannot accept a JS-hashed envelope without the authoritative native validator", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-save-inspect-"));
  const path = join(root, "save.pxsav");
  const envelope: Record<string, unknown> = {
    format: "PrismatiXSave", schemaRevision: 1, engineVersion: "0.1.0",
    gameId: "cli-sample", packageFingerprint: "a".repeat(64),
    contentVersion: "chapter-1", saveVersion: 1,
    anchor: {runtimeDocumentId: "main", sourceId: "line", operationId: "operation"},
    state: {chapter: "Chapter 1", timestamp: 10, playtimeMs: 20},
  };
  envelope.integrityHash = createHash("sha256").update(stable(envelope)).digest("hex");
  await writeFile(path, Buffer.concat([Buffer.from("PRISMATIX-PERSIST\n"), Buffer.from(stable(envelope))]));
  // This intentionally incomplete state passed the old hash-only JS validator.
  // Native format/encryption acceptance is covered by the Player native suite.
  const inspected = run(["inspect-save", path, "--runtime", join(root, "absent-runtime")]);
  assert.equal(inspected.status, 1, inspected.stderr);
  assert.match(inspected.stderr, /PXCLI1401/);
  assert.equal(inspected.stdout, "");
});

const multiLocaleFixture = resolve("../..", "PrismatiXEngine/Tests/Fixtures/MultiLocaleVN");

test("a multi-locale project validates, builds and publishes every release-gated capability", async () => {
  const sample = join(multiLocaleFixture, "project.pxproject");
  for (const locale of ["zh-TW", "ja-JP"]) for (const scene of ["prologue", "chapter1"]) {
    const story = join(multiLocaleFixture, `Story/${locale}/${scene}.pxstory`);
    assert.doesNotMatch(await readFile(story, "utf8"), /\[id\s/u);
    assert.equal((await stat(`${story}.pxids.json`)).isFile(), true);
  }
  const root = await mkdtemp(join(tmpdir(), "prismatix-multi-locale-"));
  const output = join(root, "artifact");
  const validated = run(["validate", sample]);
  assert.equal(validated.status, 0, validated.stderr);
  const built = run(["build", sample, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);

  for (const path of [
    "Runtime/main.pxir", "Runtime/main.pxmap",
    "Runtime/Locales/zh-TW/main.pxir", "Runtime/Locales/zh-TW/main.pxmap",
    "Runtime/Locales/ja-JP/main.pxir", "Runtime/Locales/ja-JP/main.pxmap",
    "Content/UI/Title.pxui", "Content/UI/HUD.pxui", "Content/UI/Gallery.pxui",
    "Content/UI/VerticalRubyCard.pxuicomponent",
    "Content/Extensions/commercial.pxextension", "Content/Extensions/commercial.js",
    "Content/Extensions/lib/payload.js",
    "Content/Migrations/chapter-0-to-1.pxsave-migration",
    "Effects/dream-tone.pxeffect", "Effects/dream-tone.frag.hlsl",
    "Assets/ending.svg",
  ]) assert.equal((await stat(join(output, path))).isFile(), true, `missing ${path}`);

  const defaultIr = JSON.parse(await readFile(join(output, "Runtime/main.pxir"), "utf8")) as {
    operations: Array<{kind: string; operationId: string; sourceId: string}>;
  };
  const japaneseIr = JSON.parse(await readFile(join(output, "Runtime/Locales/ja-JP/main.pxir"), "utf8")) as typeof defaultIr;
  assert.ok(defaultIr.operations.some((operation) => operation.kind === "effect"));
  assert.ok(defaultIr.operations.some((operation) => operation.kind === "choiceOption"));
  assert.ok(defaultIr.operations.some((operation) => operation.kind === "narration"));
  assert.deepEqual(
    japaneseIr.operations.map(({kind, operationId, sourceId}) => ({kind, operationId, sourceId})),
    defaultIr.operations.map(({kind, operationId, sourceId}) => ({kind, operationId, sourceId})),
    "localized programs must share stable execution identity and topology",
  );
});

test("the Starter sample (Studio's default new project) validates and builds", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-starter-sample-"));
  try {
    // The sample pins the release; staging links this checkout's packages in.
    const root = join(parent, "starter");
    const staged = spawnSync(process.execPath, [resolve("../../scripts/stage-starter-sample.mjs"), root], {encoding: "utf8"});
    assert.equal(staged.status, 0, staged.stderr);
    const manifest = JSON.parse(await readFile(join(root, "prismatix.json"), "utf8")) as
      {characters?: Array<{alias: string; displayName: string; source: string}>;
        assets: Array<{kind: string; source: string}>; uiEntryPoints: Record<string, string>};
    // The Studio's new-project cast: 白音 and 小春, one portrait each.
    assert.deepEqual(manifest.characters, [
      {alias: "shirone", displayName: "白音", source: "Characters/shirone.pxcharacter"},
      {alias: "koharu", displayName: "小春", source: "Characters/koharu.pxcharacter"}]);
    assert.deepEqual(manifest.assets.filter((asset) => asset.kind === "background").map((asset) => asset.source),
      ["Assets/Backgrounds/street.jpg"]);
    assert.deepEqual(Object.keys(manifest.uiEntryPoints).sort(),
      ["backlog", "hud", "load", "save", "settings", "splash", "title"]);
    const pinned = JSON.parse(await readFile(join(root, "package.json"), "utf8")) as {devDependencies: Record<string, string>};
    const version = (JSON.parse(await readFile("package.json", "utf8")) as {version: string}).version;
    assert.deepEqual(pinned.devDependencies, {"@prismatix/cli": version, "@prismatix/authoring-sdk": version,
      "@prismatix/runtime": version});
    const story = await readFile(join(root, "Story/main.pxstory"), "utf8");
    assert.match(story, /^\[bg street duration=1s\]$/mu);
    assert.match(story, /^\[show shirone normal left\]$[\s\S]*^\[show koharu normal right\]$/mu);

    const validated = run(["validate", root]);
    assert.equal(validated.status, 0, validated.stdout + validated.stderr);
    const output = join(parent, "artifact");
    const built = run(["build", root, "--artifact-only", "--output", output]);
    assert.equal(built.status, 0, built.stdout + built.stderr);
    for (const name of ["Splash", "Title", "HUD", "Save", "Load", "Settings", "Backlog"])
      assert.equal((await stat(join(output, `Content/UI/${name}.pxui`))).isFile(), true, `missing ${name}.pxui`);
    for (const path of ["Assets/Backgrounds/street.jpg", "Assets/Characters/shirone/normal.png",
      "Assets/Characters/koharu/normal.png"])
      assert.equal((await stat(join(output, path))).isFile(), true, `missing ${path}`);
    const ir = JSON.parse(await readFile(join(output, "Runtime/main.pxir"), "utf8")) as
      {operations: Array<{kind: string; arguments?: Record<string, string>}>};
    assert.deepEqual(ir.operations.filter((operation) => operation.kind === "background")
      .map((operation) => operation.arguments?.asset), ["Assets/Backgrounds/street.jpg"]);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("the V1 to V2 release journey builds TSX screens for every route and two locales from public inputs", async () => {
  const fixture = resolve("../..", "PrismatiXEngine/Tests/Fixtures/ReleaseJourney/project.pxproject");
  const authored = JSON.parse(await readFile(fixture, "utf8")) as {uiEntryPoints: Record<string, string>};
  assert.deepEqual(Object.keys(authored.uiEntryPoints).sort(), ["backlog", "gallery", "hud", "load", "save", "title"]);
  assert.ok(Object.values(authored.uiEntryPoints).every((path) => path.endsWith(".tsx")));
  // scripts/release-journey.mjs generates the Story and media, builds both
  // versions and checks the artifacts (canonical .pxui only, one topology per
  // locale and version, locale-specific picture and voice, changed V2 media).
  // CTest PrismatiX.SDK.ReleaseJourney runs it again with the native tools.
  const root = await mkdtemp(join(tmpdir(), "prismatix-release-journey-test-"));
  try {
    const script = resolve("../..", "scripts/release-journey.mjs");
    const result = spawnSync(process.execPath, [script, "--cli", resolve("dist/src/cli.js"), "--output", root],
      {encoding: "utf8"});
    assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
    assert.match(result.stdout, /Release journey passed/u);
    assert.equal((await stat(join(root, "author-v1/project.pxproject"))).isFile(), true);
    assert.equal((await stat(join(root, "author-v2/Content/UI/Backlog.pxui"))).isFile(), true);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("distribution.linkMedia hard-links project media into the artifact", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-link-media-"));
  const root = join(parent, "linked-game");
  try {
    await createStarter(root, {name: "Linked", install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const projectPath = join(root, "prismatix.json");
    const manifest = JSON.parse(await readFile(projectPath, "utf8")) as Record<string, unknown>;
    await writeFile(projectPath, JSON.stringify({...manifest, distribution: {linkMedia: true}}));
    const artifact = join(parent, "artifact");
    const built = run(["build", projectPath, "--artifact-only", "--output", artifact]);
    assert.equal(built.status, 0, built.stderr);
    const media = await stat(join(artifact, "Assets", "Backgrounds", "street.jpg"));
    assert.ok(media.nlink >= 2, "the artifact shares the project's image instead of copying it");
    await writeFile(projectPath, JSON.stringify({...manifest, distribution: {linkMedia: "yes"}}));
    assert.notEqual(run(["build", projectPath, "--artifact-only", "--output", join(parent, "bad")]).status, 0);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("distribution.transcode stores smaller media under the same paths and caches encodings", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-transcode-"));
  const root = join(parent, "small-game");
  try {
    await createStarter(root, {name: "Small", install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const tools = join(parent, "tools");
    const packager = join(tools, "mock-packager.mjs");
    const calls = join(tools, "calls.log");
    // Encodes as "WEBP" + 8 source bytes; grow.png would grow and broken.png
    // fails, so both must stay original. Each call is logged.
    await write(packager, `
      import {appendFileSync, readFileSync, writeFileSync} from "node:fs";
      const [mode, path] = process.argv.slice(2);
      if (mode !== "--transcode") process.exit(2);
      const request = JSON.parse(readFileSync(path, "utf8"));
      appendFileSync(${JSON.stringify(calls)}, JSON.stringify({images: request.images,
        audio: request.audio, jobs: request.jobs.map((job) => job.source.replaceAll("\\\\", "/"))}) + "\\n");
      request.jobs.forEach((job, index) => {
        const input = readFileSync(job.source);
        const event = {event: "transcoded", index, inputBytes: input.length};
        if (job.source.endsWith("grow.png")) Object.assign(event, {result: "kept", reason: "larger"});
        else if (job.source.endsWith("broken.png"))
          Object.assign(event, {result: "kept", reason: "failed", detail: "image did not decode"});
        else {
          const output = Buffer.concat([Buffer.from("WEBP"), input.subarray(0, 8)]);
          writeFileSync(job.output, output);
          Object.assign(event, {result: "encoded", outputBytes: output.length});
        }
        console.log(JSON.stringify(event));
      });
      console.log(JSON.stringify({event: "completed", jobs: request.jobs.length}));
    `);
    const media = Buffer.alloc(4096, 7);
    await write(join(root, "Assets", "extra", "grow.png"), "small");
    await writeFile(join(root, "Assets", "extra", "broken.png"), media);
    await mkdir(join(root, "Assets", "ui"), {recursive: true});
    await writeFile(join(root, "Assets", "ui", "button.png"), media);
    await writeFile(join(root, "Assets", "theme.ogg"), media);
    const projectPath = join(root, "prismatix.json");
    const manifest = JSON.parse(await readFile(projectPath, "utf8")) as Record<string, unknown>;
    await writeFile(projectPath, JSON.stringify({...manifest, resourceIncludes: ["Assets"],
      distribution: {transcode: {images: {format: "webp", quality: 80},
        audio: {format: "opus", bitrate: 48}, exclude: ["Assets/ui/**"]}}}));
    const artifact = join(parent, "artifact");
    const build = (...extra: string[]) => run(["build", projectPath, "--artifact-only",
      "--output", artifact, "--packager", packager, ...extra]);
    const logged = async () => (await readFile(calls, "utf8")).trim().split("\n")
      .map((line) => JSON.parse(line) as {images: unknown; audio: unknown; jobs: string[]});
    const portrait = join(root, "Assets", "Characters", "shirone", "normal.png");
    const original = await readFile(portrait);

    const first = build();
    assert.equal(first.status, 0, first.stderr);
    const [call] = await logged();
    assert.deepEqual(call!.images, {lossless: false, quality: 80, effort: 4});
    assert.deepEqual(call!.audio, {codec: "opus", bitrate: 48000});
    // The starter's PrismatiX splash logo is encoded with the rest.
    const logo = ["Content/UI/PrismatiX/PrismatiXEngine_Logo.png"];
    assert.deepEqual(call!.jobs.map((path) => path.slice(root.length + 1)).sort(), [
      "Assets/Characters/koharu/normal.png", "Assets/Characters/shirone/normal.png",
      "Assets/extra/broken.png", "Assets/extra/grow.png", "Assets/theme.ogg", ...logo]);
    const stored = await readFile(join(artifact, "Assets", "Characters", "shirone", "normal.png"));
    assert.deepEqual(stored, Buffer.concat([Buffer.from("WEBP"), original.subarray(0, 8)]),
      "the logical path keeps its name and holds the smaller encoding");
    assert.equal((await readFile(join(artifact, "Assets", "theme.ogg"))).subarray(0, 4).toString(), "WEBP");
    for (const kept of ["Assets/extra/grow.png", "Assets/extra/broken.png", "Assets/ui/button.png"])
      assert.deepEqual(await readFile(join(artifact, kept)), await readFile(join(root, kept)), kept);
    assert.match(first.stderr, /"code":"PXCLI1231".*broken\.png/u);
    const summary = first.stderr.split("\n").map((line) => {
      try { return JSON.parse(line) as Record<string, unknown>; } catch { return {}; }
    }).find((event) => event.code === "PXCLI1230");
    assert.deepEqual(summary?.images, {files: 5, inputBytes: original.length +
      (await stat(join(root, "Assets", "Characters", "koharu", "normal.png"))).size + 5 + 4096 +
      (await stat(join(root, logo[0]!))).size,
    outputBytes: 12 * 3 + 5 + 4096});

    // Unchanged media comes from the cache, and the artifact is kept as is;
    // only the failed file is tried again.
    const second = build();
    assert.equal(second.status, 0, second.stderr);
    assert.deepEqual((await logged())[1]!.jobs.map((path) => path.slice(root.length + 1)),
      ["Assets/extra/broken.png"]);
    assert.match(second.stderr, /"phase":"reuse-artifact"/u);

    // An edited file is encoded again and its stale cache entry is dropped.
    const cacheEntries = async () => (await readdir(join(root, ".prismatix", "transcode-cache"),
      {recursive: true})).filter((name) => name.endsWith(".json")).length;
    const entriesBefore = await cacheEntries();
    await writeFile(portrait, Buffer.concat([original, Buffer.from("x")]));
    const third = build();
    assert.equal(third.status, 0, third.stderr);
    assert.deepEqual((await logged())[2]!.jobs.map((path) => path.slice(root.length + 1)).sort(),
      ["Assets/Characters/shirone/normal.png", "Assets/extra/broken.png"]);
    assert.equal(await cacheEntries(), entriesBefore);

    const plain = build("--no-transcode");
    assert.equal(plain.status, 0, plain.stderr);
    assert.equal((await logged()).length, 3, "--no-transcode never calls the encoder");
    assert.deepEqual(await readFile(join(artifact, "Assets", "Characters", "shirone", "normal.png")),
      await readFile(portrait));

    await writeFile(projectPath, JSON.stringify({...manifest,
      distribution: {transcode: {images: {format: "avif"}}}}));
    const invalid = build();
    assert.notEqual(invalid.status, 0);
    assert.match(invalid.stderr, /distribution\.transcode\.images\.format must be webp/u);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

function arm64Elf(): Buffer {
  const header = Buffer.alloc(64);
  header.writeUInt32BE(0x7f454c46, 0);
  header[4] = 2; header[5] = 1;
  header.writeUInt16LE(183, 18);
  return header;
}

function squarePng(size: number): Buffer {
  const header = Buffer.alloc(33);
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(header, 0);
  header.writeUInt32BE(13, 8);
  header.write("IHDR", 12, "latin1");
  header.writeUInt32BE(size, 16);
  header.writeUInt32BE(size, 20);
  return header;
}

test("pack --target android stages the Gradle template and collects the APK and AAB", async () => {
  const parent = await mkdtemp(join(tmpdir(), "prismatix-cli-android-"));
  try {
    const root = join(parent, "game");
    await createStarter(root, {name: "Android Sample", install: async (directory) => {
      await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      await symlink(resolve("."), join(directory, "node_modules", "@prismatix", "cli"), "junction");
    }});
    const projectPath = join(root, "prismatix.json");
    const manifest = JSON.parse(await readFile(projectPath, "utf8")) as Record<string, unknown>;
    await writeFile(join(root, "Assets", "icon.png"), squarePng(192));
    const android = {applicationId: "com.example.androidsample", versionCode: 4,
      label: "Ann's <Game> & co", icon: "Assets/icon.png"};
    await writeFile(projectPath, JSON.stringify({...manifest, distribution: {android}}));

    const tools = join(parent, "tools");
    const packager = join(tools, "mock-packager.mjs");
    await write(packager, `
      import {copyFile, mkdir, readFile, writeFile} from "node:fs/promises";
      import {basename, join} from "node:path";
      const request = JSON.parse(await readFile(process.argv[3], "utf8"));
      const out = request.outputDir;
      await mkdir(join(out, "Package"), {recursive: true});
      await mkdir(join(out, "Notices"), {recursive: true});
      await writeFile(join(out, "Package", "manifest.json"), JSON.stringify({gameId: request.gameId,
        title: request.title, resolution: {width: request.width, height: request.height}}));
      await writeFile(join(out, "Content.pdx"), "pdx");
      await writeFile(join(out, "Notices", "LICENSE"), "license");
      await writeFile(join(out, "SDL3.dll"), "desktop library");
      await copyFile(request.playerExecutable, join(out, basename(request.playerExecutable)));
      process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
        requestId: request.requestId}) + "\\n");
    `);
    const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
    await writeExecutable(player, "desktop player");
    const gradle = join(tools, "mock-gradle.mjs");
    await write(gradle, `
      import {mkdir, readFile, readdir, stat, writeFile} from "node:fs/promises";
      import {join, relative} from "node:path";
      const args = process.argv.slice(2);
      const project = args[args.indexOf("-p") + 1];
      const tasks = args.filter((value) => /^(assemble|bundle)/.test(value));
      const main = join(project, "app", "src", "main");
      const exists = (path) => stat(path).then(() => true, () => false);
      const list = async (directory) => (await readdir(directory, {recursive: true, withFileTypes: true}))
        .filter((entry) => entry.isFile())
        .map((entry) => relative(directory, join(entry.parentPath, entry.name)).split("\\\\").join("/")).sort();
      await writeFile(process.env.PX_GRADLE_RECORD, JSON.stringify({tasks,
        properties: await readFile(join(project, "prismatix.properties"), "utf8"),
        local: await readFile(join(project, "local.properties"), "utf8"),
        strings: await readFile(join(main, "res", "values", "strings.xml"), "utf8"),
        jni: await list(join(main, "jniLibs", "arm64-v8a")),
        assets: await list(join(main, "assets")),
        icon: await exists(join(main, "res", "mipmap-xxxhdpi", "ic_launcher.png")),
        adaptive: await exists(join(main, "res", "mipmap-anydpi-v26")),
        sdlGlue: await exists(join(main, "java", "org", "libsdl", "app", "SDLActivity.java"))}));
      const outputs = join(project, "app", "build", "outputs");
      const emit = async (path, text) => {
        await mkdir(join(outputs, path, ".."), {recursive: true});
        await writeFile(join(outputs, path), text);
      };
      if (tasks.includes("assembleDebug")) await emit("apk/debug/app-debug.apk", "debug apk");
      if (tasks.includes("assembleRelease")) await emit("apk/release/app-release.apk", "release apk");
      if (tasks.includes("bundleRelease")) await emit("bundle/release/app-release.aab", "release aab");
    `);
    const libraries = join(parent, "android", "shipped", "arm64-v8a");
    await mkdir(libraries, {recursive: true});
    await writeFile(join(libraries, "libmain.so"), arm64Elf());
    await writeFile(join(libraries, "libavcodec.so"), arm64Elf());
    await writeFile(join(libraries, "notes.txt"), "not a library");
    await writeFile(join(libraries, "prismatix-player.json"),
      JSON.stringify({format: "PrismatiXAndroidPlayer", variant: "shipped", sdlVersion: "3.4.10"}));
    const sdk = join(parent, "sdk");
    await mkdir(join(sdk, "platforms"), {recursive: true});
    const record = join(parent, "gradle-record.json");
    const output = join(parent, "android-out");
    const pack = (extra: readonly string[], env: Record<string, string> = {}) => spawnSync(process.execPath,
      [resolve("dist/src/cli.js"), "pack", projectPath, "--target", "android", "--packager", packager,
        "--player", player, "--android-player", join(parent, "android", "shipped"), "--gradle", gradle,
        "--android-sdk", sdk, "--output", output, ...extra, "--json"],
      {encoding: "utf8", env: {...process.env, PX_GRADLE_RECORD: record, ...env}});
    type GradleRecord = {tasks: string[]; properties: string; local: string; strings: string;
      jni: string[]; assets: string[]; icon: boolean; adaptive: boolean; sdlGlue: boolean};

    const debug = pack([]);
    assert.equal(debug.status, 0, debug.stderr);
    assert.match(debug.stderr, /PXCLI1251/u, "an unsigned build warns that it is for testing");
    const debugSummary = JSON.parse(debug.stdout.trim().split("\n").at(-1)!) as Record<string, unknown>;
    assert.equal(debugSummary.buildType, "debug");
    assert.equal(await readFile(String(debugSummary.apk), "utf8"), "debug apk");
    let seen = JSON.parse(await readFile(record, "utf8")) as GradleRecord;
    assert.deepEqual(seen.tasks, ["assembleDebug"]);
    assert.deepEqual(seen.jni, ["libavcodec.so", "libmain.so"]);
    assert.deepEqual(seen.assets, ["Content.pdx", "Notices/LICENSE", "Package/manifest.json"],
      "the desktop Player and its libraries stay out of the APK");
    assert.match(seen.properties, /^applicationId=com\.example\.androidsample$/mu);
    assert.match(seen.properties, /^versionCode=4$/mu);
    assert.match(seen.properties, /^screenOrientation=sensorLandscape$/mu);
    assert.doesNotMatch(seen.properties, /keystore/u);
    assert.match(seen.local, /^sdk\.dir=.*sdk$/mu);
    assert.match(seen.strings, /Ann\\'s &lt;Game&gt; &amp; co/u);
    assert.deepEqual([seen.icon, seen.adaptive, seen.sdlGlue], [true, false, true]);

    await writeFile(join(parent, "release.jks"), "keystore");
    await writeFile(projectPath, JSON.stringify({...manifest, distribution: {android: {...android,
      keystore: {path: "../release.jks", alias: "upload", storePasswordEnv: "PX_TEST_STORE_PASSWORD"}}}}));
    await rm(record);
    const missing = pack([], {PX_TEST_STORE_PASSWORD: ""});
    assert.equal(missing.status, 1);
    assert.match(missing.stderr, /PX_TEST_STORE_PASSWORD must hold the keystore password/u);
    assert.equal(await stat(record).then(() => true, () => false), false, "Gradle must not run");

    const release = pack([], {PX_TEST_STORE_PASSWORD: "correct horse"});
    assert.equal(release.status, 0, release.stderr);
    const releaseSummary = JSON.parse(release.stdout.trim().split("\n").at(-1)!) as Record<string, unknown>;
    assert.equal(releaseSummary.buildType, "release");
    assert.equal(await readFile(String(releaseSummary.aab), "utf8"), "release aab");
    seen = JSON.parse(await readFile(record, "utf8")) as GradleRecord;
    assert.deepEqual(seen.tasks, ["assembleRelease", "bundleRelease"]);
    assert.match(seen.properties, /^storePasswordEnv=PX_TEST_STORE_PASSWORD$/mu);
    assert.match(seen.properties, /^keyAlias=upload$/mu);
    assert.doesNotMatch(seen.properties, /correct horse/u, "passwords never reach a file");
    const forcedDebug = pack(["--debug"], {PX_TEST_STORE_PASSWORD: ""});
    assert.equal(forcedDebug.status, 0, forcedDebug.stderr);

    await writeFile(join(libraries, "prismatix-player.json"),
      JSON.stringify({format: "PrismatiXAndroidPlayer", variant: "qa", sdlVersion: "3.4.10"}));
    const qa = pack(["--debug"]);
    assert.equal(qa.status, 1);
    assert.match(qa.stderr, /QA Player, which must never ship/u);
    await writeFile(join(libraries, "prismatix-player.json"),
      JSON.stringify({format: "PrismatiXAndroidPlayer", variant: "shipped", sdlVersion: "3.4.10"}));

    const foreign = join(parent, "foreign");
    await write(join(foreign, "keep.txt"), "user file");
    const refused = spawnSync(process.execPath, [resolve("dist/src/cli.js"), "pack", projectPath,
      "--target", "android", "--android-player", libraries, "--gradle", gradle, "--android-sdk", sdk,
      "--output", foreign, "--debug", "--json"], {encoding: "utf8"});
    assert.equal(refused.status, 1);
    assert.match(refused.stderr, /not empty and was not created by prismatix/u);

    assert.equal(run(["pack", projectPath, "--debug"]).status, 2);
    assert.equal(run(["pack", projectPath, "--target", "ios"]).status, 2);
    assert.equal(run(["pack", projectPath, "--target", "android", "--check"]).status, 2);
  } finally { await rm(parent, {recursive: true, force: true}); }
});

test("run --dev packs into .prismatix/run and reports the game's lifecycle as NDJSON", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-run-dev-"));
  try {
    // The "game" is a Node executable that exits at once (no script on stdin).
    const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
    await link(process.execPath, player);
    const mock = join(tools, "mock-packager.mjs");
    await write(mock, `
      import {link, readFile} from "node:fs/promises";
      import {basename, join} from "node:path";
      const request = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
      if (request.compression !== "none") process.exit(43);
      await link(request.playerExecutable, join(request.outputDir, basename(request.playerExecutable)));
      process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
        requestId: request.requestId}) + "\\n");
    `);
    const result = run(["run", value.root, "--dev", "--packager", mock, "--player", player]);
    assert.equal(result.status, 0, result.stderr);
    const records = result.stderr.split(/\r?\n/u).filter(Boolean)
      .map((line) => JSON.parse(line) as Record<string, unknown>)
      .filter((record) => String(record.event).startsWith("run."));
    assert.deepEqual(records.map((record) => `${String(record.event)}:${String(record.phase)}`),
      ["run.build:start", "run.build:done", "run.player:start", "run.player:exit"]);
    const output = join(value.root, ".prismatix", "run");
    assert.equal(records[1]!.output, output);
    assert.equal(records[2]!.pid, records[3]!.pid);
    assert.equal(typeof records[2]!.pid, "number");
    assert.equal(records[3]!.exitCode, 0);
    assert.equal((await stat(join(value.root, ".prismatix", "run-artifact", "project.pxproject"))).isFile(), true);
    // A dev pack records no release lineage in the project.
    assert.equal((await readdir(value.root)).includes(".prismatix-release"), false);
  } finally { await rm(tools, {recursive: true, force: true}); }
});

test("run --dev skips a build nothing changed, rebuilds after an edit, and --prepare starts no game", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-run-unchanged-"));
  try {
    const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
    await link(process.execPath, player);
    const mock = join(tools, "mock-packager.mjs");
    await write(mock, `
      import {link, readFile} from "node:fs/promises";
      import {basename, join} from "node:path";
      const request = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
      if (request.looseContent !== true) process.exit(44);
      await link(request.playerExecutable, join(request.outputDir, basename(request.playerExecutable)));
      process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
        requestId: request.requestId}) + "\\n");
    `);
    const play = (...extra: string[]) => {
      const result = run(["run", value.root, "--dev", "--packager", mock, "--player", player, ...extra]);
      assert.equal(result.status, 0, result.stderr);
      const records = result.stderr.split(/\r?\n/u).filter(Boolean)
        .map((line) => JSON.parse(line) as Record<string, unknown>);
      return {
        unchanged: records.some((record) => record.code === "PXCLI1317"),
        compiled: records.some((record) => record.phase === "compile"),
        started: records.some((record) => record.event === "run.player" && record.phase === "start"),
      };
    };
    assert.deepEqual(play(), {unchanged: false, compiled: true, started: true});
    assert.deepEqual(play(), {unchanged: true, compiled: false, started: true});
    // An edited project document is read again.
    await writeFile(value.project, `${await readFile(value.project, "utf8")}\n`);
    assert.deepEqual(play("--prepare"), {unchanged: false, compiled: true, started: false});
    assert.deepEqual(play(), {unchanged: true, compiled: false, started: true});
  } finally { await rm(tools, {recursive: true, force: true}); }
});

test("run --at names a Story position and refuses a QA pack outside .prismatix", async () => {
  const value = await fixture();
  const outside = run(["run", value.root, "--at", "main:1", "--output", join(value.root, "dist", "qa"),
    "--player-qa", process.execPath]);
  assert.equal(outside.status, 1);
  assert.match(outside.stderr, /QA Player, which is for local development only/u);
  const help = runHuman(["run", "--help"]);
  assert.match(help.stdout + help.stderr, /\.prismatix\/run-qa/u);
  assert.match(help.stdout + help.stderr, /never be released/u);
});
