import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {mkdtemp, mkdir, readFile, readdir, stat, symlink, utimes, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {spawnSync} from "node:child_process";
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

function run(args: readonly string[]) {
  return spawnSync(process.execPath, [resolve("dist/src/cli.js"), ...args, "--json"], {encoding: "utf8"});
}

function runHuman(args: readonly string[]) {
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
  const ui = await readFile(join(root, "Content/UI/Title.pxui"), "utf8");
  assert.match(ui, /[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}/u);
  assert.doesNotMatch(ui, /\{\{[A-Z_]+\}\}/u);
  const manifest = JSON.parse(await readFile(project, "utf8")) as Record<string, unknown>;
  assert.deepEqual(Object.keys(manifest).sort(),
    ["assets", "characters", "defaultLocale", "id", "name", "resolution", "uiEntryPoints"]);
  assert.equal((await stat(join(root, "Content/Localization/zh-TW.json"))).isFile(), true);
  assert.deepEqual((await readdir(join(root, "Assets"))).sort(), ["guide.png", "window.png"]);
});

test("runtime discovery accepts an installed prefix manifest and reports attempted sources", async () => {
  const prefix = await mkdtemp(join(tmpdir(), "prismatix-runtime-prefix-"));
  const bin = join(prefix, "bin");
  const manifest = join(prefix, "share", "PrismatiX", "runtime-manifest.json");
  await write(join(bin, process.platform === "win32" ? "PrismatiXPreviewHost.exe" : "PrismatiXPreviewHost"), "fixture");
  await write(manifest, JSON.stringify({format: "PrismatiXRuntimeManifest", schemaRevision: 1,
    version: "0.2.0", target: `${process.platform}-${process.arch}`, binDir: "../../bin", tools: {
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
  await write(uiPath, JSON.stringify(ui));
  const registryPath = join(value.root, "PrismatiXUiTypeRegistry.json");
  await write(registryPath, JSON.stringify({
    format: "PrismatiXUiTypeRegistry", schemaRevision: 2,
    controls: [{runtimeType: "Control", properties: [{id: "modulate", valueType: "color",
      writable: true, bindable: true, animatable: true}]}],
  }));
  const invalid = run(["validate", value.project, "--ui-registry", registryPath]);
  assert.equal(invalid.status, 1);
  assert.match(invalid.stderr, /PXSDKSEM1324/u);
  const rejectedBuild = run(["build", value.project, "--artifact-only", "--output",
    join(value.root, "invalid-build"), "--ui-registry", registryPath]);
  assert.equal(rejectedBuild.status, 1);
  assert.match(rejectedBuild.stderr, /PXSDKSEM1324/u);

  ui.nodes[0]!.runtimeProperties.modulate = {type: "color", value: "#2D171DFF"};
  await write(uiPath, JSON.stringify(ui));
  const valid = run(["validate", value.project, "--ui-registry", registryPath]);
  assert.equal(valid.status, 0, valid.stderr);
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
  await write(registry, JSON.stringify({format: "PrismatiXUiTypeRegistry", schemaRevision: 2,
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
      process.stdout.write(JSON.stringify({...request, type, runtimeIrSchemaRevision: 2, uiSchemaRevision: 2,
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

test("ids reports fallback identities and writes stable Story anchors with a backup", async () => {
  const value = await fixture();
  const checkedBefore = run(["ids", value.root, "--check"]);
  assert.equal(checkedBefore.status, 1, checkedBefore.stdout);
  const before = JSON.parse(checkedBefore.stdout) as {dryRun: boolean; fallback: number};
  assert.equal(before.dryRun, true);
  assert.ok(before.fallback > 0);

  const backup = join(value.root, "identity-backup");
  const written = run(["ids", value.root, "--write", "--backup", backup]);
  assert.equal(written.status, 0, written.stderr);
  const report = JSON.parse(written.stdout) as {dryRun: boolean; assigned: number; fallback: number};
  assert.equal(report.dryRun, false);
  assert.ok(report.assigned > 0);
  assert.equal(report.fallback, 0);
  assert.match(await readFile(join(value.root, "Story", "main.pxstory"), "utf8"), /\[id auto\.[0-9a-f]+\]/u);
  assert.equal((await stat(join(backup, "Story", "main.pxstory"))).isFile(), true);

  const checkedAfter = run(["ids", value.root, "--check"]);
  assert.equal(checkedAfter.status, 0, checkedAfter.stderr);
});

test("ids --external moves inline anchors to a sidecar without changing compiled operation ids", async () => {
  const value = await fixture();
  const sourcePath = join(value.root, "Story", "main.pxstory");
  await write(sourcePath, "[id pin.hello]\nHello.\n[id pin.end]\n[end]\n");
  const before = join(value.root, "before-artifact");
  assert.equal(run(["build", value.project, "--artifact-only", "--output", before]).status, 0);
  const converted = run(["ids", value.project, "--external", "--write"]);
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
  assert.equal(run(["ids", value.project, "--external", "--write"]).status, 0);
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
  };
  assert.equal(previewReport.dryRun, true);
  assert.equal(previewReport.changed, 1);
  assert.equal(previewReport.preserved, 2);
  assert.equal(previewReport.added, 1);
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

test("artifact build reports load, compile, copy and fingerprint progress", async () => {
  const value = await fixture();
  const built = run(["build", value.project, "--artifact-only", "--output",
    join(value.root, ".prismatix", "progress-artifact")]);
  assert.equal(built.status, 0, built.stderr);
  const events = built.stderr.split(/\r?\n/u).filter(Boolean).map(line => JSON.parse(line) as {
    code: string; phase?: string; current?: number; total?: number; done?: boolean;
  }).filter(event => event.code === "PXCLI1102");
  for (const phase of ["load", "compile", "copy", "fingerprint-output", "publish"])
    assert.ok(events.some(event => event.phase === phase && event.done), phase);
  const copy = events.find(event => event.phase === "copy" && event.done)!;
  assert.equal(copy.current, copy.total);
});

test("TSX UI authoring lowers to canonical JSON and never enters the Runtime artifact", async () => {
  const value = await fixture();
  const project = JSON.parse(await readFile(value.project, "utf8")) as {
    uiEntryPoints: Record<string, string>;
    uiComponents?: Array<{id: string; name: string; source: string}>;
  };
  project.uiEntryPoints.title = "Content/UI/Title.tsx";
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
  };
  assert.equal(builtProject.uiEntryPoints.title, "Content/UI/Title.pxui");
  await assert.rejects(stat(join(output, "Content/UI/Title.tsx")));
  assert.equal((await stat(join(output, "Content/UI/Panel.pxuicomponent"))).isFile(), true);
  await assert.rejects(stat(join(output, "Content/UI/Panel.jsx")));

  const secondOutput = join(value.root, "tsx-artifact-second");
  const rebuilt = run(["build", value.project, "--artifact-only", "--output", secondOutput]);
  assert.equal(rebuilt.status, 0, rebuilt.stderr);
  assert.equal(await readFile(join(secondOutput, "Content/UI/Title.pxui"), "utf8"),
    await readFile(canonicalPath, "utf8"));
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
  assert.match(packed.stderr, /PXCLI1042/u);
  assert.match(packed.stdout, /"fileRequest":true/);
  assert.match(packed.stdout, /"packed":true/);
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
  await write(player, "fixture");
  await write(join(tools, "PrismatiXUiTypeRegistry.json"), JSON.stringify({
    format: "PrismatiXUiTypeRegistry", schemaRevision: 2,
    controls: [{runtimeType: "Control", properties: [{id: "modulate", valueType: "color",
      writable: true, bindable: true, animatable: true}]}],
  }));
  const packed = run(["pack", value.project, "--packager", mock, "--player", player,
    "--output", join(value.root, "package")]);
  assert.equal(packed.status, 1);
  assert.match(packed.stderr, /PXSDKSEM1324/u);
  assert.doesNotMatch(packed.stderr, /PXCLI1042/u);
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
  await write(player, "fixture");
  const packed = runHuman(["pack", value.project, "--packager", mock,
    "--player", player, "--output", join(value.root, "packaged")]);
  assert.equal(packed.status, 0, packed.stderr.slice(-2000));
  assert.match(packed.stderr, /Packaging archive: 0\/4200/u);
  assert.match(packed.stderr, /Packaging archive: 4200\/4200/u);
  assert.match(packed.stdout, /Packaged playable game/u);
});

test("pack reuses an owned artifact without recompiling edited author sources", async () => {
  const value = await fixture();
  const artifact = join(value.root, ".prismatix", "artifact");
  const built = run(["build", value.project, "--artifact-only", "--output", artifact]);
  assert.equal(built.status, 0, built.stderr);
  await write(join(value.root, "Story/main.pxstory"), "[unknown-command]\n");
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cli-reused-artifact-"));
  const mock = join(tools, "mock-packager.mjs");
  const output = join(value.root, "packaged");
  await write(mock, `
    import {readFile} from "node:fs/promises";
    const request = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
    if (request.projectRoot !== ${JSON.stringify(artifact)} ||
        request.cacheSourceDir !== ${JSON.stringify(output)} ||
        request.outputDir === request.cacheSourceDir ||
        !request.inputs.some(value => value.uri === "Runtime/main.pxir") ||
        !request.inputs.some(value => value.uri === "project.pxproject")) process.exit(42);
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed",
      requestId: request.requestId, reusedArtifact: true}) + "\\n");
  `);
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await write(player, "fixture");
  const packed = run(["pack", value.project, "--artifact", artifact,
    "--packager", mock, "--player", player, "--output", output]);
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
  const built = run(["build", value.project, "--artifact-only", "--output", output]);
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

test("inspect-save cannot accept a JS-hashed envelope without the authoritative native validator", async () => {
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
  // This intentionally incomplete state passed the old hash-only JS validator.
  // Native format/encryption acceptance is covered by the Player native suite.
  const inspected = run(["inspect-save", path, "--runtime", join(root, "absent-runtime")]);
  assert.equal(inspected.status, 1, inspected.stderr);
  assert.match(inspected.stderr, /PXCLI1401/);
  assert.equal(inspected.stdout, "");
});

test("the commercial sample compiles and publishes every release-gated capability", async () => {
  const sample = resolve("../..", "Samples/CommercialVN/project.pxproject");
  const root = await mkdtemp(join(tmpdir(), "prismatix-commercial-sample-"));
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

test("the PrismaFamilyDemo authors every UI route in TSX and builds canonical-only UI", async () => {
  const sample = resolve("../..", "Samples/PrismaFamilyDemo/project.pxproject");
  const root = await mkdtemp(join(tmpdir(), "prismatix-jsx-sample-"));
  const output = join(root, "artifact");
  const authoredProject = JSON.parse(await readFile(sample, "utf8")) as {
    uiEntryPoints: Record<string, string>;
  };
  assert.ok(Object.values(authoredProject.uiEntryPoints).every((path) => path.endsWith(".tsx")));
  const validated = run(["validate", sample]);
  assert.equal(validated.status, 0, validated.stderr);
  const built = run(["build", sample, "--artifact-only", "--output", output]);
  assert.equal(built.status, 0, built.stderr);
  for (const name of ["Title", "HUD", "Gallery", "Backlog", "Save", "Load"])
    assert.equal((await stat(join(output, `Content/UI/${name}.pxui`))).isFile(), true);
  for (const path of [
    "Runtime/Locales/zh-TW/main.pxir", "Runtime/Locales/ja-JP/main.pxir",
    "Assets/Locales/zh-TW/greeting-v2.wav", "Assets/Locales/ja-JP/greeting-v2.wav",
    "Assets/three-cups-ending.png", "Assets/evening-room.png",
    "Content/Migrations/family-v1-to-v2.pxsave-migration",
  ]) assert.equal((await stat(join(output, path))).isFile(), true, `missing ${path}`);
  const published = await readFile(join(output, "project.pxproject"), "utf8");
  assert.doesNotMatch(published, /\.(?:jsx|tsx)/u);
  assert.equal((await readFile(join(output, "Content/UI/HUD.pxui"), "utf8"))
    .includes('"format": "PrismatiXUIScene"'), true);
  const zh = JSON.parse(await readFile(
    join(output, "Runtime/Locales/zh-TW/main.pxir"), "utf8")) as {
      operations: Array<{kind: string; operationId: string; sourceId: string;
        arguments: Record<string, string>}>;
    };
  const ja = JSON.parse(await readFile(
    join(output, "Runtime/Locales/ja-JP/main.pxir"), "utf8")) as typeof zh;
  assert.deepEqual(
    ja.operations.map(({kind, operationId, sourceId}) => ({kind, operationId, sourceId})),
    zh.operations.map(({kind, operationId, sourceId}) => ({kind, operationId, sourceId})),
  );
  assert.ok(zh.operations.filter(({kind}) =>
    kind === "narration" || kind === "dialogue").length >= 521);
  assert.notDeepEqual(
    zh.operations.filter(({kind}) => kind === "background").map(({arguments: value}) => value.asset),
    ja.operations.filter(({kind}) => kind === "background").map(({arguments: value}) => value.asset),
  );
  assert.notDeepEqual(
    zh.operations.filter(({kind}) => kind === "voice").map(({arguments: value}) => value.asset),
    ja.operations.filter(({kind}) => kind === "voice").map(({arguments: value}) => value.asset),
  );
});

test("the PrismaFamily V1 to V2 release journey stays on public authoring inputs", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-family-release-test-"));
  const script = resolve("../..", "Samples/PrismaFamilyDemo/Scripts/release-journey.mjs");
  const cli = resolve("dist/src/cli.js");
  const result = spawnSync(process.execPath,
    [script, "--cli", cli, "--output", root], {encoding: "utf8"});
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  assert.match(result.stdout, /PrismaFamily release journey passed/u);
  assert.equal((await stat(join(root, "author-v1/project.pxproject"))).isFile(), true);
  assert.equal((await stat(join(root, "author-v2/project.pxproject"))).isFile(), true);
});
