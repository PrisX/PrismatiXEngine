// A packed distribution as the one file a player downloads: a .zip for
// Windows and macOS, a .tar.gz for Linux. Executables (ELF and Mach-O
// files, scripts) are stored with mode 755 whatever the host filesystem
// kept, so a macOS or Linux game packed on Windows still starts.

import {randomUUID} from "node:crypto";
import {lstat, rename, rm} from "node:fs/promises";
import {basename, dirname, join} from "node:path";

import {archiveTree, looksExecutable, writeTarGz, writeZip, type ArchiveEntry} from "./archive.js";
import {platformOs, type DesktopPlatform} from "./platform-runtime.js";

export function archiveExtension(platform: DesktopPlatform): ".zip" | ".tar.gz" {
  return platformOs(platform) === "linux" ? ".tar.gz" : ".zip";
}

export interface DistributionArchive {
  readonly path: string;
  readonly files: number;
  readonly executables: readonly string[];
  readonly bytes: number;
}

/**
 * Writes `directory` (a published distribution) to `archive`. The CLI's own
 * bookkeeping files (.prismatix-*) are left out. `topFolder` puts everything
 * under one folder, so extracting a Windows or Linux game gives one folder;
 * a macOS distribution is already one .app.
 */
export async function archiveDistribution(directory: string, archive: string, platform: DesktopPlatform,
  topFolder?: string): Promise<DistributionArchive> {
  const tree = await archiveTree(directory, (path) => !path.includes("/") && basename(path).startsWith(".prismatix"));
  const executables: string[] = [];
  const entries: ArchiveEntry[] = [];
  const sources = new Map<string, string>();
  for (const entry of tree) {
    let mode = entry.mode;
    if (entry.type === "directory") mode = 0o755;
    else if (entry.type === "file") {
      const path = join(directory, ...entry.path.split("/"));
      const executable = await looksExecutable(path) || (process.platform !== "win32" && (entry.mode & 0o111) !== 0);
      mode = executable ? 0o755 : 0o644;
      if (executable) executables.push(entry.path);
    }
    const stored = topFolder === undefined ? entry.path : `${topFolder}/${entry.path}`;
    entries.push({...entry, path: stored, mode});
    sources.set(stored, join(directory, ...entry.path.split("/")));
  }
  if (topFolder !== undefined) entries.unshift({path: topFolder, type: "directory", mode: 0o755, size: 0});
  const partial = join(dirname(archive), `.${basename(archive)}.${randomUUID()}.partial`);
  const source = {entries, fileOf: (entry: ArchiveEntry) => sources.get(entry.path)!};
  try {
    if (archiveExtension(platform) === ".zip") await writeZip(partial, source);
    else await writeTarGz(partial, source);
    await rm(archive, {force: true});
    await rename(partial, archive);
  } finally {
    await rm(partial, {force: true});
  }
  return {path: archive, files: entries.filter(entry => entry.type === "file").length, executables,
    bytes: (await lstat(archive)).size};
}
