import {existsSync, readFileSync, readdirSync, statSync} from "node:fs";
import {readFile} from "node:fs/promises";
import {dirname, isAbsolute, join, relative, resolve, sep} from "node:path";

import type {
  AuthoringDiagnostic,
  CharacterDocument,
  ExtensionManifest,
  GameDocument,
  GameVariable,
  ProjectAssetDescriptor,
  ProjectDocument,
  StoryAssetCategory,
  StoryAssetIndex,
  StoryAssetRoots,
  StoryIndexDocument,
  StoryProjectCompileContext,
} from "@prismatix/authoring-sdk";

import {loadSdk, type LoadedSdk, type SdkMode} from "./sdk.js";

export const manifestNames = ["prismatix.json", "project.pxproject"] as const;

/** A label definition in one Story source. Lines are zero-based. */
export interface LabelInfo {
  readonly name: string;
  readonly line: number;
}

export interface SceneSource {
  readonly sceneId: string;
  readonly locale: string;
  readonly path: string;
  readonly title?: string;
}

export interface ExpressionInfo {
  readonly id: string;
  readonly name: string;
  readonly aliases: readonly string[];
  /** Project-relative image (or .pxportrait) shown for the expression. */
  readonly assetPath?: string;
  readonly facePath?: string;
}

export interface CharacterInfo {
  readonly id: string;
  readonly displayName: string;
  readonly displayNames: Readonly<Record<string, string>>;
  readonly aliases: readonly string[];
  readonly expressions: readonly ExpressionInfo[];
  /** Project-relative .pxcharacter path. */
  readonly source: string;
}

/**
 * compileStoryProject rewrites cross-scene targets to internal labels before
 * compiling a scene, so an unknown scene or label is reported twice: once
 * clearly (PXSTORY1304/1305) and once as an unresolved internal target
 * (PXSTORY1117/1118/1119 naming `pxscene.*`, or PXSTORY1215). Keep the
 * clear one.
 */
export function withoutRewriteEchoes(diagnostics: readonly AuthoringDiagnostic[]): AuthoringDiagnostic[] {
  const lineKey = (diagnostic: AuthoringDiagnostic) => diagnostic.span === undefined ? undefined
    : `${diagnostic.span.path}:${diagnostic.span.start.line}`;
  const flowErrors = new Set(diagnostics
    .filter((diagnostic) => diagnostic.code === "PXSTORY1304" || diagnostic.code === "PXSTORY1305")
    .map(lineKey));
  const unknownTarget = new Set(["PXSTORY1117", "PXSTORY1118", "PXSTORY1119"]);
  return diagnostics.filter((diagnostic) => {
    if (unknownTarget.has(diagnostic.code)) {
      if (!diagnostic.message.includes("pxscene.")) return true;
    } else if (diagnostic.code !== "PXSTORY1215") return true;
    return !flowErrors.has(lineKey(diagnostic));
  });
}

/** Normalizes a filesystem path so Windows drive letters compare equal. */
export function normalizeFsPath(path: string): string {
  const absolute = resolve(path);
  return process.platform === "win32" && /^[A-Za-z]:/u.test(absolute)
    ? absolute[0]!.toLowerCase() + absolute.slice(1) : absolute;
}

/** Walks up from a Story file to the directory holding prismatix.json. */
export function findProjectRoot(filePath: string): string | undefined {
  let directory = dirname(normalizeFsPath(filePath));
  for (;;) {
    for (const name of manifestNames)
      if (existsSync(join(directory, name))) return directory;
    const parent = dirname(directory);
    if (parent === directory) return undefined;
    directory = parent;
  }
}

function object(value: unknown): Record<string, unknown> | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : undefined;
}

function safeVirtualPath(value: unknown): value is string {
  return typeof value === "string" && value.length > 0 && !value.startsWith("/") &&
    !value.includes("\\") && !value.includes(":") && !value.includes("\0") &&
    value.split("/").every((part) => part.length > 0 && part !== "." && part !== "..");
}

function issue(code: string, message: string, path?: string, details?: string,
  severity: AuthoringDiagnostic["severity"] = "warning"): AuthoringDiagnostic {
  return {severity, code, message, ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details})};
}

