// Layer-1 (raw) capability checks and layer-2 native extensions end to end:
// keygen, a signed pack with per-platform native packs, --allow-native,
// per-platform hashes in the lock, a static index with platform downloads
// and filters, the Web target refusing native extensions, and the files a
// game's maker supplies (userFiles) beside the libraries in Plugins/.

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {createHash} from "node:crypto";
import {existsSync} from "node:fs";
import {mkdir, mkdtemp, readdir, readFile, stat, writeFile} from "node:fs/promises";
import {createServer, type Server} from "node:http";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import test from "node:test";

import {isAndroidAsset} from "../src/android-export.js";
import {parseNativePlatformList, resolveNativeUserFiles, stageNativePlugins} from "../src/extension-native.js";
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
    "import {toBase64} from \"@prismatix/runtime/text\";",
    "const raw = createPrismatiXRaw();",
    "const mesh = raw.createMesh(\"fx.quad\", {blend: \"additive\"});",
    "mesh.vertices(quadVertices({x: 0, y: 0, w: 10, h: 10}));",
    "mesh.indices(QUAD_INDICES);",
    "mesh.geometry(quadVertices({x: 0, y: 0, w: 10, h: 10}), QUAD_INDICES);",
    "raw.upload([{mesh: \"fx.quad\", indices: QUAD_INDICES}]);",
    "raw.createTarget(\"fx.rt\", 64, 64);",
    "console.log(toBase64(new Uint8Array([1, 2, 3])));",
    "createPrismatiXNative().request(\"fx.service\", \"hi\");",
    "console.log(raw.limits().maxVerticesPerFrame);", ""].join("\n"));
  const validated = await run(["validate", root]);
  assert.equal(validated.status, 0, validated.stderr);
  assert.match(validated.stderr, /PXCLIEXT2004.*raw\.createMesh.*"raw\.mesh"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*mesh\.vertices.*"raw\.mesh"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*mesh\.geometry.*"raw\.mesh"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*raw\.upload.*"raw\.mesh"/u);
  assert.doesNotMatch(validated.stderr, /PXCLIEXT1001/u, "the /text entry and native's import of it bundle");
  assert.match(validated.stderr, /PXCLIEXT2004.*raw\.createTarget.*"raw\.target"/u);
  assert.match(validated.stderr, /PXCLIEXT2004.*request.*"native"/u);
  assert.doesNotMatch(validated.stderr, /raw\.limits/u, "limits needs no capability");
  await editManifest(root, "fx", (manifest) => ({...manifest, capabilities: [...manifest.capabilities ?? [], "raw.mesh", "raw.target"]}));
  const declared = await run(["validate", root]);
  assert.equal(declared.status, 0, declared.stderr);
  assert.doesNotMatch(declared.stderr, /"raw\./u, "declared raw capabilities are not reported");
});

