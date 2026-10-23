// Sharing extensions end to end: pack, a local mock registry (the HTTP API
// v1 and a static index), add/update/remove/outdated/verify, the lock and
// publish. The CLI runs as a child process so the mock server keeps serving.

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {createHash} from "node:crypto";
import {existsSync} from "node:fs";
import {mkdir, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {createServer, type IncomingMessage, type Server, type ServerResponse} from "node:http";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {pathToFileURL} from "node:url";
import test from "node:test";

import {
  readExtensionPackage,
  resolveExtensionVersion,
  validateExtensionPackage,
  validateRegistryDocument,
  writeExtensionPackage,
  type RegistryVersion,
} from "@prismatix/authoring-sdk";

import {parseExtensionSource} from "../src/extension-install.js";

const cli = resolve("dist/src/cli.js");

interface Ran {readonly status: number | null; readonly stdout: string; readonly stderr: string}

function run(args: readonly string[], options: {env?: NodeJS.ProcessEnv; cwd?: string} = {}): Promise<Ran> {
  return new Promise((done) => {
    const child = spawn(process.execPath, [cli, ...args], {cwd: options.cwd,
      env: {...process.env, PRISMATIX_EXTENSION_REGISTRY: "", PRISMATIX_REGISTRY_TOKEN: "", CI: "1", ...options.env},
      stdio: ["ignore", "pipe", "pipe"]});
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8").on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.setEncoding("utf8").on("data", (chunk: string) => { stderr += chunk; });
    child.on("close", (status) => done({status, stdout, stderr}));
  });
}

function last(output: string): Record<string, unknown> {
  const lines = output.split(/\r?\n/u).filter((line) => line.startsWith("{"));
  return JSON.parse(lines.at(-1)!) as Record<string, unknown>;
}

async function write(path: string, value: string | Uint8Array): Promise<void> {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, value);
}

async function project(name: string): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), `prismatix-share-${name}-`));
  await write(join(root, "prismatix.json"), `{\n  "name": "Share",\n  "id": "share-${name}",\n  "defaultLocale": "en",\n` +
    `  "resolution": {"width": 1280, "height": 720}\n}\n`);
  await write(join(root, "Story/main.pxstory"), "*start\nHello.\n[end]\n");
  return root;
}

/** An authored JavaScript extension `id` in `root`, packed at `version` by owner `owen`. */
async function authored(root: string, id: string, command: string): Promise<void> {
  const created = await run(["extension", "create", id, "--lang", "js", "--command", command, "--project", root, "--json"]);
  assert.equal(created.status, 0, created.stderr);
}

async function setVersion(root: string, id: string, version: string, extra: Record<string, unknown> = {}): Promise<void> {
  const path = join(root, "Content/Extensions", id, `${id}.pxextension`);
  const manifest = JSON.parse(await readFile(path, "utf8")) as Record<string, unknown>;
  await writeFile(path, `${JSON.stringify({...manifest, version, ...extra}, null, 2)}\n`);
}

async function pack(root: string, id: string, output: string, extra: readonly string[] = []): Promise<{file: string; sha256: string}> {
  const packed = await run(["extension", "pack", id, "--owner", "owen", "--output", output, "--project", root, "--json", ...extra]);
  assert.equal(packed.status, 0, packed.stderr);
  return JSON.parse(packed.stdout) as {file: string; sha256: string};
}

