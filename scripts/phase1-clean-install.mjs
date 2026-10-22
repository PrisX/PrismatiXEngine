#!/usr/bin/env node

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {constants, readFileSync, statSync} from "node:fs";
import {
  access, appendFile, cp, lstat, mkdir, mkdtemp, readFile, readdir,
  realpath, rm, writeFile,
} from "node:fs/promises";
import {createRequire} from "node:module";
import {homedir, tmpdir} from "node:os";
import {
  basename, delimiter, dirname, extname, isAbsolute, join, resolve,
} from "node:path";
import {fileURLToPath} from "node:url";
import {startCandidateRegistry} from "./candidate-registry.mjs";
import {checkPackedContents, nativePackageName, packPublicPackages, publicPackages, releasePlatforms, tarballName,
  webPlayerPackageName} from "./release-packages.mjs";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
// The release version every candidate package and the bundled runtime must carry.
const releaseVersion = JSON.parse(readFileSync(join(repositoryRoot, "packages", "cli", "package.json"), "utf8")).version;
const authorEdit = "The clean-install edit reached Preview and the packaged game.";
// The native package npm and pnpm must install for this machine (an optional dependency of the CLI).
const hostPlatform = `${process.platform}-${process.arch}`;
const hostNativePackage = nativePackageName(hostPlatform);
const invalidStoryCommand = "[phase1-invalid-command]";
const defaultTimeoutMs = 60_000;

