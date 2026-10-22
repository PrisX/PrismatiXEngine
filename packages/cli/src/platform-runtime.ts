// The native runtimes of other desktop platforms, for packing a game for a
// platform other than the one the CLI runs on (`pack --platform`).
//
// Each platform's runtime is the npm package @prismatix/native-<platform>.
// Package managers install only the host's (its os/cpu fields), so
// `prismatix add platform <platform>` downloads another platform's package
// tarball at exactly this CLI's version from the npm registry, checks it
// against the registry's integrity (SHA-512) and extracts it into the
// project's .prismatix/platforms/<platform>/. Nothing in it is executed:
// the host's own Packager copies that platform's Player into the game.

import {createHash} from "node:crypto";
import {createReadStream, createWriteStream} from "node:fs";
import {copyFile, cp, lstat, mkdir, mkdtemp, readFile, readdir, rename, rm, stat, writeFile} from "node:fs/promises";
import {createRequire} from "node:module";
import {dirname, join, relative, resolve, sep} from "node:path";
import {Readable} from "node:stream";
import {pipeline} from "node:stream/promises";

import {extractTarGz, type ArchiveEntry} from "./archive.js";
import {checkRuntimeManifest, checkoutRoot, cliRuntimeVersion, nativePlatforms} from "./runtime-discovery.js";

export type DesktopPlatform = typeof nativePlatforms[number];
export const desktopPlatforms: readonly DesktopPlatform[] = nativePlatforms;
export const hostPlatform = `${process.platform}-${process.arch}`;

export const platformLabels: Readonly<Record<DesktopPlatform, string>> = {
  "win32-x64": "Windows x64", "linux-x64": "Linux x64", "darwin-x64": "macOS Intel", "darwin-arm64": "macOS Apple silicon",
};

export function isDesktopPlatform(value: string): value is DesktopPlatform {
  return (desktopPlatforms as readonly string[]).includes(value);
}

/** The operating system a platform runs ("win32", "linux" or "darwin"). */
export function platformOs(platform: DesktopPlatform): "win32" | "linux" | "darwin" {
  return platform.slice(0, platform.indexOf("-")) as "win32" | "linux" | "darwin";
}

/**
 * The platforms named by `--platform` values: each a platform, a comma
 * list, `all` or `host`, in the order given without repeats.
 */
export function parsePlatformList(values: readonly string[]): DesktopPlatform[] {
  const result: DesktopPlatform[] = [];
  for (const value of values.flatMap(item => item.split(","))) {
    const name = value.trim();
    if (name === "") continue;
    const expanded = name === "all" ? [...desktopPlatforms] : name === "host" ? [hostPlatform] : [name];
    for (const platform of expanded) {
      if (!isDesktopPlatform(platform))
        throw new Error(name === "host"
          ? `This computer (${hostPlatform}) is not a platform PrismatiX packs for`
          : `Unknown platform ${JSON.stringify(name)}; use ${desktopPlatforms.join(", ")} or all`);
      if (!result.includes(platform)) result.push(platform);
    }
  }
  if (result.length === 0) throw new Error(`--platform needs a platform: ${desktopPlatforms.join(", ")} or all`);
  return result;
}

export const nativePackageNameOf = (platform: DesktopPlatform) => `@prismatix/native-${platform}`;
export const addPlatformCommand = (platform: DesktopPlatform) => `prismatix add platform ${platform}`;

const stampFile = "platform.json";
const runtimeDirectory = "runtime";
const platformsDirectory = join(".prismatix", "platforms");

/** Where `add platform` keeps a platform's runtime in a project. */
export function platformCacheDirectory(projectRoot: string, platform: DesktopPlatform): string {
  return join(projectRoot, platformsDirectory, platform);
}

export class PlatformRuntimeMissing extends Error {
  constructor(readonly platform: DesktopPlatform, readonly attempts: readonly string[]) {
    super(`The ${platformLabels[platform]} runtime (${nativePackageNameOf(platform)}@${cliRuntimeVersion}) is not ` +
      `installed for this project; add it with \`${addPlatformCommand(platform)}\``);
    this.name = "PlatformRuntimeMissing";
  }
}

export interface PlatformRuntime {
  readonly platform: DesktopPlatform;
  /** The runtime bundle (bin/ and share/PrismatiX/). */
  readonly root: string;
  readonly player: string;
  readonly version: string;
  readonly source: string;
}

