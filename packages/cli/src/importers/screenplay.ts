// `prismatix import screenplay`: a writers' Markdown draft (one heading per
// scene, a speaker tag before what each character says, cut passages,
// letters) plus a staging table in screenplay-import.json becomes Story
// scenes. The draft stays the source of truth: re-import it after the
// writers edit it, and the staging finds its lines again by text.
import {readFile} from "node:fs/promises";
import {isAbsolute, join, relative, resolve, sep} from "node:path";

import {canonicalJson, externalizeStoryIds, parseStory, serializeStoryIdentityMap,
  storyIdentityMapMarker, synchronizeStoryIdentityMap, validateDocument} from "@prismatix/authoring-sdk";

import {ImportFailure, writeIfChanged} from "./publish.js";

type Json = Record<string, unknown>;

export type ScreenplayCue =
  | {readonly at: string; readonly after?: undefined; readonly commands: readonly string[]}
  | {readonly after: string; readonly at?: undefined; readonly commands: readonly string[]};

/** Staging for one scene, matched to the draft by order. */
export interface ScreenplayScene {
  readonly id?: string;
  readonly title?: string;
  /** Shown by `[chapter ...]` when the scene starts. */
  readonly chapterTitle?: string;
  /** Story lines before the first prose line. */
  readonly start?: readonly string[];
  /** Story lines inserted before the first line (after the previous cue)
   * whose draft text contains `at`, or after the one containing `after`
   * (a choice that follows a question, say). */
  readonly cues?: readonly ScreenplayCue[];
  /** Story lines after the last prose line, before the jump to the next scene. */
  readonly end?: readonly string[];
}

export interface ScreenplayConfig {
  readonly locale?: string;
  readonly chapter?: {readonly id: string; readonly title: string};
  /** A line that starts a scene; capture group 1 is its heading. */
  readonly sceneHeading?: string;
  /** A line that only names who speaks the following lines (capture 1). */
  readonly speakerTag?: string;
  /** A line that is a name and what they say ("凪：……"): captures 1 and 2. */
  readonly speakerLine?: string;
  /** Names in the draft -> character aliases; other names stay literal. */
  readonly speakers?: Readonly<Record<string, string>>;
  /** Names whose lines are narration. */
  readonly narrators?: readonly string[];
  /** Cut passages: everything from the first marker to the second is dropped. */
  readonly cut?: readonly [string, string] | null;
  /** A line holding only this opens and closes a letter (an NVL page). */
  readonly letter?: string | null;
  /** Scene ids when `scenes` does not name them: <prefix>01, <prefix>02, ... */
  readonly idPrefix?: string;
  /** A label at the top of every scene, for journeys and jumps. */
  readonly entryLabel?: string;
  readonly scenes?: readonly ScreenplayScene[];
}

export interface ScreenplaySummary {
  readonly scenes: number;
  readonly lines: number;
  readonly unmatchedCues: readonly string[];
  readonly storyFiles: number;
  readonly storyFilesWritten: number;
  readonly regeneratedIdentities: readonly string[];
}

const identifier = /^[A-Za-z][A-Za-z0-9_]*$/u;

function fail(message: string): never {
  throw new ImportFailure(message);
}

function regex(value: unknown, fallback: string, name: string): RegExp {
  if (value !== undefined && typeof value !== "string") fail(`screenplay ${name} must be a regular expression string`);
  try { return new RegExp(value ?? fallback, "u"); }
  catch (error) { return fail(`screenplay ${name} is not a valid regular expression: ${String(error)}`); }
}

function lines(value: unknown, name: string): readonly string[] {
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.some((line) => typeof line !== "string" || /[\r\n]/u.test(line)))
    fail(`${name} must be a list of single Story lines`);
  return value as string[];
}

/** Prose as Story text: brackets and braces would start commands or
 * variables, and a leading `*` a label, so they become their fullwidth
 * forms; a line opened by `*` (or wrapped in `_..._`) is shown in italics. */
export function screenplayProse(text: string): string {
  let line = text.trim();
  // Writers often open an inner voice with `*` and never close it; `* `
  // with a space is a Markdown bullet and stays text.
  const italic = /^\*(?![*\s])(.+?)\*?$|^_(.+?)_$/u.exec(line);
  const body = italic ? (italic[1] ?? italic[2]!).trim() : line;
  line = body.replace(/\[/gu, "［").replace(/\]/gu, "］").replace(/\{/gu, "｛").replace(/\}/gu, "｝")
    .replace(/^[*@;\-]/u, (first) => ({"*": "＊", "@": "＠", ";": "；", "-": "－"})[first]!);
  // A trailing backslash would join the next line.
  line = line.replace(/\\$/u, "＼");
  return italic ? `[i]${line}[/i]` : line;
}

