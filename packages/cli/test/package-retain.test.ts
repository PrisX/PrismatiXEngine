import assert from "node:assert/strict";
import {writeFileSync} from "node:fs";
import {mkdir, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {packageContentKey, packageRetentionKey, refreshRuntimePackageIfMatching,
  retainPackageIfMatching, reusedArchiveInputs, runtimeReplacementFiles,
  writePackageProvenance} from "../src/package-retain.js";
import {outputOwnershipMarker, publishDirectory, publishedInputs,
  type PublicationOptions} from "../src/output-safety.js";

test("only native whole-shard reuse reports select owned archive fingerprints", () => {
  const previous = [
    {uri: "Content-00.pdx", size: 100, fingerprint: "a".repeat(64)},
    {uri: "Content-01.pdx", size: 200, fingerprint: "b".repeat(64)},
  ];
  const events = [
    {event: "progress", phase: "archive-write", message: "Reused unchanged Content-00.pdx"},
    {event: "progress", phase: "archive-write", message: "Writing Content-01.pdx"},
    {event: "progress", phase: "archive-write", message: "Reused unchanged Content-02.pdx"},
    {event: "progress", phase: "archive-write", message: "Reused unchanged ../asset.bin"},
    {event: "failed", phase: "archive-write", message: "Reused unchanged Content-01.pdx"},
  ];
  assert.deepEqual([...reusedArchiveInputs(events, previous).keys()], ["Content-00.pdx"]);
});

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

test("runtime-only refresh replaces Player, libraries and notices without rebuilding owned content", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-runtime-refresh-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const projectRoot = join(root, "author");
  const artifact = join(root, "artifact");
  const output = join(root, "dist");
  const packagerRoot = join(root, "packager");
  const playerRoot = join(root, "runtime", "bin");
  const share = join(root, "runtime", "share", "PrismatiX");
  await mkdir(projectRoot);
  await mkdir(packagerRoot);
  await mkdir(playerRoot, {recursive: true});
  await mkdir(join(share, "licenses"), {recursive: true});
  const packager = join(packagerRoot, process.platform === "win32" ? "Packager.exe" : "Packager");
  const player = join(playerRoot, process.platform === "win32" ? "Player.exe" : "Player");
  const libraryName = process.platform === "win32" ? "codec.dll" :
    process.platform === "darwin" ? "codec.dylib" : "codec.so";
  const library = join(playerRoot, libraryName);
  await writeFile(packager, "packager-v1");
  await writeFile(player, "player-v1");
  await writeFile(library, "library-v1");
  await writeFile(join(share, "LICENSE"), "license-v1");
  await writeFile(join(share, "licenses", "component.txt"), "notice-v1");
  await publishDirectory({output: artifact, projectRoot, protectedPaths: []}, async stage => {
    await writeFile(join(stage, "asset.bin"), "asset-v1");
  });
  const inputs = await publishedInputs(artifact);
  const request = {gameId: "sample", playerExecutable: player, inputs};
  const oldPlan = (await packageRetentionKey(packager, player, artifact, request))!;
  const content = (await packageContentKey(packager, artifact, request))!;
  const oldSources = (await runtimeReplacementFiles(player))!;
  const publication: PublicationOptions = {output, projectRoot,
    protectedPaths: [artifact, packager, player]};
  await publishDirectory(publication, async stage => {
    await mkdir(join(stage, "Package"));
    await mkdir(join(stage, "Notices", "licenses"), {recursive: true});
    await writeFile(join(stage, "Content.pdx"), "unchanged-content");
    await writeFile(join(stage, "Package", "manifest.json"), "unchanged-manifest");
    for (const [uri, source] of oldSources) {
      const target = join(stage, ...uri.split("/"));
      await mkdir(join(target, ".."), {recursive: true});
      await writeFile(target, await readFile(source));
    }
    await writePackageProvenance(stage, oldPlan, content, [...oldSources.keys()].sort());
  });
  await writeFile(player, "player-v2");
  await writeFile(library, "library-v2");
  await writeFile(join(share, "LICENSE"), "license-v2");
  await rm(join(share, "licenses", "component.txt"));
  await writeFile(join(share, "licenses", "new-component.txt"), "notice-v2");
  const newSources = (await runtimeReplacementFiles(player))!;
  const newPlan = (await packageRetentionKey(packager, player, artifact, request))!;
  assert.notEqual(newPlan, oldPlan);
  assert.equal(await packageContentKey(packager, artifact, request), content);
  const refresh = () => refreshRuntimePackageIfMatching(publication, artifact, inputs,
    newPlan, content, newSources,
    () => packageRetentionKey(packager, player, artifact, request),
    () => packageContentKey(packager, artifact, request));
  assert.equal(await refresh(), true);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "unchanged-content");
  assert.equal(await readFile(join(output, "Package", "manifest.json"), "utf8"), "unchanged-manifest");
  assert.equal(await readFile(join(output, process.platform === "win32" ? "Player.exe" : "Player"), "utf8"), "player-v2");
  assert.equal(await readFile(join(output, libraryName), "utf8"), "library-v2");
  assert.equal(await readFile(join(output, "Notices", "LICENSE"), "utf8"), "license-v2");
  assert.equal(await readFile(join(output, "Notices", "licenses", "new-component.txt"), "utf8"), "notice-v2");
  await assert.rejects(readFile(join(output, "Notices", "licenses", "component.txt")), /ENOENT/u);
  assert.equal(await retainPackageIfMatching(publication, artifact, inputs, newPlan,
    () => packageRetentionKey(packager, player, artifact, request)), true);

  // Same-size tampering of the artifact must be detected before promotion.
  await writeFile(join(artifact, "asset.bin"), "asset-v2");
  await writeFile(player, "player-v3");
  const artifactTamperPlan = (await packageRetentionKey(packager, player, artifact, request))!;
  const artifactTamperSources = (await runtimeReplacementFiles(player))!;
  await assert.rejects(refreshRuntimePackageIfMatching(publication, artifact, inputs,
    artifactTamperPlan, content, artifactTamperSources,
    () => packageRetentionKey(packager, player, artifact, request),
    () => packageContentKey(packager, artifact, request)), /modified|changed/u);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "unchanged-content");
  assert.equal(await readFile(join(output, process.platform === "win32" ? "Player.exe" : "Player"), "utf8"), "player-v2");
  await writeFile(join(artifact, "asset.bin"), "asset-v1");

  // An edited distribution is never silently overwritten by this fast path.
  await writeFile(join(output, "Content.pdx"), "tampered");
  const nextPlan = (await packageRetentionKey(packager, player, artifact, request))!;
  const nextSources = (await runtimeReplacementFiles(player))!;
  await assert.rejects(refreshRuntimePackageIfMatching(publication, artifact, inputs,
    nextPlan, content, nextSources,
    () => packageRetentionKey(packager, player, artifact, request),
    () => packageContentKey(packager, artifact, request)), /unowned or modified/u);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "tampered");
});