interface Stamp {
  readonly format: "PrismatiXPlatformRuntime";
  readonly schemaRevision: 1;
  readonly platform: DesktopPlatform;
  readonly package: string;
  readonly version: string;
  readonly source: string;
  readonly integrity?: string;
  readonly files: Readonly<Record<string, {readonly size: number; readonly sha256: string; readonly mode: number}>>;
}

async function sha256(path: string): Promise<string> {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path) as AsyncIterable<Buffer>) hash.update(chunk);
  return hash.digest("hex");
}

async function readJsonFile(path: string, limit = 4 * 1024 * 1024): Promise<unknown> {
  const info = await lstat(path);
  if (!info.isFile() || info.size > limit) throw new Error(`${path} is not a bounded regular file`);
  return JSON.parse(await readFile(path, "utf8")) as unknown;
}

/** A runtime bundle's Player after checking its manifest belongs to `platform` and this CLI. */
async function bundlePlayer(root: string, platform: DesktopPlatform, developer: boolean):
  Promise<{player: string; version: string}> {
  const manifestPath = join(root, "share", "PrismatiX", "runtime-manifest.json");
  const manifest = checkRuntimeManifest(await readJsonFile(manifestPath), platform, developer);
  const name = manifest.tools.player;
  if (name === undefined) throw new Error("the runtime manifest names no Player");
  const player = resolve(dirname(manifestPath), manifest.binDir, name);
  const rel = relative(resolve(root), player);
  if (rel.startsWith("..") || resolve(root, rel) !== player) throw new Error("the Player lies outside the runtime");
  const info = await stat(player).catch(() => undefined);
  if (!info?.isFile()) throw new Error(`the runtime has no Player: ${player}`);
  return {player, version: manifest.version};
}

async function verifyStamp(directory: string, platform: DesktopPlatform, full: boolean): Promise<Stamp> {
  const stamp = await readJsonFile(join(directory, stampFile)) as Stamp;
  if (stamp?.format !== "PrismatiXPlatformRuntime" || stamp.schemaRevision !== 1 || stamp.platform !== platform ||
      typeof stamp.files !== "object" || stamp.files === null)
    throw new Error(`${join(directory, stampFile)} is not a platform runtime record`);
  if (stamp.version !== cliRuntimeVersion)
    throw new Error(`the installed runtime is ${stamp.version}, not ${cliRuntimeVersion}`);
  const root = join(directory, runtimeDirectory);
  for (const [path, expected] of Object.entries(stamp.files)) {
    const file = join(root, ...path.split("/"));
    const info = await lstat(file).catch(() => undefined);
    if (!info?.isFile() || info.size !== expected.size) throw new Error(`the installed runtime is missing or changed: ${path}`);
    if (full && await sha256(file) !== expected.sha256) throw new Error(`the installed runtime has changed: ${path}`);
  }
  return stamp;
}

export interface LocateOptions {
  readonly environment?: NodeJS.ProcessEnv;
  /** Hash every installed file (packing does; a status listing only checks sizes). */
  readonly verify?: boolean;
}

/**
 * Finds `platform`'s runtime for a project: a developer directory
 * (PRISMATIX_PLATFORM_RUNTIMES/<platform>), the project's `add platform`
 * install, an npm-installed @prismatix/native-<platform> of this version,
 * or an engine checkout's out/platforms/<platform>.
 */
export async function locatePlatformRuntime(projectRoot: string, platform: DesktopPlatform,
  options: LocateOptions = {}): Promise<PlatformRuntime> {
  const environment = options.environment ?? process.env;
  const attempts: string[] = [];
  const tryBundle = async (root: string, source: string, developer: boolean) => {
    try {
      const found = await bundlePlayer(root, platform, developer);
      return {platform, root, player: found.player, version: found.version, source};
    } catch (error) {
      attempts.push(`${source} (${root}): ${error instanceof Error ? error.message : String(error)}`);
      return undefined;
    }
  };
  const developerRoot = environment.PRISMATIX_PLATFORM_RUNTIMES;
  if (developerRoot) {
    const found = await tryBundle(resolve(developerRoot, platform), "PRISMATIX_PLATFORM_RUNTIMES (developer override)", true);
    if (found) return found;
    throw new PlatformRuntimeMissing(platform, attempts);
  }
  const cache = platformCacheDirectory(projectRoot, platform);
  try {
    const stamp = await verifyStamp(cache, platform, options.verify !== false);
    const found = await tryBundle(join(cache, runtimeDirectory), `prismatix add platform (${stamp.source})`, false);
    if (found) return found;
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") attempts.push(`${cache}: not installed`);
    else attempts.push(`${cache}: ${error instanceof Error ? error.message : String(error)}`);
  }
  for (const base of [join(projectRoot, "package.json"), import.meta.url]) {
    try {
      const manifest = createRequire(base).resolve(`${nativePackageNameOf(platform)}/package.json`);
      const found = await tryBundle(dirname(manifest), `installed ${nativePackageNameOf(platform)}`, false);
      if (found) return found;
    } catch { /* Not installed there. */ }
  }
  const checkout = await checkoutRoot(environment);
  if (checkout !== undefined) {
    const found = await tryBundle(join(checkout, "out", "platforms", platform), "engine checkout (developer)", true);
    if (found) return found;
  }
  throw new PlatformRuntimeMissing(platform, attempts);
}

