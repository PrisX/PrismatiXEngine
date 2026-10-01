import {mkdir, readFile, writeFile} from "node:fs/promises";
import {dirname, join} from "node:path";

import {assembleImportedStoryProject, serializeStoryIdentityMap, synchronizeAlignedStoryIdentityMap,
  type AuthoringDiagnostic, type ImportedStoryProjectInput} from "@prismatix/authoring-sdk";

/** Shared output side of `prismatix import <format>`. */
export class ImportFailure extends Error {
  constructor(message: string, readonly diagnostics: readonly AuthoringDiagnostic[] = []) {
    super(message);
  }
}

async function readOptional(path: string): Promise<string | undefined> {
  try { return await readFile(path, "utf8"); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined;
    throw error;
  }
}

/** Write `text` unless the file already holds it (ignoring CRLF checkouts). */
export async function writeIfChanged(path: string, text: string | Buffer): Promise<boolean> {
  if (typeof text === "string") {
    const existing = await readOptional(path);
    if (existing !== undefined && existing.replace(/\r\n/gu, "\n") === text) return false;
  }
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, text, typeof text === "string" ? "utf8" : undefined);
  return true;
}

export interface PublishedStory {
  readonly files: number;
  readonly written: number;
  /** Sources whose earlier sidecar identities were kept through a form-only edit. */
  readonly preservedIdentities: number;
  /** Sources whose operations changed so their identities were regenerated. */
  readonly regenerated: readonly string[];
}

type IdentityEntry = {readonly id?: string; readonly kind?: string; readonly command?: string};
const identitySequence = (map: unknown): string | undefined => {
  const entries = (map as {entries?: unknown} | null)?.entries;
  return Array.isArray(entries)
    ? JSON.stringify(entries.map((entry: IdentityEntry) => [entry.id, entry.kind, entry.command ?? null]))
    : undefined;
};

/**
 * Assemble translated scenes with the SDK and publish index, sources and
 * identity sidecars as one set. When a source already exists with a sidecar
 * whose IDs differ from the fresh ones (an author edited them, or an older
 * importer assigned them), the old IDs are kept if every executable line is
 * the same operation in the same place: unchanged, or re-spelled in a form
 * `equivalent` vouches for. Otherwise the fresh IDs are written and the
 * source is listed in `regenerated`.
 */
export async function publishImportedStory(root: string, input: ImportedStoryProjectInput,
  equivalent: (previous: string, current: string) => boolean = () => false): Promise<PublishedStory> {
  const assembled = assembleImportedStoryProject(input);
  if (!assembled.valid || assembled.value === undefined)
    throw new ImportFailure("The translated Story does not assemble", assembled.diagnostics);
  const files = {...assembled.value.files};
  let preservedIdentities = 0;
  const regenerated: string[] = [];
  for (const scene of input.scenes) for (const source of Object.values(scene.sources)) {
    const mapPath = `${source.path}.pxids.json`;
    const previousSource = await readOptional(join(root, ...source.path.split("/")));
    const previousMapText = await readOptional(join(root, ...mapPath.split("/")));
    if (previousSource === undefined || previousMapText === undefined) continue;
    let previousMap: unknown;
    try { previousMap = JSON.parse(previousMapText); } catch { regenerated.push(source.path); continue; }
    if (identitySequence(previousMap) === identitySequence(JSON.parse(files[mapPath]!))) continue;
    const synchronized = synchronizeAlignedStoryIdentityMap(previousSource.replace(/\r\n/gu, "\n"),
      files[source.path]!, previousMap, {path: source.path, mapPath,
        isAllowedEdit: (before, after) => equivalent(before, after)});
    if (synchronized.valid && synchronized.map !== undefined) {
      files[mapPath] = serializeStoryIdentityMap(synchronized.map);
      preservedIdentities += 1;
    } else regenerated.push(source.path);
  }
  let written = 0;
  for (const [path, text] of Object.entries(files))
    if (await writeIfChanged(join(root, ...path.split("/")), text)) written += 1;
  return {files: Object.keys(files).length, written, preservedIdentities, regenerated};
}
