// Installing shared extensions: `prismatix extension add | remove | update |
// outdated | verify | inspect`. A package (from a registry, a file, an https
// URL or an npm package) is checked whole before anything is written: its
// SHA-256 and size, the canonical .pxext form (no path traversal, links or
// oversized files), its manifest, its engine range and its command/Action ids
// against the project's other extensions. It is then extracted into
// Extensions/<owner>/<name>/ (committed with the project), registered in the
// project's `extensions` and recorded in prismatix-extensions.lock. Nothing
// from a package runs at install time; its scripts run only inside the
// game's sandboxed JavaScript host, limited to the capabilities it declares.

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
  isVersionRange,
  maxSatisfyingVersion,
  parseExtensionPackageId,
  readExtensionPackage,
  satisfiesVersionRange,
  validateDocument,
  type AuthoringDiagnostic,
  type ExtensionManifest,
  type ExtensionPackageContents,
  type ExtensionPackageMetadata,
} from "@prismatix/authoring-sdk";

import {extractTarGz} from "./archive.js";
import {integrityMatches, registryUrl} from "./platform-runtime.js";
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
  /** extensionFilesDigest of Extensions/<owner>/<name>/. */
  readonly files: string;
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
    ordered[id] = {version: entry.version, sha256: entry.sha256, size: entry.size, source: entry.source,
      ...(entry.range === undefined ? {} : {range: entry.range}), manifest: entry.manifest, files: entry.files};
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
}

