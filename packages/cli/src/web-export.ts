import {spawn} from "node:child_process";
import {createHash} from "node:crypto";
import {readFileSync} from "node:fs";
import {copyFile, mkdir, open, readFile, readdir, stat, writeFile} from "node:fs/promises";
import {createRequire} from "node:module";
import {basename, delimiter, dirname, extname, join, relative, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {fingerprintFile, publishDirectory} from "./output-safety.js";
import {readWebShaderArtifact} from "./web-pdx.js";
import {checkoutRoot} from "./runtime-discovery.js";
import {convertWebShader, sdlGpuWgsl} from "./web-shader.js";
import {distributionDataRoot} from "./distribution-layout.js";

export const webTemplateRoot = fileURLToPath(new URL("../../templates/web", import.meta.url));
const chunkSize = 1024 * 1024;
interface Chunk {url: string; size: number; sha256: string}
interface WebFile {path: string; size: number; chunks: Chunk[]}
export interface WebExportOptions {
  projectRoot: string;
  distribution: string;
  playerDirectory: string;
  /** Which of the package's renderers the export uses (default webgl2). */
  renderer?: WebRenderer;
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
export const webRenderers = ["webgl2", "webgpu"] as const;
export type WebRenderer = typeof webRenderers[number];
const CLI_VERSION = (JSON.parse(readFileSync(new URL("../../package.json", import.meta.url), "utf8")) as {version: string}).version;
interface WebPlayerManifest {
  format?: string; schemaRevision?: number; variant?: string; engineVersion?: string; defaultRenderer?: string;
  artifacts?: Record<string, string>; notices?: string[]; sources?: string[];
  renderers?: Record<string, {directory?: string; artifacts?: Record<string, string>; notices?: string[]} & Record<string, unknown>>;
}
/** One renderer of a verified Web Player package: what an export copies, by export path. */
export interface ResolvedWebPlayer {
  root: string;
  renderer: WebRenderer;
  engineVersion: string;
  /** Export path to package path, for every file the export copies. */
  files: Map<string, string>;
  notices: string[];
  sources: string[];
  /** The export's own web-player.json: this renderer only, with export paths. */
  exportManifest: Record<string, unknown>;
}
/**
 * Verifies a shipping Web Player package (scripts/build-wasm-player.mjs,
 * schema revision 2) and selects one renderer: every shared file and every
 * file of that renderer must match its SHA-256, and the package must be this
 * CLI's version.
 */
export async function resolveWebPlayer(directory: string, renderer: WebRenderer = "webgl2"): Promise<ResolvedWebPlayer> {
  const root = resolve(directory);
  const metadata = JSON.parse(await readFile(join(root, "web-player.json"), "utf8")) as WebPlayerManifest;
  if (metadata.format !== "PrismatiXWebPlayer" || metadata.variant !== "shipped")
    throw new Error("--web-player must select a shipping Web Player; Preview and QA runtimes cannot be exported");
  if (metadata.schemaRevision !== 2)
    throw new Error(`Web Player schema revision ${String(metadata.schemaRevision)} is not supported; rebuild it with scripts/build-wasm-player.mjs`);
  if (metadata.engineVersion !== CLI_VERSION)
    throw new Error(`Web Player ${String(metadata.engineVersion)} does not match this CLI (${CLI_VERSION})`);
  const entry = metadata.renderers?.[renderer];
  if (!entry || typeof entry.directory !== "string" || !safePath(entry.directory))
    throw new Error(`This Web Player package has no ${renderer} renderer (it has ${Object.keys(metadata.renderers ?? {}).join(", ") || "none"})`);
  const prefix = `${entry.directory}/`;
  const files = new Map<string, string>();
  const verify = async (path: string, hashes: Record<string, string> | undefined, exportPath: string) => {
    if (!safePath(path) || !safePath(exportPath)) throw new Error(`Unsafe Web Player artifact path: ${path}`);
    if (hashes?.[path] !== await fingerprintFile(join(root, ...path.split("/"))))
      throw new Error(`Web Player artifact checksum mismatch: ${path}`);
    if (files.has(exportPath)) throw new Error(`Web Player artifact appears twice: ${exportPath}`);
    files.set(exportPath, path);
  };
  const unprefixed = (path: string) => {
    if (!path.startsWith(prefix)) throw new Error(`Web Player ${renderer} artifact outside ${prefix}: ${path}`);
    return path.slice(prefix.length);
  };
  for (const path of ["web-shaders.json", ...(metadata.notices ?? []), ...(metadata.sources ?? [])])
    await verify(path, metadata.artifacts, path);
  for (const path of [`${prefix}prismatix-player.mjs`, `${prefix}prismatix-player.wasm`, ...(entry.notices ?? [])])
    await verify(path, entry.artifacts, unprefixed(path));
  const handle = await open(join(root, ...`${prefix}prismatix-player.wasm`.split("/")), "r");
  try {
    const header = Buffer.alloc(8);
    const {bytesRead} = await handle.read(header, 0, 8, 0);
    if (bytesRead !== 8 || !header.equals(Buffer.from([0, 97, 115, 109, 1, 0, 0, 0])))
      throw new Error("Web Player is not a WebAssembly module");
  } finally { await handle.close(); }
  const notices = [...(metadata.notices ?? []), ...(entry.notices ?? []).map(unprefixed)];
  const sources = metadata.sources ?? [];
  const artifacts: Record<string, string> = {};
  for (const [exportPath, path] of files)
    artifacts[exportPath] = path.startsWith(prefix) ? entry.artifacts![path]! : metadata.artifacts![path]!;
  const details = Object.fromEntries(Object.entries(entry).filter(([key]) => !["directory", "artifacts", "notices"].includes(key)));
  return {root, renderer, engineVersion: metadata.engineVersion, files, notices, sources,
    exportManifest: {format: "PrismatiXWebPlayer", schemaRevision: 2, variant: "shipped", status: "experimental",
      engineVersion: metadata.engineVersion, renderer, ...details, artifacts, notices, sources}};
}

/** The npm package holding the Web Player (both renderers), installed on demand at exactly the CLI's version. */
export const webPlayerPackageName = "@prismatix/web-player";

export interface WebPlayerLocation {
  readonly directory: string;
  /** project: the project's own dependency; cli: resolvable from this CLI; checkout: an engine checkout's build. */
  readonly source: "project" | "cli" | "checkout";
  readonly version: string;
}
export interface WebPlayerInstall {
  readonly manager: "pnpm" | "npm";
  readonly args: readonly string[];
  /** The command line to show an author. */
  readonly command: string;
}

/** No Web Player of this CLI's version; `install` is the command that adds one to the project. */
export class WebPlayerMissing extends Error {
  constructor(readonly install: WebPlayerInstall, readonly projectRoot: string | undefined, readonly problems: readonly string[]) {
    super(`The Web Player is not installed. Web export uses ${webPlayerPackageName} at exactly this CLI's version ` +
      `(${CLI_VERSION}); add it to the project with \`${install.command}\`` +
      `${projectRoot === undefined ? "" : ` in ${projectRoot}`} (or \`prismatix add web-player\`), ` +
      `or pass --web-player <dir> (docs/web-export.md).${problems.length > 0 ? ` ${problems.join("; ")}.` : ""}`);
    this.name = "WebPlayerMissing";
  }
}

const isFile = (path: string) => stat(path).then(info => info.isFile(), () => false);

/**
 * How a project adds the Web Player: pinned exactly, like the project's other
 * @prismatix packages, with the package manager whose lockfile the project has
 * (pnpm, the documented tool, when it has none).
 */
export async function webPlayerInstall(projectRoot: string | undefined): Promise<WebPlayerInstall> {
  const npm = projectRoot !== undefined && !await isFile(join(projectRoot, "pnpm-lock.yaml")) &&
    await isFile(join(projectRoot, "package-lock.json"));
  const spec = `${webPlayerPackageName}@${CLI_VERSION}`;
  return npm ? {manager: "npm", args: ["install", "-D", "--save-exact", spec], command: `npm install -D --save-exact ${spec}`}
    : {manager: "pnpm", args: ["add", "-D", spec], command: `pnpm add -D ${spec}`};
}

/**
 * The Web Player `prismatix pack --target web` uses without --web-player:
 * @prismatix/web-player as the project resolves it (its own devDependency),
 * else as this CLI resolves it, else (CLI running from an engine checkout)
 * that checkout's out/package/wasm-player. A package of another version is
 * reported, not used.
 */
export async function locateWebPlayer(projectRoot: string | undefined,
  environment: NodeJS.ProcessEnv = process.env): Promise<WebPlayerLocation> {
  const problems: string[] = [];
  const resolvers: [WebPlayerLocation["source"], NodeJS.Require][] = [];
  if (projectRoot !== undefined) resolvers.push(["project", createRequire(join(resolve(projectRoot), "package.json"))]);
  resolvers.push(["cli", createRequire(import.meta.url)]);
  for (const [source, resolveFrom] of resolvers) {
    let manifestPath: string;
    try { manifestPath = resolveFrom.resolve(`${webPlayerPackageName}/package.json`); }
    catch { continue; }
    const directory = dirname(manifestPath);
    const version = (JSON.parse(await readFile(manifestPath, "utf8")) as {version?: unknown}).version;
    if (version !== CLI_VERSION) problems.push(`${directory} is ${webPlayerPackageName}@${String(version)}, not ${CLI_VERSION}`);
    else if (!await isFile(join(directory, "web-player.json"))) problems.push(`${directory} has no web-player.json`);
    else return {directory, source, version};
  }
  const checkout = await checkoutRoot(environment);
  if (checkout) {
    const directory = join(checkout, "out", "package", "wasm-player");
    if (await isFile(join(directory, "web-player.json"))) {
      const version = (JSON.parse(await readFile(join(directory, "web-player.json"), "utf8")) as WebPlayerManifest).engineVersion;
      if (version === CLI_VERSION) return {directory, source: "checkout", version};
      problems.push(`the engine checkout's Web Player in ${directory} is ${String(version)}, not ${CLI_VERSION}; ` +
        "rebuild it with scripts/build-wasm-player.mjs");
    } else problems.push(`the engine checkout has no Web Player build in ${directory} (scripts/build-wasm-player.mjs)`);
  }
  throw new WebPlayerMissing(await webPlayerInstall(projectRoot), projectRoot, problems);
}

/** A host shader converter: the environment override, the package's tools/, then PATH. */
async function shaderTool(name: "spirv-cross" | "naga", variable: string, player: string): Promise<string> {
  const suffix = process.platform === "win32" ? ".exe" : "";
  const candidates = [process.env[variable], join(player, "tools", `${name}${suffix}`),
    ...(process.env.PATH ?? "").split(delimiter).filter(Boolean).map(directory => join(directory, `${name}${suffix}`))];
  for (const candidate of candidates)
    if (candidate && await stat(candidate).then(info => info.isFile(), () => false)) return candidate;
  throw new Error(`Custom effects need the ${name} shader converter for Web export: set ${variable}, or put ${name} on PATH ` +
    "(see docs/web-export.md, custom effects)");
}

/** Immutable chunks support on-demand reads, CDN hosting and offline playback
 * without depending on HTTP Range support or allocating a complete PDX shard. */
export async function exportWeb(options: WebExportOptions) {
  const resolved = await resolveWebPlayer(options.playerDirectory, options.renderer ?? "webgl2");
  const player = resolved.root;
  // A desktop distribution's game data may sit in an engine folder or a .app.
  const source = await distributionDataRoot(resolve(options.distribution));
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
      const shader = await convertWebShader(temporary, join(cache, artifact.fingerprint),
        await shaderTool("spirv-cross", "PRISMATIX_SPIRV_CROSS", player), await shaderTool("naga", "PRISMATIX_NAGA", player));
      // The WebGPU renderer hands the WGSL to SDL_GPU, which binds differently.
      const json = join(cache, `${artifact.fingerprint}.${resolved.renderer}.json`);
      await writeFile(json, JSON.stringify(resolved.renderer === "webgpu" ? {...shader, sdlGpuWgsl: sdlGpuWgsl(shader.wgsl)} : shader));
      const path = `effects/${artifact.fingerprint}.json`;
      await addFile(path, json); effects[effect.id] = {fingerprint: artifact.fingerprint, path};
    }
    // Only the chosen renderer's files: its runtime and notices at the export
    // root, beside the shared shaders, notices and source archives.
    for (const [exportPath, packagePath] of resolved.files) {
      await mkdir(dirname(join(staging, ...exportPath.split("/"))), {recursive: true});
      await copyFile(join(player, ...packagePath.split("/")), join(staging, ...exportPath.split("/")));
    }
    await writeFile(join(staging, "web-player.json"), `${JSON.stringify(resolved.exportManifest, null, 2)}\n`);
    const notices = resolved.notices;
    const templates = ["index.html", "player.mjs", "files.mjs", "video.mjs", "effects.mjs", "accessibility.mjs", "sw.mjs", "styles.css"];
    for (const filename of templates) await copyFile(join(webTemplateRoot, filename), join(staging, filename));
    const projectConfig = JSON.parse(await readFile(join(options.projectRoot, "prismatix.json"), "utf8").catch(() => "{}")) as {defaultLocale?: string};
    const manifest = {format: "PrismatiXWebExport", schemaRevision: 1,
      gameId: packageManifest.gameId, title: packageManifest.title,
      locale: packageManifest.defaultLocale ?? projectConfig.defaultLocale ?? "und",
      packageFingerprint: packageManifest.packageFingerprint, resolution: packageManifest.resolution,
      renderer: resolved.renderer, engineVersion: resolved.engineVersion,
      files: files.sort((a, b) => a.path.localeCompare(b.path)), videos, effects,
      shell: ["./", ...templates, "web-export.json", "prismatix-player.mjs", "prismatix-player.wasm", "web-player.json", "web-shaders.json", ...notices]};
    const identity = createHash("sha256").update(JSON.stringify(manifest));
    for (const filename of [...templates, "prismatix-player.mjs", "prismatix-player.wasm", "web-player.json", "web-shaders.json", ...notices, ...resolved.sources])
      identity.update(await fingerprintFile(join(staging, filename)));
    const exportId = identity.digest("hex");
    const serviceWorker = await readFile(join(staging, "sw.mjs"), "utf8");
    await writeFile(join(staging, "sw.mjs"), serviceWorker.replaceAll("__PRISMATIX_EXPORT_ID__", exportId));
    await writeFile(join(staging, "web-export.json"), JSON.stringify({...manifest, exportId}));
    await writeFile(join(staging, "_headers"), "/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n  X-Content-Type-Options: nosniff\n  Cache-Control: no-cache\n\n/*.wasm\n  Content-Type: application/wasm\n\n/_data/*\n  Cache-Control: public, max-age=31536000, immutable\n");
    return {output: resolve(options.output), target: "web", renderer: resolved.renderer, gameId: manifest.gameId,
      files: files.length, videos: videoSources.size,
      bytes: files.reduce((sum, file) => sum + file.size, 0)};
  });
}
