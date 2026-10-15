import {createHash} from "node:crypto";
import {copyFile, link, lstat, mkdir, readdir, readFile, writeFile} from "node:fs/promises";
import {basename, dirname, extname, join, relative, resolve} from "node:path";
import {fingerprintFile, maxOutputMarkerBytes, outputOwnershipMarker,
  publishDirectory, publishedInputs, retainPublishedDirectoryIfMatching, verifiedPublishedInputs,
  type PublicationOptions, type PublishedInput} from "./output-safety.js";

export const packageProvenanceUri = "Package/cli-package-provenance.json";
const provenanceFormat = "PrismatiXPackageRetention/1";
const contentFormat = "PrismatiXPackageContent/1";
const runtimeProvenanceFormat = "PrismatiXPackageRetention/2";
const fingerprintPattern = /^[0-9a-f]{64}$/u;
const reusedArchiveMessage = /^Reused unchanged (Content(?:-\d{2})?\.pdx)$/u;

/** Native Packager reports only archives it hard-linked from the old output.
 * Publication checks their file identity again before it reuses marker hashes. */
export function reusedArchiveInputs(events: readonly Readonly<Record<string, unknown>>[],
  previous: readonly PublishedInput[]): Map<string, PublishedInput> {
  const byUri = new Map(previous.map(input => [input.uri, input]));
  const reused = new Map<string, PublishedInput>();
  for (const event of events) {
    if (event.event !== "progress" || event.phase !== "archive-write" ||
        typeof event.message !== "string") continue;
    const archive = reusedArchiveMessage.exec(event.message)?.[1];
    const input = archive === undefined ? undefined : byUri.get(archive);
    if (input !== undefined) reused.set(input.uri, input);
  }
  return reused;
}

function isDynamicLibrary(name: string): boolean {
  const lower = name.toLowerCase();
  if (process.platform === "win32") return lower.endsWith(".dll");
  if (process.platform === "darwin") return lower.endsWith(".dylib");
  return /\.so(?:\.|$)/u.test(lower);
}

function copiedRuntimeLibrary(name: string): boolean {
  const lower = name.toLowerCase();
  return isDynamicLibrary(name) && lower !== "dxil.dll" &&
    !lower.includes("spirv-cross") && !lower.startsWith("clang_rt.");
}

async function regularFile(path: string): Promise<boolean> {
  const info = await lstat(path);
  return info.isFile() && !info.isSymbolicLink();
}

async function runtimeFiles(packager: string, player: string): Promise<string[] | undefined> {
  // A script override may import arbitrary code. Its dependency graph is not
  // bounded by the adjacent native-library inventory, so never retain it.
  if ([packager, player].some(path => [".js", ".mjs", ".cjs"].includes(extname(path).toLowerCase())))
    return undefined;
  const files = new Set([resolve(packager), resolve(player)]);
  for (const directory of new Set([dirname(resolve(packager)), dirname(resolve(player))])) {
    for (const entry of await readdir(directory, {withFileTypes: true})) {
      if (isDynamicLibrary(entry.name)) {
        if (!entry.isFile() || entry.isSymbolicLink()) return undefined;
        files.add(join(directory, entry.name));
      }
    }
  }
  const share = join(dirname(dirname(resolve(player))), "share", "PrismatiX");
  const visit = async (directory: string): Promise<boolean> => {
    for (const entry of await readdir(directory, {withFileTypes: true})) {
      if (entry.isSymbolicLink()) return false;
      const path = join(directory, entry.name);
      if (entry.isDirectory()) {
        if (!await visit(path)) return false;
      } else if (entry.isFile()) files.add(path);
      else return false;
    }
    return true;
  };
  let shareInfo;
  try { shareInfo = await lstat(share); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
  }
  if (shareInfo !== undefined) {
    if (!shareInfo.isDirectory() || shareInfo.isSymbolicLink() ||
        !await visit(share)) return undefined;
  }
  const ordered = [...files].sort();
  for (const path of ordered) if (!await regularFile(path)) return undefined;
  return ordered;
}

async function packagerFiles(packager: string): Promise<string[] | undefined> {
  if ([".js", ".mjs", ".cjs"].includes(extname(packager).toLowerCase())) return undefined;
  const files = [resolve(packager)];
  for (const entry of await readdir(dirname(resolve(packager)), {withFileTypes: true})) {
    if (!isDynamicLibrary(entry.name)) continue;
    if (!entry.isFile() || entry.isSymbolicLink()) return undefined;
    files.push(join(dirname(resolve(packager)), entry.name));
  }
  files.sort();
  for (const path of files) if (!await regularFile(path)) return undefined;
  return files;
}

