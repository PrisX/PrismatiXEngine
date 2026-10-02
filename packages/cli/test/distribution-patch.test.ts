import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {copyFile, mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join} from "node:path";
import {fileURLToPath} from "node:url";
import test from "node:test";

import {applyDistributionPatch, createDistributionPatch} from "../src/distribution-patch.js";
import {publishDirectory, verifiedPublishedInputs} from "../src/output-safety.js";

const cli = fileURLToPath(new URL("../src/cli.js", import.meta.url));
const a = "a".repeat(64);
const b = "b".repeat(64);
const c = "c".repeat(64);

async function distribution(root: string, name: string, target: boolean): Promise<string> {
  const output = join(root, name);
  await publishDirectory({output, projectRoot: join(root, "author"),
    protectedPaths: []}, async (stage) => {
    await mkdir(join(stage, "Package"), {recursive: true});
    await writeFile(join(stage, "Content-00.pdx"), target ? "target-pdx" : "base-pdx");
    await writeFile(join(stage, "Package/manifest.json"), JSON.stringify({
      format: "PrismatiXPackageManifest", schemaRevision: 1,
      gameId: "patch-test", contentVersion: target ? "v2" : "v1",
      packageFingerprint: target ? b : a,
      archives: [{file: "Content-00.pdx", group: "base", optional: false}],
    }));
    await writeFile(join(stage, "Package/content-index.json"), JSON.stringify({
      format: "PrismatiXContentIndex", schemaRevision: 1,
      gameId: "patch-test", packageFingerprint: target ? b : a,
      entries: target ? [
        {uri: "Assets/a.txt", fingerprint: c, size: 7, archive: "Content-00.pdx"},
        {uri: "Assets/new.txt", fingerprint: b, size: 5, archive: "Content-00.pdx"},
      ] : [
        {uri: "Assets/a.txt", fingerprint: a, size: 4, archive: "Content-00.pdx"},
        {uri: "Assets/old.txt", fingerprint: c, size: 3, archive: "Content-00.pdx"},
      ],
    }));
    await writeFile(join(stage, "Shared.dll"), "unchanged-runtime");
    await writeFile(join(stage, target ? "new.txt" : "old.txt"), target ? "new" : "old");
  });
  return output;
}

