import {createHash, randomUUID} from "node:crypto";
import {createReadStream} from "node:fs";
import filesystem from "node:fs/promises";
import {lstat, mkdir, mkdtemp, readFile, readdir, realpath, rm, writeFile} from "node:fs/promises";
import {basename, dirname, isAbsolute, join, relative, resolve} from "node:path";

export const outputOwnershipMarker = ".prismatix-output.json";
export const maxOutputMarkerBytes = 16 * 1024 * 1024;
export interface PublishedInput {
  readonly uri: string;
  readonly fingerprint: string;
  readonly size: number;
}
export interface PublishedScanProgress {
  readonly files: number;
  readonly bytes: number;
  readonly done: boolean;
}
export interface PublicationProgress {
  readonly phase: "verify-existing" | "clone-existing" | "fingerprint-output" |
    "verify-before-publish" | "verify-reuse-source";
  readonly files: number;
  readonly bytes: number;
  readonly done: boolean;
}
export interface PublicationOptions {
  output: string;
  projectRoot: string;
  /** Author directories and every build/package input, including external dependencies. */
  protectedPaths: readonly string[];
  /** Only for builders that never read the old output. The old output is still
   * fully verified immediately before replacement; this skips an otherwise
   * redundant pre-build hash pass over potentially multi-GiB artifacts. */
  readonly verifyExistingBeforeBuild?: boolean;
  /** Recheck external published revisions linked into staging before promotion. */
  readonly beforePromotion?: () => Promise<void>;
  /** Files hard-linked from this exact output by the current build. Their
   * existing marker hashes may be reused only while both paths retain the
   * same file identity; the old output is still content-verified at promotion. */
  readonly linkedOutputFiles?: ReadonlyMap<string, PublishedInput>;
  /** A development output the CLI keeps for itself (in <project>/.prismatix):
   * before replacing it, only its ownership marker is checked, not every
   * file's content again. Never for an output an author may edit. */
  readonly trustExistingOutput?: boolean;
  readonly onProgress?: (value: PublicationProgress) => void;
}

function missing(error: unknown): boolean {
  return (error as NodeJS.ErrnoException).code === "ENOENT";
}
function key(path: string): string { return process.platform === "win32" ? path.toLowerCase() : path; }
function contains(parent: string, child: string): boolean {
  const rel = relative(key(parent), key(child));
  return rel === "" || (!isAbsolute(rel) && rel !== ".." && !rel.startsWith(`..${process.platform === "win32" ? "\\" : "/"}`));
}
/** Resolve even missing destinations through their nearest existing ancestor. */
async function canonical(path: string): Promise<string> {
  const absolute = resolve(path);
  try { return await realpath(absolute); }
  catch (error) {
    if (!missing(error) || dirname(absolute) === absolute) throw error;
    return join(await canonical(dirname(absolute)), basename(absolute));
  }
}
/**
 * canonical() for many paths at once. A large game protects tens of thousands
 * of files in a few hundred directories: each directory is resolved and listed
 * once, and a plain file in it resolves to its directory's real path plus its
 * name. Anything else (a directory, a link, a missing path) is resolved on its
 * own, so the result is the same as calling canonical() on every path.
 */
