// Where a packed distribution keeps its game data, and the macOS steps that
// finish an .app bundle after the native Packager laid it out
// (Engine/SDK/PackageBranding.h describes the layouts themselves).

import {createHash} from "node:crypto";
import {lstat, readdir} from "node:fs/promises";
import {basename, join} from "node:path";

/**
 * The data folder of a distribution (where Package/manifest.json is): the
 * distribution itself (flat, Linux), its Windows engine folder ("bundled"),
 * or a macOS <Name>.app/Contents/Resources. Mirrors DistributionDataRoot in
 * the native Packager; returns the distribution unchanged when there is no
 * single such folder. An engine folder is recognised by its assembly
 * manifest, a bundle by its .app name.
 */
export async function distributionDataRoot(distribution: string): Promise<string> {
  const hasManifest = async (directory: string) =>
    (await lstat(join(directory, "Package", "manifest.json")).catch(() => undefined))?.isFile() === true;
  if (await hasManifest(distribution)) return distribution;
  const entries = await readdir(distribution, {withFileTypes: true}).catch(() => []);
  const found: string[] = [];
  for (const entry of entries.slice(0, 4096)) {
    if (!entry.isDirectory()) continue;
    // An engine folder carries its assembly manifest (<folder>/<folder>.manifest),
    // so the parent of an ordinary distribution is not mistaken for one.
    const app = entry.name.toLowerCase().endsWith(".app");
    const candidate = app ? join(distribution, entry.name, "Contents", "Resources") : join(distribution, entry.name);
    const marked = app || (await lstat(join(candidate, `${entry.name}.manifest`)).catch(() => undefined))?.isFile() === true;
    if (marked && await hasManifest(candidate)) found.push(candidate);
  }
  return found.length === 1 ? found[0]! : distribution;
}

