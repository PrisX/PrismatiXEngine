import {readdir, readFile, rmdir, stat, unlink} from "node:fs/promises";
import {join, relative, resolve, sep} from "node:path";

import {createStoryAssetIndex, type StoryAssetRoots} from "@prismatix/authoring-sdk";

import {ImportFailure, publishImportedStory, writeIfChanged} from "../publish.js";
import {StoryForms} from "../story-forms.js";
import {field, isTable, numbered, readArtemisTables} from "./ast.js";
import {artemisImportConfigFile, loadArtemisImportConfig, type ArtemisImportConfig} from "./config.js";
import {ArtemisConverter, joinProjectPath, ProjectFiles, type ArtemisSystemDefaults} from "./converter.js";

export interface ArtemisImportOptions {
  /** The extracted game: `script/*.ast` and `system/table/list_windows.tbl`. */
  readonly extracted: string;
  /** A PrismatiX project whose `originalRoot` mirrors the extracted media. */
  readonly project: string;
  /** Defaults to `<project>/artemis-import.json` when that file exists. */
  readonly config?: string;
  /** Spell every command with named arguments (the pre-0.2 importer output). */
  readonly longForms?: boolean;
  readonly log?: (message: string) => void;
}

export interface ArtemisImportSummary {
  readonly scenes: number;
  readonly dialogue: number;
  readonly voices: number;
  readonly characters: number;
  readonly expressions: number;
  readonly unsupported: number;
  readonly removedPortraitFiles: number;
  readonly storyFiles: number;
  readonly storyFilesWritten: number;
  readonly preservedIdentities: number;
  readonly regeneratedIdentities: readonly string[];
  readonly report: string;
}

type Json = Record<string, unknown>;

async function readJson(path: string): Promise<Json> {
  const text = await readFile(path, "utf8");
  const value = JSON.parse(text.charCodeAt(0) === 0xfeff ? text.slice(1) : text) as unknown;
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new ImportFailure(`${path} must contain a JSON object`);
  return value as Json;
}

async function isDirectory(path: string): Promise<boolean> {
  try { return (await stat(path)).isDirectory(); } catch { return false; }
}

/** Project-relative paths of every file below `directory` (following links). */
async function listFiles(root: string, directory: string, found: string[]): Promise<void> {
  let entries;
  try { entries = await readdir(join(root, ...directory.split("/")), {withFileTypes: true}); }
  catch { return; }
  for (const entry of entries) {
    const path = `${directory}/${entry.name}`;
    if (entry.isDirectory() || (entry.isSymbolicLink() && await isDirectory(join(root, ...path.split("/")))))
      await listFiles(root, path, found);
    else found.push(path);
  }
}

/** Game-wide defaults from list_windows.tbl `init`, then the project config. */
async function systemDefaults(extracted: string, config: ArtemisImportConfig,
  manifest: Json): Promise<ArtemisSystemDefaults> {
  let init: ReturnType<typeof field> = null;
  try { init = (await readArtemisTables(join(extracted, "system", "table", "list_windows.tbl"))).get("init") ?? null; }
  catch (error) { if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error; }
  const number = (key: string, fallback: number): number => {
    const value = field(init, key);
    return typeof value === "number" ? value : fallback;
  };
  const scale = numbered(field(init, "game_scale"));
  const resolution = manifest.resolution as {width?: unknown; height?: unknown} | undefined;
  const screen: [number, number] = config.screen !== undefined ? [config.screen[0], config.screen[1]]
    : typeof scale[0] === "number" && typeof scale[1] === "number" ? [scale[0], scale[1]]
    : typeof resolution?.width === "number" && typeof resolution.height === "number"
      ? [resolution.width, resolution.height] : [1920, 1080];
  const sizes = numbered(field(init, "fontsize"));
  const fontSizes = new Map<number, number>();
  (sizes.length > 0 ? sizes : [50, 75, 100, 150, 200]).forEach((value, index) => {
    if (typeof value === "number") fontSizes.set(index + 1, value);
  });
  return {bgFadeMs: config.bgFadeMs ?? number("bg_fade", 1000), fgFadeMs: config.fgFadeMs ?? number("fg_fade", 250),
    vague: config.vague ?? number("vague", 40), screen, fontSizes};
}