function parseArguments(values) {
  const result = {mock: false, keep: false, runtime: process.env.PRISMATIX_PHASE1_RUNTIME, projectName: "first-game",
    packageManager: "pnpm"};
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (value === "--mock") result.mock = true;
    else if (value === "--keep") result.keep = true;
    else if (value === "--runtime") result.runtime = values[++index];
    else if (value.startsWith("--runtime=")) result.runtime = value.slice("--runtime=".length);
    else if (value === "--project-name") result.projectName = values[++index];
    else if (value.startsWith("--project-name=")) result.projectName = value.slice("--project-name=".length);
    else if (value === "--candidates") result.candidates = values[++index];
    else if (value.startsWith("--candidates=")) result.candidates = value.slice("--candidates=".length);
    else if (value === "--package-manager") result.packageManager = values[++index];
    else if (value.startsWith("--package-manager=")) result.packageManager = value.slice("--package-manager=".length);
    else throw new Error(`Unknown argument: ${value}`);
  }
  if (typeof result.projectName !== "string" || result.projectName.length === 0 ||
      result.projectName === "." || result.projectName === ".." || /[\\/<>:"|?*\u0000-\u001f]/u.test(result.projectName)) {
    throw new Error("--project-name must be one nonempty portable directory name");
  }
  if (result.packageManager !== "pnpm" && result.packageManager !== "npm")
    throw new Error("--package-manager must be pnpm (the documented author path) or npm");
  if (result.candidates !== undefined && result.mock)
    throw new Error("--candidates tests assembled release tarballs and cannot be combined with --mock");
  if (!result.mock && result.candidates === undefined && (result.runtime === undefined || result.runtime.length === 0)) {
    throw new Error(
      "The real Phase 1 gate requires --runtime <installed-prefix>, PRISMATIX_PHASE1_RUNTIME " +
      "or --candidates <assembled release tarballs>. Use --mock only for the fast packaging/workflow gate.",
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

// Where a packed game keeps its data: beside the executable (flat, Linux),
// in the Windows engine folder (default "bundled" layout) or in a macOS
// .app's Contents/Resources.
async function packageDataRoot(output) {
  if (await exists(join(output, "Package", "manifest.json"))) return output;
  for (const entry of await readdir(output)) {
    const candidate = entry.endsWith(".app") ? join(output, entry, "Contents", "Resources") : join(output, entry);
    if (await exists(join(candidate, "Package", "manifest.json"))) return candidate;
  }
  return output;
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
    throw new Error(`${label} failed (${result.code}${result.signal ? `, ${result.signal}` : ""})\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
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
  // Under `pnpm run`, npm_execpath is pnpm; this harness tests the npm flow.
  const candidate = process.env.npm_execpath;
  if (candidate !== undefined && candidate.length > 0 && extname(candidate).toLowerCase() !== ".ps1" &&
      !basename(candidate).toLowerCase().startsWith("pnpm")) {
    return {command: process.execPath, prefix: [candidate]};
  }
  const nodeRoot = dirname(process.execPath);
  const candidates = process.platform === "win32"
    ? [join(nodeRoot, "node_modules", "npm", "bin", "npm-cli.js"),
       join(nodeRoot, "..", "node_modules", "npm", "bin", "npm-cli.js")]
    : [join(nodeRoot, "..", "lib", "node_modules", "npm", "bin", "npm-cli.js")];
  return {command: process.execPath, prefix: [candidates[0]]};
}

function existsSyncSafe(path) {
  try { return statSync(path).isFile(); } catch { return false; }
}

/** The pnpm an author has on PATH, run without a shell, and the directory holding it. */
function pnpmInvocation() {
  const windows = process.platform === "win32";
  const pathDirectories = (process.env.PATH ?? process.env.Path ?? "").split(delimiter).filter(Boolean);
  const directory = pathDirectories.find((candidate) => windows
    ? existsSyncSafe(join(candidate, "pnpm.exe")) || existsSyncSafe(join(candidate, "pnpm.cmd"))
    : existsSyncSafe(join(candidate, "pnpm")));
  assert.ok(directory !== undefined,
    "pnpm is not on PATH; install pnpm or run `corepack enable pnpm`, or pass --package-manager npm");
  // Under `pnpm run`, npm_execpath is the running pnpm's JavaScript entry.
  const launcher = process.env.npm_execpath;
  if (launcher !== undefined && /^pnpm(\.c?js)?$/u.test(basename(launcher)) && existsSyncSafe(launcher))
    return extname(launcher) === "" ? {command: launcher, prefix: [], directory}
      : {command: process.execPath, prefix: [launcher], directory};
  if (!windows) return {command: join(directory, "pnpm"), prefix: [], directory};
  if (existsSyncSafe(join(directory, "pnpm.exe"))) return {command: join(directory, "pnpm.exe"), prefix: [], directory};
  for (const script of [join(directory, "node_modules", "pnpm", "bin", "pnpm.cjs"),
    join(directory, "node_modules", "corepack", "dist", "pnpm.js")])
    if (existsSyncSafe(script)) return {command: process.execPath, prefix: [script], directory};
  throw new Error(`Found a pnpm shim in ${directory} but not its JavaScript entry`);
}

async function pnpm(args, options = {}) {
  const invocation = pnpmInvocation();
  return run(invocation.command, [...invocation.prefix, ...args], options);
}

/**
 * Corepack runs the newest pnpm for a directory whose package.json files name
 * none, and pnpm 12 fetches its platform binary from the npm registry on first
 * use. Left to the install under test that is a registry request in the middle
 * of the journey (a 504 on it failed the Windows journey), so run pnpm once
 * here, in the real environment and with a few tries: the download lands in
 * Corepack's cache, which the clean room shares. A pnpm that needs nothing
 * downloaded answers at once.
 */
async function warmPnpm(directory, corepackHome) {
  const environment = {...process.env, COREPACK_HOME: corepackHome, COREPACK_ENABLE_DOWNLOAD_PROMPT: "0"};
  let last;
  for (let attempt = 1; attempt <= 4; attempt += 1) {
    last = await pnpm(["--version"], {cwd: directory, env: environment, timeoutMs: 180_000});
    if (last.code === 0) return last.stdout.trim();
    await new Promise((accept) => setTimeout(accept, attempt * 5_000));
  }
  throw new Error(`pnpm --version failed (${last.code}${last.signal ? `, ${last.signal}` : ""}) after 4 tries
stdout:
${last.stdout}
stderr:
${last.stderr}`);
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
    // npm links dependency executables into node_modules/.bin on POSIX; such a
    // link is fine while its target stays inside the tree (the check below).
    const resolved = await realpath(path);
    assert.ok(normalized(resolved).startsWith(`${normalized(rootRealPath)}/`),
      `${label} escapes its root: ${path} -> ${resolved}`);
    if (info.isSymbolicLink()) continue;
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

/**
 * The native package an installed CLI resolves (as the CLI itself does, from
 * its real path): this platform's, at the release version, and no other.
 */
async function installedNativePackage(cliEntry, label) {
  const resolveFrom = createRequire(await realpath(cliEntry));
  let root;
  try { root = dirname(resolveFrom.resolve(`${hostNativePackage}/package.json`)); }
  catch { assert.fail(`${label} cannot resolve ${hostNativePackage}: its optional dependency was not installed`); }
  const manifest = JSON.parse(await readFile(join(root, "package.json"), "utf8"));
  assert.equal(manifest.version, releaseVersion, `${label} got ${hostNativePackage}@${manifest.version}`);
  assert.ok(await exists(join(root, "share", "PrismatiX", "runtime-manifest.json")), `${hostNativePackage} has no runtime manifest`);
  for (const {id} of releasePlatforms) {
    const other = nativePackageName(id);
    if (other !== hostNativePackage)
      assert.throws(() => resolveFrom.resolve(`${other}/package.json`), `${label} also installed ${other} on ${hostPlatform}`);
  }
  return root;
}

/** Unpacks one file of a published tarball (bsdtar on Windows, so drive letters are not hosts). */
async function extractTarball(tarball, destination, members = []) {
  await mkdir(destination, {recursive: true});
  const tar = process.platform === "win32" ? join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe") : "tar";
  requireSuccess(await run(tar, ["-xzf", tarball, "-C", destination, ...members], {timeoutMs: 300_000}), `extract ${tarball}`);
}

/**
 * Uses already assembled release tarballs (assemble-release.mjs) instead of
 * packing the checkout: the repository's packages, this platform's native
 * package, and whichever other native packages and Web Player package exist.
 */
async function prebuiltCandidates(directory, packDirectory) {
  await mkdir(packDirectory, {recursive: true});
  const candidates = [];
  const available = new Set(await readdir(directory));
  const optional = [...releasePlatforms.map(({id}) => nativePackageName(id)), webPlayerPackageName]
    .filter((name) => name !== hostNativePackage && available.has(tarballName(name, releaseVersion)));
  for (const name of [hostNativePackage, ...optional, ...publicPackages.map((item) => item.name)]) {
    const filename = tarballName(name, releaseVersion);
    assert.ok(available.has(filename), `${directory} has no ${filename}`);
    const tarball = join(packDirectory, filename);
    await cp(join(directory, filename), tarball);
    const unpacked = join(packDirectory, "unpacked", basename(filename, ".tgz"));
    await extractTarball(tarball, unpacked);
    const manifest = JSON.parse(await readFile(join(unpacked, "package", "package.json"), "utf8"));
    assert.equal(manifest.name, name);
    assert.equal(manifest.version, releaseVersion, `${filename} is not the checkout's release version`);
    candidates.push({manifest, tarball, root: join(unpacked, "package")});
  }
  return candidates;
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
  if (request.type === "hello") write({...response(request, "ready"), runtimeIrSchemaRevision: 1, uiSchemaRevision: 1, renderMode: "mock"});
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
    format: "PrismatiXRuntimeManifest", schemaRevision: 1, version: releaseVersion, testOnly: true,
    target: `${process.platform}-${process.arch}`, binDir: "../../bin",
    tools: {previewHost: previewName, packager: packagerName, player: playerName},
  }, null, 2)}\n`, "utf8");
  const property = (id, valueType) => ({id, valueType,
    writable: true, bindable: true, animatable: false});
  // What the starter's startup screen (Content/UI/Splash.tsx) uses: show
  // transitions, centred text and an opacity animation on its Logo group.
  const showTransition = [property("showTransitionSeconds", "number"), property("showOffsetX", "number"),
    property("showOffsetY", "number"), property("showTransitionEase", "string"),
    property("showDelaySeconds", "number"), property("showScale", "number")];
  await writeFile(join(manifestDirectory, "PrismatiXUiTypeRegistry.json"), `${JSON.stringify({
    format: "PrismatiXUiTypeRegistry", schemaRevision: 1, testOnly: true,
    controls: [
      {runtimeType: "Control", properties: [{...property("opacity", "number"), animatable: true}]},
      {runtimeType: "Panel", properties: []},
      {runtimeType: "Label", properties: [property("fontSize", "number"), property("wrap", "boolean"),
        property("letterSpacing", "number"), property("horizontalAlignment", "string"),
        property("verticalAlignment", "string"), ...showTransition]},
      {runtimeType: "RichTextLabel", properties: [property("fontSize", "number"), property("wrap", "boolean")]},
      {runtimeType: "Button", properties: [property("focusMode", "string"), property("horizontalAlignment", "string"),
        property("fontSize", "number"), property("mouseFilter", "string")]},
      // What the starter's TSX title writes (packages/cli/templates/minimal).
      {runtimeType: "TextureRect", properties: [property("path", "string"),
        property("scaleMode", "string"), property("mouseFilter", "string"), ...showTransition]},
      {runtimeType: "ColorRect", properties: [property("color", "color"),
        property("mouseFilter", "string")]},
    ],
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
      // The exit event ends this wait; a Player still inside GPU or audio
      // device start-up on a busy machine can take seconds to be torn down.
      30_000);
    }, settleMs);
  });
}

