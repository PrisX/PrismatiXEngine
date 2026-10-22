// Packing for other desktop platforms: --platform, `add platform` (registry
// download with integrity, local tarballs and directories), the archives
// foreign distributions ship in and macOS signing from any host.

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {createHash} from "node:crypto";
import {chmod, mkdir, mkdtemp, readFile, readdir, stat, writeFile} from "node:fs/promises";
import {createServer} from "node:http";
import type {AddressInfo} from "node:net";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import test from "node:test";
import {inflateRawSync} from "node:zlib";

import {archiveTree, extractTarGz, safeArchivePath, writeTarGz, writeZip, type ArchiveEntry} from "../src/archive.js";
import {archiveDistribution} from "../src/distribution-archive.js";
import {machOCodeSignature, machOForeignDependencies, machOSlices} from "../src/distribution-layout.js";
import {findRcodesign, signMacAppBundle} from "../src/macos-signing.js";
import {addPlatformRuntime, hostPlatform, integrityMatches, locatePlatformRuntime, parsePlatformList,
  platformCacheDirectory, PlatformRuntimeMissing, registryUrl} from "../src/platform-runtime.js";

const version = (JSON.parse(await readFile(new URL("../../package.json", import.meta.url), "utf8")) as {version: string}).version;
const triples: Record<string, string> = {
  "win32-x64": "x86_64-pc-windows-msvc", "linux-x64": "x86_64-unknown-linux-gnu",
  "darwin-x64": "x86_64-apple-darwin", "darwin-arm64": "aarch64-apple-darwin",
};
// Another Mac's runtime is left out on a Mac, which signs with its own codesign.
const foreign = ["linux-x64", "darwin-arm64", "win32-x64"].filter(platform => platform !== hostPlatform &&
  !(process.platform === "darwin" && platform.startsWith("darwin"))) as ("linux-x64" | "darwin-arm64" | "win32-x64")[];

async function write(path: string, value: string | Buffer): Promise<void> {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, value);
}

/** A minimal arm64 Mach-O executable with a valid ad hoc code signature over its one page. */
function signedMachO(): Buffer {
  const file = Buffer.alloc(4096);
  file.writeUInt32LE(0xfeedfacf, 0);
  file.writeUInt32LE(0x0100000c, 4);
  file.writeUInt32LE(2, 12);
  file.writeUInt32LE(2, 16);
  file.writeUInt32LE(16 + 24, 20);
  // LC_RPATH @executable_path/../Frameworks, then LC_CODE_SIGNATURE.
  file.writeUInt32LE(0x8000001c, 32);
  file.writeUInt32LE(16, 36);
  file.writeUInt32LE(12, 40);
  file.write("@ep\0", 44);
  const identifier = Buffer.from("game\0");
  const directoryLength = 44 + identifier.length + 32;
  const blobLength = 20 + directoryLength;
  file.writeUInt32LE(0x1d, 48);
  file.writeUInt32LE(16, 52);
  file.writeUInt32LE(4096, 56);
  file.writeUInt32LE(blobLength, 60);
  const blob = Buffer.alloc(blobLength);
  blob.writeUInt32BE(0xfade0cc0, 0); blob.writeUInt32BE(blobLength, 4); blob.writeUInt32BE(1, 8);
  blob.writeUInt32BE(0, 12); blob.writeUInt32BE(20, 16);
  const directory = blob.subarray(20);
  directory.writeUInt32BE(0xfade0c02, 0); directory.writeUInt32BE(directoryLength, 4);
  directory.writeUInt32BE(0x20001, 8); directory.writeUInt32BE(0x2, 12);
  directory.writeUInt32BE(44 + identifier.length, 16); directory.writeUInt32BE(44, 20);
  directory.writeUInt32BE(0, 24); directory.writeUInt32BE(1, 28); directory.writeUInt32BE(4096, 32);
  directory[36] = 32; directory[37] = 2; directory[39] = 12;
  identifier.copy(directory, 44);
  createHash("sha256").update(file).digest().copy(directory, 44 + identifier.length);
  return Buffer.concat([file, blob]);
}

