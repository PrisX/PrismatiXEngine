import {readFile} from "node:fs/promises";

import {storyAssetRootsError, type StoryAssetRoots} from "@prismatix/authoring-sdk";

/**
 * `artemis-import.json`: everything about one Artemis game that the generic
 * Artemis translation cannot know. All fields are optional; a game whose
 * scripts only use stock Artemis tags imports without this file.
 *
 * {
 *   "format": "PrismatiXArtemisImport", "schemaRevision": 1,
 *   "story": {"id": "my-story", "entryScene": "start", "language": "ja",
 *             "chapters": [{"match": "^(\\d\\d)_", "id": "chapter_$1"}],
 *             "defaultChapter": "system"},
 *   "assets": {"original": "Assets/Original", "portraits": "Assets/Portraits"},
 *   "storyAssets": {"bg": ["Assets/Original/image/bg"], ...},
 *   "actPresets": {"びっくり": "hop"},
 *   "ignoredCommands": ["cacheclear"],
 *   "movies": {"extension": ".wmv", "files": {"op": "opening"}},
 *   "replayScenes": "^scene\\d\\d$",
 *   "characterAliases": {"Display name": "alias"},
 *   "gallery": {"thumbnails": "Assets/Gallery/Thumbnails", "thumbnailSize": [320, 180]},
 *   "inputActions": [...],
 *   "report": "docs/conversion-report.json"
 * }
 */
export const artemisImportConfigFile = "artemis-import.json";

export interface ArtemisChapterRule {
  readonly match: RegExp;
  readonly id: string;
}

export interface ArtemisImportConfig {
  readonly storyId?: string;
  readonly entryScene?: string;
  readonly language: string;
  readonly chapters: readonly ArtemisChapterRule[];
  readonly defaultChapter: string;
  /** Project folder that mirrors the extracted archive (image/, sound/, ...). */
  readonly originalRoot: string;
  /** Importer-owned folder of layered portrait descriptors. */
  readonly portraitRoot: string;
  readonly charactersRoot: string;
  readonly storyAssets?: StoryAssetRoots;
  readonly actPresets: ReadonlyMap<string, string>;
  readonly ignoredCommands: ReadonlySet<string>;
  readonly movieExtension: string;
  readonly movies: ReadonlyMap<string, string>;
  readonly replayScenes?: RegExp;
  readonly characterAliases: ReadonlyMap<string, string>;
  readonly galleryThumbnails: string;
  readonly thumbnailSize: readonly [number, number];
  readonly inputActions?: readonly unknown[];
  readonly report: string;
  /** System table overrides; normally read from system/table/list_windows.tbl. */
  readonly bgFadeMs?: number;
  readonly fgFadeMs?: number;
  readonly vague?: number;
  readonly screen?: readonly [number, number];
}

export class ArtemisConfigError extends Error {}

type Json = Record<string, unknown>;
const projectPath = /^(?!\/)(?!.*(?:^|\/)\.\.?(?:\/|$))[^\\:\u0000-\u001f]+$/u;
const identifier = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;

function object(value: unknown, where: string): Json {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new ArtemisConfigError(`${where} must be an object`);
  return value as Json;
}

function only(value: Json, where: string, keys: readonly string[]): void {
  for (const key of Object.keys(value))
    if (!keys.includes(key)) throw new ArtemisConfigError(`${where}.${key} is not a known setting`);
}

function text(value: unknown, where: string, pattern?: RegExp): string {
  if (typeof value !== "string" || value.length === 0 || (pattern !== undefined && !pattern.test(value)))
    throw new ArtemisConfigError(`${where} must be ${pattern === projectPath ? "a project-relative path"
      : pattern === identifier ? "a stable identifier" : "a nonempty string"}`);
  return value;
}

function regex(value: unknown, where: string): RegExp {
  const source = text(value, where);
  try { return new RegExp(source, "u"); }
  catch (error) { throw new ArtemisConfigError(`${where} is not a valid regular expression: ${String(error)}`); }
}

function stringMap(value: unknown, where: string, pattern?: RegExp): Map<string, string> {
  const result = new Map<string, string>();
  for (const [key, item] of Object.entries(object(value, where))) result.set(key, text(item, `${where}.${key}`, pattern));
  return result;
}

function milliseconds(value: unknown, where: string): number {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0)
    throw new ArtemisConfigError(`${where} must be a nonnegative number`);
  return value;
}

function size(value: unknown, where: string): [number, number] {
  if (!Array.isArray(value) || value.length !== 2 ||
      !value.every((item) => Number.isInteger(item) && (item as number) > 0 && (item as number) <= 16384))
    throw new ArtemisConfigError(`${where} must be [width, height] in pixels`);
  return [value[0] as number, value[1] as number];
}

