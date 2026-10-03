import {spawn} from "node:child_process";
import {createHash} from "node:crypto";
import {copyFile, mkdir, open, readFile, readdir, stat, writeFile} from "node:fs/promises";
import {basename, dirname, extname, join, relative, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {fingerprintFile, publishDirectory} from "./output-safety.js";
import {readWebShaderArtifact} from "./web-pdx.js";
import {convertWebShader} from "./web-shader.js";

export const webTemplateRoot = fileURLToPath(new URL("../../templates/web", import.meta.url));
const chunkSize = 1024 * 1024;
interface Chunk {url: string; size: number; sha256: string}
interface WebFile {path: string; size: number; chunks: Chunk[]}
export interface WebExportOptions {
  projectRoot: string;
  distribution: string;
  playerDirectory: string;
  output: string;
  ffmpeg?: string;
  onProgress?: (message: string) => void;
}
function safePath(path: string): boolean {
  return path.length > 0 && !path.startsWith("/") && !/[\\:\0]/u.test(path) &&
    path.split("/").every(part => part.length > 0 && part !== "." && part !== "..");
}
async function regularFiles(root: string, prefix = ""): Promise<string[]> {
  const result: string[] = [];
  for (const entry of await readdir(join(root, prefix), {withFileTypes: true})) {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (!safePath(path) || entry.isSymbolicLink()) throw new Error(`Unsafe export input: ${path}`);
    if (entry.isDirectory()) result.push(...await regularFiles(root, path));
    else if (entry.isFile() && !entry.name.startsWith(".prismatix")) result.push(path);
  }
  return result.sort();
}
async function runFfmpeg(executable: string, source: string, output: string): Promise<void> {
  await new Promise<void>((accept, reject) => {
    const child = spawn(executable, ["-nostdin", "-hide_banner", "-loglevel", "error",
      "-i", source, "-map", "0:v:0", "-map", "0:a?", "-c:v", "libx264",
      "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "aac",
      "-b:a", "192k", "-movflags", "+faststart", "-y", output], {windowsHide: true});
    let error = "";
    child.stderr.on("data", (data: Buffer) => { error = (error + data.toString()).slice(-8192); });
    child.on("error", reject);
    child.on("close", code => code === 0 ? accept() : reject(new Error(`Video transcode failed (${code}): ${error}`)));
  });
}
export async function resolveWebPlayer(directory: string): Promise<string> {
  const root = resolve(directory);
  const metadata = JSON.parse(await readFile(join(root, "web-player.json"), "utf8")) as
    {format?: string; schemaRevision?: number; variant?: string; artifacts?: Record<string, string>; notices?: string[]; sources?: string[]};
  if (metadata.format !== "PrismatiXWebPlayer" || metadata.schemaRevision !== 1 || metadata.variant !== "shipped")
    throw new Error("--web-player must select a shipping Web Player; Preview and QA runtimes cannot be exported");
  for (const filename of ["prismatix-player.mjs", "prismatix-player.wasm", "web-shaders.json", ...(metadata.notices ?? []), ...(metadata.sources ?? [])]) {
    if (!safePath(filename)) throw new Error(`Unsafe Web Player artifact path: ${filename}`);
    if (metadata.artifacts?.[filename] !== await fingerprintFile(join(root, filename)))
      throw new Error(`Web Player artifact checksum mismatch: ${filename}`);
  }
  const handle = await open(join(root, "prismatix-player.wasm"), "r");
  try {
    const header = Buffer.alloc(8);
    const {bytesRead} = await handle.read(header, 0, 8, 0);
    if (bytesRead !== 8 || !header.equals(Buffer.from([0, 97, 115, 109, 1, 0, 0, 0])))
      throw new Error("Web Player is not a WebAssembly module");
  } finally { await handle.close(); }
  return root;
}

/** Immutable chunks support on-demand reads, CDN hosting and offline playback
 * without depending on HTTP Range support or allocating a complete PDX shard. */
export async function exportWeb(options: WebExportOptions) {
  const player = await resolveWebPlayer(options.playerDirectory);
  const source = resolve(options.distribution);
  const packageManifest = JSON.parse(await readFile(join(source, "Package", "manifest.json"), "utf8")) as
    {format: string; schemaRevision: number; gameId: string; title: string; packageFingerprint: string;
      archives: {file: string}[]; resolution: {width: number; height: number}; encryption?: {archiveKey?: string};
      defaultLocale?: string; customEffects?: {id: string; artifacts: {format: string; asset: string; fingerprint: string}[]}[]};
  if (packageManifest.format !== "PrismatiXPackageManifest" || packageManifest.schemaRevision !== 1 ||
      !Array.isArray(packageManifest.archives) || packageManifest.archives.length === 0)
    throw new Error("Web export needs a valid packaged distribution");
  const index = JSON.parse(await readFile(join(source, "Package", "content-index.json"), "utf8")) as
    {entries: {uri: string}[]};
  const videoPaths = [...new Set(index.entries.map(entry => entry.uri).filter(path =>
    /\.(?:mp4|webm|mkv|avi|mov|ogv|mpg|mpeg|wmv)$/iu.test(path)))].sort();
  const videoSources = new Map<string, string>();
  for (const path of videoPaths) {
    if (!safePath(path)) throw new Error(`Unsafe video path: ${path}`);
    const video = join(options.projectRoot, ...path.split("/"));
    if (!(await stat(video)).isFile()) throw new Error(`Video source is missing: ${video}`);
    videoSources.set(path, video);
  }
  const inputs = [...await regularFiles(source)].filter(path => path.startsWith("Package/") ||
    path.startsWith("Notices/") || ![".exe", ".dll", ".so", ".dylib", ".pdb", ".map"].includes(extname(path)));
  for (const archive of packageManifest.archives)
    if (!safePath(archive.file) || !inputs.includes(archive.file)) throw new Error(`Missing packaged archive: ${archive.file}`);
  return publishDirectory({output: options.output, projectRoot: options.projectRoot,
    protectedPaths: [source, player, ...videoSources.values()]}, async staging => {
    const files: WebFile[] = [], videos: Record<string, string> = {}, effects: Record<string, {fingerprint: string; path: string}> = {};
    await mkdir(join(staging, "_data"), {recursive: true});
    const addFile = async (path: string, input: string) => {
      const handle = await open(input, "r");
      const chunks: Chunk[] = [];
      let size = 0;
      try {
        const bytes = Buffer.alloc(chunkSize);
        for (;;) {
          const {bytesRead} = await handle.read(bytes, 0, bytes.length, size);
          if (!bytesRead) break;
          const data = bytes.subarray(0, bytesRead);
          const sha256 = createHash("sha256").update(data).digest("hex");
          const url = `_data/${sha256}`;
          try { await writeFile(join(staging, url), data, {flag: "wx"}); }
          catch (error) { if ((error as NodeJS.ErrnoException).code !== "EEXIST") throw error; }
          chunks.push({url, size: bytesRead, sha256});
          size += bytesRead;
        }
      } finally { await handle.close(); }
      files.push({path, size, chunks});
    };
    for (let i = 0; i < inputs.length; i++) {
      const path = inputs[i]!;
      options.onProgress?.(`Exporting ${i + 1}/${inputs.length}: ${path}`);
      await addFile(path, join(source, ...path.split("/")));
    }
    const cache = join(options.projectRoot, ".prismatix", "web-video-cache");
    await mkdir(cache, {recursive: true});
    for (const [path, video] of videoSources) {
      const key = createHash("sha256").update("prismatix-web-avc-aac-1\0")
        .update(await fingerprintFile(video)).digest("hex");
      const encoded = join(cache, `${key}.mp4`);
      if (!await stat(encoded).catch(() => undefined)) {
        options.onProgress?.(`Encoding Web video: ${path}`);
        const temporary = join(cache, `${key}.pending.mp4`);
        await runFfmpeg(options.ffmpeg ?? "ffmpeg", video, temporary);
        // A failed/partial encoding never becomes a reusable cache entry.
        const {rename} = await import("node:fs/promises");
        await rename(temporary, encoded);
      }
      const url = `media/${key}.mp4`;
      await addFile(url, encoded);
      videos[path] = url;
    }
    for (const effect of packageManifest.customEffects ?? []) {
      const artifact = effect.artifacts.find(value => value.format === "spirv");
      if (!artifact || !safePath(artifact.asset)) throw new Error(`Custom effect needs a SPIR-V artifact: ${effect.id}`);
      let bytes: Buffer | undefined;
      for (const archive of packageManifest.archives) {
        bytes = await readWebShaderArtifact(join(source, archive.file), artifact.asset, packageManifest.encryption?.archiveKey);
        if (bytes) break;
      }
      if (!bytes || createHash("sha256").update(bytes).digest("hex") !== artifact.fingerprint)
        throw new Error(`Custom effect artifact integrity mismatch: ${effect.id}`);
      options.onProgress?.(`Converting Web effect: ${effect.id}`);
      const temporary = join(cache, `${artifact.fingerprint}.spv`);
      await writeFile(temporary, bytes);
      const suffix = process.platform === "win32" ? ".exe" : "";
      const shader = await convertWebShader(temporary, join(cache, artifact.fingerprint),
        process.env.PRISMATIX_SPIRV_CROSS ?? join(player, "tools", `spirv-cross${suffix}`),
        process.env.PRISMATIX_NAGA ?? join(player, "tools", `naga${suffix}`));
      const json = join(cache, `${artifact.fingerprint}.json`);
      await writeFile(json, JSON.stringify(shader));
      const path = `effects/${artifact.fingerprint}.json`;
      await addFile(path, json); effects[effect.id] = {fingerprint: artifact.fingerprint, path};
    }
    for (const filename of ["prismatix-player.mjs", "prismatix-player.wasm", "web-player.json", "web-shaders.json"])
      await copyFile(join(player, filename), join(staging, filename));
    const playerMetadata = JSON.parse(await readFile(join(player, "web-player.json"), "utf8")) as {notices?: string[]; sources?: string[]};
    const notices = playerMetadata.notices ?? [];
    for (const filename of [...notices, ...(playerMetadata.sources ?? [])]) {
      await mkdir(dirname(join(staging, filename)), {recursive: true});
      await copyFile(join(player, filename), join(staging, filename));
    }
    const templates = ["index.html", "player.mjs", "files.mjs", "video.mjs", "effects.mjs", "accessibility.mjs", "sw.mjs", "styles.css"];
    for (const filename of templates) await copyFile(join(webTemplateRoot, filename), join(staging, filename));
    const projectConfig = JSON.parse(await readFile(join(options.projectRoot, "prismatix.json"), "utf8").catch(() => "{}")) as {defaultLocale?: string};
    const manifest = {format: "PrismatiXWebExport", schemaRevision: 1,
      gameId: packageManifest.gameId, title: packageManifest.title,
      locale: packageManifest.defaultLocale ?? projectConfig.defaultLocale ?? "und",
      packageFingerprint: packageManifest.packageFingerprint, resolution: packageManifest.resolution,
      files: files.sort((a, b) => a.path.localeCompare(b.path)), videos, effects,
      shell: ["./", ...templates, "web-export.json", "prismatix-player.mjs", "prismatix-player.wasm", "web-player.json", "web-shaders.json", ...notices]};
    const identity = createHash("sha256").update(JSON.stringify(manifest));
    for (const filename of [...templates, "prismatix-player.mjs", "prismatix-player.wasm", "web-player.json", "web-shaders.json", ...notices, ...(playerMetadata.sources ?? [])])
      identity.update(await fingerprintFile(join(staging, filename)));
    const exportId = identity.digest("hex");
    const serviceWorker = await readFile(join(staging, "sw.mjs"), "utf8");
    await writeFile(join(staging, "sw.mjs"), serviceWorker.replaceAll("__PRISMATIX_EXPORT_ID__", exportId));
    await writeFile(join(staging, "web-export.json"), JSON.stringify({...manifest, exportId}));
    await writeFile(join(staging, "_headers"), "/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n  X-Content-Type-Options: nosniff\n  Cache-Control: no-cache\n\n/*.wasm\n  Content-Type: application/wasm\n\n/_data/*\n  Cache-Control: public, max-age=31536000, immutable\n");
    return {output: resolve(options.output), target: "web", gameId: manifest.gameId,
      files: files.length, videos: videoSources.size,
      bytes: files.reduce((sum, file) => sum + file.size, 0)};
  });
}