interface DraftScene {readonly heading: string; readonly lines: string[]}

export function splitScreenplay(text: string, config: ScreenplayConfig): DraftScene[] {
  const cut = config.cut === null ? undefined : config.cut ?? ["~~", "~~"];
  let draft = text.replace(/^﻿/u, "");
  if (cut !== undefined) {
    const [open, close] = cut;
    for (let start = draft.indexOf(open); start >= 0; start = draft.indexOf(open, start)) {
      const end = draft.indexOf(close, start + open.length);
      if (end < 0) break;
      draft = draft.slice(0, start) + draft.slice(end + close.length);
    }
  }
  const heading = regex(config.sceneHeading, "^#{1,3}\\s+(.+?)\\s*$", "sceneHeading");
  const scenes: DraftScene[] = [];
  for (const line of draft.split(/\r?\n/u)) {
    const match = heading.exec(line);
    if (match) scenes.push({heading: (match[1] ?? match[0]).trim(), lines: []});
    else scenes.at(-1)?.lines.push(line);
  }
  return scenes;
}

/** Translates one draft scene into Story lines (without stable IDs). */
export function screenplayScene(scene: DraftScene, staging: ScreenplayScene,
  config: ScreenplayConfig, unmatched: string[] = []): string[] {
  const speakerTag = regex(config.speakerTag, "^\\[text speaker=\"([^\"]*)\"\\]\\s*$", "speakerTag");
  const speakerLine = config.speakerLine === undefined ? undefined
    : regex(config.speakerLine, "", "speakerLine");
  const speakers = config.speakers ?? {};
  const narrators = new Set(config.narrators ?? []);
  const letterMarker = config.letter === null ? undefined : config.letter ?? "\"";
  const out: string[] = [];
  if (config.entryLabel !== undefined) out.push(`*${config.entryLabel}`);
  if (staging.chapterTitle !== undefined) out.push(`[chapter ${screenplayProse(staging.chapterTitle)}]`);
  out.push(...lines(staging.start, "start"));
  const cues = [...staging.cues ?? []];
  const who = (name: string): string | undefined => {
    const trimmed = name.trim();
    if (narrators.has(trimmed)) return undefined;
    const alias = speakers[trimmed];
    if (alias !== undefined) return alias;
    // A literal name shown as written (stage directions in brackets dropped).
    return `"${trimmed.replace(/[（(].*$/u, "").trim().replaceAll("\"", "”")}"`;
  };
  let speaker: string | undefined;
  let letter = false;
  // The first remaining cue of the kind that this line matches runs; cues
  // listed before it are passed over (and reported).
  const runCue = (raw: string, kind: "at" | "after") => {
    for (let index = 0; index < cues.length; index += 1) {
      const cue = cues[index]!;
      const needle = cue[kind];
      if (needle === undefined || !raw.includes(needle)) continue;
      out.push(...lines(cue.commands, `cue "${needle}"`));
      unmatched.push(...cues.splice(0, index + 1).slice(0, -1).map((skipped) => skipped.at ?? skipped.after!));
      return;
    }
  };
  const emit = (raw: string, by: string | undefined) => {
    const text = screenplayProse(raw);
    if (text === "") return;
    runCue(raw, "at");
    out.push(by === undefined || letter ? text : `@${by} ${text}`);
    runCue(raw, "after");
  };
  for (const raw of scene.lines) {
    const tag = speakerTag.exec(raw);
    if (tag) {
      speaker = who(tag[1] ?? "");
      continue;
    }
    if (letterMarker !== undefined && raw.trim() === letterMarker) {
      letter = !letter;
      out.push(letter ? "[nvl]" : "[adv]");
      continue;
    }
    if (raw.trim() === "") {
      // A blank line inside a letter turns the page.
      if (letter && out.at(-1) !== "[nvl]" && out.at(-1) !== "[nvl.clear]") out.push("[nvl.clear]");
      continue;
    }
    const inline = speakerLine?.exec(raw);
    if (inline && inline[1] !== undefined && inline[2] !== undefined) {
      emit(inline[2], who(inline[1]));
      continue;
    }
    emit(raw, speaker);
  }
  if (letter) out.push("[adv]");
  unmatched.push(...cues.map((cue) => cue.at ?? cue.after!));
  out.push(...lines(staging.end, "end"));
  // The order lives in the story index; the last scene ends the story.
  out.push("[jump scene=next]");
  return out;
}

/** The Story source and identity sidecar for one scene. A source that was
 * imported before keeps its operations' identities wherever the edit can be
 * matched (unchanged lines around inserted or rewritten ones), so saves on
 * them still resume; otherwise the scene gets fresh identities. */
