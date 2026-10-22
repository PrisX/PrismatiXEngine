#!/usr/bin/env node
// A game that cannot start ends in a report, not a crash. Each boot failure
// below must make the packaged Player log PXPLAYER5031 with its cause and exit
// with code 1, with no message box in the way of a tool that reads its output
// (a pipe), and `prismatix test` must name the cause, an extension's line at
// its TypeScript source.
//
//   1. a TypeScript extension's engine.ready handler throws
//   2. the start route's UI template is missing from the package
//   3. Content.pdx is damaged (its header is not a PDX header)
//   4. Content.pdx is missing
//
// The project is an empty Starter with one extension, created by this script
// and built with the public CLI, once with the shipped Player and once with
// PrismatiXPlayerQA; each case changes the built distribution in place.
//
//   node scripts/test-player-boot-failure.mjs --cli packages/cli/dist/src/cli.js
//     --packager <PrismatiXPackager> --player <PrismatiXPlayer> --player-qa <PrismatiXPlayerQA>

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {appendFile, mkdir, mkdtemp, readFile, readdir, rename, rm, stat, symlink, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, join, resolve} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

function options(argv) {
  const result = new Map();
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (!key?.startsWith("--") || value === undefined) throw new Error(`Unexpected arguments: ${argv.join(" ")}`);
    result.set(key.slice(2), resolve(value));
  }
  for (const name of ["cli", "packager", "player", "player-qa"])
    if (!result.has(name)) throw new Error(`--${name} is required`);
  return result;
}

const args = options(process.argv.slice(2));
const cli = args.get("cli");
const cliModules = dirname(cli);
const {createStarter} = await import(pathToFileURL(join(cliModules, "starter.js")).href);
const {distributionDataRoot} = await import(pathToFileURL(join(cliModules, "distribution-layout.js")).href);

function runCli(cliArguments, expectedStatus = 0) {
  const result = spawnSync(process.execPath, [cli, ...cliArguments],
    {encoding: "utf8", stdio: "pipe", windowsHide: true, timeout: 600_000});
  if (result.status !== expectedStatus) {
    process.stderr.write(result.stdout ?? "");
    process.stderr.write(result.stderr ?? "");
    throw new Error(`prismatix ${cliArguments.join(" ")} exited with ${result.status ?? result.signal}`);
  }
  return result;
}

const isFile = async (path) => (await stat(path).catch(() => undefined))?.isFile() === true;

// The shipped Player of a distribution: at its root, or in a macOS bundle.
async function playerExecutable(distribution) {
  const name = `PrismatiXPlayer${process.platform === "win32" ? ".exe" : ""}`;
  const bundles = (await readdir(distribution)).filter((entry) => entry.endsWith(".app"))
    .map((entry) => join(distribution, entry, "Contents", "MacOS"));
  for (const directory of [distribution, ...bundles])
    if (await isFile(join(directory, name))) return join(directory, name);
  throw new Error(`No ${name} in ${distribution}`);
}

