#!/usr/bin/env node
import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {spawn, spawnSync} from "node:child_process";
import {mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile} from "node:fs/promises";
import {cpus, freemem, platform, release, tmpdir, totalmem} from "node:os";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";
import {deflateSync} from "node:zlib";
import ts from "typescript-compiler";

const repository = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
const playable = args.includes("--playable");
function option(name, fallback) { const i = args.indexOf(name); return i < 0 ? fallback : args[i + 1]; }
const lines = Number(option("--lines", "100000"));
assert.ok(Number.isInteger(lines) && lines >= 100 && lines <= 1000000, "--lines must be 100..1000000");
const baselineRef = option("--baseline-ref", "HEAD");
const candidateCli = resolve(option("--candidate-cli", join(repository, "packages/cli/dist/src/cli.js")));
const output = resolve(option("--output", join(repository, "out/release-evidence", `scale-${new Date().toISOString().replace(/[:.]/g, "-")}`)));
await mkdir(output, {recursive: true});
const fixture = await mkdtemp(join(tmpdir(), "prismatix-scale-"));
const seed = 20260922;
const evidence = {kind: "author-source-cli-artifact-benchmark", seed, lines, fixture, output,
  limitations: ["First-pass is not OS-cache-cold: no privileged cache flush is performed.",
    "Media are genuine generated PNG/PCM WAV; this benchmark validates artifact copying, not native decode/playback or frame time.",
    "One measured run per condition; timings are observations, not statistically established performance changes.",
    "Baseline CLI is transpiled from the specified Git ref and uses the same installed Authoring SDK/dependencies as the candidate."],
  machine: {platform: platform(), release: release(), arch: process.arch, node: process.version,
    cpu: cpus()[0]?.model, logicalCpus: cpus().length, totalMemoryBytes: totalmem(), freeMemoryBytesAtStart: freemem()}, runs: []};
