import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {spawnSync} from "node:child_process";

const executable = process.argv[2];
if (!executable) throw new Error("Usage: test-package-verifier-cold-start.mjs <PrismatiXPackager>");
const temporaryParent = resolve(tmpdir());
const directory = await mkdtemp(join(temporaryParent, "prismatix-verifier-cold-"));
const entries = [];
const archives = [];
let rawBytes = 0;
const crc32 = bytes => {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (~crc) >>> 0;
};

try {
  await mkdir(join(directory, "Package"));
  // Independent, plain PDX5 fixtures ensure no encryption or archive writer
  // initializes PSA before the fresh Packager process starts four workers.
  for (let shard = 0; shard < 4; ++shard) {
    const file = `Content-0${shard}.pdx`;
    archives.push({file, group: "base", optional: false});
    const payloads = [];
    const count = Buffer.alloc(4);
    count.writeUInt32LE(256);
    const index = [count];
    let offset = 28;
    for (let entry = 0; entry < 256; ++entry) {
      const uri = `Data/shard-${shard}/${String(entry).padStart(4, "0")}.bin`;
      const name = Buffer.from(uri);
      const data = Buffer.from(`${uri}/`.repeat(64));
      let pathHash = 1469598103934665603n;
      for (const byte of name) pathHash = BigInt.asUintN(64, (pathHash ^ BigInt(byte)) * 1099511628211n);
      const record = Buffer.alloc(39 + name.length);
      record.writeBigUInt64LE(pathHash);
      record.writeUInt16LE(name.length, 8);
      name.copy(record, 10);
      record.writeBigUInt64LE(BigInt(offset), 10 + name.length);
      record.writeBigUInt64LE(BigInt(data.length), 18 + name.length);
      record.writeBigUInt64LE(BigInt(data.length), 26 + name.length);
      record.writeUInt32LE(crc32(data), 34 + name.length);
      index.push(record);
      payloads.push(data);
      offset += data.length;
      rawBytes += data.length;
      entries.push({uri, archive: file, size: data.length,
        fingerprint: createHash("sha256").update(data).digest("hex")});
    }
    const indexData = Buffer.concat(index);
    const header = Buffer.alloc(28);
    header.write("PDX5");
    header.writeUInt32LE(5, 4);
    header.writeBigUInt64LE(BigInt(offset), 12);
    header.writeBigUInt64LE(BigInt(indexData.length), 20);
    await writeFile(join(directory, file), Buffer.concat([header, ...payloads, indexData]));
  }
  const packageFingerprint = "f".repeat(64);
  const gameId = "package-verifier-cold-start";
  await writeFile(join(directory, "Package/manifest.json"), JSON.stringify({
    format: "PrismatiXPackageManifest", schemaRevision: 1, engineVersion: "0.2.0",
    gameId, title: "Verifier cold start", contentVersion: "1", saveVersion: 1,
    graphicsTier: "basic", packageFingerprint, resolution: {width: 1280, height: 720},
    encryption: {enabled: false, archiveKey: ""}, archives, customEffects: [],
    extensions: [], saveMigrations: [],
    entry: {route: "title", runtimeIr: "Runtime/main.pxir", sourceMap: "Runtime/main.pxmap"},
    routes: [{id: "title", scene: "Content/UI/Title.pxui", sceneId: "ca266002-9f2e-5fac-bb9c-3e0f96bdeb10"}],
    persistence: {dataId: gameId, keyId: "game-data-v1", compatibleSaveFingerprints: [], legacyReaders: []},
  }));
  await writeFile(join(directory, "Package/content-index.json"), JSON.stringify({
    format: "PrismatiXContentIndex", schemaRevision: 1, gameId, packageFingerprint,
    entries: entries.sort((a, b) => a.uri < b.uri ? -1 : a.uri > b.uri ? 1 : 0),
  }));
  const run = () => spawnSync(resolve(executable), ["--verify-package", directory], {
    encoding: "utf8", timeout: 30000, maxBuffer: 2 * 1024 * 1024,
    env: {...process.env, PRISMATIX_PACKAGE_VERIFY_WORKERS: "4"},
  });
  for (let iteration = 0; iteration < 32; ++iteration) {
    const result = run();
    assert.equal(result.status, 0,
      `Fresh process ${iteration + 1} failed: ${result.error ?? result.signal ?? "exit"}\n${result.stdout}\n${result.stderr}`);
    const report = result.stdout.split(/\r?\n/u).filter(line => line.startsWith("{"))
      .map(line => JSON.parse(line)).find(value => value.event === "verified");
    assert.deepEqual(report, {event: "verified", archives: 4, resources: 1024, rawBytes});
  }
  const archivePath = join(directory, archives[0].file);
  const corrupted = await readFile(archivePath);
  corrupted[28] ^= 1;
  await writeFile(archivePath, corrupted);
  const rejected = run();
  assert.notEqual(rejected.status, 0, "The same verifier must reject corrupted payload bytes");
  assert.match(rejected.stdout, /PXPKG1511/u);
  console.log("PASS: 32 cold verifier processes, four archive workers, and payload corruption rejection");
} finally {
  assert.equal(dirname(directory), temporaryParent);
  assert.ok(directory.startsWith(join(temporaryParent, "prismatix-verifier-cold-")));
  await rm(directory, {recursive: true, force: true});
}