async function canonicalAll(paths: readonly string[]): Promise<string[]> {
  const byDirectory = new Map<string, {real: string; files: ReadonlySet<string>} | null>();
  const directories = [...new Set(paths.map((path) => dirname(resolve(path))))];
  for (let start = 0; start < directories.length; start += 64) {
    await Promise.all(directories.slice(start, start + 64).map(async (directory) => {
      try {
        const [real, entries] = await Promise.all([realpath(directory), readdir(directory, {withFileTypes: true})]);
        byDirectory.set(directory, {real, files: new Set(entries.filter((entry) => entry.isFile()).map((entry) => entry.name))});
      } catch { byDirectory.set(directory, null); }
    }));
  }
  const result: string[] = new Array<string>(paths.length);
  const others: number[] = [];
  for (const [index, path] of paths.entries()) {
    const absolute = resolve(path);
    const directory = byDirectory.get(dirname(absolute));
    if (directory?.files.has(basename(absolute))) result[index] = join(directory.real, basename(absolute));
    else others.push(index);
  }
  for (let start = 0; start < others.length; start += 256) {
    const batch = others.slice(start, start + 256);
    const resolved = await Promise.all(batch.map((index) => canonical(paths[index]!)));
    for (const [offset, index] of batch.entries()) result[index] = resolved[offset]!;
  }
  return result;
}
async function validatePaths(options: PublicationOptions): Promise<string> {
  const output = await canonical(options.output);
  const root = await canonical(options.projectRoot);
  if (contains(output, root)) throw new Error(`Unsafe output overlaps project root: ${options.output}`);
  const inputs = [...new Set(options.protectedPaths)];
  for (const [index, source] of (await canonicalAll(inputs)).entries()) {
    if (contains(output, source) || contains(source, output))
      throw new Error(`Unsafe output overlaps author input: ${inputs[index]}`);
  }
  try {
    const info = await lstat(options.output);
    if (!info.isDirectory() || info.isSymbolicLink()) throw new Error(`Unsafe output is not a regular directory: ${options.output}`);
  } catch (error) { if (!missing(error)) throw error; }
  return output;
}
/**
 * SHA-256 fingerprints remembered by file identity (volume, file id, size and
 * modification time), like git's index. Publication re-verifies multi-GiB
 * outputs several times per build; an unchanged file is read once. Any edit
 * changes the modification time and is hashed again. The cache sits beside
 * the output directory and only ever holds identities seen in its last run.
 */
const fingerprintCacheFile = ".prismatix-fingerprints.json";
const fingerprintCaches = new Map<string, {path: string; known: Map<string, string>; seen: Map<string, string>}>();

async function fingerprintCache(directory: string) {
  const path = join(directory, fingerprintCacheFile);
  let cache = fingerprintCaches.get(key(path));
  if (cache === undefined) {
    const known = new Map<string, string>();
    try {
      const parsed: unknown = JSON.parse(await readFile(path, "utf8"));
      if (parsed !== null && typeof parsed === "object" && !Array.isArray(parsed))
        for (const [identity, fingerprint] of Object.entries(parsed as Record<string, unknown>))
          if (typeof fingerprint === "string" && /^[0-9a-f]{64}$/u.test(fingerprint))
            known.set(identity, fingerprint);
    } catch { /* A missing or damaged cache only costs a full hash. */ }
    cache = {path, known, seen: new Map()};
    fingerprintCaches.set(key(path), cache);
  }
  return cache;
}

async function cachedFingerprint(path: string, cacheDirectory: string | undefined,
  onChunk?: (bytes: number) => void): Promise<string> {
  if (cacheDirectory === undefined) return fingerprintFile(path, onChunk);
  const info = await lstat(path, {bigint: true});
  // Without a stable file id the identity would not survive renames.
  if (info.ino === 0n) return fingerprintFile(path, onChunk);
  const identity = `${info.dev}:${info.ino}:${info.size}:${info.mtimeNs}`;
  const cache = await fingerprintCache(cacheDirectory);
  let fingerprint = cache.known.get(identity) ?? cache.seen.get(identity);
  if (fingerprint === undefined) fingerprint = await fingerprintFile(path, onChunk);
  else onChunk?.(Number(info.size));
  cache.seen.set(identity, fingerprint);
  return fingerprint;
}

async function saveFingerprintCache(directory: string): Promise<void> {
  const cache = fingerprintCaches.get(key(join(directory, fingerprintCacheFile)));
  if (cache === undefined || cache.seen.size === 0) return;
  try {
    await writeFile(cache.path, JSON.stringify(Object.fromEntries(cache.seen)));
    cache.known = cache.seen;
    cache.seen = new Map();
  } catch { /* The cache is an optimization; publication already succeeded. */ }
}

