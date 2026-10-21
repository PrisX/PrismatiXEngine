import {mkdir, readFile, rename, rm, stat, writeFile} from "node:fs/promises";
import {join} from "node:path";
import {distributionDataRoot} from "./distribution-layout.js";

/** Save identity of the last distribution packed from a project. The Player
 * accepts a same-version save only when its package fingerprint is the current
 * one or listed as compatible, so this record lives in the project, under
 * version control, instead of depending on an old output folder surviving. */
export interface ReleaseLineage {
  readonly packageFingerprint: string;
  readonly contentVersion: string;
  readonly saveVersion: number;
  readonly compatibleSaveFingerprints: readonly string[];
  readonly legacyReaders: readonly {readonly keyId: string; readonly secret: string}[];
}

interface VersionedProject {
  readonly id: string;
  readonly contentVersion: string;
  readonly saveVersion: number;
  readonly saveMigrations?: readonly {
    readonly from: {readonly contentVersion: string; readonly saveVersion: number};
    readonly to: {readonly contentVersion: string; readonly saveVersion: number};
  }[];
}

export const releaseLineageFile = ".prismatix-release/lineage.json";

const fingerprintPattern = /^[0-9a-f]{64}$/u;
const readerKeys = new Set(["legacy-archive-v1", "legacy-fingerprint-v1"]);

function record(value: unknown): Record<string, unknown> | undefined {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown> : undefined;
}

async function boundedText(path: string, limit: number): Promise<string | undefined> {
  try {
    const info = await stat(path);
    if (!info.isFile() || info.size > limit)
      throw new Error(`${path} is not a regular file of at most ${limit} bytes`);
    return await readFile(path, "utf8");
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined;
    throw error;
  }
}

/** Reads the project's lineage; undefined when the project has none yet. */
export async function readReleaseLineage(projectRoot: string, gameId: string):
    Promise<ReleaseLineage | undefined> {
  const path = join(projectRoot, releaseLineageFile);
  const text = await boundedText(path, 1024 * 1024);
  if (text === undefined) return undefined;
  const invalid = (reason: string) => new Error(`${path} is invalid: ${reason}`);
  let document: Record<string, unknown> | undefined;
  try { document = record(JSON.parse(text) as unknown); } catch { throw invalid("not JSON"); }
  if (document?.format !== "PrismatiXReleaseLineage" || document.schemaRevision !== 1)
    throw invalid("expected format PrismatiXReleaseLineage, schemaRevision 1");
  if (document.gameId !== gameId)
    throw invalid(`it records game ${JSON.stringify(document.gameId)}, not ${JSON.stringify(gameId)}`);
  const {packageFingerprint, contentVersion, saveVersion,
    compatibleSaveFingerprints, legacyReaders} = document;
  if (typeof packageFingerprint !== "string" || !fingerprintPattern.test(packageFingerprint))
    throw invalid("packageFingerprint must be 64 lowercase hex digits");
  if (typeof contentVersion !== "string" || contentVersion.length === 0)
    throw invalid("contentVersion must be a non-empty string");
  if (!Number.isSafeInteger(saveVersion) || Number(saveVersion) < 1)
    throw invalid("saveVersion must be a positive integer");
  if (!Array.isArray(compatibleSaveFingerprints) || compatibleSaveFingerprints.length > 64 ||
      !compatibleSaveFingerprints.every(value =>
        typeof value === "string" && fingerprintPattern.test(value)))
    throw invalid("compatibleSaveFingerprints must list at most 64 fingerprints");
  const readers = Array.isArray(legacyReaders) ? legacyReaders.map(record) : [];
  if (!Array.isArray(legacyReaders) || readers.length > 64 ||
      !readers.every(reader => reader !== undefined && readerKeys.has(String(reader.keyId)) &&
        typeof reader.secret === "string" && reader.secret.length > 0 &&
        reader.secret.length <= 1024))
    throw invalid("legacyReaders must list at most 64 known persistence readers");
  return {packageFingerprint, contentVersion, saveVersion: Number(saveVersion),
    compatibleSaveFingerprints: compatibleSaveFingerprints as string[],
    legacyReaders: readers.map(reader => ({keyId: String(reader!.keyId),
      secret: String(reader!.secret)}))};
}

/** The save identity a packaged distribution carries, in lineage form.
 * Undefined when the directory holds no readable package of this game. */
