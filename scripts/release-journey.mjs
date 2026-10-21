#!/usr/bin/env node
// The V1 to V2 release journey: one project shipped twice through the public
// CLI, with a save migration between the versions.
//
// The project is PrismatiXEngine/Tests/Fixtures/ReleaseJourney (TSX screens
// for every UI route, zh-TW and ja-JP Story, a gallery CG, a migration) plus
// what this script generates into a work copy: a 520-page Story route, the
// pictures and voice lines (procedural PNG and WAV, original to this script)
// and the repository's Noto fonts with their licence. Version one is derived
// from version two: older variable name, other media, a changed line, no
// migration.
//
//   node scripts/release-journey.mjs [--cli packages/cli/dist/src/cli.js]
//     [--output <dir>]
//     [--packager <PrismatiXPackager> --player <PrismatiXPlayer>
//      --verifier <PrismatiXReleaseJourneyTests>]
//
// Without the native tools it validates and builds both versions and checks
// the artifacts. With them it also packs both versions, writes V1 saves from
// the V1 package, upgrades to the V2 package and verifies the migration
// (PrismatiXEngine/Tests/ReleaseJourneyTests.cpp).

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {createHash} from "node:crypto";
import {copyFile, cp, lstat, mkdir, mkdtemp, readFile, readdir, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, join, relative, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";
import {deflateSync} from "node:zlib";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const fixtureRoot = join(repositoryRoot, "PrismatiXEngine", "Tests", "Fixtures", "ReleaseJourney");

function options(argv) {
  const result = new Map();
  for (let index = 0; index < argv.length; index += 1) {
    const key = argv[index];
    if (!key.startsWith("--")) throw new Error(`Unexpected argument: ${key}`);
    const value = argv[index + 1];
    if (value === undefined || value.startsWith("--")) throw new Error(`${key} requires a value`);
    result.set(key.slice(2), value);
    index += 1;
  }
  return result;
}

function run(command, args, cwd = repositoryRoot) {
  const result = spawnSync(command, args, {cwd, encoding: "utf8", stdio: "pipe", windowsHide: true});
  if (result.status !== 0) {
    process.stderr.write(result.stdout ?? "");
    process.stderr.write(result.stderr ?? "");
    throw new Error(`${basename(command)} ${args.join(" ")} failed with ${result.status ?? result.signal}`);
  }
  return result;
}

const runCli = (cli, args) => run(process.execPath, [cli, ...args]);
const readJson = async (path) => JSON.parse(await readFile(path, "utf8"));
const writeJson = (path, value) => writeFile(path, `${JSON.stringify(value, null, 2)}\n`, "utf8");

async function walk(root) {
  const result = [];
  for (const entry of await readdir(root, {withFileTypes: true})) {
    const path = join(root, entry.name);
    if (entry.isDirectory()) result.push(...await walk(path));
    else if (entry.isFile()) result.push(path);
  }
  return result;
}

// ---- Generated media: original to this script, no outside source. ----

const crcTable = Array.from({length: 256}, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (bytes) => {
  let c = 0xffffffff;
  for (const byte of bytes) c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
function chunk(type, data) {
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const length = Buffer.alloc(4); length.writeUInt32BE(data.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body));
  return Buffer.concat([length, body, crc]);
}
/** An RGB PNG whose pixel (x, y) is `color(x / width, y / height)`. */
function png(width, height, color) {
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const row = y * (width * 3 + 1);
    for (let x = 0; x < width; x += 1) {
      const [r, g, b] = color(x / width, y / height);
      raw[row + 1 + x * 3] = r; raw[row + 2 + x * 3] = g; raw[row + 3 + x * 3] = b;
    }
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0); header.writeUInt32BE(height, 4);
  header[8] = 8; header[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", header), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const mix = (a, b, t) => a.map((value, index) => Math.round(value + (b[index] - value) * t));
/** A 16-bit mono WAV: a decaying tone at each of `notes` (Hz), one after another. */
function wav(notes, seconds = 0.4, rate = 22050) {
  const perNote = Math.floor(rate * seconds);
  const samples = perNote * notes.length;
  const data = Buffer.alloc(44 + samples * 2);
  data.write("RIFF", 0); data.writeUInt32LE(36 + samples * 2, 4); data.write("WAVE", 8);
  data.write("fmt ", 12); data.writeUInt32LE(16, 16); data.writeUInt16LE(1, 20); data.writeUInt16LE(1, 22);
  data.writeUInt32LE(rate, 24); data.writeUInt32LE(rate * 2, 28); data.writeUInt16LE(2, 32);
  data.writeUInt16LE(16, 34); data.write("data", 36); data.writeUInt32LE(samples * 2, 40);
  for (let i = 0; i < samples; i += 1) {
    const note = notes[Math.floor(i / perNote)];
    const t = (i % perNote) / rate;
    const value = Math.sin(2 * Math.PI * note * t) * Math.exp(-4 * t) * 9000;
    data.writeInt16LE(Math.round(value), 44 + i * 2);
  }
  return data;
}

const pictures = {
  "Assets/evening-room.png": () => png(1280, 720, (x, y) =>
    y > 0.72 ? mix([46, 38, 52], [30, 24, 36], (y - 0.72) / 0.28)
      : mix([58, 74, 128], [236, 150, 110], y / 0.72).map((value, index) =>
        x > 0.12 && x < 0.38 && y > 0.18 && y < 0.6 ? Math.min(255, value + [40, 34, 10][index]) : value)),
  "Assets/ending-cg.png": () => png(1280, 720, (x, y) => {
    const moon = Math.hypot(x - 0.76, (y - 0.26) * 0.5625) < 0.06;
    if (moon) return [255, 240, 200];
    return y > 0.7 ? [16, 23, 40] : mix([25, 38, 77], [239, 141, 123], y / 0.7);
  }),
};
const sounds = {
  "Assets/chime.wav": () => wav([880, 1320]),
  "Assets/Locales/zh-TW/greeting-v2.wav": () => wav([440, 554, 659]),
  "Assets/Locales/ja-JP/greeting-v2.wav": () => wav([523, 659, 784]),
};

// ---- Generated Story: identical structure and ids in both locales. ----

const lines = {
  "zh-TW": {
    openingV2: "傍晚的房間掛上了新的星空畫。窗外的街燈一盞一盞亮起。",
    openingV1: "傍晚的房間裡，兩個書包並排靠著沙發。窗外的街燈一盞一盞亮起。",
    greeting: "歡迎回來。今晚要怎麼度過？",
    prompt: "要做什麼呢？",
    walk: "到河邊散步", long: "翻開五百頁的長夜筆記",
    walked: "沿著河邊走了一圈，風很涼。",
    page: (n) => `長夜筆記第 ${n} 頁：今天也記下了一件小事。`,
    ending: "夜深了，今晚就寫到這裡。",
  },
  "ja-JP": {
    openingV2: "夕暮れの部屋には新しい星空の絵が掛けられた。窓の外で街灯がひとつずつ灯る。",
    openingV1: "夕暮れの部屋では、二つの鞄がソファのそばに並んでいた。窓の外で街灯がひとつずつ灯る。",
    greeting: "おかえりなさい。今夜はどう過ごしますか？",
    prompt: "何をしようか？",
    walk: "川辺を散歩する", long: "五百ページの夜のノートを開く",
    walked: "川辺をひと回りした。風が涼しい。",
    page: (n) => `夜のノート ${n} ページ目：今日も小さな出来事を書き留めた。`,
    ending: "夜が更けた。今夜はここまで。",
  },
};
const pages = 520;

function story(locale, version) {
  const text = lines[locale];
  const choice = version === 1 ? "walkChoice" : "eveningChoice";
  const out = [];
  const op = (id, line) => out.push(`[id main.${id}]`, line);
  op("start", "*start");
  op("bg", "[bg asset=journey.backdrop]");
  op("chime", "[se asset=journey.chime]");
  op("opening", version === 1 ? text.openingV1 : text.openingV2);
  out.push("@guide");
  op("voice", "[voice asset=journey.greeting]");
  op("greeting", text.greeting);
  out.push("");
  op("prompt", text.prompt);
  op("choice.walk", `[choice text="${text.walk}" goto=walk]`);
  op("choice.long", `[choice text="${text.long}" goto=long-night]`);
  out.push("[choice.wait]", "");
  op("walk", "*walk");
  op("walk.set", `[set ${choice}=walk]`);
  op("walk.line", text.walked);
  op("walk.jump", "[jump target=ending]");
  out.push("");
  op("long", "*long-night");
  op("long.set", `[set ${choice}=long-night]`);
  for (let page = 1; page <= pages; page += 1) op(`long.page.${page}`, text.page(page));
  op("long.jump", "[jump target=ending]");
  out.push("");
  op("ending", "*ending");
  op("ending.set", "[set endingUnlocked=true]");
  op("ending.line", text.ending);
  op("end", "[end]");
  return `${out.join("\n")}\n`;
}

const provenance = `# Release journey fixture provenance

Everything in this project is original to the PrismatiX test suite and is
covered by the repository's MIT license, except the fonts.

- The JSON documents, the TSX screens and the character come from
  \`PrismatiXEngine/Tests/Fixtures/ReleaseJourney\`.
- \`Story/*/main.pxstory\`, the PNG pictures and the WAV tones were generated
  by \`scripts/release-journey.mjs\` (gradients and decaying sine tones).
- \`Content/Fonts/NotoSansTC-Bold.ttf\` and \`NotoSerifJP-Bold.ttf\` are Noto
  Sans TC Bold and Noto Serif JP Bold from the repository's
  \`Resources/Fonts\`, under the SIL Open Font License 1.1
  (\`Content/Fonts/OFL-1.1.txt\`).

Every asset is packaged; no asset is read from the repository after packaging.
`;

async function prepare(root, version) {
  await cp(fixtureRoot, root, {recursive: true});
  for (const [path, make] of Object.entries({...pictures, ...sounds})) {
    await mkdir(dirname(join(root, path)), {recursive: true});
    await writeFile(join(root, path), make());
  }
  await mkdir(join(root, "Content/Fonts"), {recursive: true});
  for (const name of ["NotoSansTC-Bold.ttf", "NotoSerifJP-Bold.ttf", "OFL-1.1.txt"])
    await copyFile(join(repositoryRoot, "Resources/Fonts", name), join(root, "Content/Fonts", name));
  await writeFile(join(root, "ASSET_PROVENANCE.md"), provenance, "utf8");
  for (const locale of Object.keys(lines)) {
    await mkdir(join(root, "Story", locale), {recursive: true});
    await writeFile(join(root, "Story", locale, "main.pxstory"), story(locale, version), "utf8");
  }
  if (version === 2) return;

  const projectPath = join(root, "project.pxproject");
  const project = await readJson(projectPath);
  project.version = "1.0.0";
  project.contentVersion = "journey-v1";
  project.saveVersion = 1;
  delete project.saveMigrations;
  project.settings = {...project.settings, "fixture.releaseJourney": "v1"};
  await writeJson(projectPath, project);
  await rm(join(root, "Content/Migrations"), {recursive: true, force: true});

  const gamePath = join(root, "Content/game.pxgame");
  const game = await readJson(gamePath);
  game.variables = game.variables
    .filter((variable) => variable.name !== "releaseJourneyVersion")
    .map((variable) => variable.name === "eveningChoice"
      ? {...variable, name: "walkChoice", description: "The evening route"} : variable);
  await writeJson(gamePath, game);

  // Version one shipped other pictures and one voice line for both locales.
  const v1Resources = {
    "zh-TW": {"journey.backdrop": "Assets/evening-room.png"},
    "ja-JP": {"journey.backdrop": "Assets/ending-cg.png"},
  };
  for (const locale of Object.keys(lines)) {
    const path = join(root, `Content/Localization/${locale}.json`);
    const document = await readJson(path);
    document.resources = {...document.resources, ...v1Resources[locale], "journey.greeting": "Assets/chime.wav"};
    document.strings["journey.release"] = "第一版";
    await writeJson(path, document);
  }
  await rm(join(root, "Assets/Locales"), {recursive: true, force: true});
}

// ---- Checks on the author artifacts. ----

const topology = (document) => document.operations.map(({kind, operationId, sourceId}) => ({kind, operationId, sourceId}));
const assets = (document, kind) => document.operations
  .filter((operation) => operation.kind === kind).map((operation) => operation.arguments.asset);

async function assertMedia(path, signature) {
  const bytes = await readFile(path);
  assert.ok(bytes.length > 44, `${path} must contain real media bytes`);
  assert.deepEqual([...bytes.subarray(0, signature.length)], [...signature], `${path} has an unexpected media signature`);
}

async function verifyAuthorArtifacts(v1, v2, sources) {
  const v1Project = await readJson(join(v1, "project.pxproject"));
  const v2Project = await readJson(join(v2, "project.pxproject"));
  assert.equal(v1Project.contentVersion, "journey-v1");
  assert.equal(v1Project.saveVersion, 1);
  assert.equal(v2Project.contentVersion, "journey-v2");
  assert.equal(v2Project.saveVersion, 2);
  assert.equal(v2Project.saveMigrations[0].asset, "Content/Migrations/journey-v1-to-v2.pxsave-migration");
  assert.doesNotMatch(JSON.stringify(v2Project), /\.(?:jsx|tsx)/u, "published UI routes must be canonical .pxui");
  for (const name of ["Title", "HUD", "Gallery", "Backlog", "Save", "Load"])
    assert.match(await readFile(join(v2, `Content/UI/${name}.pxui`), "utf8"), /"format": "PrismatiXUIScene"/u,
      `${name} must be built from TSX into a canonical UI scene`);

  const localeIr = (root, locale) => readJson(join(root, `Runtime/Locales/${locale}/main.pxir`));
  const [v1Zh, v1Ja, v2Zh, v2Ja] = await Promise.all([
    localeIr(v1, "zh-TW"), localeIr(v1, "ja-JP"), localeIr(v2, "zh-TW"), localeIr(v2, "ja-JP")]);
  assert.deepEqual(topology(v1Ja), topology(v1Zh), "V1 localized programs must keep one execution topology");
  assert.deepEqual(topology(v2Ja), topology(v2Zh), "V2 localized programs must keep one execution topology");
  assert.deepEqual(topology(v2Zh), topology(v1Zh), "the content upgrade must retain stable operation/source identity");
  assert.ok(v2Zh.operations.filter(({kind}) => kind === "narration" || kind === "dialogue").length >= 521,
    "the long route must provide a 501+ entry backlog journey");
  assert.notDeepEqual(assets(v2Zh, "background"), assets(v2Ja, "background"),
    "locale switching must select a different image resource");
  assert.notDeepEqual(assets(v2Zh, "voice"), assets(v2Ja, "voice"),
    "locale switching must select a different voice resource");
  assert.notDeepEqual(assets(v1Zh, "background"), assets(v2Zh, "background"), "V2 must ship a changed image asset");
  assert.notDeepEqual(assets(v1Zh, "voice"), assets(v2Zh, "voice"), "V2 must ship a changed voice asset");
  const opening = (document) => document.operations.find(({kind}) => kind === "narration")?.text;
  assert.notEqual(opening(v1Zh), opening(v2Zh), "V2 must ship a changed dialogue line");

  for (const path of ["Assets/ending-cg.png", "Assets/evening-room.png"])
    await assertMedia(join(v2, path), Buffer.from([0x89, 0x50, 0x4e, 0x47]));
  for (const path of ["Assets/Locales/zh-TW/greeting-v2.wav", "Assets/Locales/ja-JP/greeting-v2.wav"])
    await assertMedia(join(v2, path), Buffer.from("RIFF", "ascii"));
  assert.match(await readFile(join(v2, "ASSET_PROVENANCE.md"), "utf8"),
    /no asset is read from the repository after packaging/u);
  assert.match(await readFile(join(v2, "Content/Fonts/OFL-1.1.txt"), "utf8"), /SIL OPEN FONT LICENSE Version 1\.1/u);

  const files = await walk(v2);
  assert.ok(files.every((path) => !/\.(?:jsx|tsx)$/u.test(path)), "release artifacts must not contain TSX authoring sources");
  const roots = [repositoryRoot, ...sources].map((root) => root.split(sep).join("/"));
  for (const path of files.filter((value) => /\.(?:json|pxgame|pxir|pxmap|pxproject|pxstory|pxui)$/u.test(value))) {
    const text = (await readFile(path, "utf8")).split(sep).join("/");
    for (const root of roots)
      assert.ok(!text.includes(root), `release artifact ${relative(v2, path)} leaked a source path`);
  }
}

const args = options(process.argv.slice(2));
const isFile = async (path) => (await lstat(path).catch(() => undefined))?.isFile() === true;
// Where a packed game keeps its data: beside the executable (flat, Linux),
// in the Windows engine folder (default "bundled" layout) or in a macOS
// .app's Contents/Resources.
async function packageDataRoot(output) {
  if (await isFile(join(output, "Package", "manifest.json"))) return output;
  for (const entry of await readdir(output)) {
    const candidate = entry.endsWith(".app") ? join(output, entry, "Contents", "Resources") : join(output, entry);
    if (await isFile(join(candidate, "Package", "manifest.json"))) return candidate;
  }
  return output;
}

const cli = resolve(args.get("cli") ?? join(repositoryRoot, "packages/cli/dist/src/cli.js"));
const explicitOutput = args.get("output");
const workRoot = explicitOutput === undefined
  ? await mkdtemp(join(tmpdir(), "prismatix-release-journey-"))
  : resolve(explicitOutput);
await mkdir(workRoot, {recursive: true});
try {
  const v1Source = join(workRoot, "source-v1");
  const v2Source = join(workRoot, "source-v2");
  await prepare(v1Source, 1);
  await prepare(v2Source, 2);

  const authorV1 = join(workRoot, "author-v1");
  const authorV2 = join(workRoot, "author-v2");
  for (const [source, output] of [[v1Source, authorV1], [v2Source, authorV2]]) {
    const project = join(source, "project.pxproject");
    runCli(cli, ["validate", project]);
    runCli(cli, ["build", project, "--artifact-only", "--output", output]);
  }
  await verifyAuthorArtifacts(authorV1, authorV2, [v1Source, v2Source]);

  const packager = args.get("packager");
  const player = args.get("player");
  const verifier = args.get("verifier");
  let packaged = false;
  if ([packager, player, verifier].some((value) => value !== undefined)) {
    assert.ok(packager && player && verifier, "--packager, --player, and --verifier must be provided together");
    const releaseV1 = join(workRoot, "package-v1");
    const releaseV2 = join(workRoot, "package-v2");
    runCli(cli, ["pack", join(v1Source, "project.pxproject"), "--packager", resolve(packager),
      "--player", resolve(player), "--compression", "fast", "--output", releaseV1]);
    // The verifier works on the game data, wherever the layout puts it.
    const dataV1 = await packageDataRoot(releaseV1);
    run(resolve(verifier), ["write-v1", dataV1]);
    const saveV1 = join(dataV1, "Save");
    const originalSaves = new Map(await Promise.all((await walk(saveV1)).map(async (path) =>
      [relative(saveV1, path), createHash("sha256").update(await readFile(path)).digest("hex")])));
    runCli(cli, ["pack", join(v2Source, "project.pxproject"), "--packager", resolve(packager),
      "--player", resolve(player), "--compression", "fast", "--output", releaseV2]);
    // This copy is limited to the isolated journey fixture. Never build over a
    // played distribution: its saves are player-owned, not compiler output.
    const dataV2 = await packageDataRoot(releaseV2);
    await cp(saveV1, join(dataV2, "Save"), {recursive: true, errorOnExist: true, force: false});
    run(resolve(verifier), ["verify-v2", dataV2]);
    for (const [path, digest] of originalSaves)
      assert.equal(createHash("sha256").update(await readFile(join(saveV1, path))).digest("hex"), digest,
        "V2 migration must leave the original V1 fixture saves unchanged");
    packaged = true;
  }

  const digest = createHash("sha256").update(await readFile(join(authorV2, "project.pxproject"))).digest("hex").slice(0, 16);
  process.stdout.write(`Release journey passed (${digest}${packaged ? ", packaged V1 to V2 save migration" : ""}) at ${workRoot}\n`);
} finally {
  if (explicitOutput === undefined) await rm(workRoot, {recursive: true, force: true, maxRetries: 5, retryDelay: 100});
}
