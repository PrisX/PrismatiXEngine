import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {existsSync, readFileSync} from "node:fs";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import test from "node:test";

import {ENGINE_HOST_BINDINGS} from "@prismatix/runtime";

import {buildTypeScriptExtensions, remapExtensionStack} from "../src/extension-build.js";
import {createExtension} from "../src/extension-scaffold.js";

const cli = resolve("dist/src/cli.js");

function run(args: readonly string[], json = true) {
  return spawnSync(process.execPath, [cli, ...args, ...(json ? ["--json"] : [])], {encoding: "utf8"});
}

function records(output: string): Record<string, unknown>[] {
  return output.split(/\r?\n/u).filter((line) => line.startsWith("{"))
    .map((line) => JSON.parse(line) as Record<string, unknown>);
}

async function write(path: string, value: string): Promise<void> {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, value, "utf8");
}

/** A beginner project: prismatix.json and one Story file. */
async function project(story = "*start\nHello.\n[end]\n"): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), "prismatix-extension-"));
  await write(join(root, "prismatix.json"), `{
  "name": "Extension Test",
  "id": "extension-test",
  "defaultLocale": "en",
  "resolution": {"width": 1280, "height": 720}
}
`);
  await write(join(root, "Story/main.pxstory"), story);
  return root;
}

