import assert from "node:assert/strict";
import {mkdtemp, rm, stat, statfs} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {assessPackageDiskSpace, availableBytesForOutput, formatDiskBytes} from "../src/disk-space.js";

const mib = 1024n * 1024n;

test("uncompressed packages fail early only when indexed inputs exceed free space", () => {
  assert.equal(assessPackageDiskSpace(100n * mib, 99n * mib, "none"), "insufficient");
  assert.equal(assessPackageDiskSpace(100n * mib, 100n * mib, "none"), "warning");
  assert.equal(assessPackageDiskSpace(100n * mib, 117n * mib, "none"), "sufficient");
});

test("compressed packages receive an advisory warning, not a false hard failure", () => {
  for (const mode of ["fast", "balanced", "maximum"]) {
    assert.equal(assessPackageDiskSpace(100n * mib, 10n * mib, mode), "warning");
    assert.equal(assessPackageDiskSpace(100n * mib, 117n * mib, mode), "sufficient");
  }
  assert.equal(formatDiskBytes(1024n * mib), "1.00 GiB");
  assert.equal(formatDiskBytes(16n * mib), "16.00 MiB");
});

test("output space is measured on its staging parent even when output does not exist", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-disk-space-"));
  try {
    const volume = await statfs(root, {bigint: true});
    const actual = await availableBytesForOutput(join(root, "missing", "nested", "game"));
    assert.equal(typeof actual, "bigint");
    assert.ok(actual! > 0n);
    // The two reads may straddle another process's writes, but must agree
    // on the volume and remain close without creating output directories.
    assert.ok(actual! <= volume.blocks * volume.bsize);
    await assert.rejects(stat(join(root, "missing")), {code: "ENOENT"});
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