test("runtime refresh falls back for legacy provenance or changed content inputs", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-runtime-refresh-fallback-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const projectRoot = join(root, "author");
  const artifact = join(root, "artifact");
  const output = join(root, "dist");
  const packagerRoot = join(root, "tools");
  await mkdir(projectRoot);
  await mkdir(packagerRoot);
  const packager = join(packagerRoot, "Packager");
  const player = join(packagerRoot, "Player");
  await writeFile(packager, "packager-v1");
  await writeFile(player, "player-v1");
  await publishDirectory({output: artifact, projectRoot, protectedPaths: []}, async stage => {
    await writeFile(join(stage, "asset.bin"), "asset-v1");
  });
  const inputs = await publishedInputs(artifact);
  const request = {gameId: "sample", playerExecutable: player, inputs};
  const plan = (await packageRetentionKey(packager, player, artifact, request))!;
  const content = (await packageContentKey(packager, artifact, request))!;
  const sources = (await runtimeReplacementFiles(player))!;
  const publication: PublicationOptions = {output, projectRoot,
    protectedPaths: [artifact, packager, player]};
  await publishDirectory(publication, async stage => {
    await mkdir(join(stage, "Package"));
    await writeFile(join(stage, "Content.pdx"), "package-payload");
    await writeFile(join(stage, "Package", "manifest.json"), "manifest");
    await writeFile(join(stage, "Player"), "player-v1");
    await writePackageProvenance(stage, plan);
  });
  const refresh = (contentKey: string) => refreshRuntimePackageIfMatching(publication,
    artifact, inputs, plan, contentKey, sources,
    () => packageRetentionKey(packager, player, artifact, request),
    () => packageContentKey(packager, artifact, request));
  assert.equal(await refresh(content), false);
  assert.equal(await readFile(join(output, "Content.pdx"), "utf8"), "package-payload");
  await writeFile(packager, "packager-v2");
  assert.notEqual(await packageContentKey(packager, artifact, request), content);
  assert.equal(await refresh((await packageContentKey(packager, artifact, request))!), false);
});