async function watchAuthorJourney(cli, project, story, environment, evidenceRoot) {
  const child = spawn(process.execPath, [cli, "dev", project, "--json"], {env: environment,
    cwd: dirname(project), shell: false, windowsHide: true, stdio: ["ignore", "pipe", "pipe"]});
  let stdout = ""; let stderr = ""; let exited = false;
  child.stdout.setEncoding("utf8"); child.stderr.setEncoding("utf8");
  child.stdout.on("data", (value) => { stdout = (stdout + value).slice(-2 * 1024 * 1024); });
  child.stderr.on("data", (value) => { stderr = (stderr + value).slice(-2 * 1024 * 1024); });
  const closed = new Promise((accept) => child.once("close", () => { exited = true; accept(); }));
  let spawnError;
  child.once("error", (error) => { spawnError = error; });
  const waitFor = async (predicate, label) => {
    const start = Date.now();
    while (!predicate()) {
      if (spawnError || exited || Date.now() - start > 60_000)
        throw new Error(`${label} failed: ${spawnError ?? "timeout or exited"}\n${stderr}`);
      await new Promise((accept) => setTimeout(accept, 50));
    }
  };
  const commits = () => parseJsonLines(stderr).filter((value) => value.code === "PXCLI1602");
  try {
    await waitFor(() => commits().length >= 1, "initial persistent preview");
    const baseline = await readFile(story, "utf8");
    // Replace a live dialogue before [end], rather than appending unreachable text.
    const edited = baseline.replace("啊，你來了。我是白音，在這附近的宅邸做女僕。", authorEdit);
    assert.notEqual(edited, baseline, "starter dialogue anchor changed; update the journey fixture");
    await writeFile(story, edited, "utf8");
    await waitFor(() => commits().length >= 2, "live dialogue rebuild");
    const latest = commits().at(-1);
    const files = await listFiles(latest.output);
    const ir = files.find((path) => path.endsWith(".pxir"));
    assert.ok(ir && (await readFile(ir, "utf8")).includes(authorEdit), "acknowledged revision does not contain edited dialogue");
    const beforeInvalid = commits().length;
    await appendFile(story, `\n${invalidStoryCommand}\n`, "utf8");
    await waitFor(() => stderr.includes("PXSTORY1199"), "syntax error during watch");
    assert.equal(commits().length, beforeInvalid, "invalid source was committed to Preview");
    await writeFile(story, edited, "utf8");
    await waitFor(() => commits().length > beforeInvalid, "repair without restarting dev");
    assert.ok(commits().every((value) => String(value.discoveredFrom).startsWith("bundled runtime")), "preview used a runtime override or PATH fallback");
  } finally {
    child.kill("SIGTERM");
    let closeTimer;
    try { await Promise.race([closed, new Promise((_, reject) => { closeTimer = setTimeout(() => reject(new Error("dev did not close")), 10_000); })]); }
    finally { clearTimeout(closeTimer); }
    await writeFile(join(evidenceRoot, "watch.stdout.log"), stdout);
    await writeFile(join(evidenceRoot, "watch.stderr.log"), stderr);
  }
}