export async function distributionLineage(distribution: string, gameId: string):
    Promise<ReleaseLineage | undefined> {
  let manifest: Record<string, unknown> | undefined;
  try {
    const text = await boundedText(join(await distributionDataRoot(distribution), "Package", "manifest.json"),
      4 * 1024 * 1024);
    manifest = text === undefined ? undefined : record(JSON.parse(text) as unknown);
  } catch {
    return undefined;
  }
  const {packageFingerprint, contentVersion, saveVersion} = manifest ?? {};
  if (manifest?.gameId !== gameId || typeof packageFingerprint !== "string" ||
      !fingerprintPattern.test(packageFingerprint.toLowerCase()) ||
      typeof contentVersion !== "string" || !Number.isSafeInteger(saveVersion))
    return undefined;
  const persistence = record(manifest.persistence);
  let legacyReaders: {keyId: string; secret: string}[];
  let compatibleSaveFingerprints: string[] = [];
  if (typeof persistence?.keyId === "string" && persistence.keyId.length > 0) {
    legacyReaders = (Array.isArray(persistence.legacyReaders) ? persistence.legacyReaders : [])
      .map(record).filter(reader => reader !== undefined &&
        typeof reader.keyId === "string" && typeof reader.secret === "string")
      .map(reader => ({keyId: String(reader!.keyId), secret: String(reader!.secret)}));
    compatibleSaveFingerprints = (Array.isArray(persistence.compatibleSaveFingerprints)
      ? persistence.compatibleSaveFingerprints : [])
      .filter((value): value is string => typeof value === "string")
      .map(value => value.toLowerCase());
  } else {
    // Packages from before stable persistence keys: the Packager derives the
    // reader from the archive key, or from the fingerprint when unencrypted.
    const encryption = record(manifest.encryption);
    legacyReaders = encryption?.enabled === true && typeof encryption.archiveKey === "string" &&
        encryption.archiveKey.length > 0
      ? [{keyId: "legacy-archive-v1", secret: encryption.archiveKey}]
      : [{keyId: "legacy-fingerprint-v1", secret: packageFingerprint}];
  }
  return {packageFingerprint: packageFingerprint.toLowerCase(), contentVersion,
    saveVersion: Number(saveVersion), compatibleSaveFingerprints, legacyReaders};
}

/** Whether a distribution already accepts every save the lineage knows, so it
 * may be kept or refreshed in place instead of repackaged. */
export function distributionCoversLineage(distribution: ReleaseLineage,
    lineage: ReleaseLineage): boolean {
  const accepted = new Set([distribution.packageFingerprint,
    ...distribution.compatibleSaveFingerprints]);
  const sameVersion = lineage.contentVersion === distribution.contentVersion &&
    lineage.saveVersion === distribution.saveVersion;
  return (!sameVersion || [lineage.packageFingerprint, ...lineage.compatibleSaveFingerprints]
    .every(fingerprint => accepted.has(fingerprint))) &&
    lineage.legacyReaders.every(reader => distribution.legacyReaders.some(known =>
      known.keyId === reader.keyId && known.secret === reader.secret));
}

/** Records the packed distribution as the project's latest release. Returns
 * true when the file changed. */
export async function recordReleaseLineage(projectRoot: string, gameId: string,
    lineage: ReleaseLineage): Promise<boolean> {
  const path = join(projectRoot, releaseLineageFile);
  const text = `${JSON.stringify({
    format: "PrismatiXReleaseLineage", schemaRevision: 1, gameId,
    packageFingerprint: lineage.packageFingerprint,
    contentVersion: lineage.contentVersion, saveVersion: lineage.saveVersion,
    compatibleSaveFingerprints: [...lineage.compatibleSaveFingerprints].sort(),
    legacyReaders: lineage.legacyReaders,
  }, null, 2)}\n`;
  if (await boundedText(path, 1024 * 1024).catch(() => undefined) === text) return false;
  await mkdir(join(projectRoot, ".prismatix-release"), {recursive: true});
  const temporary = `${path}.${process.pid}.tmp`;
  try {
    await writeFile(temporary, text, "utf8");
    await rename(temporary, path);
  } finally {
    await rm(temporary, {force: true});
  }
  return true;
}

/** Explains when the package about to be written would reject saves from the
 * previous release: no lineage is known and nothing migrates into the current
 * version, or the version changed without a migration from the last one. */
export function saveCompatibilityWarning(project: VersionedProject,
    previous: ReleaseLineage | undefined): string | undefined {
  const migrations = project.saveMigrations ?? [];
  const current = `contentVersion ${JSON.stringify(project.contentVersion)} / ` +
    `saveVersion ${project.saveVersion}`;
  if (previous === undefined) {
    if (migrations.some(migration => migration.to.contentVersion === project.contentVersion &&
        migration.to.saveVersion === project.saveVersion))
      return undefined;
    return `No previous release lineage was found (${releaseLineageFile}, the output ` +
      `folder, or --previous-package). If a build of ${project.id} with ${current} ` +
      "has already shipped, this package gets a new fingerprint and the Player will " +
      "reject every save from that release (PXSAVE6111). Restore " +
      `${releaseLineageFile} from version control, pass --previous-package with the ` +
      "old distribution, or advance contentVersion/saveVersion with a save migration. " +
      `For a first release, commit the ${releaseLineageFile} that packing writes.`;
  }
  if (previous.contentVersion === project.contentVersion &&
      previous.saveVersion === project.saveVersion)
    return undefined;
  if (migrations.some(migration =>
      migration.from.contentVersion === previous.contentVersion &&
      migration.from.saveVersion === previous.saveVersion))
    return undefined;
  return `The previous release used contentVersion ${JSON.stringify(previous.contentVersion)} / ` +
    `saveVersion ${previous.saveVersion}, but this package uses ${current} and no ` +
    "saveMigrations entry starts from the previous version. The Player will reject " +
    "saves from the previous release. Declare a save migration from that version.";
}
