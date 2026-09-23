import {createHash, randomUUID} from "node:crypto";
import {createReadStream} from "node:fs";
import filesystem from "node:fs/promises";
import {lstat, mkdir, mkdtemp, readFile, readdir, realpath, rm, writeFile} from "node:fs/promises";
import {basename, dirname, isAbsolute, join, relative, resolve} from "node:path";

export const outputOwnershipMarker = ".prismatix-output.json";
export interface PublishedInput {
  readonly uri: string;
  readonly fingerprint: string;
  readonly size: number;
}
export interface PublicationOptions {
  output: string;
  projectRoot: string;
  /** Author directories and every build/package input, including external dependencies. */
  protectedPaths: readonly string[];
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
async function validatePaths(options: PublicationOptions): Promise<string> {
  const output = await canonical(options.output);
  const root = await canonical(options.projectRoot);
  if (contains(output, root)) throw new Error(`Unsafe output overlaps project root: ${options.output}`);
  for (const input of options.protectedPaths) {
    const source = await canonical(input);
    if (contains(output, source) || contains(source, output)) {
      throw new Error(`Unsafe output overlaps author input: ${input}`);
    }
  }
  try {
    const info = await lstat(options.output);
    if (!info.isDirectory() || info.isSymbolicLink()) throw new Error(`Unsafe output is not a regular directory: ${options.output}`);
  } catch (error) { if (!missing(error)) throw error; }
  return output;
}
async function digest(path: string): Promise<string> {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return hash.digest("hex");
}
async function inventory(root: string): Promise<Record<string, string>> {
  const entries: Record<string, string> = Object.create(null) as Record<string, string>;
  async function visit(dir: string): Promise<void> {
    for (const entry of await readdir(dir, {withFileTypes: true})) {
      const path = join(dir, entry.name);
      const name = relative(root, path).split("\\").join("/");
      if (name === outputOwnershipMarker) continue;
      if (entry.isSymbolicLink()) throw new Error(`Unsafe output contains a symbolic link: ${path}`);
      if (entry.isDirectory()) { entries[name] = "directory"; await visit(path); }
      else if (entry.isFile()) entries[name] = await digest(path);
      else throw new Error(`Unsafe output contains a special file: ${path}`);
    }
  }
  await visit(root);
  return entries;
}
function sameInventory(a: Record<string, string>, b: Record<string, string>): boolean {
  const names = Object.keys(a);
  return names.length === Object.keys(b).length && names.every(name => a[name] === b[name]);
}
async function assertOwned(output: string, allowEmpty = true): Promise<void> {
  let names: string[];
  try { names = await readdir(output); } catch (error) { if (missing(error) && allowEmpty) return; throw error; }
  if (names.length === 0 && allowEmpty) return;
  try {
    const markerPath = join(output, outputOwnershipMarker);
    if (!(await lstat(markerPath)).isFile()) throw new Error("invalid marker");
    const marker = JSON.parse(await readFile(markerPath, "utf8")) as {format?: string; files?: Record<string, string>};
    if (marker.format !== "PrismatiXOutput/1" || !marker.files || !sameInventory(marker.files, await inventory(output))) throw new Error("output changed");
  } catch (error) {
    throw new Error(`Refusing to replace unowned or modified output directory: ${output}`, {cause: error});
  }
}

/** Read an existing artifact without rehashing multi-GiB media. The native
 * Packager verifies each supplied SHA-256 and size before accepting it. */
export async function publishedInputs(output: string): Promise<readonly PublishedInput[]> {
  const root = resolve(output);
  const info = await lstat(root);
  if (!info.isDirectory() || info.isSymbolicLink())
    throw new Error(`Artifact is not a regular directory: ${root}`);
  const markerPath = join(root, outputOwnershipMarker);
  const markerInfo = await lstat(markerPath);
  if (!markerInfo.isFile() || markerInfo.isSymbolicLink() || markerInfo.size > 16 * 1024 * 1024)
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
  const visit = async (directory: string): Promise<void> => {
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
        if (expectedValue !== "directory") throw new Error(`Artifact directory is not owned: ${uri}`);
        found.add(uri);
        await visit(path);
      } else if (entry.isFile()) {
        if (typeof expectedValue !== "string" || !/^[0-9a-f]{64}$/iu.test(expectedValue))
          throw new Error(`Artifact file has no valid fingerprint: ${uri}`);
        found.add(uri);
        inputs.push({uri, fingerprint: expectedValue, size: (await lstat(path)).size});
      } else throw new Error(`Artifact contains a special file: ${uri}`);
    }
  };
  await visit(root);
  if (found.size !== Object.keys(expected).length ||
      Object.keys(expected).some((uri) => !found.has(uri)))
    throw new Error(`Artifact ownership marker does not match directory contents: ${root}`);
  return inputs.sort((left, right) => left.uri.localeCompare(right.uri, "en"));
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
    await assertOwned(output);
    staging = await mkdtemp(join(dirname(output), ".prismatix-stage-"));
    const value = await build(staging);
    // A callback must not impersonate the ownership marker or supply symbolic links.
    try { await lstat(join(staging, outputOwnershipMarker)); throw new Error("Build produced reserved output ownership marker"); }
    catch (error) { if (!missing(error)) throw error; }
    await writeFile(join(staging, outputOwnershipMarker), JSON.stringify({format: "PrismatiXOutput/1", files: await inventory(staging)}), {flag: "wx"});
    if (key(await validatePaths(options)) !== key(output)) throw new Error("Output path changed during publication");
    await assertOwned(output);
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
