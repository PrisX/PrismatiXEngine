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
    const argsFile = join(root, "native-args.json");
    await writeFile(packager,
      "const fs=require('node:fs');\n" +
      `fs.writeFileSync(${JSON.stringify(argsFile)},JSON.stringify(process.argv.slice(2)));\n` +
      "if(process.argv[2]==='--create-overlay'){\n" +
      "  fs.writeFileSync(process.argv[5],'overlay');\n" +
      "  process.stdout.write(JSON.stringify({event:'overlay-created',changedResources:2,removedResources:1,changedRawBytes:20})+'\\n');\n" +
      "}else if(process.argv[2]==='--verify-overlay'){\n" +
      "  process.stdout.write(JSON.stringify({event:'overlay-verified',changedResources:2,removedResources:1,changedRawBytes:20})+'\\n');\n" +
      "}else if(process.argv[2]==='--generate-overlay-key'){\n" +
      "  fs.writeFileSync(process.argv[3],'private'); fs.writeFileSync(process.argv[4],'public');\n" +
      "  process.stdout.write(JSON.stringify({event:'overlay-key-generated'})+'\\n');\n" +
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

    const signingKey = join(root, "signing-key.txt");
    await writeFile(signingKey, "a".repeat(64) + "\n");
    const signedOutput = join(root, "signed.pdx");
    const signed = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", join(root, "base"), "--target", join(root, "target"),
      "--output", signedOutput, "--signing-key", signingKey,
      "--packager", packager, "--json"], {encoding: "utf8"});
    assert.equal(signed.status, 0, signed.stderr);
    assert.deepEqual(JSON.parse(await readFile(argsFile, "utf8")),
      ["--create-overlay", join(root, "base"), join(root, "target"),
        signedOutput, "--signing-key", signingKey]);
    const signedCheck = spawnSync(process.execPath, [cli, "verify-overlay",
      "--base", join(root, "base"), "--patch", signedOutput,
      "--require-signature", "--packager", packager, "--json"],
    {encoding: "utf8"});
    assert.equal(signedCheck.status, 0, signedCheck.stderr);
    assert.deepEqual(JSON.parse(await readFile(argsFile, "utf8")),
      ["--verify-overlay", join(root, "base"), signedOutput,
        "--require-signature"]);

    const generatedPrivate = join(root, "generated-private.txt");
    const generatedPublic = join(root, "generated-public.txt");
    const generated = spawnSync(process.execPath, [cli, "overlay-keygen",
      "--private", generatedPrivate, "--public", generatedPublic,
      "--packager", packager, "--json"], {encoding: "utf8"});
    assert.equal(generated.status, 0, generated.stderr);
    assert.deepEqual(JSON.parse(await readFile(argsFile, "utf8")),
      ["--generate-overlay-key", generatedPrivate, generatedPublic]);
    assert.equal(await readFile(generatedPrivate, "utf8"), "private");

    const missing = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", join(root, "base"), "--packager", packager],
    {encoding: "utf8"});
    assert.equal(missing.status, 2);
    assert.match(missing.stderr, /requires --base, --target, and --output/u);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
