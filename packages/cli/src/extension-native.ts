// Native extensions (layer 2): signed C ABI libraries beside an extension's
// manifest, in `native/<platform>/`. This module signs them, checks a
// project's extensions against the platforms a build targets, writes the
// per-platform native packs of a shared extension and copies the libraries
// into a packed distribution (`<data root>/Plugins/<extension id>/`), where
// the Player looks for them. docs/native-extensions.md describes the model.
//
// Signatures are ECDSA P-256 over SHA-256 of the native descriptor
// (`nativeSignatureMessage` in the SDK), raw r||s in hex: the form the
// Player's mbedTLS check and Web Crypto both read.

import {createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign, type KeyObject} from "node:crypto";
import {existsSync} from "node:fs";
import {chmod, copyFile, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {dirname, join} from "node:path";

import {
  manifestPlatforms,
  nativeExtensionPlatforms,
  nativeManifestDiagnostics,
  nativePlatformFiles,
  nativeSignatureMessage,
  verifyNativeSignature,
  type AuthoringDiagnostic,
  type ExtensionManifest,
  type ExtensionNativeLibrary,
  type ExtensionPackageFile,
  type NativeExtensionPlatform,
} from "@prismatix/authoring-sdk";

/** A PEM file with the P-256 private key that signs native libraries. */
export const SIGNING_KEY_ENVIRONMENT = "PRISMATIX_EXTENSION_SIGNING_KEY";
/** Where a project keeps the key that signs its own native libraries for local builds. */
export const DEVELOPMENT_KEY_FILE = ".prismatix/native-development-key.pem";
/** The folder of a distribution's data root that holds native libraries. */
export const NATIVE_PLUGIN_FOLDER = "Plugins";

export interface SigningKey {
  readonly privateKey: KeyObject;
  /** Uncompressed SEC1 point, lowercase hex (65 bytes). */
  readonly publicKey: string;
  /** Where it came from, for messages. */
  readonly source: string;
}

function base64url(value: string): Buffer {
  return Buffer.from(value.replace(/-/gu, "+").replace(/_/gu, "/"), "base64");
}

function publicKeyHex(key: KeyObject): string {
  const jwk = createPublicKey(key).export({format: "jwk"});
  if (jwk.crv !== "P-256" || jwk.x === undefined || jwk.y === undefined) throw new Error("Native signing keys are P-256 (prime256v1) EC keys");
  return `04${base64url(jwk.x).toString("hex").padStart(64, "0")}${base64url(jwk.y).toString("hex").padStart(64, "0")}`;
}

/** A new P-256 key pair; the PEM is the private key (PKCS #8). */
export function generateSigningKey(): {readonly pem: string; readonly publicKey: string} {
  const {privateKey} = generateKeyPairSync("ec", {namedCurve: "prime256v1"});
  return {pem: privateKey.export({format: "pem", type: "pkcs8"}).toString(), publicKey: publicKeyHex(privateKey)};
}

export async function readSigningKey(path: string): Promise<SigningKey> {
  let privateKey: KeyObject;
  try { privateKey = createPrivateKey(await readFile(path)); }
  catch (cause) { throw new Error(`${path} is not a PEM private key: ${cause instanceof Error ? cause.message : String(cause)}`); }
  return {privateKey, publicKey: publicKeyHex(privateKey), source: path};
}

/** Writes a new key file that only its owner can read; refuses to overwrite one. */
export async function writeSigningKey(path: string): Promise<SigningKey> {
  const generated = generateSigningKey();
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, generated.pem, {encoding: "utf8", flag: "wx", mode: 0o600});
  await chmod(path, 0o600).catch(() => undefined);
  return readSigningKey(path);
}

export function signNativeMessage(message: string, key: SigningKey): string {
  return sign("sha256", Buffer.from(message, "utf8"), {key: key.privateKey, dsaEncoding: "ieee-p1363"}).toString("hex");
}

function sha256(data: Uint8Array): string {
  return createHash("sha256").update(data).digest("hex");
}

function manifestFolder(manifestPath: string): string {
  return manifestPath.includes("/") ? manifestPath.slice(0, manifestPath.lastIndexOf("/")) : "";
}

function projectPath(root: string, manifestPath: string, path: string): string {
  const folder = manifestFolder(manifestPath);
  return join(root, ...(folder === "" ? [] : folder.split("/")), ...path.split("/"));
}

function error(code: string, message: string, path: string, hint?: string): AuthoringDiagnostic {
  return {severity: "error", code, message, path, ...(hint === undefined ? {} : {hint})};
}

/** How a build signs authored native libraries. */
export type NativeSigning =
  /** Check only (validate): fill hashes, sign nothing, write nothing. */
  | {readonly mode: "check"}
  /** Sign with this key (pack --signing-key, PRISMATIX_EXTENSION_SIGNING_KEY or the project's development key). */
  | {readonly mode: "sign"; readonly key: () => Promise<SigningKey>};

