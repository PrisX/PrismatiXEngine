// Installing shared extensions: `prismatix extension add | remove | update |
// outdated | verify | inspect`. A package (from a registry, a file, an https
// URL or an npm package) is checked whole before anything is written: its
// SHA-256 and size, the canonical .pxext form (no path traversal, links or
// oversized files), its manifest, its engine range and its command/Action ids
// against the project's other extensions. It is then extracted into
// Extensions/<owner>/<name>/ (committed with the project), registered in the
// project's `extensions` and recorded in prismatix-extensions.lock. Nothing
// from a package runs at install time; its scripts run only inside the
// game's sandboxed JavaScript host, limited to the capabilities it declares,
// and its Studio side only in PrismatiX Studio's sandboxed frame, limited to
// its permissions (docs/studio-extensions.md).

import {createHash, randomUUID} from "node:crypto";
import {createWriteStream, existsSync} from "node:fs";
import {lstat, mkdir, mkdtemp, readdir, readFile, rename, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, isAbsolute, join, relative, resolve, sep} from "node:path";
import {Readable} from "node:stream";
import {pipeline} from "node:stream/promises";

import {
  compareSemVer,
  ENGINE_VERSION,
  EXTENSION_INSTALL_FOLDER,
  EXTENSION_LOCK_FILE,
  extensionFilesDigest,
  extensionPackageLimits,
  extensionSides,
  isNativeExtensionPlatform,
  isVersionRange,
  manifestPlatforms,
  maxSatisfyingVersion,
  nativeExtensionPlatforms,
  nativeKeyFingerprint,
  nativePackBytes,
  nativePackFileName,
  parseExtensionPackageId,
  readExtensionNativePack,
  readExtensionPackage,
  satisfiesVersionRange,
  validateDocument,
  type AuthoringDiagnostic,
  type ExtensionManifest,
  type ExtensionNativePack,
  type ExtensionSide,
  type ExtensionPackageContents,
  type ExtensionPackageMetadata,
  type NativeExtensionPlatform,
} from "@prismatix/authoring-sdk";

import {extractTarGz} from "./archive.js";
import {hostPlatform, integrityMatches, registryUrl} from "./platform-runtime.js";
import {checkedUrl, RegistryClient, requireRegistry, type Located} from "./extension-registry.js";
import {projectExtensions, projectManifest, registerText, type LocatedExtension} from "./extension-scaffold.js";
import type {RegistryVersion} from "@prismatix/authoring-sdk";

// Sources --------------------------------------------------------------------

export type ExtensionSource =
  | {readonly kind: "registry"; readonly id: string; readonly range?: string}
  | {readonly kind: "file"; readonly path: string}
  | {readonly kind: "url"; readonly url: string}
  | {readonly kind: "npm"; readonly name: string; readonly range?: string};

/** Reads `owner/name[@range]`, `./file.pxext`, `https://…/file.pxext` or `npm:<package>[@range]`. */
export function parseExtensionSource(spec: string): ExtensionSource {
  if (spec.startsWith("npm:")) {
    const rest = spec.slice(4);
    const at = rest.lastIndexOf("@");
    const name = at > 0 ? rest.slice(0, at) : rest;
    const range = at > 0 ? rest.slice(at + 1) : undefined;
    if (!/^(?:@[a-z0-9-~][a-z0-9-._~]*\/)?[a-z0-9-~][a-z0-9-._~]*$/u.test(name)) throw new Error(`Not an npm package name: ${name}`);
    if (range !== undefined && !isVersionRange(range)) throw new Error(`Not a version range: ${range}`);
    return {kind: "npm", name, ...(range === undefined ? {} : {range})};
  }
  if (/^[a-z][a-z0-9+.-]*:\/\//iu.test(spec)) {
    checkedUrl(spec);
    return {kind: "url", url: spec};
  }
  if (/\.pxext$/iu.test(spec) || spec.startsWith(".") || isAbsolute(spec) || spec.includes("\\") || existsSync(spec))
    return {kind: "file", path: spec};
  const at = spec.indexOf("@");
  const id = at < 0 ? spec : spec.slice(0, at);
  const range = at < 0 ? undefined : spec.slice(at + 1);
  if (parseExtensionPackageId(id) === undefined)
    throw new Error(`${spec} is not owner/name[@range], a .pxext file, an https URL or npm:<package>`);
  if (range !== undefined && !isVersionRange(range)) throw new Error(`Not a version range: ${range}`);
  return {kind: "registry", id, ...(range === undefined || range === "" ? {} : {range})};
}

// Lock -----------------------------------------------------------------------

export interface ExtensionLockEntry {
  readonly version: string;
  /** SHA-256 of the .pxext. */
  readonly sha256: string;
  readonly size: number;
  /** `registry+<url>`, `file:<path>`, an https URL or `npm:<package>@<version>`. */
  readonly source: string;
  /** The range `update` follows (registry and npm sources). */
  readonly range?: string;
  readonly manifest: string;
  /** extensionFilesDigest of Extensions/<owner>/<name>/ (native files included). */
  readonly files: string;
  /** SHA-256 of the native signing key accepted with --allow-native. */
  readonly nativeKey?: string;
  /** The native pack installed for each platform. */
  readonly platforms?: Readonly<Partial<Record<NativeExtensionPlatform, {readonly sha256: string; readonly size: number}>>>;
}

export interface ExtensionLock {
  readonly format: "PrismatiXExtensionLock";
  readonly lockVersion: 1;
  readonly extensions: Readonly<Record<string, ExtensionLockEntry>>;
}

export async function readExtensionLock(root: string): Promise<ExtensionLock> {
  const path = join(root, EXTENSION_LOCK_FILE);
  if (!existsSync(path)) return {format: "PrismatiXExtensionLock", lockVersion: 1, extensions: {}};
  let value: unknown;
  try { value = JSON.parse((await readFile(path, "utf8")).replace(/^﻿/u, "")); }
  catch (cause) { throw new Error(`${EXTENSION_LOCK_FILE} is not valid JSON: ${String(cause)}`); }
  const result = validateDocument<ExtensionLock>("extensionLock", value, EXTENSION_LOCK_FILE);
  if (!result.valid)
    throw new Error(`${EXTENSION_LOCK_FILE} is not a valid lock: ${result.diagnostics.map((item) => item.details ?? item.message).join("; ")}`);
  return result.value!;
}

