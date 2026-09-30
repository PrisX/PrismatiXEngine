import assert from "node:assert/strict";
import {mkdir, mkdtemp, readFile, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {distributionCoversLineage, distributionLineage, readReleaseLineage,
  recordReleaseLineage, releaseLineageFile, saveCompatibilityWarning,
  type ReleaseLineage} from "../src/release-lineage.js";

const f1 = "1".repeat(64);
const f2 = "2".repeat(64);
const f3 = "3".repeat(64);
const lineage: ReleaseLineage = {packageFingerprint: f2, contentVersion: "chapter-1",
  saveVersion: 1, compatibleSaveFingerprints: [f1],
  legacyReaders: [{keyId: "legacy-archive-v1", secret: "archive-key"}]};

async function writeManifest(root: string, manifest: unknown): Promise<void> {
  await mkdir(join(root, "Package"), {recursive: true});
  await writeFile(join(root, "Package", "manifest.json"), JSON.stringify(manifest), "utf8");
}

test("release lineage round-trips through the project and rejects another game", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-lineage-"));
  assert.equal(await readReleaseLineage(root, "game"), undefined);
  assert.equal(await recordReleaseLineage(root, "game", lineage), true);
  assert.equal(await recordReleaseLineage(root, "game", lineage), false,
    "an unchanged lineage leaves the tracked file untouched");
  assert.deepEqual(await readReleaseLineage(root, "game"), lineage);
  await assert.rejects(readReleaseLineage(root, "other-game"), /records game "game"/u);
  const path = join(root, releaseLineageFile);
  const stored = JSON.parse(await readFile(path, "utf8")) as Record<string, unknown>;
  await writeFile(path, JSON.stringify({...stored, saveVersion: 0}), "utf8");
  await assert.rejects(readReleaseLineage(root, "game"), /saveVersion/u);
  await writeFile(path, JSON.stringify({...stored, compatibleSaveFingerprints: ["ABC"]}), "utf8");
  await assert.rejects(readReleaseLineage(root, "game"), /compatibleSaveFingerprints/u);
  await writeFile(path, JSON.stringify({...stored,
    legacyReaders: [{keyId: "unknown", secret: "x"}]}), "utf8");
  await assert.rejects(readReleaseLineage(root, "game"), /legacyReaders/u);
});

test("distribution lineage reads current and pre-stable-key manifests", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-lineage-dist-"));
  assert.equal(await distributionLineage(root, "game"), undefined);
  await writeManifest(root, {gameId: "game", packageFingerprint: f2,
    contentVersion: "chapter-1", saveVersion: 1,
    persistence: {dataId: "game", keyId: "game-data-v1",
      legacyReaders: [{keyId: "legacy-archive-v1", secret: "archive-key"}],
      compatibleSaveFingerprints: [f1]}});
  assert.deepEqual(await distributionLineage(root, "game"), lineage);
  assert.equal(await distributionLineage(root, "other-game"), undefined);
  await writeManifest(root, {gameId: "game", packageFingerprint: f1,
    contentVersion: "chapter-1", saveVersion: 1,
    encryption: {enabled: true, archiveKey: "old-key"}});
  assert.deepEqual((await distributionLineage(root, "game"))?.legacyReaders,
    [{keyId: "legacy-archive-v1", secret: "old-key"}]);
  await writeManifest(root, {gameId: "game", packageFingerprint: f1,
    contentVersion: "chapter-1", saveVersion: 1, encryption: {enabled: false}});
  assert.deepEqual((await distributionLineage(root, "game"))?.legacyReaders,
    [{keyId: "legacy-fingerprint-v1", secret: f1}]);
});

test("a kept distribution must already accept every save the lineage knows", () => {
  const packed = {...lineage, packageFingerprint: f3, compatibleSaveFingerprints: [f1, f2]};
  assert.equal(distributionCoversLineage(packed, lineage), true);
  assert.equal(distributionCoversLineage({...packed, compatibleSaveFingerprints: [f1]}, lineage),
    false);
  assert.equal(distributionCoversLineage({...packed, legacyReaders: []}, lineage), false);
  assert.equal(distributionCoversLineage({...packed, contentVersion: "chapter-2",
    compatibleSaveFingerprints: []}, lineage), true,
    "saves from another content version are a migration's concern");
});

test("pack warns when the previous release's saves would be rejected", () => {
  const project = {id: "game", contentVersion: "chapter-1", saveVersion: 1};
  assert.match(saveCompatibilityWarning(project, undefined) ?? "",
    /No previous release lineage.*PXSAVE6111/su);
  assert.equal(saveCompatibilityWarning({...project, saveMigrations: [{
    from: {contentVersion: "chapter-0", saveVersion: 1},
    to: {contentVersion: "chapter-1", saveVersion: 1}}]}, undefined), undefined,
  "a migration into the current version covers the previous release");
  assert.equal(saveCompatibilityWarning(project, lineage), undefined);
  const bumped = {...project, contentVersion: "chapter-2"};
  assert.match(saveCompatibilityWarning(bumped, lineage) ?? "",
    /"chapter-1".*no saveMigrations entry/su);
  assert.equal(saveCompatibilityWarning({...bumped, saveMigrations: [{
    from: {contentVersion: "chapter-1", saveVersion: 1},
    to: {contentVersion: "chapter-2", saveVersion: 1}}]}, lineage), undefined);
});
