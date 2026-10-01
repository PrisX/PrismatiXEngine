import {createHash} from "node:crypto";
import {spawn} from "node:child_process";
import {mkdir, readFile, readdir, rename, rm, stat, writeFile} from "node:fs/promises";
import {extname, join} from "node:path";

import type {ProjectTranscode} from "@prismatix/authoring-sdk";

import {fingerprintFile} from "./output-safety.js";

// distribution.transcode: smaller encodings of a project's PNG and Ogg/WAV
// media for distribution builds. The Packager encodes (`--transcode`); this
// module chooses the files, keeps a content-addressed cache under
// .prismatix/transcode-cache so unchanged media is never encoded twice, and
// tells the artifact builder which logical paths to fill from the cache.
// Logical paths never change: the Player detects WebP and Opus by content.

/** Bump when the encoder's output for the same settings changes. */
const encoderRevision = "px-transcode-1";
const imageExtensions = new Set([".png"]);
const audioExtensions = new Set([".ogg", ".wav"]);

export interface TranscodeDependency {
  readonly logicalPath: string;
  readonly source: string;
}

export interface TranscodedFile {
  /** Cached encoding to link or copy into the artifact. */
  readonly file: string;
  readonly fingerprint: string;
  readonly size: number;
}

export interface TranscodeTotals {
  files: number;
  inputBytes: number;
  outputBytes: number;
}

export interface TranscodeSummary {
  readonly images: TranscodeTotals;
  readonly audio: TranscodeTotals;
  /** Files encoded by this run (the rest came from the cache). */
  readonly encoded: number;
  readonly cached: number;
  /** Candidates that stay original because re-encoding would not shrink them. */
  readonly kept: number;
  readonly failed: readonly {readonly path: string; readonly detail: string}[];
  /** Set when the Packager could not transcode at all. */
  readonly unavailable?: string;
}

export interface TranscodeOptions {
  readonly root: string;
  readonly settings: ProjectTranscode;
  readonly dependencies: readonly TranscodeDependency[];
  readonly sourcePath: (dependency: TranscodeDependency) => string;
  readonly tool: () => Promise<string>;
  readonly onProgress?: (phase: "transcode-scan" | "transcode", current: number,
    total: number, bytes: number, done: boolean) => void;
}

interface CacheEntry {
  readonly result: "encoded" | "kept";
  readonly fingerprint?: string;
  readonly size?: number;
  readonly reason?: string;
}

interface Candidate {
  readonly dependency: TranscodeDependency;
  readonly kind: "image" | "audio";
  readonly source: string;
  readonly sourceSize: number;
  readonly key: string;
}

export function transcodeCacheDirectory(root: string): string {
  return join(root, ".prismatix", "transcode-cache");
}

/** `*` and `?` stay within one path segment, `**` crosses segments. */
export function globMatcher(patterns: readonly string[]): (path: string) => boolean {
  const expressions = patterns.map((pattern) => {
    let source = "";
    for (let index = 0; index < pattern.length; index += 1) {
      const character = pattern[index]!;
      if (character === "*" && pattern[index + 1] === "*") {
        const slash = pattern[index + 2] === "/";
        source += slash ? "(?:.*/)?" : ".*";
        index += slash ? 2 : 1;
      } else if (character === "*") source += "[^/]*";
      else if (character === "?") source += "[^/]";
      else source += character.replace(/[.+^${}()|[\]\\]/gu, "\\$&");
    }
    return new RegExp(`^${source}$`, "iu");
  });
  return (path) => expressions.some((expression) => expression.test(path));
}

function encoderSettings(settings: ProjectTranscode): {images?: object; audio?: object} {
  const images = settings.images === undefined ? undefined : {
    lossless: settings.images.quality === "lossless",
    quality: typeof settings.images.quality === "number" ? settings.images.quality : 90,
    effort: settings.images.effort ?? 4,
  };
  const audio = settings.audio === undefined ? undefined
    : settings.audio.format === "opus"
      ? {codec: "opus", bitrate: (settings.audio.bitrate ?? 96) * 1000}
      : {codec: "vorbis", quality: settings.audio.quality ?? 4};
  return {...(images === undefined ? {} : {images}), ...(audio === undefined ? {} : {audio})};
}

function entryPaths(root: string, key: string): {json: string; bin: string; directory: string} {
  const directory = join(transcodeCacheDirectory(root), key.slice(0, 2));
  return {directory, json: join(directory, `${key}.json`), bin: join(directory, `${key}.bin`)};
}

async function readEntry(root: string, key: string): Promise<CacheEntry | undefined> {
  const paths = entryPaths(root, key);
  try {
    const entry = JSON.parse(await readFile(paths.json, "utf8")) as CacheEntry;
    if (entry.result === "kept") return entry;
    if (entry.result !== "encoded" || typeof entry.fingerprint !== "string" ||
        !/^[0-9a-f]{64}$/u.test(entry.fingerprint) || !Number.isSafeInteger(entry.size))
      return undefined;
    const info = await stat(paths.bin);
    return info.isFile() && info.size === entry.size ? entry : undefined;
  } catch {
    return undefined;
  }
}

