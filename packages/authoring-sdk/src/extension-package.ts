// .pxext extension packages: the one file an extension is shared as.
//
// A package is a ZIP archive in one canonical form, so the same files always
// give the same bytes (and SHA-256): every entry is a regular file, stored
// without compression, in path order, with no extra fields, comments, data
// descriptors or timestamps. Readers accept only that form, which also rules
// out zip bombs, symbolic links, overlapping entries and hidden data.
//
// This module is pure JavaScript (Uint8Array, DataView, TextDecoder and Web
// Crypto; no Node.js APIs), so a registry can run `validateExtensionPackage`
// on uploads server-side, in Node.js or an edge runtime.
//
// docs/extensions-sharing.md specifies the format and the registry API.

import {validateContractDefinition, validateDocument} from "./contracts.js";
import type {AuthoringDiagnostic, ExtensionManifest, ExtensionPlatform, NativeExtensionPlatform} from "./types.js";
import {compareSemVer, isExactVersion, isVersionRange, parseSemVer, satisfiesVersionRange} from "./version-range.js";

export const EXTENSION_PACKAGE_FORMAT = "PrismatiXExtensionPackage";
/** The package metadata file, beside an extension's manifest and at the root of a package. */
export const EXTENSION_PACKAGE_FILE = "pxext.json";
export const EXTENSION_PACKAGE_SUFFIX = ".pxext";
/** The lock file `prismatix extension add` writes at the project root. */
export const EXTENSION_LOCK_FILE = "prismatix-extensions.lock";
/** Where installed packages live in a project: `Extensions/<owner>/<name>/`. */
export const EXTENSION_INSTALL_FOLDER = "Extensions";

export const extensionPackageLimits = {
  /** The whole .pxext file. */
  packageBytes: 32 * 1024 * 1024,
  entries: 2048,
  pathLength: 200,
  segmentLength: 100,
  metadataBytes: 64 * 1024,
  manifestBytes: 1024 * 1024,
  /** All of dist/ together. */
  distBytes: 16 * 1024 * 1024,
  iconBytes: 256 * 1024,
  screenshotBytes: 2 * 1024 * 1024,
  screenshots: 8,
} as const;

export interface ExtensionPackageAuthor {
  readonly name: string;
  readonly email?: string;
  readonly url?: string;
}

/** pxext.json: who publishes an extension and how a registry shows it. */
export interface ExtensionPackageMetadata {
  readonly format: "PrismatiXExtensionPackage";
  readonly formatVersion: 1;
  /** `owner/name`; the name is the manifest's id. */
  readonly id: string;
  /** Required in a package (pack copies the manifest's version). */
  readonly version?: string;
  /** The manifest file at the package root, `<name>.pxextension` (pack fills it). */
  readonly manifest?: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly author?: ExtensionPackageAuthor;
  /** An SPDX license expression, e.g. `MIT`. */
  readonly license?: string;
  readonly homepage?: string;
  readonly repository?: string;
  readonly keywords?: readonly string[];
  /** `icon.png` in a package: a PNG of at most 256 KiB, ideally 256x256. */
  readonly icon?: string;
  /** `screenshots/<file>` in a package: PNG, JPEG or WebP, at most 8. */
  readonly screenshots?: readonly string[];
}

export interface ExtensionPackageFile {
  readonly path: string;
  readonly data: Uint8Array;
}

// Identifiers ----------------------------------------------------------------

const ownerPattern = /^[a-z0-9](?:[a-z0-9]|-(?=[a-z0-9])){0,38}$/u;
const namePattern = /^[a-z0-9][a-z0-9._-]{0,63}$/u;

/** `owner/name` split into its parts, or undefined when it is not a package id. */
export function parseExtensionPackageId(id: string): {readonly owner: string; readonly name: string} | undefined {
  const [owner, name, ...rest] = id.split("/");
  if (rest.length > 0 || owner === undefined || name === undefined || !ownerPattern.test(owner) || !namePattern.test(name))
    return undefined;
  return {owner, name};
}

/** The file name `prismatix extension pack` writes: `<owner>-<name>-<version>.pxext`. */
export function extensionPackageFileName(id: string, version: string): string {
  const parsed = parseExtensionPackageId(id);
  if (parsed === undefined) throw new RangeError(`Not an extension package id (owner/name): ${id}`);
  return `${parsed.owner}-${parsed.name}-${version}${EXTENSION_PACKAGE_SUFFIX}`;
}

const reservedNames = /^(?:con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\.|$)/iu;

/**
 * A path a package may hold: "/"-separated segments of letters, digits, "_",
 * "-" and "." that do not start with "." or "-", end with ".", or name a
 * Windows device. Every package path extracts to the same file on Windows,
 * macOS and Linux.
 */
export function isSafePackagePath(path: string): boolean {
  if (path.length === 0 || path.length > extensionPackageLimits.pathLength) return false;
  return path.split("/").every((segment) => segment.length <= extensionPackageLimits.segmentLength &&
    /^[A-Za-z0-9_][A-Za-z0-9._-]*$/u.test(segment) && !segment.endsWith(".") && !reservedNames.test(segment));
}

// Hashing --------------------------------------------------------------------

