import {createHash, randomUUID} from "node:crypto";
import {constants, readFileSync, readdirSync} from "node:fs";
import {lstat, mkdir, open, rename, rm, writeFile} from "node:fs/promises";
import {dirname, join} from "node:path";
import {fileURLToPath} from "node:url";
import {gunzipSync, gzipSync} from "node:zlib";

import {StoryProjectCompilationCache} from "@prismatix/authoring-sdk";

const format = "PrismatiXStoryDiskCache/1";
const maxCompressedBytes = 128 * 1024 * 1024;
const maxJsonBytes = 512 * 1024 * 1024;
const cacheName = "story-scenes-v1.json.gz";

function cachePath(root: string): string {
  return join(root, ".prismatix", "cache", cacheName);
}

function hashCompiledTree(hash: ReturnType<typeof createHash>, root: string): void {
  const visit = (directory: string): void => {
    for (const entry of readdirSync(directory, {withFileTypes: true})
      .sort((a, b) => a.name.localeCompare(b.name, "en"))) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) visit(path);
      else if (entry.isFile() && entry.name.endsWith(".js")) {
        hash.update(path.slice(root.length)).update("\0");
        hash.update(readFileSync(path)).update("\0");
      }
    }
  };
  visit(root);
}

let compilerIdentity: string | undefined;
function currentCompilerIdentity(): string {
  if (compilerIdentity !== undefined) return compilerIdentity;
  const hash = createHash("sha256").update(format).update("\0")
    .update(process.versions.node).update("\0");
  hashCompiledTree(hash, fileURLToPath(new URL(".", import.meta.url)));
  hashCompiledTree(hash, dirname(fileURLToPath(import.meta.resolve("@prismatix/authoring-sdk"))));
  compilerIdentity = hash.digest("hex");
  return compilerIdentity;
}

async function regularDirectory(path: string, create: boolean): Promise<boolean> {
  try {
    const info = await lstat(path);
    return info.isDirectory() && !info.isSymbolicLink();
  } catch (error) {
    if (!create || (error as NodeJS.ErrnoException).code !== "ENOENT") return false;
    try { await mkdir(path); } catch { return false; }
    const info = await lstat(path);
    return info.isDirectory() && !info.isSymbolicLink();
  }
}

async function readBoundedCache(path: string): Promise<Buffer | undefined> {
  const handle = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  try {
    const info = await handle.stat();
    if (!info.isFile() || info.size > maxCompressedBytes) return undefined;
    const parts: Buffer[] = [];
    let size = 0;
    while (true) {
      const part = Buffer.allocUnsafe(Math.min(64 * 1024, maxCompressedBytes - size + 1));
      const {bytesRead} = await handle.read(part, 0, part.length, null);
      if (bytesRead === 0) break;
      size += bytesRead;
      if (size > maxCompressedBytes) return undefined;
      parts.push(part.subarray(0, bytesRead));
    }
    return Buffer.concat(parts, size);
  } finally { await handle.close(); }
}

/** A local performance hint, never an authority for artifact publication. */
export async function loadStoryCompilationCache(root: string): Promise<StoryProjectCompilationCache> {
  const cache = new StoryProjectCompilationCache();
  try {
    const path = cachePath(root);
    if (!await regularDirectory(dirname(dirname(path)), false) ||
        !await regularDirectory(dirname(path), false)) return cache;
    const info = await lstat(path);
    if (!info.isFile() || info.isSymbolicLink()) return cache;
    const compressed = await readBoundedCache(path);
    if (compressed === undefined) return cache;
    const json = gunzipSync(compressed, {maxOutputLength: maxJsonBytes});
    const value = JSON.parse(json.toString("utf8")) as {
      format?: unknown; compiler?: unknown; scenes?: unknown;
    };
    if (value?.format === format && value.compiler === currentCompilerIdentity())
      cache.importScenes(value.scenes);
  } catch { /* A missing, stale, or damaged cache always falls back to compilation. */ }
  return cache;
}

export async function saveStoryCompilationCache(root: string,
  cache: StoryProjectCompilationCache): Promise<void> {
  if (!cache.needsPersistence()) return;
  let temporary: string | undefined;
  try {
    const path = cachePath(root);
    const cacheRoot = dirname(path);
    const stateRoot = dirname(cacheRoot);
    if (!await regularDirectory(stateRoot, true) ||
        !await regularDirectory(cacheRoot, true)) return;
    const json = JSON.stringify({format, compiler: currentCompilerIdentity(),
      scenes: cache.exportScenes()});
    if (Buffer.byteLength(json, "utf8") > maxJsonBytes) return;
    const compressed = gzipSync(json, {level: 1});
    if (compressed.length > maxCompressedBytes) return;
    temporary = join(cacheRoot, `.${cacheName}.${randomUUID()}.tmp`);
    await writeFile(temporary, compressed, {flag: "wx", mode: 0o600});
    await rename(temporary, path);
    temporary = undefined;
    cache.markPersisted();
  } catch { /* Cache IO cannot fail an otherwise valid build. */ }
  finally { if (temporary !== undefined) await rm(temporary, {force: true}).catch(() => {}); }
}