export async function fingerprintFile(path: string, onChunk?: (bytes: number) => void): Promise<string> {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) {
    hash.update(chunk);
    onChunk?.(Buffer.byteLength(chunk));
  }
  return hash.digest("hex");
}
async function assertLinkedIdentity(output: string, staging: string,
  uri: string, size: number): Promise<boolean> {
  const oldPath = join(output, ...uri.split("/"));
  const stagedPath = join(staging, ...uri.split("/"));
  const [oldFile, stagedFile] = await Promise.all([
    lstat(oldPath, {bigint: true}), lstat(stagedPath, {bigint: true}),
  ]);
  if (!oldFile.isFile() || oldFile.isSymbolicLink() ||
      !stagedFile.isFile() || stagedFile.isSymbolicLink() ||
      oldFile.dev !== stagedFile.dev || oldFile.size !== BigInt(size) ||
      stagedFile.size !== BigInt(size)) {
    throw new Error(`Reused artifact file is no longer linked to existing output: ${uri}`);
  }
  // Some filesystems expose no stable file ID or link count. On those, keep
  // the ordinary staged-content hash instead of making rebuilds fail.
  if (oldFile.ino === 0n || stagedFile.ino === 0n ||
      oldFile.nlink < 2n || stagedFile.nlink < 2n) return false;
  if (oldFile.ino !== stagedFile.ino)
    throw new Error(`Reused artifact file is no longer linked to existing output: ${uri}`);
  return true;
}

async function inventory(root: string, phase?: PublicationProgress["phase"],
  onProgress?: PublicationOptions["onProgress"],
  linkedOutput?: {root: string; files: ReadonlyMap<string, PublishedInput>; verified: Set<string>},
  cacheDirectory: string | undefined = dirname(resolve(root))): Promise<Record<string, string>> {
  const entries: Record<string, string> = Object.create(null) as Record<string, string>;
  const fileEntries: {readonly name: string; readonly path: string}[] = [];
  const seenLinks = new Set<string>();
  let files = 0;
  let bytes = 0;
  let lastReport = Date.now();
  let lastFiles = 0;
  const report = (done = false): void => {
    if (phase === undefined || onProgress === undefined) return;
    const now = Date.now();
    if (!done && files - lastFiles < 512 && now - lastReport < 2000) return;
    onProgress({phase, files, bytes, done});
    lastReport = now;
    lastFiles = files;
  };
  if (phase !== undefined && onProgress !== undefined)
    onProgress({phase, files: 0, bytes: 0, done: false});
  async function visit(dir: string): Promise<void> {
    for (const entry of await readdir(dir, {withFileTypes: true})) {
      const path = join(dir, entry.name);
      const name = relative(root, path).split("\\").join("/");
      if (name === outputOwnershipMarker) continue;
      if (entry.isSymbolicLink()) throw new Error(`Unsafe output contains a symbolic link: ${path}`);
      if (entry.isDirectory()) { entries[name] = "directory"; await visit(path); }
      else if (entry.isFile()) {
        // Reserve the insertion order so published markers remain stable even
        // though bounded workers complete fingerprints out of order.
        entries[name] = "";
        fileEntries.push({name, path});
      }
      else throw new Error(`Unsafe output contains a special file: ${path}`);
    }
  }
  await visit(root);
  let nextFile = 0;
  let failed = false;
  let failure: unknown;
  const worker = async (): Promise<void> => {
    while (!failed && nextFile < fileEntries.length) {
      const {name, path} = fileEntries[nextFile++]!;
      try {
        const linked = linkedOutput?.files.get(name);
        if (linked !== undefined) {
          if (await assertLinkedIdentity(linkedOutput!.root, root, name, linked.size)) {
            entries[name] = linked.fingerprint;
            linkedOutput!.verified.add(name);
            bytes += linked.size;
          } else {
            entries[name] = await cachedFingerprint(path, cacheDirectory,
              onProgress === undefined ? undefined : (size) => {
                bytes += size;
                report();
              });
          }
          seenLinks.add(name);
        } else {
          entries[name] = await cachedFingerprint(path, cacheDirectory,
            onProgress === undefined ? undefined : (size) => {
              bytes += size;
              report();
            });
        }
        files += 1;
        report();
      } catch (error) {
        failed = true;
        failure = error;
      }
    }
  };
  // Keep open files and disk contention bounded for large VN artifacts.
  await Promise.all(Array.from({length: Math.min(8, fileEntries.length)}, worker));
  if (failed) throw failure;
  if (linkedOutput !== undefined && seenLinks.size !== linkedOutput.files.size)
    throw new Error("A reused artifact file is missing from staged output");
  report(true);
  return entries;
}
function inventoryDifference(expected: Record<string, string>, actual: Record<string, string>,
  output: string): string | undefined {
  const location = (name: string): string => {
    const absolute = resolve(output, ...name.split("/"));
    if (actual[name] !== "directory") return `${name} (${absolute})`;
    const empty = !Object.keys(actual).some((entry) => entry.startsWith(`${name}/`));
    return `${name} (${absolute}; ${empty ? "empty" : "nonempty"} directory)`;
  };
  for (const name of Object.keys(expected).sort()) {
    if (!Object.hasOwn(actual, name)) return `owned entry is missing: ${location(name)}`;
    if (expected[name] !== actual[name]) return `owned entry was modified: ${location(name)}`;
  }
  for (const name of Object.keys(actual).sort()) {
    if (!Object.hasOwn(expected, name)) {
      const nested = Object.keys(actual).filter((entry) => entry.startsWith(`${name}/`))
        .sort((left, right) => right.split("/").length - left.split("/").length ||
          left.localeCompare(right, "en"))[0];
      return `unowned entry was added: ${location(name)}` +
        (nested === undefined ? "" : `; nested entry: ${location(nested)}`);
    }
  }
  return undefined;
}
async function assertOwned(output: string, allowEmpty = true,
  phase?: PublicationProgress["phase"], onProgress?: PublicationOptions["onProgress"]): Promise<void> {
  let names: string[];
  try { names = await readdir(output); } catch (error) { if (missing(error) && allowEmpty) return; throw error; }
  if (names.length === 0 && allowEmpty) return;
  try {
    const markerPath = join(output, outputOwnershipMarker);
    const markerInfo = await lstat(markerPath);
    if (!markerInfo.isFile() || markerInfo.isSymbolicLink() ||
        markerInfo.size > maxOutputMarkerBytes) throw new Error("invalid marker");
    const marker = JSON.parse(await readFile(markerPath, "utf8")) as {format?: string; files?: Record<string, string>};
    if (marker.format !== "PrismatiXOutput/1" || !marker.files ||
        typeof marker.files !== "object" || Array.isArray(marker.files))
      throw new Error("invalid ownership marker");
    const difference = inventoryDifference(marker.files,
      await inventory(output, phase, onProgress), output);
    if (difference !== undefined) throw new Error(difference);
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    throw new Error(`Refusing to replace unowned or modified output directory: ${output} (${detail}). ` +
      "Inspect the listed entry or choose a new --output; the existing output was left untouched.",
    {cause: error});
  }
}

