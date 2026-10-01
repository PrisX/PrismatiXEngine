import {createHash} from "node:crypto";
import {mkdir, readFile, writeFile} from "node:fs/promises";
import {dirname, join} from "node:path";

import type {ImportedStoryLine, StoryAssetIndex, StoryAssetRoots} from "@prismatix/authoring-sdk";

import {cropImage, decodePng, encodePng, readImageHeader, thumbnailImage} from "../images.js";
import {storyToken, StoryForms} from "../story-forms.js";
import {compactJson, formatG, formatNumber, roundHalfEven} from "../text-format.js";
import {field, isSourceSpace, isTable, numbered, readArtemisTables,
  type LuaTable, type LuaValue} from "./ast.js";
import type {ArtemisImportConfig} from "./config.js";

/**
 * Translates Artemis scenario tags into PrismatiX Story lines. The tag
 * semantics here are Artemis's own (image_bg, image_view, fg, se.lua,
 * message.lua exfont, ...); everything that differs per game comes from
 * ArtemisImportConfig and the game's system tables. Unsupported tags stay in
 * the Story as `; ARTEMIS` comments and are counted in the report.
 */

/** Artemis tween eases -> engine easing names. */
const artemisEase: Readonly<Record<string, string>> = {none: "linear", in: "inQuad", out: "outQuad",
  inout: "inOutQuad", easeout_cubic: "outCubic", easein_cubic: "inCubic", easeinout_cubic: "inOutCubic"};
const identifierPattern = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;
const speakerTerminators = new Set(Array.from("[「『（(“"));
const openers = ["「", "『", "（", "(", "“"];
const variablePattern = /(?<![\p{L}\p{N}_])([fgs])\.([A-Za-z0-9_]+)(?![\p{L}\p{N}_])/gu;
const assignmentPattern = /^([fgs])\.([A-Za-z0-9_]+)=((?:[fgs]\.[A-Za-z0-9_]+[+-])?-?\d+)$/u;
const incrementPattern = /^([fgs])\.([A-Za-z0-9_]+)([+-])(\d+)$/u;

/** Game-wide values the game's system tables declare (list_windows.tbl `init`). */
export interface ArtemisSystemDefaults {
  readonly bgFadeMs: number;
  readonly fgFadeMs: number;
  readonly vague: number;
  readonly screen: readonly [number, number];
  /** exfont size="fN" -> percent of the message size (init.fontsize). */
  readonly fontSizes: ReadonlyMap<number, number>;
}

/** Project files the converter may name, with case-insensitive lookup. */
export class ProjectFiles {
  private readonly byFolded = new Map<string, string>();
  constructor(paths: Iterable<string>) {
    for (const path of paths) this.byFolded.set(path.toLowerCase(), path);
  }
  /** The file's actual project path, if it exists. */
  find(path: string): string | undefined { return this.byFolded.get(path.toLowerCase()); }
  add(path: string): void { this.byFolded.set(path.toLowerCase(), path); }
}

export interface ConvertedScene {
  readonly id: string;
  readonly chapter: string;
  readonly path: string;
  readonly lines: readonly ImportedStoryLine[];
}

export interface GalleryEntry {
  readonly id: string;
  readonly title: string;
  readonly image: {readonly id: string; readonly path: string; readonly kind: "cg"};
  readonly thumbnail: {readonly id: string; readonly path: string; readonly kind: "cg"};
  readonly unlockFlag: string;
}

type Values = LuaTable;
type Add = (source: string, executable?: boolean, sourceId?: string) => void;
type PendingAct = [alias: string, preset: string, options: Map<string, string | number>];

class Counter extends Map<string, number> {
  bump(key: string, amount = 1): void { this.set(key, (this.get(key) ?? 0) + amount); }
  /** Most common first; ties keep first-counted order. */
  ordered(): Record<string, number> {
    return Object.fromEntries([...this].sort((left, right) => right[1] - left[1]));
  }
  total(): number { let sum = 0; for (const value of this.values()) sum += value; return sum; }
}

const isNumber = (value: LuaValue | undefined): value is number => typeof value === "number";
const isInteger = (value: LuaValue | undefined): value is number =>
  typeof value === "number" && Number.isInteger(value);
const isString = (value: LuaValue | undefined): value is string => typeof value === "string";
function truthy(value: LuaValue | undefined): boolean {
  if (value === null || value === undefined || value === false || value === 0 || value === "") return false;
  if (Array.isArray(value)) return value.length > 0;
  if (isTable(value)) return value.size > 0;
  return true;
}
/** `values.get(key, fallback)`: the fallback only when the key is absent. */
const option = (values: Values, key: string, fallback: LuaValue): LuaValue =>
  values.has(key) ? values.get(key)! : fallback;
/** Python's str() of a scalar Artemis value. */
function show(value: LuaValue | undefined): string {
  if (value === null || value === undefined) return "None";
  if (typeof value === "number") return formatNumber(value);
  if (typeof value === "boolean") return value ? "True" : "False";
  if (typeof value === "string") return value;
  return compactJson(value);
}
function trailingId(value: number): string { return String(value).padStart(3, "0"); }

function safeScene(value: string): string {
  return Array.from(value, (character) => /^[A-Za-z0-9._-]$/u.test(character) ? character
    : `_u${character.codePointAt(0)!.toString(16).toUpperCase().padStart(4, "0")}`).join("");
}

/** pathlib-style join: separators collapse and `.` parts drop. */
export function joinProjectPath(...parts: string[]): string {
  return parts.flatMap((part) => part.split(/[\\/]/u)).filter((part) => part !== "" && part !== ".").join("/");
}

function trimChars(value: string, characters: string, start = true, end = true): string {
  let first = 0;
  let last = value.length;
  while (start && first < last && characters.includes(value[first]!)) first += 1;
  while (end && last > first && characters.includes(value[last - 1]!)) last -= 1;
  return value.slice(first, last);
}

/** Text the Story line syntax keeps verbatim (no trimming, no continuation). */
function stableText(text: string): boolean {
  if (text.length === 0 || text.endsWith("\\") || text.startsWith("﻿") || text.endsWith("﻿")) return false;
  const characters = Array.from(text);
  return !isSourceSpace(characters[0]!) && !isSourceSpace(characters.at(-1)!);
}