function hex(bytes: ArrayBuffer): string {
  return [...new Uint8Array(bytes)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** Lowercase hex SHA-256 of `bytes` (Web Crypto). */
export async function sha256Hex(bytes: Uint8Array | string): Promise<string> {
  const data = typeof bytes === "string" ? new TextEncoder().encode(bytes) : bytes;
  return hex(await globalThis.crypto.subtle.digest("SHA-256", data as Uint8Array<ArrayBuffer>));
}

/**
 * The digest of an installed package's files that the lock records as
 * `files`: SHA-256 over `<sha256>  <path>\n` lines (sha256sum's format)
 * sorted by path. Any added, removed or changed file changes it.
 */
export async function extensionFilesDigest(files: Iterable<readonly [string, Uint8Array]>): Promise<string> {
  const lines: string[] = [];
  for (const [path, data] of files) lines.push(`${await sha256Hex(data)}  ${path}\n`);
  return sha256Hex(lines.sort(comparePaths).join(""));
}

function comparePaths(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

// Platforms and native libraries ---------------------------------------------
// An extension with `native` libraries is shared as its main package (every
// file but the libraries) plus one native pack per platform holding only
// `native/<platform>/**`. The main package's manifest pins every library and
// file by SHA-256 and carries an ECDSA P-256 signature of each platform's
// descriptor, so a native pack is checked against it before it is written,
// and the Player checks the same signature before it loads a library.

export const extensionPlatforms: readonly ExtensionPlatform[] =
  ["win32-x64", "linux-x64", "darwin-x64", "darwin-arm64", "android-arm64", "web"];
export const nativeExtensionPlatforms: readonly NativeExtensionPlatform[] =
  ["win32-x64", "linux-x64", "darwin-x64", "darwin-arm64"];
/** The PxHostApi major ABI version this engine's Player provides (PX_PLUGIN_ABI_MAJOR). */
export const NATIVE_PLUGIN_ABI = 1;
/** The first line of every signed native descriptor. */
export const NATIVE_SIGNATURE_DOMAIN = "PrismatiX native plugin v1";
/** At most this much per native pack. */
export const nativePackBytes = 64 * 1024 * 1024;

export function isNativeExtensionPlatform(value: string): value is NativeExtensionPlatform {
  return (nativeExtensionPlatforms as readonly string[]).includes(value);
}

/**
 * The platforms an extension runs on: its `platforms`, else the platforms of
 * its native libraries, else every platform.
 */
export function manifestPlatforms(manifest: Pick<ExtensionManifest, "platforms" | "native">): readonly ExtensionPlatform[] {
  if (manifest.platforms !== undefined) return manifest.platforms;
  if (manifest.native !== undefined)
    return nativeExtensionPlatforms.filter((platform) => manifest.native!.libraries[platform] !== undefined);
  return extensionPlatforms;
}

/** True when a manifest (or a registry `platforms` list, undefined meaning all) runs on `platform`. */
export function supportsPlatform(platforms: readonly string[] | undefined, platform: string): boolean {
  return platforms === undefined || platforms.includes(platform);
}

/** The package file name of a platform's native pack: `<owner>-<name>-<version>-native-<platform>.pxext`. */
export function nativePackFileName(id: string, version: string, platform: NativeExtensionPlatform): string {
  return extensionPackageFileName(id, version).replace(/\.pxext$/u, `-native-${platform}${EXTENSION_PACKAGE_SUFFIX}`);
}

/** Every file a platform's native pack holds (the library first), by path relative to the manifest. */
export function nativePlatformFiles(manifest: ExtensionManifest, platform: NativeExtensionPlatform):
  readonly {readonly path: string; readonly sha256?: string}[] {
  const library = manifest.native?.libraries[platform];
  if (library === undefined) return [];
  return [{path: library.path, ...(library.sha256 === undefined ? {} : {sha256: library.sha256})},
    ...[...library.files ?? []].sort((left, right) => comparePaths(left.path, right.path))];
}

/**
 * The text a platform's library signature covers (UTF-8, "\n" after every
 * line). The Player rebuilds it from the manifest it loads, so the signature
 * binds the extension id, version, platform, ABI, services and every file's
 * SHA-256 together:
 *
 *     PrismatiX native plugin v1
 *     id <manifest id>
 *     version <version>
 *     platform <platform>
 *     abi <abi>
 *     services <services, sorted, comma-separated>
 *     file <sha256> <path>          (the library, then its files by path)
 */
export function nativeSignatureMessage(manifest: ExtensionManifest, platform: NativeExtensionPlatform): string {
  const native = manifest.native;
  if (native === undefined || native.libraries[platform] === undefined)
    throw new RangeError(`${manifest.id} has no native library for ${platform}`);
  const files = nativePlatformFiles(manifest, platform);
  for (const file of files)
    if (file.sha256 === undefined) throw new RangeError(`${file.path} has no sha256; pack fills it`);
  return [NATIVE_SIGNATURE_DOMAIN, `id ${manifest.id}`, `version ${manifest.version}`, `platform ${platform}`,
    `abi ${native.abi}`, `services ${[...native.services].sort().join(",")}`,
    ...files.map((file) => `file ${file.sha256} ${file.path}`)].map((line) => `${line}\n`).join("");
}

function hexBytes(value: string): Uint8Array {
  const bytes = new Uint8Array(value.length / 2);
  for (let index = 0; index < bytes.length; index += 1) bytes[index] = Number.parseInt(value.slice(index * 2, index * 2 + 2), 16);
  return bytes;
}

/** True when `signature` (hex r||s) is the ECDSA P-256 SHA-256 signature of `message` by `publicKey` (hex SEC1). */
export async function verifyNativeSignature(message: string, signature: string, publicKey: string): Promise<boolean> {
  if (!/^[0-9a-f]{128}$/u.test(signature) || !/^04[0-9a-f]{128}$/u.test(publicKey)) return false;
  try {
    const key = await globalThis.crypto.subtle.importKey("raw", hexBytes(publicKey) as Uint8Array<ArrayBuffer>,
      {name: "ECDSA", namedCurve: "P-256"}, false, ["verify"]);
    return await globalThis.crypto.subtle.verify({name: "ECDSA", hash: "SHA-256"}, key,
      hexBytes(signature) as Uint8Array<ArrayBuffer>, new TextEncoder().encode(message));
  } catch { return false; }
}

/** SHA-256 of a native signing public key: what the lock records and installs show. */
export async function nativeKeyFingerprint(publicKey: string): Promise<string> {
  return sha256Hex(hexBytes(publicKey));
}

/**
 * Checks a manifest's `platforms` and `native` fields. `packaged` also
 * requires what pack fills: the public key and every SHA-256 and signature.
 */
export function nativeManifestDiagnostics(manifest: ExtensionManifest, path: string, packaged: boolean): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const native = manifest.native;
  const declared = manifest.platforms;
  const hasCapability = manifest.capabilities.includes("native");
  if (native === undefined) {
    if (hasCapability)
      diagnostics.push(error("PXEXTNAT1001", "The \"native\" capability needs a \"native\" section with the libraries", path));
    return diagnostics;
  }
  if (!hasCapability)
    diagnostics.push(error("PXEXTNAT1001", "An extension with native libraries declares the \"native\" capability", path));
  if (native.abi !== NATIVE_PLUGIN_ABI)
    diagnostics.push(error("PXEXTNAT1002", `native.abi ${native.abi} is not the ABI this engine provides (${NATIVE_PLUGIN_ABI})`, path));
  const libraries = nativeExtensionPlatforms.filter((platform) => native.libraries[platform] !== undefined);
  if (declared !== undefined) {
    if (declared.includes("web"))
      diagnostics.push(error("PXEXTNAT1003", "An extension with native libraries cannot run on the Web; remove \"web\" from platforms", path));
    for (const platform of declared)
      if (platform !== "web" && !libraries.includes(platform as NativeExtensionPlatform))
        diagnostics.push(error("PXEXTNAT1003", `platforms lists ${platform}, which has no native library`, path, platform));
    for (const platform of libraries)
      if (!declared.includes(platform))
        diagnostics.push(error("PXEXTNAT1003", `native.libraries has ${platform}, which platforms does not list`, path, platform));
  }
  for (const platform of libraries) {
    const library = native.libraries[platform]!;
    const files = nativePlatformFiles(manifest, platform);
    const folder = `native/${platform}/`;
    const seen = new Set<string>();
    for (const file of files) {
      if (!file.path.startsWith(folder))
        diagnostics.push(error("PXEXTNAT1004", `Native files of ${platform} live in ${folder}`, path, file.path));
      if (seen.has(file.path.toLowerCase())) diagnostics.push(error("PXEXTNAT1004", "A native file is listed twice", path, file.path));
      seen.add(file.path.toLowerCase());
      if (packaged && file.sha256 === undefined)
        diagnostics.push(error("PXEXTNAT1005", "A packaged native file has its sha256", path, file.path));
    }
    const expected = platform === "win32-x64" ? ".dll" : platform.startsWith("darwin") ? ".dylib" : ".so";
    if (!library.path.endsWith(expected))
      diagnostics.push(error("PXEXTNAT1004", `The ${platform} library is a ${expected} file`, path, library.path));
    if (packaged && library.signature === undefined)
      diagnostics.push(error("PXEXTNAT1005", `The ${platform} library of a package is signed (pack --signing-key)`, path, library.path));
  }
  if (packaged && native.publicKey === undefined)
    diagnostics.push(error("PXEXTNAT1005", "A package with native libraries names its signing publicKey", path));
  return diagnostics;
}

/** Checks every platform's signature in a packaged manifest. */
export async function nativeSignatureDiagnostics(manifest: ExtensionManifest, path: string): Promise<AuthoringDiagnostic[]> {
  const native = manifest.native;
  if (native?.publicKey === undefined) return [];
  const diagnostics: AuthoringDiagnostic[] = [];
  for (const platform of nativeExtensionPlatforms) {
    const library = native.libraries[platform];
    if (library?.signature === undefined) continue;
    let message: string;
    try { message = nativeSignatureMessage(manifest, platform); } catch { continue; }
    if (!await verifyNativeSignature(message, library.signature, native.publicKey))
      diagnostics.push(error("PXEXTNAT1006", `The ${platform} library's signature does not verify with the manifest's publicKey`, path, library.path));
  }
  return diagnostics;
}

// ZIP ------------------------------------------------------------------------

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) value = (value & 1) !== 0 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    table[index] = value >>> 0;
  }
  return table;
})();