/** An existing output carries a valid ownership marker (or does not exist). */
async function assertMarked(output: string): Promise<void> {
  try { await lstat(output); } catch (error) { if (missing(error)) return; throw error; }
  const markerPath = join(output, outputOwnershipMarker);
  const info = await lstat(markerPath).catch(() => undefined);
  const marker: unknown = info?.isFile() && !info.isSymbolicLink() && info.size <= maxOutputMarkerBytes
    ? JSON.parse(await readFile(markerPath, "utf8")) : undefined;
  if (marker === null || typeof marker !== "object" || Array.isArray(marker) ||
      (marker as {format?: unknown}).format !== "PrismatiXOutput/1")
    throw new Error(`Refusing to replace an unowned output directory: ${output}`);
}

/** Read an existing artifact without rehashing multi-GiB media. The native
 * Packager verifies each supplied SHA-256 and size before accepting it. */
export async function publishedInputs(output: string,
  onProgress?: (value: PublishedScanProgress) => void,
  requireOwnedDirectories = false): Promise<readonly PublishedInput[]> {
  const root = resolve(output);
  const info = await lstat(root);
  if (!info.isDirectory() || info.isSymbolicLink())
    throw new Error(`Artifact is not a regular directory: ${root}`);
  const markerPath = join(root, outputOwnershipMarker);
  const markerInfo = await lstat(markerPath);
  if (!markerInfo.isFile() || markerInfo.isSymbolicLink() || markerInfo.size > maxOutputMarkerBytes)
    throw new Error(`Artifact ownership marker is invalid: ${markerPath}`);
  const marker: unknown = JSON.parse(await readFile(markerPath, "utf8"));
  if (marker === null || typeof marker !== "object" || Array.isArray(marker) ||
      (marker as {format?: unknown}).format !== "PrismatiXOutput/1")
    throw new Error(`Artifact ownership marker has an unsupported format: ${markerPath}`);
  const files = (marker as {files?: unknown}).files;
  if (files === null || typeof files !== "object" || Array.isArray(files))
    throw new Error(`Artifact ownership marker has no file inventory: ${markerPath}`);
  const expected = files as Record<string, unknown>;
  const found = new Set<string>();
  const inputs: PublishedInput[] = [];
  const caseFolded = new Set<string>();
  let bytes = 0;
  let lastReport = Date.now();
  let lastFiles = 0;
  const report = (done = false): void => {
    if (onProgress === undefined) return;
    const now = Date.now();
    if (!done && inputs.length - lastFiles < 512 && now - lastReport < 2000) return;
    onProgress({files: inputs.length, bytes, done});
    lastReport = now;
    lastFiles = inputs.length;
  };
  onProgress?.({files: 0, bytes: 0, done: false});
  const visit = async (directory: string): Promise<number> => {
    let ownedFiles = 0;
    for (const entry of await readdir(directory, {withFileTypes: true})) {
      const path = join(directory, entry.name);
      const uri = relative(root, path).split("\\").join("/");
      if (uri === outputOwnershipMarker) continue;
      if (!uri || uri.startsWith("/") || uri.includes(":") ||
          uri.split("/").some((part) => !part || part === "." || part === ".."))
        throw new Error(`Artifact has an unsafe path: ${uri}`);
      const folded = process.platform === "win32" ? uri.toLowerCase() : uri;
      if (caseFolded.has(folded)) throw new Error(`Artifact has duplicate paths: ${uri}`);
      caseFolded.add(folded);
      if (entry.isSymbolicLink()) throw new Error(`Artifact contains a symbolic link: ${uri}`);
      const expectedValue = Object.hasOwn(expected, uri) ? expected[uri] : undefined;
      if (entry.isDirectory()) {
        if (expectedValue !== undefined && expectedValue !== "directory")
          throw new Error(`Artifact directory is not owned: ${uri}`);
        if (requireOwnedDirectories && expectedValue === undefined)
          throw new Error(`Artifact directory is not owned: ${uri}`);
        const children = await visit(path);
        // Legacy artifact post-processors sometimes listed every file but
        // omitted the containing directory. Packager rechecks file hashes;
        // accept this only when the implicit directory has owned files.
        if (expectedValue === undefined && children === 0)
          throw new Error(`Artifact directory is not owned: ${uri}`);
        if (expectedValue === "directory") found.add(uri);
        ownedFiles += children;
      } else if (entry.isFile()) {
        if (typeof expectedValue !== "string" || !/^[0-9a-f]{64}$/iu.test(expectedValue))
          throw new Error(`Artifact file has no valid fingerprint: ${uri}`);
        found.add(uri);
        const size = (await lstat(path)).size;
        inputs.push({uri, fingerprint: expectedValue, size});
        bytes += size;
        ++ownedFiles;
        report();
      } else throw new Error(`Artifact contains a special file: ${uri}`);
    }
    return ownedFiles;
  };
  await visit(root);
  if (found.size !== Object.keys(expected).length ||
      Object.keys(expected).some((uri) => !found.has(uri)))
    throw new Error(`Artifact ownership marker does not match directory contents: ${root}`);
  report(true);
  return inputs.sort((left, right) => left.uri.localeCompare(right.uri, "en"));
}