function uuid5Url(name: string): string {
  const namespace = Buffer.from("6ba7b8119dad11d180b400c04fd430c8", "hex");
  const hash = createHash("sha1").update(namespace).update(name, "utf8").digest().subarray(0, 16);
  hash[6] = (hash[6]! & 0x0f) | 0x50;
  hash[8] = (hash[8]! & 0x3f) | 0x80;
  const hex = hash.toString("hex");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function strictInteger(text: string): number {
  const trimmed = text.trim();
  if (!/^[+-]?\d+$/u.test(trimmed)) throw new Error(`Invalid integer in image comment: ${JSON.stringify(text)}`);
  return Number(trimmed);
}

function compareScalar(left: LuaValue | undefined, right: LuaValue | undefined): number {
  if (typeof left === "number" && typeof right === "number") return left - right;
  const a = show(left);
  const b = show(right);
  return a < b ? -1 : a > b ? 1 : 0;
}

export interface ArtemisConverterOptions {
  readonly extracted: string;
  readonly project: string;
  readonly config: ArtemisImportConfig;
  readonly system: ArtemisSystemDefaults;
  readonly files: ProjectFiles;
  readonly storyAssets: StoryAssetIndex;
  readonly storyAssetRoots: StoryAssetRoots;
  readonly forms: StoryForms;
  readonly locale: string;
  readonly scripts: readonly string[];
  readonly log?: (message: string) => void;
}

export class ArtemisConverter {
  readonly counts = new Counter();
  readonly unsupported = new Counter();
  readonly missing = new Counter();
  /** alias -> expression -> portrait descriptor path. */
  readonly expressions = new Map<string, Map<string, string>>();
  /** Absolute paths of importer-owned portrait files this run produced. */
  readonly portraitFiles = new Set<string>();
  readonly variables = new Set<string>();
  readonly gallery: GalleryEntry[] = [];
  readonly galleryAssets = new Map<string, string>();
  galleryDeclared = 0;
  galleryMissing = 0;
  voiceCount = 0;
  dialogueCount = 0;
  aliasByName = new Map<string, string>();
  nameByAlias = new Map<string, string>();

  private pendingActs: PendingAct[] = [];
  private readonly fgSlots = new Map<string, string>();
  private readonly fgOffsets = new Map<string, [number, number]>();
  private fgEmpty = false;
  private readonly imageLoops = new Set<string>();
  private readonly imageSizes = new Map<string, readonly [number, number]>();
  private readonly config: ArtemisImportConfig;
  private readonly system: ArtemisSystemDefaults;
  private readonly forms: StoryForms;

  constructor(private readonly options: ArtemisConverterOptions) {
    this.config = options.config;
    this.system = options.system;
    this.forms = options.forms;
  }

  private projectFile(path: string): string { return join(this.options.project, ...path.split("/")); }

  async prepare(): Promise<void> {
    await this.characterAliases();
    await this.loadGallery();
  }

  /** Display name <-> ASCII alias, taken from the fg folder each name is drawn from. */
  private async characterAliases(): Promise<void> {
    const folders = new Map<string, Counter>();
    for (const script of this.options.scripts) {
      const ast = await this.readScript(script);
      if (!isTable(ast)) continue;
      for (const block of ast.values()) {
        if (!isTable(block) && !Array.isArray(block)) continue;
        for (const item of numbered(block)) {
          if (!isTable(item) || item.get(1) !== "fg") continue;
          const name = item.get("ch");
          const path = item.get("path");
          if (!isString(name) || !isString(path)) continue;
          const alias = portraitAlias(path);
          if (alias === undefined) continue;
          let counter = folders.get(name);
          if (counter === undefined) folders.set(name, counter = new Counter());
          counter.bump(alias);
        }
      }
    }
    const aliasByName = new Map<string, string>();
    for (const [name, counter] of folders)
      aliasByName.set(name, [...counter].reduce((best, entry) => entry[1] > best[1] ? entry : best)[0]);
    const nameByAlias = new Map<string, string>();
    const byUse = [...aliasByName].sort((left, right) =>
      folders.get(right[0])!.get(right[1])! - folders.get(left[0])!.get(left[1])!);
    for (const [name, alias] of byUse) if (!nameByAlias.has(alias)) nameByAlias.set(alias, name);
    // Only one display name may own an alias; others stay literal speakers.
    this.aliasByName = new Map([...aliasByName].filter(([name, alias]) => nameByAlias.get(alias) === name));
    this.nameByAlias = nameByAlias;
    for (const [name, alias] of this.config.characterAliases) {
      this.aliasByName.set(name, alias);
      this.nameByAlias.set(alias, name);
    }
  }

  async readScript(script: string): Promise<LuaValue> {
    const document = await readArtemisTables(join(this.options.extracted, "script", script));
    if (!document.has("ast")) throw new Error(`${script} has no ast table`);
    return document.get("ast")!;
  }

  private async loadGallery(): Promise<void> {
    const tablePath = join(this.options.extracted, "system", "table", "list_windows.tbl");
    let groups: LuaValue;
    try {
      const table = await readArtemisTables(tablePath);
      groups = field(table.get("csv") ?? null, "extra_cgmode");
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") {
        this.options.log?.("No system/table/list_windows.tbl: the CG gallery stays empty.");
        return;
      }
      throw error;
    }
    if (!isTable(groups)) {
      this.options.log?.("list_windows.tbl declares no csv.extra_cgmode: the CG gallery stays empty.");
      return;
    }
    const entries = [...groups].sort((left, right) => {
      const a = numbered(left[1]);
      const b = numbered(right[1]);
      return compareScalar(a[0], b[0]) || compareScalar(a[1], b[1]);
    });
    for (const [setName, variants] of entries) {
      if (!isString(setName) || !Array.isArray(variants)) continue;
      for (const name of variants.slice(2)) {
        if (!isString(name)) continue;
        this.galleryDeclared += 1;
        const path = joinProjectPath(this.config.originalRoot, "image", "ev", setName, `${name}.png`);
        const found = this.options.files.find(path);
        if (found === undefined) { this.galleryMissing += 1; continue; }
        const thumbnailPath = joinProjectPath(this.config.galleryThumbnails, `${name}.png`);
        if (this.options.files.find(thumbnailPath) === undefined) {
          const [width, height] = this.config.thumbnailSize;
          const image = thumbnailImage(decodePng(await readFile(this.projectFile(found)), found), width, height);
          await mkdir(dirname(this.projectFile(thumbnailPath)), {recursive: true});
          await writeFile(this.projectFile(thumbnailPath), encodePng(image));
          this.options.files.add(thumbnailPath);
        }
        this.gallery.push({id: name, title: name,
          image: {id: uuid5Url(path), path, kind: "cg"},
          thumbnail: {id: uuid5Url(thumbnailPath), path: thumbnailPath, kind: "cg"},
          unlockFlag: name});
        this.galleryAssets.set(name, path);
      }
    }
  }

  private expression(source: string): string {
    return source.replace(variablePattern, (_match, scope: string, name: string) => {
      const result = `${scope}_${name}`;
      this.variables.add(result);
      return result;
    });
  }

  private assignment(source: string, add: Add): boolean {
    const match = assignmentPattern.exec(source.replaceAll(" ", ""));
    if (match === null) return false;
    const variable = `${match[1]}_${match[2]}`;
    this.variables.add(variable);
    const value = match[3]!;
    const increment = incrementPattern.exec(value);
    if (increment !== null) {
      const referenced = `${increment[1]}_${increment[2]}`;
      this.variables.add(referenced);
      if (referenced !== variable) return false;
      add(this.forms.add(variable, Number(increment[4]) * (increment[3] === "+" ? 1 : -1)));
    } else add(this.forms.set(variable, Number(value)));
    return true;
  }

  /** The project path of an Artemis `:dir/` + name reference, or undefined (counted as missing). */
  private asset(category: string, path: string, file: string, extension = ".png"): string | undefined {
    const directory = trimChars(trimChars(path, ":/", true, false), "/");
    const base = [this.config.originalRoot, category, directory];
    const result = joinProjectPath(...base, `${file}${extension}`);
    // Artemis resolves image names without an extension; zoomed stage images ship as JPEG.
    const extensions = category === "image" && extension === ".png" ? [extension, ".jpg"] : [extension];
    for (const candidate of extensions) {
      const found = this.options.files.find(joinProjectPath(...base, `${file}${candidate}`));
      if (found !== undefined) return found;
    }
    this.missing.bump(result);
    return undefined;
  }

  /** The shortest Story name that still resolves to exactly this file. */
  private short(category: "bg" | "rule" | "bgm" | "se" | "voice" | "video", path: string): string {
    for (const root of this.options.storyAssetRoots[category] ?? []) {
      const prefix = `${root.replace(/\/+$/u, "")}/`;
      if (!path.startsWith(prefix)) continue;
      const relative = path.slice(prefix.length);
      const name = relative.slice(relative.lastIndexOf("/") + 1);
      for (const candidate of [withoutExtension(name), name, withoutExtension(relative), relative]) {
        if (candidate === undefined || candidate.length === 0) continue;
        const lookup = this.options.storyAssets.resolve(category, candidate);
        if (lookup?.kind === "resolved" && lookup.path === path) return candidate;
      }
    }
    return path;
  }

  private async imageSize(asset: string): Promise<readonly [number, number]> {
    let size = this.imageSizes.get(asset);
    if (size === undefined) {
      const header = await readImageHeader(this.projectFile(asset));
      size = [header.width, header.height];
      this.imageSizes.set(asset, size);
    }
    return size;
  }

  /**
   * Artemis draws a layer at pos(bx,by) + move(x,y), zoomed about the anchor
   * (ax,ay): the Story x/y are bx+x and by+y, and x2/y2/z2 start an eased move.
   * A base background Artemis centres exactly as cover fitting would needs no
   * framing at all.
   */
  private async framing(values: Values, asset: string, layer: boolean): Promise<Map<string, string>> {
    const number = (key: string): number | undefined => {
      const value = values.get(key);
      return isNumber(value) ? value : undefined;
    };
    const [width, height] = await this.imageSize(asset);
    const [screenWidth, screenHeight] = this.system.screen;
    const ax = number("ax"), ay = number("ay");
    const bx = number("bx") || 0, by = number("by") || 0;
    const x = number("x"), y = number("y"), zoom = number("zoom");
    const x2 = number("x2"), y2 = number("y2"), z2 = number("z2");
    const moving = x2 !== undefined || y2 !== undefined || z2 !== undefined;
    const centred = bx === (screenWidth - width) / 2 && by === (screenHeight - height) / 2 &&
      (ax === undefined || ax === width / 2) && (ay === undefined || ay === height / 2);
    const framed = new Map<string, string>();
    if (!layer && !moving && x === undefined && y === undefined && zoom === undefined &&
        (centred || (!values.has("ax") && !values.has("bx")))) return framed;
    const text = (value: number) => Number.isInteger(value) ? String(value) : formatG(value);
    framed.set("x", text(bx + (x ?? 0)));
    framed.set("y", text(by + (y ?? 0)));
    if (zoom !== undefined && zoom !== 100) framed.set("zoom", text(zoom / 100));
    if (ax !== undefined && ay !== undefined) framed.set("anchor", `${text(ax)},${text(ay)}`);
    if (moving) {
      if (x2 !== undefined) framed.set("from-x", text(bx + x2));
      if (y2 !== undefined) framed.set("from-y", text(by + y2));
      if (z2 !== undefined) framed.set("from-zoom", text(z2 / 100));
      framed.set("ease", artemisEase[show(option(values, "ease", "out"))] ?? "outQuad");
    }
    return framed;
  }

  private rule(value: LuaValue | undefined): string | undefined {
    let asset: string | undefined;
    if (isNumber(value) && Number.isInteger(value))
      asset = this.asset("image", "rule", value < 0 ? `-${String(-value).padStart(2, "0")}` : trailingId(value));
    else if (isString(value)) asset = this.asset("image", "rule", value);
    else return undefined;
    if (asset === undefined) { this.unsupported.bump("rule.unresolved"); return undefined; }
    return this.short("rule", asset);
  }

  /**
   * Registers a layered portrait expression and returns [alias, expression].
   * Bodies are drawn from the original images; only overlays whose sprite
   * sheet needs cropping get a small cropped copy.
   */
  private async portrait(values: Values): Promise<[string, string] | undefined> {
    const path = values.get("path");
    const bodyName = values.get("file");
    if (!isString(path) || !isString(bodyName)) return undefined;
    const alias = portraitAlias(path);
    const base = this.asset("image", path, bodyName);
    if (base === undefined || alias === undefined) return undefined;
    const overlays = ["face", "hoho", "ex01", "ex02", "ex03", "ex04"]
      .map((key) => values.get(key)).filter(isString);
    const stem = bodyName.startsWith(`${alias}_`) ? bodyName.slice(alias.length + 1) : bodyName;
    const expression = [stem, ...overlays].join("_").replace(/[^A-Za-z0-9._-]/gu, "_");
    let known = this.expressions.get(alias);
    if (known === undefined) this.expressions.set(alias, known = new Map());
    if (known.has(expression)) return [alias, expression];
    const directory = joinProjectPath(this.config.portraitRoot, trimChars(path, ":/"));
    const header = await readImageHeader(this.projectFile(base));
    const [bodyX, bodyY] = imagePosition(header.comment);
    // Artemis `pos` comments are top-left screen coordinates. A screen-sized
    // canvas keeps them exact: the Stage bottom-aligns and centres the canvas.
    const layers: Record<string, string | number>[] = [
      {path: base, x: bodyX, y: bodyY, width: header.width, height: header.height, role: "base"}];
    for (const name of overlays) {
      let overlayPath = this.asset("image", path, name);
      if (overlayPath === undefined) continue;
      const overlay = await readImageHeader(this.projectFile(overlayPath));
      const [x, y, cropWidth, cropHeight] = imagePosition(overlay.comment);
      let layerWidth = overlay.width, layerHeight = overlay.height;
      if (cropWidth !== undefined && cropHeight !== undefined &&
          (cropWidth !== layerWidth || cropHeight !== layerHeight)) {
        const cropped = joinProjectPath(directory, "layers", `${name}.png`);
        const target = this.projectFile(cropped);
        if (this.options.files.find(cropped) === undefined) {
          const image = decodePng(await readFile(this.projectFile(overlayPath)), overlayPath);
          await mkdir(dirname(target), {recursive: true});
          await writeFile(target, encodePng(cropImage(image, cropWidth, cropHeight)));
          this.options.files.add(cropped);
        }
        this.portraitFiles.add(target);
        overlayPath = cropped;
        layerWidth = cropWidth; layerHeight = cropHeight;
      }
      layers.push({path: overlayPath, x, y, width: layerWidth, height: layerHeight, role: "expression"});
    }
    const descriptor = joinProjectPath(directory, `${expression}.pxportrait`);
    const target = this.projectFile(descriptor);
    const text = `${compactJson({format: "PrismatiXLayeredPortrait", schemaRevision: 2,
      width: this.system.screen[0], height: this.system.screen[1], layers})}\n`;
    let existing: string | undefined;
    try { existing = (await readFile(target, "utf8")).replace(/\r\n/gu, "\n"); } catch { existing = undefined; }
    if (existing !== text) {
      await mkdir(dirname(target), {recursive: true});
      await writeFile(target, text, "utf8");
    }
    this.portraitFiles.add(target);
    known.set(expression, descriptor);
    return [alias, expression];
  }

  private static volume(values: Values): string {
    const vol = values.get("vol");
    return isNumber(vol) && vol >= 0 && vol < 100 ? ` volume=${formatG(vol)}` : "";
  }

  /** The name authors read: the display name when it is a bare word. */
  private who(alias: string): string {
    const name = this.nameByAlias.get(alias) ?? alias;
    const bare = !identifierPattern.test(name) && !Array.from(name).some((character) =>
      isSourceSpace(character) || speakerTerminators.has(character) || "\"=]".includes(character));
    return bare && this.aliasByName.get(name) === alias ? name : alias;
  }

  /** image_act parameters shared by fgact, mwact and cgact. */
  private static actOptions(values: Values, preset: string): Map<string, string | number> {
    const options = new Map<string, string | number>();
    if (preset === "stop") return options;
    // ウゴウゴ stretches by s1/s2 and ignores size.
    const size = values.get("size");
    if (isNumber(size) && values.get("act") !== "ウゴウゴ") options.set("size", size);
    const time = values.get("time");
    if (isNumber(time)) options.set("time", `${Math.trunc(time)}ms`);
    const loop = values.get("loop");
    if (isInteger(loop) && (loop === -1 || (loop >= 1 && loop <= 1000))) options.set("loop", loop);
    return options;
  }

  private static optionText(options: Map<string, string | number>): string {
    return [...options].map(([key, value]) => ` ${key}=${typeof value === "number" ? formatNumber(value) : value}`).join("");
  }

  /** A re-shown bg/cg layer drops its act; the engine keeps one on reframing. */
  private endImageLoop(target: string, add: Add, sourceId: string): void {
    if (!this.imageLoops.delete(target)) return;
    add(`[act ${target} stop]`, true, sourceId);
  }

  private flushActs(add: Add): void {
    for (const [alias, preset, options] of this.pendingActs)
      add(`[act ${this.who(alias)} ${preset}${ArtemisConverter.optionText(options)}]`);
    this.pendingActs = [];
  }

  private static annotation(name: string, values: Values): string {
    const fields = [...values].filter(([key]) => typeof key === "string")
      .map(([key, value]) => `${key}=${compactJson(value)}`).join(" ");
    return `; ARTEMIS ${name}${fields ? ` ${fields}` : ""}`;
  }

  private chapter(scene: string): string {
    for (const rule of this.config.chapters) {
      const match = rule.match.exec(scene);
      if (match !== null) return rule.id.replace(/\$(\d)/gu, (_text, group: string) => match[Number(group)] ?? "");
    }
    return this.config.defaultChapter;
  }

  async convert(script: string): Promise<ConvertedScene> {
    const scene = safeScene(script.replace(/\.ast$/iu, ""));
    const ast = await this.readScript(script);
    if (!isTable(ast)) throw new Error(`Invalid AST root: ${script}`);
    const config = this.config;
    const forms = this.forms;
    const language = config.language;
    const labelsByBlock = new Map<string, string[]>();
    const labels = ast.get("label");
    if (isTable(labels))
      for (const [label, entry] of labels) {
        const block = field(entry, "block");
        if (isTable(entry) && isString(block))
          labelsByBlock.set(block, [...labelsByBlock.get(block) ?? [], show(label)]);
      }
    const choiceEffects = new Map<string, string[]>();
    for (const block of ast.values()) {
      if (!isTable(block) && !Array.isArray(block)) continue;
      for (const item of numbered(block)) {
        if (!isTable(item) || item.get(1) !== "select" || !isString(item.get("exp"))) continue;
        const label = item.get("label");
        const target = isTable(labels) && (isString(label) || isNumber(label)) ? labels.get(label) : undefined;
        const block = field(target, "block");
        if (isTable(target) && isString(block))
          choiceEffects.set(block, [...choiceEffects.get(block) ?? [], item.get("exp") as string]);
        else this.unsupported.bump("select.unresolvedEffectTarget");
      }
    }
    const lines: ImportedStoryLine[] = [];
    this.fgSlots.clear();
    this.fgOffsets.clear();
    this.fgEmpty = false;
    this.imageLoops.clear();
    let currentBlock = "head";
    const serial = new Counter();
    // IDs are scoped by Artemis block so re-imports only move identities
    // inside the block that changed. Continuation lines carry no ID.
    const add: Add = (source, executable = true, sourceId) => {
      const parts = source.split("\n");
      if (executable) {
        if (sourceId === undefined) {
          serial.bump(currentBlock);
          sourceId = `artemis.${scene}.${currentBlock}.${trailingId(serial.get(currentBlock)!)}`;
        }
        lines.push({source: parts[0]!, id: sourceId});
      } else lines.push({source: parts[0]!});
      for (const part of parts.slice(1)) lines.push({source: part});
    };
    const lastSource = () => lines.at(-1)?.source;

    add(`; Imported from ${script}; unsupported Artemis commands stay in comments.`, false);
    for (const [blockKey, block] of ast) {
      const blockName = show(blockKey);
      if (!blockName.startsWith("block_") || (!isTable(block) && !Array.isArray(block))) continue;
      currentBlock = blockName.slice("block_".length);
      add(`; Artemis ${blockName}`, false);
      for (const label of labelsByBlock.get(blockName) ?? []) {
        if (label === blockName) continue;
        add(`*${label}`);
        this.fgSlots.clear();
        this.fgEmpty = false;
      }
      for (const effect of choiceEffects.get(blockName) ?? []) {
        if (!this.assignment(effect, add)) {
          this.unsupported.bump("select.effectExpression");
          add(`; ARTEMIS selected effect=${compactJson(effect)}`, false);
        }
      }
      const items = numbered(block);
      for (let itemIndex = 0; itemIndex < items.length; itemIndex += 1) {
        const item = items[itemIndex]!;
        if (!isTable(item) && !Array.isArray(item)) continue;
        const name = isTable(item) ? item.get(1) : item[0];
        if (!isString(name)) continue;
        const values: Values = isTable(item) ? item : new Map([[1, name]]);
        const index5 = String(itemIndex).padStart(5, "0");
        const pinned = (kind: string, suffix = "") => `artemis.${scene}.${kind}.${blockName}.${index5}${suffix}`;
        this.counts.bump(name);
        if (name === "text") { await this.convertText(block, add); continue; }
        if (name === "fgact" && (values.get("act") === "icon" || values.get("act") === "stopicon")) {
          // image_fgicon draws :icon/<name>.ipt only when it exists; without
          // an icon folder the original shows nothing.
          this.counts.bump("fgact.iconWithoutAsset");
          add(ArtemisConverter.annotation(name, values), false);
          continue;
        }
        if (name === "fgact") {
          const preset = config.actPresets.get(show(values.get("act") ?? null));
          const alias = this.aliasByName.get(show(values.get("ch") ?? null));
          if (preset === undefined || alias === undefined) {
            this.unsupported.bump("fgact.unmapped");
            add(ArtemisConverter.annotation(name, values), false);
            continue;
          }
          this.pendingActs.push([alias, preset, ArtemisConverter.actOptions(values, preset)]);
          continue;
        }
        this.flushActs(add);
        if (name === "cgact") {
          // image_bg.cgact runs image_act on bg layer `id`: 0 is the base
          // bg/ev ([bg]); others are the [image cgN] layers.
          const preset = config.actPresets.get(show(values.get("act") ?? null));
          const layerId = option(values, "id", 0);
          if (preset === undefined || !isInteger(layerId) || layerId < 0) {
            this.unsupported.bump("cgact.unmapped");
            add(ArtemisConverter.annotation(name, values), false);
            continue;
          }
          const target = layerId === 0 ? "bg" : `image=cg${layerId}`;
          const options = ArtemisConverter.actOptions(values, preset);
          if (preset === "stop") this.imageLoops.delete(target);
          else if (options.get("loop") === -1) this.imageLoops.add(target);
          // Pinned ids keep the block's serial ids unchanged by this tag.
          add(`[act ${target} ${preset}${ArtemisConverter.optionText(options)}]`, true, pinned("act"));
          continue;
        }
        if (name === "bg") {
          const file = values.get("file");
          const image = isString(file) ? this.asset("image", show(option(values, "path", "")), file) : undefined;
          if (image === undefined) {
            this.unsupported.bump("bg.unresolved");
            add(ArtemisConverter.annotation(name, values), false);
            continue;
          }
          // sync=0 stages the change for the next [transition]; otherwise
          // the layer fades in over time (default bg_fade).
          const staged = values.get("sync") === 0;
          const time = option(values, "speed", values.get("time") ?? null);
          const duration = isNumber(time) ? Math.trunc(time) : undefined;
          const layerId = values.get("id");
          const layer = isInteger(layerId) && layerId !== 0;
          if (!layer && !staged && values.get("hide") !== -1 && values.get("notrans") !== 1 &&
              (duration ?? this.system.bgFadeMs) >= 2)
            // image_bg: an immediate background hides the message window
            // for the change; the next line brings it back.
            add(forms.hud(false), true, pinned("hud", ".bg"));
          if (!layer && values.get("face") !== 1) {
            // image_view: a base bg/ev deletes every portrait unless it is a
            // face=1 change. The removal shares the background's transition.
            const wipe = `[hide all${staged ? " transition=false" : ""}]`;
            if (!this.fgEmpty && lastSource() !== wipe) add(wipe);
            this.fgSlots.clear();
            this.fgEmpty = true;
          }
          const framed = await this.framing(values, image, layer);
          const moving = framed.has("from-x") || framed.has("from-y") || framed.has("from-zoom");
          const options = new Map<string, string | number>();
          if (layer) {
            // Artemis orders layers by lv; characters default to lv 5.
            const lv = option(values, "lv", 0);
            const level = isNumber(lv) ? Math.trunc(lv) : 0;
            options.set("z", level > 5 ? level + 10 : level - 10);
            const alpha = values.get("alpha");
            if (isNumber(alpha)) options.set("alpha", formatG(Math.max(0, Math.min(255, alpha)) / 255, 3));
            for (const [key, value] of framed) options.set(key, value);
            if ((!staged || moving) && (duration ?? 0) > 0) options.set("duration", `${duration}ms`);
            if (staged && !moving) options.delete("duration");
            add(`[image cg${layerId} ${storyToken(this.short("bg", image))}${tokenOptions(options)}]`);
            this.endImageLoop(`image=cg${layerId}`, add, pinned("act", ".stop"));
            if (values.get("rule") !== null && values.get("rule") !== undefined) this.counts.bump("bg.layerRuleAsFade");
          } else {
            const rule = staged ? undefined : this.rule(values.get("rule"));
            if (staged) options.set("transition", "false");
            else if (rule !== undefined) {
              options.set("rule", rule);
              options.set("vague", show(option(values, "vague", this.system.vague)));
            }
            if (!staged || moving) options.set("duration", `${duration ?? this.system.bgFadeMs}ms`);
            for (const [key, value] of framed) options.set(key, value);
            add(`[bg ${storyToken(this.short("bg", image))}${tokenOptions(options)}]`);
            this.endImageLoop("bg", add, pinned("act", ".stop"));
            if (!staged) {
              // trans() holds the script until the fade ends, then `wait`
              // more milliseconds; a click cuts both short.
              const extra = isNumber(values.get("wait")) ? values.get("wait") as number : 0;
              const hold = Math.trunc((duration ?? this.system.bgFadeMs) + extra);
              if (hold > 0) add(forms.wait(`${hold}ms`));
            }
          }
          if (isString(file) && this.galleryAssets.get(file) === image) add(forms.unlock("cg", file));
          continue;
        }
        if (name === "cgdel") {
          const layerId = values.get("id");
          const time = values.get("time");
          const fade = values.get("sync") === 0 || !isNumber(time) || time <= 0 ? "" : ` ${Math.trunc(time)}ms`;
          if (layerId === -1) {
            add(`[image.hide all${fade}]`);
            for (const loop of [...this.imageLoops]) if (loop !== "bg") this.imageLoops.delete(loop);
          } else if (isInteger(layerId)) {
            add(`[image.hide cg${layerId}${fade}]`);
            this.imageLoops.delete(`image=cg${layerId}`);
          } else {
            this.unsupported.bump("cgdel.id");
            add(ArtemisConverter.annotation(name, values), false);
          }
          continue;
        }
        if (name === "extrans") {
          const time = option(values, "time", this.system.bgFadeMs);
          if (!isNumber(time) || time <= 0) continue;
          const options = new Map<string, string | number>();
          const rule = this.rule(values.get("rule"));
          if (rule !== undefined) {
            options.set("rule", rule);
            options.set("vague", show(option(values, "vague", this.system.vague)));
          }
          add(`[transition ${Math.trunc(time)}ms${tokenOptions(options)}]`);
          continue;
        }
        if (name === "mwact") {
          const preset = config.actPresets.get(show(values.get("act") ?? null));
          if (preset === undefined) {
            this.unsupported.bump("mwact.unmapped");
            add(ArtemisConverter.annotation(name, values), false);
            continue;
          }
          const options = new Map<string, string | number>();
          const size = values.get("size");
          if (isNumber(size)) options.set("size", size);
          const time = values.get("time");
          if (isNumber(time)) options.set("time", `${Math.trunc(time)}ms`);
          add(`[act window ${preset}${ArtemisConverter.optionText(options)}]`);
          continue;
        }
        if (name === "bgm") {
          const time = values.get("time");
          const fade = isNumber(time) ? `${formatNumber(time)}ms` : undefined;
          if (truthy(values.get("stop"))) add(forms.stopMusic("bgm.stop", fade));
          else if (isString(values.get("file"))) {
            const asset = this.asset("sound", "bgm", values.get("file") as string, ".ogg");
            if (asset !== undefined)
              add(`[bgm ${storyToken(this.short("bgm", asset))}${fade === undefined ? "" : ` fade=${fade}`}${ArtemisConverter.volume(values)}]`);
            else {
              this.unsupported.bump("bgm.unresolved");
              add(ArtemisConverter.annotation(name, values), false);
            }
          }
          continue;
        }
        if (name === "autoplay") {
          // tags.autoplay: the scene reads itself (speed ms per character,
          // auto ms between lines) with input off, unless its autoflag says
          // it was seen before.
          if (values.get("mode") === "stop") { add("[auto off]"); continue; }
          const speed = values.get("speed"), wait = values.get("auto");
          const options = (isNumber(speed) ? ` speed=${Math.max(0, Math.min(1000, Math.trunc(speed)))}` : "") +
            (isNumber(wait) ? ` wait=${Math.max(0, Math.min(60000, Math.trunc(wait)))}ms` : "");
          const flag = values.get("autoflag");
          if (isString(flag) && new RegExp(`^${variablePattern.source}$`, "u").test(flag)) {
            add(`[if ${this.expression(flag)} == 1]`);
            add(`[auto on${options}]`);
            add("[else]");
            add(`[auto on${options} lock=true]`);
            add("[endif]");
          } else add(`[auto on${options} lock=true]`);
          continue;
        }
        if (name === "msgoff" || name === "msgon") {
          // time= is the window's fade (mw_time/mw_hidetime otherwise).
          const time = values.get("time");
          add(forms.hud(name === "msgon", isNumber(time) && time >= 0 ? `${Math.trunc(time)}ms` : undefined),
            true, pinned("hud"));
          for (const key of ["mode", "hide"]) if (values.has(key)) this.unsupported.bump(`${name}.${key}`);
          continue;
        }
        if (name === "se") {
          // media/se.lua: every SE plays on a channel (`id`, default 1) and
          // replaces that channel's cue; loop=1 repeats until stopped.
          // stop=1 with id=-1 or no id stops every channel.
          const channel = values.get("id");
          const time = values.get("time");
          const fade = isNumber(time) && time > 0 ? `${Math.trunc(time)}ms` : undefined;
          const valid = isInteger(channel) && channel >= 1 && channel <= 64;
          if (truthy(values.get("stop"))) add(forms.stopSound(valid ? channel as number : undefined, fade));
          else if (isString(values.get("file"))) {
            const asset = this.asset("sound", "se", values.get("file") as string, ".ogg");
            if (asset !== undefined) {
              const looped = values.get("loop") === 1 ? " loop=true" : "";
              add(`[se ${storyToken(this.short("se", asset))} channel=${valid ? channel : 1}${looped}${
                fade === undefined ? "" : ` fade=${fade}`}${ArtemisConverter.volume(values)}]`);
              if (values.get("sync") === 1) this.unsupported.bump("se.syncWait");
            } else {
              this.unsupported.bump("se.unresolved");
              add(ArtemisConverter.annotation(name, values), false);
            }
          }
          continue;
        }
        if (name === "vostop") { add("[voice.stop]"); continue; }
        if (name === "fg") { await this.convertFg(values, add); continue; }
        if (name === "ex" && values.get("func") === "wait" && isNumber(values.get("time"))) {
          add(forms.wait(`${formatNumber(values.get("time") as number)}ms`));
          continue;
        }
        if (name === "excall") {
          const targetFile = values.get("file");
          const targetLabel = values.get("label");
          if (isString(targetFile) || isString(targetLabel)) {
            const condition = values.get("cond");
            if (isString(condition)) add(`[if ${this.expression(condition)}]`);
            add(forms.transfer(values.get("call") === 1 ? "call" : "jump",
              isString(targetFile) ? safeScene(targetFile) : undefined,
              isString(targetLabel) ? targetLabel : undefined));
            if (isString(condition)) add("[endif]");
          } else {
            this.unsupported.bump("excall.unresolvedTarget");
            add(ArtemisConverter.annotation(name, values), false);
          }
          continue;
        }
        if (name === "select") {
          const label = values.get("label");
          if (isString(label)) {
            const file = values.get("file");
            add(forms.choice(this.choiceText(block, values, language), label,
              isString(file) ? safeScene(file) : undefined));
            if (values.has("cond")) this.unsupported.bump("select.condition");
          } else if (truthy(values.get("mode"))) add("[choice.wait]", false);
          continue;
        }
        if (name === "eval" && isString(values.get("exp"))) {
          const condition = values.get("cond");
          if (isString(condition)) add(`[if ${this.expression(condition)}]`);
          if (!this.assignment(values.get("exp") as string, add)) {
            this.unsupported.bump("eval.expression");
            add(ArtemisConverter.annotation(name, values), false);
          }
          if (isString(condition)) add("[endif]");
          continue;
        }
        if (name === "movie" && isString(values.get("file"))) {
          const file = values.get("file") as string;
          const asset = this.asset("movie", "", config.movies.get(file) ?? file, config.movieExtension);
          if (asset !== undefined) add(`[video ${storyToken(this.short("video", asset))}]`);
          else {
            this.unsupported.bump("movie.unresolved");
            add(ArtemisConverter.annotation(name, values), false);
          }
          continue;
        }
        if (name === "evset") {
          const cg = values.get("file");
          if (isString(cg) && this.galleryAssets.has(cg)) add(forms.unlock("cg", cg));
          else {
            this.unsupported.bump("evset.unresolved");
            add(ArtemisConverter.annotation(name, values), false);
          }
          continue;
        }
        if (name === "scene" && isString(values.get("file"))) {
          const sceneId = values.get("file") as string;
          if (config.replayScenes?.test(sceneId)) add(forms.unlock("scene", sceneId));
          else {
            this.unsupported.bump("scene.unresolved");
            add(ArtemisConverter.annotation(name, values), false);
          }
          continue;
        }
        if (name === "quake") {
          // image_act.lua quake(): `time` ms per swing, `loop` swings each
          // 0.85x the last. dir="v" moves x and "h" moves y; "r" (the
          // default) moves both. The r0..r4 yoyo variants approximate "r".
          const mode = option(values, "mode", "gr");
          const perSwing = option(values, "time", 60);
          const loops = option(values, "loop", 10);
          const size = option(values, "size", 6);
          const direction = show(option(values, "dir", "r"));
          const axis = direction === "v" ? "h" : direction === "h" ? "v" : "r";
          if ((mode === "gr" || mode === "mw") && !truthy(values.get("stop")) &&
              isNumber(perSwing) && perSwing > 0 && isInteger(loops) && loops >= 1 && loops <= 100 &&
              isNumber(size) && size >= 0) {
            const duration = roundHalfEven(perSwing * (loops + 0.5));
            add(`[shake ${duration}ms ${formatNumber(size)} dir=${axis}${loops !== 10 ? ` count=${loops}` : ""}${
              mode === "mw" ? " target=window" : ""}]`);
            this.counts.bump(mode === "mw" ? "quakeWindow" : "quakeGlobal");
            if (!["r", "v", "h"].includes(direction) || values.has("w") || values.has("h"))
              this.counts.bump("quake.approximateVariant");
          } else {
            this.unsupported.bump("quake.params");
            add(ArtemisConverter.annotation(name, values), false);
          }
          continue;
        }
        if (name === "colortone") {
          // scr.tone: a screen-wide grade until the next colortone.
          const style = option(values, "style", "reset");
          const tone = ({gray: "gray", sepia: "sepia", sepia2: "sepia", reset: "none"} as Record<string, string>)[
            isString(style) ? style : ""];
          if (tone !== undefined) { add(forms.tone(tone)); continue; }
        }
        if (name === "flash") {
          let color = show(option(values, "color", "ffffff")).toLowerCase();
          if (color.startsWith("0x")) color = color.slice(2);
          if (color.startsWith("x")) color = color.slice(1);
          const alpha = option(values, "alpha", 255);
          const time = option(values, "time", 100);
          const loop = option(values, "loop", option(values, "count", 1));
          if (/^[0-9a-f]{6}$/u.test(color) && isNumber(alpha) && isNumber(time) &&
              isInteger(loop) && loop >= 1 && loop <= 99) {
            add(forms.flash(color, (Math.max(0, Math.min(255, alpha)) / 255).toFixed(2),
              `${Math.max(2, Math.trunc(time))}ms`, loop));
            continue;
          }
        }
        if (name === "savetitle" && isString(values.get("text")) && values.get("text") !== "") {
          // sv.savetitle: the label a save slot (and the window) shows.
          add(forms.chapter(values.get("text") as string));
          continue;
        }
        if (config.ignoredCommands.has(name)) {
          // Kept as a comment for traceability; nothing to reproduce.
          add(ArtemisConverter.annotation(name, values), false);
          continue;
        }
        this.unsupported.bump(name);
        add(ArtemisConverter.annotation(name, values), false);
      }
    }
    this.flushActs(add);
    const last = lastSource();
    if (last === undefined || !["[jump", "[call", "[end", "[return"].some((prefix) => last.startsWith(prefix)))
      add("[end]");
    return {id: scene, chapter: this.chapter(scene), path: `Story/${this.options.locale}/${scene}.pxstory`, lines};
  }

  private async convertFg(values: Values, add: Add): Promise<void> {
    const name = "fg";
    const mode = option(values, "mode", 3);
    const actor = values.get("ch");
    const staged = values.get("sync") === 0 ? " transition=false" : "";
    const mx = isNumber(values.get("mx")) ? values.get("mx") as number : 0;
    const my = isNumber(values.get("my")) ? values.get("my") as number : 0;
    if (mode === -2) {
      add(`[hide all${staged}]`);
      this.fgSlots.clear();
      this.fgEmpty = true;
    } else if (mode === -1 && isString(actor) && this.aliasByName.has(actor)) {
      const alias = this.aliasByName.get(actor)!;
      const who = this.who(alias);
      if ((mx || my) && !staged) {
        // image_disp: the layer slides out by (mx,my) while it fades.
        const time = option(values, "time", this.system.fgFadeMs);
        const [ox, oy] = this.fgOffsets.get(alias) ?? [0, 0];
        const moved: string[] = [];
        if (mx) moved.push(` x=${Math.trunc(ox + mx)}`);
        if (my) moved.push(` y=${Math.trunc(oy + my)}`);
        add(`[move ${who}${moved.join("")} duration=${Math.trunc(time as number)}ms wait=false]`);
      }
      add(`[hide ${who}${staged}]`);
      for (const [slot, holder] of [...this.fgSlots]) if (holder === alias) this.fgSlots.delete(slot);
      this.fgOffsets.delete(alias);
    } else if (isNumber(mode) && mode >= 2 && isString(actor)) {
      const shown = await this.portrait(values);
      if (shown === undefined) {
        this.unsupported.bump("fg.unresolved");
        add(ArtemisConverter.annotation(name, values), false);
        return;
      }
      const [alias, expression] = shown;
      const who = this.who(alias);
      const lv = option(values, "lv", 5);
      const id = values.get("id");
      const layerKey = `${isNumber(lv) ? Math.trunc(lv) : 5},${isNumber(id) ? Math.trunc(id) : 0}`;
      // fg(): a different character already on this layer is deleted.
      const holder = this.fgSlots.get(layerKey);
      if (holder !== undefined && holder !== alias) add(`[hide ${this.who(holder)} transition=false]`);
      for (const [slot, other] of [...this.fgSlots]) if (other === alias) this.fgSlots.delete(slot);
      this.fgSlots.set(layerKey, alias);
      this.fgEmpty = false;
      const target = new Map<string, string | number>();
      for (const axis of ["x", "y"]) if (isNumber(values.get(axis))) target.set(axis, values.get(axis) as number);
      const zoom = values.get("zoom");
      if (isNumber(zoom) && zoom !== 100) target.set("scale", formatG(zoom / 100));
      // image.lua: rotate (or r/r1) turns the layer; r2 animates from it.
      const turnKey = ["rotate", "r1", "r"].find((key) => isNumber(values.get(key)));
      const turn = turnKey === undefined ? undefined : values.get(turnKey) as number;
      if (turn) target.set("rotate", formatG(turn));
      this.fgOffsets.set(alias, [Number(target.get("x") ?? 0), Number(target.get("y") ?? 0)]);
      const start = new Map(target);
      for (const [axis, source] of [["x", "x2"], ["y", "y2"]] as const)
        if (isNumber(values.get(source))) start.set(axis, values.get(source) as number);
      // image_disp: mx/my start the layer offset and slide it home.
      if (mx) start.set("x", Math.trunc(Number(start.get("x") ?? 0) + mx));
      if (my) start.set("y", Math.trunc(Number(start.get("y") ?? 0) + my));
      if (isNumber(values.get("z2"))) start.set("scale", formatG((values.get("z2") as number) / 100));
      const r2 = values.get("r2");
      if (isNumber(r2) && r2 !== (turn || 0)) start.set("rotate", formatG(r2));
      add(`[show ${who} ${expression} center${ArtemisConverter.optionText(start)}${staged}]`);
      const sameKeys = start.size === target.size && [...start].every(([key, value]) => target.get(key) === value);
      if (!sameKeys) {
        // Artemis x2/y2/z2 and mx/my start an eased entrance move.
        const time = option(values, "speed", option(values, "time", this.system.fgFadeMs));
        const moved = new Map<string, string | number>();
        for (const [key, value] of start)
          if (value !== target.get(key)) moved.set(key, target.get(key) ?? (key !== "scale" ? 0 : 1));
        add(`[move ${who}${ArtemisConverter.optionText(moved)} duration=${Math.trunc(time as number)}ms wait=false]`);
      }
    } else {
      this.unsupported.bump("fg.mode");
      add(ArtemisConverter.annotation(name, values), false);
    }
  }

  private choiceText(block: LuaValue, values: Values, language: string): string {
    const text = isTable(block) ? block.get("select") : undefined;
    if (isTable(text)) {
      const choices = text.get(language);
      if (Array.isArray(choices)) {
        const selects = numbered(block).filter((item): item is LuaTable =>
          isTable(item) && item.get(1) === "select" && truthy(item.get("label")));
        const fallback = selects.filter((item) => item !== values).length;
        // The displayed choices follow the option declarations.
        const found = selects.map((item) => item.get("label")).indexOf(values.get("label")!);
        const position = found < 0 ? fallback : found;
        const choice = choices[position];
        if (position < choices.length && isString(choice)) return choice;
      }
    }
    return show(option(values, "label", "選択"));
  }

  private async convertText(block: LuaValue, add: Add): Promise<void> {
    const text = isTable(block) ? block.get("text") : undefined;
    if (!isTable(text)) return;
    const voices: string[] = [];
    for (const item of numbered(text.get("vo"))) {
      if (!isTable(item) || item.get(1) !== "vo") continue;
      const file = item.get("file");
      const actor = item.get("ch");
      if (!isString(file) || !isString(actor)) continue;
      const asset = this.asset("sound", `vo/${actor}`, file, ".ogg");
      if (asset !== undefined) { voices.push(this.short("voice", asset)); this.voiceCount += 1; }
      else this.unsupported.bump("vo.unresolved");
    }
    let firstLine = true;
    for (const entry of numbered(text.get(this.config.language))) {
      if (isString(entry)) {
        if (entry) {
          this.emitText(add, undefined, entry, firstLine ? voices : []);
          firstLine = false;
          this.dialogueCount += 1;
        }
        continue;
      }
      if (!isTable(entry) && !Array.isArray(entry)) continue;
      let speakerValue = isTable(entry) ? entry.get("name") : undefined;
      if (Array.isArray(speakerValue)) speakerValue = speakerValue.map(show).join("");
      const speaker = isString(speakerValue) ? speakerValue : undefined;
      const segments: string[] = [];
      let rubyReading: string | undefined;
      let rubyBase: string[] = [];
      // Closing tags of each open exfont, innermost last.
      const openFonts: string[] = [];
      for (const piece of numbered(entry)) {
        if (isString(piece)) {
          (rubyReading === undefined ? segments : rubyBase).push(piece);
          continue;
        }
        if (!isTable(piece) && !Array.isArray(piece)) continue;
        const marker = isTable(piece) ? piece.get(1) : piece[0];
        if (marker === "exfont") {
          // A bare {"exfont"} arrives as a list.
          const [opening, closing] = this.exfontTags(isTable(piece) ? piece : new Map());
          const target = rubyReading === undefined ? segments : rubyBase;
          // A bare exfont is e:tag{"/font"}: it ends the latest one.
          if (opening === undefined) { if (openFonts.length > 0) target.push(openFonts.pop()!); }
          else { target.push(opening); openFonts.push(closing); }
          this.counts.bump("text.exfont");
        } else if (marker === "rt2") {
          if (segments.length > 0 && segments.at(-1) !== "\n") segments.push("\n");
        } else if (marker === "txruby") {
          const reading = isTable(piece) ? piece.get("text") : undefined;
          if (isString(reading)) {
            if (rubyReading !== undefined) {
              segments.push(...rubyBase);
              this.unsupported.bump("text.unclosedRuby");
            }
            rubyReading = reading;
            rubyBase = [];
          } else if (rubyReading !== undefined) {
            segments.push(`[ruby=${rubyReading}]${rubyBase.join("")}[/ruby]`);
            rubyReading = undefined;
            rubyBase = [];
          }
        } else this.unsupported.bump(`text.${show(marker ?? null)}`);
      }
      if (rubyReading !== undefined) {
        segments.push(...rubyBase);
        this.unsupported.bump("text.unclosedRuby");
      }
      // A style left open ends with the line in both engines.
      segments.push(...[...openFonts].reverse());
      const content = segments.join("").replace(/\n+$/u, "");
      if (content) {
        this.emitText(add, speaker, content, firstLine ? voices : []);
        firstLine = false;
        this.dialogueCount += 1;
      }
    }
    if (firstLine) for (const voice of voices) add(`[voice ${storyToken(voice)}]`);
    this.flushActs(add);
  }

  /** PrismatiX tags for one message.lua exfont call; [undefined, ""] closes. */
  private exfontTags(values: Values): [string | undefined, string] {
    const size = values.get("size") ?? null, color = values.get("color") ?? null, style = values.get("style") ?? null;
    if (size === null && color === null && style === null) return [undefined, ""];
    const opening: string[] = [];
    const closing: string[] = [];
    const level = isString(size) && /^f[0-9]+$/u.test(size) ? Number(size.slice(1)) : undefined;
    if (level !== undefined && this.system.fontSizes.has(level)) {
      opening.push(`[size=${this.system.fontSizes.get(level)}%]`);
      closing.push("[/size]");
    } else if (isNumber(size) && size > 0) {
      // Artemis sizes the line box; glyphs match about 0.8 of it.
      opening.push(`[size=${Math.max(1, roundHalfEven(size * 0.8))}]`);
      closing.push("[/size]");
    } else if (size !== null) this.unsupported.bump("text.exfont.size");
    if (isString(color) && color.toLowerCase().startsWith("0x") && (color.length === 8 || color.length === 10)) {
      opening.push(`[color=#${color.slice(2).toUpperCase()}]`);
      closing.push("[/color]");
    } else if (color !== null) this.unsupported.bump("text.exfont.color");
    if (style === "bold" || style === "italic") {
      const tag = style === "bold" ? "b" : "i";
      opening.push(`[${tag}]`);
      closing.push(`[/${tag}]`);
    } else if (style !== null) this.unsupported.bump("text.exfont.style");
    return [opening.join(""), closing.reverse().join("")];
  }

  /** Writes one advance in the shortest Story form that keeps its meaning. */
  private emitText(add: Add, speaker: string | undefined, content: string, voices: readonly string[]): void {
    const alias = speaker ? this.aliasByName.get(speaker) : undefined;
    const options: string[] = [];
    if (alias !== undefined) {
      const matching = this.pendingActs.filter((act) => act[0] === alias);
      if (matching.length === 1 && matching[0]![2].size === 0 && matching[0]![1] !== "stop") {
        options.push(`act=${matching[0]![1]}`);
        this.pendingActs.splice(this.pendingActs.indexOf(matching[0]!), 1);
      }
    }
    const segments = content.split("\n");
    const head = segments[0]!;
    // Prose that the line syntax would trim or misread keeps the long form.
    const short = segments.every(stableText) &&
      (speaker !== undefined || ![";", "*", "[", "@"].some((prefix) => head.startsWith(prefix)));
    if (!short) {
      // The long form has no inline options; act and voice get lines.
      for (const value of options) this.pendingActs.push([alias ?? "", value.slice(value.indexOf("=") + 1), new Map()]);
      options.length = 0;
    }
    this.flushActs(add);
    const inlineVoice = short && speaker !== undefined && voices.length === 1;
    if (!inlineVoice) for (const voice of voices) add(`[voice ${storyToken(voice)}]`);
    if (!short) {
      add(speaker === undefined ? `[narrate text=${JSON.stringify(content)}]`
        : `[say speaker=${JSON.stringify(speaker)} text=${JSON.stringify(content)}${
          alias === undefined ? "" : ` character=${JSON.stringify(alias)}`}]`);
      return;
    }
    if (inlineVoice) options.push(`voice=${storyToken(voices[0]!)}`);
    let first: string;
    if (speaker === undefined) first = head;
    else {
      let name: string;
      if (alias !== undefined) name = this.who(alias);
      else if (identifierPattern.test(speaker) || Array.from(speaker).some((character) =>
        isSourceSpace(character) || speakerTerminators.has(character) || character === "\""))
        name = JSON.stringify(speaker);
      else name = speaker;
      const prefix = `@${name}${options.length > 0 ? `[${options.join(" ")}]` : ""}`;
      first = prefix + (openers.some((opener) => head.startsWith(opener)) && options.length === 0 ? "" : " ") + head;
    }
    add([first, ...segments.slice(1)].join("\n").replaceAll("\n", "\\\n"));
  }
}