/** The exact runtime outputs copied by native Packager (not arbitrary files
 * adjacent to the Player). A changed Packager cannot use this fast path. */
export async function runtimeReplacementFiles(player: string): Promise<Map<string, string> | undefined> {
  if ([".js", ".mjs", ".cjs"].includes(extname(player).toLowerCase())) return undefined;
  const resolved = resolve(player);
  if (!await regularFile(resolved)) return undefined;
  const outputs = new Map<string, string>([[basename(resolved), resolved]]);
  for (const entry of await readdir(dirname(resolved), {withFileTypes: true})) {
    if (!copiedRuntimeLibrary(entry.name)) continue;
    if (!entry.isFile() || entry.isSymbolicLink() || outputs.has(entry.name)) return undefined;
    outputs.set(entry.name, join(dirname(resolved), entry.name));
  }
  const share = join(dirname(dirname(resolved)), "share", "PrismatiX");
  let shareInfo;
  try { shareInfo = await lstat(share); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
  }
  if (shareInfo !== undefined) {
    if (!shareInfo.isDirectory() || shareInfo.isSymbolicLink()) return undefined;
    const addNotice = async (source: string): Promise<boolean> => {
      const info = await lstat(source);
      if (!info.isFile() || info.isSymbolicLink()) return false;
      outputs.set(`Notices/${relative(share, source).split("\\").join("/")}`, source);
      return true;
    };
    for (const name of ["LICENSE", "THIRD_PARTY_NOTICES.md",
      "ASSET_PROVENANCE.json", "release-inventory.json"]) {
      const source = join(share, name);
      try { if (!await addNotice(source)) return undefined; }
      catch (error) { if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error; }
    }
    const visit = async (directory: string): Promise<boolean> => {
      for (const entry of await readdir(directory, {withFileTypes: true})) {
        if (entry.isSymbolicLink()) return false;
        const source = join(directory, entry.name);
        if (entry.isDirectory()) {
          if (!await visit(source)) return false;
        } else if (entry.isFile()) {
          if (!await addNotice(source)) return false;
        } else return false;
      }
      return true;
    };
    for (const name of ["licenses", "docs/releases"]) {
      const source = join(share, name);
      try {
        const info = await lstat(source);
        if (!info.isDirectory() || info.isSymbolicLink() || !await visit(source)) return undefined;
      } catch (error) { if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error; }
    }
  }
  return outputs;
}

export async function runtimeOutputsMatch(staging: string,
  sources: ReadonlyMap<string, string>): Promise<boolean> {
  for (const [uri, source] of sources) {
    const target = join(staging, ...uri.split("/"));
    try {
      if (!await regularFile(target) ||
          await fingerprintFile(target) !== await fingerprintFile(source)) return false;
    } catch { return false; }
  }
  return true;
}

/** A retention key covers the complete native request semantics and all
 * binaries/adjacent runtime files that can change its output. It does not
 * prove source bytes; retainPackageIfMatching verifies those separately. */
export async function packageRetentionKey(packager: string, player: string,
  artifactRoot: string, request: Readonly<Record<string, unknown>>): Promise<string | undefined> {
  const sources = await runtimeFiles(packager, player);
  if (sources === undefined) return undefined;
  const tools: Array<{path: string; fingerprint: string}> = [];
  for (const path of sources) tools.push({path, fingerprint: await fingerprintFile(path)});
  const material = JSON.stringify({format: provenanceFormat,
    artifactRoot: resolve(artifactRoot), request, tools});
  return createHash("sha256").update(material).digest("hex");
}

/** Content-producing inputs exclude Player binaries and release notices.
 * Artifact bytes are verified under the output lock before runtime refresh. */
export async function packageContentKey(packager: string, artifactRoot: string,
  request: Readonly<Record<string, unknown>>): Promise<string | undefined> {
  const sources = await packagerFiles(packager);
  if (sources === undefined) return undefined;
  const tools: Array<{path: string; fingerprint: string}> = [];
  for (const path of sources) tools.push({path, fingerprint: await fingerprintFile(path)});
  const {playerExecutable: _playerExecutable, ...contentRequest} = request;
  const material = JSON.stringify({format: contentFormat,
    artifactRoot: resolve(artifactRoot), request: contentRequest, tools});
  return createHash("sha256").update(material).digest("hex");
}

type PackageProvenance = {
  planFingerprint: string;
  contentFingerprint?: string;
  runtimeOutputs?: string[];
};

