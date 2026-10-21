import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {mkdtemp, mkdir, readFile, readdir, rm, symlink, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {augmentArtifact, augmentationOwnershipPath,
  validateArtifactAugmentation} from "../src/artifact-augment.js";
import {outputOwnershipMarker, publishDirectory, publishedInputs} from "../src/output-safety.js";

async function fixture(t: test.TestContext) {
  const temp = await mkdtemp(join(tmpdir(), "px-augment-"));
  t.after(() => rm(temp, {recursive: true, force: true}));
  const projectRoot = join(temp, "project");
  const artifactRoot = join(projectRoot, ".prismatix", "artifact");
  await mkdir(join(projectRoot, "Assets"), {recursive: true});
  await writeFile(join(projectRoot, "Assets", "new.png"), "new image");
  await writeFile(join(projectRoot, "Assets", "replacement.png"), "replacement image");
  await publishDirectory({output: artifactRoot, projectRoot, protectedPaths: []}, async stage => {
    await mkdir(join(stage, "Assets"));
    await writeFile(join(stage, "Assets", "existing.png"), "original image");
    await writeFile(join(stage, "project.pxproject"), "canonical project");
  });
  const original = (await publishedInputs(artifactRoot)).find(item =>
    item.uri === "Assets/existing.png")!;
  return {temp, projectRoot, artifactRoot, original};
}

test("augment atomically adds and replaces owned artifact files without modifying the source", async t => {
  const f = await fixture(t);
  const manifest = {format: "PrismatiXArtifactAugmentation", schemaRevision: 1,
    owner: "custom-ui", files: [
      {source: "Assets/new.png", target: "Assets/new.png", mode: "add"},
      {source: "Assets/replacement.png", target: "Assets/existing.png", mode: "replace",
        expectedFingerprint: f.original.fingerprint},
    ]};
  let sawOldOutputDuringPublication = false;
  let sawCloneProgress = false;
  const result = await augmentArtifact({...f, manifest, onProgress: (value) => {
    if (value.phase === "clone-existing" && value.done && value.files === 2)
      sawCloneProgress = true;
    if (value.phase === "fingerprint-output" && value.done) {
      assert.equal(readFileSync(join(f.artifactRoot, "Assets", "existing.png"), "utf8"),
        "original image");
      sawOldOutputDuringPublication = true;
    }
  }});
  assert.deepEqual([result.added, result.replaced, result.owner], [1, 1, "custom-ui"]);
  assert.equal(sawOldOutputDuringPublication, true);
  assert.equal(sawCloneProgress, true);
  assert.equal(await readFile(join(f.artifactRoot, "Assets", "existing.png"), "utf8"),
    "replacement image");
  assert.equal(await readFile(join(f.artifactRoot, "Assets", "new.png"), "utf8"), "new image");
  const owners = JSON.parse(await readFile(join(f.artifactRoot, augmentationOwnershipPath), "utf8"));
  assert.equal(owners.owners["Assets/existing.png"], "custom-ui");
  assert.equal(owners.owners["Assets/new.png"], "custom-ui");
  assert.equal((await publishedInputs(f.artifactRoot)).some(item =>
    item.uri === augmentationOwnershipPath), true);
  await publishDirectory({output: f.artifactRoot, projectRoot: f.projectRoot,
    protectedPaths: []}, async stage => writeFile(join(stage, "project.pxproject"), "next build"));
  assert.equal(await readFile(join(f.artifactRoot, "project.pxproject"), "utf8"), "next build");
  assert.deepEqual((await readdir(f.artifactRoot)).sort(),
    [outputOwnershipMarker, "project.pxproject"].sort());
});

test("augment rejects stale, conflicting, unsafe and externally modified inputs without publishing", async t => {
  const f = await fixture(t);
  const add = {format: "PrismatiXArtifactAugmentation", schemaRevision: 1,
    owner: "one", files: [{source: "Assets/new.png", target: "Assets/new.png", mode: "add"}]};
  await assert.rejects(augmentArtifact({...f, manifest: {...add,
    files: [{source: "Assets/replacement.png", target: "Assets/existing.png",
      mode: "replace", expectedFingerprint: "0".repeat(64)}]}}), /stale/u);
  assert.equal(await readFile(join(f.artifactRoot, "Assets", "existing.png"), "utf8"),
    "original image");
  await augmentArtifact({...f, manifest: add});
  const current = (await publishedInputs(f.artifactRoot)).find(item =>
    item.uri === "Assets/new.png")!;
  await assert.rejects(augmentArtifact({...f, manifest: {format: add.format,
    schemaRevision: add.schemaRevision, owner: "two", files: [{
      source: "Assets/replacement.png", target: "Assets/new.png", mode: "replace",
      expectedFingerprint: current.fingerprint,
    }]}}), /another augmentation owner/u);
  assert.equal(await readFile(join(f.artifactRoot, "Assets", "new.png"), "utf8"), "new image");
  for (const target of ["../escape", outputOwnershipMarker, augmentationOwnershipPath])
    assert.throws(() => validateArtifactAugmentation({...add,
      files: [{source: "Assets/new.png", target, mode: "add"}]}));
  await assert.rejects(augmentArtifact({...f, manifest: {...add,
    files: [{source: ".prismatix/artifact/project.pxproject",
      target: "Assets/self-copy.bin", mode: "add"}]}}), /overlaps author input/u);
  const linked = join(f.projectRoot, "Assets", "linked.png");
  try {
    await symlink(join(f.projectRoot, "Assets", "new.png"), linked);
    await assert.rejects(augmentArtifact({...f, manifest: {...add,
      files: [{source: "Assets/linked.png", target: "Assets/linked.png", mode: "add"}]}}),
    /not a regular file/u);
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "EPERM") throw error;
  }
  await writeFile(join(f.artifactRoot, "Assets", "new.png"), "tampered");
  await assert.rejects(augmentArtifact({...f, manifest: {format: add.format,
    schemaRevision: add.schemaRevision, owner: "one", files: [{
      source: "Assets/replacement.png", target: "Assets/existing.png", mode: "replace",
      expectedFingerprint: f.original.fingerprint,
    }]}}), /unowned or modified/u);
  assert.equal(await readFile(join(f.artifactRoot, "Assets", "new.png"), "utf8"), "tampered");
});
