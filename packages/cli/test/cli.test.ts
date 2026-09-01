import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {mkdtemp, mkdir, readFile, stat, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {spawnSync} from "node:child_process";
import test from "node:test";

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

function run(args: readonly string[]) {
  return spawnSync(process.execPath, [resolve("dist/src/cli.js"), ...args], {encoding: "utf8"});
}

async function fixture(legacy = false): Promise<{root: string; project: string}> {
  const root = await mkdtemp(join(tmpdir(), "prismatix-cli-test-"));
  const project = join(root, "project.pxproject");
  await write(project, JSON.stringify({
    format: "PrismatiXProject", schemaRevision: legacy ? 1 : 2, id: "cli-sample",
    name: "CLI Sample", version: "0.2.0",
    ...(legacy ? {} : {contentVersion: "chapter-1"}), saveVersion: 1,
    resolution: {width: 1280, height: 720}, entry: {story: "main", ui: "title"},
    defaultLocale: "en", supportedLocales: ["en"],
    storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame",
    extensions: [], uiEntryPoints: {title: "Content/UI/Title.pxui"}, assets: [],
  }));
  await write(join(root, "Story/story.pxindex"), JSON.stringify({
    format: "PrismatiXStoryIndex", schemaRevision: legacy ? 1 : 2, id: "main", entryScene: "main",
    chapters: [{id: "chapter", title: "Chapter", scenes: ["main"]}],
    scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}],
  }));
  await write(join(root, "Story/main.pxstory"), "Hello.\n[end]\n");
  await write(join(root, "Content/game.pxgame"), JSON.stringify({
    format: "PrismatiXGame", schemaRevision: legacy ? 1 : 2,
    variables: legacy
      ? [{name: "route", type: "string", default: "common", persistent: false}]
      : [{name: "route", type: "string", default: "common", scope: "session"}],
    gallery: [], unlockables: [],
  }));
  const id = "11111111-1111-4111-8111-111111111111";
  await write(join(root, "Content/UI/Title.pxui"), JSON.stringify({
    format: "PrismatiXUIScene", schemaRevision: 2, id, revision: 1,
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
    format: "PrismatiXLocale", schemaRevision: legacy ? 1 : 2, locale: "en", strings: {},
  }));
  return {root, project};
}

