import {createHash} from "node:crypto";
import {copyFile, link, lstat, mkdir, readFile, stat, writeFile} from "node:fs/promises";
import {dirname, join, resolve} from "node:path";

import {planPackagePatch} from "./package-diff.js";
import {outputOwnershipMarker, publishDirectory, verifiedPublishedInputs,
  type PublicationProgress, type PublishedInput} from "./output-safety.js";

const patchManifestPath = "Patch/manifest.json";
const patchPayloadRoot = "Payload";
const maxPatchManifestBytes = 16 * 1024 * 1024;
const fingerprintPattern = /^[0-9a-f]{64}$/u;

type VerifyContents = (distribution: string) => Promise<unknown>;
type Inventory = Readonly<Record<string, string>>;

interface DistributionSnapshot {
  readonly files: Inventory;
  readonly inputs: ReadonlyMap<string, PublishedInput>;
  readonly fingerprint: string;
}

interface PatchManifest {
  readonly format: "PrismatiXDistributionPatch";
  readonly schemaRevision: 1;
  readonly gameId: string;
  readonly basePackageFingerprint: string;
  readonly targetPackageFingerprint: string;
  readonly baseInventoryFingerprint: string;
  readonly targetInventoryFingerprint: string;
  readonly targetFiles: Inventory;
  readonly changedFiles: readonly string[];
}

export interface DistributionPatchReport {
  readonly gameId: string;
  readonly basePackageFingerprint: string;
  readonly targetPackageFingerprint: string;
  readonly changedFiles: number;
  readonly removedFiles: number;
  readonly payloadBytes: string;
  readonly output: string;
}

export interface DistributionPatchProgress {
  readonly phase: string;
  readonly files: number;
  readonly bytes: number;
  readonly done: boolean;
  readonly linkedFiles?: number;
  readonly linkedBytes?: number;
}

type PatchProgress = (value: DistributionPatchProgress) => void;

function forwardProgress(label: string, report: PatchProgress):
  (value: PublicationProgress) => void {
  return (value) => report({
    phase: `${label}-${value.phase}`, files: value.files,
    bytes: value.bytes, done: value.done,
  });
}

function safePath(value: string): boolean {
  if (value.length === 0 || value.length > 4096 || value.startsWith("/") ||
      /[\\:\x00-\x1f]/u.test(value)) return false;
  return value.split("/").every((part) => {
    if (part.length === 0 || part === "." || part === "..") return false;
    if (process.platform !== "win32") return true;
    return !/[<>"|?*]/u.test(part) && !/[. ]$/u.test(part) &&
      !/^(?:con|prn|aux|nul|conin\$|conout\$|com[1-9]|lpt[1-9])(?:\..*)?$/iu.test(part);
  });
}

function fileAt(root: string, uri: string): string {
  if (!safePath(uri)) throw new Error(`Unsafe distribution path: ${uri}`);
  return join(root, ...uri.split("/"));
}

function inventoryFingerprint(files: Inventory): string {
  const ordered = Object.entries(files).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0);
  return createHash("sha256").update(JSON.stringify(ordered)).digest("hex");
}

function sameInventory(left: Inventory, right: Inventory): boolean {
  const names = Object.keys(left);
  return names.length === Object.keys(right).length &&
    names.every((name) => Object.hasOwn(right, name) && left[name] === right[name]);
}

async function markerInventory(directory: string): Promise<Inventory> {
  const path = join(directory, outputOwnershipMarker);
  const details = await lstat(path);
  if (!details.isFile() || details.isSymbolicLink() || details.size <= 0 ||
      details.size > maxPatchManifestBytes)
    throw new Error(`Distribution ownership marker is invalid: ${path}`);
  const raw = JSON.parse(await readFile(path, "utf8")) as unknown;
  if (raw === null || typeof raw !== "object" || Array.isArray(raw) ||
      (raw as {format?: unknown}).format !== "PrismatiXOutput/1")
    throw new Error(`Distribution ownership marker has an unsupported format: ${path}`);
  const files = (raw as {files?: unknown}).files;
  if (files === null || typeof files !== "object" || Array.isArray(files))
    throw new Error(`Distribution ownership marker lacks an inventory: ${path}`);
  const inventory = files as Record<string, unknown>;
  const folded = new Set<string>();
  for (const [uri, fingerprint] of Object.entries(inventory)) {
    const key = process.platform === "win32" ? uri.toLowerCase() : uri;
    if (!safePath(uri) || uri === outputOwnershipMarker || folded.has(key) ||
        (fingerprint !== "directory" &&
          (typeof fingerprint !== "string" || !fingerprintPattern.test(fingerprint))))
      throw new Error(`Distribution ownership marker contains an invalid path: ${uri}`);
    folded.add(key);
  }
  return inventory as Inventory;
}

