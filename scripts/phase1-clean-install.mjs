#!/usr/bin/env node

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {constants} from "node:fs";
import {
  access, appendFile, cp, lstat, mkdir, mkdtemp, readFile, readdir,
  realpath, rm, writeFile,
} from "node:fs/promises";
import {tmpdir} from "node:os";
import {
  basename, delimiter, dirname, extname, isAbsolute, join, resolve,
} from "node:path";
import {fileURLToPath} from "node:url";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const packageRoots = [
  join(repositoryRoot, "packages", "authoring-sdk"),
  join(repositoryRoot, "runtime-sdk"),
  join(repositoryRoot, "packages", "cli"),
];
const authorEdit = "The clean-install edit reached Preview and the packaged game.";
const invalidStoryCommand = "[phase1-invalid-command]";
const defaultTimeoutMs = 60_000;

function parseArguments(values) {
  const result = {mock: false, keep: false, runtime: process.env.PRISMATIX_PHASE1_RUNTIME};
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (value === "--mock") result.mock = true;
    else if (value === "--keep") result.keep = true;
    else if (value === "--runtime") result.runtime = values[++index];
    else if (value.startsWith("--runtime=")) result.runtime = value.slice("--runtime=".length);
    else throw new Error(`Unknown argument: ${value}`);
  }
  if (!result.mock && (result.runtime === undefined || result.runtime.length === 0)) {
    throw new Error(
      "The real Phase 1 gate requires --runtime <installed-prefix> or " +
      "PRISMATIX_PHASE1_RUNTIME. Use --mock only for the fast packaging/workflow gate.",
    );
  }
  return result;
}

function log(step, details = "") {
  process.stdout.write(`[phase1] ${step}${details.length > 0 ? `: ${details}` : ""}\n`);
}

function normalized(path) {
  return resolve(path).replaceAll("\\", "/").toLowerCase();
}

function assertOutsideRepository(path, label) {
  const root = `${normalized(repositoryRoot)}/`;
  const candidate = normalized(path);
  assert.ok(candidate !== normalized(repositoryRoot) && !candidate.startsWith(root),
    `${label} must be outside the repository: ${path}`);
}

async function exists(path) {
  try { await access(path, constants.F_OK); return true; }
  catch { return false; }
}

function commandText(command, args) {
  return [command, ...args].map((value) => /\s/u.test(value) ? JSON.stringify(value) : value).join(" ");
}

async function run(command, args, options = {}) {
  const timeoutMs = options.timeoutMs ?? defaultTimeoutMs;
  return new Promise((accept, reject) => {
    const child = spawn(command, args, {
      cwd: options.cwd,
      env: options.env,
      shell: false,
      windowsHide: true,
      stdio: [options.input === undefined ? "ignore" : "pipe", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (value) => { stdout += value; });
    child.stderr.on("data", (value) => { stderr += value; });
    child.on("error", reject);
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`Timed out after ${timeoutMs} ms: ${commandText(command, args)}`));
    }, timeoutMs);
    child.on("close", (code, signal) => {
      clearTimeout(timer);
      accept({code: code ?? 1, signal, stdout, stderr});
    });
    if (options.input !== undefined) child.stdin.end(options.input);
  });
}

