import {randomUUID} from "node:crypto";
import {readFile, rename, rm, stat, writeFile} from "node:fs/promises";

const format = "PrismatiXDevelopmentInputs/1";

/**
 * What a development build (`run --dev`) read, so the next Play can tell
 * that nothing changed without loading and compiling the project again:
 * every file it read or copied (size and modification time), the
 * directories it listed or looked into (a file added or removed changes
 * their modification time), and the paths it found absent. Media the Player
 * reads in place is covered by its directories: only whether it exists
 * changes the build.
 */
export interface DevelopmentInputs {
  /** The CLI, SDK, Node, TypeScript, runtime and options that built it. */
  readonly identity: string;
  readonly files: readonly string[];
  readonly directories: readonly string[];
  readonly absent: readonly string[];
  /** The Packager's externalInputs for this build (the media left in place). */
  readonly externalInputs: readonly string[];
}

interface Recorded {
  readonly format: string;
  readonly identity: string;
  readonly files: readonly (readonly [string, number, number])[];
  readonly directories: readonly (readonly [string, number])[];
  readonly absent: readonly string[];
  readonly externalInputs: readonly string[];
}

async function inBatches<T, R>(items: readonly T[], each: (item: T) => Promise<R>): Promise<R[]> {
  const results: R[] = [];
  for (let start = 0; start < items.length; start += 128)
    results.push(...await Promise.all(items.slice(start, start + 128).map(each)));
  return results;
}

/**
 * Writes the record, or removes it when an input changed after `startedAt`
 * (the build may have read it before or after that change).
 */
export async function recordDevelopmentInputs(path: string, inputs: DevelopmentInputs,
  startedAt: number): Promise<void> {
  try {
    const files = await inBatches([...new Set(inputs.files)], async (file) => {
      const info = await stat(file);
      return info.isFile() ? [file, info.size, info.mtimeMs] as const : undefined;
    });
    const directories = await inBatches([...new Set(inputs.directories)], async (directory) => {
      const info = await stat(directory).catch(() => undefined);
      return [directory, info === undefined ? -1 : info.mtimeMs] as const;
    });
    if (files.some((file) => file === undefined || file[2] > startedAt) ||
        directories.some(([, time]) => time > startedAt)) {
      await rm(path, {force: true});
      return;
    }
    const recorded: Recorded = {format, identity: inputs.identity,
      files: files as (readonly [string, number, number])[], directories,
      absent: [...new Set(inputs.absent)], externalInputs: inputs.externalInputs};
    const temporary = `${path}.${randomUUID()}.tmp`;
    await writeFile(temporary, JSON.stringify(recorded), {flag: "wx"});
    await rename(temporary, path).finally(() => rm(temporary, {force: true}));
  } catch {
    await rm(path, {force: true}).catch(() => {});
  }
}

/** The last development build's externalInputs when none of its inputs changed. */
export async function unchangedDevelopmentInputs(path: string,
  identity: string): Promise<readonly string[] | undefined> {
  let recorded: Recorded;
  try { recorded = JSON.parse(await readFile(path, "utf8")) as Recorded; }
  catch { return undefined; }
  if (recorded?.format !== format || recorded.identity !== identity ||
      !Array.isArray(recorded.files) || !Array.isArray(recorded.directories) ||
      !Array.isArray(recorded.absent) || !Array.isArray(recorded.externalInputs)) return undefined;
  const files = await inBatches(recorded.files, async ([file, size, time]) => {
    const info = await stat(file).catch(() => undefined);
    return info !== undefined && info.isFile() && info.size === size && info.mtimeMs === time;
  });
  if (files.includes(false)) return undefined;
  const directories = await inBatches(recorded.directories, async ([directory, time]) => {
    const info = await stat(directory).catch(() => undefined);
    return (info === undefined ? -1 : info.mtimeMs) === time;
  });
  if (directories.includes(false)) return undefined;
  const absent = await inBatches(recorded.absent, async (file) => await stat(file).catch(() => undefined) === undefined);
  if (absent.includes(false)) return undefined;
  return recorded.externalInputs;
}
