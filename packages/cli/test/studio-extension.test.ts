// Studio extensions (docs/studio-extensions.md) through the CLI: create with
// a Studio side, build it, pack it, inspect and install the package, and a
// game build that leaves the Studio side out.

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {existsSync} from "node:fs";
import {mkdir, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import test from "node:test";

import {readExtensionPackage} from "@prismatix/authoring-sdk";

const cli = resolve("dist/src/cli.js");

function run(args: readonly string[]) {
  return spawnSync(process.execPath, [cli, ...args], {encoding: "utf8",
    env: {...process.env, PRISMATIX_EXTENSION_REGISTRY: "none", CI: "1"}});
}

function json(args: readonly string[]): Record<string, unknown> {
  const result = run([...args, "--json"]);
  assert.equal(result.status, 0, result.stderr);
  const line = result.stdout.split(/\r?\n/u).filter((item) => item.startsWith("{")).at(-1)!;
  return JSON.parse(line) as Record<string, unknown>;
}

async function write(path: string, value: string): Promise<void> {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, value, "utf8");
}

async function project(name: string, story = "*start\nHello.\n[end]\n"): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), `prismatix-studio-${name}-`));
  await write(join(root, "prismatix.json"), `{\n  "name": "Studio",\n  "id": "studio-${name}",\n  "defaultLocale": "en",\n` +
    `  "resolution": {"width": 1280, "height": 720}\n}\n`);
  await write(join(root, "Story/main.pxstory"), story);
  return root;
}

async function readJson(path: string): Promise<Record<string, unknown>> {
  return JSON.parse(await readFile(path, "utf8")) as Record<string, unknown>;
}

