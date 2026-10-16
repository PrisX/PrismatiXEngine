import assert from "node:assert/strict";
import {mkdir, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {inspectSaveWithRuntime, resolvePackageManifest} from "../src/save-inspection.js";

async function fixture(t: test.TestContext, source: string) {
  const root = await mkdtemp(join(tmpdir(), "prismatix-inspect-transport-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const path = join(root, "save.dat"), player = join(root, "player.mjs");
  await writeFile(path, "fixture input; native validation is tested separately");
  await writeFile(player, source);
  return {root, path, runtime: {player, environment: {}}};
}

test("inspection preserves native floating numeric lexemes and uint64 precision", async t => {
  const nativeReply = '{"schemaRevision":1,"state":{"zoom":1.0,"timestamp":18446744073709551615}}';
  const f = await fixture(t, `process.stdout.write(${JSON.stringify(nativeReply)});`);
  const result = await inspectSaveWithRuntime(f);
  assert.equal(result.output, `${nativeReply}\n`);
  assert.match(result.source, /unverified developer override/);
});

test("inspection passes file, package and secret as distinct arguments without a shell", async t => {
  const f = await fixture(t, "process.stdout.write(JSON.stringify({args: process.argv.slice(2)}));");
  const packagePath = join(f.root, "manifest with spaces.json");
  await writeFile(packagePath, "{}");
  const result = await inspectSaveWithRuntime({...f, packagePath, secret: "literal ; $() & secret", json: true});
  assert.deepEqual(JSON.parse(result.output).args,
    ["--inspect-save", f.path, "--package", packagePath, "--secret", "literal ; $() & secret", "--json"]);
  assert.equal(await readFile(f.path, "utf8"), "fixture input; native validation is tested separately");
});

test("native rejection and invalid response cannot be reported as inspected", async t => {
  const f = await fixture(t, "process.stderr.write('PXSAVE6101: integrity verification failed'); process.exitCode = 2;");
  await assert.rejects(inspectSaveWithRuntime(f), /integrity verification failed/);
  await writeFile(f.runtime.player, "process.stdout.write('not JSON');");
  await assert.rejects(inspectSaveWithRuntime(f), /invalid JSON/);
});

test("inspection times out and releases child resources", async t => {
  const f = await fixture(t, "setInterval(() => {}, 1000);");
  await assert.rejects(inspectSaveWithRuntime({...f, timeoutMs: 100}), /timed out/);
});

test("inspection bounds noisy native stdout and diagnostic stderr", async t => {
  const f = await fixture(t, "const chunk=Buffer.alloc(1024*1024,120); for(let i=0;i<65;i++)process.stdout.write(chunk);");
  await assert.rejects(inspectSaveWithRuntime(f), /output exceeds/);
  await writeFile(f.runtime.player, "process.stderr.write('x'.repeat(128*1024)); process.stderr.write('END-DIAGNOSTIC'); process.exitCode=2;");
  await assert.rejects(inspectSaveWithRuntime(f), (error: unknown) =>
    error instanceof Error && error.message.endsWith("END-DIAGNOSTIC") && error.message.length < 66 * 1024);
});

test("--package accepts a distribution folder, its Package folder or the manifest file", async t => {
  const f = await fixture(t, "process.stdout.write(JSON.stringify({args: process.argv.slice(2)}));");
  const distribution = join(f.root, "My Game");
  const manifest = join(distribution, "Package", "manifest.json");
  await mkdir(join(distribution, "Package"), {recursive: true});
  await writeFile(manifest, "{}");
  for (const packagePath of [distribution, join(distribution, "Package"), manifest]) {
    assert.equal(await resolvePackageManifest(packagePath), manifest);
    const result = await inspectSaveWithRuntime({...f, packagePath});
    assert.deepEqual(JSON.parse(result.output).args, ["--inspect-save", f.path, "--package", manifest]);
  }
  await assert.rejects(resolvePackageManifest(f.root), /no Package\/manifest\.json or manifest\.json/u);
  await assert.rejects(resolvePackageManifest(join(f.root, "missing")), /does not exist/u);
});