async function snapshot(directory: string, label?: string,
  report?: PatchProgress): Promise<DistributionSnapshot> {
  const inputs = await verifiedPublishedInputs(directory,
    label === undefined || report === undefined ? undefined :
      forwardProgress(label, report));
  const files = await markerInventory(directory);
  const mapped = new Map(inputs.map((input) => [input.uri, input]));
  for (const [uri, fingerprint] of Object.entries(files)) {
    if (fingerprint !== "directory" && mapped.get(uri)?.fingerprint !== fingerprint)
      throw new Error(`Distribution changed during inventory scan: ${uri}`);
  }
  return {files, inputs: mapped, fingerprint: inventoryFingerprint(files)};
}

async function boundedPatchManifest(directory: string): Promise<PatchManifest> {
  const path = fileAt(directory, patchManifestPath);
  const details = await lstat(path);
  if (!details.isFile() || details.isSymbolicLink() || details.size <= 0 ||
      details.size > maxPatchManifestBytes)
    throw new Error(`Patch manifest is missing or oversized: ${path}`);
  const value = JSON.parse(await readFile(path, "utf8")) as unknown;
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error("Patch manifest is not an object");
  const patch = value as Record<string, unknown>;
  if (patch.format !== "PrismatiXDistributionPatch" || patch.schemaRevision !== 1 ||
      typeof patch.gameId !== "string" || patch.gameId.length === 0 ||
      ![patch.basePackageFingerprint, patch.targetPackageFingerprint,
        patch.baseInventoryFingerprint, patch.targetInventoryFingerprint]
        .every((item) => typeof item === "string" && fingerprintPattern.test(item)) ||
      patch.targetFiles === null || typeof patch.targetFiles !== "object" ||
      Array.isArray(patch.targetFiles) || !Array.isArray(patch.changedFiles))
    throw new Error("Patch manifest identity or schema is invalid");
  const targetFiles = patch.targetFiles as Record<string, unknown>;
  const folded = new Set<string>();
  for (const [uri, fingerprint] of Object.entries(targetFiles)) {
    const key = process.platform === "win32" ? uri.toLowerCase() : uri;
    if (!safePath(uri) || uri === outputOwnershipMarker || folded.has(key) ||
        (fingerprint !== "directory" &&
          (typeof fingerprint !== "string" || !fingerprintPattern.test(fingerprint))))
      throw new Error(`Patch target inventory contains an invalid path: ${uri}`);
    folded.add(key);
  }
  if (inventoryFingerprint(targetFiles as Inventory) !== patch.targetInventoryFingerprint)
    throw new Error("Patch target inventory fingerprint is invalid");
  let previous = "";
  for (const changed of patch.changedFiles) {
    if (typeof changed !== "string" || !safePath(changed) ||
        changed <= previous || !fingerprintPattern.test(String(targetFiles[changed] ?? "")))
      throw new Error("Patch changed-file list is invalid");
    previous = changed;
  }
  return patch as unknown as PatchManifest;
}

async function assertPatchPayload(directory: string, patch: PatchManifest,
  report?: PatchProgress): Promise<void> {
  const bundle = await snapshot(directory, "patch", report);
  const expected = new Set([patchManifestPath,
    ...patch.changedFiles.map((uri) => `${patchPayloadRoot}/${uri}`)]);
  if (bundle.inputs.size !== expected.size ||
      [...bundle.inputs.keys()].some((uri) => !expected.has(uri)))
    throw new Error("Patch bundle contains missing or unexpected files");
  for (const uri of patch.changedFiles) {
    if (bundle.inputs.get(`${patchPayloadRoot}/${uri}`)?.fingerprint !==
        patch.targetFiles[uri])
      throw new Error(`Patch payload differs from target inventory: ${uri}`);
  }
}

