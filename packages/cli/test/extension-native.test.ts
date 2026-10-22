// Layer-1 (raw) capability checks and layer-2 native extensions end to end:
// keygen, a signed pack with per-platform native packs, --allow-native,
// per-platform hashes in the lock, a static index with platform downloads
// and filters, and the Web target refusing native extensions.

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {existsSync} from "node:fs";
import {mkdir, mkdtemp, readFile, stat, writeFile} from "node:fs/promises";
import {createServer, type Server} from "node:http";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import test from "node:test";

import {
  nativePackFileName,
  readExtensionNativePack,
  validateExtensionPackage,
  type ExtensionManifest,
} from "@prismatix/authoring-sdk";

const cli = resolve("dist/src/cli.js");

interface Ran {readonly status: number | null; readonly stdout: string; readonly stderr: string}

function run(args: readonly string[], env: NodeJS.ProcessEnv = {}): Promise<Ran> {
  return new Promise((done) => {
    const child = spawn(process.execPath, [cli, ...args], {
      env: {...process.env, PRISMATIX_EXTENSION_REGISTRY: "", PRISMATIX_EXTENSION_SIGNING_KEY: "", CI: "1", ...env},
      stdio: ["ignore", "pipe", "pipe"]});
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8").on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.setEncoding("utf8").on("data", (chunk: string) => { stderr += chunk; });
    child.on("close", (status) => done({status, stdout, stderr}));
  });
}

async function write(path: string, value: string | Uint8Array): Promise<void> {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, value);
}

async function project(name: string): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), `prismatix-native-${name}-`));
  await write(join(root, "prismatix.json"), `{\n  "name": "Native",\n  "id": "native-${name}",\n  "defaultLocale": "en",\n` +
    `  "resolution": {"width": 1280, "height": 720}\n}\n`);
  await write(join(root, "Story/main.pxstory"), "*start\nHello.\n[end]\n");
  return root;
}

async function editManifest(root: string, id: string, change: (manifest: ExtensionManifest) => object): Promise<void> {
  const path = join(root, "Content/Extensions", id, `${id}.pxextension`);
  await writeFile(path, `${JSON.stringify(change(JSON.parse(await readFile(path, "utf8")) as ExtensionManifest), null, 2)}\n`);
}

function serve(directory: string): Promise<{server: Server; url: string}> {
  return new Promise((ready) => {
    const server = createServer(async (request, response) => {
      const path = join(directory, ...decodeURIComponent(new URL(request.url!, "http://x").pathname).split("/").filter(Boolean));
      if (!(await stat(path).catch(() => undefined))?.isFile()) { response.writeHead(404).end(); return; }
      response.writeHead(200, {"content-type": path.endsWith(".json") ? "application/json" : "application/octet-stream"});
      response.end(await readFile(path));
    });
    server.listen(0, "127.0.0.1", () => {
      const address = server.address() as {port: number};
      ready({server, url: `http://127.0.0.1:${address.port}`});
    });
  });
}