async function writeExtensionLock(root: string, extensions: Readonly<Record<string, ExtensionLockEntry>>): Promise<void> {
  const ordered: Record<string, ExtensionLockEntry> = {};
  for (const id of Object.keys(extensions).sort()) {
    const entry = extensions[id]!;
    const platforms = entry.platforms === undefined ? undefined : Object.fromEntries(nativeExtensionPlatforms
      .filter((platform) => entry.platforms![platform] !== undefined)
      .map((platform) => [platform, {sha256: entry.platforms![platform]!.sha256, size: entry.platforms![platform]!.size}]));
    ordered[id] = {version: entry.version, sha256: entry.sha256, size: entry.size, source: entry.source,
      ...(entry.range === undefined ? {} : {range: entry.range}), manifest: entry.manifest, files: entry.files,
      ...(entry.nativeKey === undefined ? {} : {nativeKey: entry.nativeKey}),
      ...(platforms === undefined || Object.keys(platforms).length === 0 ? {} : {platforms})};
  }
  const lock: ExtensionLock = {format: "PrismatiXExtensionLock", lockVersion: 1, extensions: ordered};
  const path = join(root, EXTENSION_LOCK_FILE);
  const partial = `${path}.${randomUUID()}.partial`;
  await writeFile(partial, `${JSON.stringify(lock, null, 2)}\n`, "utf8");
  await rename(partial, path);
}

// Fetching -------------------------------------------------------------------

export interface FetchedPackage {
  readonly bytes: Uint8Array;
  readonly sha256: string;
  readonly contents: ExtensionPackageContents;
  readonly source: string;
  readonly range?: string;
  /** Set when the registry marks this version yanked. */
  readonly yanked?: string;
  readonly notes: readonly string[];
  /** The checked native pack of each platform fetched (native extensions only). */
  readonly natives?: ReadonlyMap<NativeExtensionPlatform, FetchedNativePack>;
}

export interface FetchedNativePack {
  readonly bytes: Uint8Array;
  readonly sha256: string;
  readonly pack: ExtensionNativePack;
}

export interface InstallOptions {
  readonly registry?: string;
  readonly yes?: boolean;
  /** Consent to install an extension that loads native libraries (code outside the sandbox). */
  readonly allowNative?: boolean;
  /** The platforms whose native packs to fetch; this computer's when absent. */
  readonly platforms?: readonly NativeExtensionPlatform[];
  /** Expected SHA-256 of a file or URL source. */
  readonly sha256?: string;
  readonly fetch?: typeof fetch;
  readonly engineVersion?: string;
  readonly confirm?: (summary: string) => Promise<boolean>;
  readonly onProgress?: (message: string) => void;
}

class PackageRejected extends Error {
  constructor(readonly diagnostics: readonly AuthoringDiagnostic[], what: string) {
    super(`${what} is not a valid extension package: ${diagnostics.filter((item) => item.severity === "error")
      .map((item) => `${item.code} ${item.message}${item.path ? ` (${item.path})` : ""}${item.details ? `: ${item.details}` : ""}`).join("; ")}`);
    this.name = "PackageRejected";
  }
}

function sha256(bytes: Uint8Array): string {
  return createHash("sha256").update(bytes).digest("hex");
}

async function readBody(response: Response, limit: number, what: string): Promise<Uint8Array> {
  if (response.body === null) return new Uint8Array();
  const chunks: Uint8Array[] = [];
  let total = 0;
  const reader = response.body.getReader();
  for (;;) {
    const {done, value} = await reader.read();
    if (done) break;
    total += value.length;
    if (total > limit) { await reader.cancel(); throw new Error(`${what} is larger than ${limit} bytes; nothing was installed`); }
    chunks.push(value);
  }
  return Buffer.concat(chunks);
}

function checkedPackage(bytes: Uint8Array, what: string): ExtensionPackageContents {
  const contents = readExtensionPackage(bytes);
  if (!contents.valid) throw new PackageRejected(contents.diagnostics, what);
  return contents;
}

function fileSource(root: string, path: string): string {
  const relativePath = relative(root, resolve(path));
  return `file:${relativePath.startsWith("..") || isAbsolute(relativePath)
    ? resolve(path).split(sep).pop() : relativePath.split(sep).join("/")}`;
}

async function fetchNpm(name: string, range: string | undefined, options: InstallOptions):
  Promise<{bytes: Uint8Array; source: string; siblings: Map<string, Uint8Array>}> {
  const fetchImpl = options.fetch ?? fetch;
  const registry = registryUrl();
  const metadataUrl = new URL(name.replace("/", "%2f"), registry);
  options.onProgress?.(`Looking up ${name} on ${registry}`);
  const response = await fetchImpl(metadataUrl, {headers: {accept: "application/json"}, redirect: "follow"});
  if (!response.ok) throw new Error(`${metadataUrl.href} answered ${response.status} ${response.statusText}`);
  const packument = await response.json() as {"dist-tags"?: Record<string, string>; versions?: Record<string, {
    dist?: {tarball?: string; integrity?: string; shasum?: string}}>};
  const versions = Object.keys(packument.versions ?? {});
  const version = range === undefined ? packument["dist-tags"]?.latest : maxSatisfyingVersion(versions, range);
  const dist = version === undefined ? undefined : packument.versions?.[version]?.dist;
  if (version === undefined || dist?.tarball === undefined) throw new Error(`No version of ${name} matches ${range ?? "latest"}`);
  const tarball = checkedUrl(dist.tarball);
  options.onProgress?.(`Downloading ${tarball.href}`);
  const download = await fetchImpl(tarball, {redirect: "follow"});
  if (!download.ok) throw new Error(`${tarball.href} answered ${download.status} ${download.statusText}`);
  const temporary = await mkdtemp(join(tmpdir(), "prismatix-npm-extension-"));
  try {
    const archive = join(temporary, "package.tgz");
    const sha512 = createHash("sha512");
    const sha1 = createHash("sha1");
    let received = 0;
    const body = Readable.fromWeb(download.body as import("node:stream/web").ReadableStream<Uint8Array>);
    body.on("data", (chunk: Buffer) => {
      received += chunk.length;
      if (received > 2 * extensionPackageLimits.packageBytes) body.destroy(new Error(`${tarball.href} is too large`));
      sha512.update(chunk); sha1.update(chunk);
    });
    await pipeline(body, createWriteStream(archive, {flags: "wx"}));
    if (!integrityMatches({sha512: sha512.digest("base64"), sha1: sha1.digest("hex")}, dist.integrity, dist.shasum))
      throw new Error(`${tarball.href} does not match the integrity npm lists; nothing was installed`);
    const extracted = join(temporary, "package");
    await mkdir(extracted);
    await extractTarGz(archive, extracted, {maxEntries: 4096, maxBytes: 4 * extensionPackageLimits.packageBytes});
    const root = join(extracted, "package");
    const manifest = existsSync(join(root, "package.json"))
      ? JSON.parse(await readFile(join(root, "package.json"), "utf8")) as {prismatix?: {extension?: unknown}} : {};
    let file = typeof manifest.prismatix?.extension === "string" ? manifest.prismatix.extension : undefined;
    if (file === undefined) {
      const candidates = (await readdir(root)).filter((entry) => entry.endsWith(".pxext"));
      if (candidates.length !== 1)
        throw new Error(`${name}@${version} must hold one .pxext at its root or name it in package.json "prismatix": {"extension": "<file>"}`);
      file = candidates[0]!;
    }
    const path = resolve(root, file);
    if (relative(root, path).startsWith("..") || !(await lstat(path)).isFile())
      throw new Error(`${name}@${version} names ${file}, which is not a file in the package`);
    // Native packs travel beside the main package.
    const siblings = new Map<string, Uint8Array>();
    for (const entry of await readdir(dirname(path), {withFileTypes: true}))
      if (entry.isFile() && /-native-[a-z0-9-]+\.pxext$/u.test(entry.name) && siblings.size < 8 &&
          (await lstat(join(dirname(path), entry.name))).size <= nativePackBytes)
        siblings.set(entry.name, new Uint8Array(await readFile(join(dirname(path), entry.name))));
    return {bytes: new Uint8Array(await readFile(path)), source: `npm:${name}@${version}`, siblings};
  } finally {
    await rm(temporary, {recursive: true, force: true});
  }
}