test("extension create scaffolds a typed TypeScript extension and registers it", async () => {
  const root = await project();
  try {
    const created = run(["extension", "create", "fx-kit", "--command", "greet,other.shout", "--action", "open",
      "--project", root]);
    assert.equal(created.status, 0, created.stderr);
    const result = JSON.parse(created.stdout) as {created: string[]; updated: string[]; manifest: string; entry: string};
    assert.deepEqual(result.created, ["Content/Extensions/fx-kit/fx-kit.pxextension",
      "Content/Extensions/fx-kit/main.ts", "Content/Extensions/fx-kit/tsconfig.json",
      "Content/Extensions/fx-kit/prismatix-extension.d.ts"]);
    assert.deepEqual(result.updated, ["prismatix.json"]);
    const manifest = JSON.parse(await readFile(join(root, result.manifest), "utf8")) as Record<string, unknown>;
    assert.equal(manifest.language, "typescript");
    assert.equal(manifest.entry, "main.ts");
    assert.deepEqual(manifest.capabilities, ["runtime", "ui"]);
    assert.deepEqual((manifest.commands as {id: string}[]).map((command) => command.id), ["fxKit.greet", "other.shout"]);
    assert.deepEqual((manifest.actions as {id: string}[]).map((action) => action.id), ["fxKit.open"]);
    const entry = await readFile(join(root, result.entry), "utf8");
    assert.match(entry, /import \{createPrismatiXContext, defineAction, defineCommand\} from "@prismatix\/runtime";/u);
    assert.match(entry, /defineCommand\("fxKit\.greet", \(\) => \{/u);
    assert.match(entry, /defineAction\("fxKit\.open", \(_args, context\) => \{/u);
    // The project text keeps its own layout; only the registration is added.
    assert.equal(await readFile(join(root, "prismatix.json"), "utf8"), `{
  "name": "Extension Test",
  "id": "extension-test",
  "defaultLocale": "en",
  "resolution": {"width": 1280, "height": 720},
  "extensions": ["Content/Extensions/fx-kit/fx-kit.pxextension"]
}
`);

    // Never overwrites: a second create fails and leaves every file alone.
    const before = await readFile(join(root, result.entry), "utf8");
    const again = run(["extension", "create", "fx-kit", "--project", root]);
    assert.equal(again.status, 1);
    assert.match(again.stderr, /already exists/u);
    const clash = run(["extension", "create", "fx-kit2", "--dir", "Content/Extensions/fx-kit", "--project", root]);
    assert.equal(clash.status, 1);
    assert.match(clash.stderr, /own folder/u);
    assert.equal(await readFile(join(root, result.entry), "utf8"), before);

    const added = run(["extension", "add-command", "fx-kit", "wait", "--await", "--project", root]);
    assert.equal(added.status, 0, added.stderr);
    assert.match(await readFile(join(root, result.entry), "utf8"),
      /defineCommand\("fxKit\.wait", async \(\) => \{\n {2}ctx\.log\.info\("fxKit\.wait"\);\n {2}await ctx\.time\.wait\(0\);/u);
    assert.match(await readFile(join(root, "Content/Extensions/fx-kit/prismatix-extension.d.ts"), "utf8"),
      /"fxKit\.wait": \{\n[^}]*readonly args: \{\};\n {6}readonly await: true;/u);
    const listed = JSON.parse(run(["extension", "list", "--project", root]).stdout) as {extensions: {id: string; commands: string[]}[]};
    assert.deepEqual(listed.extensions.map((item) => [item.id, item.commands]),
      [["fx-kit", ["fxKit.greet", "other.shout", "fxKit.wait"]]]);
    const types = JSON.parse(run(["extension", "types", "--project", root]).stdout) as {updated: string[]};
    assert.deepEqual(types.updated, [], "add-command already refreshed the declarations");

    const validated = run(["validate", root]);
    assert.equal(validated.status, 0, validated.stderr);
    assert.deepEqual(records(validated.stderr).filter((item) => item.severity !== "information" && item.code !== undefined &&
      String(item.code).startsWith("PXCLIEXT")), [], "the scaffold type-checks cleanly");
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("extension create --lang js writes an Engine-global script and can skip registration", async () => {
  const root = await project();
  try {
    const result = await createExtension(root, "plain", {language: "js", directory: "Content/Extensions", register: false});
    assert.deepEqual(result.created, ["Content/Extensions/plain.pxextension", "Content/Extensions/main.js"]);
    assert.deepEqual(result.updated, []);
    const entry = await readFile(join(root, "Content/Extensions/main.js"), "utf8");
    assert.match(entry, /Engine\.RegisterCommand\("plain\.notify", async \(\{message\}\) => \{/u);
    assert.match(entry, /Engine\.RegisterAction\("plain\.openScreen", \(\{route\}, context\) => \{\n {2}if \(!context\.preview\) Engine\.ShowModal\(route\);/u);
    assert.doesNotMatch(entry, /import /u);
    assert.equal(JSON.parse(await readFile(join(root, "prismatix.json"), "utf8")).extensions, undefined);
    await assert.rejects(createExtension(root, "bad id"), /Extension id/u);
    await assert.rejects(createExtension(root, "ok", {language: "rust"}), /--lang/u);
    await assert.rejects(createExtension(root, "ok", {directory: "../outside"}), /--dir/u);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("build bundles a TypeScript extension into deterministic JavaScript the Player loads", async () => {
  const root = await project("*start\n[fxKit.greet name=\"Aoi\"]\nHello.\n[end]\n");
  try {
    await createExtension(root, "fx-kit", {commands: ["greet"]});
    const directory = join(root, "Content/Extensions/fx-kit");
    const manifestPath = join(directory, "fx-kit.pxextension");
    const manifest = JSON.parse(await readFile(manifestPath, "utf8")) as {commands: {parameters: unknown[]}[]};
    manifest.commands[0]!.parameters = [{name: "name", type: "string", required: true}];
    await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
    await write(join(directory, "lib/format.ts"), "export const greeting = (name: string): string => `Hello, ${name}`;\n");
    await write(join(root, "node_modules/tiny-case/package.json"), JSON.stringify({name: "tiny-case", version: "1.0.0", main: "index.js"}));
    await write(join(root, "node_modules/tiny-case/index.js"), "exports.upper = (value) => String(value).toUpperCase();\n");
    await write(join(directory, "main.ts"), [
      "import {createPrismatiXContext, defineCommand} from \"@prismatix/runtime\";",
      "import {greeting} from \"./lib/format\";",
      "// @ts-expect-error -- an untyped CommonJS package still bundles",
      "import {upper} from \"tiny-case\";",
      "const ctx = createPrismatiXContext();",
      "defineCommand(\"fxKit.greet\", ({name}) => {",
      "  ctx.variables.set(\"greeting\", upper(greeting(name)));",
      "});",
      ""].join("\n"));
    const output = join(root, "artifact");
    const built = run(["build", root, "--artifact-only", "--output", output]);
    assert.equal(built.status, 0, built.stderr);
    const bundle = await readFile(join(output, "Content/Extensions/fx-kit/main.js"), "utf8");
    assert.match(bundle, /RegisterCommand/u, "the @prismatix/runtime facade is inside the bundle");
    assert.match(bundle, /Hello, \$\{name\}/u);
    assert.match(bundle, /toUpperCase/u);
    assert.doesNotMatch(bundle, /^\s*import\s/mu, "no import is left for the Player to resolve");
    assert.doesNotMatch(bundle, /sourceMappingURL/u);
    assert.doesNotMatch(bundle, new RegExp(root.replaceAll("\\", "\\\\").replace(/[.*+?^${}()|[\]]/gu, "\\$&"), "u"),
      "no machine path leaks into the bundle");
    const runtimeManifest = JSON.parse(await readFile(join(output, "Content/Extensions/fx-kit/fx-kit.pxextension"), "utf8")) as Record<string, unknown>;
    assert.equal(runtimeManifest.entry, "main.js");
    assert.equal(runtimeManifest.language, "javascript");
    assert.equal(existsSync(join(output, "Content/Extensions/fx-kit/main.ts")), false);
    assert.equal(existsSync(join(output, "Content/Extensions/fx-kit/lib/format.ts")), false);

    // The cache returns the same bytes; a cold build produces them again.
    const cached = await buildTypeScriptExtensions(root, {"Content/Extensions/fx-kit/fx-kit.pxextension": runtimeManifestSource(root)});
    const cold = await buildTypeScriptExtensions(root, {"Content/Extensions/fx-kit/fx-kit.pxextension": runtimeManifestSource(root)}, {cache: false});
    assert.equal(cached.builds["Content/Extensions/fx-kit/fx-kit.pxextension"]?.code, bundle);
    assert.equal(cold.builds["Content/Extensions/fx-kit/fx-kit.pxextension"]?.code, bundle);
    assert.deepEqual([...cold.sources.keys()].sort(), ["Content/Extensions/fx-kit/lib/format.ts",
      "Content/Extensions/fx-kit/main.ts", "node_modules/tiny-case/index.js"]);

    // A changed import invalidates the cache.
    await write(join(directory, "lib/format.ts"), "export const greeting = (name: string): string => `Hi, ${name}`;\n");
    const changed = await buildTypeScriptExtensions(root, {"Content/Extensions/fx-kit/fx-kit.pxextension": runtimeManifestSource(root)});
    assert.match(changed.builds["Content/Extensions/fx-kit/fx-kit.pxextension"]!.code, /Hi, \$\{name\}/u);

    // Player positions map back to the TypeScript line.
    const line = changed.builds["Content/Extensions/fx-kit/fx-kit.pxextension"]!.code.split("\n")
      .findIndex((text) => text.includes("Hi, ${name}")) + 1;
    assert.match(remapExtensionStack(`at greeting (Content/Extensions/fx-kit/main.js:${line}:30)`, root,
      ["Content/Extensions/fx-kit/fx-kit.pxextension"]), /at greeting \(Content\/Extensions\/fx-kit\/lib\/format\.ts:1:\d+\)/u);
  } finally { await rm(root, {recursive: true, force: true}); }
});

function runtimeManifestSource(root: string): unknown {
  return JSON.parse(readFileSync(join(root, "Content/Extensions/fx-kit/fx-kit.pxextension"), "utf8")) as unknown;
}

test("validate reports TypeScript errors as located warnings and sandbox misuse", async () => {
  const root = await project();
  try {
    await createExtension(root, "fx-kit", {commands: ["greet"]});
    await write(join(root, "Content/Extensions/fx-kit/main.ts"), [
      "import {defineCommand} from \"@prismatix/runtime\";",
      "defineCommand(\"fxKit.greet\", ({name}) => {",
      "  const count: number = \"three\";",
      "  console.log(name, count, Date.now(), Math.random());",
      "});",
      "defineCommand(\"fxKit.undeclared\", () => {});",
      ""].join("\n"));
    const validated = run(["validate", root]);
    assert.equal(validated.status, 0, "type errors are warnings: the build still runs");
    const warnings = records(validated.stderr).filter((item) => String(item.code).startsWith("PXCLIEXT"));
    const at = (code: string) => warnings.filter((item) => item.code === code).map((item) => {
      const span = item.span as {path: string; start: {line: number; column: number}};
      return `${span.path}:${span.start.line}:${span.start.column}`;
    });
    assert.deepEqual(at("PXCLIEXT2001"), [
      "Content/Extensions/fx-kit/main.ts:2:32",
      "Content/Extensions/fx-kit/main.ts:3:9",
      "Content/Extensions/fx-kit/main.ts:6:15",
    ], JSON.stringify(warnings, null, 2));
    assert.deepEqual(at("PXCLIEXT2002"), [
      "Content/Extensions/fx-kit/main.ts:4:28",
      "Content/Extensions/fx-kit/main.ts:4:40",
    ]);
    assert.ok(warnings.every((item) => item.severity === "warning"));
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("a Node.js import or a syntax error stops the build with the source position", async () => {
  const root = await project();
  try {
    await createExtension(root, "fx-kit", {commands: ["greet"]});
    const entry = join(root, "Content/Extensions/fx-kit/main.ts");
    await write(entry, "import {readFileSync} from \"node:fs\";\nreadFileSync(\"x\");\n");
    const nodeImport = run(["validate", root]);
    assert.equal(nodeImport.status, 1);
    const error = records(nodeImport.stderr).find((item) => item.code === "PXCLIEXT1001");
    assert.match(String(error?.message), /"node:fs" is a Node\.js module/u);
    assert.equal((error?.span as {start: {line: number}} | undefined)?.start.line, 1);
    await write(entry, "const broken = ;\n");
    const syntax = run(["validate", root]);
    assert.equal(syntax.status, 1);
    assert.equal(records(syntax.stderr).find((item) => item.code === "PXCLIEXT1001")?.path,
      "Content/Extensions/fx-kit/main.ts");
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("every Engine binding of the native JavaScriptHost is typed by @prismatix/runtime", (context) => {
  const source = fileURLToPath(new URL("../../../../PrismatiXEngine/Engine/Script/JavaScriptHost.cpp", import.meta.url));
  if (!existsSync(source)) { context.skip("engine sources are not beside this package"); return; }
  const text = readFileSync(source, "utf8");
  const sandbox = text.slice(text.indexOf("void BindEngine()"), text.indexOf("delete Engine.__runtimeCall"));
  const native = new Set([
    ...[...sandbox.matchAll(/JS_SetPropertyStr\(\s*context, engine, "(\w+)"/gu)].map((match) => match[1]!),
    ...[...sandbox.matchAll(/^\s*(\w+): \{ value:/gmu)].map((match) => match[1]!),
  ].filter((name) => name !== "__runtimeCall"));
  assert.ok(native.size > 100, "the binding list was not found in JavaScriptHost.cpp");
  const typed: readonly string[] = ENGINE_HOST_BINDINGS;
  assert.deepEqual([...native].filter((name) => !typed.includes(name)), [],
    "native Engine bindings missing from @prismatix/runtime");
  assert.deepEqual(typed.filter((name) => !native.has(name)), [],
    "@prismatix/runtime declares Engine bindings the native host does not install");
});

test("validate warns when code uses a host capability the manifest does not declare", async () => {
  const root = await project();
  try {
    await createExtension(root, "fx-kit", {commands: ["greet"]});
    await write(join(root, "Content/Extensions/fx-kit/main.ts"), [
      "import {createPrismatiXContext, defineAction, defineCommand} from \"@prismatix/runtime\";",
      "const ctx = createPrismatiXContext();",
      "const saved = ctx.state.define(\"fx.saved\", {initial: 0});",
      "defineCommand(\"fxKit.greet\", () => { ctx.audio.playSE(\"a.ogg\"); Engine.Save(1); saved.set(1); });",
      ""].join("\n"));
    const validated = run(["validate", root]);
    const findings = records(validated.stderr).filter((item) => item.code === "PXCLIEXT2004")
      .map((item) => String(item.message).split(" needs ")[0] + ":" + String(item.message).match(/"(\w+)"/u)?.[1]);
    assert.deepEqual(findings, ["ctx.state:persistence", "ctx.audio:audio", "Engine.Save:persistence"],
      validated.stderr);
  } finally { await rm(root, {recursive: true, force: true}); }
});