// A stored ZIP with arbitrary names and attributes, for archives pack never writes.
function rawZip(entries: readonly {name: string; data: Uint8Array; method?: number; mode?: number}[]): Uint8Array {
  const parts: Buffer[] = [];
  const central: Buffer[] = [];
  let offset = 0;
  for (const entry of entries) {
    const name = Buffer.from(entry.name, "utf8");
    const crc = Buffer.alloc(4);
    crc.writeUInt32LE(crc32(entry.data));
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4); local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(entry.method ?? 0, 8); local.writeUInt16LE(0x21, 12); crc.copy(local, 14);
    local.writeUInt32LE(entry.data.length, 18); local.writeUInt32LE(entry.data.length, 22); local.writeUInt16LE(name.length, 26);
    const record = Buffer.alloc(46);
    record.writeUInt32LE(0x02014b50, 0); record.writeUInt16LE(0x0314, 4); record.writeUInt16LE(20, 6);
    record.writeUInt16LE(0x0800, 8); record.writeUInt16LE(entry.method ?? 0, 10); record.writeUInt16LE(0x21, 14); crc.copy(record, 16);
    record.writeUInt32LE(entry.data.length, 20); record.writeUInt32LE(entry.data.length, 24); record.writeUInt16LE(name.length, 28);
    record.writeUInt32LE(((entry.mode ?? 0o100644) << 16) >>> 0, 38); record.writeUInt32LE(offset, 42);
    parts.push(local, name, Buffer.from(entry.data));
    central.push(record, name);
    offset += 30 + name.length + entry.data.length;
  }
  const centralBytes = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBytes.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...parts, centralBytes, end]));
}

function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc & 1) !== 0 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// The mock registry ---------------------------------------------------------

interface Stored {readonly document: RegistryVersion; readonly bytes: Uint8Array}

class MockRegistry {
  readonly versions = new Map<string, Map<string, Stored>>();
  readonly uploads = new Map<string, {id: string; version: string; sha256: string; bytes?: Uint8Array}>();
  readonly requests: string[] = [];
  /** Serve this many bytes in place of a download, to test the hash check. */
  tamper: Uint8Array | undefined;
  leakedToken = false;
  server: Server | undefined;
  base = "";
  staticRoot: string | undefined;

  async start(): Promise<void> {
    this.server = createServer((request, response) => { void this.handle(request, response); });
    await new Promise<void>((done) => this.server!.listen(0, "127.0.0.1", done));
    const address = this.server.address() as {port: number};
    this.base = `http://127.0.0.1:${address.port}`;
  }

  async stop(): Promise<void> {
    await new Promise<void>((done) => this.server?.close(() => done()));
  }

  send(response: ServerResponse, status: number, value: unknown): void {
    response.writeHead(status, {"content-type": "application/json"});
    response.end(JSON.stringify(value));
  }

  fail(response: ServerResponse, status: number, code: string, message: string): void {
    this.send(response, status, {error: {code, message}});
  }

  async body(request: IncomingMessage): Promise<Buffer> {
    const chunks: Buffer[] = [];
    for await (const chunk of request) chunks.push(chunk as Buffer);
    return Buffer.concat(chunks);
  }

  details(id: string) {
    const versions = [...this.versions.get(id)!.values()].map((item) => item.document)
      .sort((left, right) => left.version < right.version ? 1 : -1);
    const latest = versions.find((item) => !item.yanked) ?? versions[0]!;
    return {id, owner: id.split("/")[0], name: id.split("/")[1], displayName: latest.metadata?.displayName,
      capabilities: latest.manifest.capabilities, latestVersion: latest.version,
      versions: versions.map((item) => ({version: item.version, requiredEngineVersion: item.requiredEngineVersion,
        yanked: item.yanked, sha256: item.sha256, size: item.size}))};
  }