async function inBatches<T>(items: readonly T[], size: number,
  task: (item: T) => Promise<void>): Promise<void> {
  for (let start = 0; start < items.length; start += size) {
    const results = await Promise.allSettled(items.slice(start, start + size).map(task));
    const failed = results.find((result) => result.status === "rejected");
    if (failed?.status === "rejected") throw failed.reason;
  }
}

function runTool(tool: string, args: readonly string[],
  onLine: (line: string) => void): Promise<{code: number; stderr: string}> {
  return new Promise((accept, reject) => {
    const script = [".js", ".mjs", ".cjs"].includes(extname(tool).toLowerCase());
    const child = spawn(script ? process.execPath : tool, script ? [tool, ...args] : [...args],
      {stdio: ["ignore", "pipe", "pipe"], windowsHide: true});
    let pending = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => {
      pending += chunk;
      const lines = pending.split("\n");
      pending = lines.pop() ?? "";
      for (const line of lines) if (line.trim().length > 0) onLine(line);
    });
    child.stderr.on("data", (chunk: string) => { stderr = (stderr + chunk).slice(-65536); });
    const interrupt = (): void => { child.kill(); };
    process.once("SIGINT", interrupt);
    process.once("SIGTERM", interrupt);
    child.on("error", (error) => {
      process.removeListener("SIGINT", interrupt);
      process.removeListener("SIGTERM", interrupt);
      reject(error);
    });
    child.on("close", (code) => {
      process.removeListener("SIGINT", interrupt);
      process.removeListener("SIGTERM", interrupt);
      if (pending.trim().length > 0) onLine(pending);
      accept({code: code ?? 1, stderr});
    });
  });
}

/**
 * Encodes every eligible dependency that has no cached result, then returns
 * the logical paths whose smaller encoding replaces the original.
 */