/** Downloads (or reads) a package and checks it; nothing is written to the project. */
export async function fetchExtensionPackage(root: string, spec: string | ExtensionSource,
  options: InstallOptions = {}): Promise<FetchedPackage> {
  const source = typeof spec === "string" ? parseExtensionSource(spec) : spec;
  const engine = options.engineVersion ?? ENGINE_VERSION;
  const notes: string[] = [];
  const pinned = (bytes: Uint8Array) => {
    if (options.sha256 !== undefined && sha256(bytes) !== options.sha256.toLowerCase())
      throw new Error(`The package's SHA-256 is ${sha256(bytes)}, not the expected ${options.sha256}; nothing was installed`);
  };
  // Only an explicit --platform narrows which versions resolve: a project may
  // target platforms other than this computer.
  const narrowed = options.platforms?.length === 1 ? options.platforms[0] : undefined;
  let fetched: FetchedPackage;
  let nativeBytes: (platform: NativeExtensionPlatform, file: string) => Promise<Uint8Array>;
  if (source.kind === "registry") {
    const location = await requireRegistry(root, options.registry);
    const client = new RegistryClient(location.url, options.fetch === undefined ? {} : {fetch: options.fetch});
    const range = source.range ?? "*";
    options.onProgress?.(`Resolving ${source.id}@${range} for engine ${engine}${narrowed ? ` on ${narrowed}` : ""} on ${location.url}`);
    const version: Located<RegistryVersion> = await client.resolve(source.id, range, engine, narrowed);
    options.onProgress?.(`Downloading ${source.id}@${version.value.version}`);
    const bytes = await client.download(version);
    const contents = checkedPackage(bytes, `${source.id}@${version.value.version}`);
    if (contents.metadata!.id !== source.id || contents.metadata!.version !== version.value.version)
      throw new Error(`The registry served ${contents.metadata!.id}@${contents.metadata!.version} for ${source.id}@${version.value.version}; nothing was installed`);
    if (version.value.yanked) notes.push(`${source.id}@${version.value.version} is yanked${version.value.yankedReason
      ? `: ${version.value.yankedReason}` : ""}; it installs only because it is pinned exactly`);
    fetched = {bytes, sha256: version.value.sha256, contents, source: `registry+${location.url}`,
      range: source.range ?? `^${version.value.version}`,
      ...(version.value.yanked ? {yanked: version.value.yankedReason ?? "yanked"} : {}), notes};
    nativeBytes = (platform) => {
      options.onProgress?.(`Downloading the ${platform} native pack of ${source.id}@${version.value.version}`);
      return client.downloadNative(version, platform);
    };
  } else if (source.kind === "file") {
    const path = resolve(source.path);
    const info = await lstat(path).catch(() => undefined);
    if (info === undefined || !info.isFile()) throw new Error(`${source.path} is not a file`);
    if (info.size > extensionPackageLimits.packageBytes)
      throw new Error(`${source.path} is larger than ${extensionPackageLimits.packageBytes} bytes`);
    const bytes = new Uint8Array(await readFile(path));
    pinned(bytes);
    fetched = {bytes, sha256: sha256(bytes), contents: checkedPackage(bytes, source.path), source: fileSource(root, path), notes};
    nativeBytes = async (_platform, file) => {
      const sibling = join(dirname(path), file);
      const found = await lstat(sibling).catch(() => undefined);
      if (found === undefined || !found.isFile()) throw new Error(`${file} is not beside ${source.path}`);
      if (found.size > nativePackBytes) throw new Error(`${file} is larger than ${nativePackBytes} bytes`);
      return new Uint8Array(await readFile(sibling));
    };
  } else if (source.kind === "url") {
    const url = checkedUrl(source.url);
    options.onProgress?.(`Downloading ${url.href}`);
    const response = await (options.fetch ?? fetch)(url, {redirect: "follow"});
    checkedUrl(response.url || url.href);
    if (!response.ok) throw new Error(`${url.href} answered ${response.status} ${response.statusText}`);
    const bytes = await readBody(response, extensionPackageLimits.packageBytes, url.href);
    pinned(bytes);
    if (options.sha256 === undefined)
      notes.push(`Downloaded from a URL without --sha256; the lock records ${sha256(bytes)} so later changes are caught`);
    url.hash = "";
    fetched = {bytes, sha256: sha256(bytes), contents: checkedPackage(bytes, url.href), source: url.href, notes};
    // A native pack is checked against the hashes the main package pins.
    nativeBytes = async (_platform, file) => {
      const sibling = checkedUrl(file, url);
      options.onProgress?.(`Downloading ${sibling.href}`);
      const answer = await (options.fetch ?? fetch)(sibling, {redirect: "follow"});
      checkedUrl(answer.url || sibling.href);
      if (!answer.ok) throw new Error(`${sibling.href} answered ${answer.status} ${answer.statusText}`);
      return readBody(answer, nativePackBytes, sibling.href);
    };
  } else {
    const npm = await fetchNpm(source.name, source.range, options);
    pinned(npm.bytes);
    fetched = {bytes: npm.bytes, sha256: sha256(npm.bytes), contents: checkedPackage(npm.bytes, npm.source), source: npm.source,
      ...(source.range === undefined ? {} : {range: source.range}), notes};
    nativeBytes = async (_platform, file) => {
      const found = npm.siblings.get(file);
      if (found === undefined) throw new Error(`${npm.source} holds no ${file}`);
      return found;
    };
  }
  const manifest = fetched.contents.manifest!;
  if (manifest.native === undefined) return fetched;
  const offered = manifestPlatforms(manifest);
  // Without --platform: this computer's pack when there is one, so inspect
  // can still show an extension built only for other platforms.
  const wanted = options.platforms ?? (isNativeExtensionPlatform(hostPlatform) && offered.includes(hostPlatform) &&
    manifest.native.libraries[hostPlatform] !== undefined ? [hostPlatform] : []);
  const natives = new Map<NativeExtensionPlatform, FetchedNativePack>();
  for (const platform of wanted) {
    if (!offered.includes(platform) || manifest.native.libraries[platform] === undefined)
      throw new Error(`${manifest.id} has no native library for ${platform} (it has ${offered.join(", ") || "none"}); nothing was installed`);
    const file = nativePackFileName(fetched.contents.metadata!.id, fetched.contents.metadata!.version!, platform);
    const bytes = await nativeBytes(platform, file);
    const pack = await readExtensionNativePack(bytes, manifest, platform);
    if (!pack.valid) throw new PackageRejected(pack.diagnostics, file);
    natives.set(platform, {bytes, sha256: sha256(bytes), pack});
  }
  if (wanted.length === 0)
    notes.push(`${manifest.id} has no native library for this computer (${hostPlatform}); choose platforms with --platform`);
  return {...fetched, natives};
}