const imageExtensions = /\.(?:png|jpe?g|webp|bmp|gif|tga|pxportrait)$/iu;
const audioExtensions = /\.(?:ogg|opus|wav|mp3|flac|m4a)$/iu;
const videoExtensions = /\.(?:mp4|webm|mkv|avi|mov|ogv|mpg|mpeg|ts|wmv)$/iu;

/** File kinds that make sense for a Story resource category when a project
 * has no storyAssets roots and Story commands take project paths instead. */
export function extensionFilterFor(category: StoryAssetCategory | "font" | "any"): RegExp {
  switch (category) {
    case "bg": case "rule": case "sprite": return imageExtensions;
    case "bgm": case "se": case "voice": case "ambience": return audioExtensions;
    case "video": return videoExtensions;
    case "font": return /\.(?:ttf|otf|ttc)$/iu;
    default: return /./u;
  }
}

/**
 * Everything the editor needs from one PrismatiX project, loaded the way the
 * CLI loads it but leniently: a broken document degrades features instead of
 * failing. Only Story label tables are kept for other scenes, not their text.
 */
export class ProjectModel {
  readonly loadDiagnostics: AuthoringDiagnostic[] = [];
  project: ProjectDocument | undefined;
  storyIndex: StoryIndexDocument | undefined;
  storyIndexPath = "Story/story.pxindex";
  characters: CharacterDocument[] = [];
  characterInfos: CharacterInfo[] = [];
  characterSources = new Map<string, string>();
  game: GameDocument | undefined;
  extensions: ExtensionManifest[] = [];
  resourceAssets: ProjectAssetDescriptor[] = [];
  private assetAliases: Record<string, string> = {};
  private localeResources = new Map<string, Record<string, string>>();
  storyAssetRoots: StoryAssetRoots | undefined;
  /** prismatix.json `storyMacros`: files every scene may call macros from. */
  storyMacroPaths: readonly string[] = [];
  storyAssets: StoryAssetIndex | undefined;
  resourceIncludes: readonly string[] = [];
  /** Files the build packages: resourceIncludes plus cataloged asset sources. */
  knownFiles: readonly string[] = [];
  readonly scenes = new Map<string, SceneSource>();
  private readonly labels = new Map<string, readonly LabelInfo[]>();
  private readonly labelLoads = new Map<string, Promise<void>>();
  private shortNames = new Map<StoryAssetCategory, readonly {name: string; path: string}[]>();
  private directoryCache = new Map<string, ReadonlySet<string> | null>();

  private constructor(readonly root: string, readonly manifestPath: string,
    readonly sdk: LoadedSdk) {}

  get sdkApi() { return this.sdk.sdk; }
  get valid(): boolean { return this.project !== undefined && this.storyIndex !== undefined; }
  get projectId(): string { return this.project?.id ?? "project"; }
  get defaultLocale(): string { return this.project?.defaultLocale ?? "en-US"; }
  get variables(): readonly GameVariable[] { return this.game?.variables ?? []; }

  static async load(root: string, sdkMode: SdkMode = "auto"): Promise<ProjectModel> {
    const normalizedRoot = normalizeFsPath(root);
    const manifestName = manifestNames.find((name) => existsSync(join(normalizedRoot, name)));
    const sdk = await loadSdk(normalizedRoot, sdkMode);
    const model = new ProjectModel(normalizedRoot,
      join(normalizedRoot, manifestName ?? manifestNames[0]), sdk);
    if (sdk.note !== undefined)
      model.loadDiagnostics.push(issue("PXLSP0002", `Using the SDK bundled with the extension; ${sdk.note}`));
    try {
      await model.populate();
    } catch (error) {
      model.project = undefined;
      model.loadDiagnostics.push(issue("PXLSP0001", "PrismatiX project could not be loaded; only Story syntax is checked",
        model.manifestVirtualPath, error instanceof Error ? error.message : String(error)));
    }
    return model;
  }

  get manifestVirtualPath(): string { return this.toVirtual(this.manifestPath) ?? "prismatix.json"; }