function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (let index = 0; index < data.length; index += 1) crc = crcTable[(crc ^ data[index]!) & 0xff]! ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

const localSignature = 0x04034b50;
const centralSignature = 0x02014b50;
const endSignature = 0x06054b50;
const utf8Flag = 0x0800;
// 1980-01-01 00:00:00, the earliest DOS time: packages carry no timestamps.
const dosTime = 0;
const dosDate = (0 << 9) | (1 << 5) | 1;
const regularFileAttributes = (0o100644 << 16) >>> 0;

/**
 * Writes files as a canonical .pxext ZIP: sorted by path, stored, with fixed
 * times and permissions. The same files always give the same bytes.
 */
export function writeExtensionPackage(files: readonly ExtensionPackageFile[]): Uint8Array {
  const sorted = [...files].sort((left, right) => comparePaths(left.path, right.path));
  const encoder = new TextEncoder();
  const seen = new Set<string>();
  for (const file of sorted) {
    if (!isSafePackagePath(file.path)) throw new RangeError(`Unsafe package path: ${file.path}`);
    const folded = file.path.toLowerCase();
    if (seen.has(folded)) throw new RangeError(`Duplicate package path (ignoring case): ${file.path}`);
    seen.add(folded);
  }
  if (sorted.length > extensionPackageLimits.entries) throw new RangeError(`A package holds at most ${extensionPackageLimits.entries} files`);
  const names = sorted.map((file) => encoder.encode(file.path));
  const crcs = sorted.map((file) => crc32(file.data));
  const localSize = sorted.reduce((total, file, index) => total + 30 + names[index]!.length + file.data.length, 0);
  const centralSize = names.reduce((total, name) => total + 46 + name.length, 0);
  const output = new Uint8Array(localSize + centralSize + 22);
  const view = new DataView(output.buffer);
  let at = 0;
  const offsets: number[] = [];
  sorted.forEach((file, index) => {
    offsets.push(at);
    view.setUint32(at, localSignature, true);
    view.setUint16(at + 4, 20, true);
    view.setUint16(at + 6, utf8Flag, true);
    view.setUint16(at + 8, 0, true);
    view.setUint16(at + 10, dosTime, true);
    view.setUint16(at + 12, dosDate, true);
    view.setUint32(at + 14, crcs[index]!, true);
    view.setUint32(at + 18, file.data.length, true);
    view.setUint32(at + 22, file.data.length, true);
    view.setUint16(at + 26, names[index]!.length, true);
    view.setUint16(at + 28, 0, true);
    output.set(names[index]!, at + 30);
    output.set(file.data, at + 30 + names[index]!.length);
    at += 30 + names[index]!.length + file.data.length;
  });
  const centralOffset = at;
  sorted.forEach((file, index) => {
    view.setUint32(at, centralSignature, true);
    view.setUint16(at + 4, 0x0314, true); // made by: Unix, ZIP 2.0
    view.setUint16(at + 6, 20, true);
    view.setUint16(at + 8, utf8Flag, true);
    view.setUint16(at + 10, 0, true);
    view.setUint16(at + 12, dosTime, true);
    view.setUint16(at + 14, dosDate, true);
    view.setUint32(at + 16, crcs[index]!, true);
    view.setUint32(at + 20, file.data.length, true);
    view.setUint32(at + 24, file.data.length, true);
    view.setUint16(at + 28, names[index]!.length, true);
    view.setUint16(at + 30, 0, true);
    view.setUint16(at + 32, 0, true);
    view.setUint16(at + 34, 0, true);
    view.setUint16(at + 36, 0, true);
    view.setUint32(at + 38, regularFileAttributes, true);
    view.setUint32(at + 42, offsets[index]!, true);
    output.set(names[index]!, at + 46);
    at += 46 + names[index]!.length;
  });
  view.setUint32(at, endSignature, true);
  view.setUint16(at + 8, sorted.length, true);
  view.setUint16(at + 10, sorted.length, true);
  view.setUint32(at + 12, at - centralOffset, true);
  view.setUint32(at + 16, centralOffset, true);
  return output;
}