// Checks ---------------------------------------------------------------------

export interface ExtensionConflict {
  readonly id: string;
  readonly kind: "extension" | "command" | "action";
  readonly with: string;
}

/** Ids the manifest shares with the project's other extensions (the one at `replacing` excluded). */
export function extensionConflicts(manifest: ExtensionManifest, others: readonly LocatedExtension[],
  replacing?: string): ExtensionConflict[] {
  const conflicts: ExtensionConflict[] = [];
  for (const other of others) {
    if (other.manifestPath === replacing) continue;
    if (other.manifest.id === manifest.id) conflicts.push({id: manifest.id, kind: "extension", with: other.manifestPath});
    const theirs = new Set([...other.manifest.commands ?? [], ...other.manifest.actions ?? []].map((item) => item.id));
    for (const command of manifest.commands ?? []) if (theirs.has(command.id)) conflicts.push({id: command.id, kind: "command", with: other.manifestPath});
    for (const action of manifest.actions ?? []) if (theirs.has(action.id)) conflicts.push({id: action.id, kind: "action", with: other.manifestPath});
  }
  return conflicts;
}

/** Conflicts between every pair of the project's registered extensions. */
export function projectConflicts(all: readonly LocatedExtension[]): ExtensionConflict[] {
  const conflicts: ExtensionConflict[] = [];
  all.forEach((item, index) => {
    for (const conflict of extensionConflicts(item.manifest, all.slice(index + 1)))
      conflicts.push({...conflict, with: `${item.manifestPath} and ${conflict.with}`});
  });
  return conflicts;
}

export const capabilityDescriptions: Readonly<Record<string, string>> = {
  runtime: "run Story commands and use events, timing, logging and variables",
  ui: "open, replace and close UI screens and register UI Actions",
  audio: "play and stop music, sound effects, voice and ambience",
  video: "play video",
  animation: "load and play animations",
  persistence: "save, load and delete save data and keep its own saved state",
  input: "read input actions (keys and buttons)",
  render: "draw its own render nodes on screen",
  "raw.mesh": "draw its own meshes (vertices, textures, blending, masks) on screen; experimental",
  "raw.texture": "create textures and write their pixels; experimental",
  "raw.target": "draw into offscreen render targets; experimental",
  native: "call its native libraries' services",
};

export const studioPermissionDescriptions: Readonly<Record<string, string>> = {
  "project.read": "read files inside the project",
  "project.write": "create and overwrite files inside the project",
};

/** What a package's Studio side asks for and adds (`extension inspect --json` `studio`). */
export interface StudioSummary {
  readonly requiredStudioVersion: string;
  readonly permissions: readonly string[];
  readonly contributes: {
    readonly assetTypes: readonly {readonly id: string; readonly displayName: string}[];
    readonly inspectors: readonly {readonly assetType: string}[];
    readonly parameterOptions: readonly {readonly id: string; readonly displayName?: string}[];
    readonly importers: readonly {readonly id: string; readonly displayName: string}[];
  };
}

/** The Studio side of a manifest, or undefined when it has none. */
export function studioSummary(manifest: Pick<ExtensionManifest, "studio">): StudioSummary | undefined {
  const studio = manifest.studio;
  if (studio === undefined) return undefined;
  const contributes = studio.contributes ?? {};
  return {requiredStudioVersion: studio.requiredStudioVersion, permissions: studio.permissions ?? [],
    contributes: {
      assetTypes: (contributes.assetTypes ?? []).map((item) => ({id: item.id, displayName: item.displayName})),
      inspectors: (contributes.inspectors ?? []).map((item) => ({assetType: item.assetType})),
      parameterOptions: (contributes.parameterOptions ?? []).map((item) =>
        ({id: item.id, ...(item.displayName === undefined ? {} : {displayName: item.displayName})})),
      importers: (contributes.importers ?? []).map((item) => ({id: item.id, displayName: item.displayName})),
    }};
}

export interface NativeSummary {
  readonly services: readonly string[];
  /** Platforms the package has native libraries for. */
  readonly platforms: readonly NativeExtensionPlatform[];
  /** Platforms whose native packs are being installed. */
  readonly installing: readonly NativeExtensionPlatform[];
  /** SHA-256 of the signing public key. */
  readonly keyFingerprint?: string;
  /** The fingerprint the lock accepted before, when it differs. */
  readonly previousKey?: string;
  /**
   * Files the game's maker must supply (not in the package, not signed),
   * per platform; packing a game for that platform needs them.
   */
  readonly userFiles: readonly {readonly platform: NativeExtensionPlatform; readonly name: string; readonly hint?: string}[];
}