export interface RegistryOptions {
  readonly environment?: NodeJS.ProcessEnv;
  readonly fetch?: typeof fetch;
  readonly onProgress?: (message: string) => void;
}

/** The npm registry `add platform` downloads from: PRISMATIX_NPM_REGISTRY, npm's configured registry, or npmjs. */
export function registryUrl(environment: NodeJS.ProcessEnv = process.env): string {
  const configured = environment.PRISMATIX_NPM_REGISTRY ?? environment.npm_config_registry ??
    environment.NPM_CONFIG_REGISTRY ?? "https://registry.npmjs.org/";
  const url = new URL(configured);
  if (url.protocol !== "https:" && url.protocol !== "http:") throw new Error(`Unsupported npm registry ${configured}`);
  return url.href.endsWith("/") ? url.href : `${url.href}/`;
}

/** Checks `bytes` against an npm `integrity` (Subresource Integrity; SHA-512 preferred) or legacy `shasum`. */
export function integrityMatches(digests: {sha512: string; sha1: string}, integrity?: string, shasum?: string): boolean {
  const entries = (integrity ?? "").split(/\s+/u).filter(Boolean);
  const sha512 = entries.filter(entry => entry.startsWith("sha512-")).map(entry => entry.slice(7).split("?")[0]);
  if (sha512.length > 0) return sha512.includes(digests.sha512);
  if (entries.length === 0 && typeof shasum === "string" && /^[0-9a-f]{40}$/u.test(shasum)) return shasum === digests.sha1;
  return false;
}

async function downloadTarball(platform: DesktopPlatform, target: string, options: RegistryOptions):
  Promise<{source: string; integrity: string}> {
  const fetchImpl = options.fetch ?? fetch;
  const registry = registryUrl(options.environment);
  const name = nativePackageNameOf(platform);
  const metadataUrl = new URL(`${name.replace("/", "%2f")}/${cliRuntimeVersion}`, registry);
  options.onProgress?.(`Looking up ${name}@${cliRuntimeVersion} on ${registry}`);
  const response = await fetchImpl(metadataUrl, {headers: {accept: "application/json"}, redirect: "follow"});
  if (response.status === 404)
    throw new Error(`${name}@${cliRuntimeVersion} is not on ${registry}; it is published with each release of @prismatix/cli`);
  if (!response.ok) throw new Error(`${metadataUrl.href} answered ${response.status} ${response.statusText}`);
  const metadata = await response.json() as {name?: unknown; version?: unknown; dist?: {tarball?: unknown; integrity?: unknown; shasum?: unknown}};
  if (metadata.name !== name || metadata.version !== cliRuntimeVersion || typeof metadata.dist?.tarball !== "string")
    throw new Error(`${metadataUrl.href} does not describe ${name}@${cliRuntimeVersion}`);
  const integrity = typeof metadata.dist.integrity === "string" ? metadata.dist.integrity : undefined;
  const shasum = typeof metadata.dist.shasum === "string" ? metadata.dist.shasum : undefined;
  if (integrity === undefined && shasum === undefined) throw new Error(`${name}@${cliRuntimeVersion} lists no integrity on ${registry}`);
  const tarballUrl = new URL(metadata.dist.tarball);
  if (tarballUrl.protocol !== "https:" && tarballUrl.protocol !== new URL(registry).protocol)
    throw new Error(`Refusing to download ${tarballUrl.href} over ${tarballUrl.protocol}`);
  options.onProgress?.(`Downloading ${tarballUrl.href}`);
  const download = await fetchImpl(tarballUrl, {redirect: "follow"});
  if (!download.ok || download.body === null) throw new Error(`${tarballUrl.href} answered ${download.status} ${download.statusText}`);
  const sha512 = createHash("sha512");
  const sha1 = createHash("sha1");
  const limit = 1024 ** 3;
  let received = 0;
  const body = Readable.fromWeb(download.body as import("node:stream/web").ReadableStream<Uint8Array>);
  body.on("data", (chunk: Buffer) => {
    received += chunk.length;
    if (received > limit) body.destroy(new Error(`${tarballUrl.href} is larger than 1 GiB`));
    sha512.update(chunk); sha1.update(chunk);
  });
  await pipeline(body, createWriteStream(target, {flags: "wx"}));
  if (!integrityMatches({sha512: sha512.digest("base64"), sha1: sha1.digest("hex")}, integrity, shasum))
    throw new Error(`${tarballUrl.href} does not match the integrity ${integrity ?? `sha1 ${shasum}`} the registry lists; nothing was installed`);
  return {source: tarballUrl.href, integrity: integrity ?? `sha1-${shasum}`};
}

