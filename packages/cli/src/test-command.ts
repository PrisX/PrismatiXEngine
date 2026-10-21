// `prismatix test`: runs journey scripts against a packaged QA Player.
//
// Each journey gets its own Player process with an isolated
// PRISMATIX_PLAYER_DATA_ROOT and PRISMATIX_QA_IPC=127.0.0.1:0. The Player
// writes the port it bound to PRISMATIX_QA_IPC_PORT_FILE; the runner connects
// and calls the journey's default export with a PlayerTestClient
// (qa-client.ts). Only PrismatiXPlayerQA answers; a shipped Player never
// opens the port, which the runner reports instead of hanging.

import {spawn, type ChildProcess} from "node:child_process";
import {mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, extname, join, resolve} from "node:path";
import {pathToFileURL} from "node:url";

import {PlayerTestClient, type Journey, type JourneyContext} from "./qa-client.js";
import {remapExtensionStack} from "./extension-build.js";

export interface TestCommandOptions {
  readonly values: ReadonlyMap<string, string>;
  readonly flags: ReadonlySet<string>;
  readonly positional: readonly string[];
}

export const testHelp = [
  "Run journey scripts against a packaged QA Player through its QA IPC test interface.",
  "prismatix test [journey...] [--project <dir>] [--package <distribution> | --player <exe>]",
  "    [--output <dir>] [--timeout <ms>] [--data-root <dir>] [--working-directory <dir>] [--keep-data]",
  "",
  "Journeys are tests/journeys/*.ts|.js|.mjs of the project (all of them when none is named), or",
  "paths. Each exports `default async function (px, context)`; px is the PrismatixTestClient that",
  "`import type {PrismatixTestClient} from \"@prismatix/cli/test\"` describes:",
  "  await px.click(\"Start\"); await px.waitFor({route: \"hud\", idle: true});",
  "  await px.jump(\"01_scene:label\"); await px.choose(0); await px.screenshot(\"choice.png\");",
  "The Player must be PrismatiXPlayerQA: pack with --player <build>/PrismatiXPlayerQA.exe. By default",
  "the package is dist/<project id>. Every journey starts a fresh Player with its own user data",
  "(kept on failure or with --keep-data). Screenshots with relative paths go to",
  "<output>/<journey> (default .prismatix/test-results). A journey fails when it throws, exceeds its",
  "time limit or the Player exits uncleanly; the report then shows the tail of the Player log. The",
  "limit is --timeout when given, else the journey's own `export const timeoutMs = 600_000`, else",
  "180000 ms. The exit code is 1 when any journey failed.",
  "Protocol: newline-delimited JSON {id, op, ...} -> {id, ok, result|error} on 127.0.0.1; operations",
  "state, controls, click, action, key, tap, input, bindings, uiData, variable, jump, advance,",
  "advanceUntil, choose, waitFor, screenshot, save, load, setting, route, resource, log, batch, quit.",
];

const journeyExtensions = [".ts", ".mts", ".js", ".mjs"];
const defaultJourneyTimeoutMs = 180_000;

interface JourneyResult {
  readonly name: string;
  readonly file: string;
  readonly passed: boolean;
  readonly seconds: number;
  readonly error?: string;
  readonly logTail?: string;
  readonly dataRoot: string;
  readonly outputDirectory: string;
}

function isJourneyFile(name: string): boolean {
  return journeyExtensions.includes(extname(name)) && !name.endsWith(".d.ts") && !name.startsWith(".");
}

async function regularFile(path: string): Promise<boolean> {
  try { return (await stat(path)).isFile(); } catch { return false; }
}

async function readJsonObject(path: string): Promise<Record<string, unknown> | undefined> {
  try {
    const value: unknown = JSON.parse(await readFile(path, "utf8"));
    return typeof value === "object" && value !== null && !Array.isArray(value) ?
      value as Record<string, unknown> : undefined;
  } catch { return undefined; }
}