export interface PackageSummary {
  readonly id: string;
  readonly version: string;
  readonly sha256: string;
  readonly size: number;
  readonly source: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly license?: string;
  readonly author?: string;
  readonly requiredEngineVersion: string;
  readonly engineVersion: string;
  readonly compatible: boolean;
  readonly capabilities: readonly string[];
  readonly commands: readonly string[];
  readonly actions: readonly string[];
  /** "game" when it has a game side (an entry), "studio" when it has a Studio side. */
  readonly sides: readonly ExtensionSide[];
  /** Set when it has a Studio side. */
  readonly studio?: StudioSummary;
  readonly yanked?: string;
  readonly installed?: {readonly version: string; readonly sha256: string};
  readonly conflicts: readonly ExtensionConflict[];
  readonly warnings: readonly AuthoringDiagnostic[];
  readonly notes: readonly string[];
  /** Platforms it runs on; absent: every platform. */
  readonly platforms?: readonly string[];
  /** Set when it loads native libraries: installing needs --allow-native. */
  readonly native?: NativeSummary;
}

function installFolder(id: string): string {
  const {owner, name} = parseExtensionPackageId(id)!;
  return `${EXTENSION_INSTALL_FOLDER}/${owner}/${name}`;
}

export async function summarizePackage(root: string, fetched: FetchedPackage, engineVersion = ENGINE_VERSION): Promise<PackageSummary> {
  const metadata = fetched.contents.metadata!;
  const manifest = fetched.contents.manifest!;
  const lock = await readExtensionLock(root);
  const target = `${installFolder(metadata.id)}/${fetched.contents.manifestPath}`;
  const installed = lock.extensions[metadata.id];
  const keyFingerprint = manifest.native?.publicKey === undefined ? undefined : await nativeKeyFingerprint(manifest.native.publicKey);
  const native: NativeSummary | undefined = manifest.native === undefined ? undefined : {
    services: manifest.native.services,
    platforms: nativeExtensionPlatforms.filter((platform) => manifest.native!.libraries[platform] !== undefined),
    installing: [...fetched.natives?.keys() ?? []],
    userFiles: nativeExtensionPlatforms.flatMap((platform) => (manifest.native!.libraries[platform]?.userFiles ?? [])
      .map((file) => ({platform, name: file.name, ...(file.hint === undefined ? {} : {hint: file.hint})}))),
    ...(keyFingerprint === undefined ? {} : {keyFingerprint}),
    ...(installed?.nativeKey !== undefined && installed.nativeKey !== keyFingerprint ? {previousKey: installed.nativeKey} : {})};
  const studio = studioSummary(manifest);
  return {id: metadata.id, version: metadata.version!, sha256: fetched.sha256, size: fetched.bytes.length, source: fetched.source,
    ...(manifest.platforms === undefined && manifest.native === undefined ? {} : {platforms: manifestPlatforms(manifest)}),
    ...(native === undefined ? {} : {native}),
    ...(metadata.displayName === undefined ? {} : {displayName: metadata.displayName}),
    ...(metadata.description === undefined || metadata.description === "" ? {} : {description: metadata.description}),
    ...(metadata.license === undefined ? {} : {license: metadata.license}),
    ...(metadata.author === undefined ? {} : {author: metadata.author.name}),
    requiredEngineVersion: manifest.requiredEngineVersion, engineVersion,
    compatible: satisfiesVersionRange(engineVersion, manifest.requiredEngineVersion, {includePrerelease: true}) === true,
    capabilities: manifest.capabilities ?? [], commands: (manifest.commands ?? []).map((item) => item.id),
    actions: (manifest.actions ?? []).map((item) => item.id),
    sides: extensionSides(manifest), ...(studio === undefined ? {} : {studio}),
    ...(fetched.yanked === undefined ? {} : {yanked: fetched.yanked}),
    ...(installed === undefined ? {} : {installed: {version: installed.version, sha256: installed.sha256}}),
    conflicts: extensionConflicts(manifest, await projectExtensions(root), installed?.manifest ?? target),
    warnings: fetched.contents.diagnostics.filter((item) => item.severity === "warning"), notes: fetched.notes};
}

export function describeSummary(summary: PackageSummary): string {
  return [
    `${summary.displayName ?? summary.id} ${summary.version} (${summary.id})${summary.author ? ` by ${summary.author}` : ""}`,
    ...(summary.description ? [`  ${summary.description}`] : []),
    `  from ${summary.source}, ${summary.size} bytes, SHA-256 ${summary.sha256}`,
    ...(summary.license ? [`  license ${summary.license}`] : []),
    `  requires engine ${summary.requiredEngineVersion} (this project: ${summary.engineVersion})`,
    ...(!summary.sides.includes("game")
      ? ["  It adds nothing to the game: it has only a Studio side."]
      : [
        "  In the game it may:",
        ...summary.capabilities.map((capability) => `    - ${capability}: ${capabilityDescriptions[capability] ?? capability}`),
        `  It adds ${summary.commands.length} Story command(s)${summary.commands.length ? `: ${summary.commands.join(", ")}` : ""}`,
        `  and ${summary.actions.length} UI Action(s)${summary.actions.length ? `: ${summary.actions.join(", ")}` : ""}.`,
      ]),
    ...(summary.studio === undefined ? [] : describeStudio(summary.studio)),
    ...(summary.platforms === undefined ? [] : [`  Runs on: ${summary.platforms.join(", ")}`]),
    ...(summary.native === undefined
      ? [summary.sides.includes("game")
        ? "  Nothing runs now; its scripts run only in the game's sandbox, limited to these capabilities."
        : "  Nothing runs now."]
      : [
        "  WARNING: it loads NATIVE CODE. Its libraries run inside the game process with the player's full",
        "  permissions (files, network, everything), outside the sandbox and the capabilities above. Install it",
        "  only if you trust its author as much as the engine itself. Nothing runs now.",
        `  Native services: ${summary.native.services.join(", ")}`,
        `  Native libraries for: ${summary.native.platforms.join(", ")}; installing: ${summary.native.installing.join(", ") || "none"}`,
        `  Signing key SHA-256: ${summary.native.keyFingerprint ?? "(none)"}`,
        ...(summary.native.previousKey === undefined ? []
          : [`  The signing key CHANGED: the installed version was signed by ${summary.native.previousKey}.`]),
        ...(summary.native.userFiles.length === 0 ? [] : [
          "  You must supply these files yourself (not in the package, not covered by its signature);",
          "  packing a game for the platform fails until they are in NativeUserFiles/ or PRISMATIX_NATIVE_USER_FILES:",
          ...summary.native.userFiles.map((file) => `    - ${file.name} (${file.platform})${file.hint === undefined ? "" : `: ${file.hint}`}`)]),
        "  It installs only with --allow-native.",
      ]),
    ...(summary.installed ? [`  Replaces the installed ${summary.installed.version}.`] : []),
    ...summary.notes.map((note) => `  Note: ${note}`),
  ].join("\n");
}