/** The signing a build uses: the environment's key, else the project's development key (created on first use). */
export function projectSigning(root: string, environment: NodeJS.ProcessEnv = process.env): NativeSigning {
  let loaded: Promise<SigningKey> | undefined;
  return {mode: "sign", key: () => loaded ??= (async () => {
    const configured = environment[SIGNING_KEY_ENVIRONMENT]?.trim();
    if (configured) return readSigningKey(configured);
    const path = join(root, ...DEVELOPMENT_KEY_FILE.split("/"));
    return existsSync(path) ? readSigningKey(path) : writeSigningKey(path);
  })()};
}

export interface SealedManifest {
  readonly manifest: ExtensionManifest;
  /** Platforms whose libraries are on disk. */
  readonly available: readonly NativeExtensionPlatform[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
}

/**
 * Fills a native manifest's SHA-256s from the files on disk and signs every
 * platform that has its libraries. A manifest that is already signed (an
 * installed package) is checked instead: its files must match and its
 * signatures verify, since the Player refuses anything else.
 */
export async function sealNativeManifest(root: string, manifestPath: string, manifest: ExtensionManifest,
  signing: NativeSigning): Promise<SealedManifest> {
  const native = manifest.native;
  if (native === undefined) return {manifest, available: [], diagnostics: []};
  const diagnostics: AuthoringDiagnostic[] = [...nativeManifestDiagnostics(manifest, manifestPath, false)];
  if (diagnostics.length > 0) return {manifest, available: [], diagnostics};
  const signed = nativeExtensionPlatforms.some((platform) => native.libraries[platform]?.signature !== undefined);
  const libraries: Partial<Record<NativeExtensionPlatform, ExtensionNativeLibrary>> = {};
  const available: NativeExtensionPlatform[] = [];
  for (const platform of nativeExtensionPlatforms) {
    const library = native.libraries[platform];
    if (library === undefined) continue;
    const hashes = new Map<string, string>();
    let complete = true;
    for (const file of nativePlatformFiles(manifest, platform)) {
      const path = projectPath(root, manifestPath, file.path);
      if (!existsSync(path)) { complete = false; continue; }
      const digest = sha256(await readFile(path));
      if (file.sha256 !== undefined && file.sha256 !== digest)
        diagnostics.push(error("PXCLIEXT1104", `${file.path} is not the file its manifest pins`, manifestPath,
          signed ? "An installed package's native file changed; run prismatix extension update <owner/name> --reinstall."
            : "Remove the sha256 from the manifest (builds fill it) or restore the file."));
      hashes.set(file.path, digest);
    }
    if (complete) available.push(platform);
    libraries[platform] = {...library, ...(hashes.has(library.path) ? {sha256: hashes.get(library.path)!} : {}),
      ...(library.files === undefined ? {} : {files: library.files.map((file) =>
        ({...file, ...(hashes.has(file.path) ? {sha256: hashes.get(file.path)!} : {})}))})};
  }
  let sealed: ExtensionManifest = {...manifest, native: {...native, libraries}};
  if (signed) {
    for (const platform of available) {
      const library = sealed.native!.libraries[platform]!;
      if (library.signature === undefined || native.publicKey === undefined ||
          !await verifyNativeSignature(nativeSignatureMessage(sealed, platform), library.signature, native.publicKey))
        diagnostics.push(error("PXCLIEXT1104", `The ${platform} native library's signature does not verify`, manifestPath,
          "Reinstall the extension; the Player refuses an unsigned or altered library."));
    }
    return {manifest: sealed, available, diagnostics};
  }
  if (signing.mode === "check" || available.length === 0) return {manifest: sealed, available, diagnostics};
  const key = await signing.key();
  if (native.publicKey !== undefined && native.publicKey !== key.publicKey) {
    diagnostics.push(error("PXCLIEXT1105", `${manifestPath} names a native publicKey that ${key.source} does not match`, manifestPath,
      `Sign with the key that publicKey belongs to (--signing-key or ${SIGNING_KEY_ENVIRONMENT}), or remove publicKey.`));
    return {manifest: sealed, available, diagnostics};
  }
  sealed = {...sealed, native: {...sealed.native!, publicKey: key.publicKey}};
  const signedLibraries: Partial<Record<NativeExtensionPlatform, ExtensionNativeLibrary>> = {...sealed.native!.libraries};
  for (const platform of available)
    signedLibraries[platform] = {...signedLibraries[platform]!, signature: signNativeMessage(nativeSignatureMessage(sealed, platform), key)};
  return {manifest: {...sealed, native: {...sealed.native!, libraries: signedLibraries}}, available, diagnostics};
}

export type BuildTarget =
  | {readonly kind: "web"}
  | {readonly kind: "android"}
  | {readonly kind: "desktop"; readonly platforms: readonly NativeExtensionPlatform[]};

/**
 * Extensions that cannot run on the build's target: a native extension on
 * the Web (PXCLIEXT1101) or Android (PXCLIEXT1102), an extension whose
 * `platforms` leave the target out (PXCLIEXT1103), a native library missing
 * for a desktop platform (PXCLIEXT1104).
 */
export function platformFindings(extensions: readonly {readonly manifestPath: string; readonly manifest: ExtensionManifest;
  readonly available?: readonly NativeExtensionPlatform[]}[], target: BuildTarget): AuthoringDiagnostic[] {
  const findings: AuthoringDiagnostic[] = [];
  for (const {manifestPath, manifest, available} of extensions) {
    const platforms = manifestPlatforms(manifest);
    if (target.kind === "web") {
      if (manifest.native !== undefined)
        findings.push(error("PXCLIEXT1101", `${manifest.id} loads native libraries, which the Web Player cannot`, manifestPath,
          "Remove it from the project's extensions for the Web build, or give its services a Web implementation in another extension."));
      else if (!platforms.includes("web"))
        findings.push(error("PXCLIEXT1103", `${manifest.id} does not list "web" in its platforms`, manifestPath));
      continue;
    }
    if (target.kind === "android") {
      if (manifest.native !== undefined)
        findings.push(error("PXCLIEXT1102", `${manifest.id} loads native libraries, which the Android Player does not support yet`, manifestPath));
      else if (!platforms.includes("android-arm64"))
        findings.push(error("PXCLIEXT1103", `${manifest.id} does not list "android-arm64" in its platforms`, manifestPath));
      continue;
    }
    for (const platform of target.platforms) {
      if (!platforms.includes(platform)) {
        findings.push(error("PXCLIEXT1103", `${manifest.id} does not run on ${platform} (its platforms: ${platforms.join(", ")})`, manifestPath));
        continue;
      }
      if (manifest.native !== undefined && available !== undefined && !available.includes(platform))
        findings.push(error("PXCLIEXT1104", `${manifest.id} has no ${platform} native library in this project`, manifestPath,
          manifestPath.startsWith("Extensions/")
            ? `Install it: prismatix extension update <owner/name> --reinstall --platform ${platform} --allow-native`
            : `Build ${nativePlatformFiles(manifest, platform)[0]?.path ?? `native/${platform}/`} first.`));
    }
  }
  return findings;
}

/**
 * Copies every native extension's files for `platform` into
 * `<dataRoot>/Plugins/<extension id>/` (flat, by file name), checking each
 * against the SHA-256 the sealed manifest pins. Returns the copied paths.
 */
export async function stageNativePlugins(root: string, extensions: readonly {readonly manifestPath: string;
  readonly manifest: ExtensionManifest}[], platform: NativeExtensionPlatform, dataRoot: string): Promise<string[]> {
  const base = join(dataRoot, NATIVE_PLUGIN_FOLDER);
  await rm(base, {recursive: true, force: true});
  const copied: string[] = [];
  for (const {manifestPath, manifest} of extensions) {
    if (manifest.native?.libraries[platform] === undefined) continue;
    const folder = join(base, manifest.id);
    await mkdir(folder, {recursive: true});
    const names = new Set<string>();
    for (const file of nativePlatformFiles(manifest, platform)) {
      const source = projectPath(root, manifestPath, file.path);
      const data = await readFile(source);
      if (file.sha256 !== undefined && sha256(data) !== file.sha256)
        throw new Error(`${file.path} of ${manifest.id} changed after it was sealed`);
      const name = file.path.slice(file.path.lastIndexOf("/") + 1);
      if (names.has(name.toLowerCase())) throw new Error(`${manifest.id} has two ${platform} native files named ${name}`);
      names.add(name.toLowerCase());
      await copyFile(source, join(folder, name));
      copied.push(`${NATIVE_PLUGIN_FOLDER}/${manifest.id}/${name}`);
    }
  }
  if (copied.length === 0) await rm(base, {recursive: true, force: true});
  return copied;
}

/** The files of a platform's native pack (`native/<platform>/...` relative to the manifest). */
export async function nativePackFiles(root: string, manifestPath: string, manifest: ExtensionManifest,
  platform: NativeExtensionPlatform): Promise<ExtensionPackageFile[]> {
  const files: ExtensionPackageFile[] = [];
  for (const file of nativePlatformFiles(manifest, platform))
    files.push({path: file.path, data: new Uint8Array(await readFile(projectPath(root, manifestPath, file.path)))});
  return files;
}