  toVirtual(fsPath: string): string | undefined {
    const back = relative(this.root, normalizeFsPath(fsPath));
    if (back === "" || back === ".." || back.startsWith(`..${sep}`) || isAbsolute(back)) return undefined;
    return back.split(sep).join("/");
  }

  toFsPath(virtualPath: string): string {
    return join(this.root, ...virtualPath.split("/"));
  }

  private async readText(virtualPath: string): Promise<string> {
    return readFile(this.toFsPath(virtualPath), "utf8");
  }

  private async readJson(virtualPath: string, into: Record<string, unknown>): Promise<void> {
    if (!safeVirtualPath(virtualPath) || Object.hasOwn(into, virtualPath)) return;
    try {
      into[virtualPath] = JSON.parse(await this.readText(virtualPath)) as unknown;
    } catch (error) {
      this.loadDiagnostics.push(issue("PXLSP0003", `Project document could not be read: ${virtualPath}`,
        virtualPath, error instanceof Error ? error.message : String(error)));
    }
  }

  private async populate(): Promise<void> {
    const sdk = this.sdk.sdk;
    const authored = object(JSON.parse(await readFile(this.manifestPath, "utf8")) as unknown);
    if (authored === undefined) throw new Error("the project manifest must be a JSON object");
    // The CLI publishes TSX UI as compiled .pxui paths before normalizing; the
    // Story editor never needs UI documents, only a manifest that validates.
    const raw = structuredClone(authored);
    const entryPoints = object(raw.uiEntryPoints);
    for (const [route, path] of Object.entries(entryPoints ?? {}))
      if (typeof path === "string") entryPoints![route] = path.replace(/\.(?:jsx|tsx)$/iu, ".pxui");
    for (const component of Array.isArray(raw.uiComponents) ? raw.uiComponents : []) {
      const descriptor = object(component);
      if (typeof descriptor?.source === "string")
        descriptor.source = descriptor.source.replace(/\.(?:jsx|tsx)$/iu, ".pxuicomponent");
    }
    const splashes = object(raw.settings)?.["player.splashes"];
    for (const splash of Array.isArray(splashes) ? splashes : []) {
      const descriptor = object(splash);
      if (typeof descriptor?.scene === "string")
        descriptor.scene = descriptor.scene.replace(/\.(?:jsx|tsx)$/iu, ".pxui");
    }

    const documents: Record<string, unknown> = Object.create(null) as Record<string, unknown>;
    if (typeof raw.storyIndex === "string") await this.readJson(raw.storyIndex, documents);
    if (typeof raw.gameCatalog === "string") await this.readJson(raw.gameCatalog, documents);
    for (const path of Array.isArray(raw.extensions) ? raw.extensions : [])
      if (typeof path === "string") await this.readJson(path, documents);
    for (const descriptor of Array.isArray(raw.characters) ? raw.characters : []) {
      const source = object(descriptor)?.source;
      if (typeof source === "string") await this.readJson(source, documents);
    }
    for (const path of Object.keys(documents))
      if (path.endsWith(".pxcharacter")) {
        try { this.characterSources.set(path, await this.readText(path)); } catch { /* reported above */ }
      }
    const locales: Record<string, unknown> = {};
    const supported = Array.isArray(raw.supportedLocales) ? raw.supportedLocales
      : [typeof raw.defaultLocale === "string" ? raw.defaultLocale : "en-US"];
    for (const locale of supported) {
      if (typeof locale !== "string") continue;
      const path = `Content/Localization/${locale}.json`;
      try {
        locales[locale] = JSON.parse(await this.readText(path)) as unknown;
      } catch { /* A missing locale document is a project validation error, not an editor one. */ }
    }
    const storyPath = typeof raw.storyIndex === "string" ? undefined : this.conventionalStory();

    const normalized = sdk.normalizeProject({project: raw, documents, locales,
      projectPath: "project.pxproject", ...(storyPath === undefined ? {} : {storyPath})});
    if (!normalized.valid || normalized.value === undefined) {
      for (const diagnostic of normalized.diagnostics)
        this.loadDiagnostics.push({...diagnostic, severity: "warning",
          ...(diagnostic.path === "project.pxproject" ? {path: this.manifestVirtualPath} : {})});
      throw new Error(normalized.diagnostics[0]?.message ?? "the project manifest is invalid");
    }
    const value = normalized.value;
    const project = value.project;
    this.project = project;
    this.storyIndexPath = project.storyIndex;
    const index = value.documents[project.storyIndex];
    const scenes = object(index)?.scenes;
    if (!Array.isArray(scenes))
      throw new Error(`${project.storyIndex} is missing or has no scenes`);
    this.storyIndex = index as StoryIndexDocument;
    for (const scene of this.storyIndex.scenes) {
      for (const [locale, path] of Object.entries(object(scene?.sources) ?? {}))
        if (typeof path === "string" && typeof scene.id === "string")
          this.scenes.set(path, {sceneId: scene.id, locale, path,
            ...(typeof scene.title === "string" ? {title: scene.title} : {})});
    }

    const validated = <T>(contract: string, path: string): T | undefined => {
      const document = value.documents[path];
      if (document === undefined) return undefined;
      const result = sdk.validateDocument<T>(contract, document, path);
      if (!result.valid) {
        this.loadDiagnostics.push(issue("PXLSP0004", `${path} is not a valid ${contract} document`, path,
          result.diagnostics[0]?.message));
        return undefined;
      }
      return result.value;
    };
    this.game = validated<GameDocument>("game", project.gameCatalog);
    for (const path of project.extensions) {
      const extension = validated<ExtensionManifest>("extension", path);
      if (extension !== undefined) this.extensions.push(extension);
    }

    this.resourceAssets = [];
    for (const asset of project.assets ?? []) {
      const record = object(asset);
      if (typeof record?.id === "string" && typeof record.source === "string" &&
          typeof record.kind === "string") {
        this.resourceAssets.push({id: record.id, source: record.source, kind: record.kind,
          ...(typeof record.name === "string" ? {name: record.name} : {})});
        this.assetAliases[record.id] = `asset:${record.id.toLowerCase()}`;
      }
    }
    for (const [locale, document] of Object.entries(value.locales)) {
      const resources = object(object(document)?.resources);
      const aliases: Record<string, string> = {};
      for (const [id, path] of Object.entries(resources ?? {}))
        if (typeof path === "string") aliases[id] = path;
      this.localeResources.set(locale, aliases);
    }

    for (const descriptor of project.characters ?? []) {
      const character = validated<CharacterDocument>("character", descriptor.source);
      if (character === undefined) continue;
      this.characters.push(character);
      const authoredCharacter = object(documents[descriptor.source]);
      const authoredExpressions = Array.isArray(authoredCharacter?.expressions)
        ? authoredCharacter.expressions : [];
      this.characterInfos.push({
        id: character.id,
        displayName: character.displayName,
        displayNames: character.displayNames ?? {},
        aliases: character.aliases,
        source: descriptor.source,
        expressions: character.expressions.map((expression, position) => {
          const authoredExpression = object(authoredExpressions[position]);
          const assetPath = typeof authoredExpression?.assetPath === "string"
            ? authoredExpression.assetPath
            : this.resourceAssets.find((asset) => asset.id === expression.assetId)?.source;
          const facePath = typeof authoredExpression?.facePath === "string"
            ? authoredExpression.facePath
            : expression.faceAssetId === undefined ? undefined
              : this.resourceAssets.find((asset) => asset.id === expression.faceAssetId)?.source;
          return {id: expression.id, name: expression.name, aliases: expression.aliases,
            ...(assetPath === undefined ? {} : {assetPath}),
            ...(facePath === undefined ? {} : {facePath})};
        }),
      });
    }

    this.resourceIncludes = value.resourceIncludes;
    this.storyAssetRoots = value.storyAssets;
    this.storyMacroPaths = (value as {storyMacros?: readonly string[]}).storyMacros ?? [];
    this.refreshFiles();
  }