/** Reuse candidates must be content-verified, unlike Packager inputs whose
 * fingerprints are independently checked by native before packaging. */
export async function verifiedPublishedInputs(output: string,
  onProgress?: PublicationOptions["onProgress"]): Promise<readonly PublishedInput[]> {
  await assertOwned(output, false, "verify-reuse-source", onProgress);
  return publishedInputs(output);
}

/** Keep an unchanged owned output without making a staging tree. The caller
 * must content-hash desired sources before this call, or use beforeRetain for
 * a final source check under the lock. Existing output bytes are always
 * checked against the complete ownership marker. */
export async function retainPublishedDirectoryIfMatching(options: PublicationOptions,
  expectedFiles: ReadonlyMap<string, string>,
  beforeRetain?: () => Promise<boolean>): Promise<boolean> {
  const output = await validatePaths(options);
  const expected: Record<string, string> = Object.create(null) as Record<string, string>;
  for (const [uri, fingerprint] of expectedFiles) {
    const parts = uri.split("/");
    if (!uri || uri === outputOwnershipMarker || uri.startsWith("/") ||
        uri.includes("\\") || uri.includes(":") ||
        uri.includes("\0") ||
        parts.some(part => !part || part === "." || part === "..") ||
        !/^[0-9a-f]{64}$/u.test(fingerprint))
      throw new Error(`Invalid unchanged-artifact input: ${uri}`);
    if (expected[uri] !== undefined)
      throw new Error(`Unchanged-artifact path collision: ${uri}`);
    expected[uri] = fingerprint;
    for (let count = 1; count < parts.length; count += 1) {
      const parent = parts.slice(0, count).join("/");
      if (expected[parent] !== undefined && expected[parent] !== "directory")
        throw new Error(`Unchanged-artifact path collision: ${parent}`);
      expected[parent] = "directory";
    }
  }
  const lock = await acquireOutputLock(output);
  try {
    if (key(await validatePaths(options)) !== key(output))
      throw new Error("Output path changed during unchanged-artifact verification");
    let markerText: string;
    try { markerText = await readFile(join(output, outputOwnershipMarker), "utf8"); }
    catch (error) { if (missing(error)) return false; throw error; }
    if (Buffer.byteLength(markerText, "utf8") > maxOutputMarkerBytes)
      throw new Error(`Artifact ownership marker exceeds ${maxOutputMarkerBytes} bytes`);
    const marker: unknown = JSON.parse(markerText);
    if (marker === null || typeof marker !== "object" || Array.isArray(marker) ||
        (marker as {format?: unknown}).format !== "PrismatiXOutput/1" ||
        (marker as {files?: unknown}).files === null ||
        typeof (marker as {files?: unknown}).files !== "object" ||
        Array.isArray((marker as {files?: unknown}).files))
      throw new Error(`Artifact ownership marker is invalid: ${output}`);
    const current = (marker as {files: Record<string, unknown>}).files;
    if (Object.keys(current).length !== Object.keys(expected).length ||
        Object.entries(expected).some(([uri, fingerprint]) => current[uri] !== fingerprint))
      return false;
    await assertOwned(output, false, "verify-before-publish", options.onProgress);
    if (beforeRetain !== undefined && !await beforeRetain()) return false;
    return true;
  } finally { await rm(lock, {recursive: true}); }
}