export async function transcodeMedia(options: TranscodeOptions): Promise<{
  readonly files: ReadonlyMap<string, TranscodedFile>;
  readonly summary: TranscodeSummary;
}> {
  const {root, settings} = options;
  const excluded = globMatcher(settings.exclude ?? []);
  const encoder = encoderSettings(settings);
  const eligible = options.dependencies.flatMap((dependency) => {
    const extension = extname(dependency.logicalPath).toLowerCase();
    const kind = imageExtensions.has(extension) && settings.images !== undefined ? "image" as const
      : audioExtensions.has(extension) && settings.audio !== undefined ? "audio" as const
        : undefined;
    if (kind === undefined || excluded(dependency.source) || excluded(dependency.logicalPath))
      return [];
    return [{dependency, kind}];
  });

  // Content addressing: the key covers the source bytes, the encoder
  // settings for its kind and the encoder revision, never paths or times.
  const candidates: Candidate[] = [];
  let scanned = 0;
  let scannedBytes = 0;
  options.onProgress?.("transcode-scan", 0, eligible.length, 0, eligible.length === 0);
  await inBatches(eligible, 16, async ({dependency, kind}) => {
    const source = options.sourcePath(dependency);
    const info = await stat(source);
    const fingerprint = await fingerprintFile(source);
    const key = createHash("sha256").update([encoderRevision, kind,
      JSON.stringify(kind === "image" ? encoder.images : encoder.audio), fingerprint].join("\0"))
      .digest("hex");
    candidates.push({dependency, kind, source, sourceSize: info.size, key});
    scanned += 1;
    scannedBytes += info.size;
    if (scanned % 512 === 0 || scanned === eligible.length)
      options.onProgress?.("transcode-scan", scanned, eligible.length, scannedBytes,
        scanned === eligible.length);
  });

  const files = new Map<string, TranscodedFile>();
  const totals = {image: {files: 0, inputBytes: 0, outputBytes: 0},
    audio: {files: 0, inputBytes: 0, outputBytes: 0}};
  const failed: {path: string; detail: string}[] = [];
  let kept = 0;
  let cached = 0;
  let encoded = 0;
  const accept = (candidate: Candidate, entry: CacheEntry): void => {
    const total = totals[candidate.kind];
    total.files += 1;
    total.inputBytes += candidate.sourceSize;
    if (entry.result === "encoded") {
      files.set(candidate.dependency.logicalPath, {file: entryPaths(root, candidate.key).bin,
        fingerprint: entry.fingerprint!, size: entry.size!});
      total.outputBytes += entry.size!;
    } else {
      total.outputBytes += candidate.sourceSize;
      kept += 1;
    }
  };

  const misses: Candidate[] = [];
  const unique = new Map<string, Candidate[]>();
  for (const candidate of candidates) {
    const sharing = unique.get(candidate.key);
    if (sharing === undefined) unique.set(candidate.key, [candidate]);
    else sharing.push(candidate);
  }
  await inBatches([...unique.values()], 32, async (sharing) => {
    const entry = await readEntry(root, sharing[0]!.key);
    if (entry === undefined) { misses.push(sharing[0]!); return; }
    for (const candidate of sharing) { accept(candidate, entry); cached += 1; }
  });

  let unavailable: string | undefined;
  if (misses.length > 0) {
    const tool = await options.tool();
    const cache = transcodeCacheDirectory(root);
    await mkdir(cache, {recursive: true});
    const temporary = (candidate: Candidate): string =>
      `${entryPaths(root, candidate.key).bin}.tmp-${process.pid}`;
    for (const directory of new Set(misses.map((miss) => entryPaths(root, miss.key).directory)))
      await mkdir(directory, {recursive: true});
    const requestPath = join(cache, `request-${process.pid}.json`);
    await writeFile(requestPath, `${JSON.stringify({
      format: "PrismatiXTranscodeRequest", schemaRevision: 1, ...encoder,
      jobs: misses.map((miss) => ({kind: miss.kind, source: miss.source, output: temporary(miss)})),
    })}\n`, "utf8");
    const finished = new Set<number>();
    const writes: Promise<void>[] = [];
    let processedBytes = 0;
    let lastReport = 0;
    options.onProgress?.("transcode", 0, misses.length, 0, false);
    try {
      const result = await runTool(tool, ["--transcode", requestPath], (line) => {
        let event: Record<string, unknown>;
        try { event = JSON.parse(line) as Record<string, unknown>; } catch { return; }
        if (event.event === "failed") {
          unavailable = String(event.message ?? event.code ?? "transcoding failed");
          return;
        }
        if (event.event !== "transcoded") return;
        const index = Number(event.index);
        const miss = misses[index];
        if (miss === undefined || finished.has(index)) return;
        finished.add(index);
        processedBytes += miss.sourceSize;
        const now = Date.now();
        if (finished.size === misses.length || now - lastReport > 2000) {
          lastReport = now;
          options.onProgress?.("transcode", finished.size, misses.length, processedBytes,
            finished.size === misses.length);
        }
        writes.push((async () => {
          const paths = entryPaths(root, miss.key);
          let entry: CacheEntry | undefined;
          if (event.result === "encoded") {
            const output = temporary(miss);
            const info = await stat(output);
            entry = {result: "encoded", size: info.size, fingerprint: await fingerprintFile(output)};
            await rename(output, paths.bin);
          } else if (event.reason === "larger" || event.reason === "unsupported") {
            entry = {result: "kept", reason: String(event.reason)};
          } else {
            failed.push({path: miss.dependency.source, detail: String(event.detail ?? event.reason)});
          }
          if (entry !== undefined) {
            await writeFile(paths.json, `${JSON.stringify(entry)}\n`, "utf8");
            for (const candidate of unique.get(miss.key)!) accept(candidate, entry);
            encoded += entry.result === "encoded" ? 1 : 0;
          } else {
            for (const candidate of unique.get(miss.key)!) {
              totals[candidate.kind].files += 1;
              totals[candidate.kind].inputBytes += candidate.sourceSize;
              totals[candidate.kind].outputBytes += candidate.sourceSize;
            }
          }
        })());
      });
      await Promise.all(writes);
      if (unavailable === undefined && (result.code !== 0 || finished.size !== misses.length))
        unavailable = `Packager transcoding exited with code ${result.code}` +
          (result.stderr.trim().length > 0 ? `: ${result.stderr.trim().split("\n").at(-1)}` : "");
    } finally {
      await rm(requestPath, {force: true});
      await Promise.allSettled(misses.map((miss) => rm(temporary(miss), {force: true})));
    }
    // Media the Packager never answered for stays original in this build and
    // is retried next time.
    for (const [index, miss] of misses.entries()) {
      if (finished.has(index)) continue;
      for (const candidate of unique.get(miss.key)!) {
        totals[candidate.kind].files += 1;
        totals[candidate.kind].inputBytes += candidate.sourceSize;
        totals[candidate.kind].outputBytes += candidate.sourceSize;
      }
    }
  }
  if (unavailable === undefined) await pruneCache(root, new Set(candidates.map((value) => value.key)));
  return {files, summary: {images: totals.image, audio: totals.audio, encoded, cached, kept,
    failed, ...(unavailable === undefined ? {} : {unavailable})}};
}

// The cache holds exactly the current encodings: entries for edited or
// removed media, or for earlier settings, are deleted after a complete run.
async function pruneCache(root: string, live: ReadonlySet<string>): Promise<void> {
  const cache = transcodeCacheDirectory(root);
  let shards: string[];
  try { shards = await readdir(cache); } catch { return; }
  for (const shard of shards) {
    if (!/^[0-9a-f]{2}$/u.test(shard)) continue;
    const directory = join(cache, shard);
    let names: string[];
    try { names = await readdir(directory); } catch { continue; }
    await inBatches(names, 64, async (name) => {
      const key = name.slice(0, 64);
      if (!live.has(key)) await rm(join(directory, name), {force: true});
    });
  }
}
