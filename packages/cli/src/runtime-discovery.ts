import {access, readFile, realpath, stat} from "node:fs/promises";
import {constants} from "node:fs";
import {basename, delimiter, dirname, extname, isAbsolute, join, relative, resolve} from "node:path";
import {fileURLToPath} from "node:url";

export type RuntimeTool = "previewHost" | "packager" | "player";
export interface RuntimeDiscoveryOptions {
  readonly runtime?: string;
  readonly previewHost?: string;
  readonly packager?: string;
  readonly player?: string;
  readonly environment?: NodeJS.ProcessEnv;
  readonly path?: string;
  /** Developer-only fallback. Normal author discovery never searches PATH or the checkout. */
  readonly allowSystemRuntime?: boolean;
  /**
   * A CLI running from an engine source checkout (linked with `file:`) uses
   * that checkout's own native build when no runtime is bundled. Defaults to
   * on with the real process environment; PRISMATIX_NO_CHECKOUT_RUNTIME=1
   * turns it off.
   */
  readonly checkoutFallback?: boolean;
}
export interface DiscoveredRuntimeTool {
  readonly path: string;
  readonly source: string;
  readonly bundleId?: string;
  readonly version?: string;
}
export class RuntimeDiscoveryFailure extends Error {
  constructor(readonly tool: RuntimeTool, readonly attempts: readonly string[]) {
    super(`PrismatiX ${toolLabel(tool)} was not found or is incompatible`);
    this.name = "RuntimeDiscoveryFailure";
  }
}
interface RuntimeManifest {
  format: "PrismatiXRuntimeManifest";
  schemaRevision: 1;
  version: string;
  target: string;
  binDir: string;
  tools: Partial<Record<RuntimeTool, string>>;
}
const packageRoot = fileURLToPath(new URL("../../", import.meta.url));
const toolNames: Readonly<Record<RuntimeTool, string>> = {
  previewHost: process.platform === "win32" ? "PrismatiXPreviewHost.exe" : "PrismatiXPreviewHost",
  packager: process.platform === "win32" ? "PrismatiXPackager.exe" : "PrismatiXPackager",
  player: process.platform === "win32" ? "PrismatiXPlayer.exe" : "PrismatiXPlayer",
};
const toolEnvironment: Readonly<Record<RuntimeTool, string>> = {
  previewHost: "PRISMATIX_PREVIEW_HOST", packager: "PRISMATIX_PACKAGER", player: "PRISMATIX_PLAYER",
};
function toolLabel(tool: RuntimeTool): string {
  return tool === "previewHost" ? "PreviewHost" : tool === "packager" ? "Packager" : "Player";
}
async function existingFile(path: string): Promise<string | undefined> {
  try {
    const value = resolve(path);
    if (!(await stat(value)).isFile()) return undefined;
    if (process.platform !== "win32" && ![".js", ".mjs", ".cjs"].includes(extname(value).toLowerCase()))
      await access(value, constants.X_OK);
    return value;
  } catch { return undefined; }
}
function targetMatches(target: unknown): boolean {
  const targets: Record<string, string> = {
    "win32-x64": "x86_64-pc-windows-msvc", "linux-x64": "x86_64-unknown-linux-gnu",
    "linux-arm64": "aarch64-unknown-linux-gnu", "darwin-x64": "x86_64-apple-darwin",
    "darwin-arm64": "aarch64-apple-darwin",
  };
  const host = `${process.platform}-${process.arch}`;
  return target === host || (targets[host] !== undefined && target === targets[host]);
}
function validateManifest(value: unknown, developer: boolean): RuntimeManifest {
  if (value === null || typeof value !== "object" || Array.isArray(value)) throw new Error("invalid manifest object");
  const source = value as Record<string, unknown>;
  if (source.format !== "PrismatiXRuntimeManifest" || source.schemaRevision !== 1) throw new Error("expected runtime manifest schema revision 1");
  if (typeof source.version !== "string" || !/^0\.2\.(0|[1-9]\d*)(?:\+[0-9A-Za-z.-]+)?$/.test(source.version)) {
    if (!developer || typeof source.version !== "string" || !/^0\.2\.(0|[1-9]\d*)-[0-9A-Za-z.-]+(?:\+[0-9A-Za-z.-]+)?$/.test(source.version))
      throw new Error(`runtime version ${String(source.version)} is incompatible; expected 0.2.x`);
  }
  if (!targetMatches(source.target)) throw new Error(`runtime target ${String(source.target)} does not match ${process.platform}-${process.arch}`);
  if (typeof source.binDir !== "string" || !source.binDir || isAbsolute(source.binDir) || /^[A-Za-z]:/.test(source.binDir)) throw new Error("invalid relative runtime binDir");
  if (!source.tools || typeof source.tools !== "object" || Array.isArray(source.tools)) throw new Error("invalid runtime tools");
  for (const filename of Object.values(source.tools)) {
    if (typeof filename !== "string" || !filename || filename === "." || filename === ".." || /[\\/:]/.test(filename)) throw new Error("runtime tool must be a filename inside binDir");
  }
  return source as unknown as RuntimeManifest;
}
function within(root: string, path: string): boolean {
  const rel = relative(root, path);
  return rel === "" || (!isAbsolute(rel) && rel !== ".." && !rel.startsWith("../") && !rel.startsWith("..\\"));
}
async function firstExisting(candidates: readonly string[], source: string, attempts: string[]): Promise<DiscoveredRuntimeTool | undefined> {
  for (const candidate of candidates) {
    attempts.push(`${source}:${resolve(candidate)}`);
    const path = await existingFile(candidate);
    if (path !== undefined) return {path, source: `${source} (unverified developer override)`};
  }
  return undefined;
}
async function fromHint(hint: string, tool: RuntimeTool, source: string, developer: boolean, attempts: string[]): Promise<DiscoveredRuntimeTool | undefined> {
  const absolute = resolve(hint);
  let directory = false;
  try { directory = (await stat(absolute)).isDirectory(); } catch { /* Missing hints are reported in attempts. */ }
  const manifestPaths = directory ? [join(absolute, "runtime-manifest.json"),
    join(absolute, "share", "PrismatiX", "runtime-manifest.json"),
    join(absolute, "..", "share", "PrismatiX", "runtime-manifest.json")] : [absolute];
  for (const manifestPath of manifestPaths) {
    attempts.push(`manifest:${resolve(manifestPath)}`);
    let contents: string;
    try { contents = await readFile(manifestPath, "utf8"); }
    catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") continue;
      attempts.push(`Cannot read runtime manifest: ${String(error)}`);
      throw new RuntimeDiscoveryFailure(tool, attempts);
    }
    try {
      const manifest = validateManifest(JSON.parse(contents) as unknown, developer);
      const filename = manifest.tools[tool];
      if (!filename) throw new Error(`manifest does not declare ${tool}`);
      const manifestDirectory = dirname(manifestPath);
      const prefix = basename(manifestDirectory).toLowerCase() === "prismatix" && basename(dirname(manifestDirectory)).toLowerCase() === "share"
        ? resolve(manifestDirectory, "..", "..") : manifestDirectory;
      const executable = resolve(manifestDirectory, manifest.binDir, filename);
      if (!within(await realpath(prefix), await realpath(executable))) throw new Error("runtime executable escapes bundle prefix");
      const path = await existingFile(executable);
      if (!path) throw new Error(`runtime executable is missing or not executable: ${executable}`);
      return {path, source: `${source}${developer ? " (developer override; manifest checked)" : " (manifest checked)"}`,
        bundleId: await realpath(manifestPath), version: manifest.version};
    } catch (error) {
      attempts.push(`Rejected ${manifestPath}: ${error instanceof Error ? error.message : String(error)}`);
      throw new RuntimeDiscoveryFailure(tool, attempts);
    }
  }
  if (developer && directory) return firstExisting([join(absolute, toolNames[tool]), join(absolute, "bin", toolNames[tool])], source, attempts);
  return undefined;
}
function repoFallbackRoots(): string[] {
  const root = resolve(packageRoot, "..", "..");
  return ["out/install/x64-release", "out/install/x64-relwithdebinfo", "out/install/x64-debug", "out/check/sdk-package",
    "out/build/x64-release/PrismatiXEngine", "out/build/x64-relwithdebinfo/PrismatiXEngine", "out/build/x64-debug/PrismatiXEngine", "build/PrismatiXEngine"]
    .map(path => resolve(root, path));
}
async function isSourceCheckout(): Promise<boolean> {
  const exists = (path: string) => stat(path).then(() => true, () => false);
  return await exists(join(packageRoot, "src")) &&
    await exists(resolve(packageRoot, "..", "..", "CMakePresets.json"));
}
/** The engine checkout's native build directory holding `file`, for a source-checkout CLI. */
export async function checkoutRuntimeFile(file: string,
  environment: NodeJS.ProcessEnv = process.env): Promise<string | undefined> {
  if (environment.PRISMATIX_NO_CHECKOUT_RUNTIME === "1" || !(await isSourceCheckout())) return undefined;
  for (const root of repoFallbackRoots())
    for (const candidate of [join(root, file), join(root, "share", "PrismatiX", file)])
      if (await existingFile(candidate)) return candidate;
  return undefined;
}
export async function discoverRuntimeTool(tool: RuntimeTool, options: RuntimeDiscoveryOptions = {}): Promise<DiscoveredRuntimeTool> {
  const environment = options.environment ?? process.env;
  const attempts: string[] = [];
  const explicit = options[tool];
  if (explicit !== undefined) {
    const found = await firstExisting([explicit], `--${tool === "previewHost" ? "preview-host" : tool}`, attempts);
    if (found) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  if (options.runtime !== undefined) {
    const found = await fromHint(options.runtime, tool, "--runtime", true, attempts);
    if (found) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  const environmentPath = environment[toolEnvironment[tool]];
  if (environmentPath) {
    const found = await firstExisting([environmentPath], toolEnvironment[tool], attempts);
    if (found) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  if (environment.PRISMATIX_RUNTIME_DIR) {
    const found = await fromHint(environment.PRISMATIX_RUNTIME_DIR, tool, "PRISMATIX_RUNTIME_DIR", true, attempts);
    if (found) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  const bundledRoot = join(packageRoot, "runtime", `${process.platform}-${process.arch}`);
  const bundled = await fromHint(bundledRoot, tool, "bundled runtime", false, attempts);
  if (bundled) return bundled;
  if (options.allowSystemRuntime === true || environment.PRISMATIX_ALLOW_SYSTEM_RUNTIME === "1") {
    const pathCandidates = (options.path ?? environment.PATH ?? "").split(delimiter).filter(Boolean).map(value => join(value, toolNames[tool]));
    const fromPath = await firstExisting(pathCandidates, "PATH", attempts);
    if (fromPath) return fromPath;
    for (const root of repoFallbackRoots()) {
      const fromRepo = await fromHint(root, tool, "repository fallback", true, attempts);
      if (fromRepo) return fromRepo;
    }
  } else {
    const checkout = (options.checkoutFallback ?? options.environment === undefined) &&
      environment.PRISMATIX_NO_CHECKOUT_RUNTIME !== "1" && await isSourceCheckout();
    if (checkout)
      for (const root of repoFallbackRoots()) {
        const fromRepo = await fromHint(root, tool, "engine checkout", true, attempts);
        if (fromRepo) return fromRepo;
      }
    attempts.push("PATH/repository fallback disabled; developer opt-in: PRISMATIX_ALLOW_SYSTEM_RUNTIME=1");
  }
  throw new RuntimeDiscoveryFailure(tool, attempts);
}

/** Discover a coherent bundle. Per-tool explicit overrides remain possible and clearly labelled. */
export async function discoverRuntimeBundle<const T extends readonly RuntimeTool[]>(tools: T,
  options: RuntimeDiscoveryOptions = {}): Promise<{[K in T[number]]: DiscoveredRuntimeTool}> {
  const entries = await Promise.all(tools.map(async tool => [tool, await discoverRuntimeTool(tool, options)] as const));
  const bundles = new Set(entries.map(([, found]) => found.bundleId).filter(value => value !== undefined));
  if (bundles.size > 1) throw new RuntimeDiscoveryFailure(tools[0] ?? "player", ["Tools resolved to different runtime manifests; select one coherent --runtime bundle"]);
  const unverified = entries.filter(([, found]) => found.bundleId === undefined);
  // Raw directory overrides and PATH fallback must also use one directory. Explicit per-tool paths are intentional exceptions.
  const directories = new Set(unverified.filter(([tool]) => options[tool] === undefined && !(options.environment ?? process.env)[toolEnvironment[tool]])
    .map(([, found]) => dirname(found.path)));
  if (directories.size > 1 || (bundles.size > 0 && directories.size > 0)) throw new RuntimeDiscoveryFailure(tools[0] ?? "player", ["Tools resolved to different runtime locations; select one coherent --runtime bundle"]);
  return Object.fromEntries(entries) as {[K in T[number]]: DiscoveredRuntimeTool};
}
const qaPlayerName = process.platform === "win32" ? "PrismatiXPlayerQA.exe" : "PrismatiXPlayerQA";

/**
 * The QA Player (PrismatiXPlayerQA: the Player with its QA IPC test interface)
 * for local development packs. It is never part of a shipped game, so a
 * runtime bundle need not declare it: an explicit `playerQA` path or
 * PRISMATIX_PLAYER_QA wins, else it is the PrismatiXPlayerQA beside the
 * discovered Player.
 */
export async function discoverQaPlayer(options: RuntimeDiscoveryOptions & {readonly playerQA?: string} = {}):
  Promise<DiscoveredRuntimeTool> {
  const environment = options.environment ?? process.env;
  const attempts: string[] = [];
  const explicit = options.playerQA ?? environment.PRISMATIX_PLAYER_QA;
  if (explicit) {
    const found = await firstExisting([explicit], options.playerQA !== undefined ? "--player-qa" : "PRISMATIX_PLAYER_QA", attempts);
    if (found) return found;
    throw new RuntimeDiscoveryFailure("player", [...attempts, "The QA Player named explicitly does not exist"]);
  }
  const player = await discoverRuntimeTool("player", options);
  const sibling = join(dirname(player.path), qaPlayerName);
  attempts.push(`beside the Player:${sibling}`);
  const path = await existingFile(sibling);
  if (path !== undefined) return {path, source: `${player.source}; QA Player beside it`,
    ...(player.bundleId === undefined ? {} : {bundleId: player.bundleId}),
    ...(player.version === undefined ? {} : {version: player.version})};
  throw new RuntimeDiscoveryFailure("player", [...attempts,
    `this runtime has no ${qaPlayerName}; build the PrismatiXPlayerQA target or pass --player-qa <path>`]);
}

export function runtimeDiscoveryHint(error: RuntimeDiscoveryFailure): string {
  const option = error.tool === "previewHost" ? "--preview-host" : `--${error.tool}`;
  return `Install a CLI package containing a compatible runtime, or explicitly select a developer runtime using ${option}, --runtime, ${toolEnvironment[error.tool]}, or PRISMATIX_RUNTIME_DIR. Checked: ${error.attempts.join(", ")}`;
}