class PackageFormatError extends Error {
  constructor(readonly code: string, message: string, readonly details?: string) {
    super(message);
  }
}

/** The files of a canonical .pxext ZIP; throws PackageFormatError for anything else. */
function readZip(bytes: Uint8Array, packageBytes: number = extensionPackageLimits.packageBytes): Map<string, Uint8Array> {
  const limits = extensionPackageLimits;
  if (bytes.length > packageBytes)
    throw new PackageFormatError("PXEXTPKG1002", `The package is larger than ${packageBytes / 1024 / 1024} MiB`);
  if (bytes.length < 22) throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (too short to be a ZIP archive)");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const end = bytes.length - 22;
  if (view.getUint32(end, true) !== endSignature)
    throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (no ZIP end record; archive comments are not allowed)");
  const count = view.getUint16(end + 8, true);
  const centralSize = view.getUint32(end + 12, true);
  const centralOffset = view.getUint32(end + 16, true);
  if (view.getUint16(end + 4, true) !== 0 || view.getUint16(end + 6, true) !== 0 || view.getUint16(end + 10, true) !== count ||
      view.getUint16(end + 20, true) !== 0 || centralOffset + centralSize !== end)
    throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (multi-disk, ZIP64 or trailing data)");
  if (count > limits.entries) throw new PackageFormatError("PXEXTPKG1002", `The package holds more than ${limits.entries} files`);
  const decoder = new TextDecoder("utf-8", {fatal: true});
  const files = new Map<string, Uint8Array>();
  const folded = new Set<string>();
  let at = centralOffset;
  let expectedOffset = 0;
  let previous: string | undefined;
  for (let index = 0; index < count; index += 1) {
    if (at + 46 > end || view.getUint32(at, true) !== centralSignature)
      throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (damaged central directory)");
    const flags = view.getUint16(at + 8, true);
    const method = view.getUint16(at + 10, true);
    const crc = view.getUint32(at + 16, true);
    const compressed = view.getUint32(at + 20, true);
    const size = view.getUint32(at + 24, true);
    const nameLength = view.getUint16(at + 28, true);
    const extraLength = view.getUint16(at + 30, true);
    const commentLength = view.getUint16(at + 32, true);
    const disk = view.getUint16(at + 34, true);
    const attributes = view.getUint32(at + 38, true);
    const offset = view.getUint32(at + 42, true);
    if (at + 46 + nameLength > end) throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (damaged central directory)");
    let name: string;
    try { name = decoder.decode(bytes.subarray(at + 46, at + 46 + nameLength)); }
    catch { throw new PackageFormatError("PXEXTPKG1003", "A file name in the package is not UTF-8"); }
    at += 46 + nameLength + extraLength + commentLength;
    const fileType = (attributes >>> 16) & 0o170000;
    if (name.endsWith("/") || fileType === 0o040000)
      throw new PackageFormatError("PXEXTPKG1005", "The package holds a directory entry; packages hold files only", name);
    if (fileType !== 0 && fileType !== 0o100000)
      throw new PackageFormatError("PXEXTPKG1005", "The package holds a symbolic link or special file", name);
    if (method !== 0 || compressed !== size)
      throw new PackageFormatError("PXEXTPKG1005", "A file in the package is compressed; .pxext stores every file uncompressed", name);
    if ((flags & ~utf8Flag) !== 0)
      throw new PackageFormatError("PXEXTPKG1005", "A file in the package is encrypted or uses a data descriptor", name);
    if (extraLength !== 0 || commentLength !== 0 || disk !== 0)
      throw new PackageFormatError("PXEXTPKG1005", "A file in the package carries ZIP extra fields or a comment", name);
    if (!isSafePackagePath(name)) throw new PackageFormatError("PXEXTPKG1003", "Unsafe file path in the package", name);
    if (previous !== undefined && comparePaths(previous, name) >= 0)
      throw new PackageFormatError("PXEXTPKG1014", "The package's files are not in path order (it was not written by prismatix extension pack)", name);
    previous = name;
    const lower = name.toLowerCase();
    if (folded.has(lower)) throw new PackageFormatError("PXEXTPKG1004", "Two files in the package differ only in case", name);
    folded.add(lower);
    // Entries are contiguous and in central-directory order: nothing hides between them.
    if (offset !== expectedOffset || offset + 30 > centralOffset || view.getUint32(offset, true) !== localSignature)
      throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (entries overlap or leave gaps)", name);
    const localNameLength = view.getUint16(offset + 26, true);
    if (view.getUint16(offset + 6, true) !== flags || view.getUint16(offset + 8, true) !== 0 ||
        view.getUint32(offset + 14, true) !== crc || view.getUint32(offset + 18, true) !== size ||
        view.getUint32(offset + 22, true) !== size || localNameLength !== nameLength || view.getUint16(offset + 28, true) !== 0)
      throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (local and central records disagree)", name);
    for (let byte = 0; byte < nameLength; byte += 1)
      if (bytes[offset + 30 + byte] !== bytes[at - nameLength - extraLength - commentLength + byte])
        throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (local and central names disagree)", name);
    const start = offset + 30 + nameLength;
    if (start + size > centralOffset) throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (a file runs past the archive)", name);
    const data = bytes.subarray(start, start + size);
    if (crc32(data) !== crc) throw new PackageFormatError("PXEXTPKG1006", "A file in the package is damaged (CRC mismatch)", name);
    files.set(name, data);
    expectedOffset = start + size;
  }
  if (at !== end || expectedOffset !== centralOffset)
    throw new PackageFormatError("PXEXTPKG1001", "Not a .pxext package (unaccounted bytes in the archive)");
  return files;
}