const scratch = await mkdtemp(join(tmpdir(), "prismatix-boot-failure-"));
let failed = true;
try {
  const project = join(scratch, "game");
  // The project's pinned tools are this checkout's, linked as
  // scripts/stage-starter-sample.mjs links them; nothing is downloaded.
  await createStarter(project, {name: "Boot failure", id: "boot-failure", empty: true,
    toolSource: "checkout", install: async (directory) => {
      await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
      for (const [name, path] of [["cli", "packages/cli"], ["authoring-sdk", "packages/authoring-sdk"],
        ["runtime", "runtime-sdk"]])
        await symlink(join(repositoryRoot, ...path.split("/")), join(directory, "node_modules", "@prismatix", name),
          "junction");
    }});
  runCli(["extension", "create", "boom", "--project", project]);
  const entry = join(project, "Content", "Extensions", "boom", "main.ts");
  await appendFile(entry, [
    "",
    "ctx.events.on(\"engine.ready\", () => {",
    "  throw new Error(\"boom at engine.ready\");",
    "});",
    ""].join("\n"));
  const throwLine = (await readFile(entry, "utf8")).split(/\r?\n/u)
    .findIndex((line) => line.includes("boom at engine.ready")) + 1;
  await mkdir(join(project, "tests", "journeys"), {recursive: true});
  await writeFile(join(project, "tests", "journeys", "boot.mjs"),
    "export default async function (px) { await px.waitFor({appState: \"title\"}); }\n");

  const shipped = join(scratch, "shipped");
  const qa = join(scratch, "qa");
  for (const [output, player] of [[shipped, args.get("player")], [qa, args.get("player-qa")]])
    runCli(["build", project, "--packager", args.get("packager"), "--player", player,
      "--output", output, "--no-transcode"]);
  const data = await distributionDataRoot(shipped);
  const manifestPath = join(data, "Package", "manifest.json");
  const originalManifest = await readFile(manifestPath, "utf8");
  const content = join(data, "Content.pdx");
  const executable = await playerExecutable(shipped);

  // Output is piped: the Player must not wait on a message box, and a crash
  // shows as another exit code or a signal.
  const launch = async (label) => {
    const userData = join(scratch, "user-data", label);
    await mkdir(userData, {recursive: true});
    const result = spawnSync(executable, [], {cwd: dirname(executable), encoding: "utf8", stdio: "pipe",
      windowsHide: true, timeout: 120_000,
      env: {...process.env, PRISMATIX_PLAYER_DATA_ROOT: userData}});
    const output = `${result.stdout ?? ""}${result.stderr ?? ""}`;
    if (result.error?.code === "ETIMEDOUT")
      throw new Error(`${label}: the Player did not exit within 120 s (a boot failure dialog?)\n${output}`);
    assert.equal(result.signal, null, `${label}: the Player was killed by ${result.signal}\n${output}`);
    assert.equal(result.status, 1, `${label}: the Player exited with ${result.status}, not 1\n${output}`);
    const logs = [];
    for (const game of await readdir(userData)) {
      const log = join(userData, game, "logs", "PrismatiXPlayer.log");
      if (await isFile(log)) logs.push(await readFile(log, "utf8"));
    }
    assert.equal(logs.length, 1, `${label}: expected one Player log in ${userData}`);
    for (const text of [output, logs[0]])
      assert.match(text, /PXPLAYER5031 \[Player\.Boot\] The game could not start/u,
        `${label}: no PXPLAYER5031 report\n${text}`);
    return logs[0];
  };

  // 1. The extension's engine.ready handler throws.
  {
    const log = await launch("extension");
    assert.match(log, /PXPLAYER5031 [^\n]*boom at engine\.ready/u);
    const test = runCli(["test", "--project", project, "--package", qa, "--json",
      "--data-root", join(scratch, "test-data")], 1);
    const report = JSON.parse(test.stdout);
    const [journey] = report.journeys;
    assert.equal(report.ok, false);
    assert.equal(journey.passed, false);
    assert.match(journey.bootFailure ?? "", /^PXPLAYER5031 [^\n]*boom at engine\.ready/u, test.stdout);
    assert.ok(journey.bootFailure.includes(`Content/Extensions/boom/main.ts:${throwLine}:`),
      `the report does not name main.ts:${throwLine}\n${journey.bootFailure}`);
    assert.match(journey.error, /^The game could not start \(Player exit code 1\): PXPLAYER5031/u);
  }

  // 2. The start route's template is missing; the extension is left out so
  // the boot reaches the UI.
  {
    const manifest = JSON.parse(originalManifest);
    manifest.extensions = [];
    const title = manifest.routes.find((route) => route.id === manifest.entry.route);
    title.scene = "Content/UI/Missing.pxui";
    await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
    const log = await launch("route");
    assert.match(log, /PXPLAYER5031 [^\n]*PXPLAYER5003 [^\n]*Content\/UI\/Missing\.pxui/u);
    await writeFile(manifestPath, originalManifest);
  }

  // 3. Content.pdx is damaged.
  {
    const original = await readFile(content);
    const damaged = Buffer.from(original);
    for (let index = 0; index < Math.min(256, damaged.length); index += 1) damaged[index] ^= 0x5a;
    await writeFile(content, damaged);
    const log = await launch("damaged-content");
    assert.match(log, /PXPLAYER5031 [^\n]*PXPLAYER5007 [^\n]*Content\.pdx/u);
    await writeFile(content, original);
  }

  // 4. Content.pdx is missing.
  {
    await rename(content, `${content}.away`);
    const log = await launch("missing-content");
    assert.match(log, /PXPLAYER5031 [^\n]*PXPLAYER5007 [^\n]*Content\.pdx/u);
    await rename(`${content}.away`, content);
  }
  failed = false;
  process.stdout.write(`${basename(process.argv[1])}: four boot failures reported with exit code 1\n`);
} finally {
  if (failed) process.stderr.write(`Kept ${scratch}\n`);
  else await rm(scratch, {recursive: true, force: true, maxRetries: 5, retryDelay: 100});
}