function describeStudio(studio: StudioSummary): string[] {
  const {assetTypes, inspectors, parameterOptions, importers} = studio.contributes;
  return [
    `  In PrismatiX Studio (${studio.requiredStudioVersion}), in a sandboxed frame, it may:`,
    ...(studio.permissions.length === 0 ? ["    - nothing beyond what Studio hands it (no permissions)"]
      : studio.permissions.map((permission) => `    - ${permission}: ${studioPermissionDescriptions[permission] ?? permission}`)),
    ...(assetTypes.length === 0 ? [] : [`  Asset types: ${assetTypes.map((item) => `${item.displayName} (${item.id})`).join(", ")}`]),
    ...(inspectors.length === 0 ? [] : [`  Inspectors for: ${inspectors.map((item) => item.assetType).join(", ")}`]),
    ...(parameterOptions.length === 0 ? [] : [`  Parameter options: ${parameterOptions.map((item) => item.displayName === undefined
      ? item.id : `${item.displayName} (${item.id})`).join(", ")}`]),
    ...(importers.length === 0 ? [] : [`  Import items in Assets: ${importers.map((item) => `${item.displayName} (${item.id})`).join(", ")}`]),
    "  Studio asks each user before it loads the Studio side.",
  ];
}

// Files ----------------------------------------------------------------------

/** Regular files under `directory` by "/" path; links and special files are refused. */
async function treeFiles(directory: string, prefix = ""): Promise<[string, Uint8Array][]> {
  const files: [string, Uint8Array][] = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    const path = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
    const full = join(directory, entry.name);
    if (entry.isSymbolicLink()) throw new Error(`${path} is a link`);
    if (entry.isDirectory()) files.push(...await treeFiles(full, path));
    else if (entry.isFile()) files.push([path, new Uint8Array(await readFile(full))]);
    else throw new Error(`${path} is not a regular file`);
  }
  return files;
}

async function projectText(root: string): Promise<{file: string; text: string}> {
  const file = await projectManifest(root);
  return {file, text: await readFile(join(root, file), "utf8")};
}

/** Removes one path from the project's `extensions`, keeping the rest of its text. */
function unregisterText(text: string, manifestPath: string): string {
  const value = JSON.parse(text.replace(/^﻿/u, "")) as {extensions?: unknown[]};
  if (!Array.isArray(value.extensions) || !value.extensions.includes(manifestPath)) return text;
  const quoted = JSON.stringify(manifestPath).replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
  for (const pattern of [new RegExp(`\\s*${quoted}\\s*,`, "u"), new RegExp(`,\\s*${quoted}`, "u"), new RegExp(`\\s*${quoted}\\s*`, "u")]) {
    const patched = text.replace(pattern, "");
    try {
      const parsed = JSON.parse(patched.replace(/^﻿/u, "")) as {extensions?: unknown[]};
      if (Array.isArray(parsed.extensions) && !parsed.extensions.includes(manifestPath) &&
          parsed.extensions.length === value.extensions.length - 1) return patched;
    } catch { /* try the next form */ }
  }
  value.extensions = value.extensions.filter((item) => item !== manifestPath);
  return `${JSON.stringify(value, null, 2)}\n`;
}

export interface InstallResult {
  readonly id: string;
  readonly version: string;
  readonly previous?: string;
  readonly manifest: string;
  readonly folder: string;
  readonly lock: ExtensionLockEntry;
  readonly summary: PackageSummary;
  readonly updated: readonly string[];
}

/** Writes a checked package into the project (after the checks and the confirmation). */
export async function installFetchedPackage(root: string, fetched: FetchedPackage, options: InstallOptions = {}): Promise<InstallResult> {
  const summary = await summarizePackage(root, fetched, options.engineVersion);
  const metadata: ExtensionPackageMetadata = fetched.contents.metadata!;
  if (!summary.compatible)
    throw new Error(`${summary.id}@${summary.version} requires engine ${summary.requiredEngineVersion}; this project's CLI is ${summary.engineVersion}`);
  if (summary.conflicts.length > 0)
    throw new Error(`${summary.id} conflicts with the project's extensions: ${summary.conflicts
      .map((conflict) => `${conflict.kind} ${conflict.id} (${conflict.with})`).join(", ")}; nothing was installed`);
  const lock = await readExtensionLock(root);
  const folder = installFolder(metadata.id);
  const manifestPath = `${folder}/${fetched.contents.manifestPath}`;
  const existing = lock.extensions[metadata.id];
  const target = join(root, ...folder.split("/"));
  if (existsSync(target) && existing === undefined)
    throw new Error(`${folder} exists but ${EXTENSION_LOCK_FILE} does not list ${metadata.id}; move the folder away first`);
  // An update signed by the key the lock already accepted needs no new consent.
  const acceptedKey = existing?.nativeKey !== undefined && existing.nativeKey === summary.native?.keyFingerprint;
  if (summary.native !== undefined && options.allowNative !== true && !acceptedKey)
    throw new Error(`${summary.id} loads native libraries (${summary.native.services.join(", ")}), code that runs outside ` +
      "the sandbox with the player's full permissions. Review it (prismatix extension inspect) and add --allow-native to install it; nothing was installed");
  if (options.yes !== true) {
    const accepted = options.confirm === undefined ? false : await options.confirm(describeSummary(summary));
    if (!accepted) throw new Error(options.confirm === undefined
      ? `Review what ${summary.id} may do (prismatix extension inspect) and confirm with --yes` : "Cancelled; nothing was installed");
  }

  const base = join(root, EXTENSION_INSTALL_FOLDER);
  await mkdir(base, {recursive: true});
  const staging = join(base, `.staging-${randomUUID()}`);
  const old = join(base, `.old-${randomUUID()}`);
  let moved = false;
  const installedFiles = new Map(fetched.contents.files);
  for (const native of fetched.natives?.values() ?? [])
    for (const [path, data] of native.pack.files) installedFiles.set(path, data);
  try {
    for (const [path, data] of installedFiles) {
      const file = join(staging, ...path.split("/"));
      if (relative(staging, file).startsWith("..")) throw new Error(`Refusing to write ${path} outside the extension folder`);
      await mkdir(dirname(file), {recursive: true});
      await writeFile(file, data, {flag: "wx"});
    }
    await mkdir(dirname(target), {recursive: true});
    if (existsSync(target)) { await rename(target, old); moved = true; }
    await rename(staging, target);
  } catch (cause) {
    await rm(staging, {recursive: true, force: true});
    if (moved && !existsSync(target)) await rename(old, target);
    throw cause;
  }
  const updated: string[] = [folder];
  try {
    const project = await projectText(root);
    let text = project.text;
    if (existing !== undefined && existing.manifest !== manifestPath) text = unregisterText(text, existing.manifest);
    text = registerText(text, manifestPath);
    if (text !== project.text) { await writeFile(join(root, project.file), text, "utf8"); updated.push(project.file); }
    const platforms = Object.fromEntries([...fetched.natives ?? []].map(([platform, native]) =>
      [platform, {sha256: native.sha256, size: native.bytes.length}]));
    const entry: ExtensionLockEntry = {version: metadata.version!, sha256: fetched.sha256, size: fetched.bytes.length,
      source: fetched.source, ...(fetched.range === undefined ? {} : {range: fetched.range}), manifest: manifestPath,
      files: await extensionFilesDigest(installedFiles),
      ...(summary.native?.keyFingerprint === undefined ? {} : {nativeKey: summary.native.keyFingerprint}),
      ...(Object.keys(platforms).length === 0 ? {} : {platforms})};
    await writeExtensionLock(root, {...lock.extensions, [metadata.id]: entry});
    updated.push(EXTENSION_LOCK_FILE);
    if (moved) await rm(old, {recursive: true, force: true});
    return {id: metadata.id, version: metadata.version!, ...(existing === undefined ? {} : {previous: existing.version}),
      manifest: manifestPath, folder, lock: entry, summary, updated};
  } catch (cause) {
    if (moved) { await rm(target, {recursive: true, force: true}); await rename(old, target); }
    throw cause;
  }
}

