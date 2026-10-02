import {lstat, readFile} from "node:fs/promises";
import {join, resolve} from "node:path";

type RecordValue = Record<string, unknown>;

export interface IndexedResource {
  readonly uri: string;
  readonly fingerprint: string;
  readonly size: number;
  readonly archive: string;
}

export interface PackagePatchPlan {
  readonly gameId: string;
  readonly baseContentVersion: string;
  readonly targetContentVersion: string;
  readonly basePackageFingerprint: string;
  readonly targetPackageFingerprint: string;
  readonly added: readonly IndexedResource[];
  readonly modified: readonly {readonly before: IndexedResource; readonly after: IndexedResource}[];
  readonly removed: readonly IndexedResource[];
  readonly unchangedCount: number;
  readonly affectedTargetArchives: readonly string[];
  readonly changedRawBytes: string;
}

interface IndexedPackage {
  readonly gameId: string;
  readonly contentVersion: string;
  readonly packageFingerprint: string;
  readonly entries: ReadonlyMap<string, IndexedResource>;
}

const fingerprintPattern = /^[0-9a-f]{64}$/u;
const maxEntrySize = 8 * 1024 ** 3;

function record(value: unknown): RecordValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as RecordValue : undefined;
}

function safeRelativePath(value: unknown): value is string {
  return typeof value === "string" && value.length > 0 &&
    !value.includes("\\") && !value.includes(":") && !value.includes("\0") &&
    !value.startsWith("/") &&
    value.split("/").every((part) => part.length > 0 && part !== "." && part !== "..");
}

async function boundedJson(path: string, maxBytes: number): Promise<unknown> {
  const details = await lstat(path);
  if (!details.isFile() || details.isSymbolicLink() ||
      details.size <= 0 || details.size > maxBytes)
    throw new Error(`Package file is not a bounded regular file: ${path}`);
  return JSON.parse(await readFile(path, "utf8")) as unknown;
}

async function loadIndexedPackage(directory: string): Promise<IndexedPackage> {
  const root = resolve(directory);
  const manifest = record(await boundedJson(join(root, "Package/manifest.json"), 4 * 1024 ** 2));
  const index = record(await boundedJson(join(root, "Package/content-index.json"), 128 * 1024 ** 2));
  if (manifest === undefined || index === undefined ||
      manifest.format !== "PrismatiXPackageManifest" ||
      manifest.schemaRevision !== 1 ||
      typeof manifest.gameId !== "string" || manifest.gameId.length === 0 ||
      typeof manifest.contentVersion !== "string" || manifest.contentVersion.length === 0 ||
      typeof manifest.packageFingerprint !== "string" ||
      !fingerprintPattern.test(manifest.packageFingerprint) ||
      !Array.isArray(manifest.archives) || manifest.archives.length < 1 ||
      manifest.archives.length > 64 ||
      index.format !== "PrismatiXContentIndex" || index.schemaRevision !== 1 ||
      index.gameId !== manifest.gameId ||
      index.packageFingerprint !== manifest.packageFingerprint ||
      !Array.isArray(index.entries) || index.entries.length < 1 ||
      index.entries.length > 1_000_000)
    throw new Error(`Package manifest and content index disagree or are invalid: ${root}`);

  const archives = new Set<string>();
  for (const value of manifest.archives) {
    const mount = record(value);
    if (mount === undefined || !safeRelativePath(mount.file) ||
        !mount.file.endsWith(".pdx") || archives.has(mount.file))
      throw new Error(`Package contains an invalid archive mount: ${root}`);
    const path = join(root, mount.file);
    const details = await lstat(path);
    if (!details.isFile() || details.isSymbolicLink())
      throw new Error(`Package archive is missing or not a regular file: ${path}`);
    archives.add(mount.file);
  }

  const entries = new Map<string, IndexedResource>();
  let previousUri = "";
  for (const value of index.entries) {
    const item = record(value);
    if (item === undefined || !safeRelativePath(item.uri) ||
        item.uri <= previousUri ||
        typeof item.fingerprint !== "string" ||
        !fingerprintPattern.test(item.fingerprint) ||
        typeof item.size !== "number" || !Number.isSafeInteger(item.size) ||
        item.size < 0 || item.size > maxEntrySize ||
        typeof item.archive !== "string" || !archives.has(item.archive))
      throw new Error(`Package content index contains an invalid entry: ${root}`);
    entries.set(item.uri, {
      uri: item.uri, fingerprint: item.fingerprint,
      size: item.size, archive: item.archive,
    });
    previousUri = item.uri;
  }
  return {
    gameId: manifest.gameId, contentVersion: manifest.contentVersion,
    packageFingerprint: manifest.packageFingerprint, entries,
  };
}

/** Read-only content comparison. This is not a signed patch or an installer. */
export async function planPackagePatch(baseDirectory: string,
  targetDirectory: string): Promise<PackagePatchPlan> {
  const [base, target] = await Promise.all([
    loadIndexedPackage(baseDirectory), loadIndexedPackage(targetDirectory),
  ]);
  if (base.gameId !== target.gameId)
    throw new Error(`Packages belong to different games: ${base.gameId} != ${target.gameId}`);
  const added: IndexedResource[] = [];
  const modified: {before: IndexedResource; after: IndexedResource}[] = [];
  const removed: IndexedResource[] = [];
  const affected = new Set<string>();
  let unchangedCount = 0;
  let changedRawBytes = 0n;
  for (const [uri, after] of target.entries) {
    const before = base.entries.get(uri);
    if (before === undefined) {
      added.push(after);
      affected.add(after.archive);
      changedRawBytes += BigInt(after.size);
    } else if (before.fingerprint !== after.fingerprint || before.size !== after.size) {
      modified.push({before, after});
      affected.add(after.archive);
      changedRawBytes += BigInt(after.size);
    } else {
      unchangedCount += 1;
    }
  }
  for (const [uri, before] of base.entries)
    if (!target.entries.has(uri)) removed.push(before);
  return {
    gameId: base.gameId,
    baseContentVersion: base.contentVersion,
    targetContentVersion: target.contentVersion,
    basePackageFingerprint: base.packageFingerprint,
    targetPackageFingerprint: target.packageFingerprint,
    added, modified, removed, unchangedCount,
    affectedTargetArchives: [...affected].sort(),
    changedRawBytes: changedRawBytes.toString(),
  };
}