export function identifyScreenplayScene(body: string, path: string,
  previous?: {readonly map: string}): {source: string; map: string; regenerated: boolean} {
  const document = parseStory(body, path);
  const errors = document.diagnostics.filter((item) => item.severity === "error");
  if (errors.length > 0) throw new ImportFailure(`${path} is not valid Story`, errors);
  if (previous !== undefined) {
    let old: unknown;
    try { old = JSON.parse(previous.map); } catch { old = undefined; }
    const marked = `${storyIdentityMapMarker}\n${body}`;
    const synchronized = old === undefined ? undefined
      : synchronizeStoryIdentityMap(marked, old, {path});
    if (synchronized?.valid && synchronized.map !== undefined)
      return {source: marked, map: serializeStoryIdentityMap(synchronized.map), regenerated: false};
  }
  const fresh = externalizeStoryIds(body, {path});
  if (!fresh.valid || fresh.source === undefined || fresh.map === undefined)
    throw new ImportFailure(`${path} could not be given save identities`, fresh.diagnostics);
  return {source: fresh.source, map: serializeStoryIdentityMap(fresh.map),
    regenerated: previous !== undefined};
}

function sceneConfig(value: unknown, index: number): ScreenplayScene {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    fail(`screenplay scenes[${index}] must be an object`);
  const scene = value as Json;
  for (const key of Object.keys(scene))
    if (!["id", "title", "chapterTitle", "start", "cues", "end"].includes(key))
      fail(`screenplay scenes[${index}] has an unknown field ${key}`);
  if (scene.id !== undefined && (typeof scene.id !== "string" || !identifier.test(scene.id)))
    fail(`screenplay scenes[${index}].id must be an identifier`);
  for (const key of ["title", "chapterTitle"] as const)
    if (scene[key] !== undefined && typeof scene[key] !== "string")
      fail(`screenplay scenes[${index}].${key} must be text`);
  if (scene.cues !== undefined && (!Array.isArray(scene.cues) || scene.cues.some((cue: unknown) =>
    cue === null || typeof cue !== "object" ||
    (typeof (cue as Json).at === "string") === (typeof (cue as Json).after === "string") ||
    !Array.isArray((cue as Json).commands))))
    fail(`screenplay scenes[${index}].cues must be a list of {at or after, commands}`);
  return scene as ScreenplayScene;
}

export async function readScreenplayConfig(path: string | undefined): Promise<ScreenplayConfig> {
  if (path === undefined) return {};
  let value: unknown;
  try { value = JSON.parse((await readFile(path, "utf8")).replace(/^﻿/u, "")); }
  catch (error) { return fail(`Cannot read ${path}: ${error instanceof Error ? error.message : String(error)}`); }
  if (value === null || typeof value !== "object" || Array.isArray(value))
    fail(`${path} must hold a JSON object`);
  const config = value as Json;
  const known = ["locale", "chapter", "sceneHeading", "speakerTag", "speakerLine", "speakers",
    "narrators", "cut", "letter", "idPrefix", "entryLabel", "scenes", "$schema"];
  for (const key of Object.keys(config)) if (!known.includes(key)) fail(`${path}: unknown field ${key}`);
  if (config.scenes !== undefined && !Array.isArray(config.scenes)) fail(`${path}: scenes must be a list`);
  (config.scenes as unknown[] | undefined)?.forEach(sceneConfig);
  if (config.entryLabel !== undefined && (typeof config.entryLabel !== "string" ||
      !identifier.test(config.entryLabel)))
    fail(`${path}: entryLabel must be an identifier`);
  if (config.idPrefix !== undefined && (typeof config.idPrefix !== "string" ||
      !identifier.test(config.idPrefix)))
    fail(`${path}: idPrefix must start an identifier`);
  return config as ScreenplayConfig;
}

async function readDraft(source: string): Promise<string> {
  if (/^https?:\/\//u.test(source)) {
    const response = await fetch(source);
    if (!response.ok) fail(`${source}: HTTP ${response.status}`);
    return await response.text();
  }
  return await readFile(source, "utf8");
}