async function findJourneys(projectRoot: string, requested: readonly string[]): Promise<string[]> {
  const directory = join(projectRoot, "tests", "journeys");
  if (requested.length === 0) {
    let entries: string[];
    try { entries = await readdir(directory); }
    catch { throw new Error(`No journeys: ${directory} does not exist`); }
    const files = entries.filter(isJourneyFile).sort().map((name) => join(directory, name));
    if (files.length === 0) throw new Error(`No journeys in ${directory}`);
    return files;
  }
  const files: string[] = [];
  for (const name of requested) {
    const candidates = [resolve(name), ...journeyExtensions.map((extension) =>
      join(directory, name.endsWith(extension) ? name : `${name}${extension}`))];
    const found = await (async () => {
      for (const candidate of candidates) if (await regularFile(candidate)) return candidate;
      return undefined;
    })();
    if (found === undefined) throw new Error(`Journey not found: ${name} (looked in ${directory})`);
    files.push(found);
  }
  return files;
}

async function resolvePlayer(options: TestCommandOptions, projectRoot: string): Promise<string> {
  const explicit = options.values.get("player");
  if (explicit !== undefined) {
    const path = resolve(explicit);
    if (!await regularFile(path)) throw new Error(`--player ${path} is not a file`);
    return path;
  }
  const project = await readJsonObject(join(projectRoot, "prismatix.json"));
  const packageDirectory = options.values.get("package") !== undefined ?
    resolve(options.values.get("package")!) :
    typeof project?.id === "string" ? join(projectRoot, "dist", project.id) : undefined;
  if (packageDirectory === undefined)
    throw new Error("Name the Player with --player <exe> or the distribution with --package <dir>");
  const distribution = project?.distribution as Record<string, unknown> | undefined;
  const suffix = process.platform === "win32" ? ".exe" : "";
  const names = [
    ...(typeof distribution?.executableName === "string" ? [distribution.executableName] : []),
    "PrismatiXPlayerQA", "PrismatiXPlayer"];
  for (const name of names) {
    const path = join(packageDirectory, `${name}${suffix}`);
    if (await regularFile(path)) return path;
  }
  throw new Error(`No Player executable in ${packageDirectory}; pack the project with ` +
    "--player <build>/PrismatiXPlayerQA" + suffix + " or pass --player");
}

interface LoadedJourney {
  readonly run: Journey;
  /** The journey's own `export const timeoutMs`. */
  readonly timeoutMs?: number;
}

export async function loadJourney(file: string, scratch: string): Promise<LoadedJourney> {
  let module: Record<string, unknown>;
  try {
    module = await import(pathToFileURL(file).href) as Record<string, unknown>;
  } catch (error) {
    const code = (error as {code?: string}).code;
    if (![".ts", ".mts"].includes(extname(file)) ||
        (code !== "ERR_UNKNOWN_FILE_EXTENSION" && code !== "ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING"))
      throw error;
    // Node without type stripping: transpile this one file.
    const ts = await import("typescript-compiler");
    const output = ts.transpileModule(await readFile(file, "utf8"), {fileName: file,
      compilerOptions: {module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022}});
    const transpiled = join(scratch, `${basename(file, extname(file))}.mjs`);
    await writeFile(transpiled, output.outputText);
    module = await import(pathToFileURL(transpiled).href) as Record<string, unknown>;
  }
  const journey = module.default ?? module.journey;
  if (typeof journey !== "function")
    throw new Error(`${file} must export a default async function (px, context)`);
  const timeoutMs = module.timeoutMs;
  if (timeoutMs !== undefined && (typeof timeoutMs !== "number" || !Number.isInteger(timeoutMs) ||
      timeoutMs <= 0))
    throw new Error(`${file}: timeoutMs must be a positive whole number of milliseconds`);
  return {run: journey as Journey, ...(timeoutMs === undefined ? {} : {timeoutMs})};
}

