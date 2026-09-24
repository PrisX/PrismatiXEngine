import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {fileURLToPath} from "node:url";
import test from "node:test";

import {planPackagePatch} from "../src/package-diff.js";

const cli = fileURLToPath(new URL("../src/cli.js", import.meta.url));
const a = "a".repeat(64);
const b = "b".repeat(64);
const c = "c".repeat(64);

async function packageFixture(root: string, version: string, fingerprint: string,
  entries: readonly {uri: string; fingerprint: string; size: number}[]): Promise<string> {
  const directory = join(root, version);
  await mkdir(join(directory, "Package"), {recursive: true});
  await writeFile(join(directory, "Content-00.pdx"), "fixture");
  await writeFile(join(directory, "Package/manifest.json"), JSON.stringify({
    format: "PrismatiXPackageManifest", schemaRevision: 2,
    gameId: "sample", contentVersion: version, packageFingerprint: fingerprint,
    archives: [{file: "Content-00.pdx", group: "base", optional: false}],
  }));
  await writeFile(join(directory, "Package/content-index.json"), JSON.stringify({
    format: "PrismatiXContentIndex", schemaRevision: 1,
    gameId: "sample", packageFingerprint: fingerprint,
    entries: entries.map((entry) => ({...entry, archive: "Content-00.pdx"})),
  }));
  return directory;
}

test("package patch plan reports added, changed and removed resources without writing", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-patch-plan-"));
  try {
    const base = await packageFixture(root, "v1", a, [
      {uri: "Assets/a.png", fingerprint: a, size: 2},
      {uri: "Assets/b.png", fingerprint: b, size: 3},
      {uri: "Assets/removed.png", fingerprint: c, size: 5},
    ]);
    const target = await packageFixture(root, "v2", b, [
      {uri: "Assets/a.png", fingerprint: a, size: 2},
      {uri: "Assets/b.png", fingerprint: c, size: 7},
      {uri: "Assets/new.png", fingerprint: b, size: 11},
    ]);
    const plan = await planPackagePatch(base, target);
    assert.deepEqual(plan.added.map((entry) => entry.uri), ["Assets/new.png"]);
    assert.deepEqual(plan.modified.map((entry) => entry.after.uri), ["Assets/b.png"]);
    assert.deepEqual(plan.removed.map((entry) => entry.uri), ["Assets/removed.png"]);
    assert.equal(plan.unchangedCount, 1);
    assert.equal(plan.changedRawBytes, "18");
    assert.deepEqual(plan.affectedTargetArchives, ["Content-00.pdx"]);

    const command = spawnSync(process.execPath, [cli, "plan-patch",
      "--base", base, "--target", target, "--json"], {encoding: "utf8"});
    assert.equal(command.status, 0, command.stderr);
    const response = JSON.parse(command.stdout) as {patchWritten: boolean;
      patchPlan: {changedRawBytes: string}};
    assert.equal(response.patchWritten, false);
    assert.equal(response.patchPlan.changedRawBytes, "18");

    const packager = join(root, "verify-packager.js");
    await writeFile(packager,
      "if (process.argv[2] !== '--verify-package') process.exit(2); " +
      "process.stdout.write(JSON.stringify({event:'verified',archives:1,resources:3,rawBytes:10})+'\\n');\n");
    const verifiedCommand = spawnSync(process.execPath, [cli, "plan-patch",
      "--base", base, "--target", target, "--verify-contents",
      "--packager", packager, "--json"], {encoding: "utf8"});
    assert.equal(verifiedCommand.status, 0, verifiedCommand.stderr);
    const verifiedResponse = JSON.parse(verifiedCommand.stdout) as {
      contentsVerified: boolean; patchWritten: boolean};
    assert.equal(verifiedResponse.contentsVerified, true);
    assert.equal(verifiedResponse.patchWritten, false);
    const directCommand = spawnSync(process.execPath, [cli, "verify-package",
      target, "--packager", packager, "--json"], {encoding: "utf8"});
    assert.equal(directCommand.status, 0, directCommand.stderr);
    const directResponse = JSON.parse(directCommand.stdout) as {
      verified: boolean; resources: number};
    assert.equal(directResponse.verified, true);
    assert.equal(directResponse.resources, 3);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("patch planning rejects mismatched games and malformed content indexes", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-patch-plan-invalid-"));
  try {
    const base = await packageFixture(root, "v1", a,
      [{uri: "Assets/a.png", fingerprint: a, size: 2}]);
    const target = await packageFixture(root, "v2", b,
      [{uri: "Assets/new.png", fingerprint: b, size: 4}]);
    const manifestPath = join(target, "Package/manifest.json");
    const manifest = JSON.parse(await readFile(manifestPath, "utf8")) as Record<string, unknown>;
    await writeFile(manifestPath, JSON.stringify({...manifest, gameId: "other"}));
    await assert.rejects(() => planPackagePatch(base, target), /disagree|different games/u);
    await writeFile(manifestPath, JSON.stringify(manifest));
    const indexPath = join(target, "Package/content-index.json");
    const index = JSON.parse(await readFile(indexPath, "utf8")) as Record<string, unknown>;
    await writeFile(indexPath, JSON.stringify({...index,
      entries: [{uri: "../escape", fingerprint: b, size: 4,
        archive: "Content-00.pdx"}]}));
    await assert.rejects(() => planPackagePatch(base, target), /invalid entry/u);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
