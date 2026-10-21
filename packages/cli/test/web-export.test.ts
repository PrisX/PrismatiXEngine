import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {mkdtemp, mkdir, readFile, readdir, rm, writeFile} from "node:fs/promises";
import {dirname, join} from "node:path";
import {tmpdir} from "node:os";
import test from "node:test";
import {exportWeb, locateWebPlayer, resolveWebPlayer, WebPlayerMissing, webPlayerInstall} from "../src/web-export.js";
import {sdlGpuWgsl} from "../src/web-shader.js";

const version = (JSON.parse(await readFile(new URL("../../package.json", import.meta.url), "utf8")) as {version: string}).version;
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

/** A schema revision 2 Web Player package with both renderers, as scripts/build-wasm-player.mjs lays it out. */
async function writePlayer(player: string, {engineVersion = version} = {}) {
  const write = async (path: string, bytes: Buffer | string) => {
    await mkdir(dirname(join(player, path)), {recursive: true});
    await writeFile(join(player, path), bytes);
    return sha256(bytes);
  };
  const artifacts: Record<string, string> = {};
  for (const [path, bytes] of [["web-shaders.json", "{}"], ["Notices/license.txt", "Runtime license"],
    ["RuntimeSource.tar.gz", "Corresponding source archive"]] as const) artifacts[path] = await write(path, bytes);
  const renderers: Record<string, unknown> = {};
  for (const renderer of ["webgl2", "webgpu"]) {
    const own: Record<string, string> = {};
    own[`${renderer}/prismatix-player.mjs`] = await write(`${renderer}/prismatix-player.mjs`, `export default()=>"${renderer}"`);
    own[`${renderer}/prismatix-player.wasm`] = await write(`${renderer}/prismatix-player.wasm`,
      Buffer.from([0, 97, 115, 109, 1, 0, 0, 0, renderer === "webgpu" ? 2 : 1]));
    own[`${renderer}/Notices/SDL/LICENSE.txt`] = await write(`${renderer}/Notices/SDL/LICENSE.txt`, `SDL for ${renderer}`);
    const notices = [`${renderer}/Notices/SDL/LICENSE.txt`];
    if (renderer === "webgpu") {
      own["webgpu/Notices/Dawn/LICENSE"] = await write("webgpu/Notices/Dawn/LICENSE", "Dawn");
      notices.push("webgpu/Notices/Dawn/LICENSE");
    }
    renderers[renderer] = {directory: renderer, artifacts: own, notices, suspend: "asyncify",
      sdl: {repository: `https://example.invalid/${renderer}`, commit: "0".repeat(40)}};
  }
  await writeFile(join(player, "web-player.json"), JSON.stringify({format: "PrismatiXWebPlayer", schemaRevision: 2,
    variant: "shipped", status: "experimental", engineVersion, defaultRenderer: "webgl2", renderers, artifacts,
    notices: ["Notices/license.txt"], sources: ["RuntimeSource.tar.gz"]}));
}

async function writeDistribution(distribution: string) {
  await mkdir(join(distribution, "Package"), {recursive: true});
  const original = {format: "PrismatiXPackageManifest", schemaRevision: 1, gameId: "fixture", title: "Fixture",
    packageFingerprint: "a".repeat(64), archives: [{file: "Content.pdx"}], resolution: {width: 1920, height: 1080}};
  await writeFile(join(distribution, "Package", "manifest.json"), JSON.stringify(original));
  await writeFile(join(distribution, "Package", "content-index.json"), JSON.stringify({entries: []}));
  const bytes = Buffer.alloc(1024 * 1024 + 15, 42);
  await writeFile(join(distribution, "Content.pdx"), bytes);
  await writeFile(join(distribution, "native.exe"), "desktop");
  return {original, bytes};
}