/** Search roots for short Story names when the config names none. */
async function defaultStoryAssets(project: string, original: string): Promise<StoryAssetRoots> {
  const existing = async (...paths: string[]) => {
    const found: string[] = [];
    for (const path of paths) if (await isDirectory(join(project, ...path.split("/")))) found.push(path);
    return found;
  };
  const roots: Record<string, string[]> = {
    bg: await existing(`${original}/image/bg`, `${original}/image/cg`, `${original}/image/ev`),
    rule: await existing(`${original}/image/rule`),
    bgm: await existing(`${original}/sound/bgm`),
    se: await existing(`${original}/sound/se`),
    voice: await existing(`${original}/sound/vo`),
    video: await existing(`${original}/movie`),
  };
  return Object.fromEntries(Object.entries(roots).filter(([, paths]) => paths.length > 0));
}

const fold = (path: string) => process.platform === "win32" ? resolve(path).toLowerCase() : resolve(path);

/** Remove importer-owned portrait files this run no longer references. */
async function prunePortraits(directory: string, keep: ReadonlySet<string>): Promise<number> {
  let removed = 0;
  const visit = async (path: string): Promise<boolean> => {
    let entries;
    try { entries = await readdir(path, {withFileTypes: true}); } catch { return false; }
    let empty = true;
    for (const entry of entries) {
      const child = join(path, entry.name);
      if (entry.isDirectory()) { if (!await visit(child)) empty = false; continue; }
      if (keep.has(fold(child))) { empty = false; continue; }
      await unlink(child);
      removed += 1;
    }
    if (empty) await rmdir(path);
    return empty;
  };
  await visit(directory);
  return removed;
}