export async function importScreenplay(options: {
  readonly draft: string; readonly project: string; readonly config?: string;
  readonly log?: (message: string) => void;
}): Promise<ScreenplaySummary> {
  const project = resolve(options.project);
  const configPath = options.config !== undefined ? resolve(options.config)
    : join(project, "screenplay-import.json");
  const config = await readScreenplayConfig(options.config !== undefined ? configPath
    : await readFile(configPath).then(() => configPath, () => undefined));
  const manifest = JSON.parse(await readFile(join(project, "prismatix.json"), "utf8")) as Json;
  const locale = config.locale ??
    (typeof manifest.defaultLocale === "string" ? manifest.defaultLocale : "zh-TW");
  const draftPath = /^https?:\/\//u.test(options.draft) || isAbsolute(options.draft)
    ? options.draft : resolve(options.draft);
  const scenes = splitScreenplay(await readDraft(draftPath), config);
  if (scenes.length === 0) fail(`${options.draft} has no scene heading`);
  const staging = config.scenes ?? [];
  if (staging.length > 0 && staging.length !== scenes.length)
    fail(`The draft has ${scenes.length} scenes but the staging table lists ${staging.length}`);
  const prefix = config.idPrefix ?? "scene";
  const ids = scenes.map((_, index) => staging[index]?.id ?? `${prefix}${String(index + 1).padStart(2, "0")}`);
  if (new Set(ids).size !== ids.length) fail("Scene ids must be unique");
  const chapter = config.chapter ?? {id: "chapter1", title: "Chapter 1"};
  const unmatched: string[] = [];
  let lineCount = 0;
  const shownDraft = /^https?:\/\//u.test(draftPath) ? draftPath
    : relative(project, draftPath).split(sep).join("/");
  const indexPath = typeof manifest.storyIndex === "string" ? manifest.storyIndex : "Story/story.pxindex";
  const files = new Map<string, string>();
  const regenerated: string[] = [];
  const indexScenes: Json[] = [];
  for (const [index, scene] of scenes.entries()) {
    const id = ids[index]!;
    const sceneStaging = staging[index] ?? {};
    const missed: string[] = [];
    const body = screenplayScene(scene, sceneStaging, config, missed);
    unmatched.push(...missed.map((at) => `${id}: ${at}`));
    lineCount += body.length;
    const path = `Story/${locale}/${id}.pxstory`;
    const text = [`; ${scene.heading.replace(/[\r\n]/gu, " ")} — imported from ${shownDraft} by prismatix import screenplay.`,
      "; Edit the draft and re-import; staging lives in screenplay-import.json.", ...body].join("\n") + "\n";
    const previousMap = await readFile(join(project, `${path}.pxids.json`), "utf8").catch(() => undefined);
    const identified = identifyScreenplayScene(text, path,
      previousMap === undefined ? undefined : {map: previousMap});
    if (identified.regenerated) regenerated.push(path);
    files.set(path, identified.source);
    files.set(`${path}.pxids.json`, identified.map);
    indexScenes.push({id, title: sceneStaging.title ?? scene.heading, sources: {[locale]: path}});
  }
  for (const cue of unmatched) options.log?.(`Staging cue never matched a line: ${cue}`);
  // The imported chapter replaces its earlier import; other scenes and
  // chapters of the index stay as they are.
  const previousIndex = await readFile(join(project, indexPath), "utf8")
    .then((text) => JSON.parse(text.replace(/^\uFEFF/u, "")) as Json, () => undefined);
  const imported = new Set(ids);
  const keptScenes = Array.isArray(previousIndex?.scenes)
    ? (previousIndex.scenes as Json[]).filter((scene) => !imported.has(scene.id as string)) : [];
  const keptChapters = Array.isArray(previousIndex?.chapters)
    ? (previousIndex.chapters as Json[]).filter((entry) => entry.id !== chapter.id)
      .map((entry) => ({...entry, scenes: (entry.scenes as string[]).filter((id) => !imported.has(id))})) : [];
  const storyIndex = {format: "PrismatiXStoryIndex", schemaRevision: 1,
    id: typeof previousIndex?.id === "string" ? previousIndex.id
      : `${typeof manifest.id === "string" ? manifest.id : "imported"}-story`,
    entryScene: typeof previousIndex?.entryScene === "string" && !imported.has(previousIndex.entryScene as string) &&
      keptScenes.some((scene) => scene.id === previousIndex.entryScene) ? previousIndex.entryScene : ids[0]!,
    chapters: [...keptChapters, {id: chapter.id, title: chapter.title, scenes: ids}],
    scenes: [...keptScenes, ...indexScenes]};
  const validated = validateDocument("storyIndex", storyIndex, indexPath);
  if (!validated.valid) throw new ImportFailure("The story index would be invalid", validated.diagnostics);
  files.set(indexPath, canonicalJson(storyIndex));
  let written = 0;
  for (const [path, text] of files)
    if (await writeIfChanged(join(project, ...path.split("/")), text)) written += 1;
  return {scenes: scenes.length, lines: lineCount, unmatchedCues: unmatched,
    storyFiles: files.size, storyFilesWritten: written, regeneratedIdentities: regenerated};
}