test("exports PDX files in verified, bounded chunks and retains the native manifest", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-web-export-"));
  try {
    const player = join(root, "runtime"), project = join(root, "project"), distribution = join(root, "native");
    await mkdir(project, {recursive: true});
    await writePlayer(player);
    const {original, bytes} = await writeDistribution(distribution);
    const output = join(project, "dist", "web");
    const result = await exportWeb({projectRoot: project, distribution, playerDirectory: player, output});
    assert.equal(result.target, "web");
    assert.equal(result.renderer, "webgl2");
    const manifest = JSON.parse(await readFile(join(output, "web-export.json"), "utf8")) as
      {renderer: string; files: {path: string; chunks: {url: string; size: number; sha256: string}[]}[]};
    assert.equal(manifest.renderer, "webgl2");
    const file = manifest.files.find(file => file.path === "Content.pdx")!;
    assert.deepEqual(file.chunks.map(chunk => chunk.size), [1024 * 1024, 15]);
    const reconstructed: Buffer[] = [];
    for (const chunk of file.chunks) {
      const encoded = await readFile(join(output, chunk.url));
      assert.equal(createHash("sha256").update(encoded).digest("hex"), chunk.sha256);
      reconstructed.push(encoded);
    }
    assert.deepEqual(Buffer.concat(reconstructed), bytes);
    assert.ok(!manifest.files.some(file => file.path === "native.exe"));
    assert.equal(await readFile(join(output, "Notices/license.txt"), "utf8"), "Runtime license");
    assert.equal(await readFile(join(output, "Notices/SDL/LICENSE.txt"), "utf8"), "SDL for webgl2");
    assert.equal(await readFile(join(output, "RuntimeSource.tar.gz"), "utf8"), "Corresponding source archive");
    await writeFile(join(player, "RuntimeSource.tar.gz"), "tampered source");
    await assert.rejects(resolveWebPlayer(player), /checksum mismatch: RuntimeSource/);
    await writeFile(join(player, "RuntimeSource.tar.gz"), "Corresponding source archive");
    assert.deepEqual(JSON.parse(await readFile(join(distribution, "Package", "manifest.json"), "utf8")), original);
    await writeFile(join(player, "webgl2/prismatix-player.mjs"), "tampered");
    await assert.rejects(resolveWebPlayer(player), /checksum mismatch/);
    await assert.rejects(exportWeb({projectRoot: project, distribution, playerDirectory: player, output}), /checksum mismatch/);
    // The other renderer's files are not part of a WebGPU export, so they cannot block it.
    await resolveWebPlayer(player, "webgpu");
    assert.deepEqual(await readFile(join(distribution, "Content.pdx")), bytes);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("a WebGPU export carries only the WebGPU runtime and names its renderer", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-web-export-"));
  try {
    const player = join(root, "runtime"), project = join(root, "project"), distribution = join(root, "native");
    await mkdir(project, {recursive: true});
    await writePlayer(player);
    await writeDistribution(distribution);
    const output = join(project, "dist", "web");
    const result = await exportWeb({projectRoot: project, distribution, playerDirectory: player, renderer: "webgpu", output});
    assert.equal(result.renderer, "webgpu");
    assert.equal(await readFile(join(output, "prismatix-player.mjs"), "utf8"), 'export default()=>"webgpu"');
    assert.equal((await readFile(join(output, "prismatix-player.wasm")))[8], 2);
    assert.equal(await readFile(join(output, "Notices/SDL/LICENSE.txt"), "utf8"), "SDL for webgpu");
    assert.equal(await readFile(join(output, "Notices/Dawn/LICENSE"), "utf8"), "Dawn");
    const entries = await readdir(output);
    assert.ok(!entries.includes("webgl2") && !entries.includes("webgpu"), "renderer directories must not be copied");
    const playerManifest = JSON.parse(await readFile(join(output, "web-player.json"), "utf8")) as
      {renderer: string; engineVersion: string; artifacts: Record<string, string>; notices: string[]; suspend: string};
    assert.equal(playerManifest.renderer, "webgpu");
    assert.equal(playerManifest.engineVersion, version);
    assert.equal(playerManifest.suspend, "asyncify");
    assert.equal(playerManifest.artifacts["prismatix-player.wasm"], sha256(await readFile(join(output, "prismatix-player.wasm"))));
    assert.ok(playerManifest.notices.includes("Notices/Dawn/LICENSE"));
    const manifest = JSON.parse(await readFile(join(output, "web-export.json"), "utf8")) as {renderer: string; shell: string[]};
    assert.equal(manifest.renderer, "webgpu");
    assert.ok(manifest.shell.includes("Notices/Dawn/LICENSE"));
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("rejects Web Players of another version, revision or without the requested renderer", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-web-export-"));
  try {
    const player = join(root, "runtime");
    await writePlayer(player, {engineVersion: "0.0.1"});
    await assert.rejects(resolveWebPlayer(player), /does not match this CLI/);
    await writePlayer(player);
    const metadata = JSON.parse(await readFile(join(player, "web-player.json"), "utf8")) as Record<string, unknown> & {renderers: Record<string, unknown>};
    delete metadata.renderers.webgpu;
    await writeFile(join(player, "web-player.json"), JSON.stringify(metadata));
    await assert.rejects(resolveWebPlayer(player, "webgpu"), /no webgpu renderer/);
    await writeFile(join(player, "web-player.json"), JSON.stringify({...metadata, schemaRevision: 1}));
    await assert.rejects(resolveWebPlayer(player), /schema revision 1 is not supported/);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("finds the project's @prismatix/web-player of this version, else names the exact install command", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-web-player-"));
  const noCheckout = {PRISMATIX_NO_CHECKOUT_RUNTIME: "1"};
  try {
    const project = join(root, "game");
    await mkdir(project, {recursive: true});
    await writeFile(join(project, "package.json"), JSON.stringify({private: true}));
    await assert.rejects(locateWebPlayer(project, noCheckout), (error: unknown) =>
      error instanceof WebPlayerMissing && error.install.manager === "pnpm" &&
      error.install.command === `pnpm add -D @prismatix/web-player@${version}` &&
      error.message.includes(`pnpm add -D @prismatix/web-player@${version}`) && error.message.includes("prismatix add web-player"));
    await writeFile(join(project, "package-lock.json"), "{}");
    assert.equal((await webPlayerInstall(project)).command, `npm install -D --save-exact @prismatix/web-player@${version}`);
    await writeFile(join(project, "pnpm-lock.yaml"), "lockfileVersion: '9.0'\n");
    assert.equal((await webPlayerInstall(project)).manager, "pnpm");

    const installed = join(project, "node_modules", "@prismatix", "web-player");
    await writePlayer(installed);
    await writeFile(join(installed, "package.json"), JSON.stringify({name: "@prismatix/web-player", version: "0.0.1"}));
    await assert.rejects(locateWebPlayer(project, noCheckout), (error: unknown) =>
      error instanceof WebPlayerMissing && /@prismatix\/web-player@0\.0\.1, not/u.test(error.message));
    await writeFile(join(installed, "package.json"), JSON.stringify({name: "@prismatix/web-player", version}));
    const found = await locateWebPlayer(project, noCheckout);
    assert.equal(found.source, "project");
    assert.equal(found.directory, installed);
    assert.equal((await resolveWebPlayer(found.directory)).renderer, "webgl2");
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("rebinds Web effect WGSL for SDL_GPU's WebGPU backend", () => {
  const wgsl = "@group(2) @binding(0) \nvar tex: texture_2d<f32>;\n@group(2) @binding(16) \nvar smp: sampler;\n" +
    "@group(2) @binding(1) var tex1: texture_2d<f32>;\n@group(2) @binding(17) var smp1: sampler;\n@group(3) @binding(0) var<uniform> u: U;\n";
  assert.equal(sdlGpuWgsl(wgsl), "@group(2) @binding(0)\nvar tex: texture_2d<f32>;\n@group(2) @binding(1)\nvar smp: sampler;\n" +
    "@group(2) @binding(2) var tex1: texture_2d<f32>;\n@group(2) @binding(3) var smp1: sampler;\n@group(3) @binding(0) var<uniform> u: U;\n");
  assert.throws(() => sdlGpuWgsl("@group(2) @binding(3) var smp: sampler;"), /Unexpected sampler binding 3/);
});