// Validation -----------------------------------------------------------------

export interface ExtensionPackageContents {
  /** Every file, by package path. Empty when the archive itself is invalid. */
  readonly files: ReadonlyMap<string, Uint8Array>;
  readonly metadata?: ExtensionPackageMetadata;
  readonly manifest?: ExtensionManifest;
  /** The manifest's path in the package. */
  readonly manifestPath?: string;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

export interface ExtensionPackageValidation extends ExtensionPackageContents {
  readonly sha256: string;
  readonly size: number;
  /** `owner/name`, when pxext.json is valid. */
  readonly id?: string;
  readonly version?: string;
}

function error(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message, ...(path === undefined ? {} : {path}), ...(details === undefined ? {} : {details})};
}

function warning(code: string, message: string, path?: string, hint?: string): AuthoringDiagnostic {
  return {severity: "warning", code, message, ...(path === undefined ? {} : {path}), ...(hint === undefined ? {} : {hint})};
}

function json(data: Uint8Array, path: string, diagnostics: AuthoringDiagnostic[], code: string): unknown {
  let text: string;
  try { text = new TextDecoder("utf-8", {fatal: true}).decode(data); }
  catch { diagnostics.push(error(code, "Not UTF-8 text", path)); return undefined; }
  if (text.startsWith("﻿")) { diagnostics.push(error(code, "JSON must not start with a byte order mark", path)); return undefined; }
  try { return JSON.parse(text) as unknown; }
  catch (cause) { diagnostics.push(error(code, "Not valid JSON", path, String(cause))); return undefined; }
}

const imageSignatures: Readonly<Record<string, readonly number[][]>> = {
  png: [[0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]],
  jpg: [[0xff, 0xd8, 0xff]],
  jpeg: [[0xff, 0xd8, 0xff]],
  webp: [[0x52, 0x49, 0x46, 0x46]],
};

function imageKind(path: string, data: Uint8Array): string | undefined {
  const extension = path.slice(path.lastIndexOf(".") + 1).toLowerCase();
  const signatures = imageSignatures[extension];
  if (signatures === undefined) return undefined;
  const matches = signatures.some((signature) => signature.every((byte, index) => data[index] === byte));
  if (!matches) return undefined;
  if (extension === "webp" && String.fromCharCode(...data.subarray(8, 12)) !== "WEBP") return undefined;
  return extension;
}

const rootFiles = new Set(["README.md", "LICENSE", "LICENSE.md", "LICENSE.txt", "CHANGELOG.md"]);

/**
 * Reads and checks a .pxext package without hashing it: the archive form,
 * pxext.json, the manifest, the files a package may hold and the size limits.
 * The files map is what an installer extracts.
 */