export async function waitForPort(portFile: string, child: ChildProcess, exited: () => boolean,
  timeoutMs: number): Promise<number> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (exited()) throw new Error(`The Player exited before opening QA IPC (code ${child.exitCode})`);
    try {
      const port = Number.parseInt((await readFile(portFile, "utf8")).trim(), 10);
      if (Number.isInteger(port) && port > 0) return port;
    } catch { /* not written yet */ }
    await new Promise((accept) => setTimeout(accept, 50));
  }
  throw new Error(`The Player did not open QA IPC within ${timeoutMs} ms; is it PrismatiXPlayerQA?`);
}

/** Player log positions in bundled TypeScript extensions, mapped back to their .ts lines. */
async function remapLog(text: string, projectRoot: string): Promise<string> {
  const project = await readJsonObject(join(projectRoot, "prismatix.json")) ??
    await readJsonObject(join(projectRoot, "project.pxproject"));
  const extensions = Array.isArray(project?.extensions)
    ? project.extensions.filter((path): path is string => typeof path === "string") : [];
  return extensions.length === 0 ? text : remapExtensionStack(text, projectRoot, extensions);
}

async function logTail(dataRoot: string, lines = 40): Promise<string | undefined> {
  const logs: string[] = [];
  try {
    for (const entry of await readdir(dataRoot)) {
      const log = join(dataRoot, entry, "logs", "PrismatiXPlayer.log");
      if (await regularFile(log)) logs.push(log);
    }
  } catch { return undefined; }
  if (logs.length === 0) return undefined;
  const text = await readFile(logs[0]!, "utf8");
  return `${logs[0]}:\n${text.split(/\r?\n/u).filter(Boolean).slice(-lines).join("\n")}`;
}

function errorText(error: unknown): string {
  if (error instanceof Error) return error.stack ?? error.message;
  return String(error);
}

async function runJourney(file: string, player: string, options: TestCommandOptions,
  projectRoot: string, outputRoot: string, timeoutOverride: number | undefined): Promise<JourneyResult> {
  const name = basename(file, extname(file));
  const started = Date.now();
  const outputDirectory = join(outputRoot, name);
  await rm(outputDirectory, {recursive: true, force: true});
  await mkdir(outputDirectory, {recursive: true});
  const scratch = await mkdtemp(join(tmpdir(), "prismatix-test-"));
  const sharedRoot = options.values.get("data-root");
  const dataRoot = sharedRoot !== undefined ? resolve(sharedRoot) : join(scratch, "user-data");
  await mkdir(dataRoot, {recursive: true});
  const portFile = join(scratch, "qa-ipc.port");
  const workingDirectory = resolve(options.values.get("working-directory") ?? dirname(player));
  let exitCode: number | null | undefined;
  let child: ChildProcess | undefined;
  let client: PlayerTestClient | undefined;
  let failure: unknown;
  const notes: string[] = [];
  try {
    const journey = await loadJourney(file, scratch);
    const timeoutMs = timeoutOverride ?? journey.timeoutMs ?? defaultJourneyTimeoutMs;
    child = spawn(player, [], {cwd: workingDirectory, stdio: "ignore", windowsHide: false,
      env: {...process.env, PRISMATIX_PLAYER_DATA_ROOT: dataRoot,
        PRISMATIX_QA_IPC: "127.0.0.1:0", PRISMATIX_QA_IPC_PORT_FILE: portFile}});
    const exited = new Promise<void>((accept) => {
      child!.once("exit", (code) => { exitCode = code; accept(); });
      child!.once("error", (error) => { failure ??= error; exitCode = null; accept(); });
    });
    const port = await waitForPort(portFile, child, () => exitCode !== undefined, 60_000);
    client = await PlayerTestClient.connect(port, outputDirectory);
    const context: JourneyContext = {name, file, projectRoot, outputDirectory, dataRoot,
      note: (message) => { notes.push(message); process.stdout.write(`  ${name}: ${message}\n`); }};
    let timer: NodeJS.Timeout | undefined;
    try {
      await Promise.race([
        Promise.resolve(journey.run(client, context)),
        new Promise<never>((_, reject) => {
          timer = setTimeout(() => reject(new Error(`Journey timed out after ${timeoutMs} ms`)), timeoutMs);
        }),
        exited.then(() => { throw new Error(`The Player exited during the journey (code ${exitCode})`); }),
      ]);
    } finally { if (timer) clearTimeout(timer); }
    if (exitCode === undefined) await client.quit().catch(() => {});
    const closed = await Promise.race([exited.then(() => true),
      new Promise<boolean>((accept) => setTimeout(() => accept(false), 15_000))]);
    if (!closed) throw new Error("The Player did not exit within 15 s after quit");
    if (exitCode !== 0) throw new Error(`The Player exited with code ${exitCode}`);
  } catch (error) {
    failure ??= error;
  } finally {
    client?.close();
    if (child !== undefined && exitCode === undefined) child.kill();
  }
  const passed = failure === undefined;
  const result: JourneyResult = {name, file, passed, seconds: (Date.now() - started) / 1000,
    dataRoot, outputDirectory,
    ...(passed ? {} : {error: errorText(failure)}),
    ...(passed ? {} : await logTail(dataRoot).then(async (tail) => tail === undefined ? {}
      : {logTail: await remapLog(tail, projectRoot)}))};
  const keep = !passed || options.flags.has("keep-data") || sharedRoot !== undefined;
  if (!keep) await rm(scratch, {recursive: true, force: true}).catch(() => {});
  else if (sharedRoot !== undefined) await rm(portFile, {force: true}).catch(() => {});
  return result;
}

