import assert from "node:assert/strict";
import {writeFileSync} from "node:fs";
import {mkdir, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {packageRetentionKey, retainPackageIfMatching,
  writePackageProvenance} from "../src/package-retain.js";
import {outputOwnershipMarker, publishDirectory, publishedInputs,
  type PublicationOptions} from "../src/output-safety.js";

test("unchanged package retention verifies artifact, tools and owned output", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-package-retain-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const projectRoot = join(root, "author");
  const artifact = join(root, "artifact");
  const output = join(root, "dist");
  const toolRoot = join(root, "tools");
  await mkdir(projectRoot);
  await mkdir(toolRoot);
  const packager = join(toolRoot, process.platform === "win32" ? "Packager.exe" : "Packager");
  const player = join(toolRoot, process.platform === "win32" ? "Player.exe" : "Player");
  const library = join(toolRoot, process.platform === "win32" ? "codec.dll" :
    process.platform === "darwin" ? "codec.dylib" : "codec.so");
  await writeFile(packager, "native-packager-v1");
  await writeFile(player, "native-player-v1");
  await writeFile(library, "codec-v1");
  await publishDirectory({output: artifact, projectRoot, protectedPaths: []}, async stage => {
    await mkdir(join(stage, "Runtime"));
    await writeFile(join(stage, "Runtime", "main.pxir"), "runtime-ir");
    await writeFile(join(stage, "project.pxproject"), "canonical-project");
    await writeFile(join(stage, "asset.bin"), "source-asset");
  });
  const inputs = await publishedInputs(artifact);
  const request = {gameId: "sample", archiveShards: 4, inputs};
  const planKey = await packageRetentionKey(packager, player, artifact, request);
  assert.match(planKey ?? "", /^[0-9a-f]{64}$/u);
  const publication: PublicationOptions = {output, projectRoot,
    protectedPaths: [artifact, packager, player]};
  await publishDirectory(publication, async stage => {
    await mkdir(join(stage, "Package"));
    await writeFile(join(stage, "Content.pdx"), "package-payload");
    await writePackageProvenance(stage, planKey!);
  });
  const current = () => packageRetentionKey(packager, player, artifact, request);
  assert.equal(await retainPackageIfMatching(publication, artifact, inputs, planKey!, current), true);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "package-payload");

  const changedRequest = {...request, archiveShards: 8};
  assert.equal(await retainPackageIfMatching(publication, artifact, inputs,
    (await packageRetentionKey(packager, player, artifact, changedRequest))!,
    () => packageRetentionKey(packager, player, artifact, changedRequest)), false);
  await writeFile(library, "codec-v2");
  assert.equal(await retainPackageIfMatching(publication, artifact, inputs, planKey!, current), false);
  await writeFile(library, "codec-v1");

  await writeFile(join(artifact, "asset.bin"), "changed-asset");
  assert.equal(await retainPackageIfMatching(publication, artifact, inputs, planKey!, current), false);
  await writeFile(join(artifact, "asset.bin"), "source-asset");
  const artifactMarkerPath = join(artifact, outputOwnershipMarker);
  const ownedMarker = await readFile(artifactMarkerPath, "utf8");
  const legacyMarker = JSON.parse(ownedMarker) as {files: Record<string, string>};
  delete legacyMarker.files.Runtime;
  await writeFile(artifactMarkerPath, JSON.stringify(legacyMarker));
  assert.equal(await retainPackageIfMatching(publication, artifact, inputs, planKey!, current), false);
  await writeFile(artifactMarkerPath, ownedMarker);
  await writeFile(join(output, "Content.pdx"), "changed-payload");
  await assert.rejects(retainPackageIfMatching(publication, artifact, inputs, planKey!, current),
    /unowned or modified/u);
});

test("late artifact changes cannot pass a retained distribution", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-package-retain-race-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const projectRoot = join(root, "author");
  const artifact = join(root, "artifact");
  const output = join(root, "dist");
  const tools = join(root, "tools");
  await mkdir(projectRoot);
  await mkdir(tools);
  const packager = join(tools, "Packager");
  const player = join(tools, "Player");
  await writeFile(packager, "packager");
  await writeFile(player, "player");
  await publishDirectory({output: artifact, projectRoot, protectedPaths: []},
    async stage => { await writeFile(join(stage, "asset.bin"), "original"); });
  const inputs = await publishedInputs(artifact);
  const request = {gameId: "sample", inputs};
  const planKey = (await packageRetentionKey(packager, player, artifact, request))!;
  const publication: PublicationOptions = {output, projectRoot,
    protectedPaths: [artifact, packager, player]};
  await publishDirectory(publication, async stage => {
    await mkdir(join(stage, "Package"));
    await writeFile(join(stage, "Content.pdx"), "stable");
    await writePackageProvenance(stage, planKey);
  });
  let changed = false;
  assert.equal(await retainPackageIfMatching({...publication,
    onProgress: value => {
      if (value.phase === "verify-before-publish" && value.done && !changed) {
        writeFileSync(join(artifact, "asset.bin"), "tampered");
        changed = true;
      }
    }}, artifact, inputs, planKey,
  () => packageRetentionKey(packager, player, artifact, request)), false);
  assert.equal(changed, true);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "stable");
});

test("script tool overrides never qualify for package retention", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-package-retain-script-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const packager = join(root, "packager.mjs");
  const player = join(root, "Player");
  await writeFile(packager, "process.exit(0)");
  await writeFile(player, "player");
  assert.equal(await packageRetentionKey(packager, player, root, {}), undefined);
});
