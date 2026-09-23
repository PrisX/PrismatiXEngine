import {validateDocument} from "./contracts.js";
import {isSafeResourcePath} from "./resources.js";
import type {ProjectDocument} from "./project.js";
import type {AuthoringDiagnostic, Result} from "./types.js";

type ObjectValue = Record<string, unknown>;

export interface NormalizeProjectInput {
  /** A complete project, a small source manifest, JSON text, or nothing. */
  readonly project?: unknown;
  readonly documents?: Readonly<Record<string, unknown>>;
  readonly sourceFiles?: Readonly<Record<string, string>>;
  readonly locales?: Readonly<Record<string, unknown>>;
  readonly projectPath?: string;
  /** Conventional single-file Story entry. Defaults to Story/main.pxstory. */
  readonly storyPath?: string;
  /** Used only when the source manifest does not declare defaultLocale. */
  readonly defaultLocale?: string;
}

export interface NormalizedProjectInput {
  readonly project: ProjectDocument;
  readonly documents: Readonly<Record<string, unknown>>;
  readonly sourceFiles: Readonly<Record<string, string>>;
  readonly locales: Readonly<Record<string, unknown>>;
  readonly projectPath: string;
  readonly storyPath: string;
  /** Author-only file or directory roots; never serialized into project.pxproject. */
  readonly resourceIncludes: readonly string[];
}

function issue(code: string, message: string, path: string, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    path,
    ...(details === undefined ? {} : {details}),
  };
}

function object(value: unknown): ObjectValue | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as ObjectValue : undefined;
}

function sourceManifest(value: unknown, path: string): Result<ObjectValue> {
  if (value === undefined) return {value: {}, diagnostics: [], valid: true};
  if (typeof value === "string") {
    try {
      const parsed: unknown = JSON.parse(value);
      const record = object(parsed);
      return record === undefined
        ? {diagnostics: [issue("PXBUILD1040", "Project source manifest must be a JSON object", path)], valid: false}
        : {value: record, diagnostics: [], valid: true};
    } catch (error) {
      return {diagnostics: [issue("PXBUILD1040", "Project source manifest is not valid JSON", path, String(error))], valid: false};
    }
  }
  const record = object(value);
  return record === undefined
    ? {diagnostics: [issue("PXBUILD1040", "Project source manifest must be an object", path)], valid: false}
    : {value: record, diagnostics: [], valid: true};
}

function hash64(input: string): string {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(input)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 0x100000001b3n);
  }
  return hash.toString(16).padStart(16, "0");
}