async function main() {
  const configuration = parseArguments(process.argv.slice(2));
  const temporaryRoot = await mkdtemp(join(tmpdir(), "prismatix-phase1-clean-"));
  assertOutsideRepository(temporaryRoot, "Acceptance workspace");
  const packDirectory = join(temporaryRoot, "tarballs");
  const installRoot = join(temporaryRoot, "author-machine");
  const projectRoot = join(temporaryRoot, configuration.projectName);
  const explicitPackage = join(temporaryRoot, "portable-game");
  const copiedRuntime = join(temporaryRoot, "runtime-bundle");
  const previewLog = join(temporaryRoot, "preview-requests.ndjson");
  let registry;

  try {
    log("workspace", temporaryRoot);
    const packagingEnvironment = {...process.env};
    delete packagingEnvironment.NODE_PATH;
    delete packagingEnvironment.INIT_CWD;
    let candidates;
    let runtimeBundle = copiedRuntime;
    if (configuration.candidates !== undefined) {
      candidates = await prebuiltCandidates(resolve(configuration.candidates), packDirectory);
      // The runtime this platform's author gets is the one in its native package.
      runtimeBundle = candidates.find(({manifest}) => manifest.name === hostNativePackage).root;
    } else if (configuration.mock) await createMockRuntime(copiedRuntime);
    else await copyRuntimeBundle(configuration.runtime, copiedRuntime);
    await assertSelfContainedTree(runtimeBundle, [repositoryRoot], "Copied runtime bundle");
    if (!configuration.mock) {
      const share = join(runtimeBundle, "share", "PrismatiX");
      for (const relative of [
        "runtime-manifest.json", "PrismatiXUiTypeRegistry.json", "release-inventory.json", "LICENSE",
        "THIRD_PARTY_NOTICES.md", "ASSET_PROVENANCE.json",
        join("licenses", "fonts", "OFL-1.1.txt"),
        join("docs", "releases", "delivery-contract.md"),
      ]) assert.ok(await exists(join(share, relative)),
        `Runtime release inventory is missing ${relative}`);
      const inventory = JSON.parse(await readFile(join(share, "release-inventory.json"), "utf8"));
      assert.equal(inventory.format, "PrismatiXReleaseInventory");
      assert.equal(inventory.version, releaseVersion);
      assert.equal(inventory.shipping?.desktopPlayer, true);
      assert.equal(inventory.shipping?.webGame, false);
      log("verified release inventory", inventory.target);
    }
    if (candidates === undefined) {
      candidates = await packPublicPackages(packDirectory, {[hostPlatform]: copiedRuntime}, packagingEnvironment);
      // A mock runtime has script tools, not the native executables.
      for (const {name, files, manifest} of candidates)
        if (!configuration.mock || name !== hostNativePackage) checkPackedContents(name, files, manifest);
      log("packed candidate npm artifacts with this platform's native package", candidates.map(({tarball}) => basename(tarball)).join(", "));
    } else log("using assembled release tarballs", candidates.map(({tarball}) => basename(tarball)).join(", "));
    const cliManifest = candidates.find(({manifest}) => manifest.name === "@prismatix/cli").manifest;
    assert.ok(cliManifest.optionalDependencies?.[hostNativePackage] === releaseVersion,
      `The CLI does not list ${hostNativePackage}@${releaseVersion} as an optional dependency`);
    // The other platforms' native packages exist on the real registry; here
    // only their metadata does, so a correct install skips them.
    const absent = releasePlatforms.filter(({id}) => !candidates.some(({manifest}) => manifest.name === nativePackageName(id)))
      .map(({id, os, cpu, libc}) => ({name: nativePackageName(id), version: releaseVersion, os: [os], cpu: [cpu],
        ...(libc ? {libc: [libc]} : {})}));
    registry = await startCandidateRegistry(candidates, {absent});
    const home = join(temporaryRoot, "home");
    const cache = join(temporaryRoot, "npm-cache");
    const prefix = join(temporaryRoot, "npm-prefix");
    await Promise.all([home, cache, prefix, installRoot].map((path) => mkdir(path, {recursive: true})));
    const npmrc = join(home, ".npmrc");
    await writeFile(npmrc, `@prismatix:registry=${registry.url}\nregistry=https://registry.npmjs.org/\naudit=false\nfund=false\n`);
    const usePnpm = configuration.packageManager === "pnpm";
    const pnpmHome = join(temporaryRoot, "pnpm-home");
    if (usePnpm) {
      await mkdir(pnpmHome, {recursive: true});
      const setting = (key, path) => `${key}=${path.replaceAll("\\", "/")}\n`;
      await appendFile(npmrc, setting("store-dir", join(temporaryRoot, "pnpm-store")) +
        setting("cache-dir", join(temporaryRoot, "pnpm-cache")) +
        setting("global-dir", join(pnpmHome, "global")) + setting("global-bin-dir", pnpmHome));
    }
    // Corepack's pnpm keeps its own download cache; keep the real one although
    // HOME/LOCALAPPDATA move into the clean room.
    const corepackHome = process.env.COREPACK_HOME ?? (process.platform === "win32"
      ? join(process.env.LOCALAPPDATA ?? join(homedir(), "AppData", "Local"), "node", "corepack")
      : join(process.env.XDG_CACHE_HOME ?? join(homedir(), ".cache"), "node", "corepack"));
    if (usePnpm) log(`pnpm ${await warmPnpm(temporaryRoot, corepackHome)} ready`, pnpmInvocation().directory);
    const toolEnvironment = cleanToolEnvironment(undefined, {HOME: home, USERPROFILE: home,
      APPDATA: join(home, "AppData", "Roaming"), LOCALAPPDATA: join(home, "AppData", "Local"),
      PRISMATIX_PHASE1_PREVIEW_LOG: previewLog,
      ...(usePnpm ? {PNPM_HOME: pnpmHome, COREPACK_HOME: corepackHome, COREPACK_ENABLE_DOWNLOAD_PROMPT: "0"} : {})});
    // An author who uses pnpm has it, and its global bin directory, on PATH
    // (older pnpm uses PNPM_HOME itself, newer pnpm PNPM_HOME/bin).
    if (usePnpm) toolEnvironment.PATH = [pnpmHome, join(pnpmHome, "bin"), pnpmInvocation().directory,
      toolEnvironment.PATH].join(delimiter);
    // cleanToolEnvironment removes inherited npm settings before these isolated settings are added.
    Object.assign(toolEnvironment, {npm_config_prefix: prefix, npm_config_cache: cache, npm_config_userconfig: npmrc});
    let installedPackages;
    if (usePnpm) {
      const pnpmEntry = pnpmInvocation();
      requireSuccess(await pnpm(["add", "--global", "@prismatix/cli", "--ignore-scripts"],
        {cwd: installRoot, env: toolEnvironment, timeoutMs: 300_000}),
      `pnpm add --global @prismatix/cli (isolated candidate registry; ${[pnpmEntry.command, ...pnpmEntry.prefix].join(" ")})`);
      const globalRoot = requireSuccess(await pnpm(["root", "--global"], {cwd: installRoot, env: toolEnvironment}), "pnpm root --global");
      // pnpm 10 reports the global node_modules itself; newer pnpm reports the
      // global directory whose isolated installs each have their own node_modules.
      const reportedRoot = globalRoot.stdout.trim();
      const layouts = [join(reportedRoot, "@prismatix")];
      for (const entry of await readdir(reportedRoot, {withFileTypes: true}).catch(() => []))
        if (entry.isDirectory()) layouts.push(join(reportedRoot, entry.name, "node_modules", "@prismatix"));
      installedPackages = layouts[0];
      for (const layout of layouts)
        if (await exists(join(layout, "cli", "dist", "src", "cli.js"))) { installedPackages = layout; break; }
      // Older pnpm links global commands into PNPM_HOME, newer pnpm into PNPM_HOME/bin.
      const shim = process.platform === "win32" ? "prismatix.cmd" : "prismatix";
      assert.ok(await exists(join(pnpmHome, shim)) || await exists(join(pnpmHome, "bin", shim)),
        "pnpm did not create the global command shim");
    } else {
      requireSuccess(await npm(["install", "-g", "@prismatix/cli", "--ignore-scripts", "--no-audit", "--no-fund"],
        {cwd: installRoot, env: toolEnvironment, timeoutMs: 180_000}), "npm install -g @prismatix/cli (isolated candidate registry)");
      const globalModules = process.platform === "win32" ? join(prefix, "node_modules") : join(prefix, "lib", "node_modules");
      installedPackages = join(globalModules, "@prismatix");
      assert.ok(await exists(join(prefix, process.platform === "win32" ? "prismatix.cmd" : "bin/prismatix")), "npm did not create the global command shim");
    }
    const cli = join(installedPackages, "cli", "dist", "src", "cli.js");
    assert.ok(await exists(cli), "The global CLI entrypoint is missing");
    if (usePnpm) {
      // pnpm links each package into node_modules from its content-addressed
      // store; check every installed package where it really lives.
      for (const name of await readdir(installedPackages))
        await assertSelfContainedTree(await realpath(join(installedPackages, name)), [repositoryRoot],
          `Installed candidate package @prismatix/${name}`);
    } else await assertSelfContainedTree(installedPackages, [repositoryRoot], "Installed candidate packages");
    log(`installed global candidate CLI with ${configuration.packageManager}`, usePnpm ? pnpmHome : prefix);
    const globalNative = await installedNativePackage(cli, "The global CLI");
    await assertSelfContainedTree(globalNative, [repositoryRoot], `Installed ${hostNativePackage}`);
    log(`installed ${hostNativePackage} with the global CLI`, globalNative);

    const cliRun = (args, options = {}) => run(process.execPath, [cli, ...args, "--json"], {
      cwd: installRoot, env: toolEnvironment, timeoutMs: options.timeoutMs ?? 120_000,
    });
    requireSuccess(await cliRun([
      // The project uses the global install's package manager: pnpm (the
      // documented path) or npm with --package-manager npm.
      "create", projectRoot, "--name", "Clean Room Story", "--id", "phase1-clean-room",
      "--locale", "zh-TW", "--package-manager", configuration.packageManager,
    ]), "prismatix create");
    const project = join(projectRoot, "prismatix.json");
    const story = join(projectRoot, "Story", "main.pxstory");
    assert.ok(await exists(project), "create did not write the minimal prismatix.json manifest");
    assert.ok(await exists(story), "create did not write Story/main.pxstory");
    assert.ok(await exists(join(projectRoot, usePnpm ? "pnpm-lock.yaml" : "package-lock.json")), "create did not install and lock project tooling");
    const projectCli = join(projectRoot, "node_modules", "@prismatix", "cli", "dist", "src", "cli.js");
    assert.ok(await exists(projectCli), "create did not prepare pinned project CLI");
    log("created prepared starter", story);
    log(`installed ${hostNativePackage} with the project CLI`, await installedNativePackage(projectCli, "The project CLI"));
    const projectManifest = JSON.parse(await readFile(join(projectRoot, "package.json"), "utf8"));
    assert.equal(projectManifest.devDependencies?.[webPlayerPackageName], undefined, "create must not add the Web Player");

    // The Web Player is installed on demand: without it a web export names the
    // exact install command; with the candidate tarball it installs and exports.
    const webOutput = join(temporaryRoot, "web-game");
    const missingWeb = await cliRun(["pack", projectRoot, "--target", "web", "--output", webOutput]);
    assert.notEqual(missingWeb.code, 0, "pack --target web succeeded without the Web Player");
    const missingDiagnostic = parseJsonLines(missingWeb.stderr).find((value) => value?.code === "PXCLI1262");
    const installHint = usePnpm ? `pnpm add -D ${webPlayerPackageName}@${releaseVersion}`
      : `npm install -D --save-exact ${webPlayerPackageName}@${releaseVersion}`;
    assert.ok(missingDiagnostic && String(missingDiagnostic.hint).includes(installHint),
      `pack --target web without the Web Player did not name \`${installHint}\`:\n${missingWeb.stderr}`);
    log("web export without the Web Player names its install", installHint);
    if (candidates.some(({manifest}) => manifest.name === webPlayerPackageName)) {
      const added = requireSuccess(await cliRun(["add", "web-player", "--project", projectRoot], {timeoutMs: 300_000}),
        "prismatix add web-player");
      assert.equal(lastObject(added.stdout, (value) => value.webPlayer !== undefined)?.webPlayer?.source, "project",
        `add web-player did not install the project's Web Player:\n${added.stdout}`);
      const pinned = JSON.parse(await readFile(join(projectRoot, "package.json"), "utf8")).devDependencies?.[webPlayerPackageName];
      assert.equal(pinned, releaseVersion, `the project pins ${webPlayerPackageName}@${pinned}, not exactly ${releaseVersion}`);
      const exported = requireSuccess(await cliRun(["pack", projectRoot, "--target", "web", "--output", webOutput],
        {timeoutMs: 600_000}), "prismatix pack --target web");
      assert.ok(lastObject(exported.stdout, (value) => value.packed === true), `web export did not report:\n${exported.stdout}`);
      for (const file of ["index.html", "web-player.json", "prismatix-player.wasm"])
        assert.ok(await exists(join(webOutput, file)), `web export has no ${file}`);
      await rm(webOutput, {recursive: true, force: true});
      log("installed the Web Player on demand and exported for the web", webOutput);
    }

    requireSuccess(await cliRun(["validate", project]), "prismatix validate after author edit");
    if (configuration.mock) {
      // The starter title is TSX; add a label whose fontSize the bundled
      // registry must reject by type.
      const title = join(projectRoot, "Content", "UI", "Title.tsx");
      const original = await readFile(title, "utf8");
      assert.ok(original.includes("    </Group>"), "starter title must close its root Group");
      await writeFile(title, original.replace("    </Group>",
        "      <Text name=\"RegistryProbe\" text=\"\" runtimeProperties={{fontSize: \"large\"}} />\n    </Group>"), "utf8");
      const rejected = await cliRun(["validate", project]);
      assert.notEqual(rejected.code, 0, "bundled registry accepted an invalid fontSize");
      assert.ok(parseJsonLines(rejected.stderr).some((entry) => entry?.code === "PXSDKSEM1324"),
        `bundled registry was not used by validate:\n${rejected.stderr}`);
      await writeFile(title, original, "utf8");
    }
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

    await watchAuthorJourney(cli, project, story, toolEnvironment, temporaryRoot);
    log("persistent edit/error/repair journey passed", configuration.mock ? "mock protocol only" : "native PreviewHost acknowledgements");
    // Preserve the separate SDK consumer import regression without making it an author setup step.
    const consumerRoot = join(temporaryRoot, "sdk-consumer-probe");
    await mkdir(consumerRoot);
    await writeFile(join(consumerRoot, "package.json"), JSON.stringify({private: true, type: "module",
      dependencies: {"@prismatix/authoring-sdk": releaseVersion, "@prismatix/runtime": releaseVersion}}));
    requireSuccess(await npm(["install", "--ignore-scripts", "--no-audit", "--no-fund"],
      {cwd: consumerRoot, env: toolEnvironment, timeoutMs: 180_000}), "isolated SDK consumer install probe");
    const importProbe = requireSuccess(await run(process.execPath, ["--input-type=module", "--eval",
      "const [a,r]=await Promise.all([import('@prismatix/authoring-sdk'),import('@prismatix/runtime')]);" +
      "if(typeof a.compileStory!=='function'||typeof r.createPrismatiXContext!=='function')process.exit(9);"],
    {cwd: consumerRoot, env: toolEnvironment}), "installed package import probe");
    assert.equal(importProbe.stderr, "");
    await registry.close();
    registry = undefined;
    // Closed registry + offline npm mode: prepared projects must need no initialization download.
    toolEnvironment.npm_config_offline = "true";
    requireSuccess(await cliRun(["dev", project, "--once"]), "offline prismatix dev --once");
    if (configuration.mock) {
      const requests = parseJsonLines(await readFile(previewLog, "utf8"));
      for (const type of ["hello", "applyRuntimeIr", "play", "shutdown"])
        assert.ok(requests.some((request) => request.type === type), `dev --once did not send ${type}`);
      const apply = requests.filter((request) => request.type === "applyRuntimeIr").at(-1);
      const ir = await readFile(join(apply.projectRoot, ...apply.irPath.split("/")), "utf8");
      assert.ok(ir.includes(authorEdit), "dev compiled stale Story content instead of the author edit");
    }
    log("preview smoke passed", configuration.mock ? "mock protocol" : "native PreviewHost");

    const built = requireSuccess(await cliRun([
      "build", project,
    ]), "prismatix build distribution");
    const buildReport = lastObject(built.stdout, (value) => value.packed === true || value.built === true);
    assert.ok(buildReport, `build did not report an output:\n${built.stdout}`);
    const buildOutput = resolve(String(buildReport.output));
    assertOutsideRepository(buildOutput, "Build output");
    assert.ok(await exists(buildOutput), `Build output is missing: ${buildOutput}`);
    assert.equal(typeof buildReport.player, "string", "Distribution build did not report its Player");
    const buildPlayer = resolve(buildReport.player);
    await launchArtifact(buildPlayer, buildOutput, toolEnvironment, configuration.mock);
    log("default build artifact smoke passed", buildPlayer);

    const packed = requireSuccess(await cliRun([
      "pack", project, "--output", explicitPackage,
    ]), "prismatix pack distribution");
    const packReport = lastObject(packed.stdout, (value) => value.packed === true);
    assert.ok(packReport, `pack did not report its Player/output:\n${packed.stdout}`);
    const packageOutput = resolve(String(packReport.output));
    const player = resolve(String(packReport.player));
    assert.equal(normalized(packageOutput), normalized(explicitPackage));
    await assertSelfContainedTree(packageOutput,
      [repositoryRoot, runtimeBundle, configuration.runtime ?? ""].filter(Boolean),
      "Packaged game");
    if (!configuration.mock) {
      const notices = join(await packageDataRoot(packageOutput), "Notices");
      for (const relative of [
        "LICENSE", "THIRD_PARTY_NOTICES.md", "ASSET_PROVENANCE.json",
        "release-inventory.json", join("licenses", "fonts", "OFL-1.1.txt"),
        join("docs", "releases", "delivery-contract.md"),
      ]) assert.ok(await exists(join(notices, relative)),
        `Packaged Player notice inventory is missing ${relative}`);
    }
    await launchArtifact(player, packageOutput, toolEnvironment, configuration.mock);
    log("portable artifact smoke passed", player);

    await writeFile(join(temporaryRoot, "evidence.json"), JSON.stringify({
      candidateOnly: true, published: false, mock: configuration.mock,
      platform: process.platform, architecture: process.arch,
      packageManager: configuration.packageManager,
      globalInstall: true, automaticProjectInstall: true, bundledRuntime: true,
      watchEditAndRepair: true, offlineReopenAndBuild: true,
      nativeVisualInteraction: "not measured by this harness", externalAuthorTrial: "not run",
      packages: candidates.map(({manifest, tarball}) => ({name: manifest.name, version: manifest.version, tarball})),
      projectName: configuration.projectName, projectRoot, player, packageOutput,
      buildPlayer, buildOutput, defaultBuildPlayerLaunch: true,
    }, null, 2));
    log("PASS", configuration.mock ? "candidate install/workflow gate (mock runtime; no native claim)" : "candidate native handshake / launch smoke (visual interaction not measured)");
  } catch (error) {
    configuration.keep = true;
    process.stderr.write(`[phase1] FAIL: ${error?.stack ?? error}\n`);
    process.stderr.write(`[phase1] retained workspace: ${temporaryRoot}\n`);
    process.exitCode = 1;
  } finally {
    if (registry !== undefined) await registry.close();
    if (!configuration.keep) await rm(temporaryRoot, {recursive: true, force: true});
  }
}

await main();
