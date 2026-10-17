import assert from "node:assert/strict";
import {chmod, mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {delimiter, join} from "node:path";
import test from "node:test";
import {discoverQaPlayer, discoverRuntimeBundle, discoverRuntimeTool, RuntimeDiscoveryFailure} from "../src/runtime-discovery.js";

async function fixture(t: test.TestContext) {
  const root = await mkdtemp(join(tmpdir(), "prismatix-runtime-discovery-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const bin = join(root, "bin");
  const share = join(root, "share", "PrismatiX");
  await mkdir(bin, {recursive: true});
  await mkdir(share, {recursive: true});
  const tools = {previewHost: "preview.mjs", packager: "packager.mjs", player: "player.mjs"};
  for (const file of Object.values(tools)) await writeFile(join(bin, file), "// fixture");
  const manifest = join(share, "runtime-manifest.json");
  const data = {format: "PrismatiXRuntimeManifest", schemaRevision: 1, version: "0.2.0",
    target: `${process.platform}-${process.arch}`, binDir: "../../bin", tools};
  await writeFile(manifest, JSON.stringify(data));
  return {root, bin, manifest, data};
}
function rejected(reason: RegExp) {
  return (error: unknown) => error instanceof RuntimeDiscoveryFailure && reason.test(error.attempts.join("\n"));
}

test("compatible installed bundle resolves all tools with one manifest identity", async t => {
  const f = await fixture(t);
  const found = await discoverRuntimeBundle(["packager", "player"], {runtime: f.root, environment: {}});
  assert.equal(found.packager.path, join(f.bin, "packager.mjs"));
  assert.equal(found.player.path, join(f.bin, "player.mjs"));
  assert.equal(found.packager.bundleId, found.player.bundleId);
  assert.equal(found.packager.version, "0.2.0");
  assert.match(found.player.source, /developer override; manifest checked/);
  const direct = await discoverRuntimeTool("player", {runtime: f.manifest, environment: {}});
  assert.equal(direct.path, found.player.path);
});

test("rejects incompatible version, missing or wrong architecture and schema", async t => {
  const f = await fixture(t);
  for (const [change, reason] of [
    [{version: "0.3.0"}, /incompatible/], [{version: "0.20.0"}, /incompatible/],
    [{schemaRevision: 2}, /schema revision/], [{target: "wrong-platform-arch"}, /does not match/],
    [{target: undefined}, /does not match/],
  ] as const) {
    await writeFile(f.manifest, JSON.stringify({...f.data, ...change}));
    await assert.rejects(discoverRuntimeTool("player", {runtime: f.root, environment: {}}), rejected(reason));
  }
});

test("invalid manifest cannot silently fall back to an adjacent executable", async t => {
  const f = await fixture(t);
  const nativeName = process.platform === "win32" ? "PrismatiXPlayer.exe" : "PrismatiXPlayer";
  await writeFile(join(f.bin, nativeName), "fixture");
  await chmod(join(f.bin, nativeName), 0o755);
  await writeFile(f.manifest, "{ broken");
  await assert.rejects(discoverRuntimeTool("player", {runtime: f.root, environment: {}}), rejected(/Rejected/));
});

test("rejects tool traversal and binDir escape from runtime prefix", async t => {
  const f = await fixture(t);
  await writeFile(f.manifest, JSON.stringify({...f.data, tools: {player: "../player.mjs"}}));
  await assert.rejects(discoverRuntimeTool("player", {runtime: f.root, environment: {}}), rejected(/filename inside binDir/));
  const outside = join(f.root, "..", `${f.root.split(/[\\/]/).at(-1)}-outside.mjs`);
  t.after(() => rm(outside, {force: true}));
  await writeFile(outside, "// outside");
  await writeFile(f.manifest, JSON.stringify({...f.data, binDir: "../../..", tools: {player: outside.split(/[\\/]/).at(-1)}}));
  await assert.rejects(discoverRuntimeTool("player", {runtime: f.root, environment: {}}), rejected(/escapes bundle prefix/));
});

test("explicit scripts remain usable with unverified developer diagnostic", async t => {
  const f = await fixture(t);
  const found = await discoverRuntimeTool("player", {player: join(f.bin, "player.mjs"), environment: {}});
  assert.equal(found.bundleId, undefined);
  assert.match(found.source, /--player.*unverified developer override/);
  await writeFile(f.manifest, JSON.stringify({...f.data, version: "0.2.0-mock"}));
  const mock = await discoverRuntimeTool("player", {runtime: f.root, environment: {}});
  assert.equal(mock.version, "0.2.0-mock");
});

test("PATH search requires explicit developer opt-in", async t => {
  const f = await fixture(t);
  const name = process.platform === "win32" ? "PrismatiXPlayer.exe" : "PrismatiXPlayer";
  const path = join(f.bin, name);
  await writeFile(path, "fixture");
  await chmod(path, 0o755);
  await assert.rejects(discoverRuntimeTool("player", {environment: {PATH: f.bin}}), rejected(/fallback disabled/));
  const found = await discoverRuntimeTool("player", {environment: {PATH: f.bin, PRISMATIX_ALLOW_SYSTEM_RUNTIME: "1"}});
  assert.equal(found.path, path);
  assert.match(found.source, /PATH.*unverified developer override/);
});

test("bundle discovery rejects split PATH tools but permits explicit per-tool overrides", async t => {
  const f = await fixture(t);
  const other = join(f.root, "other");
  await mkdir(other);
  for (const [dir, tool] of [[f.bin, "Packager"], [other, "Player"]]) {
    const path = join(dir!, `PrismatiX${tool}${process.platform === "win32" ? ".exe" : ""}`);
    await writeFile(path, "fixture");
    await chmod(path, 0o755);
  }
  await assert.rejects(discoverRuntimeBundle(["packager", "player"], {
    environment: {PATH: [f.bin, other].join(delimiter), PRISMATIX_ALLOW_SYSTEM_RUNTIME: "1"},
  }), rejected(/different runtime locations/));
  const explicit = await discoverRuntimeBundle(["packager", "player"], {
    packager: join(f.bin, "packager.mjs"), player: join(f.bin, "player.mjs"), environment: {},
  });
  assert.match(explicit.packager.source, /unverified developer override/);
  assert.match(explicit.player.source, /unverified developer override/);
  assert.equal(await readFile(f.manifest, "utf8"), JSON.stringify(f.data));
});

test("the QA Player for run --at is found beside the Player, or named explicitly", async t => {
  const f = await fixture(t);
  const qaName = process.platform === "win32" ? "PrismatiXPlayerQA.exe" : "PrismatiXPlayerQA";
  await assert.rejects(discoverQaPlayer({runtime: f.root, environment: {}}), rejected(/build the PrismatiXPlayerQA target/));
  await writeFile(join(f.bin, qaName), "fixture");
  await chmod(join(f.bin, qaName), 0o755);
  const found = await discoverQaPlayer({runtime: f.root, environment: {}});
  assert.equal(found.path, join(f.bin, qaName));
  assert.match(found.source, /QA Player beside it/);
  const other = join(f.root, "other-qa.mjs");
  await writeFile(other, "// fixture");
  assert.equal((await discoverQaPlayer({runtime: f.root, playerQA: other, environment: {}})).path, other);
  assert.equal((await discoverQaPlayer({runtime: f.root, environment: {PRISMATIX_PLAYER_QA: other}})).path, other);
  await assert.rejects(discoverQaPlayer({playerQA: join(f.root, "missing.exe"), environment: {}}),
    rejected(/does not exist/));
});
