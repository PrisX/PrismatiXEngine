import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {fileURLToPath} from "node:url";
import test from "node:test";

const cli = fileURLToPath(new URL("../src/cli.js", import.meta.url));

test("overlay CLI delegates creation and verification to native Packager", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-overlay-cli-"));
  try {
    const packager = join(root, "packager.js");
    await writeFile(packager,
      "const fs=require('node:fs');\n" +
      "if(process.argv[2]==='--create-overlay'){\n" +
      "  fs.writeFileSync(process.argv[5],'overlay');\n" +
      "  process.stdout.write(JSON.stringify({event:'overlay-created',changedResources:2,removedResources:1,changedRawBytes:20})+'\\n');\n" +
      "}else if(process.argv[2]==='--verify-overlay'){\n" +
      "  process.stdout.write(JSON.stringify({event:'overlay-verified',changedResources:2,removedResources:1,changedRawBytes:20})+'\\n');\n" +
      "}else process.exit(2);\n");
    const output = join(root, "patch.pdx");
    const created = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", join(root, "base"), "--target", join(root, "target"),
      "--output", output, "--packager", packager, "--json"],
    {encoding: "utf8"});
    assert.equal(created.status, 0, created.stderr);
    assert.equal(await readFile(output, "utf8"), "overlay");
    const creation = JSON.parse(created.stdout.trim()) as {
      overlayCreated: boolean; changedResources: number; removedResources: number};
    assert.equal(creation.overlayCreated, true);
    assert.equal(creation.changedResources, 2);
    assert.equal(creation.removedResources, 1);

    const checked = spawnSync(process.execPath, [cli, "verify-overlay",
      "--base", join(root, "base"), "--patch", output,
      "--packager", packager, "--json"], {encoding: "utf8"});
    assert.equal(checked.status, 0, checked.stderr);
    const verification = JSON.parse(checked.stdout.trim()) as {
      overlayVerified: boolean; changedRawBytes: number};
    assert.equal(verification.overlayVerified, true);
    assert.equal(verification.changedRawBytes, 20);

    const missing = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", join(root, "base"), "--packager", packager],
    {encoding: "utf8"});
    assert.equal(missing.status, 2);
    assert.match(missing.stderr, /requires --base, --target, and --output/u);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