function elf(): Buffer {
  const bytes = Buffer.alloc(64);
  bytes.write("\x7fELF", 0, "latin1"); bytes[4] = 2; bytes[5] = 1; bytes.writeUInt16LE(0x3e, 18);
  return bytes;
}

/** A runtime bundle for `platform` as the native package ships it. */
async function writeRuntime(root: string, platform: string, runtimeVersion = version): Promise<void> {
  const suffix = platform.startsWith("win32") ? ".exe" : "";
  await write(join(root, "share", "PrismatiX", "runtime-manifest.json"), JSON.stringify({
    format: "PrismatiXRuntimeManifest", schemaRevision: 1, version: runtimeVersion, target: triples[platform],
    binDir: "../../bin", tools: {previewHost: `PrismatiXPreviewHost${suffix}`, packager: `PrismatiXPackager${suffix}`,
      player: `PrismatiXPlayer${suffix}`}}));
  const player = platform.startsWith("darwin") ? signedMachO() : platform.startsWith("linux") ? elf() : Buffer.from("MZ");
  await write(join(root, "bin", `PrismatiXPlayer${suffix}`), player);
  if (platform.startsWith("linux")) await write(join(root, "bin", "libgame.so.1"), "library");
  await write(join(root, "share", "PrismatiX", "LICENSE"), "MIT");
}

/** A small zip central directory reader: name, Unix mode and contents. */
function readZip(bytes: Buffer): Map<string, {mode: number; data: Buffer}> {
  const end = bytes.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  const count = bytes.readUInt16LE(end + 10);
  let at = bytes.readUInt32LE(end + 16);
  const entries = new Map<string, {mode: number; data: Buffer}>();
  for (let index = 0; index < count; index++) {
    assert.equal(bytes.readUInt32LE(at), 0x02014b50);
    assert.equal(bytes[at + 5], 3, "made by Unix");
    const method = bytes.readUInt16LE(at + 10);
    const compressed = bytes.readUInt32LE(at + 20);
    const nameLength = bytes.readUInt16LE(at + 28);
    const extra = bytes.readUInt16LE(at + 30);
    const comment = bytes.readUInt16LE(at + 32);
    const mode = bytes.readUInt32LE(at + 38) >>> 16;
    const local = bytes.readUInt32LE(at + 42);
    const name = bytes.toString("utf8", at + 46, at + 46 + nameLength);
    const start = local + 30 + bytes.readUInt16LE(local + 26) + bytes.readUInt16LE(local + 28);
    const raw = bytes.subarray(start, start + compressed);
    entries.set(name, {mode, data: method === 8 ? inflateRawSync(raw) : Buffer.from(raw)});
    at += 46 + nameLength + extra + comment;
  }
  return entries;
}

test("--platform values are platforms, comma lists, all or host, without repeats", () => {
  assert.deepEqual(parsePlatformList(["linux-x64,darwin-arm64", "linux-x64"]), ["linux-x64", "darwin-arm64"]);
  assert.deepEqual(parsePlatformList(["all"]), ["win32-x64", "linux-x64", "darwin-x64", "darwin-arm64"]);
  assert.throws(() => parsePlatformList(["linux-arm64"]), /Unknown platform "linux-arm64"/u);
  assert.throws(() => parsePlatformList([""]), /--platform needs a platform/u);
});