function requireSuccess(result, label) {
  if (result.code !== 0) {
    throw new Error(`${label} failed (${result.code})\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  }
  return result;
}

function parseJsonLines(text) {
  const values = [];
  for (const line of text.split(/\r?\n/u)) {
    if (line.trim().length === 0) continue;
    try { values.push(JSON.parse(line)); }
    catch { /* Native tools may mix human-readable logs with NDJSON events. */ }
  }
  return values;
}

function lastObject(text, predicate) {
  return parseJsonLines(text).reverse().find((value) =>
    value !== null && typeof value === "object" && !Array.isArray(value) && predicate(value));
}

function npmInvocation() {
  const candidate = process.env.npm_execpath;
  if (candidate !== undefined && candidate.length > 0 && extname(candidate).toLowerCase() !== ".ps1") {
    return {command: process.execPath, prefix: [candidate]};
  }
  const nodeRoot = dirname(process.execPath);
  const candidates = process.platform === "win32"
    ? [join(nodeRoot, "node_modules", "npm", "bin", "npm-cli.js"),
       join(nodeRoot, "..", "node_modules", "npm", "bin", "npm-cli.js")]
    : [join(nodeRoot, "..", "lib", "node_modules", "npm", "bin", "npm-cli.js")];
  return {command: process.execPath, prefix: [candidates[0]]};
}

async function npm(args, options = {}) {
  const invocation = npmInvocation();
  return run(invocation.command, [...invocation.prefix, ...args], options);
}

async function listFiles(root, directory = root) {
  const output = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) output.push(...await listFiles(root, path));
    else output.push(path);
  }
  return output;
}

async function assertSelfContainedTree(root, forbiddenRoots, label) {
  const rootRealPath = await realpath(root);
  for (const path of await listFiles(root)) {
    const info = await lstat(path);
    assert.ok(!info.isSymbolicLink(), `${label} contains a symbolic link: ${path}`);
    const resolved = await realpath(path);
    assert.ok(normalized(resolved).startsWith(`${normalized(rootRealPath)}/`),
      `${label} escapes its root: ${path} -> ${resolved}`);
    if (info.size > 2 * 1024 * 1024) continue;
    const extension = extname(path).toLowerCase();
    if (!["", ".cjs", ".cmd", ".d.ts", ".js", ".json", ".md", ".mjs", ".pxindex",
          ".pxir", ".pxmap", ".pxproject", ".pxstory", ".pxui", ".sh", ".txt"].includes(extension)) continue;
    const text = await readFile(path, "utf8");
    const portable = text.replaceAll("\\", "/").toLowerCase();
    for (const forbidden of forbiddenRoots) {
      assert.ok(!portable.includes(normalized(forbidden)),
        `${label} text embeds forbidden host path ${forbidden}: ${path}`);
    }
    assert.ok(!portable.includes("vcpkg_installed") && !portable.includes("vcpkg\\installed"),
      `${label} text embeds a vcpkg installation path: ${path}`);
  }
}

async function packWorkspacePackages(packDirectory, environment) {
  await mkdir(packDirectory, {recursive: true});
  const tarballs = [];
  for (const packageRoot of packageRoots) {
    const packed = requireSuccess(await npm([
      "pack", "--json", "--ignore-scripts", "--pack-destination", packDirectory,
    ], {cwd: packageRoot, env: environment}), `npm pack ${packageRoot}`);
    const report = JSON.parse(packed.stdout);
    assert.ok(Array.isArray(report) && typeof report[0]?.filename === "string",
      `npm pack returned an unexpected report for ${packageRoot}`);
    const tarball = join(packDirectory, report[0].filename);
    assert.ok(await exists(tarball), `npm pack did not create ${tarball}`);
    tarballs.push(tarball);
  }
  return tarballs;
}

function cleanToolEnvironment(runtimeRoot, extra = {}) {
  const environment = {...process.env, ...extra};
  for (const name of Object.keys(environment)) {
    if (name.toLowerCase().startsWith("npm_")) delete environment[name];
    else if (typeof environment[name] === "string" &&
             environment[name].replaceAll("\\", "/").toLowerCase().includes(normalized(repositoryRoot)))
      delete environment[name];
  }
  for (const name of [
    "CMAKE_PREFIX_PATH", "CPATH", "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
    "INIT_CWD", "LD_LIBRARY_PATH", "LIBRARY_PATH", "NODE_PATH", "OLDPWD", "PWD",
    "PRISMATIX_PACKAGER", "PRISMATIX_PLAYER", "PRISMATIX_PREVIEW_HOST",
    "PRISMATIX_RUNTIME_DIR", "VCPKG_INSTALLATION_ROOT", "VCPKG_ROOT",
  ]) delete environment[name];

  const systemPaths = [];
  const add = (value) => {
    if (value !== undefined && value.length > 0 && !systemPaths.some((item) => normalized(item) === normalized(value)))
      systemPaths.push(value);
  };
  add(dirname(process.execPath));
  if (process.platform === "win32") {
    const windows = environment.SystemRoot ?? environment.WINDIR;
    add(windows);
    if (windows !== undefined) {
      add(join(windows, "System32"));
      add(join(windows, "System32", "Wbem"));
    }
  } else {
    add("/usr/local/bin"); add("/usr/bin"); add("/bin");
  }
  if (runtimeRoot !== undefined) add(join(runtimeRoot, "bin"));
  environment.PATH = systemPaths.join(delimiter);
  return environment;
}

async function createMockRuntime(root) {
  const bin = join(root, "bin");
  const manifestDirectory = join(root, "share", "PrismatiX");
  await mkdir(bin, {recursive: true});
  await mkdir(manifestDirectory, {recursive: true});
  const previewName = "mock-preview-host.mjs";
  const packagerName = "mock-packager.mjs";
  const playerName = "mock-player.mjs";
  const previewScript = join(bin, previewName);
  const packagerScript = join(bin, packagerName);

  await writeFile(previewScript, `
import {appendFileSync} from "node:fs";
import readline from "node:readline";
const log = process.env.PRISMATIX_PHASE1_PREVIEW_LOG;
const write = (value) => process.stdout.write(JSON.stringify(value) + "\\n");
const response = (request, type) => ({
  protocol: request.protocol, schemaRevision: request.schemaRevision,
  protocolVersion: request.protocolVersion, type,
  sessionId: request.sessionId, requestId: request.requestId,
  documentId: request.documentId, revision: request.revision,
});
const interface_ = readline.createInterface({input: process.stdin, crlfDelay: Infinity});
interface_.on("line", (line) => {
  const request = JSON.parse(line);
  if (log) appendFileSync(log, JSON.stringify(request) + "\\n");
  if (request.type === "hello") write({...response(request, "ready"), runtimeIrSchemaRevision: 2, uiSchemaRevision: 2, renderMode: "mock"});
  else if (request.type === "applyRuntimeIr" || request.type === "patchRuntimeIr") write({
    ...response(request, "runtimeIrApplied"), documentId: request.documentId,
    appliedRevision: request.committedRevision, operationCount: 2,
    applyMode: request.type === "patchRuntimeIr" ? "inPlaceRuntimeIrPatch" : "fullRuntimeRestart",
    renderMode: "mock",
  });
  else if (request.type === "play") write({...response(request, "playAccepted"), runtimeState: "running"});
  else if (request.type === "shutdown") {
    write(response(request, "shutdownAccepted"));
    interface_.close();
  } else write(response(request, request.type + "Accepted"));
});
`, "utf8");

  await writeFile(packagerScript, `
import {chmod, cp, mkdir, readFile, writeFile} from "node:fs/promises";
import {basename, join} from "node:path";
const at = process.argv.indexOf("--request");
if (at < 0 || !process.argv[at + 1]) throw new Error("--request is required");
const request = JSON.parse(await readFile(process.argv[at + 1], "utf8"));
await mkdir(request.outputDir, {recursive: true});
await cp(request.projectRoot, join(request.outputDir, "MockContent"), {recursive: true});
const player = join(request.outputDir, basename(request.playerExecutable));
await cp(request.playerExecutable, player);
if (process.platform !== "win32") await chmod(player, 0o755);
await writeFile(join(request.outputDir, "mock-package.json"), JSON.stringify({
  format: "PrismatiXMockPackage", gameId: request.gameId,
  startScript: request.startScript, sourceMap: request.sourceMap,
  inputs: request.inputs,
}) + "\\n", "utf8");
process.stdout.write(JSON.stringify({event: "completed", outputDir: request.outputDir}) + "\\n");
`, "utf8");

  await writeFile(join(bin, playerName), `
import {writeFile} from "node:fs/promises";
import {join} from "node:path";
await writeFile(join(process.cwd(), "phase1-artifact-launched.json"),
  JSON.stringify({launched: true}) + "\\n", "utf8");
`, "utf8");
  await writeFile(join(manifestDirectory, "runtime-manifest.json"), `${JSON.stringify({
    format: "PrismatiXRuntimeManifest", schemaRevision: 1, version: "0.2.0-mock",
    target: `${process.platform}-${process.arch}`, binDir: "../../bin",
    tools: {previewHost: previewName, packager: packagerName, player: playerName},
  }, null, 2)}\n`, "utf8");
  return root;
}

async function copyRuntimeBundle(source, destination) {
  const root = resolve(source);
  assert.ok(await exists(root), `Runtime bundle does not exist: ${root}`);
  await cp(root, destination, {recursive: true, dereference: true, force: false});
  return destination;
}

async function launchArtifact(player, output, environment, mock) {
  assert.ok(isAbsolute(player), `Pack did not report an absolute Player path: ${player}`);
  assertOutsideRepository(player, "Packaged Player");
  assert.ok(await exists(player), `Packaged Player is missing: ${player}`);
  if (mock) {
    const script = [".js", ".mjs", ".cjs"].includes(extname(player).toLowerCase());
    const result = requireSuccess(await run(script ? process.execPath : player,
      script ? [player] : [], {cwd: output, env: environment}),
      "mock packaged Player");
    const sentinelPath = join(output, "phase1-artifact-launched.json");
    assert.ok(await exists(sentinelPath), "The mock packaged Player did not create its launch sentinel");
    const sentinel = JSON.parse(await readFile(sentinelPath, "utf8"));
    assert.equal(sentinel.launched, true);
    return result;
  }

  const settleMs = Number(process.env.PRISMATIX_PHASE1_LAUNCH_MS ?? "2500");
  assert.ok(Number.isFinite(settleMs) && settleMs >= 500 && settleMs <= 30_000,
    "PRISMATIX_PHASE1_LAUNCH_MS must be between 500 and 30000");
  await new Promise((accept, reject) => {
    const child = spawn(player, [], {cwd: output, env: environment, shell: false, windowsHide: true});
    let stdout = ""; let stderr = ""; let smokeWindowPassed = false;
    let terminationTimer;
    child.stdout?.setEncoding("utf8"); child.stderr?.setEncoding("utf8");
    child.stdout?.on("data", (value) => { stdout += value; });
    child.stderr?.on("data", (value) => { stderr += value; });
    child.on("error", reject);
    child.on("exit", (code, signal) => {
      if (terminationTimer !== undefined) clearTimeout(terminationTimer);
      if (!smokeWindowPassed) reject(new Error(
        `Packaged Player exited before the ${settleMs} ms playable smoke window ` +
        `(code=${code}, signal=${signal})\nstdout:\n${stdout}\nstderr:\n${stderr}`));
      else accept();
    });
    setTimeout(() => {
      smokeWindowPassed = true;
      child.kill();
      terminationTimer = setTimeout(() => reject(new Error(
        `Packaged Player did not exit after the playable smoke window\nstdout:\n${stdout}\nstderr:\n${stderr}`)),
      5_000);
    }, settleMs);
  });
}

async function main() {
  const configuration = parseArguments(process.argv.slice(2));
  const temporaryRoot = await mkdtemp(join(tmpdir(), "prismatix-phase1-clean-"));
  assertOutsideRepository(temporaryRoot, "Acceptance workspace");
  const packDirectory = join(temporaryRoot, "tarballs");
  const installRoot = join(temporaryRoot, "author-machine");
  const projectRoot = join(temporaryRoot, "first-game");
  const explicitPackage = join(temporaryRoot, "portable-game");
  const copiedRuntime = join(temporaryRoot, "runtime-bundle");
  const previewLog = join(temporaryRoot, "preview-requests.ndjson");

  try {
    log("workspace", temporaryRoot);
    const packagingEnvironment = {...process.env};
    delete packagingEnvironment.NODE_PATH;
    delete packagingEnvironment.INIT_CWD;
    const tarballs = await packWorkspacePackages(packDirectory, packagingEnvironment);
    log("packed npm artifacts", tarballs.map((path) => basename(path)).join(", "));

    await mkdir(installRoot, {recursive: true});
    const dependencies = Object.fromEntries(tarballs.map((path) => {
      const filename = basename(path);
      const name = filename.includes("authoring-sdk") ? "@prismatix/authoring-sdk" :
        filename.includes("runtime-") ? "@prismatix/runtime" : "@prismatix/cli";
      return [name, `file:../tarballs/${filename}`];
    }));
    await writeFile(join(installRoot, "package.json"), `${JSON.stringify({
      name: "prismatix-phase1-clean-machine", private: true, type: "module", dependencies,
    }, null, 2)}\n`, "utf8");
    requireSuccess(await npm([
      "install", "--ignore-scripts", "--no-audit", "--no-fund", "--package-lock=false",
    ], {cwd: installRoot, env: packagingEnvironment, timeoutMs: 180_000}), "clean npm install");
    const installedPackages = join(installRoot, "node_modules", "@prismatix");
    await assertSelfContainedTree(installedPackages, [repositoryRoot], "Installed npm packages");

    const cli = join(installedPackages, "cli", "dist", "src", "cli.js");
    assert.ok(await exists(cli), "The installed CLI entrypoint is missing");
    const importProbe = requireSuccess(await run(process.execPath, [
      "--input-type=module", "--eval",
      "const [a,r]=await Promise.all([import('@prismatix/authoring-sdk'),import('@prismatix/runtime')]);" +
      "if(typeof a.compileStory!=='function'||typeof r.createPrismatiXContext!=='function')process.exit(9);",
    ], {cwd: installRoot, env: cleanToolEnvironment()}), "installed package import probe");
    assert.equal(importProbe.stderr, "");
    log("installed npm artifacts", installRoot);

    if (configuration.mock) await createMockRuntime(copiedRuntime);
    else await copyRuntimeBundle(configuration.runtime, copiedRuntime);
    await assertSelfContainedTree(copiedRuntime, [repositoryRoot], "Copied runtime bundle");
    if (!configuration.mock) {
      const share = join(copiedRuntime, "share", "PrismatiX");
      for (const relative of [
        "runtime-manifest.json", "release-inventory.json", "LICENSE",
        "THIRD_PARTY_NOTICES.md", "ASSET_PROVENANCE.json",
        join("licenses", "fonts", "OFL-1.1.txt"),
        join("docs", "releases", "0.2.0.md"),
      ]) assert.ok(await exists(join(share, relative)),
        `Runtime release inventory is missing ${relative}`);
      const inventory = JSON.parse(await readFile(join(share, "release-inventory.json"), "utf8"));
      assert.equal(inventory.format, "PrismatiXReleaseInventory");
      assert.equal(inventory.version, "0.2.0");
      assert.equal(inventory.shipping?.desktopPlayer, true);
      assert.equal(inventory.shipping?.webGame, false);
      log("verified release inventory", inventory.target);
    }
    const toolEnvironment = cleanToolEnvironment(copiedRuntime, {
      PRISMATIX_PHASE1_PREVIEW_LOG: previewLog,
    });

    const cliRun = (args, options = {}) => run(process.execPath, [cli, ...args], {
      cwd: installRoot, env: toolEnvironment, timeoutMs: options.timeoutMs ?? 120_000,
    });
    requireSuccess(await cliRun([
      "create", projectRoot, "--name", "Clean Room Story", "--id", "phase1-clean-room",
      "--locale", "en",
    ]), "prismatix create");
    const project = join(projectRoot, "prismatix.json");
    const story = join(projectRoot, "Story", "main.pxstory");
    assert.ok(await exists(project), "create did not write the minimal prismatix.json manifest");
    assert.ok(await exists(story), "create did not write Story/main.pxstory");
    await appendFile(story, `\n${authorEdit}\n`, "utf8");
    log("created and edited starter", story);

    requireSuccess(await cliRun(["validate", project]), "prismatix validate after author edit");
    const goodStory = await readFile(story, "utf8");
    const invalidLine = goodStory.split(/\r?\n/u).length + 1;
    await appendFile(story, `\n${invalidStoryCommand}\n`, "utf8");
    const invalid = await cliRun(["validate", project]);
    assert.notEqual(invalid.code, 0, "validate accepted an intentionally invalid Story command");
    const sourceDiagnostic = parseJsonLines(invalid.stderr).find((value) =>
      value?.code === "PXSTORY1199" && value?.span?.path === "Story/main.pxstory");
    assert.ok(sourceDiagnostic, `validate did not return a source diagnostic for Story/main.pxstory:\n${invalid.stderr}`);
    assert.ok(Number(sourceDiagnostic.span?.start?.line) >= invalidLine,
      `Story diagnostic did not identify the edited line: ${JSON.stringify(sourceDiagnostic)}`);
    await writeFile(story, goodStory, "utf8");
    log("verified source diagnostics", `PXSTORY1199 at Story/main.pxstory:${sourceDiagnostic.span.start.line}`);

    requireSuccess(await cliRun([
      "dev", project, "--runtime", copiedRuntime, "--once",
    ]), "prismatix dev --once");
    if (configuration.mock) {
      const requests = parseJsonLines(await readFile(previewLog, "utf8"));
      for (const type of ["hello", "applyRuntimeIr", "play", "shutdown"])
        assert.ok(requests.some((request) => request.type === type), `dev --once did not send ${type}`);
      const apply = requests.find((request) => request.type === "applyRuntimeIr");
      const ir = await readFile(join(apply.projectRoot, ...apply.irPath.split("/")), "utf8");
      assert.ok(ir.includes(authorEdit), "dev compiled stale Story content instead of the author edit");
    }
    log("preview smoke passed", configuration.mock ? "mock protocol" : "native PreviewHost");

    const built = requireSuccess(await cliRun([
      "build", project, "--runtime", copiedRuntime,
    ]), "prismatix build distribution");
    const buildReport = lastObject(built.stdout, (value) => value.packed === true || value.built === true);
    assert.ok(buildReport, `build did not report an output:\n${built.stdout}`);
    const buildOutput = resolve(String(buildReport.output));
    assertOutsideRepository(buildOutput, "Build output");
    assert.ok(await exists(buildOutput), `Build output is missing: ${buildOutput}`);

    const packed = requireSuccess(await cliRun([
      "pack", project, "--runtime", copiedRuntime, "--output", explicitPackage,
    ]), "prismatix pack distribution");
    const packReport = lastObject(packed.stdout, (value) => value.packed === true);
    assert.ok(packReport, `pack did not report its Player/output:\n${packed.stdout}`);
    const packageOutput = resolve(String(packReport.output));
    const player = resolve(String(packReport.player));
    assert.equal(normalized(packageOutput), normalized(explicitPackage));
    await assertSelfContainedTree(packageOutput,
      [repositoryRoot, copiedRuntime, configuration.runtime ?? ""].filter(Boolean),
      "Packaged game");
    if (!configuration.mock) {
      for (const relative of [
        "LICENSE", "THIRD_PARTY_NOTICES.md", "ASSET_PROVENANCE.json",
        "release-inventory.json", join("licenses", "fonts", "OFL-1.1.txt"),
        join("docs", "releases", "0.2.0.md"),
      ]) assert.ok(await exists(join(packageOutput, "Notices", relative)),
        `Packaged Player notice inventory is missing ${relative}`);
    }
    await launchArtifact(player, packageOutput, toolEnvironment, configuration.mock);
    log("portable artifact smoke passed", player);

    log("PASS", configuration.mock ? "fast mock gate" : "real playable runtime gate");
  } catch (error) {
    configuration.keep = true;
    process.stderr.write(`[phase1] FAIL: ${error?.stack ?? error}\n`);
    process.stderr.write(`[phase1] retained workspace: ${temporaryRoot}\n`);
    process.exitCode = 1;
  } finally {
    if (!configuration.keep) await rm(temporaryRoot, {recursive: true, force: true});
  }
}

await main();