export function readExtensionPackage(bytes: Uint8Array): ExtensionPackageContents {
  let files: Map<string, Uint8Array>;
  try { files = readZip(bytes); }
  catch (cause) {
    if (!(cause instanceof PackageFormatError)) throw cause;
    return {files: new Map(), diagnostics: [error(cause.code, cause.message, cause.details)], valid: false};
  }
  const diagnostics: AuthoringDiagnostic[] = [];
  const limits = extensionPackageLimits;

  const metadataData = files.get(EXTENSION_PACKAGE_FILE);
  let metadata: ExtensionPackageMetadata | undefined;
  if (metadataData === undefined) diagnostics.push(error("PXEXTPKG1007", `The package has no ${EXTENSION_PACKAGE_FILE}`));
  else if (metadataData.length > limits.metadataBytes)
    diagnostics.push(error("PXEXTPKG1002", `${EXTENSION_PACKAGE_FILE} is larger than ${limits.metadataBytes / 1024} KiB`, EXTENSION_PACKAGE_FILE));
  else {
    const value = json(metadataData, EXTENSION_PACKAGE_FILE, diagnostics, "PXEXTPKG1007");
    if (value !== undefined) {
      const result = validateDocument<ExtensionPackageMetadata>("extensionPackage", value, EXTENSION_PACKAGE_FILE);
      if (!result.valid) diagnostics.push(...result.diagnostics.map((item) =>
        error("PXEXTPKG1008", `${EXTENSION_PACKAGE_FILE} is not valid package metadata`, EXTENSION_PACKAGE_FILE, item.details === undefined ? item.message : `${item.message}: ${item.details}`)));
      else {
        metadata = result.value!;
        if (metadata.version === undefined || metadata.manifest === undefined)
          diagnostics.push(error("PXEXTPKG1008", `${EXTENSION_PACKAGE_FILE} in a package must name its version and manifest`, EXTENSION_PACKAGE_FILE));
      }
    }
  }

  const name = metadata === undefined ? undefined : parseExtensionPackageId(metadata.id)?.name;
  const manifestPath = metadata?.manifest;
  let manifest: ExtensionManifest | undefined;
  if (metadata !== undefined && manifestPath !== undefined && name !== undefined) {
    if (manifestPath !== `${name}.pxextension`)
      diagnostics.push(error("PXEXTPKG1009", `The manifest must be ${name}.pxextension at the package root`, EXTENSION_PACKAGE_FILE, manifestPath));
    const data = files.get(manifestPath);
    if (data === undefined) diagnostics.push(error("PXEXTPKG1009", `The package has no ${manifestPath}`, manifestPath));
    else if (data.length > limits.manifestBytes)
      diagnostics.push(error("PXEXTPKG1002", `${manifestPath} is larger than ${limits.manifestBytes / 1024 / 1024} MiB`, manifestPath));
    else {
      const value = json(data, manifestPath, diagnostics, "PXEXTPKG1009");
      if (value !== undefined) {
        const result = validateDocument<ExtensionManifest>("extension", value, manifestPath);
        if (!result.valid) diagnostics.push(...result.diagnostics.map((item) =>
          error("PXEXTPKG1009", "The manifest is not a valid extension manifest", manifestPath, item.details === undefined ? item.message : `${item.message}: ${item.details}`)));
        else manifest = result.value!;
      }
    }
  }

  const referenced = new Set<string>();
  if (manifest !== undefined && metadata !== undefined && name !== undefined) {
    if (manifest.id !== name)
      diagnostics.push(error("PXEXTPKG1010", `The manifest id ${manifest.id} is not the package name ${name}`, manifestPath));
    if (manifest.version !== metadata.version || parseSemVer(manifest.version) === undefined ||
        /^[vV]/u.test(manifest.version))
      diagnostics.push(error("PXEXTPKG1010", `The manifest version ${manifest.version} is not the package version ${metadata.version}`, manifestPath));
    if (manifest.language !== "javascript")
      diagnostics.push(error("PXEXTPKG1010", "A packaged manifest's language is javascript (pack bundles TypeScript)", manifestPath));
    if (!isVersionRange(manifest.requiredEngineVersion))
      diagnostics.push(error("PXEXTPKG1013", "requiredEngineVersion is not a SemVer range", manifestPath, manifest.requiredEngineVersion));
    for (const script of [manifest.entry, ...manifest.modules ?? []]) {
      referenced.add(script);
      if (!script.startsWith("dist/") || !script.endsWith(".js"))
        diagnostics.push(error("PXEXTPKG1010", "The entry and modules of a packaged manifest are .js files in dist/", manifestPath, script));
      else if (!files.has(script)) diagnostics.push(error("PXEXTPKG1010", `The package has no ${script}`, manifestPath, script));
    }
    diagnostics.push(...nativeManifestDiagnostics(manifest, manifestPath!, true));
  }

  let distBytes = 0;
  const screenshots = new Set(metadata?.screenshots ?? []);
  for (const [path, data] of files) {
    if (path === EXTENSION_PACKAGE_FILE || path === manifestPath || rootFiles.has(path) || path.startsWith("src/")) continue;
    if (path.startsWith("dist/") && path.endsWith(".js")) {
      distBytes += data.length;
      if (manifest !== undefined && !referenced.has(path))
        diagnostics.push(warning("PXEXTPKG2004", "A script in dist/ is not the manifest's entry or one of its modules", path,
          "The Player loads only the entry and the listed modules."));
      continue;
    }
    if (path.startsWith("types/") && path.endsWith(".d.ts")) continue;
    if (path === "icon.png" && metadata?.icon === path) {
      if (data.length > limits.iconBytes || imageKind(path, data) !== "png")
        diagnostics.push(error("PXEXTPKG1012", `icon.png must be a PNG of at most ${limits.iconBytes / 1024} KiB`, path));
      continue;
    }
    if (path.startsWith("screenshots/") && !path.slice(12).includes("/") && screenshots.has(path)) {
      if (data.length > limits.screenshotBytes || imageKind(path, data) === undefined)
        diagnostics.push(error("PXEXTPKG1012", `A screenshot must be a PNG, JPEG or WebP of at most ${limits.screenshotBytes / 1024 / 1024} MiB`, path));
      continue;
    }
    if (path.startsWith("native/")) {
      diagnostics.push(error("PXEXTPKG1011", "Native libraries go in each platform's native pack, not the main package", path));
      continue;
    }
    diagnostics.push(error("PXEXTPKG1011", "A package holds pxext.json, the manifest, dist/*.js, types/*.d.ts, README.md, " +
      "LICENSE, CHANGELOG.md, the icon and screenshots pxext.json names, and src/; not this file", path));
  }
  if (distBytes > limits.distBytes)
    diagnostics.push(error("PXEXTPKG1002", `dist/ is larger than ${limits.distBytes / 1024 / 1024} MiB`));
  if (metadata?.icon !== undefined && metadata.icon !== "icon.png")
    diagnostics.push(error("PXEXTPKG1012", "A package's icon is icon.png", EXTENSION_PACKAGE_FILE, metadata.icon));
  for (const shot of screenshots)
    if (!shot.startsWith("screenshots/") || !files.has(shot))
      diagnostics.push(error("PXEXTPKG1012", "A listed screenshot is not a file in screenshots/", EXTENSION_PACKAGE_FILE, shot));
  if (metadata?.icon === "icon.png" && !files.has("icon.png"))
    diagnostics.push(error("PXEXTPKG1012", "pxext.json names icon.png but the package has none", EXTENSION_PACKAGE_FILE));
  if (!files.has("README.md"))
    diagnostics.push(warning("PXEXTPKG2001", "The package has no README.md", undefined, "Registries show it as the extension's page."));
  if (!["LICENSE", "LICENSE.md", "LICENSE.txt"].some((path) => files.has(path)))
    diagnostics.push(warning("PXEXTPKG2002", "The package has no LICENSE file"));
  if (metadata !== undefined && metadata.license === undefined)
    diagnostics.push(warning("PXEXTPKG2003", `${EXTENSION_PACKAGE_FILE} names no license`, EXTENSION_PACKAGE_FILE,
      "Add an SPDX expression such as \"MIT\" so users know how they may use it."));

  return {files, ...(metadata === undefined ? {} : {metadata}), ...(manifest === undefined ? {} : {manifest}),
    ...(manifestPath === undefined ? {} : {manifestPath}), diagnostics,
    valid: !diagnostics.some((item) => item.severity === "error")};
}