test("distribution patch reconstructs a complete target without copying unchanged files into the bundle", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-dist-patch-"));
  try {
    const base = await distribution(root, "base", false);
    const target = await distribution(root, "target", true);
    const bundle = join(root, "patch");
    const output = join(root, "updated");
    const verified: string[] = [];
    const verify = async (directory: string): Promise<void> => { verified.push(directory); };
    const created = await createDistributionPatch(base, target, bundle, verify);
    assert.equal(created.changedFiles, 4);
    assert.equal(created.removedFiles, 1);
    const bundleFiles = (await verifiedPublishedInputs(bundle)).map((input) => input.uri);
    assert.deepEqual(new Set(bundleFiles), new Set([
      "Patch/manifest.json", "Payload/Content-00.pdx",
      "Payload/Package/content-index.json", "Payload/Package/manifest.json",
      "Payload/new.txt",
    ]));
    const applied = await applyDistributionPatch(base, bundle, output, false, verify);
    assert.equal(applied.changedFiles, 4);
    assert.deepEqual(await verifiedPublishedInputs(output),
      await verifiedPublishedInputs(target));
    assert.equal((await readFile(join(output, "new.txt"), "utf8")), "new");
    await assert.rejects(() => readFile(join(output, "old.txt"), "utf8"),
      {code: "ENOENT"});
    assert.ok(verified.includes(output) || verified.some((path) =>
      path.includes(".prismatix-stage-")));

    const inplaceBase = await distribution(root, "in-place", false);
    await writeFile(join(inplaceBase, "player-save.dat"), "private save");
    await assert.rejects(() => applyDistributionPatch(
      inplaceBase, bundle, inplaceBase, true, verify), /unowned|modified|inventory/u);
    assert.equal(await readFile(join(inplaceBase, "player-save.dat"), "utf8"), "private save");
    await rm(join(inplaceBase, "player-save.dat"));
    await applyDistributionPatch(inplaceBase, bundle, inplaceBase, true, verify);
    assert.deepEqual(await verifiedPublishedInputs(inplaceBase),
      await verifiedPublishedInputs(target));

    const interruptedBase = await distribution(root, "interrupted", false);
    const originalManifest = await readFile(join(interruptedBase, "Package/manifest.json"), "utf8");
    await assert.rejects(() => applyDistributionPatch(interruptedBase, bundle,
      interruptedBase, true, async (directory) => {
        if (directory.includes(".prismatix-stage-"))
          throw new Error("injected verifier rejection");
      }), /injected verifier rejection/u);
    assert.equal(await readFile(join(interruptedBase, "Package/manifest.json"), "utf8"),
      originalManifest);
    assert.deepEqual(await verifiedPublishedInputs(interruptedBase),
      await verifiedPublishedInputs(base));
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("distribution patch refuses modified payload, wrong base, and CLI creates/applies with an explicit verifier", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-dist-patch-fail-"));
  try {
    const base = await distribution(root, "base", false);
    const target = await distribution(root, "target", true);
    const bundle = join(root, "patch");
    const verify = async (): Promise<void> => {};
    await createDistributionPatch(base, target, bundle, verify);
    const wrong = await distribution(root, "wrong", true);
    await assert.rejects(() => applyDistributionPatch(
      wrong, bundle, join(root, "wrong-output"), false, verify),
      /exact input inventory/u);
    const manifestPath = join(bundle, "Patch/manifest.json");
    const intactManifest = await readFile(manifestPath, "utf8");
    const unsafeManifest = JSON.parse(intactManifest) as Record<string, unknown>;
    unsafeManifest.changedFiles = ["../escape"];
    await writeFile(manifestPath, JSON.stringify(unsafeManifest));
    await assert.rejects(() => applyDistributionPatch(
      base, bundle, join(root, "unsafe-output"), false, verify),
      /changed-file list is invalid/u);
    await writeFile(manifestPath, intactManifest);
    await writeFile(join(bundle, "Payload/new.txt"), "tampered");
    await assert.rejects(() => applyDistributionPatch(
      base, bundle, join(root, "tampered-output"), false, verify),
      /unowned|modified/u);

    const mock = join(root, "packager.js");
    await writeFile(mock,
      "if (process.argv[2] !== '--verify-package') process.exit(2); " +
      "process.stdout.write(JSON.stringify({event:'verified',archives:1,resources:2,rawBytes:12})+'\\n');\n");
    const cliBundle = join(root, "cli-patch");
    const created = spawnSync(process.execPath, [cli, "create-patch",
      "--base", base, "--target", target, "--output", cliBundle,
      "--packager", mock, "--json"], {encoding: "utf8"});
    assert.equal(created.status, 0, created.stderr);
    assert.equal((JSON.parse(created.stdout) as {patchCreated: boolean}).patchCreated, true);
    const rebuilt = join(root, "cli-updated");
    const applied = spawnSync(process.execPath, [cli, "apply-patch",
      "--base", base, "--patch", cliBundle, "--output", rebuilt,
      "--packager", mock, "--json"], {encoding: "utf8"});
    assert.equal(applied.status, 0, applied.stderr);
    assert.equal((JSON.parse(applied.stdout) as {patchApplied: boolean}).patchApplied, true);
    assert.deepEqual(await verifiedPublishedInputs(rebuilt),
      await verifiedPublishedInputs(target));
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("removal-only patch needs no PDX payload and omits deleted files", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-dist-patch-remove-"));
  try {
    const base = await distribution(root, "base", false);
    const target = join(root, "target");
    await publishDirectory({output: target, projectRoot: join(root, "author"),
      protectedPaths: [base]}, async (stage) => {
      for (const input of await verifiedPublishedInputs(base)) {
        if (input.uri === "old.txt") continue;
        const destination = join(stage, input.uri);
        await mkdir(dirname(destination), {recursive: true});
        await copyFile(join(base, input.uri), destination);
      }
    });
    const bundle = join(root, "patch");
    const verify = async (): Promise<void> => {};
    const created = await createDistributionPatch(base, target, bundle, verify);
    assert.equal(created.changedFiles, 0);
    assert.equal(created.removedFiles, 1);
    assert.deepEqual((await verifiedPublishedInputs(bundle)).map((item) => item.uri),
      ["Patch/manifest.json"]);
    const output = join(root, "updated");
    await applyDistributionPatch(base, bundle, output, false, verify);
    assert.deepEqual(await verifiedPublishedInputs(output),
      await verifiedPublishedInputs(target));
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