/** `prismatix import artemis`: translate an extracted Artemis game into the project. */
export async function importArtemis(options: ArtemisImportOptions): Promise<ArtemisImportSummary> {
  const extracted = resolve(options.extracted);
  const project = resolve(options.project);
  const scriptDirectory = join(extracted, "script");
  if (!await isDirectory(scriptDirectory))
    throw new ImportFailure(`${extracted} has no script/ folder; pass the extracted Artemis game directory`);
  const manifestPath = join(project, "prismatix.json");
  let manifest: Json;
  try { manifest = await readJson(manifestPath); }
  catch (error) {
    throw error instanceof ImportFailure ? error
      : new ImportFailure(`${project} has no readable prismatix.json: ${String(error)}`);
  }
  const config = await loadArtemisImportConfig(resolve(options.config ?? join(project, artemisImportConfigFile)),
    options.config !== undefined);
  const system = await systemDefaults(extracted, config, manifest);
  const storyAssets = config.storyAssets ?? await defaultStoryAssets(project, config.originalRoot);
  const locale = typeof manifest.defaultLocale === "string" ? manifest.defaultLocale : "ja-JP";

  const scripts = (await readdir(scriptDirectory)).filter((name) => /\.ast$/iu.test(name))
    .sort((left, right) => left.toLowerCase() < right.toLowerCase() ? -1
      : left.toLowerCase() > right.toLowerCase() ? 1 : left < right ? -1 : left > right ? 1 : 0);
  if (scripts.length === 0) throw new ImportFailure(`${scriptDirectory} contains no .ast scenarios`);

  const paths: string[] = [];
  const roots = new Set([config.originalRoot, config.portraitRoot, config.galleryThumbnails,
    ...Object.values(storyAssets).flat()]);
  for (const root of roots) await listFiles(project, root, paths);
  const unique = [...new Set(paths)];
  const files = new ProjectFiles(unique);
  const assetIndex = createStoryAssetIndex(storyAssets, unique);
  const forms = new StoryForms(options.longForms === true);
  const converter = new ArtemisConverter({extracted, project, config, system, files, storyAssets: assetIndex, storyAssetRoots: storyAssets,
    forms, locale, scripts, ...(options.log === undefined ? {} : {log: options.log})});
  await converter.prepare();
  const scenes = [];
  for (const script of scripts) scenes.push(await converter.convert(script));

  const catalogPath = typeof manifest.gameCatalog === "string" ? manifest.gameCatalog : "Content/game.pxgame";
  let previousCatalog: Json | undefined;
  try { previousCatalog = await readJson(join(project, ...catalogPath.split("/"))); } catch { previousCatalog = undefined; }
  const game = {format: "PrismatiXGame", schemaRevision: 3,
    variables: [...converter.variables].sort().map((name) => ({name, type: "integer", default: 0,
      scope: name.startsWith("g_") ? "profile" : "session"})),
    progressionFlags: converter.gallery.map((entry) => entry.id),
    inputActions: config.inputActions ?? previousCatalog?.inputActions ?? [],
    gallery: converter.gallery, unlockables: []};
  await writeIfChanged(join(project, ...catalogPath.split("/")), `${JSON.stringify(game, null, 2)}\n`);

  const characters = [];
  for (const alias of [...converter.expressions.keys()].sort()) {
    const displayName = converter.nameByAlias.get(alias) ?? alias;
    const expressions = [...converter.expressions.get(alias)!].sort((left, right) =>
      left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0);
    const document = {format: "PrismatiXCharacter", schemaRevision: 2, revision: 1, displayName, aliases: [alias],
      expressions: expressions.map(([expression, assetPath]) => ({name: `${displayName} ${expression}`,
        aliases: [expression], assetPath, thumbnailAssetId: null}))};
    const source = joinProjectPath(config.charactersRoot, `${alias}.pxcharacter`);
    await writeIfChanged(join(project, ...source.split("/")), `${JSON.stringify(document, null, 1)}\n`);
    characters.push({alias, displayName, source});
  }
  const updatedManifest = {...manifest, characters, storyAssets};
  if (JSON.stringify(updatedManifest) !== JSON.stringify(manifest))
    await writeIfChanged(manifestPath, `${JSON.stringify(updatedManifest, null, 2)}\n`);

  // The portrait folder is importer-owned: drop descriptors and crops this run no longer references.
  const removedPortraitFiles = await prunePortraits(join(project, ...config.portraitRoot.split("/")),
    new Set([...converter.portraitFiles].map(fold)));

  const chapters = [...new Set(scenes.map((scene) => scene.chapter))].sort();
  const entryScene = config.entryScene ??
    (typeof (manifest.entry as Json | undefined)?.story === "string" ? (manifest.entry as Json).story as string
      : scenes[0]!.id);
  const published = await publishImportedStory(project, {
    id: config.storyId ?? `${typeof manifest.id === "string" ? manifest.id : "imported"}-story`,
    entryScene, locales: [locale],
    chapters: chapters.map((id) => ({id, title: id.replaceAll("_", " ")})),
    scenes: scenes.map((scene) => ({id: scene.id, chapter: scene.chapter,
      sources: {[locale]: {path: scene.path, lines: scene.lines}}})),
    ...(typeof manifest.storyIndex === "string" ? {indexPath: manifest.storyIndex} : {}),
  }, (previous, current) => forms.equivalent(previous, current));

  const expressionCount = [...converter.expressions.values()].reduce((sum, map) => sum + map.size, 0);
  const report = {sourceScripts: scripts.length, importedScenes: scenes.length,
    dialogueOperations: converter.dialogueCount, voiceOperations: converter.voiceCount,
    portraitExpressions: expressionCount, characters: converter.expressions.size,
    galleryDeclaredVariants: converter.galleryDeclared, galleryImportedVariants: converter.gallery.length,
    galleryMissingVariants: converter.galleryMissing,
    commandCounts: converter.counts.ordered(), unsupported: converter.unsupported.ordered(),
    missingAssets: converter.missing.ordered()};
  await writeIfChanged(join(project, ...config.report.split("/")), `${JSON.stringify(report, null, 2)}\n`);
  return {scenes: scenes.length, dialogue: converter.dialogueCount, voices: converter.voiceCount,
    characters: characters.length, expressions: expressionCount, unsupported: converter.unsupported.total(),
    removedPortraitFiles, storyFiles: published.files, storyFilesWritten: published.written,
    preservedIdentities: published.preservedIdentities, regeneratedIdentities: published.regenerated,
    report: relative(project, join(project, ...config.report.split("/"))).split(sep).join("/")};
}
