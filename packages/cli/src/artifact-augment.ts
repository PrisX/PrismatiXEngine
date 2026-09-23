import {constants} from "node:fs";
import {copyFile, link, lstat, mkdir, readFile, readdir, realpath, rm, writeFile} from "node:fs/promises";
import {dirname, isAbsolute, join, relative, resolve, sep} from "node:path";

import {outputOwnershipMarker, publishDirectory, type PublicationProgress} from "./output-safety.js";

export const augmentationOwnershipPath = ".prismatix-augmentations.json";

export interface ArtifactAugmentationEntry {
  readonly source: string;
  readonly target: string;
  readonly mode: "add" | "replace";
  /** Required for replacement, so a stale manifest cannot overwrite a newer build. */
  readonly expectedFingerprint?: string;
}

export interface ArtifactAugmentationManifest {
  readonly format: "PrismatiXArtifactAugmentation";
  readonly schemaRevision: 1;
  readonly owner: string;
  readonly files: readonly ArtifactAugmentationEntry[];
}

export interface ArtifactAugmentOptions {
  readonly projectRoot: string;
  readonly artifactRoot: string;
  readonly manifest: unknown;
  readonly manifestPath?: string;
  readonly onProgress?: (value: PublicationProgress) => void;
}

export interface ArtifactAugmentResult {
  readonly added: number;
  readonly replaced: number;
  readonly owner: string;
  readonly artifactRoot: string;
}

function safePath(value: unknown): value is string {
  return typeof value === "string" && value.length > 0 && value.length <= 4096 &&
    !value.startsWith("/") && !value.includes("\\") && !value.includes(":") &&
    !value.includes("\0") && value.split("/").every((part) =>
      part.length > 0 && part !== "." && part !== "..");
}

function record(value: unknown): Record<string, unknown> | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : undefined;
}

function fingerprint(value: unknown): value is string {
  return typeof value === "string" && /^[0-9a-f]{64}$/u.test(value);
}

export function validateArtifactAugmentation(value: unknown): ArtifactAugmentationManifest {
  const root = record(value);
  const owner = root?.owner;
  const files = root?.files;
  if (root?.format !== "PrismatiXArtifactAugmentation" || root.schemaRevision !== 1 ||
      typeof owner !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/u.test(owner) ||
      !Array.isArray(files) || files.length === 0 || files.length > 10000 ||
      Object.keys(root).some((key) => !["format", "schemaRevision", "owner", "files"].includes(key)))
    throw new Error("Artifact augmentation manifest has an invalid format, owner or file list");
  const targets = new Set<string>();
  for (const [index, candidate] of files.entries()) {
    const entry = record(candidate);
    if (entry === undefined || !safePath(entry.source) || !safePath(entry.target) ||
        (entry.mode !== "add" && entry.mode !== "replace") ||
        Object.keys(entry).some((key) => !["source", "target", "mode", "expectedFingerprint"].includes(key)) ||
        (entry.mode === "replace" ? !fingerprint(entry.expectedFingerprint)
          : entry.expectedFingerprint !== undefined))
      throw new Error(`Artifact augmentation entry ${index} is invalid`);
    const folded = entry.target.toLowerCase();
    if ([outputOwnershipMarker.toLowerCase(), augmentationOwnershipPath.toLowerCase()].includes(folded))
      throw new Error(`Artifact augmentation targets a reserved file: ${entry.target}`);
    if (targets.has(folded)) throw new Error(`Duplicate artifact augmentation target: ${entry.target}`);
    targets.add(folded);
  }
  return value as ArtifactAugmentationManifest;
}

async function canonicalSourceRoot(root: string): Promise<string> {
  const base = resolve(root);
  const baseInfo = await lstat(base);
  if (!baseInfo.isDirectory() || baseInfo.isSymbolicLink())
    throw new Error(`Augmentation project root is not a regular directory: ${base}`);
  return realpath(base);
}

async function sourceFile(root: string, canonicalRoot: string, path: string): Promise<string> {
  const base = resolve(root);
  let current = base;
  const parts = path.split("/");
  for (const [index, part] of parts.entries()) {
    current = join(current, part);
    const info = await lstat(current);
    if (info.isSymbolicLink() || (index === parts.length - 1 ? !info.isFile() : !info.isDirectory()))
      throw new Error(`Augmentation source is not a regular file inside the project: ${path}`);
  }
  const canonicalFile = await realpath(current);
  const rel = relative(canonicalRoot, canonicalFile);
  if (!rel || rel === ".." || rel.startsWith(`..${sep}`) || isAbsolute(rel))
    throw new Error(`Augmentation source escapes the project: ${path}`);
  return current;
}