function stableUuid(seed: string): string {
  const value = `${hash64(seed)}${hash64(`${seed}:tail`)}`.split("");
  value[12] = "5";
  value[16] = "8";
  const hex = value.join("");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`;
}

function identifier(name: unknown): string {
  if (typeof name !== "string") return "game";
  const result = name.toLowerCase().replace(/[^a-z0-9._-]+/gu, "-")
    .replace(/^[^a-z0-9]+/u, "").replace(/[^a-z0-9]+$/u, "").slice(0, 256);
  return result.length === 0 ? "game" : result;
}

function conventionalStoryPath(input: NormalizeProjectInput): string {
  if (input.storyPath !== undefined) return input.storyPath;
  if (input.sourceFiles?.["Story/main.pxstory"] !== undefined) return "Story/main.pxstory";
  const candidates = Object.keys(input.sourceFiles ?? {})
    .filter((path) => path.endsWith(".pxstory")).sort();
  return candidates.length === 1 ? candidates[0]! : "Story/main.pxstory";
}

function defaultUi(projectId: string, name: string, width: number, height: number): unknown {
  const sceneId = stableUuid(`${projectId}:default-ui:scene`);
  const rootId = stableUuid(`${projectId}:default-ui:root`);
  const titleId = stableUuid(`${projectId}:default-ui:title`);
  const startId = stableUuid(`${projectId}:default-ui:start`);
  const layout = (x: number, y: number, nodeWidth: number, nodeHeight: number) => ({
    mode: "free", x, y, width: nodeWidth, height: nodeHeight,
    anchorX: 0, anchorY: 0, anchorRight: 0, anchorBottom: 0,
    pivotX: 0, pivotY: 0, margin: 0, alignment: "start", sizeRule: "fixed",
  });
  return {
    format: "PrismatiXUIScene",
    schemaRevision: 2,
    id: sceneId,
    revision: 1,
    name,
    width,
    height,
    rootId,
    nodes: [{
      id: rootId,
      parentId: null,
      order: 0,
      kind: "control",
      runtimeType: "Control",
      name: "Root",
      visible: true,
      locked: false,
      layout: layout(0, 0, width, height),
      runtimeProperties: {},
      bindings: {},
    }, {
      id: titleId,
      parentId: rootId,
      order: 0,
      kind: "label",
      runtimeType: "Label",
      name: "GameTitle",
      visible: true,
      locked: false,
      layout: layout(Math.round(width * 0.1), Math.round(height * 0.16),
        Math.round(width * 0.8), 96),
      text: name,
      runtimeProperties: {fontSize: 52},
      bindings: {},
    }, {
      id: startId,
      parentId: rootId,
      order: 1,
      kind: "button",
      runtimeType: "Button",
      name: "Start",
      visible: true,
      locked: false,
      layout: layout(Math.round((width - 300) / 2), Math.round(height * 0.55), 300, 64),
      text: "Start",
      onClick: {id: "game.start", arguments: {}},
      accessibilityLabel: "Start game",
      accessibilityRole: "button",
      runtimeProperties: {},
      bindings: {},
    }],
    theme: [],
    behaviorGraph: {nodes: [], links: [], groups: []},
    behaviorTriggers: [],
    visualStateGroups: [],
  };
}

/**
 * Expands the beginner-facing single-locale project shape into the unchanged,
 * strict runtime contracts. Explicit advanced configuration always wins.
 * Generated documents are returned in-memory so callers may preview/build a
 * project containing only `Story/main.pxstory` and an optional small manifest.
 */
export function normalizeProject(input: NormalizeProjectInput = {}): Result<NormalizedProjectInput> {
  const projectPath = input.projectPath ?? "project.pxproject";
  const parsedSource = sourceManifest(input.project, projectPath);
  if (!parsedSource.valid || parsedSource.value === undefined)
    return {diagnostics: parsedSource.diagnostics, valid: false};

  const raw = parsedSource.value;
  const includeValue = raw.resourceIncludes;
  if (includeValue !== undefined && (!Array.isArray(includeValue) ||
      includeValue.length > 4096 || !includeValue.every((value: unknown) =>
        typeof value === "string" && isSafeResourcePath(value) &&
        !value.includes(":") && !value.includes("\0") &&
        ![".prismatix", ".git", "node_modules"].includes(value.split("/")[0]!.toLowerCase()) &&
        value.split("/").every((part) => part.length > 0 && part !== "."))))
    return {diagnostics: [issue("PXBUILD1051",
      "resourceIncludes must be an array of safe project-relative file or directory paths", projectPath)],
    valid: false};
  const resourceIncludes = includeValue as readonly string[] | undefined;
  const runtimeSource = {...raw};
  delete runtimeSource.resourceIncludes;
  const name = typeof raw.name === "string" ? raw.name : "PrismatiX Game";
  const projectId = typeof raw.id === "string" ? raw.id : identifier(name);
  const locale = typeof raw.defaultLocale === "string" ? raw.defaultLocale
    : input.defaultLocale ?? "en-US";
  const supportedLocales = raw.supportedLocales ?? [locale];
  const resolution = object(raw.resolution) ?? {};
  const width = typeof resolution.width === "number" ? resolution.width : 1280;
  const height = typeof resolution.height === "number" ? resolution.height : 720;
  const storyIndexPath = typeof raw.storyIndex === "string"
    ? raw.storyIndex : "Story/story.pxindex";
  const gameCatalogPath = typeof raw.gameCatalog === "string"
    ? raw.gameCatalog : "Content/game.pxgame";
  const uiEntryPoints = object(raw.uiEntryPoints);
  const defaultUiPath = "Content/UI/Title.pxui";
  const defaultUiRoute = uiEntryPoints === undefined
    ? "title" : Object.keys(uiEntryPoints).sort()[0] ?? "title";
  const entry = object(raw.entry) ?? {};
  const explicitStoryIndex = object(input.documents?.[storyIndexPath]);
  const entryStory = typeof entry.story === "string" ? entry.story
    : typeof explicitStoryIndex?.entryScene === "string" ? explicitStoryIndex.entryScene : "main";
  const storyPath = conventionalStoryPath(input);

  const candidate: unknown = {
    format: "PrismatiXProject",
    schemaRevision: 2,
    id: projectId,
    name,
    version: "0.1.0",
    contentVersion: "1",
    saveVersion: 1,
    defaultLocale: locale,
    supportedLocales,
    storyIndex: storyIndexPath,
    gameCatalog: gameCatalogPath,
    extensions: [],
    uiEntryPoints: uiEntryPoints ?? {[defaultUiRoute]: defaultUiPath},
    ...runtimeSource,
    resolution: {...{width, height}, ...resolution},
    entry: {
      story: entryStory,
      ui: typeof entry.ui === "string" ? entry.ui : defaultUiRoute,
    },
  };
  const validated = validateDocument<ProjectDocument>("project", candidate, projectPath);
  if (!validated.valid || validated.value === undefined)
    return {diagnostics: validated.diagnostics, valid: false};

  const project = validated.value;
  const documents: Record<string, unknown> = {...input.documents};
  const locales: Record<string, unknown> = {...input.locales};
  if (raw.storyIndex === undefined && documents[project.storyIndex] === undefined) {
    documents[project.storyIndex] = {
      format: "PrismatiXStoryIndex",
      schemaRevision: 2,
      id: "main",
      entryScene: project.entry.story,
      chapters: [{id: "main", title: name, scenes: [project.entry.story]}],
      scenes: [{id: project.entry.story, sources: {[project.defaultLocale]: storyPath}}],
    };
  }
  if (raw.gameCatalog === undefined && documents[project.gameCatalog] === undefined) {
    documents[project.gameCatalog] = {
      format: "PrismatiXGame", schemaRevision: 2,
      variables: [], gallery: [], unlockables: [],
    };
  }
  if (raw.uiEntryPoints === undefined && documents[defaultUiPath] === undefined) {
    documents[defaultUiPath] = defaultUi(project.id, name,
      project.resolution.width, project.resolution.height);
  }
  if (project.supportedLocales.length === 1 && locales[project.defaultLocale] === undefined) {
    locales[project.defaultLocale] = {
      format: "PrismatiXLocale", schemaRevision: 2,
      locale: project.defaultLocale, strings: {},
    };
  }

  return {
    value: {
      project,
      documents,
      sourceFiles: input.sourceFiles ?? {},
      locales,
      projectPath,
      storyPath,
      resourceIncludes: resourceIncludes ?? [],
    },
    diagnostics: validated.diagnostics,
    valid: true,
  };
}
