import {randomUUID} from "node:crypto";
import {lstat, readFile, rename, rm, writeFile} from "node:fs/promises";
import {dirname, join} from "node:path";

import {currentCompilerIdentity, regularDirectory} from "./story-disk-cache.js";

const format = "PrismatiXJsxUiCache/1";
const cacheName = "jsx-ui-v1.json";
const maxBytes = 64 * 1024 * 1024;

/** Where a compiled UI node was authored; `fileName` is relative to the project root. */
export interface CachedNodeSource {
  readonly fileName: string;
  readonly lineNumber: number;
  readonly columnNumber: number;
}

/**
 * One filesystem question module resolution asked, and its answer: whether a
 * file or directory exists, a file's text (its SHA-256, "" when unreadable),
 * or a real path ("" when it failed). Asking them again answers whether an
 * import would still resolve the same way, without loading TypeScript.
 */
export interface ResolutionProbe {
  readonly kind: "file" | "directory" | "read" | "realpath";
  readonly path: string;
  readonly result: boolean | string;
}

/** One TSX screen or component as it compiled, and what that result depended on. */
export interface JsxUiCacheEntry {
  /** Every module the TypeScript program read, relative to the project root, with the SHA-256 of its text. */
  readonly files: readonly {readonly path: string; readonly sha256: string}[];
  /** What resolving those modules' relative imports looked at. */
  readonly probes: readonly ResolutionProbe[];
  readonly value: unknown;
  readonly nodeSources: readonly (readonly [number, CachedNodeSource])[];
  readonly warnings: readonly unknown[];
}

/**
 * Compiled TSX UI kept between builds in .prismatix/cache: type-checking and
 * evaluating a screen takes the TypeScript compiler about half a second, and
 * a game's dozen screens rarely change between two Plays. An entry is used
 * only when every module it read has the same text and every import still
 * resolves to the same file, with the same project folder, CLI, SDK, Node
 * and TypeScript. Like the Story cache it is a local performance hint: only
 * screens that compiled cleanly, and import nothing but the SDK and their
 * own project's modules, are kept.
 */
export class JsxUiCache {
  readonly #entries: Map<string, JsxUiCacheEntry>;
  readonly #used = new Set<string>();
  #changed = false;

  constructor(entries: Map<string, JsxUiCacheEntry> = new Map()) {
    this.#entries = entries;
  }

  get(key: string): JsxUiCacheEntry | undefined {
    return this.#entries.get(key);
  }

  /** Marks an entry as still part of the project, so the next save keeps it. */
  use(key: string): void {
    this.#used.add(key);
  }

  set(key: string, entry: JsxUiCacheEntry): void {
    this.#entries.set(key, entry);
    this.#used.add(key);
    this.#changed = true;
  }

  /** The entries to write, or undefined when the file on disk is already current. */
  pending(): Map<string, JsxUiCacheEntry> | undefined {
    const kept = new Map([...this.#entries].filter(([key]) => this.#used.has(key)));
    return this.#changed || kept.size !== this.#entries.size ? kept : undefined;
  }
}

function cachePath(root: string): string {
  return join(root, ".prismatix", "cache", cacheName);
}

export async function loadJsxUiCache(root: string, toolIdentity: string): Promise<JsxUiCache> {
  try {
    const path = cachePath(root);
    if (!await regularDirectory(dirname(dirname(path)), false) ||
        !await regularDirectory(dirname(path), false)) return new JsxUiCache();
    const info = await lstat(path);
    if (!info.isFile() || info.isSymbolicLink() || info.size > maxBytes) return new JsxUiCache();
    const value = JSON.parse(await readFile(path, "utf8")) as {
      format?: unknown; compiler?: unknown; tool?: unknown; entries?: unknown;
    };
    if (value?.format !== format || value.compiler !== currentCompilerIdentity() ||
        value.tool !== toolIdentity || value.entries === null || typeof value.entries !== "object" ||
        Array.isArray(value.entries)) return new JsxUiCache();
    return new JsxUiCache(new Map(Object.entries(value.entries as Record<string, JsxUiCacheEntry>)));
  } catch { /* A missing, stale, or damaged cache always falls back to compilation. */ }
  return new JsxUiCache();
}

export async function saveJsxUiCache(root: string, toolIdentity: string, cache: JsxUiCache): Promise<void> {
  const entries = cache.pending();
  if (entries === undefined) return;
  let temporary: string | undefined;
  try {
    const path = cachePath(root);
    const cacheRoot = dirname(path);
    if (!await regularDirectory(dirname(cacheRoot), true) ||
        !await regularDirectory(cacheRoot, true)) return;
    const json = JSON.stringify({format, compiler: currentCompilerIdentity(), tool: toolIdentity,
      entries: Object.fromEntries(entries)});
    if (Buffer.byteLength(json, "utf8") > maxBytes) return;
    temporary = join(cacheRoot, `.${cacheName}.${randomUUID()}.tmp`);
    await writeFile(temporary, json, {flag: "wx", mode: 0o600});
    await rename(temporary, path);
    temporary = undefined;
  } catch { /* Cache IO cannot fail an otherwise valid build. */ }
  finally { if (temporary !== undefined) await rm(temporary, {force: true}).catch(() => {}); }
}
