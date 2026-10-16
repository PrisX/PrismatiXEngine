import {mkdir, readdir, readFile, writeFile} from "node:fs/promises";
import {basename, extname, join, relative, resolve, sep} from "node:path";

/**
 * `prismatix add character|scene`: the files an author would otherwise
 * write by hand, registered in prismatix.json / the story index.
 */
export interface ScaffoldResult {
  readonly created: readonly string[];
  readonly updated: readonly string[];
}

type Json = Record<string, unknown>;
const identifier = /^[A-Za-z][A-Za-z0-9_-]{0,63}$/u;
const imageExtensions = new Set([".png", ".webp", ".jpg", ".jpeg", ".pxportrait"]);

async function readObject(path: string): Promise<Json> {
  const text = await readFile(path, "utf8");
  const value = JSON.parse(text.startsWith("﻿") ? text.slice(1) : text) as unknown;
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error(`${path} must contain a JSON object`);
  return value as Json;
}

async function writeJson(path: string, value: unknown): Promise<void> {
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`, "utf8");
}

async function exists(path: string): Promise<boolean> {
  return readFile(path).then(() => true, () => false);
}

function projectRelative(root: string, path: string): string {
  const value = relative(root, path);
  if (value.startsWith("..") || resolve(root, value) !== resolve(path))
    throw new Error(`${path} is outside the project`);
  return value.split(sep).join("/");
}

/** File stems that mark the expression a character shows by default. */
const defaultStems = ["default", "normal", "neutral", "base", "idle"];

/**
 * Registers a character whose expressions are the images in one folder, or
 * a character with no sprite (a narrator or the protagonist) without `from`.
 */
export async function addCharacter(root: string, alias: string, options: {
  readonly from?: string; readonly name?: string;
}): Promise<ScaffoldResult> {
  if (!identifier.test(alias)) throw new Error(`Character alias must be an identifier: ${alias}`);
  const manifestPath = join(root, "prismatix.json");
  const manifest = await readObject(manifestPath);
  const characters = Array.isArray(manifest.characters) ? manifest.characters as Json[] : [];
  const displayName = options.name ?? alias;
  const taken = characters.find((character) =>
    character.alias === alias || character.displayName === displayName);
  if (taken !== undefined)
    throw new Error(`prismatix.json already registers ${String(taken.alias)} (${String(taken.displayName)}); ` +
      "Story addresses characters by alias and display name, so both must be unique");
  const folder = options.from === undefined ? undefined : resolve(root, options.from);
  const images = folder === undefined ? [] : (await readdir(folder, {withFileTypes: true}))
    .filter((entry) => entry.isFile() && imageExtensions.has(extname(entry.name).toLowerCase()))
    .map((entry) => entry.name).sort((left, right) => left.localeCompare(right));
  if (folder !== undefined && images.length === 0)
    throw new Error(`No .png, .webp, .jpg or .pxportrait images in ${options.from}`);
  // `normal.png` (or default/neutral/...) is the default expression; otherwise the first file.
  const stemOf = (file: string) => basename(file, extname(file)).toLowerCase();
  const preferred = defaultStems.map((stem) => images.find((file) => stemOf(file) === stem))
    .find((file) => file !== undefined) ?? images[0];
  const expressionNames = new Set<string>();
  const expressions = images.map((file) => {
    const stem = basename(file, extname(file));
    const name = expressionNames.has(stem) ? `${stem}-${extname(file).slice(1)}` : stem;
    expressionNames.add(name);
    // Story addresses an expression by its name (`[show kei smile]`) or alias.
    return {name, aliases: file === preferred && name.toLowerCase() !== "default" ? ["default"] : [],
      assetPath: projectRelative(root, join(folder!, file)), thumbnailAssetId: null};
  });
  const source = `Characters/${alias}.pxcharacter`;
  const characterPath = join(root, source);
  if (await exists(characterPath)) throw new Error(`${source} already exists`);
  await mkdir(join(root, "Characters"), {recursive: true});
  await writeJson(characterPath, {format: "PrismatiXCharacter", schemaRevision: 1, revision: 1,
    displayName, aliases: [alias], expressions});
  manifest.characters = [...characters, {alias, displayName, source}];
  await writeJson(manifestPath, manifest);
  return {created: [source], updated: ["prismatix.json"]};
}

/** Adds an empty scene to the story index for every supported locale. */
export async function addScene(root: string, id: string, options: {
  readonly chapter?: string; readonly title?: string;
}): Promise<ScaffoldResult> {
  if (!identifier.test(id)) throw new Error(`Scene id must be an identifier: ${id}`);
  const manifest = await readObject(join(root, "prismatix.json"));
  const indexPath = typeof manifest.storyIndex === "string" ? manifest.storyIndex : "Story/story.pxindex";
  if (!(await exists(join(root, indexPath))))
    throw new Error(`${indexPath} does not exist; a project with a single Story/main.pxstory ` +
      "has no scene list yet. Create the index first (see docs/story-reference.md).");
  const index = await readObject(join(root, indexPath));
  const scenes = Array.isArray(index.scenes) ? index.scenes as Json[] : [];
  const chapters = Array.isArray(index.chapters) ? index.chapters as Json[] : [];
  if (scenes.some((scene) => scene.id === id)) throw new Error(`${indexPath} already has scene ${id}`);
  const locales = Array.isArray(manifest.supportedLocales) && manifest.supportedLocales.length > 0
    ? manifest.supportedLocales.filter((locale): locale is string => typeof locale === "string")
    : [typeof manifest.defaultLocale === "string" ? manifest.defaultLocale : "zh-TW"];
  const sources: Record<string, string> = {};
  const created: string[] = [];
  for (const locale of locales) {
    const source = `Story/${locale}/${id}.pxstory`;
    if (await exists(join(root, source))) throw new Error(`${source} already exists`);
    sources[locale] = source;
  }
  for (const source of Object.values(sources)) {
    await mkdir(join(root, source, ".."), {recursive: true});
    // scene=next continues with the following scene of the story index (the
    // game ends after the last one), so adding or reordering scenes needs no
    // jump edits; replace it with [end] or a jump of your own where needed.
    await writeFile(join(root, source), `*top\n; ${options.title ?? id}\n[jump scene=next]\n`, "utf8");
    created.push(source);
  }
  const chapterId = options.chapter ?? (typeof chapters.at(-1)?.id === "string"
    ? chapters.at(-1)!.id as string : undefined);
  let chapter = chapters.find((entry) => entry.id === chapterId);
  if (chapter === undefined) {
    if (chapterId === undefined) throw new Error("Name the chapter with --chapter");
    chapter = {id: chapterId, title: chapterId, scenes: []};
    chapters.push(chapter);
  }
  chapter.scenes = [...Array.isArray(chapter.scenes) ? chapter.scenes : [], id];
  index.chapters = chapters;
  index.scenes = [...scenes, {id, ...(options.title === undefined ? {} : {title: options.title}), sources}];
  await writeJson(join(root, indexPath), index);
  return {created, updated: [indexPath]};
}