export function parseArtemisImportConfig(value: unknown, where = artemisImportConfigFile): ArtemisImportConfig {
  const root = object(value ?? {}, where);
  only(root, where, ["$schema", "format", "schemaRevision", "story", "assets", "storyAssets", "actPresets",
    "ignoredCommands", "movies", "replayScenes", "characterAliases", "gallery", "inputActions", "report",
    "timing", "screen"]);
  if (root.format !== undefined && root.format !== "PrismatiXArtemisImport")
    throw new ArtemisConfigError(`${where}.format must be "PrismatiXArtemisImport"`);
  if (root.schemaRevision !== undefined && root.schemaRevision !== 1)
    throw new ArtemisConfigError(`${where}.schemaRevision ${String(root.schemaRevision)} is not supported; expected 1`);
  const story = object(root.story ?? {}, `${where}.story`);
  only(story, `${where}.story`, ["id", "entryScene", "language", "chapters", "defaultChapter"]);
  const chapters = (story.chapters ?? []) as unknown;
  if (!Array.isArray(chapters)) throw new ArtemisConfigError(`${where}.story.chapters must be an array`);
  const assets = object(root.assets ?? {}, `${where}.assets`);
  only(assets, `${where}.assets`, ["original", "portraits", "characters"]);
  const movies = object(root.movies ?? {}, `${where}.movies`);
  only(movies, `${where}.movies`, ["extension", "files"]);
  const gallery = object(root.gallery ?? {}, `${where}.gallery`);
  only(gallery, `${where}.gallery`, ["thumbnails", "thumbnailSize"]);
  const timing = object(root.timing ?? {}, `${where}.timing`);
  only(timing, `${where}.timing`, ["bgFadeMs", "fgFadeMs", "vague"]);
  if (root.storyAssets !== undefined) {
    const problem = storyAssetRootsError(root.storyAssets);
    if (problem !== undefined) throw new ArtemisConfigError(`${where}: ${problem}`);
  }
  if (root.inputActions !== undefined && !Array.isArray(root.inputActions))
    throw new ArtemisConfigError(`${where}.inputActions must be an array of game input actions`);
  const ignored = root.ignoredCommands ?? [];
  if (!Array.isArray(ignored) || !ignored.every((item) => typeof item === "string" && item.length > 0))
    throw new ArtemisConfigError(`${where}.ignoredCommands must be an array of Artemis tag names`);
  const extension = movies.extension === undefined ? ".wmv" : text(movies.extension, `${where}.movies.extension`);
  if (!/^\.[A-Za-z0-9]+$/u.test(extension))
    throw new ArtemisConfigError(`${where}.movies.extension must look like ".wmv"`);
  return {
    ...(story.id === undefined ? {} : {storyId: text(story.id, `${where}.story.id`, identifier)}),
    ...(story.entryScene === undefined ? {} : {entryScene: text(story.entryScene, `${where}.story.entryScene`)}),
    language: story.language === undefined ? "ja" : text(story.language, `${where}.story.language`),
    chapters: chapters.map((rule, index) => {
      const authored = object(rule, `${where}.story.chapters[${index}]`);
      only(authored, `${where}.story.chapters[${index}]`, ["match", "id"]);
      return {match: regex(authored.match, `${where}.story.chapters[${index}].match`),
        id: text(authored.id, `${where}.story.chapters[${index}].id`)};
    }),
    defaultChapter: story.defaultChapter === undefined ? "main"
      : text(story.defaultChapter, `${where}.story.defaultChapter`, identifier),
    originalRoot: assets.original === undefined ? "Assets/Original"
      : text(assets.original, `${where}.assets.original`, projectPath).replace(/\/+$/u, ""),
    portraitRoot: assets.portraits === undefined ? "Assets/Portraits"
      : text(assets.portraits, `${where}.assets.portraits`, projectPath).replace(/\/+$/u, ""),
    charactersRoot: assets.characters === undefined ? "Characters"
      : text(assets.characters, `${where}.assets.characters`, projectPath).replace(/\/+$/u, ""),
    ...(root.storyAssets === undefined ? {} : {storyAssets: root.storyAssets as StoryAssetRoots}),
    actPresets: root.actPresets === undefined ? new Map() : stringMap(root.actPresets, `${where}.actPresets`),
    ignoredCommands: new Set(ignored as string[]),
    movieExtension: extension,
    movies: movies.files === undefined ? new Map() : stringMap(movies.files, `${where}.movies.files`),
    ...(root.replayScenes === undefined ? {} : {replayScenes: regex(root.replayScenes, `${where}.replayScenes`)}),
    characterAliases: root.characterAliases === undefined ? new Map()
      : stringMap(root.characterAliases, `${where}.characterAliases`, identifier),
    galleryThumbnails: gallery.thumbnails === undefined ? "Assets/Gallery/Thumbnails"
      : text(gallery.thumbnails, `${where}.gallery.thumbnails`, projectPath).replace(/\/+$/u, ""),
    thumbnailSize: gallery.thumbnailSize === undefined ? [320, 180]
      : size(gallery.thumbnailSize, `${where}.gallery.thumbnailSize`),
    ...(root.inputActions === undefined ? {} : {inputActions: root.inputActions}),
    report: root.report === undefined ? "docs/conversion-report.json"
      : text(root.report, `${where}.report`, projectPath),
    ...(timing.bgFadeMs === undefined ? {} : {bgFadeMs: milliseconds(timing.bgFadeMs, `${where}.timing.bgFadeMs`)}),
    ...(timing.fgFadeMs === undefined ? {} : {fgFadeMs: milliseconds(timing.fgFadeMs, `${where}.timing.fgFadeMs`)}),
    ...(timing.vague === undefined ? {} : {vague: milliseconds(timing.vague, `${where}.timing.vague`)}),
    ...(root.screen === undefined ? {} : {screen: size(root.screen, `${where}.screen`)}),
  };
}

/** Read the project's config, or defaults when the file does not exist. */
export async function loadArtemisImportConfig(path: string, required: boolean): Promise<ArtemisImportConfig> {
  let source: string;
  try { source = await readFile(path, "utf8"); }
  catch (error) {
    if (!required && (error as NodeJS.ErrnoException).code === "ENOENT") return parseArtemisImportConfig({});
    throw new ArtemisConfigError(`Cannot read ${path}: ${String(error)}`);
  }
  let value: unknown;
  try { value = JSON.parse(source.charCodeAt(0) === 0xfeff ? source.slice(1) : source); }
  catch (error) { throw new ArtemisConfigError(`${path} is not valid JSON: ${String(error)}`); }
  return parseArtemisImportConfig(value, path);
}