test("archives keep Unix modes, links and long names; extraction refuses unsafe paths", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-archive-"));
  const source = join(root, "source");
  const long = `${"deep/".repeat(30)}name.txt`;
  await write(join(source, "Game"), elf());
  await write(join(source, long), "long");
  await write(join(source, "readme.txt"), "hello ".repeat(1000));
  const entries: ArchiveEntry[] = [
    {path: "Game", type: "file", mode: 0o755, size: 64},
    {path: long, type: "file", mode: 0o644, size: 4},
    {path: "readme.txt", type: "file", mode: 0o644, size: 6000},
    {path: "lib/libgame.so", type: "symlink", mode: 0o777, size: 0, target: "libgame.so.1"},
  ];
  const fileOf = (entry: ArchiveEntry) => join(source, ...entry.path.split("/"));
  await writeTarGz(join(root, "game.tar.gz"), {entries, fileOf});
  const extracted = await extractTarGz(join(root, "game.tar.gz"), join(root, "out"));
  assert.deepEqual(extracted.map(entry => [entry.path, entry.type, entry.mode, entry.target]),
    entries.map(entry => [entry.path, entry.type, entry.mode, entry.target]));
  assert.equal(await readFile(join(root, "out", long), "utf8"), "long");
  const zip = readZip(await (async () => { await writeZip(join(root, "game.zip"), {entries, fileOf}); return readFile(join(root, "game.zip")); })());
  assert.equal(zip.get("Game")!.mode, 0o100755);
  assert.equal(zip.get("readme.txt")!.mode, 0o100644);
  assert.equal(zip.get("readme.txt")!.data.toString(), "hello ".repeat(1000), "deflated entries round trip");
  assert.equal(zip.get("lib/libgame.so")!.mode, 0o120777);
  assert.equal(zip.get("lib/libgame.so")!.data.toString(), "libgame.so.1");
  for (const unsafe of ["../escape", "/absolute", "C:/drive", "a/../../b", "back\\slash"]) {
    assert.equal(safeArchivePath(unsafe), undefined, unsafe);
    const archive = join(root, `unsafe-${createHash("sha1").update(unsafe).digest("hex")}.tar.gz`);
    await writeTarGz(archive, {entries: [{path: unsafe, type: "file", mode: 0o644, size: 4}], fileOf: () => join(root, "out", long)});
    await assert.rejects(extractTarGz(archive, join(root, `x-${createHash("sha1").update(unsafe).digest("hex")}`)),
      /unsafe path/u, unsafe);
  }
});

test("a distribution archive marks executables by their contents, leaves out CLI files and adds a top folder", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-distribution-archive-"));
  const output = join(root, "linux-x64");
  await write(join(output, "Game"), elf());
  await write(join(output, "lib", "libgame.so.1"), elf());
  await write(join(output, "Content.pdx"), "data");
  await write(join(output, ".prismatix-output.json"), "{}");
  if (process.platform !== "win32") await chmod(join(output, "Game"), 0o644);
  const archive = await archiveDistribution(output, join(root, "Game-linux-x64.tar.gz"), "linux-x64", "Game");
  assert.deepEqual([...archive.executables].sort(), ["Game", "lib/libgame.so.1"]);
  const entries = await extractTarGz(archive.path, join(root, "extracted"));
  const modes = Object.fromEntries(entries.map(entry => [entry.path, entry.mode]));
  assert.equal(modes["Game/Game"], 0o755, "the Player is executable even when the folder lost the bit");
  assert.equal(modes["Game/Content.pdx"], 0o644);
  assert.equal(modes["Game/.prismatix-output.json"], undefined, "the CLI's marker is not shipped");
  const app = join(root, "darwin-arm64", "Game.app", "Contents");
  await write(join(app, "MacOS", "Game"), signedMachO());
  await write(join(app, "Info.plist"), "<plist/>");
  const zip = readZip(await readFile((await archiveDistribution(join(root, "darwin-arm64"),
    join(root, "Game-darwin-arm64.zip"), "darwin-arm64")).path));
  assert.equal(zip.get("Game.app/Contents/MacOS/Game")!.mode, 0o100755);
  assert.equal(zip.get("Game.app/Contents/Info.plist")!.mode, 0o100644);
  assert.ok(zip.has("Game.app/"), "a macOS archive holds the .app itself");
  assert.deepEqual((await archiveTree(join(root, "darwin-arm64"))).map(entry => entry.path)[0], "Game.app");
});