function tokenOptions(options: Map<string, string | number>): string {
  return [...options].map(([key, value]) =>
    ` ${key}=${storyToken(typeof value === "number" ? formatNumber(value) : value)}`).join("");
}

function withoutExtension(path: string): string | undefined {
  const slash = path.lastIndexOf("/");
  const dot = path.lastIndexOf(".");
  return dot > slash + 1 ? path.slice(0, dot) : undefined;
}

/** `fg/<alias>/...` portrait folders name their character. */
export function portraitAlias(path: string): string | undefined {
  const parts = trimChars(path, ":/").split("/").filter((part) => part.length > 0);
  return parts.length >= 2 && parts[0] === "fg" && identifierPattern.test(parts[1]!) ? parts[1] : undefined;
}

/** Artemis `pos,x,y[,w,h,...]` image comment: [x, y, crop width, crop height]. */
function imagePosition(comment: string | undefined): [number, number, number | undefined, number | undefined] {
  const parts = (comment ?? "").split(",");
  if (parts.length < 3 || parts[0] !== "pos") return [0, 0, undefined, undefined];
  return [strictInteger(parts[1]!), strictInteger(parts[2]!),
    parts.length >= 6 ? strictInteger(parts[3]!) : undefined,
    parts.length >= 6 ? strictInteger(parts[4]!) : undefined];
}