  private conventionalStory(): string | undefined {
    if (existsSync(this.toFsPath("Story/main.pxstory"))) return "Story/main.pxstory";
    const found: string[] = [];
    const visit = (virtual: string): void => {
      let entries;
      try { entries = readdirSync(this.toFsPath(virtual), {withFileTypes: true}); } catch { return; }
      for (const entry of entries) {
        const path = `${virtual}/${entry.name}`;
        if (entry.isDirectory()) visit(path);
        else if (entry.isFile() && entry.name.toLowerCase().endsWith(".pxstory")) found.push(path);
      }
    };
    visit("Story");
    return found.length === 1 ? found[0] : undefined;
  }

  /** Re-expands resourceIncludes after files were created or deleted. */
  refreshFiles(): void {
    const files = new Set<string>();
    for (const include of this.resourceIncludes) {
      if (!safeVirtualPath(include) ||
          [".prismatix", ".git", "node_modules"].includes(include.split("/")[0]!.toLowerCase())) continue;
      const visit = (virtual: string): void => {
        let entries;
        try { entries = readdirSync(this.toFsPath(virtual), {withFileTypes: true}); } catch { return; }
        for (const entry of entries) {
          const path = `${virtual}/${entry.name}`;
          if (entry.isDirectory()) visit(path);
          else if (entry.isFile()) files.add(path);
        }
      };
      let info;
      try { info = statSync(this.toFsPath(include)); } catch { continue; }
      if (info.isDirectory()) visit(include);
      else if (info.isFile()) files.add(include);
    }
    for (const asset of this.resourceAssets) files.add(asset.source);
    this.knownFiles = [...files].sort();
    this.storyAssets = this.storyAssetRoots === undefined ? undefined
      : this.sdk.sdk.createStoryAssetIndex(this.storyAssetRoots, this.knownFiles);
    this.shortNames.clear();
    this.directoryCache.clear();
  }