test("Mach-O load commands and code signatures are read without Apple's tools", () => {
  const bytes = signedMachO();
  const slices = machOSlices(bytes)!;
  assert.deepEqual(slices.map(slice => [slice.cpu, slice.rpaths, slice.signed]), [["arm64", ["@ep"], true]]);
  assert.deepEqual(machOForeignDependencies([{cpu: "arm64", rpaths: [], signed: true,
    dependencies: ["/usr/lib/libSystem.B.dylib", "@rpath/libgame.dylib", "/opt/homebrew/lib/libpng.dylib"]}]),
  ["/opt/homebrew/lib/libpng.dylib"]);
  assert.deepEqual(machOCodeSignature(bytes), {state: "valid", adhoc: true});
  const changed = Buffer.from(bytes);
  changed[2000] = 1;
  assert.equal(machOCodeSignature(changed)!.state, "invalid", "a change after signing is caught");
  assert.equal(machOCodeSignature(elf()), undefined);
});

test("add platform downloads the native package and checks the registry's integrity", async (context) => {
  const platform = foreign[0]!;
  const root = await mkdtemp(join(tmpdir(), "prismatix-add-platform-"));
  const project = join(root, "project");
  await mkdir(project);
  const runtime = join(root, "runtime", "package");
  await writeRuntime(runtime, platform);
  await write(join(runtime, "package.json"), JSON.stringify({name: `@prismatix/native-${platform}`, version}));
  const tree = await archiveTree(join(root, "runtime"));
  await writeTarGz(join(root, "package.tgz"), {entries: tree, fileOf: (entry) => join(root, "runtime", ...entry.path.split("/"))});
  const tarball = await readFile(join(root, "package.tgz"));
  let integrity = `sha512-${createHash("sha512").update(tarball).digest("base64")}`;
  const requests: string[] = [];
  const server = createServer((request, response) => {
    requests.push(request.url ?? "");
    if (request.url === `/@prismatix%2fnative-${platform}/${version}`) {
      const port = (server.address() as AddressInfo).port;
      response.setHeader("content-type", "application/json");
      response.end(JSON.stringify({name: `@prismatix/native-${platform}`, version,
        dist: {tarball: `http://127.0.0.1:${port}/native.tgz`, integrity}}));
    } else if (request.url === "/native.tgz") response.end(tarball);
    else { response.statusCode = 404; response.end(); }
  });
  await new Promise<void>((accept) => server.listen(0, "127.0.0.1", accept));
  context.after(() => server.close());
  const environment = {PRISMATIX_NPM_REGISTRY: `http://127.0.0.1:${(server.address() as AddressInfo).port}/`};
  await assert.rejects(locatePlatformRuntime(project, platform, {environment}), PlatformRuntimeMissing);
  const added = await addPlatformRuntime(project, platform, {environment});
  assert.equal(added.version, version);
  assert.equal(added.integrity, integrity);
  assert.deepEqual(requests, [`/@prismatix%2fnative-${platform}/${version}`, "/native.tgz"]);
  const found = await locatePlatformRuntime(project, platform, {environment});
  assert.equal(found.player, added.player);
  assert.match(found.source, /prismatix add platform/u);
  // A tarball that does not match the registry's integrity installs nothing.
  integrity = `sha512-${createHash("sha512").update("something else").digest("base64")}`;
  const other = await mkdtemp(join(tmpdir(), "prismatix-add-platform-tampered-"));
  await assert.rejects(addPlatformRuntime(other, platform, {environment}), /does not match the integrity/u);
  await assert.rejects(stat(platformCacheDirectory(other, platform)), /ENOENT/u);
  // An installed file that changes afterwards is noticed before packing.
  await writeFile(added.player, Buffer.concat([await readFile(added.player), Buffer.from("x")]));
  await assert.rejects(locatePlatformRuntime(project, platform, {environment}), PlatformRuntimeMissing);
  assert.equal(registryUrl({}), "https://registry.npmjs.org/");
  assert.ok(integrityMatches({sha512: "abc", sha1: "0".repeat(40)}, "sha1-x sha512-abc"));
  assert.ok(!integrityMatches({sha512: "abc", sha1: "0".repeat(40)}, undefined, undefined));
});

