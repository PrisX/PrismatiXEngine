import {createHash} from "node:crypto";
import {lstat, readdir, readFile, writeFile} from "node:fs/promises";
import {dirname, extname, join, resolve} from "node:path";
import {fingerprintFile, maxOutputMarkerBytes, outputOwnershipMarker,
  retainPublishedDirectoryIfMatching, verifiedPublishedInputs,
  type PublicationOptions, type PublishedInput} from "./output-safety.js";

export const packageProvenanceUri = "Package/cli-package-provenance.json";
const provenanceFormat = "PrismatiXPackageRetention/1";
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

async function existingProvenance(output: string): Promise<string | undefined> {
  const path = join(output, ...packageProvenanceUri.split("/"));
  let info;
  try { info = await lstat(path); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined;
    throw error;
  }
  if (!info.isFile() || info.isSymbolicLink() || info.size > 4096) return undefined;
  let document: unknown;
  try { document = JSON.parse(await readFile(path, "utf8")); }
  catch { return undefined; }
  if (document === null || typeof document !== "object" || Array.isArray(document)) return undefined;
  const value = document as {format?: unknown; planFingerprint?: unknown};
  return value.format === provenanceFormat &&
    typeof value.planFingerprint === "string" &&
    fingerprintPattern.test(value.planFingerprint) ? value.planFingerprint : undefined;
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
  if (await existingProvenance(options.output) !== planFingerprint) return false;
  const expectedOutput = await outputFingerprints(options.output);
  if (expectedOutput === undefined) return false;
  return retainPublishedDirectoryIfMatching(options, expectedOutput, async () => {
    if (await existingProvenance(options.output) !== planFingerprint ||
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

export async function writePackageProvenance(staging: string,
  planFingerprint: string): Promise<void> {
  if (!fingerprintPattern.test(planFingerprint)) throw new Error("Invalid package retention key");
  await writeFile(join(staging, ...packageProvenanceUri.split("/")),
    `${JSON.stringify({format: provenanceFormat, planFingerprint})}\n`, {flag: "wx"});
}