test("validate and build use the same canonical project compiler", async () => {
  const value = await fixture();
  const validated = run(["validate", value.project]);
  assert.equal(validated.status, 0, validated.stderr);
  const output = join(value.root, "artifact");
  const built = run(["build", value.project, "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  assert.equal((await stat(join(output, "Runtime/main.pxir"))).isFile(), true);
  assert.equal((await stat(join(output, "Runtime/main.pxmap"))).isFile(), true);
});

test("pack invokes the native file-based request protocol", async () => {
  const value = await fixture();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-packager-"));
  const mock = join(tools, "mock-packager.mjs");
  await write(mock, `
    import {readFile} from "node:fs/promises";
    import {isAbsolute} from "node:path";
    const [flag, path] = process.argv.slice(2);
    if (flag !== "--request" || !path || !isAbsolute(path)) process.exit(41);
    const request = JSON.parse(await readFile(path, "utf8"));
    if (request.format !== "PrismatiXPackageRequest" || request.schemaRevision !== 2 ||
        request.startScript !== "Runtime/main.pxir" || !Array.isArray(request.inputs)) process.exit(42);
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
      requestId: request.requestId, fileRequest: true}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await write(player, "fixture");
  const output = join(value.root, "packaged");
  const packed = run(["pack", value.project, "--packager", mock,
    "--player", player, "--output", output]);
  assert.equal(packed.status, 0, packed.stderr);
  assert.match(packed.stdout, /"fileRequest":true/);
  assert.match(packed.stdout, /"packed":true/);
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
  await write(player, "fixture");
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
    format: "PrismatiXExtension", schemaRevision: 2, language: "javascript",
    id: "module", version: "1.0.0", requiredEngineVersion: "^0.2.0",
    entry: "entry.js", modules: ["lib/helper.js"], capabilities: ["runtime"],
    commands: [], actions: [],
  }));
  await write(join(value.root, "Content/Extensions/entry.js"),
    "import {value} from './lib/helper.js'; void value;\n");
  await write(join(value.root, "Content/Extensions/lib/helper.js"),
    "export const value = 1;\n");

  const output = join(value.root, "artifact-modules");
  const built = run(["build", value.project, "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  assert.equal((await stat(join(output, "Content/Extensions/entry.js"))).isFile(), true);
  assert.equal((await stat(join(output, "Content/Extensions/lib/helper.js"))).isFile(), true);
});

test("migrate is dry-run by default and commits with a recoverable backup", async () => {
  const value = await fixture(true);
  const legacyProject = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  legacyProject.graphicsTier = "gpu-effects";
  legacyProject.engineCompatibility = {minimum: "0.2.0", maximum: "0.2.x"};
  await write(value.project, JSON.stringify(legacyProject));
  const originalProject = await readFile(value.project, "utf8");
  const dryRun = run(["migrate", value.project, "--content-version", "chapter-1"]);
  assert.equal(dryRun.status, 0, dryRun.stderr);
  assert.equal(await readFile(value.project, "utf8"), originalProject);
  const backup = join(value.root, "backup");
  const committed = run(["migrate", value.project, "--content-version", "chapter-1", "--write", "--backup", backup]);
  assert.equal(committed.status, 0, committed.stderr);
  assert.match(await readFile(value.project, "utf8"), /"contentVersion":\s*"chapter-1"/);
  assert.match(await readFile(value.project, "utf8"), /"graphicsTier":\s*"gpu-effects"/);
  assert.match(await readFile(value.project, "utf8"), /"engineCompatibility"/);
  assert.match(await readFile(join(value.root, "Content/game.pxgame"), "utf8"), /"scope":\s*"session"/);
  assert.equal((await stat(join(backup, "project.pxproject"))).isFile(), true);
});

test("migrate imports lossless TypedDocument project, catalog, and UI resources", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-cli-typed-migration-"));
  const project = join(root, "project.pxproject");
  const projectId = "11111111-1111-4111-8111-111111111111";
  const rootId = "22222222-2222-4222-8222-222222222222";
  const variableId = "33333333-3333-4333-8333-333333333333";
  await write(project, `@pxproject 4 ${projectId} PrismatiXProject
name = "Typed Project"
version = "0.1.0"
saveVersion = 1
width = 1280
height = 720
entry = object("story", "main", "ui", "title")
defaultLocale = "en"
supportedLocales = array("en")
storyIndex = "Story/story.pxindex"
gameCatalog = "Content/Game.pxres"
extensions = array()
uiEntryPoints = object("title", "Content/UI/Title.pxscene")
`);
  await write(join(root, "Story/story.pxindex"), JSON.stringify({
    format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "main",
    chapters: [{id: "chapter", title: "Chapter", scenes: ["main"]}],
    scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}],
  }));
  await write(join(root, "Story/main.pxstory"), "Typed migration.\n[end]\n");
  await write(join(root, "Content/Game.pxres"), `@pxresource 4 ${projectId} GameCatalog

[node "${variableId}", "", "route", "Variable"]
name = "route"
default = "common"
persistent = false
`);
  await write(join(root, "Content/UI/Title.pxscene"), `@pxscene 4 ${projectId} UIScene
uiSchemaVersion = 5
canvasSize = vec2(1280, 720)

[node "${rootId}", "", "Root", "Control"]
`);
  await write(join(root, "Content/Localization/en.json"), JSON.stringify({title: "Typed"}));

  const migrated = run(["migrate", project, "--content-version", "chapter-1", "--write",
    "--backup", join(root, "backup")]);
  assert.equal(migrated.status, 0, migrated.stderr);
  const canonicalProject = JSON.parse(await readFile(project, "utf8")) as Record<string, unknown>;
  assert.equal(canonicalProject.format, "PrismatiXProject");
  assert.equal(canonicalProject.contentVersion, "chapter-1");
  const game = JSON.parse(await readFile(join(root, "Content/Game.pxres"), "utf8")) as {
    format: string; variables: Array<{name: string; type: string; default: unknown; scope: string}>;
  };
  assert.deepEqual(game, {format: "PrismatiXGame", schemaRevision: 2,
    variables: [{name: "route", type: "string", default: "common", scope: "session"}],
    gallery: [], unlockables: []});
  const ui = JSON.parse(await readFile(join(root, "Content/UI/Title.pxscene"), "utf8")) as {
    format: string; width: number; height: number; rootId: string;
  };
  assert.deepEqual({format: ui.format, width: ui.width, height: ui.height, rootId: ui.rootId},
    {format: "PrismatiXUIScene", width: 1280, height: 720, rootId});
  const validated = run(["validate", project]);
  assert.equal(validated.status, 0, validated.stderr);
});

test("migrate refuses lossy fields and reports their exact source span", async () => {
  const value = await fixture(true);
  const project = JSON.parse(await readFile(value.project, "utf8")) as Record<string, unknown>;
  project.studioUi = {legacy: true};
  const authored = `${JSON.stringify(project, null, 2)}\n`;
  await write(value.project, authored);
  const migrated = run(["migrate", value.project, "--content-version", "chapter-1"]);
  assert.equal(migrated.status, 1, migrated.stderr);
  const report = JSON.parse(migrated.stdout) as {
    diagnostics: Array<{code: string; span?: {path: string; start: {line: number; column: number; offset: number}; end: {line: number; column: number; offset: number}}}>;
  };
  const issue = report.diagnostics.find((value) => value.code === "PXMIG2004");
  assert.ok(issue?.span);
  assert.equal(issue.span.path, value.project);
  assert.ok(issue.span.start.line > 1 && issue.span.end.offset > issue.span.start.offset);
  assert.equal(await readFile(value.project, "utf8"), authored);
});

test("migrate pinpoints the non-string value in an unsafe legacy locale", async () => {
  const value = await fixture(true);
  const localePath = join(value.root, "Content/Localization/en.json");
  const authored = "{\n  \"welcome\": \"Hello\",\n  \"broken\": 42\n}\n";
  await write(localePath, authored);
  const migrated = run(["migrate", value.project, "--content-version", "chapter-1"]);
  assert.equal(migrated.status, 1, migrated.stderr);
  const report = JSON.parse(migrated.stdout) as {
    diagnostics: Array<{code: string; span?: {path: string; start: {line: number; column: number; offset: number}; end: {line: number; column: number; offset: number}}}>;
  };
  const issue = report.diagnostics.find((value) => value.code === "PXMIG2003");
  assert.ok(issue?.span);
  assert.equal(issue.span.path, localePath);
  assert.equal(issue.span.start.line, 3);
  assert.equal(authored.slice(issue.span.start.offset, issue.span.end.offset), "42");
  assert.equal(await readFile(localePath, "utf8"), authored);
});

test("inspect-save verifies the V2 envelope integrity before reporting metadata", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-save-inspect-"));
  const path = join(root, "save.pxsav");
  const envelope: Record<string, unknown> = {
    format: "PrismatiXSave", schemaRevision: 4, engineVersion: "0.2.0",
    gameId: "cli-sample", packageFingerprint: "a".repeat(64),
    contentVersion: "chapter-1", saveVersion: 1,
    anchor: {runtimeDocumentId: "main", sourceId: "line", operationId: "operation"},
    state: {chapter: "Chapter 1", timestamp: 10, playtimeMs: 20},
  };
  envelope.integrityHash = createHash("sha256").update(stable(envelope)).digest("hex");
  await writeFile(path, Buffer.concat([Buffer.from("PRISMATIX-PERSIST\n"), Buffer.from(stable(envelope))]));
  const inspected = run(["inspect-save", path]);
  assert.equal(inspected.status, 0, inspected.stderr);
  assert.match(inspected.stdout, /"gameId": "cli-sample"/);
});

test("the commercial sample compiles and publishes every release-gated capability", async () => {
  const sample = resolve("../..", "Samples/CommercialVN/project.pxproject");
  const root = await mkdtemp(join(tmpdir(), "prismatix-commercial-sample-"));
  const output = join(root, "artifact");
  const validated = run(["validate", sample]);
  assert.equal(validated.status, 0, validated.stderr);
  const built = run(["build", sample, "--output", output]);
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