  async handle(request: IncomingMessage, response: ServerResponse): Promise<void> {
    const url = new URL(request.url!, this.base);
    this.requests.push(`${request.method} ${url.pathname}`);
    if (this.staticRoot !== undefined) {
      // A static host: files only; a missing file is a plain 404 page.
      const path = join(this.staticRoot, ...url.pathname.split("/").filter(Boolean).map(decodeURIComponent));
      if (request.method === "GET" && existsSync(path) && !path.endsWith(this.staticRoot)) {
        try {
          const data = await readFile(path);
          response.writeHead(200, {"content-type": path.endsWith(".json") ? "application/json" : "application/octet-stream"});
          response.end(data);
          return;
        } catch { /* a folder */ }
      }
      // Some static hosts list a folder instead (python -m http.server).
      if (request.method === "GET" && existsSync(path)) {
        response.writeHead(200, {"content-type": "text/html; charset=utf-8"});
        response.end("<ul><li>index.json</li></ul>");
        return;
      }
      response.writeHead(404, {"content-type": "text/html"});
      response.end("<h1>404</h1>");
      return;
    }
    const parts = url.pathname.split("/").filter(Boolean).map(decodeURIComponent);
    const authorized = request.headers.authorization === "Bearer secret-token";
    if (request.method === "GET" && url.pathname === "/v1/search") {
      const query = (url.searchParams.get("q") ?? "").toLowerCase();
      const results = [...this.versions.keys()].filter((id) => id.includes(query)).map((id) => {
        const details = this.details(id);
        const latest = details.versions.find((item) => !item.yanked)!;
        return {id, latestVersion: latest.version, requiredEngineVersion: latest.requiredEngineVersion,
          capabilities: details.capabilities, displayName: details.displayName};
      });
      this.send(response, 200, {query, results, total: results.length, page: 1, pageSize: 20});
      return;
    }
    if (request.method === "GET" && parts[0] === "v1" && parts[1] === "extensions" && parts.length >= 4) {
      const id = `${parts[2]}/${parts[3]}`;
      const known = this.versions.get(id);
      if (known === undefined) { this.fail(response, 404, "not_found", `${id} is not published`); return; }
      if (parts.length === 4) { this.send(response, 200, this.details(id)); return; }
      if (parts[4] === "resolve") {
        const resolved = resolveExtensionVersion([...known.values()].map((item) => item.document),
          url.searchParams.get("range") ?? "*", url.searchParams.get("engine") ?? undefined);
        if (resolved.version === undefined) this.fail(response, 404, "no_matching_version", `nothing matches (${resolved.problem})`);
        else this.send(response, 200, resolved.version);
        return;
      }
      if (parts[4] === "versions" && parts.length === 6) {
        const stored = known.get(parts[5]!);
        if (stored === undefined) this.fail(response, 404, "not_found", "no such version");
        else this.send(response, 200, stored.document);
        return;
      }
    }
    if (request.method === "GET" && parts[0] === "files") {
      const stored = [...this.versions.values()].flatMap((map) => [...map.values()])
        .find((item) => item.document.download === `/files/${parts[1]}`);
      if (stored === undefined) { this.fail(response, 404, "not_found", "no file"); return; }
      response.writeHead(200, {"content-type": "application/octet-stream"});
      response.end(this.tamper ?? stored.bytes);
      return;
    }
    if (request.method === "POST" && url.pathname === "/v1/extensions") {
      if (!authorized) { this.fail(response, 401, "unauthorized", "a token is needed"); return; }
      const announced = JSON.parse((await this.body(request)).toString("utf8")) as unknown;
      const checked = validateRegistryDocument<{id: string; version: string; sha256: string}>("publishRequest", announced);
      if (!checked.valid) { this.fail(response, 400, "invalid", JSON.stringify(checked.diagnostics)); return; }
      if (this.versions.get(checked.value!.id)?.has(checked.value!.version)) {
        this.fail(response, 409, "version_exists", `${checked.value!.id}@${checked.value!.version} is already published`);
        return;
      }
      const uploadId = `u${this.uploads.size + 1}`;
      this.uploads.set(uploadId, {id: checked.value!.id, version: checked.value!.version, sha256: checked.value!.sha256});
      this.send(response, 201, {uploadId, uploadUrl: `/storage/${uploadId}?signature=abc`, uploadMethod: "PUT",
        uploadHeaders: {"x-storage-meta": "pxext"}});
      return;
    }
    if (request.method === "PUT" && parts[0] === "storage") {
      if (request.headers.authorization !== undefined) this.leakedToken = true;
      const upload = this.uploads.get(parts[1]!);
      if (upload === undefined || url.searchParams.get("signature") !== "abc" || request.headers["x-storage-meta"] !== "pxext") {
        response.writeHead(403); response.end(); return;
      }
      upload.bytes = new Uint8Array(await this.body(request));
      response.writeHead(200); response.end();
      return;
    }
    if (request.method === "POST" && parts[0] === "v1" && parts[1] === "uploads" && parts[3] === "confirm") {
      if (!authorized) { this.fail(response, 401, "unauthorized", "a token is needed"); return; }
      const upload = this.uploads.get(parts[2]!);
      if (upload?.bytes === undefined) { this.fail(response, 400, "not_uploaded", "upload the file first"); return; }
      // The registry runs the same check `prismatix extension add` does.
      const validation = await validateExtensionPackage(upload.bytes);
      if (!validation.valid || validation.sha256 !== upload.sha256 || validation.id !== upload.id) {
        this.fail(response, 422, "invalid_package", "the upload is not the announced package"); return;
      }
      const document: RegistryVersion = {id: upload.id, version: validation.version!,
        requiredEngineVersion: validation.manifest!.requiredEngineVersion, sha256: validation.sha256, size: validation.size,
        download: `/files/${upload.id.replace("/", "-")}-${validation.version}.pxext`, manifest: validation.manifest!,
        metadata: validation.metadata!, yanked: false};
      if (!this.versions.has(upload.id)) this.versions.set(upload.id, new Map());
      this.versions.get(upload.id)!.set(document.version, {document, bytes: upload.bytes});
      this.send(response, 200, document);
      return;
    }
    if (request.method === "POST" && parts[0] === "v1" && parts[1] === "extensions" && parts[6] === "yank") {
      if (!authorized) { this.fail(response, 401, "unauthorized", "a token is needed"); return; }
      const stored = this.versions.get(`${parts[2]}/${parts[3]}`)?.get(parts[5]!);
      if (stored === undefined) { this.fail(response, 404, "not_found", "no such version"); return; }
      const body = JSON.parse((await this.body(request)).toString("utf8")) as {yanked: boolean; reason?: string};
      const document = {...stored.document, yanked: body.yanked, ...(body.reason ? {yankedReason: body.reason} : {})};
      this.versions.get(`${parts[2]}/${parts[3]}`)!.set(parts[5]!, {...stored, document});
      this.send(response, 200, document);
      return;
    }
    this.fail(response, 404, "not_found", `${request.method} ${url.pathname}`);
  }
}