test("native extensions: signed packs, consent, lock hashes, platform resolution and the Web target", async () => {
  const author = await project("author");
  assert.equal((await run(["extension", "create", "glow", "--lang", "js", "--command", "shine", "--project", author])).status, 0);
  await editManifest(author, "glow", (manifest) => ({...manifest, capabilities: [...manifest.capabilities ?? [], "native"],
    native: {abi: 1, services: ["glow.fx"], libraries: {
      "win32-x64": {path: "native/win32-x64/glow.dll", files: [{path: "native/win32-x64/helper.dll"}],
        userFiles: [{name: "Core.dll", hint: "Get it from the vendor's SDK."}]},
      "linux-x64": {path: "native/linux-x64/libglow.so"}}}}));
  await write(join(author, "Content/Extensions/glow/native/win32-x64/glow.dll"), "MZ not really a library");
  // The author's own copy of the user-supplied file never ships.
  await write(join(author, "Content/Extensions/glow/native/win32-x64/Core.dll"), "vendor runtime");
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
  assert.deepEqual(main.manifest!.native!.libraries["win32-x64"]!.userFiles, [{name: "Core.dll", hint: "Get it from the vendor's SDK."}]);
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
  assert.match(inspected.stdout, /supply these files yourself[\s\S]*Core\.dll \(win32-x64\): Get it from the vendor's SDK\./u);
  const inspectedJson = await run(["extension", "inspect", result.file, "--project", user, "--json"]);
  assert.deepEqual((JSON.parse(inspectedJson.stdout) as {native: {userFiles: unknown}}).native.userFiles,
    [{platform: "win32-x64", name: "Core.dll", hint: "Get it from the vendor's SDK."}]);
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

  // Packing for Windows needs the file the maker supplies.
  const unsupplied = await run(["validate", user, "--target", "desktop", "--platform", "win32-x64"]);
  assert.equal(unsupplied.status, 1);
  assert.match(unsupplied.stderr, /PXCLIEXT1106[\s\S]*Core\.dll[\s\S]*Get it from the vendor's SDK\./u);
  await write(join(user, "NativeUserFiles/win32-x64/Core.dll"), "vendor runtime");
  const supplied = await run(["validate", user, "--target", "desktop", "--platform", "win32-x64"]);
  assert.equal(supplied.status, 0, supplied.stderr);

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

test("Android: android-arm64 native packs, --platform android-arm64, and the APK's Plugins assets", async () => {
  assert.deepEqual(parseNativePlatformList("android-arm64,win32-x64"), ["android-arm64", "win32-x64"]);
  assert.deepEqual(parseNativePlatformList("all"), ["win32-x64", "linux-x64", "darwin-x64", "darwin-arm64", "android-arm64"]);
  assert.deepEqual(parseNativePlatformList("host", "linux-x64"), ["linux-x64"]);
  assert.throws(() => parseNativePlatformList("android-x64"), /Unknown platform "android-x64"/u);
  assert.equal(isAndroidAsset("Plugins/glow/libglow.so", "Player.exe"), true, "native files ride in assets/");
  assert.equal(isAndroidAsset("Plugins/.prismatix-managed.json", "Player.exe"), false);

  const author = await project("android-author");
  assert.equal((await run(["extension", "create", "glow", "--lang", "js", "--command", "shine", "--project", author])).status, 0);
  await editManifest(author, "glow", (manifest) => ({...manifest, capabilities: [...manifest.capabilities, "native"],
    native: {abi: 1, services: ["glow.fx"], libraries: {
      "android-arm64": {path: "native/android-arm64/libglow.so", files: [{path: "native/android-arm64/libhelper.so"}],
        userFiles: [{name: "libcore.so", hint: "From the vendor's Android SDK."}]}}}}));
  await write(join(author, "Content/Extensions/glow/native/android-arm64/libglow.so"), "\x7fELF glow");
  await write(join(author, "Content/Extensions/glow/native/android-arm64/libhelper.so"), "\x7fELF helper");
  const keys = await mkdtemp(join(tmpdir(), "prismatix-native-keys-"));
  const key = join(keys, "glow.pem");
  assert.equal((await run(["extension", "keygen", key])).status, 0);
  const packed = await run(["extension", "pack", "glow", "--owner", "owen", "--output", join(author, "out"), "--project", author,
    "--signing-key", key, "--json"]);
  assert.equal(packed.status, 0, packed.stderr);
  const result = JSON.parse(packed.stdout) as {file: string; nativePacks: {platform: string; file: string}[]};
  assert.deepEqual(result.nativePacks.map((item) => item.platform), ["android-arm64"]);
  assert.equal(result.nativePacks[0]!.file, join(author, "out", nativePackFileName("owen/glow", "1.0.0", "android-arm64")));

  // Installing on a Windows, macOS or Linux computer fetches the Android pack only when asked.
  const user = await project("android-user");
  const added = await run(["extension", "add", result.file, "--yes", "--allow-native", "--platform", "android-arm64",
    "--project", user, "--json"]);
  assert.equal(added.status, 0, added.stderr);
  assert.ok(existsSync(join(user, "Extensions/owen/glow/native/android-arm64/libglow.so")));
  const lock = JSON.parse(await readFile(join(user, "prismatix-extensions.lock"), "utf8")) as {
    extensions: Record<string, {platforms?: Record<string, unknown>}>};
  assert.deepEqual(Object.keys(lock.extensions["owen/glow"]!.platforms ?? {}), ["android-arm64"]);
  const desktop = await run(["validate", user, "--target", "desktop", "--platform", "win32-x64"]);
  assert.equal(desktop.status, 1);
  assert.match(desktop.stderr, /PXCLIEXT1103/u);

  // The Android target checks the user files like a desktop one.
  const unsupplied = await run(["validate", user, "--target", "android"]);
  assert.equal(unsupplied.status, 1);
  assert.match(unsupplied.stderr, /PXCLIEXT1106[\s\S]*libcore\.so[\s\S]*From the vendor's Android SDK\./u);
  await write(join(user, "NativeUserFiles/android-arm64/libcore.so"), "\x7fELF core");
  const supplied = await run(["validate", user, "--target", "android"]);
  assert.equal(supplied.status, 0, supplied.stderr);

  // pack --target android: the package's Plugins/ folder becomes assets/Plugins/.
  const tools = await mkdtemp(join(tmpdir(), "prismatix-native-android-"));
  const packager = join(tools, "mock-packager.mjs");
  await write(packager, [
    "import {copyFile, mkdir, readFile, writeFile} from \"node:fs/promises\";",
    "import {basename, join} from \"node:path\";",
    "const request = JSON.parse(await readFile(process.argv[3], \"utf8\"));",
    "const out = request.outputDir;",
    "await mkdir(join(out, \"Package\"), {recursive: true});",
    "await writeFile(join(out, \"Package\", \"manifest.json\"), JSON.stringify({gameId: request.gameId,",
    "  title: request.title, resolution: {width: request.width, height: request.height}}));",
    "await copyFile(request.playerExecutable, join(out, basename(request.playerExecutable)));",
    "process.stdout.write(JSON.stringify({protocolVersion: 1, event: \"completed\", requestId: request.requestId}) + \"\\n\");", ""].join("\n"));
  const player = join(tools, process.platform === "win32" ? "Player.exe" : "Player");
  await write(player, "desktop player");
  const gradle = join(tools, "mock-gradle.mjs");
  await write(gradle, [
    "import {mkdir, readdir, writeFile} from \"node:fs/promises\";",
    "import {join, relative} from \"node:path\";",
    "const args = process.argv.slice(2);",
    "const project = args[args.indexOf(\"-p\") + 1];",
    "const main = join(project, \"app\", \"src\", \"main\");",
    "const list = async (directory) => (await readdir(directory, {recursive: true, withFileTypes: true}))",
    "  .filter((entry) => entry.isFile())",
    "  .map((entry) => relative(directory, join(entry.parentPath, entry.name)).split(\"\\\\\").join(\"/\")).sort();",
    "await writeFile(process.env.PX_GRADLE_RECORD, JSON.stringify({jni: await list(join(main, \"jniLibs\", \"arm64-v8a\")),",
    "  assets: await list(join(main, \"assets\"))}));",
    "await mkdir(join(project, \"app\", \"build\", \"outputs\", \"apk\", \"debug\"), {recursive: true});",
    "await writeFile(join(project, \"app\", \"build\", \"outputs\", \"apk\", \"debug\", \"app-debug.apk\"), \"apk\");", ""].join("\n"));
  const arm64 = Buffer.alloc(64);
  arm64.writeUInt32BE(0x7f454c46, 0);
  arm64[4] = 2; arm64[5] = 1;
  arm64.writeUInt16LE(183, 18);
  await write(join(tools, "android", "libmain.so"), arm64);
  await mkdir(join(tools, "sdk", "platforms"), {recursive: true});
  const record = join(tools, "gradle-record.json");
  const projectPath = join(user, "prismatix.json");
  const manifest = JSON.parse(await readFile(projectPath, "utf8")) as Record<string, unknown>;
  await writeFile(projectPath, JSON.stringify({...manifest, distribution: {android: {applicationId: "com.example.glow"}}}));
  const android = await run(["pack", user, "--target", "android", "--packager", packager, "--player", player,
    "--android-player", join(tools, "android"), "--gradle", gradle, "--android-sdk", join(tools, "sdk"),
    "--output", join(tools, "out"), "--json"], {PX_GRADLE_RECORD: record});
  assert.equal(android.status, 0, android.stderr);
  assert.match(android.stderr, /Copied 3 native extension file\(s\) for android-arm64/u);
  const seen = JSON.parse(await readFile(record, "utf8")) as {jni: string[]; assets: string[]};
  assert.deepEqual(seen.jni, ["libmain.so"], "extension libraries are not in lib/arm64-v8a/");
  assert.deepEqual(seen.assets.filter((path) => path.startsWith("Plugins/")),
    ["Plugins/glow/libcore.so", "Plugins/glow/libglow.so", "Plugins/glow/libhelper.so"]);
});

test("user-supplied native files: lookup order, allow list, and Plugins staging that keeps files put there by hand", async () => {
  const root = await project("user-files");
  const library = {path: "native/win32-x64/glow.dll", userFiles: [{name: "Core.dll", hint: "From the vendor."}]};
  const manifest = {format: "PrismatiXExtension", schemaRevision: 1, language: "javascript", id: "glow", version: "1.0.0",
    requiredEngineVersion: "*", entry: "dist/main.js", capabilities: ["runtime", "native"], commands: [], actions: [],
    native: {abi: 1, services: ["glow.fx"], libraries: {"win32-x64": library}}} as unknown as ExtensionManifest;
  const extensions = [{manifestPath: "Content/Extensions/glow/glow.pxextension", manifest}];
  await write(join(root, "Content/Extensions/glow/native/win32-x64/glow.dll"), "library");

  const missing = await resolveNativeUserFiles(root, extensions, ["win32-x64", "linux-x64", "web"]);
  assert.deepEqual(missing.findings.map((item) => item.code), ["PXCLIEXT1106"], "only platforms with the library need it");
  assert.match(missing.findings[0]!.hint ?? "", /^From the vendor\. Put it at NativeUserFiles\/glow\/win32-x64\/Core\.dll/u);

  await write(join(root, "NativeUserFiles/glow/win32-x64/Core.dll"), "project copy");
  const fromProject = await resolveNativeUserFiles(root, extensions, ["win32-x64"]);
  assert.equal(fromProject.findings.length, 0);
  assert.equal(fromProject.files[0]!.source, join(root, "NativeUserFiles/glow/win32-x64/Core.dll"));

  // PRISMATIX_NATIVE_USER_FILES comes first, and may hold the file directly.
  const vendor = await mkdtemp(join(tmpdir(), "prismatix-vendor-"));
  await write(join(vendor, "Core.dll"), "vendor copy");
  const previous = process.env.PRISMATIX_NATIVE_USER_FILES;
  process.env.PRISMATIX_NATIVE_USER_FILES = vendor;
  try {
    const fromEnvironment = await resolveNativeUserFiles(root, extensions, ["win32-x64"]);
    assert.equal(fromEnvironment.files[0]!.source, join(vendor, "Core.dll"));

    // sha256s is an allow list.
    const sha = (text: string) => (createHash("sha256").update(text).digest("hex"));
    const allowing = (sha256s: string[]) => [{manifestPath: extensions[0]!.manifestPath, manifest: {...manifest, native: {
      ...manifest.native!, libraries: {"win32-x64": {...library, userFiles: [{name: "Core.dll", sha256s}]}}}} as ExtensionManifest}];
    assert.equal((await resolveNativeUserFiles(root, allowing([sha("vendor copy")]), ["win32-x64"])).findings.length, 0);
    const refused = await resolveNativeUserFiles(root, allowing([sha("another version")]), ["win32-x64"]);
    assert.deepEqual(refused.findings.map((item) => item.code), ["PXCLIEXT1107"]);

    // Staging replaces only what an earlier pack copied.
    const dataRoot = join(root, "out");
    await write(join(dataRoot, "Plugins/glow/notes.txt"), "mine");
    await write(join(dataRoot, "Plugins/other/manual.dll"), "mine too");
    const copied = await stageNativePlugins(root, extensions, "win32-x64", dataRoot, fromEnvironment.files);
    assert.deepEqual(copied, ["Plugins/glow/glow.dll", "Plugins/glow/Core.dll"]);
    assert.equal(await readFile(join(dataRoot, "Plugins/glow/Core.dll"), "utf8"), "vendor copy");
    assert.deepEqual(JSON.parse(await readFile(join(dataRoot, "Plugins/.prismatix-managed.json"), "utf8")),
      {files: ["glow/glow.dll", "glow/Core.dll"]});
    await stageNativePlugins(root, extensions, "win32-x64", dataRoot, fromEnvironment.files);
    assert.equal(await readFile(join(dataRoot, "Plugins/glow/notes.txt"), "utf8"), "mine", "a file put there by hand stays");
    // Without native extensions the managed files go; the rest stays.
    assert.deepEqual(await stageNativePlugins(root, [], "win32-x64", dataRoot), []);
    assert.deepEqual((await readdir(join(dataRoot, "Plugins/glow"))).sort(), ["notes.txt"]);
    assert.equal(await readFile(join(dataRoot, "Plugins/other/manual.dll"), "utf8"), "mine too");
    assert.ok(!existsSync(join(dataRoot, "Plugins/.prismatix-managed.json")));
  } finally {
    if (previous === undefined) delete process.env.PRISMATIX_NATIVE_USER_FILES;
    else process.env.PRISMATIX_NATIVE_USER_FILES = previous;
  }
});
