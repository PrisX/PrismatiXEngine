import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {join} from "node:path";
import {tmpdir} from "node:os";
import test from "node:test";
import {exportWeb, resolveWebPlayer} from "../src/web-export.js";

test("exports PDX files in verified, bounded chunks and retains the native manifest", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-web-export-"));
  try {
    const player = join(root, "runtime"), project = join(root, "project"), distribution = join(root, "native");
    for (const folder of [player, project, join(distribution, "Package")]) await mkdir(folder, {recursive: true});
    const artifacts: Record<string, string> = {};
    for (const [filename, bytes] of [["prismatix-player.mjs", Buffer.from("export default()=>{}")],
      ["prismatix-player.wasm", Buffer.from([0, 97, 115, 109, 1, 0, 0, 0])],
      ["web-shaders.json", Buffer.from("{}")], ["license.txt", Buffer.from("Runtime license")],
      ["RuntimeSource.tar.gz", Buffer.from("Corresponding source archive")]] as const) {
      await writeFile(join(player, filename), bytes);
      artifacts[filename] = createHash("sha256").update(bytes).digest("hex");
    }
    await writeFile(join(player, "web-player.json"), JSON.stringify({format: "PrismatiXWebPlayer", schemaRevision: 1, variant: "shipped", artifacts,
      notices: ["license.txt"], sources: ["RuntimeSource.tar.gz"]}));
    const original = {format: "PrismatiXPackageManifest", schemaRevision: 1, gameId: "fixture", title: "Fixture",
      packageFingerprint: "a".repeat(64), archives: [{file: "Content.pdx"}], resolution: {width: 1920, height: 1080}};
    await writeFile(join(distribution, "Package", "manifest.json"), JSON.stringify(original));
    await writeFile(join(distribution, "Package", "content-index.json"), JSON.stringify({entries: []}));
    const bytes = Buffer.alloc(1024 * 1024 + 15, 42);
    await writeFile(join(distribution, "Content.pdx"), bytes);
    await writeFile(join(distribution, "native.exe"), "desktop");
    const output = join(project, "dist", "web");
    const result = await exportWeb({projectRoot: project, distribution, playerDirectory: player, output});
    assert.equal(result.target, "web");
    const manifest = JSON.parse(await readFile(join(output, "web-export.json"), "utf8")) as
      {files: {path: string; chunks: {url: string; size: number; sha256: string}[]}[]};
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
    assert.equal(await readFile(join(output, "license.txt"), "utf8"), "Runtime license");
    assert.equal(await readFile(join(output, "RuntimeSource.tar.gz"), "utf8"), "Corresponding source archive");
    await writeFile(join(player, "RuntimeSource.tar.gz"), "tampered source");
    await assert.rejects(resolveWebPlayer(player), /checksum mismatch: RuntimeSource/);
    await writeFile(join(player, "RuntimeSource.tar.gz"), "Corresponding source archive");
    assert.deepEqual(JSON.parse(await readFile(join(distribution, "Package", "manifest.json"), "utf8")), original);
    await writeFile(join(player, "prismatix-player.mjs"), "tampered");
    await assert.rejects(resolveWebPlayer(player), /checksum mismatch/);
    await assert.rejects(exportWeb({projectRoot: project, distribution, playerDirectory: player, output}), /checksum mismatch/);
    assert.deepEqual(await readFile(join(distribution, "Content.pdx")), bytes);
  } finally { await rm(root, {recursive: true, force: true}); }
});