export async function testCommand(options: TestCommandOptions): Promise<number> {
  const json = options.flags.has("json");
  let results: JourneyResult[] = [];
  try {
    const projectRoot = resolve(options.values.get("project") ?? ".");
    const timeoutText = options.values.get("timeout");
    const timeoutMs = timeoutText === undefined ? undefined : Number(timeoutText);
    if (timeoutMs !== undefined && (!Number.isInteger(timeoutMs) || timeoutMs <= 0))
      throw new Error("--timeout must be a positive number of milliseconds");
    const journeys = await findJourneys(projectRoot, options.positional);
    const player = await resolvePlayer(options, projectRoot);
    const outputRoot = resolve(options.values.get("output") ?? join(projectRoot, ".prismatix", "test-results"));
    if (!json) process.stdout.write(`Player ${player}\n`);
    for (const file of journeys) {
      if (!json) process.stdout.write(`RUN  ${basename(file)}\n`);
      const result = await runJourney(file, player, options, projectRoot, outputRoot, timeoutMs);
      results = [...results, result];
      if (json) continue;
      process.stdout.write(`${result.passed ? "PASS" : "FAIL"} ${result.name} (${result.seconds.toFixed(1)} s)\n`);
      if (!result.passed) {
        process.stdout.write(`${result.error}\n`);
        if (result.logTail !== undefined) process.stdout.write(`--- Player log tail: ${result.logTail}\n---\n`);
        process.stdout.write(`User data kept in ${result.dataRoot}\n`);
      }
    }
  } catch (error) {
    if (json) process.stdout.write(`${JSON.stringify({ok: false, error: String(error), journeys: results})}\n`);
    else process.stderr.write(`prismatix test: ${error instanceof Error ? error.message : String(error)}\n`);
    return 2;
  }
  const failed = results.filter((result) => !result.passed).length;
  if (json) process.stdout.write(`${JSON.stringify({ok: failed === 0, journeys: results})}\n`);
  else process.stdout.write(`${results.length - failed} passed, ${failed} failed\n`);
  return failed === 0 ? 0 : 1;
}