test("raw and native host calls without their capability are PXCLIEXT2004 warnings", async () => {
  const root = await project("raw");
  assert.equal((await run(["extension", "create", "fx", "--command", "go", "--project", root, "--json"])).status, 0);
  await write(join(root, "Content/Extensions/fx/main.ts"), [
    "import {createPrismatiXRaw, quadVertices, QUAD_INDICES} from \"@prismatix/runtime/raw\";",
    "import {createPrismatiXNative} from \"@prismatix/runtime/native\";",
    "const raw = createPrismatiXRaw();",
    "const mesh = raw.createMesh(\"fx.quad\", {blend: \"additive\"});",
    "mesh.vertices(quadVertices({x: 0, y: 0, w: 10, h: 10}));",
    "mesh.indices(QUAD_INDICES);",
    "raw.createTarget(\"fx.rt\", 64, 64);",
    "createPrismatiXNative().request(\"fx.service\", \"hi\");",
    "console.log(raw.limits().maxVerticesPerFrame);", ""].join("\n"));
  const validated = await run(["validate", root]);
  assert.equal(validated.status, 0, validated.stderr);
  assert.match(validated.stderr, /PXCLIEXT2004.*raw\.createMesh.*"raw\.mesh"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*mesh\.vertices.*"raw\.mesh"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*raw\.createTarget.*"raw\.target"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*request.*"native"/u);
  assert.doesNotMatch(validated.stderr, /raw\.limits/u, "limits needs no capability");
  await editManifest(root, "fx", (manifest) => ({...manifest, capabilities: [...manifest.capabilities, "raw.mesh", "raw.target"]}));
  const declared = await run(["validate", root]);
  assert.equal(declared.status, 0, declared.stderr);
  assert.doesNotMatch(declared.stderr, /"raw\./u, "declared raw capabilities are not reported");
});

test("native extensions: signed packs, consent, lock hashes, platform resolution and the Web target", async () => {
  const author = await project("author");
  assert.equal((await run(["extension", "create", "glow", "--lang", "js", "--command", "shine", "--project", author])).status, 0);
  await editManifest(author, "glow", (manifest) => ({...manifest, capabilities: [...manifest.capabilities, "native"],
    native: {abi: 1, services: ["glow.fx"], libraries: {
      "win32-x64": {path: "native/win32-x64/glow.dll", files: [{path: "native/win32-x64/helper.dll"}]},
      "linux-x64": {path: "native/linux-x64/libglow.so"}}}}));
  await write(join(author, "Content/Extensions/glow/native/win32-x64/glow.dll"), "MZ not really a library");
  await write(join(author, "Content/Extensions/glow/native/win32-x64/helper.dll"), "MZ helper");

  // A shared native extension must be signed with a key outside the project.
  const keys = await mkdtemp(join(tmpdir(), "prismatix-native-keys-"));
  const key = join(keys, "glow.pem");
  const generated = await run(["extension", "keygen", key, "--json"]);
  assert.equal(generated.status, 0, generated.stderr);
  const {publicKey} = JSON.parse(generated.stdout) as {publicKey: string};
  assert.match(publicKey, /^04[0-9a-f]{128}$/u);
  const out = join(author, "out");
  const unsigned = await run(["extension", "pack", "glow", "--owner", "owen", "--output", out, "--project", author, "--json"]);
  assert.notEqual(unsigned.status, 0);
  assert.match(unsigned.stderr, /--signing-key/u);
  const packed = await run(["extension", "pack", "glow", "--owner", "owen", "--output", out, "--project", author,
    "--signing-key", key, "--json"]);
  assert.equal(packed.status, 0, packed.stderr);
  const result = JSON.parse(packed.stdout) as {file: string; nativePacks: {platform: string; file: string}[]; warnings: {code: string}[]};
  assert.deepEqual(result.nativePacks.map((item) => item.platform), ["win32-x64"]);
  assert.ok(result.warnings.some((item) => item.code === "PXCLIEXT2006"), "the unbuilt linux library is reported");
  const main = await validateExtensionPackage(new Uint8Array(await readFile(result.file)));
  assert.ok(main.valid, JSON.stringify(main.diagnostics));
  assert.equal(main.manifest!.native!.publicKey, publicKey);
  assert.deepEqual(Object.keys(main.manifest!.native!.libraries), ["win32-x64"], "a platform without its library is left out");
  assert.ok(![...main.files.keys()].some((path) => path.startsWith("native/")), "libraries ship only in native packs");
  const pack = await readExtensionNativePack(new Uint8Array(await readFile(result.nativePacks[0]!.file)), main.manifest!, "win32-x64");
  assert.ok(pack.valid, JSON.stringify(pack.diagnostics));
  assert.equal(result.nativePacks[0]!.file, join(out, nativePackFileName("owen/glow", "1.0.0", "win32-x64")));

  // Installing needs --allow-native; the lock pins the native pack and the key.
  const user = await project("user");
  const refused = await run(["extension", "add", result.file, "--yes", "--platform", "win32-x64", "--project", user, "--json"]);
  assert.notEqual(refused.status, 0);
  assert.match(refused.stderr, /--allow-native/u);
  assert.ok(!existsSync(join(user, "Extensions/owen/glow")), "nothing is installed without consent");
  const wrongPlatform = await run(["extension", "add", result.file, "--yes", "--allow-native", "--platform", "linux-x64",
    "--project", user, "--json"]);
  assert.notEqual(wrongPlatform.status, 0);
  assert.match(wrongPlatform.stderr, /no native library for linux-x64/u);
  const inspected = await run(["extension", "inspect", result.file, "--platform", "win32-x64", "--project", user]);
  assert.equal(inspected.status, 0, inspected.stderr);
  assert.match(inspected.stdout, /NATIVE CODE/u);
  const added = await run(["extension", "add", result.file, "--yes", "--allow-native", "--platform", "win32-x64",
    "--project", user, "--json"]);
  assert.equal(added.status, 0, added.stderr);
  assert.ok(existsSync(join(user, "Extensions/owen/glow/native/win32-x64/glow.dll")));
  const lock = JSON.parse(await readFile(join(user, "prismatix-extensions.lock"), "utf8")) as {
    extensions: Record<string, {nativeKey?: string; platforms?: Record<string, {sha256: string; size: number}>}>};
  const entry = lock.extensions["owen/glow"]!;
  assert.match(entry.nativeKey ?? "", /^[0-9a-f]{64}$/u);
  assert.deepEqual(Object.keys(entry.platforms ?? {}), ["win32-x64"]);
  const verified = await run(["extension", "verify", "--project", user, "--json"]);
  assert.equal(verified.status, 0, verified.stdout + verified.stderr);
  await writeFile(join(user, "Extensions/owen/glow/native/win32-x64/glow.dll"), "tampered");
  assert.equal((await run(["extension", "verify", "--project", user, "--json"])).status, 1, "a changed library fails verify");
  await writeFile(join(user, "Extensions/owen/glow/native/win32-x64/glow.dll"), "MZ not really a library");

  // The Web target refuses a native extension before building anything.
  const web = await run(["validate", user, "--target", "web"]);
  assert.equal(web.status, 1);
  assert.match(web.stderr, /PXCLIEXT1101/u);
  const webPack = await run(["pack", user, "--target", "web", "--web-player", join(user, "missing")]);
  assert.equal(webPack.status, 1);
  assert.match(webPack.stderr, /PXCLIEXT1101/u);
  const linux = await run(["validate", user, "--target", "desktop", "--platform", "linux-x64"]);
  assert.equal(linux.status, 1);
  assert.match(linux.stderr, /PXCLIEXT1103/u);

  // A static index publishes per-platform downloads; search and resolve filter by platform.
  const entries = join(author, "entries");
  await write(join(entries, "glow.json"), JSON.stringify({id: "owen/glow", versions: [{url: `../out/${result.file.split(/[\\/]/u).pop()}`}]}));
  const site = join(author, "site");
  const {buildExtensionIndex} = await import(new URL("../../../../scripts/extension-index.mjs", import.meta.url).href) as {
    buildExtensionIndex: (entries: string, out: string, log?: (message: string) => void) => Promise<string[]>};
  await buildExtensionIndex(entries, site, () => {});
  const version = JSON.parse(await readFile(join(site, "v1/extensions/owen/glow/versions/1.0.0/index.json"), "utf8")) as {
    platforms: string[]; platformDownloads: Record<string, {download: string}>};
  assert.deepEqual(version.platforms, ["win32-x64"]);
  assert.ok(version.platformDownloads["win32-x64"] !== undefined);
  const {server, url} = await serve(site);
  try {
    const found = await run(["extension", "search", "glow", "--platform", "win32-x64", "--registry", url, "--json"]);
    assert.equal((JSON.parse(found.stdout) as {total: number}).total, 1, found.stderr);
    const none = await run(["extension", "search", "glow", "--platform", "linux-x64", "--registry", url, "--json"]);
    assert.equal((JSON.parse(none.stdout) as {total: number}).total, 0, none.stderr);
    const other = await project("registry");
    const missing = await run(["extension", "add", "owen/glow", "--registry", url, "--yes", "--allow-native",
      "--platform", "linux-x64", "--project", other, "--json"]);
    assert.notEqual(missing.status, 0);
    assert.match(missing.stderr, /runs on linux-x64|platform/u);
    const installed = await run(["extension", "add", "owen/glow", "--registry", url, "--yes", "--allow-native",
      "--platform", "win32-x64", "--project", other, "--json"]);
    assert.equal(installed.status, 0, installed.stderr);
    assert.ok(existsSync(join(other, "Extensions/owen/glow/native/win32-x64/helper.dll")));
  } finally {
    server.close();
  }
});