async function cloneOwnedContents(source: string, stage: string,
  onProgress?: (value: PublicationProgress) => void): Promise<void> {
  let files = 0;
  let lastReport = Date.now();
  let lastFiles = 0;
  const report = (done = false): void => {
    if (onProgress === undefined) return;
    const now = Date.now();
    if (!done && files - lastFiles < 512 && now - lastReport < 2000) return;
    onProgress({phase: "clone-existing", files, bytes: 0, done});
    lastReport = now;
    lastFiles = files;
  };
  onProgress?.({phase: "clone-existing", files: 0, bytes: 0, done: false});
  const visit = async (from: string, to: string): Promise<void> => {
    for (const entry of await readdir(from, {withFileTypes: true})) {
      if (from === source && entry.name === outputOwnershipMarker) continue;
      const sourcePath = join(from, entry.name);
      const destination = join(to, entry.name);
      if (entry.isSymbolicLink()) throw new Error(`Artifact contains a symbolic link: ${sourcePath}`);
      if (entry.isDirectory()) { await mkdir(destination); await visit(sourcePath, destination); }
      else if (entry.isFile()) {
        try { await link(sourcePath, destination); }
        catch (error) {
          if (!["EXDEV", "EPERM", "EOPNOTSUPP", "ENOSYS"].includes(
            (error as NodeJS.ErrnoException).code ?? "")) throw error;
          await copyFile(sourcePath, destination, constants.COPYFILE_EXCL);
        }
        files += 1;
        report();
      } else throw new Error(`Artifact contains a special file: ${sourcePath}`);
    }
  };
  await visit(source, stage);
  report(true);
}

function readOwners(value: unknown, files: Record<string, unknown>): Map<string, string> {
  if (value === undefined) return new Map();
  const root = record(value);
  const entries = record(root?.owners);
  if (root?.format !== "PrismatiXArtifactAugmentations" || root.schemaRevision !== 1 ||
      entries === undefined || Object.keys(root).some((key) =>
        !["format", "schemaRevision", "owners"].includes(key)))
    throw new Error("Artifact augmentation ownership metadata is invalid");
  const owners = new Map<string, string>();
  for (const [path, owner] of Object.entries(entries)) {
    if (!safePath(path) || !fingerprint(files[path]) ||
        typeof owner !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/u.test(owner))
      throw new Error(`Artifact augmentation ownership entry is invalid: ${path}`);
    owners.set(path, owner);
  }
  return owners;
}

/** Atomically overlay explicit resources onto an intact artifact. Unchanged
 * files are hard-linked into staging where possible; replacements are unlinked
 * before copying, so the published source artifact is never mutated in place. */
export async function augmentArtifact(options: ArtifactAugmentOptions): Promise<ArtifactAugmentResult> {
  const manifest = validateArtifactAugmentation(options.manifest);
  const projectRoot = resolve(options.projectRoot);
  const artifactRoot = resolve(options.artifactRoot);
  const canonicalRoot = await canonicalSourceRoot(projectRoot);
  const sources = await Promise.all(manifest.files.map((entry) =>
    sourceFile(projectRoot, canonicalRoot, entry.source)));
  const protectedPaths = [...sources, ...(options.manifestPath === undefined ? [] : [resolve(options.manifestPath)])];
  let added = 0;
  let replaced = 0;
  await publishDirectory({output: artifactRoot, projectRoot, protectedPaths,
    ...(options.onProgress === undefined ? {} : {onProgress: options.onProgress})}, async (stage) => {
    const marker = record(JSON.parse(await readFile(join(artifactRoot, outputOwnershipMarker), "utf8")) as unknown);
    const files = record(marker?.files);
    if (marker?.format !== "PrismatiXOutput/1" || files === undefined)
      throw new Error("Artifact ownership marker is invalid");
    const previousOwnersPath = join(artifactRoot, augmentationOwnershipPath);
    let previousOwners: unknown;
    try { previousOwners = JSON.parse(await readFile(previousOwnersPath, "utf8")) as unknown; }
    catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    }
    const owners = readOwners(previousOwners, files);
    const existingPaths = new Set(Object.keys(files).map((path) => path.toLowerCase()));
    for (const entry of manifest.files) {
      const current = files[entry.target];
      const occupied = existingPaths.has(entry.target.toLowerCase());
      const previousOwner = owners.get(entry.target);
      if (previousOwner !== undefined && previousOwner !== manifest.owner)
        throw new Error(`Artifact target belongs to another augmentation owner: ${entry.target}`);
      if (entry.mode === "add" && occupied)
        throw new Error(`Artifact augmentation target already exists: ${entry.target}`);
      if (entry.mode === "replace" &&
          (!fingerprint(current) || current !== entry.expectedFingerprint))
        throw new Error(`Artifact replacement fingerprint is stale: ${entry.target}`);
    }
    await cloneOwnedContents(artifactRoot, stage, options.onProgress);
    for (const [index, entry] of manifest.files.entries()) {
      if (await sourceFile(projectRoot, canonicalRoot, entry.source) !== sources[index])
        throw new Error(`Augmentation source changed before publication: ${entry.source}`);
      const destination = resolve(stage, ...entry.target.split("/"));
      await mkdir(dirname(destination), {recursive: true});
      if (entry.mode === "replace") { await rm(destination); replaced += 1; }
      else added += 1;
      await copyFile(sources[index]!, destination, constants.COPYFILE_EXCL);
      owners.set(entry.target, manifest.owner);
    }
    const ownershipPath = join(stage, augmentationOwnershipPath);
    try { await rm(ownershipPath); }
    catch (error) { if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error; }
    await writeFile(ownershipPath, `${JSON.stringify({format: "PrismatiXArtifactAugmentations",
      schemaRevision: 1, owners: Object.fromEntries([...owners].sort(([a], [b]) => a.localeCompare(b, "en")))})}\n`,
    {flag: "wx"});
  });
  return {added, replaced, owner: manifest.owner, artifactRoot};
}