async function acquireOutputLock(output: string): Promise<string> {
  const lock = join(dirname(output), `.prismatix-${createHash("sha256").update(key(output)).digest("hex").slice(0, 24)}.lock`);
  try { await mkdir(lock); } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "EEXIST") throw new Error(`Output is locked by another publication (or an interrupted process): ${output}`);
    throw error;
  }
  return lock;
}

/** Exclusive cooperative publication. Unknown backups and modified outputs are never removed. */
export async function publishDirectory<T>(options: PublicationOptions, build: (staging: string) => Promise<T>): Promise<T> {
  const output = await validatePaths(options);
  await mkdir(dirname(output), {recursive: true});
  const lock = await acquireOutputLock(output);
  let staging: string | undefined;
  try {
    if (options.verifyExistingBeforeBuild !== false)
      await assertOwned(output, true, "verify-existing", options.onProgress);
    staging = await mkdtemp(join(dirname(output), ".prismatix-stage-"));
    const value = await build(staging);
    // A callback must not impersonate the ownership marker or supply symbolic links.
    try { await lstat(join(staging, outputOwnershipMarker)); throw new Error("Build produced reserved output ownership marker"); }
    catch (error) { if (!missing(error)) throw error; }
    let linkedOutput: {root: string; files: ReadonlyMap<string, PublishedInput>; verified: Set<string>} | undefined;
    if (options.linkedOutputFiles !== undefined && options.linkedOutputFiles.size > 0) {
      const linkedFiles = new Map(options.linkedOutputFiles);
      const markerPath = join(output, outputOwnershipMarker);
      const markerInfo = await lstat(markerPath);
      if (!markerInfo.isFile() || markerInfo.isSymbolicLink() ||
          markerInfo.size > maxOutputMarkerBytes)
        throw new Error(`Existing output marker is invalid: ${markerPath}`);
      const marker: unknown = JSON.parse(await readFile(markerPath, "utf8"));
      if (marker === null || typeof marker !== "object" || Array.isArray(marker) ||
          (marker as {format?: unknown}).format !== "PrismatiXOutput/1" ||
          (marker as {files?: unknown}).files === null ||
          typeof (marker as {files?: unknown}).files !== "object" ||
          Array.isArray((marker as {files?: unknown}).files))
        throw new Error(`Existing output marker is invalid: ${markerPath}`);
      const oldFiles = (marker as {files: Record<string, unknown>}).files;
      for (const [uri, input] of linkedFiles) {
        if (uri !== input.uri || !uri || uri.startsWith("/") || uri.includes("\\") ||
            uri.includes(":") || uri.split("/").some(part => !part || part === "." || part === "..") ||
            !Number.isSafeInteger(input.size) || input.size < 0 ||
            !/^[0-9a-f]{64}$/u.test(input.fingerprint) ||
            !Object.hasOwn(oldFiles, uri) || oldFiles[uri] !== input.fingerprint)
          throw new Error(`Reused artifact file is not owned by existing output: ${uri}`);
      }
      linkedOutput = {root: output, files: linkedFiles, verified: new Set()};
    }
    const marker = JSON.stringify({format: "PrismatiXOutput/1",
      files: await inventory(staging, "fingerprint-output", options.onProgress, linkedOutput)});
    if (Buffer.byteLength(marker, "utf8") > maxOutputMarkerBytes)
      throw new Error(`Artifact ownership marker exceeds ${maxOutputMarkerBytes} bytes`);
    await writeFile(join(staging, outputOwnershipMarker), marker, {flag: "wx"});
    await options.beforePromotion?.();
    if (key(await validatePaths(options)) !== key(output)) throw new Error("Output path changed during publication");
    if (options.trustExistingOutput === true) await assertMarked(output);
    else await assertOwned(output, true, "verify-before-publish", options.onProgress);
    if (linkedOutput !== undefined)
      for (const uri of linkedOutput.verified)
        if (!await assertLinkedIdentity(output, staging, uri,
          linkedOutput.files.get(uri)!.size))
          throw new Error(`Reused artifact file identity became unavailable: ${uri}`);
    const backup = join(dirname(output), `.prismatix-backup-${randomUUID()}`);
    let backedUp = false;
    try { await filesystem.rename(output, backup); backedUp = true; } catch (error) { if (!missing(error)) throw error; }
    try { await filesystem.rename(staging, output); staging = undefined; }
    catch (error) {
      if (backedUp) {
        try { await filesystem.rename(backup, output); }
        catch (restoreError) { throw new AggregateError([error, restoreError], `Publication failed; previous output is preserved at ${backup}`); }
      }
      throw error;
    }
    if (backedUp) await rm(backup, {recursive: true});
    await saveFingerprintCache(dirname(output));
    return value;
  } finally {
    try { if (staging) await rm(staging, {recursive: true, force: true}); }
    finally { await rm(lock, {recursive: true}); }
  }
}


/** Retire only an intact published revision after its consumers have exited. */
export async function retirePublishedDirectory(options: PublicationOptions): Promise<void> {
  const output = await validatePaths(options);
  try { await lstat(output); } catch (error) { if (missing(error)) return; throw error; }
  const lock = await acquireOutputLock(output);
  try {
    if (key(await validatePaths(options)) !== key(output)) throw new Error("Output path changed during retirement");
    // Unlike first publication, an empty unmarked directory is not ours to delete.
    await assertOwned(output, false);
    const retired = join(dirname(output), `.prismatix-retired-${randomUUID()}`);
    await filesystem.rename(output, retired);
    try {
      // Catch changes made outside the cooperative lock before the rename.
      await assertOwned(retired, false);
      await filesystem.rm(retired, {recursive: true});
    } catch (error) {
      // Never rename back over a newly created output, or delete an unknown backup.
      throw new Error(`Output retirement could not finish; remaining files are preserved at ${retired}`, {cause: error});
    }
  } finally { await rm(lock, {recursive: true}); }
}