test("add platform --from accepts a runtime of exactly this version for that platform only", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-add-from-"));
  const project = join(root, "project");
  await mkdir(project);
  const platform = foreign[0]!;
  await writeRuntime(join(root, "good"), platform);
  await writeRuntime(join(root, "old"), platform, "0.0.1");
  await writeRuntime(join(root, "other"), foreign[1]!);
  await assert.rejects(addPlatformRuntime(project, platform, {from: join(root, "old")}), /not .* this CLI/u);
  await assert.rejects(addPlatformRuntime(project, platform, {from: join(root, "other")}), /runtime target/u);
  const added = await addPlatformRuntime(project, platform, {from: join(root, "good")});
  assert.match(added.source, /^directory /u);
  const stamp = JSON.parse(await readFile(join(platformCacheDirectory(project, platform), "platform.json"), "utf8")) as
    {files: Record<string, {sha256: string}>};
  assert.ok(Object.keys(stamp.files).some(path => path.startsWith("bin/PrismatiXPlayer")), "every file is recorded");
  // A developer directory of runtimes overrides the project's install.
  const developer = join(root, "developer");
  await writeRuntime(join(developer, platform), platform, `${version.split(".").slice(0, 2).join(".")}.0-dev.1`);
  const found = await locatePlatformRuntime(project, platform, {environment: {PRISMATIX_PLATFORM_RUNTIMES: developer}});
  assert.match(found.source, /developer override/u);
});

test("macOS signing uses rcodesign off a Mac, ad hoc or with Developer ID and notarization", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-sign-"));
  const tool = join(root, process.platform === "win32" ? "rcodesign.exe" : "rcodesign");
  await write(tool, "");
  await chmod(tool, 0o755);
  const calls: string[][] = [];
  const passwords: string[] = [];
  const run = async (command: string, args: readonly string[]) => {
    calls.push([command, ...args]);
    const file = args.indexOf("--p12-password-file");
    if (file >= 0) passwords.push(await readFile(args[file + 1]!, "utf8"));
    return {code: 0, stdout: "", stderr: ""};
  };
  const none = await signMacAppBundle("Game.app", run, {environment: {PATH: ""}, label: "dist/Game.app", playerSigned: true});
  assert.equal(none!.signature, "none");
  assert.match(none!.warnings[0]!, /dist\/Game\.app is not signed as a bundle.*rcodesign/su);
  assert.equal(await signMacAppBundle("Game.app", run, {environment: {}, macHost: true}), undefined,
    "a Mac signs ad hoc with codesign");
  assert.equal(await findRcodesign({PATH: root}), tool);
  const adhoc = await signMacAppBundle("Game.app", run, {environment: {PRISMATIX_RCODESIGN: tool}});
  assert.deepEqual([adhoc!.signature, calls.at(-1)], ["adhoc", [tool, "sign", "Game.app"]]);
  const developer = await signMacAppBundle("Game.app", run, {environment: {PRISMATIX_RCODESIGN: tool,
    PRISMATIX_MACOS_P12: "id.p12", PRISMATIX_MACOS_P12_PASSWORD: "secret", PRISMATIX_MACOS_NOTARY_KEY: "key.json"}});
  assert.deepEqual([developer!.signature, developer!.notarized], ["developer-id", true]);
  assert.ok(calls.some(call => call.includes("--p12-file") && call.includes("runtime") && !call.includes("secret")),
    "the password never reaches the command line");
  assert.deepEqual(passwords, ["secret"]);
  assert.deepEqual(calls.at(-1), [tool, "notary-submit", "--api-key-file", "key.json", "--staple", "Game.app"]);
  await assert.rejects(signMacAppBundle("Game.app", run, {environment: {PATH: "", PRISMATIX_MACOS_P12: "id.p12",
    PRISMATIX_MACOS_P12_PASSWORD: "secret"}}), /Developer ID signing uses rcodesign/u);
  await assert.rejects(signMacAppBundle("Game.app", run, {environment: {PRISMATIX_MACOS_NOTARY_KEY: "key.json"}}),
    /needs a Developer ID certificate/u);
});