export async function addExtension(root: string, spec: string, options: InstallOptions = {}): Promise<InstallResult> {
  const fetched = await fetchExtensionPackage(root, spec, options);
  return installFetchedPackage(root, fetched, options);
}

export interface RemoveResult {
  readonly id: string;
  readonly version: string;
  readonly folder: string;
  readonly updated: readonly string[];
}

export async function removeExtension(root: string, id: string): Promise<RemoveResult> {
  const lock = await readExtensionLock(root);
  const entry = lock.extensions[id];
  if (entry === undefined)
    throw new Error(`${EXTENSION_LOCK_FILE} lists no ${id}; installed: ${Object.keys(lock.extensions).join(", ") || "none"}`);
  const folder = installFolder(id);
  const project = await projectText(root);
  const text = unregisterText(project.text, entry.manifest);
  const updated: string[] = [];
  if (text !== project.text) { await writeFile(join(root, project.file), text, "utf8"); updated.push(project.file); }
  const remaining = {...lock.extensions};
  delete remaining[id];
  await writeExtensionLock(root, remaining);
  updated.push(EXTENSION_LOCK_FILE);
  await rm(join(root, ...folder.split("/")), {recursive: true, force: true});
  const owner = join(root, ...folder.split("/").slice(0, 2));
  if (existsSync(owner) && (await readdir(owner)).length === 0) await rm(owner, {recursive: true, force: true});
  updated.push(folder);
  return {id, version: entry.version, folder, updated};
}

// Update and outdated ---------------------------------------------------------

function sourceOf(id: string, entry: ExtensionLockEntry, range?: string): ExtensionSource | undefined {
  if (entry.source.startsWith("registry+")) return {kind: "registry", id, range: range ?? entry.range ?? "*"};
  if (entry.source.startsWith("npm:")) {
    const rest = entry.source.slice(4);
    return {kind: "npm", name: rest.slice(0, rest.lastIndexOf("@")), ...(range ?? entry.range) === undefined ? {} : {range: range ?? entry.range!}};
  }
  return undefined;
}

export interface UpdateOutcome {
  readonly id: string;
  readonly from: string;
  readonly to?: string;
  readonly status: "updated" | "reinstalled" | "current" | "skipped";
  readonly reason?: string;
}

/**
 * Updates installed packages within their locked ranges (`to` replaces the
 * range). `reinstall` installs the locked version again from its source,
 * keeping the range: it restores files `verify` reports as changed.
 */