/** Library install names from `otool -L` output (the first line names the file itself). */
export function otoolDependencies(output: string): string[] {
  return output.split(/\r?\n/u).slice(1)
    .map(line => /^\s+(.+?) \(compatibility version/u.exec(line)?.[1])
    .filter((name): name is string => name !== undefined);
}

/** LC_RPATH entries from `otool -l` output. */
export function otoolRpaths(output: string): string[] {
  const paths: string[] = [];
  const lines = output.split(/\r?\n/u);
  for (let index = 0; index < lines.length; index++) {
    if (!/^\s*cmd LC_RPATH$/u.test(lines[index]!)) continue;
    for (let next = index + 1; next < Math.min(lines.length, index + 4); next++) {
      const path = /^\s*path (.+?) \(offset \d+\)$/u.exec(lines[next]!)?.[1];
      if (path !== undefined) { paths.push(path); break; }
    }
  }
  return paths;
}

export interface MacBundleFile {
  readonly path: string;
  readonly dependencies: readonly string[];
  readonly rpaths: readonly string[];
}

/**
 * install_name_tool invocations that make the bundle self-contained: every
 * library in Contents/Frameworks is identified as @rpath/<name>, references
 * to bundled libraries go through @rpath, the executable searches
 * @executable_path/../Frameworks and libraries search their own folder.
 */
export function macBundleFixups(executable: MacBundleFile, libraries: readonly MacBundleFile[]): string[][] {
  const bundled = new Set(libraries.map(library => basename(library.path)));
  const commands: string[][] = [];
  const relink = (file: MacBundleFile, selfName?: string) => {
    const args: string[] = [];
    if (selfName !== undefined) args.push("-id", `@rpath/${selfName}`);
    for (const dependency of file.dependencies) {
      const name = basename(dependency);
      if (bundled.has(name) && dependency !== `@rpath/${name}` && name !== selfName)
        args.push("-change", dependency, `@rpath/${name}`);
    }
    return args;
  };
  for (const library of libraries) {
    const args = relink(library, basename(library.path));
    if (!library.rpaths.includes("@loader_path")) args.push("-add_rpath", "@loader_path");
    commands.push(["install_name_tool", ...args, library.path]);
  }
  const args = relink(executable);
  if (libraries.length > 0 && !executable.rpaths.includes("@executable_path/../Frameworks"))
    args.push("-add_rpath", "@executable_path/../Frameworks");
  if (args.length > 0) commands.push(["install_name_tool", ...args, executable.path]);
  return commands;
}

/** One architecture slice of a Mach-O file: its libraries and LC_RPATH entries. */
export interface MachOSlice {
  readonly cpu: "arm64" | "x86_64" | "other";
  readonly dependencies: readonly string[];
  readonly rpaths: readonly string[];
  /** Whether it carries a code signature (LC_CODE_SIGNATURE), e.g. the linker's ad hoc one. */
  readonly signed: boolean;
}

/**
 * Reads the load commands of a Mach-O file (thin or universal) without
 * Apple's tools, so any host can check a macOS distribution. Undefined for
 * anything else.
 */
export function machOSlices(bytes: Buffer): MachOSlice[] | undefined {
  if (bytes.length < 32) return undefined;
  const magic = bytes.readUInt32BE(0);
  if (magic === 0xcafebabe || magic === 0xcafebabf) {
    const wide = magic === 0xcafebabf;
    const count = bytes.readUInt32BE(4);
    if (count === 0 || count > 16) return undefined;
    const slices: MachOSlice[] = [];
    for (let index = 0; index < count; index++) {
      const at = 8 + index * (wide ? 32 : 20);
      if (at + (wide ? 32 : 20) > bytes.length) return undefined;
      const offset = wide ? Number(bytes.readBigUInt64BE(at + 8)) : bytes.readUInt32BE(at + 8);
      const size = wide ? Number(bytes.readBigUInt64BE(at + 16)) : bytes.readUInt32BE(at + 12);
      if (offset + size > bytes.length) return undefined;
      const slice = machOSlices(bytes.subarray(offset, offset + size));
      if (slice === undefined) return undefined;
      slices.push(...slice);
    }
    return slices;
  }
  if (bytes.readUInt32LE(0) !== 0xfeedfacf) return undefined;
  const cpuType = bytes.readUInt32LE(4);
  const commands = bytes.readUInt32LE(16);
  const dependencies: string[] = [];
  const rpaths: string[] = [];
  let signed = false;
  let at = 32;
  for (let index = 0; index < commands && at + 8 <= bytes.length; index++) {
    const command = bytes.readUInt32LE(at);
    const size = bytes.readUInt32LE(at + 4);
    if (size < 8 || at + size > bytes.length) return undefined;
    const text = (offset: number) => {
      const start = at + bytes.readUInt32LE(at + offset);
      const end = bytes.indexOf(0, start);
      return bytes.toString("utf8", start, end < 0 || end > at + size ? at + size : end);
    };
    // LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB
    if ([0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023].includes(command)) dependencies.push(text(8));
    else if (command === 0x8000001c) rpaths.push(text(8));
    else if (command === 0x1d) signed = true;
    at += size;
  }
  return [{cpu: cpuType === 0x0100000c ? "arm64" : cpuType === 0x01000007 ? "x86_64" : "other",
    dependencies, rpaths, signed}];
}

/**
 * Checks the code signature embedded in each slice of a Mach-O file the way
 * the kernel does before running it: the CodeDirectory's page hashes must
 * match the file. "unsigned" when a slice has no signature, "invalid" when
 * a hash differs (the file changed after signing), else "valid". `adhoc`
 * says whether every signature is ad hoc (no certificate).
 */
export function machOCodeSignature(bytes: Buffer): {state: "valid" | "invalid" | "unsigned"; adhoc: boolean} | undefined {
  const magic = bytes.length >= 8 ? bytes.readUInt32BE(0) : 0;
  const slices: Buffer[] = [];
  if (magic === 0xcafebabe || magic === 0xcafebabf) {
    const wide = magic === 0xcafebabf;
    for (let index = 0; index < Math.min(bytes.readUInt32BE(4), 16); index++) {
      const at = 8 + index * (wide ? 32 : 20);
      if (at + (wide ? 32 : 20) > bytes.length) return undefined;
      const offset = wide ? Number(bytes.readBigUInt64BE(at + 8)) : bytes.readUInt32BE(at + 8);
      const size = wide ? Number(bytes.readBigUInt64BE(at + 16)) : bytes.readUInt32BE(at + 12);
      if (offset + size > bytes.length) return undefined;
      slices.push(bytes.subarray(offset, offset + size));
    }
  } else if (bytes.length >= 32 && bytes.readUInt32LE(0) === 0xfeedfacf) slices.push(bytes);
  else return undefined;
  let adhoc = true;
  for (const slice of slices) {
    const commands = slice.readUInt32LE(16);
    let signature: {offset: number; size: number} | undefined;
    for (let at = 32, index = 0; index < commands && at + 16 <= slice.length; index++) {
      const size = slice.readUInt32LE(at + 4);
      if (size < 8) return undefined;
      if (slice.readUInt32LE(at) === 0x1d) signature = {offset: slice.readUInt32LE(at + 8), size: slice.readUInt32LE(at + 12)};
      at += size;
    }
    if (signature === undefined) return {state: "unsigned", adhoc: false};
    const blob = slice.subarray(signature.offset, signature.offset + signature.size);
    if (blob.length < 12 || blob.readUInt32BE(0) !== 0xfade0cc0) return {state: "invalid", adhoc: false};
    let checked = false;
    for (let index = 0; index < blob.readUInt32BE(8) && 12 + index * 8 + 8 <= blob.length; index++) {
      const type = blob.readUInt32BE(12 + index * 8);
      const offset = blob.readUInt32BE(16 + index * 8);
      // The primary CodeDirectory (slot 0) and alternates (0x1000+).
      if (type !== 0 && (type < 0x1000 || type > 0x1004)) continue;
      const directory = blob.subarray(offset);
      if (directory.length < 44 || directory.readUInt32BE(0) !== 0xfade0c02) return {state: "invalid", adhoc: false};
      const flags = directory.readUInt32BE(12);
      const hashOffset = directory.readUInt32BE(16);
      const codeSlots = directory.readUInt32BE(28);
      const codeLimit = directory.readUInt32BE(32);
      const hashSize = directory[36]!;
      const hashType = directory[37]!;
      const pageSize = directory[39] === 0 ? codeLimit : 2 ** directory[39]!;
      const algorithm = hashType === 1 ? "sha1" : hashType === 2 || hashType === 3 ? "sha256" : hashType === 4 ? "sha384" : undefined;
      if (algorithm === undefined || codeLimit > slice.length) return {state: "invalid", adhoc: false};
      if ((flags & 0x2) === 0) adhoc = false;
      for (let page = 0; page < codeSlots; page++) {
        const start = page * pageSize;
        const digest = createHash(algorithm).update(slice.subarray(start, Math.min(start + pageSize, codeLimit))).digest();
        if (!digest.subarray(0, hashSize).equals(directory.subarray(hashOffset + page * hashSize, hashOffset + (page + 1) * hashSize)))
          return {state: "invalid", adhoc};
      }
      checked = true;
    }
    if (!checked) return {state: "invalid", adhoc: false};
  }
  return {state: "valid", adhoc};
}

/**
 * Libraries a bundled Mach-O file loads from outside macOS and the bundle:
 * absolute paths other than /usr/lib and /System. Such a Player only runs
 * on the machine that built it; on a Mac the CLI relinks it, elsewhere this
 * is reported.
 */
export function machOForeignDependencies(slices: readonly MachOSlice[]): string[] {
  const foreign = new Set<string>();
  for (const slice of slices)
    for (const dependency of slice.dependencies)
      if (!dependency.startsWith("@") && !dependency.startsWith("/usr/lib/") && !dependency.startsWith("/System/"))
        foreign.add(dependency);
  return [...foreign].sort();
}

type Run = (command: string, args: readonly string[]) => Promise<{code: number; stdout: string; stderr: string}>;

/**
 * Finishes a macOS .app on a Mac: relinks bundled libraries (macBundleFixups)
 * and signs the bundle ad hoc (`codesign --sign -`), which Apple Silicon
 * requires before anything runs. Developer ID signing and notarization stay
 * the author's step (docs/shipping.md). Returns warnings for missing tools.
 */
export async function finishMacAppBundle(app: string, executable: string, run: Run): Promise<string[]> {
  const frameworks = join(app, "Contents", "Frameworks");
  const libraries = (await readdir(frameworks).catch(() => [] as string[]))
    .filter(name => name.toLowerCase().endsWith(".dylib")).map(name => join(frameworks, name));
  const describe = async (path: string): Promise<MacBundleFile> => ({
    path,
    dependencies: otoolDependencies((await run("otool", ["-L", path])).stdout),
    rpaths: otoolRpaths((await run("otool", ["-l", path])).stdout),
  });
  const warnings: string[] = [];
  try {
    const commands = macBundleFixups(await describe(executable), await Promise.all(libraries.map(describe)));
    for (const [command, ...args] of commands) {
      const result = await run(command!, args);
      if (result.code !== 0) throw new Error(`${command} ${args.join(" ")} failed: ${result.stderr.trim()}`);
    }
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    warnings.push("otool/install_name_tool not found (install the Xcode Command Line Tools); " +
      "bundled libraries were not relinked");
  }
  try {
    for (const target of [...libraries, app]) {
      const signed = await run("codesign", ["--force", "--sign", "-", "--timestamp=none", target]);
      if (signed.code !== 0) throw new Error(`codesign ${target} failed: ${signed.stderr.trim()}`);
    }
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    warnings.push("codesign not found; the .app is unsigned and will not open on Apple Silicon until signed");
  }
  return warnings;
}