/** The smallest project `pack` builds (cli.test.ts has the fuller fixture). */
async function project(): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), "prismatix-cross-pack-"));
  const id = "11111111-1111-4111-8111-111111111111";
  const files: Record<string, unknown> = {
    "project.pxproject": {format: "PrismatiXProject", schemaRevision: 1, id: "cross-sample", name: "Cross Sample",
      version: "0.2.0", contentVersion: "1", saveVersion: 1, resolution: {width: 1280, height: 720},
      entry: {story: "main", ui: "title"}, defaultLocale: "en", supportedLocales: ["en"],
      storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame", extensions: [],
      uiEntryPoints: {title: "Content/UI/Title.pxui"}, assets: []},
    "Story/story.pxindex": {format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main", entryScene: "main",
      chapters: [{id: "chapter", title: "Chapter", scenes: ["main"]}], scenes: [{id: "main", sources: {en: "Story/main.pxstory"}}]},
    "Content/game.pxgame": {format: "PrismatiXGame", schemaRevision: 1, variables: [], gallery: [], unlockables: []},
    "Content/UI/Title.pxui": {format: "PrismatiXUIScene", schemaRevision: 1, id, revision: 1, name: "Title",
      width: 1280, height: 720, rootId: id, nodes: [{id, parentId: null, order: 0, kind: "control", runtimeType: "Control",
        name: "Root", visible: true, locked: false, layout: {mode: "free", x: 0, y: 0, width: 1280, height: 720,
          anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0, pivotX: 0, pivotY: 0, margin: 0,
          alignment: "start", sizeRule: "fixed"}, runtimeProperties: {}, bindings: {}}], theme: [],
      behaviorGraph: {nodes: [], links: [], groups: []}, behaviorTriggers: [], visualStateGroups: []},
    "Content/Localization/en.json": {format: "PrismatiXLocale", schemaRevision: 1, locale: "en", strings: {}},
  };
  for (const [path, value] of Object.entries(files)) await write(join(root, path), JSON.stringify(value));
  await write(join(root, "Story/main.pxstory"), "Hello.\n[end]\n");
  return root;
}