async function existingProvenance(output: string): Promise<PackageProvenance | undefined> {
  const path = join(output, ...packageProvenanceUri.split("/"));
  let info;
  try { info = await lstat(path); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined;
    throw error;
  }
  // Installed release inventories can include hundreds of notice files.
  if (!info.isFile() || info.isSymbolicLink() || info.size > 1024 * 1024) return undefined;
  let document: unknown;
  try { document = JSON.parse(await readFile(path, "utf8")); }
  catch { return undefined; }
  if (document === null || typeof document !== "object" || Array.isArray(document)) return undefined;
  const value = document as {format?: unknown; planFingerprint?: unknown;
    contentFingerprint?: unknown; runtimeOutputs?: unknown};
  if (typeof value.planFingerprint !== "string" ||
      !fingerprintPattern.test(value.planFingerprint)) return undefined;
  if (value.format === provenanceFormat) return {planFingerprint: value.planFingerprint};
  if (value.format !== runtimeProvenanceFormat ||
      typeof value.contentFingerprint !== "string" ||
      !fingerprintPattern.test(value.contentFingerprint) ||
      !Array.isArray(value.runtimeOutputs) ||
      value.runtimeOutputs.length > 4096 ||
      !value.runtimeOutputs.every((uri): uri is string =>
        typeof uri === "string" && runtimeOutputUri(uri))) return undefined;
  const outputs = value.runtimeOutputs as string[];
  if (new Set(outputs).size !== outputs.length) return undefined;
  return {planFingerprint: value.planFingerprint,
    contentFingerprint: value.contentFingerprint, runtimeOutputs: outputs};
}

function runtimeOutputUri(uri: string): boolean {
  if (!uri || uri.includes("\\") || uri.includes(":") || uri.startsWith("/") ||
      uri.split("/").some(part => !part || part === "." || part === "..")) return false;
  if (uri.startsWith("Notices/")) return true;
  return !uri.includes("/") && (isDynamicLibrary(uri) ||
    uri.toLowerCase().endsWith(".exe") || !uri.includes("."));
}

async function outputFingerprints(output: string): Promise<Map<string, string> | undefined> {
  const path = join(output, outputOwnershipMarker);
  let info;
  try { info = await lstat(path); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined;
    throw error;
  }
  if (!info.isFile() || info.isSymbolicLink() || info.size > maxOutputMarkerBytes)
    return undefined;
  let document: unknown;
  try { document = JSON.parse(await readFile(path, "utf8")); }
  catch { return undefined; }
  if (document === null || typeof document !== "object" || Array.isArray(document)) return undefined;
  const value = document as {format?: unknown; files?: unknown};
  if (value.format !== "PrismatiXOutput/1" || value.files === null ||
      typeof value.files !== "object" || Array.isArray(value.files)) return undefined;
  const files = new Map<string, string>();
  for (const [uri, fingerprint] of Object.entries(value.files as Record<string, unknown>)) {
    if (fingerprint === "directory") continue;
    if (typeof fingerprint !== "string" || !fingerprintPattern.test(fingerprint))
      return undefined;
    files.set(uri, fingerprint);
  }
  return files;
}

function sameInputs(expected: readonly PublishedInput[], actual: readonly PublishedInput[]): boolean {
  return expected.length === actual.length && expected.every((input, index) =>
    input.uri === actual[index]?.uri &&
    input.size === actual[index]?.size &&
    input.fingerprint === actual[index]?.fingerprint);
}

/** Reuse only a distribution produced by the same request and toolchain.
 * There is no staging writer on this path: one complete artifact content
 * check under the distribution lock, after output verification, is the late
 * source-change check. Never trust only sizes or timestamps. */
export async function retainPackageIfMatching(options: PublicationOptions,
  artifactRoot: string, inputs: readonly PublishedInput[], planFingerprint: string,
  currentPlanFingerprint: () => Promise<string | undefined>): Promise<boolean> {
  if ((await existingProvenance(options.output))?.planFingerprint !== planFingerprint) return false;
  const expectedOutput = await outputFingerprints(options.output);
  if (expectedOutput === undefined) return false;
  return retainPublishedDirectoryIfMatching(options, expectedOutput, async () => {
    if ((await existingProvenance(options.output))?.planFingerprint !== planFingerprint ||
        await currentPlanFingerprint() !== planFingerprint) return false;
    try {
      return sameInputs(inputs,
        await verifiedPublishedInputs(artifactRoot, options.onProgress));
    } catch {
      // Legacy artifacts may have implicitly owned directories. The native
      // Packager still accepts them after independently hashing every file.
      // A changed source likewise falls through to its normal diagnostics.
      return false;
    }
  });
}

class RuntimeRefreshUnavailable extends Error {}