function patchReport(patch: PatchManifest, base: DistributionSnapshot,
  output: string, payloadBytes: bigint): DistributionPatchReport {
  return {
    gameId: patch.gameId,
    basePackageFingerprint: patch.basePackageFingerprint,
    targetPackageFingerprint: patch.targetPackageFingerprint,
    changedFiles: patch.changedFiles.length,
    removedFiles: Object.keys(base.files).filter((uri) =>
      base.files[uri] !== "directory" && !Object.hasOwn(patch.targetFiles, uri)).length,
    payloadBytes: payloadBytes.toString(), output,
  };
}

/** Produces a whole-file delta bundle. It is not a signed or drop-in PDX overlay. */
export async function createDistributionPatch(baseDirectory: string,
  targetDirectory: string, outputDirectory: string,
  verifyContents: VerifyContents,
  report?: PatchProgress): Promise<DistributionPatchReport> {
  const base = resolve(baseDirectory);
  const target = resolve(targetDirectory);
  const output = resolve(outputDirectory);
  const [plan, oldSnapshot, newSnapshot] = await Promise.all([
    planPackagePatch(base, target), snapshot(base, "base", report),
    snapshot(target, "target", report),
  ]);
  await verifyContents(base);
  if (base !== target) await verifyContents(target);
  const changedFiles = Object.entries(newSnapshot.files)
    .filter(([uri, fingerprint]) => fingerprint !== "directory" &&
      oldSnapshot.files[uri] !== fingerprint)
    .map(([uri]) => uri).sort();
  if (changedFiles.length === 0 && sameInventory(oldSnapshot.files, newSnapshot.files))
    throw new Error("Base and target distributions are identical");
  const patch: PatchManifest = {
    format: "PrismatiXDistributionPatch", schemaRevision: 1,
    gameId: plan.gameId,
    basePackageFingerprint: plan.basePackageFingerprint,
    targetPackageFingerprint: plan.targetPackageFingerprint,
    baseInventoryFingerprint: oldSnapshot.fingerprint,
    targetInventoryFingerprint: newSnapshot.fingerprint,
    targetFiles: newSnapshot.files,
    changedFiles,
  };
  const encoded = JSON.stringify(patch);
  if (Buffer.byteLength(encoded, "utf8") > maxPatchManifestBytes)
    throw new Error("Patch manifest exceeds its size limit");
  let payloadBytes = 0n;
  let stagePath = "";
  await publishDirectory({output, projectRoot: base,
    protectedPaths: [base, target],
    ...(report === undefined ? {} : {onProgress: forwardProgress("patch-output", report)}),
    beforePromotion: async () => { await assertPatchPayload(stagePath, patch, report); }}, async (stage) => {
    stagePath = stage;
    await mkdir(join(stage, "Patch"), {recursive: true});
    await writeFile(fileAt(stage, patchManifestPath), encoded, {flag: "wx"});
    report?.({phase: "patch-payload-copy", files: 0, bytes: 0, done: false});
    let copiedFiles = 0;
    let lastReport = Date.now();
    for (const uri of changedFiles) {
      const source = fileAt(target, uri);
      const destination = fileAt(stage, `${patchPayloadRoot}/${uri}`);
      await mkdir(dirname(destination), {recursive: true});
      await copyFile(source, destination);
      const copied = await stat(destination);
      payloadBytes += BigInt(copied.size);
      ++copiedFiles;
      if (copiedFiles === changedFiles.length || copiedFiles % 512 === 0 ||
          Date.now() - lastReport >= 2000) {
        report?.({phase: "patch-payload-copy", files: copiedFiles,
          bytes: Number(payloadBytes), done: copiedFiles === changedFiles.length});
        lastReport = Date.now();
      }
    }
    if (changedFiles.length === 0)
      report?.({phase: "patch-payload-copy", files: 0, bytes: 0, done: true});
  });
  return patchReport(patch, oldSnapshot, output, payloadBytes);
}