/** Copies a symbolic or hard link's target over the link's path, so the runtime holds only regular files. */
async function materializeLinks(root: string, entries: readonly ArchiveEntry[]): Promise<void> {
  const files = new Set(entries.filter(entry => entry.type === "file").map(entry => entry.path));
  let pending = entries.filter(entry => entry.type === "symlink" || entry.type === "hardlink");
  // Links may point at links; resolve in rounds.
  for (let round = 0; round < 8 && pending.length > 0; round++) {
    const next: ArchiveEntry[] = [];
    for (const link of pending) {
      if (link.type === "symlink" && (link.target!.startsWith("/") || /^[A-Za-z]:/u.test(link.target!)))
        throw new Error(`Archive link ${link.path} is absolute`);
      const target = link.type === "hardlink" ? link.target! :
        join(dirname(link.path), link.target!).split(sep).join("/");
      const normalized = target.split("/").reduce<string[]>((parts, part) => {
        if (part === "..") { if (parts.length === 0) throw new Error(`Archive link ${link.path} points outside the runtime`); parts.pop(); }
        else if (part !== "." && part !== "") parts.push(part);
        return parts;
      }, []).join("/");
      if (!files.has(normalized)) { next.push(link); continue; }
      const destination = join(root, ...link.path.split("/"));
      await mkdir(dirname(destination), {recursive: true});
      await copyFile(join(root, ...normalized.split("/")), destination);
      files.add(link.path);
    }
    pending = next;
  }
  if (pending.length > 0) throw new Error(`Archive links do not resolve to files: ${pending.map(link => link.path).join(", ")}`);
}

async function listFiles(root: string, prefix = ""): Promise<string[]> {
  const result: string[] = [];
  for (const entry of await readdir(join(root, ...prefix.split("/").filter(Boolean)), {withFileTypes: true})) {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) result.push(...await listFiles(root, path));
    else if (entry.isFile()) result.push(path);
    else throw new Error(`The runtime contains ${path}, which is not a file or directory`);
  }
  return result.sort();
}

export interface AddPlatformOptions extends RegistryOptions {
  /** A package tarball (`npm pack` output), a runtime .tar.gz or a runtime directory instead of the registry. */
  readonly from?: string;
}

export interface AddedPlatform {
  readonly platform: DesktopPlatform;
  readonly version: string;
  readonly directory: string;
  readonly player: string;
  readonly source: string;
  readonly integrity?: string;
}

/**
 * Installs `platform`'s runtime into the project (.prismatix/platforms/
 * <platform>/), replacing an older one only once the new one is checked.
 */
