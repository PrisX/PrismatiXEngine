import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {mkdir, mkdtemp, readFile, realpath, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join, sep} from "node:path";
import {fileURLToPath} from "node:url";
import test from "node:test";
import {createStarter} from "../src/starter.js";

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

    const optionalOutput = join(root, "adult.pdx");
    const optional = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", join(root, "base"), "--target", join(root, "target"),
      "--output", optionalOutput, "--signing-key", signingKey,
      "--optional-id", "adult", "--packager", packager, "--json"],
    {encoding: "utf8"});
    assert.equal(optional.status, 0, optional.stderr);
    assert.deepEqual(JSON.parse(await readFile(argsFile, "utf8")),
      ["--create-overlay", join(root, "base"), join(root, "target"),
        optionalOutput, "--signing-key", signingKey, "--optional-id", "adult"]);
    const unsignedOptional = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", join(root, "base"), "--target", join(root, "target"),
      "--output", join(root, "invalid.pdx"), "--optional-id", "adult",
      "--packager", packager, "--json"], {encoding: "utf8"});
    assert.notEqual(unsignedOptional.status, 0);

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
    assert.match(missing.stderr, /requires --base, --output, and exactly one of --target or --artifact/u);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("overlay CLI plans a signed patch from a published artifact", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-direct-overlay-cli-"));
  try {
    const starter = await createStarter(join(root, "game"), {
      name: "Direct overlay", install: async () => {},
    });
    const packagePath = join(starter.root, "package.json");
    const packageManifest = JSON.parse(await readFile(packagePath, "utf8")) as {
      devDependencies?: Record<string, string>};
    delete packageManifest.devDependencies?.["@prismatix/cli"];
    await writeFile(packagePath, JSON.stringify(packageManifest));
    const artifact = join(root, "artifact");
    const built = spawnSync(process.execPath,
      [cli, "build", starter.project, "--artifact-only", "--output", artifact,
        "--json"], {encoding: "utf8"});
    assert.equal(built.status, 0, built.stderr);
    const project = JSON.parse(await readFile(join(artifact,
      "project.pxproject"), "utf8")) as {id: string};
    const base = join(root, "base");
    await mkdir(join(base, "Package"), {recursive: true});
    await writeFile(join(base, "Package", "manifest.json"), JSON.stringify({
      gameId: project.id, publisherPublicKey: "", encryption: {enabled: false},
    }));
    await writeFile(join(base, "Package", "archive-shards.json"),
      JSON.stringify({archiveShards: 4}));
    const player = join(root, "Player.exe");
    await writeFile(player, "fixture player");
    const packager = join(root, "packager.js");
    const argsFile = join(root, "native-args.json");
    await writeFile(packager,
      "const fs=require('node:fs');\n" +
      `fs.writeFileSync(${JSON.stringify(argsFile)},JSON.stringify(process.argv.slice(2)));\n` +
      "const args=process.argv.slice(2);\n" +
      "if(args[0]!=='--create-overlay-from-request')process.exit(40);\n" +
      "const request=JSON.parse(fs.readFileSync(args[1],'utf8'));\n" +
      `if(request.projectRoot!==${JSON.stringify(artifact)}||` +
      `request.cacheSourceDir!==${JSON.stringify(base)}||` +
      "request.archiveShards!==4||request.inputs.length===0)process.exit(41);\n" +
      "fs.writeFileSync(args[3],'overlay');\n" +
      "process.stdout.write(JSON.stringify({event:'overlay-created'," +
      "changedResources:1,removedResources:0,changedRawBytes:7})+'\\n');\n");
    const signingKey = join(root, "signing-key.txt");
    await writeFile(signingKey, "a".repeat(64));
    const output = join(root, "patch.pdx");
    const created = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", base, "--artifact", artifact, "--output", output,
      "--signing-key", signingKey, "--packager", packager,
      "--player", player, "--json"], {encoding: "utf8"});
    assert.equal(created.status, 0, created.stderr);
    assert.equal(await readFile(output, "utf8"), "overlay");
    const argumentsUsed = JSON.parse(await readFile(argsFile, "utf8")) as string[];
    assert.equal(argumentsUsed[0], "--create-overlay-from-request");
    assert.equal(argumentsUsed[2], base);
    assert.equal(argumentsUsed[3], output);
    assert.deepEqual(argumentsUsed.slice(4), ["--signing-key", signingKey]);
    const ambiguous = spawnSync(process.execPath, [cli, "create-overlay",
      "--base", base, "--artifact", artifact, "--target", root,
      "--output", join(root, "invalid.pdx"), "--packager", packager,
      "--player", player], {encoding: "utf8"});
    assert.equal(ambiguous.status, 2);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("native CLI builds and verifies a signed overlay directly from an artifact", {
  skip: !process.env.PRISMATIX_NATIVE_PACKAGER || !process.env.PRISMATIX_NATIVE_PLAYER,
}, async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-direct-overlay-native-"));
  try {
    const starter = await createStarter(join(root, "game"), {
      name: "Direct overlay native", install: async () => {},
    });
    const packagePath = join(starter.root, "package.json");
    const packageManifest = JSON.parse(await readFile(packagePath, "utf8")) as {
      devDependencies?: Record<string, string>};
    delete packageManifest.devDependencies?.["@prismatix/cli"];
    await writeFile(packagePath, JSON.stringify(packageManifest));
    const run = (args: string[]): Record<string, unknown> => {
      const result = spawnSync(process.execPath, [cli, ...args, "--json"], {
        encoding: "utf8", maxBuffer: 8 * 1024 * 1024,
      });
      assert.equal(result.status, 0, result.stderr);
      return JSON.parse(result.stdout.trim().split(/\r?\n/u).at(-1)!) as Record<string, unknown>;
    };
    const packager = process.env.PRISMATIX_NATIVE_PACKAGER!;
    const player = process.env.PRISMATIX_NATIVE_PLAYER!;
    const firstArtifact = join(root, "artifact-v1");
    const nextArtifact = join(root, "artifact-v2");
    const base = join(root, "base");
    const patch = join(root, "patch.pdx");
    const privateKey = join(root, "private.txt");
    const publicKey = join(root, "public.txt");
    run(["build", starter.project, "--artifact-only", "--output", firstArtifact]);
    run(["overlay-keygen", "--private", privateKey, "--public", publicKey,
      "--packager", packager]);
    run(["pack", starter.project, "--artifact", firstArtifact, "--output", base,
      "--publisher-key", publicKey, "--packager", packager, "--player", player]);
    run(["build", starter.project, "--artifact-only", "--output", nextArtifact]);
    await writeFile(join(starter.root, "bonus.bin"), "direct overlay payload");
    const augmentation = join(starter.root, "augmentation.json");
    await writeFile(augmentation, JSON.stringify({
      format: "PrismatiXArtifactAugmentation", schemaRevision: 1,
      owner: "direct-overlay", files: [{source: "bonus.bin",
        target: "Content/Overlay/bonus.bin", mode: "add"}],
    }));
    run(["augment", starter.project, "--artifact", nextArtifact,
      "--manifest", augmentation]);
    const created = run(["create-overlay", "--base", base, "--artifact", nextArtifact,
      "--output", patch, "--signing-key", privateKey,
      "--packager", packager, "--player", player]);
    const verified = run(["verify-overlay", "--base", base, "--patch", patch,
      "--require-signature", "--packager", packager]);
    assert.equal(created.overlayCreated, true);
    assert.equal(verified.overlayVerified, true);
    assert.ok(Number(created.changedResources) > 0);
  } finally {
    const realRoot = await realpath(root);
    const tempRoot = await realpath(tmpdir());
    assert.ok(realRoot.startsWith(tempRoot + sep));
    await rm(realRoot, {recursive: true, force: true});
  }
});