/** Reconstructs a complete target distribution via safe staged publication. */
export async function applyDistributionPatch(baseDirectory: string,
  patchDirectory: string, outputDirectory: string, inPlace: boolean,
  verifyContents: VerifyContents,
  report?: PatchProgress): Promise<DistributionPatchReport> {
  const base = resolve(baseDirectory);
  const bundle = resolve(patchDirectory);
  const output = resolve(outputDirectory);
  const sameOutput = process.platform === "win32"
    ? base.toLowerCase() === output.toLowerCase() : base === output;
  if (sameOutput !== inPlace)
    throw new Error("Use --in-place only when --output is exactly the base distribution");
  const patch = await boundedPatchManifest(bundle);
  await assertPatchPayload(bundle, patch, report);
  const oldSnapshot = await snapshot(base, "base", report);
  if (oldSnapshot.fingerprint !== patch.baseInventoryFingerprint)
    throw new Error("Base distribution does not match the patch's exact input inventory");
  await verifyContents(base);
  const basePlan = await planPackagePatch(base, base);
  if (basePlan.gameId !== patch.gameId ||
      basePlan.basePackageFingerprint !== patch.basePackageFingerprint)
    throw new Error("Base distribution has the wrong game or package fingerprint");
  const changed = new Set(patch.changedFiles);
  const targetFilePaths = Object.entries(patch.targetFiles)
    .filter(([, fingerprint]) => fingerprint !== "directory")
    .map(([uri]) => uri).sort();
  for (const uri of targetFilePaths) {
    if (!changed.has(uri) && oldSnapshot.files[uri] !== patch.targetFiles[uri])
      throw new Error(`Patch omits a changed target file: ${uri}`);
  }
  let payloadBytes = 0n;
  for (const uri of patch.changedFiles)
    payloadBytes += BigInt((await stat(fileAt(bundle, `${patchPayloadRoot}/${uri}`))).size);
  let stagePath = "";
  await publishDirectory({output, projectRoot: bundle,
    protectedPaths: sameOutput ? [bundle] : [bundle, base],
    verifyExistingBeforeBuild: !sameOutput,
    ...(report === undefined ? {} : {onProgress: forwardProgress("output", report)}),
    beforePromotion: async () => {
      const stagedInventory = await markerInventory(stagePath);
      if (!sameInventory(stagedInventory, patch.targetFiles))
        throw new Error("Assembled distribution differs from patch target inventory");
      await assertPatchPayload(bundle, patch, report);
      if (!sameOutput) {
        const recheckedBase = await snapshot(base, "base-recheck", report);
        if (recheckedBase.fingerprint !== oldSnapshot.fingerprint)
          throw new Error("Base distribution changed while applying the patch");
      }
    }}, async (stage) => {
    stagePath = stage;
    for (const [uri, fingerprint] of Object.entries(patch.targetFiles))
      if (fingerprint === "directory") await mkdir(fileAt(stage, uri), {recursive: true});
    report?.({phase: "distribution-assemble", files: 0, bytes: 0,
      linkedFiles: 0, linkedBytes: 0, done: false});
    let copiedFiles = 0;
    let copiedBytes = 0;
    let linkedFiles = 0;
    let linkedBytes = 0;
    let lastReport = Date.now();
    for (const uri of targetFilePaths) {
      const source = changed.has(uri)
        ? fileAt(bundle, `${patchPayloadRoot}/${uri}`) : fileAt(base, uri);
      const destination = fileAt(stage, uri);
      await mkdir(dirname(destination), {recursive: true});
      let linked = false;
      if (sameOutput && !changed.has(uri)) {
        try { await link(source, destination); linked = true; }
        catch { await copyFile(source, destination); }
      } else await copyFile(source, destination);
      ++copiedFiles;
      const size = (await stat(destination)).size;
      copiedBytes += size;
      if (linked) { ++linkedFiles; linkedBytes += size; }
      if (copiedFiles === targetFilePaths.length || copiedFiles % 512 === 0 ||
          Date.now() - lastReport >= 2000) {
        report?.({phase: "distribution-assemble", files: copiedFiles,
          bytes: copiedBytes, linkedFiles, linkedBytes,
          done: copiedFiles === targetFilePaths.length});
        lastReport = Date.now();
      }
    }
    const assembled = await planPackagePatch(base, stage);
    if (assembled.targetPackageFingerprint !== patch.targetPackageFingerprint)
      throw new Error("Assembled distribution has the wrong package identity");
    await verifyContents(stage);
  });
  return patchReport(patch, oldSnapshot, output, payloadBytes);
}
