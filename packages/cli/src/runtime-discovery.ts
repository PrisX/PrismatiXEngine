import {access, readFile, stat} from "node:fs/promises";
import {constants} from "node:fs";
import {delimiter, dirname, extname, isAbsolute, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

export type RuntimeTool = "previewHost" | "packager" | "player";

export interface RuntimeDiscoveryOptions {
  readonly runtime?: string;
  readonly previewHost?: string;
  readonly packager?: string;
  readonly player?: string;
  readonly environment?: NodeJS.ProcessEnv;
  readonly path?: string;
}

export interface DiscoveredRuntimeTool {
  readonly path: string;
  readonly source: string;
}

export class RuntimeDiscoveryFailure extends Error {
  constructor(readonly tool: RuntimeTool, readonly attempts: readonly string[]) {
    super(`PrismatiX ${toolLabel(tool)} was not found`);
    this.name = "RuntimeDiscoveryFailure";
  }
}

interface RuntimeManifest {
  readonly format: "PrismatiXRuntimeManifest";
  readonly schemaRevision: 1;
  readonly version: string;
  readonly binDir: string;
  readonly tools: Readonly<Partial<Record<RuntimeTool, string>>>;
}

const packageRoot = fileURLToPath(new URL("../../", import.meta.url));
const toolNames: Readonly<Record<RuntimeTool, string>> = {
  previewHost: process.platform === "win32" ? "PrismatiXPreviewHost.exe" : "PrismatiXPreviewHost",
  packager: process.platform === "win32" ? "PrismatiXPackager.exe" : "PrismatiXPackager",
  player: process.platform === "win32" ? "PrismatiXPlayer.exe" : "PrismatiXPlayer",
};
const toolOptions: Readonly<Record<RuntimeTool, keyof RuntimeDiscoveryOptions>> = {
  previewHost: "previewHost", packager: "packager", player: "player",
};
const toolEnvironment: Readonly<Record<RuntimeTool, string>> = {
  previewHost: "PRISMATIX_PREVIEW_HOST",
  packager: "PRISMATIX_PACKAGER",
  player: "PRISMATIX_PLAYER",
};

function toolLabel(tool: RuntimeTool): string {
  return tool === "previewHost" ? "PreviewHost" : tool === "packager" ? "Packager" : "Player";
}

async function existingFile(path: string): Promise<string | undefined> {
  try {
    const value = resolve(path);
    const info = await stat(value);
    if (!info.isFile()) return undefined;
    if (process.platform !== "win32" && ![".js", ".mjs", ".cjs"].includes(extname(value).toLowerCase()))
      await access(value, constants.X_OK);
    return value;
  } catch {
    return undefined;
  }
}

function manifestShape(value: unknown): RuntimeManifest | undefined {
  if (value === null || typeof value !== "object" || Array.isArray(value)) return undefined;
  const source = value as Record<string, unknown>;
  const tools = source.tools;
  if (source.format !== "PrismatiXRuntimeManifest" || source.schemaRevision !== 1 ||
      typeof source.version !== "string" || typeof source.binDir !== "string" ||
      tools === null || typeof tools !== "object" || Array.isArray(tools)) return undefined;
  return source as unknown as RuntimeManifest;
}

async function loadManifest(path: string): Promise<RuntimeManifest | undefined> {
  try { return manifestShape(JSON.parse(await readFile(path, "utf8")) as unknown); }
  catch { return undefined; }
}

async function pathIsDirectory(path: string): Promise<boolean> {
  try { return (await stat(path)).isDirectory(); } catch { return false; }
}

async function runtimeHintCandidates(hint: string, tool: RuntimeTool,
  attempts: string[]): Promise<string[]> {
  const absolute = resolve(hint);
  const candidates: string[] = [];
  let manifestPaths: string[];
  if ((await existingFile(absolute)) !== undefined) {
    manifestPaths = [absolute];
  } else {
    manifestPaths = [
      join(absolute, "runtime-manifest.json"),
      join(absolute, "share", "PrismatiX", "runtime-manifest.json"),
      join(absolute, "..", "share", "PrismatiX", "runtime-manifest.json"),
    ];
  }
  for (const manifestPath of manifestPaths) {
    attempts.push(`manifest:${resolve(manifestPath)}`);
    const manifest = await loadManifest(manifestPath);
    const filename = manifest?.tools[tool];
    if (manifest !== undefined && typeof filename === "string" && filename.length > 0) {
      candidates.push(resolve(dirname(manifestPath), manifest.binDir, filename));
    }
  }
  if (await pathIsDirectory(absolute)) {
    candidates.push(join(absolute, toolNames[tool]), join(absolute, "bin", toolNames[tool]));
  }
  return candidates;
}

async function firstExisting(candidates: readonly string[], source: string,
  attempts: string[]): Promise<DiscoveredRuntimeTool | undefined> {
  for (const candidate of candidates) {
    attempts.push(`${source}:${resolve(candidate)}`);
    const path = await existingFile(candidate);
    if (path !== undefined) return {path, source};
  }
  return undefined;
}

function repoFallbackRoots(): string[] {
  const root = resolve(packageRoot, "..", "..");
  return [
    join(root, "out", "install", "x64-release"),
    join(root, "out", "install", "x64-relwithdebinfo"),
    join(root, "out", "install", "x64-debug"),
    join(root, "out", "check", "sdk-package"),
    join(root, "out", "build", "x64-release", "PrismatiXEngine"),
    join(root, "out", "build", "x64-relwithdebinfo", "PrismatiXEngine"),
    join(root, "out", "build", "x64-debug", "PrismatiXEngine"),
    join(root, "build", "PrismatiXEngine"),
  ];
}

export async function discoverRuntimeTool(tool: RuntimeTool,
  options: RuntimeDiscoveryOptions = {}): Promise<DiscoveredRuntimeTool> {
  const environment = options.environment ?? process.env;
  const attempts: string[] = [];
  const explicit = options[toolOptions[tool]] as string | undefined;
  if (explicit !== undefined) {
    const found = await firstExisting([explicit], `--${tool === "previewHost" ? "preview-host" : tool}`, attempts);
    if (found !== undefined) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  if (options.runtime !== undefined) {
    const candidates = await runtimeHintCandidates(options.runtime, tool, attempts);
    const found = await firstExisting(candidates, "--runtime", attempts);
    if (found !== undefined) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  const environmentPath = environment[toolEnvironment[tool]];
  if (environmentPath !== undefined && environmentPath.length > 0) {
    const found = await firstExisting([environmentPath], toolEnvironment[tool], attempts);
    if (found !== undefined) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  const runtimeDirectory = environment.PRISMATIX_RUNTIME_DIR;
  if (runtimeDirectory !== undefined && runtimeDirectory.length > 0) {
    const candidates = await runtimeHintCandidates(runtimeDirectory, tool, attempts);
    const found = await firstExisting(candidates, "PRISMATIX_RUNTIME_DIR", attempts);
    if (found !== undefined) return found;
    throw new RuntimeDiscoveryFailure(tool, attempts);
  }
  const bundledRoot = join(packageRoot, "runtime", `${process.platform}-${process.arch}`);
  const bundled = await firstExisting(await runtimeHintCandidates(bundledRoot, tool, attempts),
    "bundled runtime", attempts);
  if (bundled !== undefined) return bundled;

  const searchPath = options.path ?? environment.PATH ?? "";
  const pathCandidates = searchPath.split(delimiter).filter((value) => value.length > 0)
    .map((value) => join(value, toolNames[tool]));
  const fromPath = await firstExisting(pathCandidates, "PATH", attempts);
  if (fromPath !== undefined) return fromPath;

  for (const root of repoFallbackRoots()) {
    const fromRepo = await firstExisting(await runtimeHintCandidates(root, tool, attempts),
      "repository fallback", attempts);
    if (fromRepo !== undefined) return fromRepo;
  }
  throw new RuntimeDiscoveryFailure(tool, attempts);
}

export function runtimeDiscoveryHint(error: RuntimeDiscoveryFailure): string {
  const option = error.tool === "previewHost" ? "--preview-host" : `--${error.tool}`;
  return `Set ${option}, --runtime, ${toolEnvironment[error.tool]}, or PRISMATIX_RUNTIME_DIR. ` +
    `Checked: ${error.attempts.join(", ")}`;
}