  resourcesFor(locale: string): Readonly<Record<string, string>> {
    return {...this.assetAliases, ...this.localeResources.get(locale)};
  }

  /** The Story compile context shared by the live and whole-project checks. */
  compileContext(locale: string, sourceFiles: Readonly<Record<string, string>>): StoryProjectCompileContext {
    if (this.storyIndex === undefined) throw new Error("project has no Story index");
    // Open editors win over the files on disk.
    const read = (path: string): string | undefined => {
      if (sourceFiles[path] !== undefined) return sourceFiles[path];
      try { return readFileSync(this.toFsPath(path), "utf8"); } catch { return undefined; }
    };
    // Newer SDK features are used only when the project's SDK provides them.
    const api = this.sdk.sdk as Partial<Record<string, unknown>>;
    let storyMacros: StoryProjectCompileContext["storyMacros"];
    if (this.storyMacroPaths.length > 0 && typeof api.createStoryMacroLibrary === "function")
      storyMacros = (api.createStoryMacroLibrary as (files: {path: string; source: string}[],
        extensions: readonly ExtensionManifest[]) => {library: StoryProjectCompileContext["storyMacros"]})(
        this.storyMacroPaths.map((path) => ({path, source: read(path) ?? ""})), this.extensions).library;
    let portraits: StoryProjectCompileContext["portraits"];
    if (typeof api.createStoryPortraitIndex === "function" &&
        Object.values(sourceFiles).some((source) => /^[ \t]*\[\s*face\s/mu.test(source))) {
      const documents: Record<string, unknown> = {};
      for (const character of this.characterInfos) for (const expression of character.expressions) {
        const path = expression.assetPath;
        if (path === undefined || !path.endsWith(".pxportrait") || path in documents) continue;
        const text = read(path);
        const parsed = text === undefined ? undefined
          : this.sdk.sdk.parseDocument<unknown>("portrait", text, path);
        if (parsed?.valid === true) documents[path] = parsed.value;
      }
      portraits = (api.createStoryPortraitIndex as (documents: Record<string, unknown>) =>
        StoryProjectCompileContext["portraits"])(documents);
    }
    let game = this.game;
    if (game !== undefined && typeof api.collectStoryVariableDeclarations === "function" &&
        typeof api.mergeStoryVariables === "function") {
      const files = this.sceneSources(locale).flatMap((scene) => {
        const source = read(scene.path);
        return source === undefined ? [] : [{path: scene.path, source}];
      });
      const declared = (api.collectStoryVariableDeclarations as (files: {path: string; source: string}[]) =>
        {declarations: unknown[]})(files);
      game = (api.mergeStoryVariables as (game: GameDocument, declarations: unknown[]) =>
        {game: GameDocument})(game, declared.declarations).game;
    }
    return {
      ...(storyMacros === undefined ? {} : {storyMacros}),
      ...(portraits === undefined ? {} : {portraits}),
      storyIndex: this.storyIndex,
      storyIndexPath: this.storyIndexPath,
      locale,
      sourceFiles,
      documentId: this.projectId,
      characters: this.characters,
      ...(game === undefined ? {} : {game}),
      extensions: this.extensions,
      resources: this.resourcesFor(locale),
      resourceAssets: this.resourceAssets,
      ...(this.storyAssets === undefined ? {} : {storyAssets: this.storyAssets}),
    };
  }

  /** Scene sources for one locale in index order. */
  sceneSources(locale: string): SceneSource[] {
    return [...this.scenes.values()].filter((scene) => scene.locale === locale);
  }

  sceneById(sceneId: string, locale: string): SceneSource | undefined {
    for (const scene of this.scenes.values())
      if (scene.sceneId === sceneId && scene.locale === locale) return scene;
    return undefined;
  }

  /** Labels of every scene in a locale, parsed once with the SDK parser. */
  ensureLabels(locale: string): Promise<void> {
    let pending = this.labelLoads.get(locale);
    if (pending === undefined) {
      pending = (async () => {
        let parsed = 0;
        for (const scene of this.sceneSources(locale)) {
          if (this.labels.has(scene.path)) continue;
          let text: string;
          try { text = await this.readText(scene.path); } catch { continue; }
          if (!this.labels.has(scene.path)) this.labels.set(scene.path, this.extractLabels(text, scene.path));
          // Keep the server responsive while a large project is indexed.
          if (++parsed % 16 === 0) await new Promise((resolveTick) => setImmediate(resolveTick));
        }
      })();
      this.labelLoads.set(locale, pending);
    }
    return pending;
  }

  extractLabels(text: string, path: string): LabelInfo[] {
    return this.sdk.sdk.parseStory(text, path).nodes
      .filter((node) => node.kind === "label" && node.name !== undefined)
      .map((node) => ({name: node.name!, line: node.span.start.line - 1}));
  }

  labelsOf(path: string): readonly LabelInfo[] | undefined { return this.labels.get(path); }

  /** Records labels from an open document or a changed file. Returns whether
   * the label set changed, which invalidates other scenes' cross-scene checks. */
  setLabels(path: string, labels: readonly LabelInfo[]): boolean {
    const previous = this.labels.get(path);
    this.labels.set(path, labels);
    return previous === undefined || previous.length !== labels.length ||
      previous.some((label, position) => label.name !== labels[position]!.name);
  }

  async reloadLabels(path: string): Promise<boolean> {
    let text: string;
    try { text = await this.readText(path); } catch { return this.setLabels(path, []); }
    return this.setLabels(path, this.extractLabels(text, path));
  }

  /** Short names a Story may use for each file under a category's roots,
   * chosen so the SDK's own resolver maps the name back to that file. */
  assetNames(category: StoryAssetCategory): readonly {name: string; path: string}[] {
    const cached = this.shortNames.get(category);
    if (cached !== undefined) return cached;
    const roots = this.storyAssetRoots?.[category];
    const index = this.storyAssets;
    const result: {name: string; path: string}[] = [];
    if (roots !== undefined && index !== undefined) {
      const seen = new Set<string>();
      for (const root of roots) {
        const prefix = `${root.replace(/\/+$/u, "")}/`;
        for (const path of this.knownFiles) {
          if (!path.startsWith(prefix) || seen.has(path)) continue;
          seen.add(path);
          const relativePath = path.slice(prefix.length);
          const fileName = relativePath.slice(relativePath.lastIndexOf("/") + 1);
          const stem = (value: string) => {
            const dot = value.lastIndexOf(".");
            return dot > value.lastIndexOf("/") + 1 ? value.slice(0, dot) : undefined;
          };
          const candidates = [stem(fileName), fileName, stem(relativePath), relativePath, path];
          for (const candidate of candidates) {
            if (candidate === undefined) continue;
            const lookup = index.resolve(category, candidate);
            if (lookup?.kind === "resolved" && lookup.path === path) {
              result.push({name: candidate, path});
              break;
            }
          }
        }
      }
    }
    this.shortNames.set(category, result);
    return result;
  }

  /** Project-relative files usable where a category has no storyAssets roots. */
  filesFor(category: StoryAssetCategory | "font" | "any"): readonly string[] {
    const filter = extensionFilterFor(category);
    return this.knownFiles.filter((path) => filter.test(path));
  }

  /** Resolves a Story resource argument the way compileStory does: author
   * aliases, then storyAssets short names, then a plain project path. */
  resolveAsset(category: StoryAssetCategory | undefined, authored: string, locale = this.defaultLocale):
    {kind: "resolved"; path: string} | {kind: "ambiguous"; candidates: readonly string[]} |
    {kind: "missing"; roots: readonly string[]} | undefined {
    const aliased = this.resourcesFor(locale)[authored];
    const aliasTarget = aliased?.startsWith("asset:")
      ? this.resourceAssets.find((asset) => `asset:${asset.id.toLowerCase()}` === aliased)?.source : aliased;
    if (aliasTarget !== undefined) return {kind: "resolved", path: aliasTarget};
    const cataloged = /^(?:asset:)?([0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12})$/iu.exec(authored)?.[1];
    if (cataloged !== undefined) {
      const asset = this.resourceAssets.find((candidate) => candidate.id.toLowerCase() === cataloged.toLowerCase());
      return asset === undefined ? undefined : {kind: "resolved", path: asset.source};
    }
    const lookup = category === undefined ? undefined : this.storyAssets?.resolve(category, authored);
    if (lookup !== undefined) {
      if (lookup.kind !== "missing" || !(authored.includes("/") || /\.[A-Za-z0-9]+$/u.test(authored)))
        return lookup;
    }
    return safeVirtualPath(authored) ? {kind: "resolved", path: authored} : undefined;
  }

  /** Existence check for resources that compile only by path. */
  fileExists(virtualPath: string): boolean {
    if (!safeVirtualPath(virtualPath)) return false;
    const absolute = this.toFsPath(virtualPath);
    const directory = dirname(absolute);
    let entries = this.directoryCache.get(directory);
    if (entries === undefined) {
      try { entries = new Set(readdirSync(directory)); } catch { entries = null; }
      this.directoryCache.set(directory, entries);
    }
    return entries?.has(absolute.slice(directory.length + 1)) === true || existsSync(absolute);
  }

  invalidateFileCache(): void { this.directoryCache.clear(); }

  /** Resolves a speaker or character argument like the SDK's character index:
   * id or alias first, then a display name used by exactly one character. */
  findCharacter(name: string): CharacterInfo | undefined {
    const direct = this.characterInfos.find((character) =>
      character.id === name || character.aliases.includes(name));
    if (direct !== undefined) return direct;
    const matches = this.characterInfos.filter((character) =>
      character.displayName === name || Object.values(character.displayNames).includes(name));
    return matches.length === 1 ? matches[0] : undefined;
  }

  /** Display names that `@名前` and `[show 名前]` resolve to one character. */
  uniqueDisplayNames(): Map<string, CharacterInfo> {
    const owners = new Map<string, CharacterInfo[]>();
    for (const character of this.characterInfos)
      for (const name of new Set([character.displayName, ...Object.values(character.displayNames)]))
        owners.set(name, [...owners.get(name) ?? [], character]);
    const result = new Map<string, CharacterInfo>();
    const shadowed = new Set(this.characterInfos.flatMap((character) => [character.id, ...character.aliases]));
    for (const [name, list] of owners)
      if (list.length === 1 && !shadowed.has(name)) result.set(name, list[0]!);
    return result;
  }
}