export async function updateExtensions(root: string, id: string | undefined,
  options: InstallOptions & {readonly to?: string; readonly reinstall?: boolean} = {}): Promise<UpdateOutcome[]> {
  const lock = await readExtensionLock(root);
  if (id !== undefined && lock.extensions[id] === undefined) throw new Error(`${EXTENSION_LOCK_FILE} lists no ${id}`);
  if (options.to !== undefined && id === undefined) throw new Error("--to needs the id of the extension to update");
  if (options.to !== undefined && options.reinstall === true) throw new Error("--reinstall keeps the locked version; it cannot be combined with --to");
  if (options.to !== undefined && !isVersionRange(options.to)) throw new Error(`Not a version range: ${options.to}`);
  const outcomes: UpdateOutcome[] = [];
  for (const [name, entry] of Object.entries(lock.extensions)) {
    if (id !== undefined && name !== id) continue;
    const source = sourceOf(name, entry, options.reinstall === true ? entry.version : options.to);
    if (source === undefined) {
      outcomes.push({id: name, from: entry.version, status: "skipped",
        reason: `installed from ${entry.source}; add the newer file or URL again to update it`});
      continue;
    }
    const {sha256: _pin, ...unpinned} = options;
    // Native packs: the platforms already installed, plus any --platform asks for.
    const lockedPlatforms = nativeExtensionPlatforms.filter((platform) => entry.platforms?.[platform] !== undefined);
    const platforms = [...new Set([...lockedPlatforms, ...options.platforms ?? []])];
    const fetched = await fetchExtensionPackage(root, source,
      {...unpinned, ...(platforms.length === 0 ? {} : {platforms})});
    const next = fetched.contents.metadata!.version!;
    if (options.reinstall === true) {
      if (fetched.sha256 !== entry.sha256)
        throw new Error(`${name}@${entry.version} from ${entry.source} is not the package the lock records (SHA-256 ${fetched.sha256}, expected ${entry.sha256}); nothing was installed`);
      for (const platform of lockedPlatforms)
        if (fetched.natives?.get(platform)?.sha256 !== entry.platforms![platform]!.sha256)
          throw new Error(`The ${platform} native pack of ${name}@${entry.version} is not the one the lock records; nothing was installed`);
      await installFetchedPackage(root, {...fetched, ...(entry.range === undefined ? {} : {range: entry.range})} as FetchedPackage, {...options, yes: true});
      outcomes.push({id: name, from: entry.version, to: next, status: "reinstalled"});
      continue;
    }
    const addsPlatforms = platforms.some((platform) => !lockedPlatforms.includes(platform));
    if (fetched.sha256 === entry.sha256 && options.to === undefined && !addsPlatforms) {
      outcomes.push({id: name, from: entry.version, status: "current"});
      continue;
    }
    if (compareSemVer(next, entry.version) < 0 && options.to === undefined) {
      outcomes.push({id: name, from: entry.version, status: "current", reason: `the range resolves to the older ${next}`});
      continue;
    }
    // New capabilities or Studio permissions need the same confirmation as an install.
    const installed = await readFile(join(root, ...entry.manifest.split("/")), "utf8")
      .then((text) => JSON.parse(text) as ExtensionManifest).catch(() => undefined);
    const incoming = fetched.contents.manifest!;
    const added = [...(incoming.capabilities ?? []).filter((item) => !(installed?.capabilities ?? []).includes(item)),
      ...(incoming.studio?.permissions ?? []).filter((item) => !(installed?.studio?.permissions ?? []).includes(item))];
    // So does native code signed by a different key.
    const key = fetched.contents.manifest!.native?.publicKey;
    const rekeyed = key !== undefined && entry.nativeKey !== await nativeKeyFingerprint(key);
    const yes = options.yes === true || (added.length === 0 && !rekeyed);
    await installFetchedPackage(root, {...fetched, range: options.to ?? entry.range ?? fetched.range ?? `^${next}`} as FetchedPackage,
      {...options, yes});
    outcomes.push({id: name, from: entry.version, to: next, status: "updated"});
  }
  return outcomes;
}

export interface OutdatedEntry {
  readonly id: string;
  readonly current: string;
  /** The newest version the locked range allows on this engine. */
  readonly wanted?: string;
  /** The newest version on this engine. */
  readonly latest?: string;
  readonly source: string;
  readonly problem?: string;
}

export async function outdatedExtensions(root: string, options: InstallOptions = {}): Promise<OutdatedEntry[]> {
  const lock = await readExtensionLock(root);
  const engine = options.engineVersion ?? ENGINE_VERSION;
  const results: OutdatedEntry[] = [];
  for (const [id, entry] of Object.entries(lock.extensions)) {
    if (!entry.source.startsWith("registry+")) {
      results.push({id, current: entry.version, source: entry.source});
      continue;
    }
    try {
      const location = await requireRegistry(root, options.registry);
      const client = new RegistryClient(location.url, options.fetch === undefined ? {} : {fetch: options.fetch});
      const wanted = await client.resolve(id, entry.range ?? "*", engine).then((found) => found.value.version, () => undefined);
      const latest = await client.resolve(id, "*", engine).then((found) => found.value.version, () => undefined);
      results.push({id, current: entry.version, ...(wanted === undefined ? {} : {wanted}),
        ...(latest === undefined ? {} : {latest}), source: entry.source});
    } catch (cause) {
      results.push({id, current: entry.version, source: entry.source, problem: cause instanceof Error ? cause.message : String(cause)});
    }
  }
  return results;
}

// Verify -----------------------------------------------------------------------

export interface VerifyEntry {
  readonly id: string;
  readonly version?: string;
  readonly status: "ok" | "modified" | "missing" | "unregistered" | "unlocked";
  readonly details?: string;
}

/** Re-hashes every installed package against the lock. */
export async function verifyExtensions(root: string): Promise<{entries: VerifyEntry[]; conflicts: ExtensionConflict[]}> {
  const lock = await readExtensionLock(root);
  const registered = await projectExtensions(root);
  const project = JSON.parse((await projectText(root)).text.replace(/^﻿/u, "")) as {extensions?: unknown[]};
  const paths = new Set((project.extensions ?? []).filter((item): item is string => typeof item === "string"));
  const entries: VerifyEntry[] = [];
  for (const [id, entry] of Object.entries(lock.extensions)) {
    const folder = join(root, ...installFolder(id).split("/"));
    if (!existsSync(folder)) { entries.push({id, version: entry.version, status: "missing", details: `${installFolder(id)} is missing`}); continue; }
    let digest: string;
    try { digest = await extensionFilesDigest(await treeFiles(folder)); }
    catch (cause) { entries.push({id, version: entry.version, status: "modified", details: String(cause)}); continue; }
    if (digest !== entry.files) {
      entries.push({id, version: entry.version, status: "modified",
        details: `the files in ${installFolder(id)} differ from what was installed; run prismatix extension update ${id} --reinstall to restore them`});
      continue;
    }
    if (!paths.has(entry.manifest)) {
      entries.push({id, version: entry.version, status: "unregistered", details: `${entry.manifest} is not in the project's extensions`});
      continue;
    }
    entries.push({id, version: entry.version, status: "ok"});
  }
  const locked = new Set(Object.values(lock.extensions).map((entry) => entry.manifest));
  for (const item of registered)
    if (item.manifestPath.startsWith(`${EXTENSION_INSTALL_FOLDER}/`) && !locked.has(item.manifestPath))
      entries.push({id: item.manifest.id, status: "unlocked", details: `${item.manifestPath} is not in ${EXTENSION_LOCK_FILE}`});
  return {entries, conflicts: projectConflicts(registered)};
}

/** Lock status of each installed package, for `extension list`. */
export async function installedStatus(root: string): Promise<Map<string, {id: string; entry: ExtensionLockEntry; status: VerifyEntry["status"]}>> {
  const lock = await readExtensionLock(root).catch(() => undefined);
  const statuses = new Map<string, {id: string; entry: ExtensionLockEntry; status: VerifyEntry["status"]}>();
  if (lock === undefined) return statuses;
  const verified = await verifyExtensions(root).catch(() => undefined);
  for (const [id, entry] of Object.entries(lock.extensions))
    statuses.set(entry.manifest, {id, entry, status: verified?.entries.find((item) => item.id === id)?.status ?? "ok"});
  return statuses;
}