export async function addPlatformRuntime(projectRoot: string, platform: DesktopPlatform,
  options: AddPlatformOptions = {}): Promise<AddedPlatform> {
  const cache = platformCacheDirectory(projectRoot, platform);
  await mkdir(dirname(cache), {recursive: true});
  const work = await mkdtemp(join(dirname(cache), `.add-${platform}-`));
  try {
    const extracted = join(work, "extracted");
    await mkdir(extracted);
    let source: string;
    let integrity: string | undefined;
    const modes = new Map<string, number>();
    const fromDirectory = options.from !== undefined && (await stat(options.from)).isDirectory();
    if (fromDirectory) {
      source = `directory ${resolve(options.from!)}`;
      await cp(resolve(options.from!), extracted, {recursive: true, dereference: true,
        filter: (path) => !relative(resolve(options.from!), path).split(sep).includes("node_modules")});
    } else {
      let tarball: string;
      if (options.from !== undefined) {
        tarball = resolve(options.from);
        source = `file ${tarball}`;
        const hash = createHash("sha512");
        for await (const chunk of createReadStream(tarball) as AsyncIterable<Buffer>) hash.update(chunk);
        integrity = `sha512-${hash.digest("base64")}`;
      } else {
        tarball = join(work, "package.tgz");
        ({source, integrity} = await downloadTarball(platform, tarball, options));
      }
      options.onProgress?.("Extracting the runtime");
      const entries = await extractTarGz(tarball, extracted, {maxBytes: 2 * 1024 ** 3});
      await materializeLinks(extracted, entries);
      for (const entry of entries) if (entry.type === "file") modes.set(entry.path, entry.mode);
    }
    // An npm package keeps the runtime under package/; a runtime archive or
    // directory has bin/ and share/ at its root.
    const packageJson = await readJsonFile(join(extracted, "package", "package.json")).catch(() => undefined) as
      {name?: unknown; version?: unknown} | undefined;
    const plainJson = await readJsonFile(join(extracted, "package.json")).catch(() => undefined) as
      {name?: unknown; version?: unknown} | undefined;
    const runtimeRoot = packageJson !== undefined ? join(extracted, "package") : extracted;
    const described = packageJson ?? plainJson;
    if (described !== undefined && (described.name !== nativePackageNameOf(platform) || described.version !== cliRuntimeVersion))
      throw new Error(`${options.from ?? source} is ${String(described.name)}@${String(described.version)}, ` +
        `not ${nativePackageNameOf(platform)}@${cliRuntimeVersion}`);
    const {version} = await bundlePlayer(runtimeRoot, platform, false).catch((error: unknown) => {
      throw new Error(`${options.from ?? source} is not the ${platformLabels[platform]} runtime ${cliRuntimeVersion}: ` +
        (error instanceof Error ? error.message : String(error)));
    });
    const staged = join(work, "staged");
    await mkdir(staged);
    await rename(runtimeRoot, join(staged, runtimeDirectory));
    const files: Record<string, {size: number; sha256: string; mode: number}> = {};
    const root = join(staged, runtimeDirectory);
    const prefix = packageJson !== undefined ? "package/" : "";
    for (const path of await listFiles(root)) {
      const file = join(root, ...path.split("/"));
      const info = await lstat(file);
      files[path] = {size: info.size, sha256: await sha256(file),
        mode: modes.get(prefix + path) ?? (process.platform === "win32" ? 0o644 : info.mode & 0o777)};
    }
    const stamp: Stamp = {format: "PrismatiXPlatformRuntime", schemaRevision: 1, platform,
      package: nativePackageNameOf(platform), version, source, ...(integrity === undefined ? {} : {integrity}), files};
    await writeFile(join(staged, stampFile), `${JSON.stringify(stamp, null, 2)}\n`, {flag: "wx"});
    const retired = join(work, "retired");
    await rename(cache, retired).catch((error: unknown) => {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    });
    await rename(staged, cache);
    const located = await bundlePlayer(join(cache, runtimeDirectory), platform, false);
    return {platform, version, directory: cache, player: located.player, source, ...(integrity === undefined ? {} : {integrity})};
  } finally {
    await rm(work, {recursive: true, force: true});
  }
}

export interface PlatformStatus {
  readonly platform: DesktopPlatform;
  readonly label: string;
  readonly host: boolean;
  readonly installed: boolean;
  readonly source?: string;
  readonly directory?: string;
  readonly problem?: string;
  /** The command that installs it (the host's comes with @prismatix/cli). */
  readonly install: string;
}

/** Which platforms' runtimes this project can pack with, without hashing every file. */
export async function platformRuntimeStatus(projectRoot: string, hostRuntime: () => Promise<{source: string; path: string}>,
  environment: NodeJS.ProcessEnv = process.env): Promise<PlatformStatus[]> {
  return Promise.all(desktopPlatforms.map(async (platform): Promise<PlatformStatus> => {
    const base = {platform, label: platformLabels[platform], host: platform === hostPlatform};
    if (platform === hostPlatform) {
      try {
        const found = await hostRuntime();
        return {...base, installed: true, source: found.source, directory: dirname(found.path),
          install: `pnpm add -D @prismatix/cli@${cliRuntimeVersion}`};
      } catch (error) {
        return {...base, installed: false, problem: error instanceof Error ? error.message : String(error),
          install: `pnpm add -D @prismatix/cli@${cliRuntimeVersion}`};
      }
    }
    try {
      const found = await locatePlatformRuntime(projectRoot, platform, {environment, verify: false});
      return {...base, installed: true, source: found.source, directory: found.root, install: addPlatformCommand(platform)};
    } catch (error) {
      return {...base, installed: false, install: addPlatformCommand(platform),
        ...(error instanceof PlatformRuntimeMissing ? {} : {problem: error instanceof Error ? error.message : String(error)})};
    }
  }));
}