export interface InstallOptions {
  readonly registry?: string;
  readonly yes?: boolean;
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
  Promise<{bytes: Uint8Array; source: string}> {
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
    return {bytes: new Uint8Array(await readFile(path)), source: `npm:${name}@${version}`};
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
  if (source.kind === "registry") {
    const location = await requireRegistry(root, options.registry);
    const client = new RegistryClient(location.url, options.fetch === undefined ? {} : {fetch: options.fetch});
    const range = source.range ?? "*";
    options.onProgress?.(`Resolving ${source.id}@${range} for engine ${engine} on ${location.url}`);
    const version: Located<RegistryVersion> = await client.resolve(source.id, range, engine);
    options.onProgress?.(`Downloading ${source.id}@${version.value.version}`);
    const bytes = await client.download(version);
    const contents = checkedPackage(bytes, `${source.id}@${version.value.version}`);
    if (contents.metadata!.id !== source.id || contents.metadata!.version !== version.value.version)
      throw new Error(`The registry served ${contents.metadata!.id}@${contents.metadata!.version} for ${source.id}@${version.value.version}; nothing was installed`);
    if (version.value.yanked) notes.push(`${source.id}@${version.value.version} is yanked${version.value.yankedReason
      ? `: ${version.value.yankedReason}` : ""}; it installs only because it is pinned exactly`);
    return {bytes, sha256: version.value.sha256, contents, source: `registry+${location.url}`,
      range: source.range ?? `^${version.value.version}`,
      ...(version.value.yanked ? {yanked: version.value.yankedReason ?? "yanked"} : {}), notes};
  }
  if (source.kind === "file") {
    const path = resolve(source.path);
    const info = await lstat(path).catch(() => undefined);
    if (info === undefined || !info.isFile()) throw new Error(`${source.path} is not a file`);
    if (info.size > extensionPackageLimits.packageBytes)
      throw new Error(`${source.path} is larger than ${extensionPackageLimits.packageBytes} bytes`);
    const bytes = new Uint8Array(await readFile(path));
    pinned(bytes);
    return {bytes, sha256: sha256(bytes), contents: checkedPackage(bytes, source.path), source: fileSource(root, path), notes};
  }
  if (source.kind === "url") {
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
    return {bytes, sha256: sha256(bytes), contents: checkedPackage(bytes, url.href), source: url.href, notes};
  }
  const npm = await fetchNpm(source.name, source.range, options);
  pinned(npm.bytes);
  return {bytes: npm.bytes, sha256: sha256(npm.bytes), contents: checkedPackage(npm.bytes, npm.source), source: npm.source,
    ...(source.range === undefined ? {} : {range: source.range}), notes};
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
    for (const command of manifest.commands) if (theirs.has(command.id)) conflicts.push({id: command.id, kind: "command", with: other.manifestPath});
    for (const action of manifest.actions) if (theirs.has(action.id)) conflicts.push({id: action.id, kind: "action", with: other.manifestPath});
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
};

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
  readonly yanked?: string;
  readonly installed?: {readonly version: string; readonly sha256: string};
  readonly conflicts: readonly ExtensionConflict[];
  readonly warnings: readonly AuthoringDiagnostic[];
  readonly notes: readonly string[];
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
  return {id: metadata.id, version: metadata.version!, sha256: fetched.sha256, size: fetched.bytes.length, source: fetched.source,
    ...(metadata.displayName === undefined ? {} : {displayName: metadata.displayName}),
    ...(metadata.description === undefined || metadata.description === "" ? {} : {description: metadata.description}),
    ...(metadata.license === undefined ? {} : {license: metadata.license}),
    ...(metadata.author === undefined ? {} : {author: metadata.author.name}),
    requiredEngineVersion: manifest.requiredEngineVersion, engineVersion,
    compatible: satisfiesVersionRange(engineVersion, manifest.requiredEngineVersion, {includePrerelease: true}) === true,
    capabilities: manifest.capabilities, commands: manifest.commands.map((item) => item.id),
    actions: manifest.actions.map((item) => item.id),
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
    "  It may:",
    ...summary.capabilities.map((capability) => `    - ${capability}: ${capabilityDescriptions[capability] ?? capability}`),
    `  It adds ${summary.commands.length} Story command(s)${summary.commands.length ? `: ${summary.commands.join(", ")}` : ""}`,
    `  and ${summary.actions.length} UI Action(s)${summary.actions.length ? `: ${summary.actions.join(", ")}` : ""}.`,
    "  Nothing runs now; its scripts run only in the game's sandbox, limited to these capabilities.",
    ...(summary.installed ? [`  Replaces the installed ${summary.installed.version}.`] : []),
    ...summary.notes.map((note) => `  Note: ${note}`),
  ].join("\n");
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
  try {
    for (const [path, data] of fetched.contents.files) {
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
    const entry: ExtensionLockEntry = {version: metadata.version!, sha256: fetched.sha256, size: fetched.bytes.length,
      source: fetched.source, ...(fetched.range === undefined ? {} : {range: fetched.range}), manifest: manifestPath,
      files: await extensionFilesDigest(fetched.contents.files)};
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
  readonly status: "updated" | "current" | "skipped";
  readonly reason?: string;
}

export async function updateExtensions(root: string, id: string | undefined, options: InstallOptions & {readonly to?: string} = {}):
  Promise<UpdateOutcome[]> {
  const lock = await readExtensionLock(root);
  if (id !== undefined && lock.extensions[id] === undefined) throw new Error(`${EXTENSION_LOCK_FILE} lists no ${id}`);
  if (options.to !== undefined && id === undefined) throw new Error("--to needs the id of the extension to update");
  if (options.to !== undefined && !isVersionRange(options.to)) throw new Error(`Not a version range: ${options.to}`);
  const outcomes: UpdateOutcome[] = [];
  for (const [name, entry] of Object.entries(lock.extensions)) {
    if (id !== undefined && name !== id) continue;
    const source = sourceOf(name, entry, options.to);
    if (source === undefined) {
      outcomes.push({id: name, from: entry.version, status: "skipped",
        reason: `installed from ${entry.source}; add the newer file or URL again to update it`});
      continue;
    }
    const {sha256: _pin, ...unpinned} = options;
    const fetched = await fetchExtensionPackage(root, source, unpinned);
    const next = fetched.contents.metadata!.version!;
    if (fetched.sha256 === entry.sha256 && options.to === undefined) {
      outcomes.push({id: name, from: entry.version, status: "current"});
      continue;
    }
    if (compareSemVer(next, entry.version) < 0 && options.to === undefined) {
      outcomes.push({id: name, from: entry.version, status: "current", reason: `the range resolves to the older ${next}`});
      continue;
    }
    // New capabilities need the same confirmation as an install.
    const installed = await readFile(join(root, ...entry.manifest.split("/")), "utf8")
      .then((text) => JSON.parse(text) as ExtensionManifest).catch(() => undefined);
    const added = fetched.contents.manifest!.capabilities.filter((item) => !installed?.capabilities.includes(item));
    const yes = options.yes === true || added.length === 0;
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
        details: `the files in ${installFolder(id)} differ from what was installed; run prismatix extension add ${id}@${entry.version} --yes to restore them`});
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