// Tests -----------------------------------------------------------------------

test("extension sources parse to registry, file, URL and npm installs", () => {
  assert.deepEqual(parseExtensionSource("owen/fx-kit@^1.2"), {kind: "registry", id: "owen/fx-kit", range: "^1.2"});
  assert.deepEqual(parseExtensionSource("owen/fx-kit"), {kind: "registry", id: "owen/fx-kit"});
  assert.deepEqual(parseExtensionSource("./a.pxext"), {kind: "file", path: "./a.pxext"});
  assert.deepEqual(parseExtensionSource("https://example.com/a.pxext"), {kind: "url", url: "https://example.com/a.pxext"});
  assert.deepEqual(parseExtensionSource("npm:@scope/fx@1.x"), {kind: "npm", name: "@scope/fx", range: "1.x"});
  assert.throws(() => parseExtensionSource("http://example.com/a.pxext"), /https/u);
  assert.throws(() => parseExtensionSource("Owen/FX"), /owner\/name/u);
});

test("pack is reproducible and validateExtensionPackage refuses unsafe archives", async () => {
  const root = await project("pack");
  try {
    await authored(root, "fx-kit", "greet");
    await write(join(root, "Content/Extensions/fx-kit/LICENSE"), "MIT License\n");
    const first = await pack(root, "fx-kit", join(root, "out-a"));
    const second = await pack(root, "fx-kit", join(root, "out-b"));
    assert.equal(first.sha256, second.sha256, "the same sources give the same package");
    assert.match(first.file, /owen-fx-kit-1\.0\.0\.pxext$/u);
    const bytes = new Uint8Array(await readFile(first.file));
    const validation = await validateExtensionPackage(bytes);
    assert.equal(validation.valid, true, JSON.stringify(validation.diagnostics));
    assert.equal(validation.id, "owen/fx-kit");
    assert.equal(validation.sha256, createHash("sha256").update(bytes).digest("hex"));
    assert.deepEqual([...validation.files.keys()], ["LICENSE", "README.md", "dist/main.js", "fx-kit.pxextension", "pxext.json",
      "types/prismatix-extension.d.ts"]);
    assert.equal(validation.manifest!.entry, "dist/main.js");
    const withSource = await pack(root, "fx-kit", join(root, "out-c"), ["--include-source"]);
    const sourceFiles = [...readExtensionPackage(new Uint8Array(await readFile(withSource.file))).files.keys()]
      .filter((path) => path.startsWith("src/"));
    assert.deepEqual(sourceFiles, ["src/LICENSE", "src/README.md", "src/fx-kit.pxextension", "src/main.js", "src/pxext.json"]);

    const files = [...validation.files].map(([path, data]) => ({path, data}));
    const traversal = rawZip([...files.map((file) => ({name: file.path, data: file.data})),
      {name: "../../evil.js", data: new TextEncoder().encode("x")}].sort((a, b) => a.name < b.name ? -1 : 1));
    const refused = await validateExtensionPackage(traversal);
    assert.equal(refused.valid, false);
    assert.equal(refused.diagnostics[0]!.code, "PXEXTPKG1003");
    const link = rawZip(files.map((file) => ({name: file.path, data: file.data,
      ...(file.path === "dist/main.js" ? {mode: 0o120777} : {})})));
    assert.equal((await validateExtensionPackage(link)).diagnostics[0]!.code, "PXEXTPKG1005");
    const deflated = rawZip(files.map((file) => ({name: file.path, data: file.data, ...(file.path === "README.md" ? {method: 8} : {})})));
    assert.equal((await validateExtensionPackage(deflated)).diagnostics[0]!.code, "PXEXTPKG1005");
    const stray = writeExtensionPackage([...files, {path: "run.sh", data: new TextEncoder().encode("rm -rf /")}]);
    assert.ok((await validateExtensionPackage(stray)).diagnostics.some((item) => item.code === "PXEXTPKG1011"));
    const truncated = bytes.subarray(0, bytes.length - 1);
    assert.equal((await validateExtensionPackage(truncated)).valid, false);

    // Installing the traversal archive from a file is refused before anything is written.
    const consumer = await project("refuse");
    try {
      await write(join(consumer, "evil.pxext"), traversal);
      const added = await run(["extension", "add", join(consumer, "evil.pxext"), "--yes", "--project", consumer]);
      assert.equal(added.status, 1);
      assert.match(added.stderr, /PXEXTPKG1003/u);
      assert.equal(existsSync(join(consumer, "Extensions")), false);
      assert.equal(existsSync(join(consumer, "prismatix-extensions.lock")), false);
    } finally { await rm(consumer, {recursive: true, force: true}); }
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("the official registry is the default; none turns registries off and default brings it back", async () => {
  const root = await project("default-registry");
  const unset = {PRISMATIX_EXTENSION_REGISTRY: ""};
  try {
    const shown = await run(["extension", "registry", "--project", root, "--json"], {env: unset});
    assert.deepEqual(last(shown.stdout), {url: "https://illyasviel.dev", source: "default"});
    const off = await run(["extension", "registry", "none", "--project", root, "--json"], {env: unset});
    assert.equal(off.status, 0, off.stderr);
    assert.equal(JSON.parse(await readFile(join(root, "prismatix.json"), "utf8")).settings["extensions.registry"], false);
    assert.deepEqual(last((await run(["extension", "registry", "--project", root, "--json"], {env: unset})).stdout),
      {url: null, source: null});
    const search = await run(["extension", "search", "fx", "--project", root], {env: unset});
    assert.equal(search.status, 1);
    assert.match(search.stderr, /turned off/u);
    assert.equal((await run(["extension", "registry", "default", "--project", root], {env: unset})).status, 0);
    assert.equal(JSON.parse(await readFile(join(root, "prismatix.json"), "utf8")).settings, undefined);
    assert.deepEqual(last((await run(["extension", "registry", "--project", root, "--json"], {env: {
      PRISMATIX_EXTENSION_REGISTRY: "https://registry.example.com"}})).stdout),
      {url: "https://registry.example.com", source: "environment"});
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("publish, search, resolve, add, verify, update, outdated and remove against a registry", async () => {
  const registry = new MockRegistry();
  await registry.start();
  const author = await project("author");
  const consumer = await project("consumer");
  const env = {PRISMATIX_EXTENSION_REGISTRY: registry.base};
  try {
    await authored(author, "fx-kit", "greet");
    // With registries turned off, the error says how to name one.
    const none = await run(["extension", "search", "fx", "--project", consumer], {env: {PRISMATIX_EXTENSION_REGISTRY: "none"}});
    assert.equal(none.status, 1);
    assert.match(none.stderr, /PRISMATIX_EXTENSION_REGISTRY/u);

    // Publishing needs the token from the environment, never from the project.
    const unauthenticated = await run(["extension", "publish", "fx-kit", "--owner", "owen", "--project", author], {env});
    assert.equal(unauthenticated.status, 1);
    assert.match(unauthenticated.stderr, /PRISMATIX_REGISTRY_TOKEN/u);
    const token = {...env, PRISMATIX_REGISTRY_TOKEN: "secret-token"};
    for (const [version, extra] of [["1.0.0", {}], ["1.0.1", {}], ["1.1.0", {requiredEngineVersion: ">=0.2.0 <0.3.0"}]] as const) {
      await setVersion(author, "fx-kit", version, extra);
      const published = await run(["extension", "publish", "fx-kit", "--owner", "owen", "--project", author, "--json"], {env: token});
      assert.equal(published.status, 0, published.stderr);
      assert.equal(last(published.stdout).version, version);
    }
    assert.equal(registry.leakedToken, false, "the presigned upload never receives the token");
    assert.ok(registry.requests.includes("PUT /storage/u1"));
    const again = await run(["extension", "publish", "fx-kit", "--project", author], {env: token});
    assert.equal(again.status, 1);
    assert.match(again.stderr, /version_exists/u);
    const yanked = await run(["extension", "yank", "owen/fx-kit@1.0.1", "--reason", "broken greeting", "--json"], {env: token});
    assert.equal(yanked.status, 0, yanked.stderr);

    const search = await run(["extension", "search", "fx", "--project", consumer, "--json"], {env});
    assert.equal(search.status, 0, search.stderr);
    assert.deepEqual((last(search.stdout).results as {id: string}[]).map((item) => item.id), ["owen/fx-kit"]);

    // ^1 on engine 0.1.0: 1.1.0 needs 0.2, 1.0.1 is yanked, so 1.0.0.
    const added = await run(["extension", "add", "owen/fx-kit@^1", "--yes", "--project", consumer, "--json"], {env});
    assert.equal(added.status, 0, added.stderr);
    assert.equal(last(added.stdout).version, "1.0.0");
    const lock = JSON.parse(await readFile(join(consumer, "prismatix-extensions.lock"), "utf8")) as
      {extensions: Record<string, {version: string; source: string; range: string; manifest: string}>};
    assert.deepEqual(Object.keys(lock.extensions), ["owen/fx-kit"]);
    assert.equal(lock.extensions["owen/fx-kit"]!.source, `registry+${registry.base}`);
    assert.equal(lock.extensions["owen/fx-kit"]!.range, "^1");
    assert.deepEqual(JSON.parse(await readFile(join(consumer, "prismatix.json"), "utf8")).extensions,
      ["Extensions/owen/fx-kit/fx-kit.pxextension"]);
    const validate = await run(["validate", consumer, "--json"]);
    assert.equal(validate.status, 0, validate.stderr);

    // Without --yes and without a terminal, nothing is installed.
    const unconfirmed = await run(["extension", "add", "owen/fx-kit@1.0.0", "--project", consumer], {env});
    assert.equal(unconfirmed.status, 1);
    assert.match(unconfirmed.stderr, /--yes/u);

    // A yanked version installs only when pinned exactly, with a warning.
    const pinned = await run(["extension", "add", "owen/fx-kit@1.0.1", "--yes", "--project", consumer], {env});
    assert.equal(pinned.status, 0, pinned.stderr);
    assert.match(pinned.stdout, /yanked: broken greeting/u);

    // An engine-incompatible pin names the engine range.
    const incompatible = await run(["extension", "add", "owen/fx-kit@1.1.0", "--yes", "--project", consumer], {env});
    assert.equal(incompatible.status, 1);
    assert.match(incompatible.stderr, /no_matching_version|engine/u);

    // update follows the locked range (1.0.1 is yanked and pinned, ^1 resolves 1.0.0); --to moves it.
    const updated = await run(["extension", "update", "owen/fx-kit", "--to", "~1.0.0", "--yes", "--project", consumer, "--json"], {env});
    assert.equal(updated.status, 0, updated.stderr);
    assert.deepEqual((last(updated.stdout).updates as {to: string}[]).map((item) => item.to), ["1.0.0"]);
    const outdated = await run(["extension", "outdated", "--project", consumer, "--json"], {env});
    assert.equal(outdated.status, 0, outdated.stderr);
    assert.deepEqual(last(outdated.stdout).extensions, [{id: "owen/fx-kit", current: "1.0.0", wanted: "1.0.0", latest: "1.0.0",
      source: `registry+${registry.base}`}]);

    // verify re-hashes the installed files against the lock.
    const verified = await run(["extension", "verify", "--project", consumer, "--json"]);
    assert.equal(verified.status, 0, verified.stdout);
    await writeFile(join(consumer, "Extensions/owen/fx-kit/dist/main.js"), "Engine.log('changed');\n");
    const tampered = await run(["extension", "verify", "--project", consumer, "--json"]);
    assert.equal(tampered.status, 1);
    assert.equal((last(tampered.stdout).entries as {status: string}[])[0]!.status, "modified");
    const listed = await run(["extension", "list", "--project", consumer, "--json"]);
    assert.equal((last(listed.stdout).extensions as {package: {lock: string}}[])[0]!.package.lock, "modified");

    // A download that does not match the listed SHA-256 is refused; the installed copy stays.
    registry.tamper = new TextEncoder().encode("not the package");
    const refused = await run(["extension", "add", "owen/fx-kit@1.0.0", "--yes", "--project", consumer], {env});
    assert.equal(refused.status, 1);
    assert.match(refused.stderr, /nothing was installed/u);
    assert.equal(await readFile(join(consumer, "Extensions/owen/fx-kit/dist/main.js"), "utf8"), "Engine.log('changed');\n");
    registry.tamper = undefined;
    const restored = await run(["extension", "update", "owen/fx-kit", "--reinstall", "--project", consumer, "--json"], {env});
    assert.equal(restored.status, 0, restored.stderr);
    assert.deepEqual((last(restored.stdout).updates as {status: string}[]).map((item) => item.status), ["reinstalled"]);
    assert.equal(JSON.parse(await readFile(join(consumer, "prismatix-extensions.lock"), "utf8")).extensions["owen/fx-kit"].range,
      "~1.0.0", "a reinstall keeps the locked range");
    assert.equal((await run(["extension", "verify", "--project", consumer])).status, 0);

    // Command id conflicts with the project's own extensions are refused.
    await authored(consumer, "local-fx", "other.thing");
    const manifestPath = join(consumer, "Content/Extensions/local-fx/local-fx.pxextension");
    const local = JSON.parse(await readFile(manifestPath, "utf8")) as {commands: {id: string}[]};
    local.commands[0]!.id = "fxKit.greet";
    await writeFile(manifestPath, JSON.stringify(local));
    const conflicting = await run(["extension", "add", "owen/fx-kit@1.0.0", "--yes", "--project", consumer], {env});
    assert.equal(conflicting.status, 1);
    assert.match(conflicting.stderr, /command fxKit\.greet/u);
    const conflictVerify = await run(["extension", "verify", "--project", consumer, "--json"]);
    assert.equal(conflictVerify.status, 1);
    assert.equal((last(conflictVerify.stdout).conflicts as unknown[]).length, 1);

    const removed = await run(["extension", "remove", "owen/fx-kit", "--project", consumer, "--json"]);
    assert.equal(removed.status, 0, removed.stderr);
    assert.equal(existsSync(join(consumer, "Extensions/owen")), false);
    assert.deepEqual(JSON.parse(await readFile(join(consumer, "prismatix-extensions.lock"), "utf8")).extensions, {});
    assert.deepEqual(JSON.parse(await readFile(join(consumer, "prismatix.json"), "utf8")).extensions,
      ["Content/Extensions/local-fx/local-fx.pxextension"]);
  } finally {
    await registry.stop();
    await rm(author, {recursive: true, force: true});
    await rm(consumer, {recursive: true, force: true});
  }
});

test("a static index built by scripts/extension-index.mjs serves search, resolve and installs", async () => {
  const registry = new MockRegistry();
  await registry.start();
  const author = await project("static-author");
  const consumer = await project("static-consumer");
  const index = await mkdtemp(join(tmpdir(), "prismatix-static-index-"));
  try {
    await authored(author, "glow", "shine");
    const packages = join(index, "entries", "owen");
    const v1 = await pack(author, "glow", packages);
    await setVersion(author, "glow", "1.1.0", {requiredEngineVersion: ">=0.2.0 <0.3.0"});
    const v2 = await pack(author, "glow", packages);
    await write(join(index, "entries", "owen", "glow.json"), JSON.stringify({id: "owen/glow", versions: [
      {url: "owen-glow-1.0.0.pxext", sha256: v1.sha256, publishedAt: "2026-10-20T00:00:00Z"},
      {url: "owen-glow-1.1.0.pxext", sha256: v2.sha256}]}));
    const script = pathToFileURL(resolve("../../scripts/extension-index.mjs")).href;
    const {buildExtensionIndex} = await import(script) as {buildExtensionIndex: (entries: string, out: string, log: (m: string) => void) => Promise<string[]>};
    assert.deepEqual(await buildExtensionIndex(join(index, "entries"), join(index, "site"), () => undefined), ["owen/glow"]);
    registry.staticRoot = join(index, "site");

    // The registry setting lives in the project when set with `extension registry`.
    const set = await run(["extension", "registry", registry.base, "--project", consumer]);
    assert.equal(set.status, 0, set.stderr);
    const shown = await run(["extension", "registry", "--project", consumer, "--json"]);
    assert.deepEqual(last(shown.stdout), {url: registry.base, source: "project"});

    const search = await run(["extension", "search", "glo", "--project", consumer, "--json"]);
    assert.equal(search.status, 0, search.stderr);
    assert.deepEqual((last(search.stdout).results as {id: string; latestVersion: string}[])
      .map((item) => `${item.id}@${item.latestVersion}`), ["owen/glow@1.1.0"]);
    // The static search lists the latest version (1.1.0, for engine 0.2); installing resolves 1.0.0 on 0.1.
    const info = await run(["extension", "info", "owen/glow", "--project", consumer, "--json"]);
    assert.equal(info.status, 0, info.stderr);
    assert.equal(last(info.stdout).compatibleVersion, "1.0.0");
    const added = await run(["extension", "add", "owen/glow", "--yes", "--project", consumer, "--json"]);
    assert.equal(added.status, 0, added.stderr);
    assert.equal(last(added.stdout).version, "1.0.0");
    assert.ok(registry.requests.includes("GET /v1/extensions/owen/glow/index.json"));
    assert.ok(registry.requests.includes("GET /v1/extensions/owen/glow/versions/1.0.0/owen-glow-1.0.0.pxext"));
    const lock = JSON.parse(await readFile(join(consumer, "prismatix-extensions.lock"), "utf8")) as
      {extensions: Record<string, {sha256: string; range: string}>};
    assert.equal(lock.extensions["owen/glow"]!.sha256, v1.sha256);
    assert.equal(lock.extensions["owen/glow"]!.range, "^1.0.0");
  } finally {
    await registry.stop();
    await rm(author, {recursive: true, force: true});
    await rm(consumer, {recursive: true, force: true});
    await rm(index, {recursive: true, force: true});
  }
});