/** Replace only the Player, adjacent libraries and release notices when an
 * owned distribution proves its packaged content came from the same request.
 * The publisher still hashes old output and the full artifact under its lock.
 * Anything uncertain returns to native Packager or fails without publishing. */
export async function refreshRuntimePackageIfMatching(options: PublicationOptions,
  artifactRoot: string, inputs: readonly PublishedInput[],
  planFingerprint: string, contentFingerprint: string,
  runtimeSources: ReadonlyMap<string, string>,
  currentPlanFingerprint: () => Promise<string | undefined>,
  currentContentFingerprint: () => Promise<string | undefined>): Promise<boolean> {
  const old = await existingProvenance(options.output);
  if (old?.contentFingerprint !== contentFingerprint ||
      old.runtimeOutputs === undefined) return false;
  try {
    await publishedInputs(artifactRoot, undefined, true);
  } catch {
    // Legacy artifacts with implicitly owned directories need native input
    // validation; do not reinterpret their marker as a fast-path guarantee.
    return false;
  }
  const linkedOutputFiles = new Map<string, PublishedInput>();
  try {
    await publishDirectory({...options, verifyExistingBeforeBuild: false,
      linkedOutputFiles,
      beforePromotion: async () => {
        await options.beforePromotion?.();
        if (await currentPlanFingerprint() !== planFingerprint ||
            await currentContentFingerprint() !== contentFingerprint)
          throw new Error("Runtime tools changed during runtime-only packaging");
        if (!sameInputs(inputs,
          await verifiedPublishedInputs(artifactRoot, options.onProgress)))
          throw new Error("Artifact changed during runtime-only packaging");
      }}, async staging => {
      const current = await existingProvenance(options.output);
      if (current?.contentFingerprint !== contentFingerprint ||
          current.runtimeOutputs === undefined)
        throw new RuntimeRefreshUnavailable("Package provenance changed");
      const oldFiles = await publishedInputs(options.output, undefined, true);
      const oldByUri = new Map(oldFiles.map(input => [input.uri, input]));
      const oldRuntime = new Set(current.runtimeOutputs);
      if (![...oldRuntime].every(uri => oldByUri.has(uri)))
        throw new RuntimeRefreshUnavailable("Recorded runtime output is missing");
      const content = oldFiles.filter(input =>
        input.uri !== packageProvenanceUri && !oldRuntime.has(input.uri));
      if (!content.some(input => input.uri === "Package/manifest.json") ||
          !content.some(input => /^Content(?:-\d{2})?\.pdx$/u.test(input.uri)) ||
          content.some(input => runtimeSources.has(input.uri)))
        throw new RuntimeRefreshUnavailable("Runtime output overlaps packaged content");
      for (const input of content) {
        const source = join(options.output, ...input.uri.split("/"));
        const target = join(staging, ...input.uri.split("/"));
        await mkdir(dirname(target), {recursive: true});
        try {
          await link(source, target);
          linkedOutputFiles.set(input.uri, input);
        } catch {
          await copyFile(source, target);
        }
      }
      for (const [uri, source] of runtimeSources) {
        if (!runtimeOutputUri(uri) || uri === packageProvenanceUri)
          throw new RuntimeRefreshUnavailable("Invalid new runtime output");
        const target = join(staging, ...uri.split("/"));
        await mkdir(dirname(target), {recursive: true});
        await copyFile(source, target);
      }
      await writePackageProvenance(staging, planFingerprint,
        contentFingerprint, [...runtimeSources.keys()].sort());
    });
    return true;
  } catch (error) {
    if (error instanceof RuntimeRefreshUnavailable) return false;
    throw error;
  }
}

export async function writePackageProvenance(staging: string,
  planFingerprint: string, contentFingerprint?: string,
  runtimeOutputs?: readonly string[]): Promise<void> {
  if (!fingerprintPattern.test(planFingerprint)) throw new Error("Invalid package retention key");
  if (contentFingerprint !== undefined && !fingerprintPattern.test(contentFingerprint))
    throw new Error("Invalid package content key");
  if (runtimeOutputs !== undefined &&
      (!runtimeOutputs.every(runtimeOutputUri) ||
       new Set(runtimeOutputs).size !== runtimeOutputs.length))
    throw new Error("Invalid package runtime outputs");
  if ((contentFingerprint === undefined) !== (runtimeOutputs === undefined))
    throw new Error("Package content key and runtime outputs must be recorded together");
  await writeFile(join(staging, ...packageProvenanceUri.split("/")),
    `${JSON.stringify(contentFingerprint === undefined
      ? {format: provenanceFormat, planFingerprint}
      : {format: runtimeProvenanceFormat, planFingerprint,
        contentFingerprint, runtimeOutputs})}\n`, {flag: "wx"});
}