test("extension create --side scaffolds a Studio side, list shows sides and build bundles it", async () => {
  const root = await project("create");
  try {
    const both = json(["extension", "create", "live2d", "--side", "both", "--project", root]) as {created: string[]; manifest: string};
    assert.deepEqual(both.created, ["Content/Extensions/live2d/live2d.pxextension", "Content/Extensions/live2d/main.ts",
      "Content/Extensions/live2d/tsconfig.json", "Content/Extensions/live2d/prismatix-extension.d.ts",
      "Content/Extensions/live2d/studio.ts", "Content/Extensions/live2d/tsconfig.studio.json",
      "Content/Extensions/live2d/dist/studio.js"]);
    const manifest = await readJson(join(root, both.manifest));
    assert.equal(manifest.schemaRevision, 2);
    assert.equal(manifest.entry, "main.ts");
    assert.deepEqual(manifest.studio, {entry: "dist/studio.js", requiredStudioVersion: ">=0.1.0", permissions: ["project.read"],
      contributes: {inspectors: [{assetType: "text", displayName: "live2d preview"}]}});
    assert.match(await readFile(join(root, "Content/Extensions/live2d/tsconfig.json"), "utf8"), /"exclude": \["studio.ts"\]/u);
    const bundle = await readFile(join(root, "Content/Extensions/live2d/dist/studio.js"), "utf8");
    assert.match(bundle, /export\s*\{\s*activate\s*\}/u);
    assert.doesNotMatch(bundle, /: any/u, "the bundle is JavaScript");

    const studioOnly = json(["extension", "create", "tools", "--side", "studio", "--lang", "js", "--project", root]) as
      {created: string[]; entry: string; manifest: string};
    assert.deepEqual(studioOnly.created, ["Content/Extensions/tools/tools.pxextension", "Content/Extensions/tools/studio.js"]);
    assert.equal(studioOnly.entry, "Content/Extensions/tools/studio.js");
    const tools = await readJson(join(root, studioOnly.manifest));
    assert.deepEqual(Object.keys(tools).sort(), ["format", "id", "requiredEngineVersion", "schemaRevision", "studio", "version"]);
    assert.equal((tools.studio as {entry: string}).entry, "studio.js");
    assert.equal(run(["extension", "create", "bad", "--side", "studio", "--command", "x", "--project", root]).status, 1);
    assert.equal(run(["extension", "create", "bad", "--side", "editor", "--project", root]).status, 1);
    const addCommand = run(["extension", "add-command", "tools", "greet", "--project", root]);
    assert.equal(addCommand.status, 1);
    assert.match(addCommand.stderr, /only a Studio side/u);

    const listed = json(["extension", "list", "--project", root]) as {extensions: {id: string; sides: string[]; entry?: string}[]};
    assert.deepEqual(listed.extensions.map((item) => [item.id, item.sides, item.entry]),
      [["live2d", ["game", "studio"], "main.ts"], ["tools", ["studio"], undefined]]);
    const text = run(["extension", "list", "--project", root]);
    assert.match(text.stdout, /live2d \(TypeScript \+ Studio\)/u);
    assert.match(text.stdout, /tools \(Studio only\)/u);

    // build rewrites the bundle from studio.ts; an unchanged source writes nothing.
    await write(join(root, "Content/Extensions/live2d/studio.ts"),
      "import {label} from \"./lib/label\";\nexport function activate(studio: any): void { studio.log?.(label); }\n");
    await write(join(root, "Content/Extensions/live2d/lib/label.ts"), "export const label: string = \"from a module\";\n");
    assert.deepEqual(json(["extension", "build", "--project", root]),
      {updated: ["Content/Extensions/live2d/dist/studio.js"], extensions: ["Content/Extensions/live2d/live2d.pxextension"]});
    assert.match(await readFile(join(root, "Content/Extensions/live2d/dist/studio.js"), "utf8"), /from a module/u);
    assert.deepEqual((json(["extension", "build", "live2d", "--project", root]) as {updated: string[]}).updated, []);
    await write(join(root, "Content/Extensions/live2d/studio.ts"), "import {readFileSync} from \"node:fs\";\nexport const x = readFileSync;\n");
    const failed = run(["extension", "build", "--project", root]);
    assert.equal(failed.status, 1);
    assert.match(failed.stderr, /PXCLIEXT1001.*Node\.js module/u);
    assert.equal(run(["extension", "build", "tools", "--project", root]).status, 1, "a JavaScript Studio side has nothing to build");

    // validate and build of the game ignore the Studio side.
    assert.equal(run(["validate", root]).status, 0);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("pack, inspect and add a Studio-only and a two-sided package; the game leaves the Studio side out", async () => {
  const author = await project("author");
  const consumer = await project("consumer", "*start\n[live2d.notify message=\"hi\"]\nHello.\n[end]\n");
  try {
    json(["extension", "create", "live2d", "--side", "both", "--lang", "js", "--project", author]);
    json(["extension", "create", "tools", "--side", "studio", "--lang", "js", "--project", author]);
    // A Studio side with contributions, permissions and a TypeScript source next to its entry.
    const toolsPath = join(author, "Content/Extensions/tools/tools.pxextension");
    const tools = await readJson(toolsPath);
    await writeFile(toolsPath, `${JSON.stringify({...tools, studio: {entry: "studio.js", requiredStudioVersion: ">=0.1.0",
      permissions: ["project.read", "project.write"], contributes: {
        assetTypes: [{id: "live2d.model", displayName: "Live2D model", patterns: ["*.model3.json"]}],
        inspectors: [{assetType: "live2d.model"}], parameterOptions: [{id: "motions", displayName: "Motions"}],
        importers: [{id: "model", displayName: "Live2D model", pick: "directory"}]}}}, null, 2)}\n`);
    await write(join(author, "Content/Extensions/tools/studio.ts"), "export function activate(studio: any): void { studio.ready = 1; }\n");
    const output = join(author, "out");
    const packedTools = json(["extension", "pack", "tools", "--owner", "owen", "--output", output, "--project", author]) as
      {file: string; files: string[]};
    assert.deepEqual(packedTools.files, ["README.md", "dist/studio.js", "pxext.json", "tools.pxextension"]);
    const toolsPackage = readExtensionPackage(new Uint8Array(await readFile(packedTools.file)));
    assert.equal(toolsPackage.valid, true, JSON.stringify(toolsPackage.diagnostics));
    assert.equal(toolsPackage.manifest!.studio!.entry, "dist/studio.js");
    assert.match(new TextDecoder().decode(toolsPackage.files.get("dist/studio.js")), /studio\.ready = 1/u,
      "pack bundles the TypeScript source next to the entry");
    const packedBoth = json(["extension", "pack", "live2d", "--owner", "owen", "--output", output, "--project", author]) as
      {file: string; files: string[]};
    assert.deepEqual(packedBoth.files, ["README.md", "dist/main.js", "dist/studio.js", "live2d.pxextension", "pxext.json",
      "types/prismatix-extension.d.ts"]);
    const bothManifest = readExtensionPackage(new Uint8Array(await readFile(packedBoth.file))).manifest!;
    assert.equal(bothManifest.entry, "dist/main.js");
    assert.equal(bothManifest.studio!.entry, "dist/studio.js");

    // inspect --json, outside and inside a project.
    const outside = json(["extension", "inspect", packedTools.file]);
    assert.deepEqual(outside.sides, ["studio"]);
    assert.deepEqual(outside.capabilities, []);
    const studioSummary = {requiredStudioVersion: ">=0.1.0", permissions: ["project.read", "project.write"],
      contributes: {assetTypes: [{id: "live2d.model", displayName: "Live2D model"}], inspectors: [{assetType: "live2d.model"}],
        parameterOptions: [{id: "motions", displayName: "Motions"}], importers: [{id: "model", displayName: "Live2D model"}]}};
    assert.deepEqual(outside.studio, studioSummary);
    const inside = json(["extension", "inspect", packedTools.file, "--project", consumer]);
    assert.deepEqual(inside.sides, ["studio"]);
    assert.deepEqual(inside.studio, studioSummary);
    assert.deepEqual(inside.commands, []);
    assert.deepEqual(json(["extension", "inspect", packedBoth.file, "--project", consumer]).sides, ["game", "studio"]);
    const described = run(["extension", "inspect", packedTools.file, "--project", consumer]);
    assert.equal(described.status, 0, described.stderr);
    assert.match(described.stdout, /adds nothing to the game/u);
    assert.match(described.stdout, /project\.write: create and overwrite files inside the project/u);
    assert.match(described.stdout, /Import items in Assets: Live2D model \(model\)/u);
    assert.match(described.stdout, /Asset types: Live2D model \(live2d\.model\)/u);
    assert.match(described.stdout, /Parameter options: Motions \(motions\)/u);

    // Install both; list reports their sides.
    const added = run(["extension", "add", packedTools.file, "--yes", "--project", consumer]);
    assert.equal(added.status, 0, added.stderr);
    assert.match(added.stdout, /It adds nothing to the game/u);
    assert.equal(run(["extension", "add", packedBoth.file, "--yes", "--project", consumer]).status, 0);
    const listed = json(["extension", "list", "--project", consumer]) as {extensions: {id: string; sides: string[];
      package?: {lock: string}}[]};
    assert.deepEqual(listed.extensions.map((item) => [item.id, item.sides, item.package?.lock]),
      [["tools", ["studio"], "ok"], ["live2d", ["game", "studio"], "ok"]]);
    assert.equal(run(["extension", "verify", "--project", consumer]).status, 0);

    // The game: built, with the two-sided extension's game side only.
    const artifact = join(consumer, "artifact");
    const built = run(["build", consumer, "--artifact-only", "--output", artifact]);
    assert.equal(built.status, 0, built.stderr);
    const gameProject = await readJson(join(artifact, "project.pxproject"));
    assert.deepEqual(gameProject.extensions, ["Extensions/owen/live2d/live2d.pxextension"]);
    assert.equal(existsSync(join(artifact, "Extensions/owen/tools")), false, "a Studio-only extension is absent");
    assert.equal(existsSync(join(artifact, "Extensions/owen/live2d/dist/studio.js")), false, "the Studio entry is absent");
    assert.equal(existsSync(join(artifact, "Extensions/owen/live2d/dist/main.js")), true);
    const shipped = await readJson(join(artifact, "Extensions/owen/live2d/live2d.pxextension"));
    assert.equal(shipped.schemaRevision, 1);
    assert.equal(shipped.studio, undefined);
    assert.match(await readFile(join(artifact, "Runtime/main.pxir"), "utf8"), /live2d\.notify/u);
    assert.doesNotMatch(await readFile(join(artifact, "Runtime/main.pxir"), "utf8"), /owen\/tools|tools\.pxextension/u);

    // Removing the Studio-only package leaves the game as it was.
    assert.equal(run(["extension", "remove", "owen/tools", "--project", consumer]).status, 0);
    assert.equal(run(["validate", consumer]).status, 0);
  } finally {
    await rm(author, {recursive: true, force: true});
    await rm(consumer, {recursive: true, force: true});
  }
});