/**
 * Checks a .pxext package as `prismatix extension add` and a registry do:
 * the canonical archive form, safe paths, pxext.json and the manifest against
 * their contracts, the files a package may hold and the size limits. Never
 * runs anything from the package.
 */
export async function validateExtensionPackage(bytes: Uint8Array): Promise<ExtensionPackageValidation> {
  let contents = readExtensionPackage(bytes);
  if (contents.manifest !== undefined && contents.manifestPath !== undefined) {
    const signatures = await nativeSignatureDiagnostics(contents.manifest, contents.manifestPath);
    if (signatures.length > 0) contents = {...contents, diagnostics: [...contents.diagnostics, ...signatures], valid: false};
  }
  return {...contents, sha256: await sha256Hex(bytes), size: bytes.length,
    ...(contents.metadata === undefined ? {} : {id: contents.metadata.id}),
    ...(contents.metadata?.version === undefined ? {} : {version: contents.metadata.version})};
}

export interface ExtensionNativePack {
  readonly platform: NativeExtensionPlatform;
  /** Every file, by package path (`native/<platform>/...`). Empty when invalid. */
  readonly files: ReadonlyMap<string, Uint8Array>;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

/**
 * Reads a platform's native pack against the main package's manifest: the
 * canonical archive form, exactly the files the manifest lists for that
 * platform, each with the SHA-256 the manifest pins.
 */
export async function readExtensionNativePack(bytes: Uint8Array, manifest: ExtensionManifest,
  platform: NativeExtensionPlatform): Promise<ExtensionNativePack> {
  const failed = (diagnostic: AuthoringDiagnostic): ExtensionNativePack =>
    ({platform, files: new Map(), diagnostics: [diagnostic], valid: false});
  if (bytes.length > nativePackBytes)
    return failed(error("PXEXTPKG1002", `A native pack is at most ${nativePackBytes / 1024 / 1024} MiB`));
  let files: Map<string, Uint8Array>;
  try { files = readZip(bytes, nativePackBytes); }
  catch (cause) {
    if (!(cause instanceof PackageFormatError)) throw cause;
    return failed(error(cause.code, cause.message, cause.details));
  }
  const expected = nativePlatformFiles(manifest, platform);
  if (expected.length === 0) return failed(error("PXEXTPKG1015", `${manifest.id} has no native library for ${platform}`));
  const diagnostics: AuthoringDiagnostic[] = [];
  const listed = new Set(expected.map((file) => file.path));
  for (const path of files.keys())
    if (!listed.has(path)) diagnostics.push(error("PXEXTPKG1015", `The ${platform} native pack holds a file its manifest does not list`, path));
  for (const file of expected) {
    const data = files.get(file.path);
    if (data === undefined) diagnostics.push(error("PXEXTPKG1015", `The ${platform} native pack has no ${file.path}`, file.path));
    else if (file.sha256 === undefined || await sha256Hex(data) !== file.sha256)
      diagnostics.push(error("PXEXTPKG1016", `${file.path} does not match the SHA-256 the manifest pins`, file.path));
  }
  const valid = diagnostics.length === 0;
  return {platform, files: valid ? files : new Map(), diagnostics, valid};
}

// Registry -------------------------------------------------------------------
// The JSON bodies of the registry HTTP API v1 (Contracts/extension-registry.schema.json).

export interface RegistrySearchResult {
  readonly id: string;
  readonly owner?: string;
  readonly name?: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly latestVersion: string;
  readonly requiredEngineVersion: string;
  readonly capabilities: readonly string[];
  /** Absent: every platform. */
  readonly platforms?: readonly ExtensionPlatform[];
  /** True when it loads native libraries (installing needs --allow-native). */
  readonly native?: boolean;
  readonly keywords?: readonly string[];
  readonly icon?: string;
  readonly downloads?: number;
  readonly updatedAt?: string;
}

export interface RegistrySearchResponse {
  readonly query?: string;
  readonly results: readonly RegistrySearchResult[];
  readonly total: number;
  readonly page: number;
  readonly pageSize: number;
}

export interface RegistryVersionSummary {
  readonly version: string;
  readonly requiredEngineVersion: string;
  readonly yanked: boolean;
  readonly sha256?: string;
  readonly size?: number;
  /** Absent: every platform. */
  readonly platforms?: readonly ExtensionPlatform[];
  readonly publishedAt?: string;
}

export interface RegistryExtension {
  readonly id: string;
  readonly owner?: string;
  readonly name?: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly author?: ExtensionPackageAuthor;
  readonly license?: string;
  readonly homepage?: string;
  readonly repository?: string;
  readonly keywords?: readonly string[];
  readonly icon?: string;
  readonly screenshots?: readonly string[];
  readonly capabilities: readonly string[];
  readonly platforms?: readonly ExtensionPlatform[];
  readonly native?: boolean;
  readonly latestVersion?: string | null;
  /** Newest first. */
  readonly versions: readonly RegistryVersionSummary[];
  readonly downloads?: number;
  readonly createdAt?: string;
  readonly updatedAt?: string;
}

export interface RegistryVersion {
  readonly id?: string;
  readonly version: string;
  readonly requiredEngineVersion: string;
  readonly sha256: string;
  readonly size: number;
  /** The .pxext URL, absolute or relative to this document. */
  readonly download: string;
  readonly manifest: ExtensionManifest;
  readonly metadata?: ExtensionPackageMetadata;
  readonly platforms?: readonly ExtensionPlatform[];
  /** The native pack of each platform (absolute URLs, or relative to this document). */
  readonly platformDownloads?: Readonly<Partial<Record<NativeExtensionPlatform, RegistryPlatformDownload>>>;
  readonly yanked: boolean;
  readonly yankedReason?: string;
  readonly publishedAt?: string;
}

export interface RegistryPlatformDownload {
  readonly download: string;
  readonly sha256: string;
  readonly size: number;
}

export interface RegistryPublishRequest {
  readonly id: string;
  readonly version: string;
  readonly sha256: string;
  readonly size: number;
  readonly manifest: ExtensionManifest;
  readonly metadata: ExtensionPackageMetadata;
  /** The native packs uploaded with the main package. */
  readonly platformPackages?: Readonly<Partial<Record<NativeExtensionPlatform, {readonly sha256: string; readonly size: number}>>>;
}

export interface RegistryUpload {
  readonly uploadUrl: string;
  readonly uploadMethod?: "PUT";
  readonly uploadHeaders?: Readonly<Record<string, string>>;
}

export interface RegistryPublishResponse extends RegistryUpload {
  readonly uploadId: string;
  readonly expiresAt?: string;
  /** Where each announced native pack goes. */
  readonly platformUploads?: Readonly<Partial<Record<NativeExtensionPlatform, RegistryUpload>>>;
}

export interface RegistryError {
  readonly error: {readonly code: string; readonly message: string};
}

export type RegistryDocumentKind = "searchResponse" | "extension" | "version" | "publishRequest" | "publishResponse" |
  "yankRequest" | "error";

/** Checks a registry JSON body against Contracts/extension-registry.schema.json. */
export function validateRegistryDocument<T>(kind: RegistryDocumentKind, value: unknown, path?: string) {
  return validateContractDefinition<T>(`extension-registry.schema.json#/$defs/${kind}`, value, path);
}

export interface ExtensionVersionResolution<T extends RegistryVersionSummary = RegistryVersionSummary> {
  /** The newest version that matches the range and the engine. */
  readonly version?: T;
  /** Why there is none. */
  readonly problem?: "invalid-range" | "no-match" | "engine" | "platform";
  /**
   * With problem "engine": the versions that match the range but need another
   * engine; with "platform": the ones for this engine that lack the platform.
   */
  readonly incompatible?: readonly T[];
}

/**
 * The `resolve` route's rule, which clients also apply to a static index:
 * the newest version that satisfies `range` and whose requiredEngineVersion
 * accepts `engineVersion`. Yanked versions are left out unless `range` names
 * that exact version; prereleases only match a range that mentions one.
 * With `platform`, only versions that run there (their `platforms` list it,
 * or they list none).
 */
export function resolveExtensionVersion<T extends RegistryVersionSummary>(versions: readonly T[], range: string,
  engineVersion?: string, platform?: string): ExtensionVersionResolution<T> {
  if (!isVersionRange(range)) return {problem: "invalid-range"};
  const exact = isExactVersion(range);
  const matching = versions.filter((item) => satisfiesVersionRange(item.version, range) === true && (exact || !item.yanked));
  if (matching.length === 0) return {problem: "no-match"};
  const compatible = engineVersion === undefined ? matching
    : matching.filter((item) => satisfiesVersionRange(engineVersion, item.requiredEngineVersion, {includePrerelease: true}) === true);
  if (compatible.length === 0) return {problem: "engine", incompatible: matching};
  const supported = platform === undefined ? compatible : compatible.filter((item) => supportsPlatform(item.platforms, platform));
  if (supported.length === 0) return {problem: "platform", incompatible: compatible};
  return {version: supported.reduce((best, item) => compareSemVer(item.version, best.version) > 0 ? item : best)};
}