function git(...values) {
  const result = spawnSync("git", values, {cwd: repository, encoding: "utf8", maxBuffer: 32 * 1024 * 1024});
  if (result.status !== 0) throw new Error(result.stderr);
  return result.stdout.trimEnd();
}
async function write(path, contents) { await mkdir(dirname(path), {recursive: true}); await writeFile(path, contents); }
async function json(path, data) { await write(path, `${JSON.stringify(data, null, 2)}\n`); }
const sha256 = data => createHash("sha256").update(data).digest("hex");
let randomState = seed;
function random() { randomState ^= randomState << 13; randomState ^= randomState >>> 17; randomState ^= randomState << 5; return randomState >>> 0; }
function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) { crc ^= byte; for (let j = 0; j < 8; j++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const name = Buffer.from(type);
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length); name.copy(out, 4); data.copy(out, 8);
  out.writeUInt32BE(crc32(Buffer.concat([name, data])), out.length - 4);
  return out;
}
function png(width, height) {
  const header = Buffer.alloc(13); header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 2;
  const data = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y++) for (let x = 0; x < width * 3; x++) data[y * (width * 3 + 1) + 1 + x] = random() & 255;
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk("IHDR", header), chunk("IDAT", deflateSync(data)), chunk("IEND", Buffer.alloc(0))]);
}
function wav(rate, seconds, frequency) {
  const samples = Math.floor(rate * seconds), data = Buffer.alloc(44 + samples * 2);
  data.write("RIFF"); data.writeUInt32LE(data.length - 8, 4); data.write("WAVEfmt ", 8); data.writeUInt32LE(16, 16);
  data.writeUInt16LE(1, 20); data.writeUInt16LE(1, 22); data.writeUInt32LE(rate, 24); data.writeUInt32LE(rate * 2, 28);
  data.writeUInt16LE(2, 32); data.writeUInt16LE(16, 34); data.write("data", 36); data.writeUInt32LE(samples * 2, 40);
  for (let i = 0; i < samples; i++) data.writeInt16LE(Math.round(5000 * Math.sin(2 * Math.PI * frequency * i / rate)), 44 + i * 2);
  return data;
}
async function inventory(root) {
  const files = [];
  async function visit(dir, prefix = "") {
    for (const item of await readdir(dir, {withFileTypes: true})) {
      const path = join(dir, item.name), name = `${prefix}${item.name}`;
      if (item.isDirectory()) await visit(path, `${name}/`);
      else { const bytes = await readFile(path); files.push({path: name, bytes: bytes.length, sha256: sha256(bytes)}); }
    }
  }
  await visit(root); return files;
}
try {
  evidence.head = git("rev-parse", "HEAD"); evidence.baselineCommit = git("rev-parse", baselineRef);
  evidence.workingTreeStatus = git("status", "--short");
  evidence.authoringSdkSourceDiff = git("diff", baselineRef, "--stat", "--", "packages/authoring-sdk");
  const baselineRoot = join(output, "baseline-cli");
  await json(join(baselineRoot, "package.json"), {type: "module"});
  for (const path of git("ls-tree", "--name-only", baselineRef, "packages/cli/src/").split("\n")) {
    if (!path.endsWith(".ts")) continue;
    const source = git("show", `${baselineRef}:${path}`);
    await write(join(baselineRoot, path.split("/").at(-1).replace(/\.ts$/, ".js")),
      ts.transpileModule(source, {compilerOptions: {target: ts.ScriptTarget.ES2023, module: ts.ModuleKind.ESNext}}).outputText);
  }
  const baselineCli = join(baselineRoot, "cli.js");
  evidence.cli = {baseline: {path: baselineCli, sha256: sha256(await readFile(baselineCli))},
    candidate: {path: candidateCli, sha256: sha256(await readFile(candidateCli))}};
  const preload = join(output, "measure-process.mjs");
  await write(preload, 'import {writeFileSync} from "node:fs"; process.once("exit",()=>{const r=process.resourceUsage();writeFileSync(process.env.PRISMATIX_BENCH_RESOURCE_FILE,JSON.stringify({maxRSSKiB:r.maxRSS||null,userCPUTimeMicros:r.userCPUTime,systemCPUTimeMicros:r.systemCPUTime,rssAtExitBytes:process.memoryUsage().rss}));});\n');
  const project = join(fixture, "project"), media = [], assets = [];
  for (let i = 0; i < 8; i++) {
    const width = 256 + i * 96, height = 192 + i * 64, path = `Assets/image-${i}.png`, bytes = png(width, height);
    await write(join(project, path), bytes); media.push({path, codec: "PNG RGB8", width, height, bytes: bytes.length, sha256: sha256(bytes)});
    assets.push({id: `10000000-0000-4000-8000-${String(i + 1).padStart(12, "0")}`, name: `image-${i}`, kind: "background", source: path});
  }
  for (let i = 0; i < 12; i++) {
    const sampleRate = i % 2 ? 44100 : 22050, durationSeconds = 1 + i / 4, path = `Assets/tone-${i}.wav`, bytes = wav(sampleRate, durationSeconds, 180 + i * 37);
    await write(join(project, path), bytes); media.push({path, codec: "PCM s16le mono WAV", sampleRate, durationSeconds, bytes: bytes.length, sha256: sha256(bytes)});
    assets.push({id: `20000000-0000-4000-8000-${String(i + 1).padStart(12, "0")}`, name: `tone-${i}`, kind: "audio", source: path});
  }
  if (playable) {
    for (const [name, kind, index] of [["NotoSansTC-Bold.ttf", "font", 1], ["OFL-1.1.txt", "license", 2]]) {
      const source = `Content/Fonts/${name}`;
      await write(join(project, source), await readFile(join(repository, "packages/cli/templates/minimal", source)));
      assets.push({id: `30000000-0000-4000-8000-${String(index).padStart(12, "0")}`, name, kind, source});
    }
  }
  await json(join(project, "project.pxproject"), {format: "PrismatiXProject", schemaRevision: 2, id: "release-scale", name: "Release Scale",
    version: "0.2.0", contentVersion: "scale-v1", saveVersion: 1, resolution: {width: 1280, height: 720}, entry: {story: "scene000", ui: "title"},
    defaultLocale: "en", supportedLocales: ["en"], storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame",
    extensions: [], uiEntryPoints: {title: "Content/UI/Title.pxui"}, assets});
  await json(join(project, "Content/game.pxgame"), {format: "PrismatiXGame", schemaRevision: 2, variables: [], gallery: [], unlockables: []});
  const id = "11111111-1111-4111-8111-111111111111";
  await json(join(project, "Content/UI/Title.pxui"), {format: "PrismatiXUIScene", schemaRevision: 2, id, revision: 1, name: "Title", width: 1280, height: 720, rootId: id,
    nodes: [{id, parentId: null, order: 0, kind: "control", runtimeType: "Control", name: "Root", visible: true, locked: false,
      layout: {mode: "free", x: 0, y: 0, width: 1280, height: 720, anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0, pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed"}, runtimeProperties: {}, bindings: {}}],
    theme: [], behaviorGraph: {nodes: [], links: [], groups: []}, behaviorTriggers: [], visualStateGroups: []});
  await json(join(project, "Content/Localization/en.json"), {format: "PrismatiXLocale", schemaRevision: 2, locale: "en", strings: {}});
  if (playable) {
    await write(join(project, "Content/UI/Title.pxui"),
      (await readFile(join(repository, "packages/cli/templates/minimal/Content/UI/Title.pxui"), "utf8"))
        .replaceAll("{{PROJECT_NAME}}", "Release Scale")
        .replaceAll("{{WIDTH}}", "1280").replaceAll("{{HEIGHT}}", "720")
        .replaceAll("{{SCENE_ID}}", "40000000-0000-4000-8000-000000000001")
        .replaceAll("{{ROOT_ID}}", "40000000-0000-4000-8000-000000000002")
        .replaceAll("{{TITLE_ID}}", "40000000-0000-4000-8000-000000000003")
        .replaceAll("{{START_ID}}", "40000000-0000-4000-8000-000000000004")
        .replaceAll("{{LOAD_ID}}", "40000000-0000-4000-8000-000000000005"));
    await json(join(project, "Content/Localization/en.json"), {format: "PrismatiXLocale", schemaRevision: 2,
      locale: "en", strings: {}, fontChain: ["Content/Fonts/NotoSansTC-Bold.ttf"]});
  }
  const sceneCount = Math.min(100, Math.floor(lines / 100)), scenes = [], edges = [];
  let generatedLines = 0;
  for (let i = 0; i < sceneCount; i++) {
    const name = `scene${String(i).padStart(3, "0")}`, source = `Story/${name}.pxstory`;
    scenes.push({id: name, sources: {en: source}});
    const count = Math.floor(lines / sceneCount) + (i < lines % sceneCount ? 1 : 0), text = ["*start", '[choice text="Follow the river" goto=river]', '[choice text="Climb the hill" goto=hill]', "[choice.wait]", "*river"];
    if (playable) text.splice(1, 0,
      `[bg asset=10000000-0000-4000-8000-${String(i % 8 + 1).padStart(12, "0")}]`,
      `[voice Assets/tone-${i % 12}.wav]`);
    for (let j = 0; j < count; j++) {
      if (j === Math.floor(count / 2)) {
        const next = i + 1;
        text.push(next < sceneCount ? `[jump scene=scene${String(next).padStart(3, "0")}]` : "[end]", "*hill");
        if (next < sceneCount) edges.push({from: name, route: "river", to: `scene${String(next).padStart(3, "0")}`});
      }
      text.push(`Sentence ${String(generatedLines++).padStart(6, "0")}: original field notes for route ${j < count / 2 ? "river" : "hill"}, seed ${seed}.`);
    }
    const next = i + 2;
    text.push(next < sceneCount ? `[jump scene=scene${String(next).padStart(3, "0")}]` : "[end]");
    if (next < sceneCount) edges.push({from: name, route: "hill", to: `scene${String(next).padStart(3, "0")}`});
    await write(join(project, source), `${text.join("\n")}\n`);
  }
  await json(join(project, "Story/story.pxindex"), {format: "PrismatiXStoryIndex", schemaRevision: 2, id: "main", entryScene: "scene000",
    chapters: [{id: "scale", title: "Scale", scenes: scenes.map(scene => scene.id)}], scenes});
  await json(join(project, "ASSET_PROVENANCE.json"), {license: "CC0-1.0", origin: "Original deterministic procedural PNG/WAV fixture generated by scripts/release-scale-benchmark.mjs", media,
    ...(playable ? {font: "Noto Sans TC; separately licensed under Content/Fonts/OFL-1.1.txt"} : {})});
  evidence.playableFixture = playable;
  evidence.fixtureInventory = await inventory(project);
  evidence.fixtureStats = {scenes: sceneCount, dialogueLines: generatedLines, branchEdges: edges.length, files: evidence.fixtureInventory.length,
    sourceAndMediaBytes: evidence.fixtureInventory.reduce((sum, file) => sum + file.bytes, 0), mediaBytes: media.reduce((sum, file) => sum + file.bytes, 0), media, edges};
  async function run(label, cli, condition, artifact) {
    const cliSha256 = sha256(await readFile(cli));
    const resourceFile = join(output, `${label}-${condition}-resources.json`), started = performance.now();
    const command = ["--import", pathToFileURL(preload).href, cli, "build", join(project, "project.pxproject"), "--artifact-only", "--output", artifact];
    process.stdout.write(`[scale] ${label} ${condition}: ${generatedLines} lines\n`);
    const child = spawn(process.execPath, command, {cwd: repository, env: {...process.env, PRISMATIX_BENCH_RESOURCE_FILE: resourceFile}, stdio: ["ignore", "pipe", "pipe"]});
    let stdout = "", stderr = "";
    child.stdout.on("data", bytes => { stdout = (stdout + bytes).slice(-128 * 1024); });
    child.stderr.on("data", bytes => { stderr = (stderr + bytes).slice(-128 * 1024); });
    const timeout = setTimeout(() => child.kill(), 10 * 60 * 1000);
    const result = await new Promise((done, fail) => { child.on("error", fail); child.on("close", (code, signal) => done({code, signal})); });
    clearTimeout(timeout);
    let resources = null; try { resources = JSON.parse(await readFile(resourceFile, "utf8")); } catch { /* unavailable on abnormal termination */ }
    const row = {label, condition, cliSha256, command: [process.execPath, ...command], elapsedMs: performance.now() - started, ...result, resources, stdout, stderr};
    evidence.runs.push(row); await json(join(output, "benchmark.json"), evidence);
    assert.equal(result.code, 0, `${label}/${condition} failed: ${stderr}`);
    const artifactInventory = await inventory(artifact);
    row.artifactFiles = artifactInventory;
    const ir = JSON.parse(await readFile(join(artifact, "Runtime/main.pxir"), "utf8"));
    row.runtimeOperations = ir.operations.length;
    row.authoredTextOperations = ir.operations.filter(operation => operation.kind === "narration" || operation.kind === "dialogue").length;
    assert.equal(row.authoredTextOperations, generatedLines, "Runtime IR must contain every authored sentence");
    for (const asset of media) assert.equal(artifactInventory.find(file => file.path === asset.path)?.sha256, asset.sha256, `Media copy differs: ${asset.path}`);
    row.verifiedMediaFiles = media.length;
    process.stdout.write(`[scale] ${label} ${condition}: ${row.elapsedMs.toFixed(1)} ms, peak RSS ${resources?.maxRSSKiB ?? "unavailable"} KiB\n`);
  }
  const edited = join(project, "Story/scene000.pxstory"), original = await readFile(edited, "utf8");
  for (const [label, cli] of [["baseline", baselineCli], ["candidate", candidateCli]]) {
    await write(edited, original);
    const artifact = join(fixture, `${label}-artifact`);
    await run(label, cli, "first-pass", artifact);
    await run(label, cli, "warm-full-build", artifact);
    await write(edited, original.replace("Sentence 000000:", "Revised sentence 000000:"));
    await run(label, cli, "one-line-edit", artifact);
  }
  evidence.outputEquivalence = ["first-pass", "warm-full-build", "one-line-edit"].map(condition => {
    const a = evidence.runs.find(run => run.label === "baseline" && run.condition === condition);
    const b = evidence.runs.find(run => run.label === "candidate" && run.condition === condition);
    const hash = row => row.artifactFiles.find(file => file.path === "Runtime/main.pxir").sha256;
    assert.equal(hash(a), hash(b), `${condition}: baseline and candidate Runtime IR differ`);
    return {condition, runtimeIrSha256: hash(a), identical: true};
  });
  const runtime = option("--runtime");
  if (runtime) {
    const distribution = join(fixture, "distribution");
    const command = [candidateCli, "build", join(project, "project.pxproject"),
      "--output", distribution, "--runtime", resolve(runtime), "--json"];
    const started = performance.now();
    const result = spawnSync(process.execPath, command,
      {cwd: repository, encoding: "utf8", maxBuffer: 1024 * 1024, timeout: 180000});
    evidence.distribution = {command: [process.execPath, ...command],
      elapsedMs: performance.now() - started, status: result.status,
      stdout: result.stdout, stderr: result.stderr, path: distribution};
    assert.equal(result.status, 0, `Native scale distribution failed: ${result.stderr}`);
    evidence.distribution.files = await inventory(distribution);
  }
  evidence.status = "passed";
} catch (error) { evidence.status = "failed"; evidence.error = String(error.stack ?? error); process.exitCode = 1; }
finally {
  evidence.fixtureRetained = args.includes("--keep");
  await json(join(output, "benchmark.json"), evidence);
  if (!evidence.fixtureRetained) await rm(fixture, {recursive: true, force: true});
  process.stdout.write(`${join(output, "benchmark.json")}\n`);
}




