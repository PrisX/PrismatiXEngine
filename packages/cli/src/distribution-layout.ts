// Where a packed distribution keeps its game data, and the macOS steps that
// finish an .app bundle after the native Packager laid it out
// (Engine/SDK/PackageBranding.h describes the layouts themselves).

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