test("pack --platform compiles once, packs each platform with its own Player and archives foreign outputs", async () => {
  const root = await project();
  const tools = await mkdtemp(join(tmpdir(), "prismatix-cross-tools-"));
  const requests = join(tools, "requests");
  await mkdir(requests);
  // A Packager stand-in that lays out what it was asked for and records the request.
  const packager = join(tools, "mock-packager.mjs");
  await write(packager, `
    import {copyFile, mkdir, readFile, writeFile} from "node:fs/promises";
    import {basename, join} from "node:path";
    const request = JSON.parse(await readFile(process.argv[3], "utf8"));
    await writeFile(join(${JSON.stringify(requests)}, (request.targetPlatform ?? "host") + ".json"), JSON.stringify(request));
    let player = join(request.outputDir, basename(request.playerExecutable));
    if ((request.targetPlatform ?? "").startsWith("darwin")) {
      await mkdir(join(request.outputDir, "Cross.app", "Contents", "MacOS"), {recursive: true});
      player = join(request.outputDir, "Cross.app", "Contents", "MacOS", basename(request.playerExecutable));
    }
    await copyFile(request.playerExecutable, player);
    await mkdir(join(request.outputDir, "Package"), {recursive: true});
    await writeFile(join(request.outputDir, "Package", "manifest.json"), JSON.stringify({gameId: request.gameId}));
    process.stdout.write(JSON.stringify({protocolVersion: 1, event: "completed", requestId: request.requestId,
      playerExecutable: player}) + "\\n");
  `);
  const hostPlayer = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await write(hostPlayer, "host");
  await chmod(hostPlayer, 0o755);
  const runtimes = join(tools, "runtimes");
  for (const platform of foreign) await writeRuntime(join(runtimes, platform), platform);
  const cli = resolve("dist/src/cli.js");
  const pack = (args: readonly string[], environment: NodeJS.ProcessEnv = {}) =>
    spawnSync(process.execPath, [cli, "pack", root, "--packager", packager, "--player", hostPlayer, ...args, "--json"],
      {encoding: "utf8", env: {...process.env, PRISMATIX_RCODESIGN: "", PATH: "", ...environment}});

  const missing = pack(["--platform", foreign.join(",")]);
  assert.equal(missing.status, 1);
  for (const platform of foreign) assert.match(missing.stderr, new RegExp(`prismatix add platform ${platform}`, "u"));
  assert.match(missing.stderr, /PXCLI1270/u);
  assert.deepEqual(await readdir(requests), [], "nothing is built or packed while a runtime is missing");

  const packed = pack(["--platform", `host,${foreign[0]}`, "--platform", foreign[1]!, "--archive"],
    {PRISMATIX_PLATFORM_RUNTIMES: runtimes});
  assert.equal(packed.status, 0, packed.stderr);
  // The Packager's own events stream first; the CLI's report is the last line.
  const report = JSON.parse(packed.stdout.trim().split(/\r?\n/u).at(-1)!) as {packed: boolean; platforms: {platform: string; output: string;
    archive?: string; signature?: string}[]};
  assert.deepEqual(report.platforms.map(item => item.platform), [hostPlatform, ...foreign]);
  assert.equal((packed.stderr.match(/"phase":"load","current":0/gu) ?? []).length, 1, "the project is loaded and compiled once");
  for (const item of report.platforms) {
    assert.equal(item.output, join(root, "dist", item.platform));
    const request = JSON.parse(await readFile(join(requests, `${item.platform}.json`), "utf8")) as
      {targetPlatform: string; playerExecutable: string; layout?: string};
    assert.equal(request.targetPlatform, item.platform);
    if (item.platform === hostPlatform) assert.equal(request.playerExecutable, hostPlayer);
    else assert.equal(request.playerExecutable, join(runtimes, item.platform, "bin", basenameOf(request.playerExecutable)));
    assert.equal(request.layout, item.platform === "win32-x64" ? "bundled" : undefined, "only Windows is bundled");
    assert.ok(item.archive !== undefined && (await stat(item.archive)).isFile(), `${item.platform} archive`);
  }
  const mac = report.platforms.find(item => item.platform === "darwin-arm64");
  if (mac !== undefined && process.platform !== "darwin") {
    assert.equal(mac.signature, "none");
    assert.match(packed.stderr, /PXCLI1227/u);
    const zip = readZip(await readFile(mac.archive!));
    assert.equal(zip.get("Cross.app/Contents/MacOS/PrismatiXPlayer")!.mode, 0o100755);
  }
  const linux = report.platforms.find(item => item.platform === "linux-x64");
  if (linux !== undefined) {
    const entries = await extractTarGz(linux.archive!, join(tools, "linux-extracted"));
    // A foreign Linux pack holds the platform runtime's Player; on a Linux host
    // the Packager stand-in lays out the host Player given by --player.
    const playerName = linux.platform === hostPlatform ? basenameOf(hostPlayer) : "PrismatiXPlayer";
    assert.equal(entries.find(entry => entry.path === `cross-sample/${playerName}`)?.mode, 0o755);
  }

  const nested = pack(["--platform", foreign[0]!, "--output", join(root, "dist", hostPlatform)],
    {PRISMATIX_PLATFORM_RUNTIMES: runtimes});
  assert.equal(nested.status, 1);
  assert.match(nested.stderr, /PXCLI1271/u, "a platform folder is not reused as the parent of others");

  const web = spawnSync(process.execPath, [cli, "pack", root, "--target", "web", "--platform", "linux-x64"], {encoding: "utf8"});
  assert.notEqual(web.status, 0);
  assert.match(web.stderr, /--platform and --archive apply to desktop packs/u);
});

function basenameOf(path: string): string {
  return path.split(/[\\/]/u).at(-1)!;
}
