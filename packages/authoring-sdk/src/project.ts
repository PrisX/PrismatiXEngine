import {createStoryAssetIndex} from "./story-assets.js";
import {collectStoryVariableDeclarations, createStoryMacroLibrary} from "./story.js";
import {mergeStoryVariables} from "./story-variables.js";
import type {StoryMacroLibrary} from "./story-macros.js";
import {canonicalJson} from "./canonical-json.js";
import {contractDocumentByteLimit, parseDocument, validateDocument} from "./contracts.js";
import {normalizeProject} from "./project-normalization.js";
import {validateInputKeyUsage, type InputKeyRegistryMetadata} from "./input-keys.js";
import {createJsonSourceLocator, jsonPointerSpans} from "./json-source-spans.js";
import {validateHostActionContract, validateUiActionUsage,
  type HostActionContract} from "./project-actions.js";
import {validatePlayerUiBindings} from "./project-ui-bindings.js";
import {closestName} from "./player-ui-contract.js";
import {contractSchemas} from "./generated/contracts.js";
import {createStoryPortraitIndex, type LayeredPortraitDocument,
  type StoryPortraitIndex} from "./portrait.js";
import type {MotionDocument} from "./motion.js";
import {validateProjectCharacterResources} from "./project-resources.js";
import {validateUiPropertyUsage, type UiTypeRegistryMetadata} from "./semantic.js";
import {
  collectProjectDependencies,
  canonicalResourceReference,
  isSafeResourcePath,
  type ProjectAssetDescriptor,
  type ProjectDependency,
  type ProjectDependencyReference,
} from "./resources.js";
import {compileStoryProject, type StoryIndexDocument, type StoryProjectCompilationCache,
  type StoryProjectProgress} from "./story-project.js";
import {ENGINE_VERSION} from "./version.js";
import type {
  AuthoringDiagnostic,
  CharacterDocument,
  ExtensionManifest,
  GameDocument,
  JsonValue,
  RuntimeIrDocument,
  RuntimeSourceMap,
  SourceSpan,
} from "./types.js";

export interface ProjectDocument {
  readonly format: "PrismatiXProject";
  readonly schemaRevision: 2;
  readonly id: string;
  readonly name: string;
  readonly version: string;
  readonly contentVersion: string;
  readonly saveVersion: number;
  readonly graphicsTier?: "basic" | "gpu-effects";
  readonly effects?: readonly CustomEffectDescriptor[];
  readonly saveMigrations?: readonly SaveMigrationDescriptor[];
  readonly resolution: {readonly width: number; readonly height: number};
  readonly entry: {readonly story: string; readonly ui: string};
  readonly gameCatalog: string;
  readonly defaultLocale: string;
  readonly supportedLocales: readonly string[];
  readonly storyIndex: string;
  readonly uiEntryPoints: Readonly<Record<string, string>>;
  readonly extensions: readonly string[];
  readonly assets?: readonly JsonValue[];
  readonly characters?: readonly {
    readonly id: string;
    readonly displayName?: string;
    readonly source: string;
  }[];
  readonly uiComponents?: readonly {
    readonly id: string;
    readonly name?: string;
    readonly source: string;
  }[];
  readonly settings?: Readonly<Record<string, JsonValue>>;
}

export interface CustomEffectDescriptor {
  readonly id: string;
  readonly source: string;
}

export interface CustomEffectUniform {
  readonly name: string;
  readonly type: "number" | "vec2" | "color";
  readonly slot: number;
  readonly default: JsonValue;
  readonly minimum?: number;
  readonly maximum?: number;
}

export interface CustomEffectDocument {
  readonly format: "PrismatiXEffect";
  readonly schemaRevision: 2 | 3;
  readonly id: string;
  readonly targetLayer: "stage" | "node" | "transition";
  readonly shader: string;
  readonly uniforms: readonly CustomEffectUniform[];
}

export interface SaveMigrationVersion {
  readonly contentVersion: string;
  readonly saveVersion: number;
}

export interface SaveMigrationDescriptor {
  readonly id: string;
  readonly from: SaveMigrationVersion;
  readonly to: SaveMigrationVersion;
  readonly asset: string;
}

export interface SaveMigrationAnchor {
  readonly runtimeDocumentId: string;
  readonly sourceId: string;
  readonly operationId: string;
}

export type SaveMigrationAnchorPolicy =
  | {readonly policy: "preserve"}
  | {readonly policy: "map" | "map-or-preserve";
    readonly mappings: readonly {readonly from: SaveMigrationAnchor;
      readonly to: SaveMigrationAnchor}[]};

export interface SaveMigrationDocument {
  readonly format: "PrismatiXSaveMigration";
  readonly schemaRevision: 2;
  readonly id: string;
  readonly from: SaveMigrationVersion;
  readonly to: SaveMigrationVersion;
  readonly anchor: SaveMigrationAnchorPolicy;
  readonly operations: readonly JsonValue[];
}

export interface CompileProjectInput {
  readonly project?: unknown;
  /** Canonical JSON documents keyed by project-relative path. */
  readonly documents?: Readonly<Record<string, unknown>>;
  /** Exact source text for JSON documents, used only for matching diagnostics. */
  readonly documentSources?: Readonly<Record<string, string>>;
  /** Exact author manifest text and its authored path, used only for diagnostics. */
  readonly projectSource?: {readonly path: string; readonly text: string};
  /** Story source text keyed by the exact paths used by story.pxindex. */
  readonly sourceFiles?: Readonly<Record<string, string>>;
  /** Exact JSON source for storyIndex, when available from the loader. */
  readonly storyIndexSource?: string;
  /** Locale documents keyed by canonical locale tag. */
  readonly locales?: Readonly<Record<string, unknown>>;
  readonly projectPath?: string;
  /** Conventional source used when a Story index is generated. */
  readonly storyPath?: string;
  /** Single-locale default used only when the source manifest omits it. */
  readonly defaultLocale?: string;
  /** Optional IO-injected existence check for production graph validation. */
  readonly resourceExists?: (path: string) => boolean;
  /** Optional text reader for dependency-bearing authored resource descriptors. */
  readonly resourceText?: (path: string) => string | undefined;
  /** IO-injected expansion of author-only `resourceIncludes` roots into files. */
  readonly resolveResourceInclude?: (path: string) => readonly string[] | undefined;
  /** Native UI TypeRegistry metadata for validating authored runtime properties. */
  readonly uiTypeRegistry?: UiTypeRegistryMetadata;
  /** Native Input key-name table for validating authored keyboard defaults. */
  readonly inputKeyRegistry?: InputKeyRegistryMetadata;
  readonly committedRevision?: number;
  /** Reuse unchanged Story compilation across repeated project builds. */
  readonly storyCompilationCache?: StoryProjectCompilationCache;
  /** Completed Story scenes across all locales; callbacks are synchronous. */
  readonly onStoryProgress?: (event: StoryProjectProgress & {
    readonly globalCurrent: number; readonly globalTotal: number;
  }) => void;
}

export interface BuildArtifact {
  readonly engineVersion: string;
  readonly project: ProjectDocument;
  readonly runtimeIr: RuntimeIrDocument;
  readonly sourceMap: RuntimeSourceMap;
  readonly runtimeIrPath: string;
  readonly sourceMapPath: string;
  /** Locale-specific programs. Operation/source identities are aligned across
   * locales so a running session can switch language without losing its
   * execution anchor. The default locale is also published through the
   * legacy-free Runtime/main aliases above. */
  readonly localeArtifacts: Readonly<Record<string, {
    readonly runtimeIr: RuntimeIrDocument;
    readonly sourceMap: RuntimeSourceMap;
    readonly runtimeIrPath: string;
    readonly sourceMapPath: string;
  }>>;
  /** Deterministically serialized package inputs keyed by virtual path. */
  readonly files: Readonly<Record<string, string>>;
  /** External package inputs which build tools must copy byte-for-byte. */
  readonly dependencies: readonly ProjectDependency[];
}

export interface ProjectCompileResult {
  readonly artifact?: BuildArtifact;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

function issue(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {severity: "error", code, message,
    ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details})};
}

interface LocatedResourceReference extends ProjectDependencyReference {
  readonly sourcePath?: string;
  readonly sourcePointer?: string;
}

function pointerSegment(value: string): string {
  return value.replaceAll("~", "~0").replaceAll("/", "~1");
}

// Input::ActionName registers these even when a game does not redeclare them.
// Other hosts may register more actions at runtime, so an unknown name is a
// warning rather than a build error.
const builtInInputActions = new Set([
  "navigate-up", "navigate-down", "navigate-left", "navigate-right",
  "accept", "cancel", "focus-next", "focus-previous", "advance", "menu",
  "backlog", "toggle-auto", "toggle-skip", "toggle-ui", "quick-save",
  "quick-load", "save.open", "rollback", "pause",
]);

function validateUiInputBindings(document: JsonValue, game: GameDocument,
  path: string, source?: string): AuthoringDiagnostic[] {
  if (document === null || typeof document !== "object" || Array.isArray(document)) return [];
  const nodes = document.nodes;
  if (!Array.isArray(nodes)) return [];
  const declared = new Set((game.inputActions ?? []).map((action) => action.id));
  const diagnostics: AuthoringDiagnostic[] = [];
  let sourceSpans: ReturnType<typeof jsonPointerSpans> | undefined;
  const at = (pointer: string) => {
    if (source !== undefined)
      sourceSpans ??= jsonPointerSpans(source, document, path);
    return sourceSpans?.get(pointer);
  };
  for (const [index, node] of nodes.entries()) {
    if (node === null || typeof node !== "object" || Array.isArray(node) ||
        node.runtimeProperties === null ||
        typeof node.runtimeProperties !== "object" ||
        Array.isArray(node.runtimeProperties)) continue;
    const action = node.runtimeProperties.bindingAction;
    const wheelThreshold = node.runtimeProperties.wheelThreshold;
    if (wheelThreshold !== undefined &&
        (typeof wheelThreshold !== "number" ||
         !Number.isFinite(wheelThreshold) ||
         wheelThreshold < 0 || wheelThreshold > 100)) {
      const span = at(`/nodes/${index}/runtimeProperties/wheelThreshold`);
      diagnostics.push({...issue("PXBUILD1073",
        "UI wheelThreshold must be a finite number from 0 to 100", path,
        `nodes[${index}].runtimeProperties.wheelThreshold`),
        ...(span === undefined ? {} : {span})});
    }
    if (action === undefined) continue;
    const location = `nodes[${index}].runtimeProperties.bindingAction`;
    if (typeof action !== "string" || action.length === 0) {
      const span = at(`/nodes/${index}/runtimeProperties/bindingAction`);
      diagnostics.push({severity: "error", code: "PXBUILD1071",
        message: "UI input binding must name a non-empty action", path,
        details: location, ...(span === undefined ? {} : {span})});
    } else if (!declared.has(action) && !builtInInputActions.has(action)) {
      const span = at(`/nodes/${index}/runtimeProperties/bindingAction`);
      diagnostics.push({severity: "warning", code: "PXBUILD1072",
        message: "UI input binding is not declared by the game or built-in Input",
        path, details: `${location}: ${action}`,
        hint: "Declare it in game.inputActions, or ensure this host registers the action before showing the UI.",
        ...(span === undefined ? {} : {span})});
    }
  }
  return diagnostics;
}

const builtInScreenEffects = new Set([
  "shake", "flash", "fade", "zoom", "pan", "blur", "vignette",
  "color-grade", "rule-dissolve",
]);
const gpuOnlyScreenEffects = new Set(["blur", "vignette", "color-grade"]);
// `[tone gray|sepia]` grades through the GPU compositor; `[tone none]` only clears it.
function needsGpuEffects(operation: {readonly arguments: Readonly<Record<string, string>>}, preset: string): boolean {
  return gpuOnlyScreenEffects.has(preset) ||
    (preset === "tone" && operation.arguments.mode !== "none");
}

function validateStoryEffects(
  runtimeIr: RuntimeIrDocument,
  sourceMap: RuntimeSourceMap,
  customEffects: ReadonlySet<string>,
  graphicsTier: "basic" | "gpu-effects",
): AuthoringDiagnostic[] {
  const diagnostics: AuthoringDiagnostic[] = [];
  const mappings = new Map(sourceMap.mappings.map((value) =>
    [value.operationId, value]));
  for (const operation of runtimeIr.operations) {
    if (operation.kind !== "effect" && operation.kind !== "camera") continue;
    const preset = operation.arguments.value ?? "";
    const mapping = mappings.get(operation.operationId);
    const path = mapping?.sourceUri;
    if (!builtInScreenEffects.has(preset) && preset !== "tone" && !customEffects.has(preset)) {
      diagnostics.push({
        ...issue("PXBUILD1024", "Story references an unknown screen effect preset", path, preset),
        documentId: runtimeIr.documentId,
        sourceId: operation.sourceId,
        hint: "Use a built-in Screen preset or declare a validated project effect.",
      });
    } else if ((needsGpuEffects(operation, preset) || customEffects.has(preset)) &&
               graphicsTier !== "gpu-effects") {
      diagnostics.push({
        ...issue("PXBUILD1025", "Story effect requires graphicsTier gpu-effects", path, preset),
        documentId: runtimeIr.documentId,
        sourceId: operation.sourceId,
        hint: "Set project.graphicsTier to gpu-effects or select a basic-tier effect.",
      });
    }
  }
  return diagnostics;
}

function parseValue<T>(contract: string, value: unknown, path: string) {
  return typeof value === "string"
    ? parseDocument<T>(contract, value, path)
    : validateDocument<T>(contract, value, path);
}

function documentAt<T>(
  contract: string,
  path: string,
  documents: Readonly<Record<string, unknown>>,
  diagnostics: AuthoringDiagnostic[],
): T | undefined {
  const source = documents[path];
  if (source === undefined) {
    diagnostics.push(issue("PXBUILD1001", `Required ${contract} document is missing`, path));
    return undefined;
  }
  const parsed = parseValue<T>(contract, source, path);
  diagnostics.push(...parsed.diagnostics);
  return parsed.valid ? parsed.value : undefined;
}

function serialized(value: unknown): string {
  if (typeof value === "string") {
    const parsed: unknown = JSON.parse(value);
    return canonicalJson(parsed);
  }
  return canonicalJson(value);
}

function localeRuntimePath(locale: string, extension: "pxir" | "pxmap"): string {
  // Locale tags have already passed the canonical locale contract. Keeping
  // each locale in its own directory makes the pair easy to inspect and
  // avoids filename ambiguity on case-insensitive filesystems.
  return `Runtime/Locales/${locale}/main.${extension}`;
}

function localizedArgument(kind: string, key: string): boolean {
  if (key === "text" && (kind === "dialogue" || kind === "narration" ||
      kind === "choiceOption")) return true;
  // The visible speaker is localized presentation; character is stable identity.
  if (key === "speaker" && kind === "dialogue") return true;
  // A [credits] roll's lines and images may differ per locale.
  if (key === "items" && kind === "timeline") return true;
  return key === "asset" && (kind === "background" || kind === "voice" ||
    kind === "bgm" || kind === "soundEffect" || kind === "ambience");
}

function comparableCustomValue(value: string | undefined,
  parameters: ReadonlySet<string> | undefined): string | undefined {
  if (value === undefined || parameters === undefined) return value;
  const decoded = JSON.parse(value) as unknown;
  if (decoded === null || typeof decoded !== "object" || Array.isArray(decoded)) return value;
  const normalized = {...decoded as Record<string, JsonValue>};
  for (const parameter of parameters) delete normalized[parameter];
  return canonicalJson(normalized, false).trim();
}

function sameLocalizedArguments(
  left: RuntimeIrDocument["operations"][number],
  right: RuntimeIrDocument["operations"][number],
  customResourceParameters: ReadonlyMap<string, ReadonlySet<string>>,
): boolean {
  // Runtime IR arguments are string-valued. Comparing their key sets and
  // values avoids sorting and serializing every operation in every locale.
  const leftArguments = left.arguments;
  const rightArguments = right.arguments;
  let comparableCount = 0;
  for (const key of Object.keys(leftArguments)) {
    if (localizedArgument(left.kind, key)) continue;
    if (!Object.hasOwn(rightArguments, key)) return false;
    const leftValue = key === "value" && left.kind === "customNode"
      ? comparableCustomValue(leftArguments.value,
          customResourceParameters.get(leftArguments.type ?? ""))
      : leftArguments[key];
    const rightValue = key === "value" && right.kind === "customNode"
      ? comparableCustomValue(rightArguments.value,
          customResourceParameters.get(rightArguments.type ?? ""))
      : rightArguments[key];
    if (typeof leftValue !== "string" || typeof rightValue !== "string" ||
        leftValue !== rightValue) return false;
    comparableCount++;
  }
  let rightCount = 0;
  for (const key of Object.keys(rightArguments))
    if (!localizedArgument(right.kind, key)) rightCount++;
  return comparableCount === rightCount;
}

function alignLocalizedProgram(
  locale: string,
  base: {readonly runtimeIr: RuntimeIrDocument; readonly sourceMap: RuntimeSourceMap},
  localized: {readonly runtimeIr: RuntimeIrDocument; readonly sourceMap: RuntimeSourceMap},
  customResourceParameters: ReadonlyMap<string, ReadonlySet<string>>,
  diagnostics: AuthoringDiagnostic[],
): {runtimeIr: RuntimeIrDocument; sourceMap: RuntimeSourceMap} | undefined {
  if (localized.runtimeIr.operations.length !== base.runtimeIr.operations.length ||
      localized.sourceMap.mappings.length !== localized.runtimeIr.operations.length) {
    diagnostics.push(issue("PXBUILD1030",
      `Story locale ${locale} does not have the same operation topology as the default locale`,
      undefined, locale));
    return undefined;
  }

  let invalid = false;
  const operations = localized.runtimeIr.operations.map((operation, index) => {
    const canonical = base.runtimeIr.operations[index]!;
    if (operation.kind !== canonical.kind ||
        !sameLocalizedArguments(operation, canonical, customResourceParameters)) {
      invalid = true;
      diagnostics.push(issue("PXBUILD1031",
        `Story locale ${locale} changes control flow or a non-localizable command at operation ${index}`,
        localized.sourceMap.mappings[index]?.sourceUri, `${canonical.kind} -> ${operation.kind}`));
    }
    return {...operation, operationId: canonical.operationId,
      sourceId: canonical.sourceId};
  });
  if (invalid) return undefined;

  const mappings = localized.sourceMap.mappings.map((mapping, index) => ({
    ...mapping,
    operationId: base.runtimeIr.operations[index]!.operationId,
    sourceId: base.runtimeIr.operations[index]!.sourceId,
  }));
  return {
    runtimeIr: {...localized.runtimeIr, operations},
    sourceMap: {...localized.sourceMap, mappings},
  };
}

type Version = readonly [number, number, number];
function parseVersion(value: string): Version | undefined {
  const match = /^[vV]?(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.-]+)?$/.exec(value.trim());
  if (match === null) return undefined;
  return [Number(match[1]), Number(match[2]), Number(match[3])];
}
function compareVersion(left: Version, right: Version): number {
  for (let index = 0; index < 3; index += 1) {
    if (left[index]! !== right[index]!) return left[index]! < right[index]! ? -1 : 1;
  }
  return 0;
}
function versionComparator(actual: Version, token: string): boolean | undefined {
  if (token === "*") return true;
  const match = /^(>=|<=|>|<|=|\^|~)?(.+)$/.exec(token);
  if (match === null) return undefined;
  const operation = match[1] ?? "=";
  const required = parseVersion(match[2]!);
  if (required === undefined) return undefined;
  const compared = compareVersion(actual, required);
  if (operation === ">=") return compared >= 0;
  if (operation === "<=") return compared <= 0;
  if (operation === ">") return compared > 0;
  if (operation === "<") return compared < 0;
  if (operation === "=") return compared === 0;
  const upper: Version = operation === "~"
    ? [required[0], required[1] + 1, 0]
    : required[0] > 0 ? [required[0] + 1, 0, 0]
      : required[1] > 0 ? [0, required[1] + 1, 0]
        : [0, 0, required[2] + 1];
  return compared >= 0 && compareVersion(actual, upper) < 0;
}
function satisfiesVersionRange(range: string): boolean | undefined {
  const clauses = range.split("||").map((value) => value.trim());
  if (clauses.some((value) => value.length === 0)) return undefined;
  let valid = true;
  for (const clause of clauses) {
    let matches = true;
    for (const token of clause.split(/\s+/)) {
      const compared = versionComparator([0, 2, 0], token);
      if (compared === undefined) valid = false;
      else matches = matches && compared;
    }
    if (matches && valid) return true;
  }
  return valid ? false : undefined;
}

export function compileProject(input: CompileProjectInput): ProjectCompileResult {
  const diagnostics: AuthoringDiagnostic[] = [];
  const normalized = normalizeProject(input);
  diagnostics.push(...normalized.diagnostics);
  if (!normalized.valid || normalized.value === undefined)
    return {diagnostics, valid: false};
  const projectPath = normalized.value.projectPath;
  let project = normalized.value.project;
  const hostActionContract = project.settings?.["authoring.actionContract"] as
    HostActionContract | undefined;
  const hostActions = hostActionContract?.actions ?? [];
  if (hostActionContract !== undefined)
    diagnostics.push(...validateHostActionContract(hostActionContract, projectPath));
  const documents = normalized.value.documents;
  const sourceFiles = normalized.value.sourceFiles;
  const locales = normalized.value.locales;
  const locate = createJsonSourceLocator((path) => {
    if (path === projectPath && input.projectSource !== undefined)
      return input.projectSource;
    const text = input.documentSources?.[path];
    return text === undefined ? undefined : {path, text};
  });
  const locatedIssue = (code: string, message: string, path: string,
    pointer: string, expected: unknown, details?: string,
    sourcePath = path): AuthoringDiagnostic => {
    const span = locate(sourcePath, pointer, expected);
    return {...issue(code, message, path, details),
      ...(span === undefined ? {} : {span})};
  };
  const includedResources: ProjectDependencyReference[] = [];
  for (const [index, root] of normalized.value.resourceIncludes.entries()) {
    let paths: readonly string[] | undefined;
    try { paths = input.resolveResourceInclude?.(root); }
    catch (error) {
      diagnostics.push(locatedIssue("PXBUILD1052", "Could not expand resourceIncludes root", projectPath,
        `/resourceIncludes/${index}`, root, `${root}: ${String(error)}`));
      continue;
    }
    if (!Array.isArray(paths) || paths.length === 0) {
      diagnostics.push(locatedIssue("PXBUILD1052",
        "resourceIncludes root has no files or no resource resolver", projectPath,
        `/resourceIncludes/${index}`, root, root));
      continue;
    }
    for (const path of paths) {
      if (typeof path !== "string" || !isSafeResourcePath(path) ||
          path.includes(":") || path.includes("\0") ||
          path.split("/").some((part) => part.length === 0 || part === ".") ||
          (path !== root && !path.startsWith(`${root}/`))) {
        diagnostics.push(locatedIssue("PXBUILD1053",
          "resourceIncludes resolver returned a path outside its root or an unsafe path",
          projectPath, `/resourceIncludes/${index}`, root, `${root}: ${String(path)}`));
      } else if (/\.(?:[jt]sx?|mjs|cjs|pxstory|pxui|pxuicomponent|pxproject|pxgame|pxindex)$/iu.test(path) ||
          path.endsWith(".pxstory.pxids.json")) {
        diagnostics.push(locatedIssue("PXBUILD1055",
          "resourceIncludes is for runtime media, not authored source or canonical documents",
          projectPath, `/resourceIncludes/${index}`, root, path));
      } else includedResources.push({value: path, origin: `resourceIncludes:${root}`});
    }
  }
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return {diagnostics, valid: false};

  const storyIndex = documentAt<StoryIndexDocument>("storyIndex", project.storyIndex, documents, diagnostics);
  let game = documentAt<GameDocument>("game", project.gameCatalog, documents, diagnostics);
  // Story `[var ...]` declarations join the published game catalog.
  let gameDeclaredInStory = false;
  if (game !== undefined && storyIndex !== undefined) {
    const storyPaths = new Set(storyIndex.scenes.flatMap((scene) => Object.values(scene.sources)));
    const declared = collectStoryVariableDeclarations([...storyPaths].flatMap((path) =>
      sourceFiles[path] === undefined ? [] : [{path, source: sourceFiles[path]!}]));
    const merged = mergeStoryVariables(game, declared.declarations, project.gameCatalog);
    diagnostics.push(...declared.diagnostics, ...merged.diagnostics);
    gameDeclaredInStory = merged.game !== game;
    game = merged.game;
  }
  if (game !== undefined) {
    if (input.inputKeyRegistry !== undefined)
      diagnostics.push(...validateInputKeyUsage(game, project.gameCatalog,
        input.inputKeyRegistry, input.documentSources?.[project.gameCatalog]));
    const routes = new Set(["title", "hud", "backlog", "save", "load",
      "gallery", "settings", "video", ...Object.keys(project.uiEntryPoints)]);
    for (const [index, action] of (game.inputActions ?? []).entries()) if (action.route !== undefined &&
        !routes.has(action.route))
      diagnostics.push(locatedIssue("PXBUILD1061", "Input action route is not registered by this project",
        project.gameCatalog, `/inputActions/${index}/route`, action.route,
        `${action.id}: ${action.route}`));
  }
  const characters: CharacterDocument[] = [];
  const characterDocuments: Record<string, unknown> = {};
  for (const descriptor of project.characters ?? []) {
    const authored = documents[descriptor.source];
    if (authored !== undefined) characterDocuments[descriptor.source] = authored;
    const character = documentAt<CharacterDocument>("character", descriptor.source, documents, diagnostics);
    if (character !== undefined) characters.push(character);
  }
  const characterGraph = validateProjectCharacterResources(project, characterDocuments, {
    projectPath,
    ...(input.projectSource === undefined ? {} : {projectSource: input.projectSource}),
    ...(input.documentSources === undefined ? {} : {characterSources: input.documentSources}),
    ...(input.resourceExists === undefined ? {} : {assetExists: input.resourceExists}),
  });
  diagnostics.push(...characterGraph.diagnostics);
  const extensions: ExtensionManifest[] = [];
  const extensionDocuments = new Map<string, ExtensionManifest>();
  for (const path of project.extensions) {
    const extension = documentAt<ExtensionManifest>("extension", path, documents, diagnostics);
    if (extension !== undefined) {
      extensionDocuments.set(path, extension);
      const compatible = satisfiesVersionRange(extension.requiredEngineVersion);
      if (parseVersion(extension.version) === undefined)
        diagnostics.push(locatedIssue("PXBUILD1011", "Extension version is not SemVer",
          path, "/version", extension.version, extension.version));
      else if (compatible === undefined)
        diagnostics.push(locatedIssue("PXBUILD1012", "Extension requiredEngineVersion is not a supported SemVer range",
          path, "/requiredEngineVersion", extension.requiredEngineVersion,
          extension.requiredEngineVersion));
      else if (!compatible)
        diagnostics.push(locatedIssue("PXBUILD1013", `Extension is incompatible with engine ${ENGINE_VERSION}`,
          path, "/requiredEngineVersion", extension.requiredEngineVersion,
          extension.requiredEngineVersion));
      else extensions.push(extension);
    }
  }
  const extensionActionIds = new Set<string>();
  for (const extension of extensions) for (const action of extension.actions) {
    if (extensionActionIds.has(action.id))
      diagnostics.push(issue("PXBUILD1066", "Extension Action id is declared more than once",
        projectPath, action.id));
    extensionActionIds.add(action.id);
  }
  for (const [index, action] of hostActions.entries()) if (extensionActionIds.has(action.id))
    diagnostics.push(locatedIssue("PXBUILD1075",
      "Host Action id collides with an extension Action",
      projectPath, `/settings/authoring.actionContract/actions/${index}/id`,
      action.id, action.id));
  const saveMigrationDocuments = new Map<string, SaveMigrationDocument>();
  const migrationSources = new Map<string, SaveMigrationDescriptor>();
  const versionKey = (version: SaveMigrationVersion) =>
    `${version.contentVersion}\n${version.saveVersion}`;
  for (const [index, descriptor] of (project.saveMigrations ?? []).entries()) {
    const migration = documentAt<SaveMigrationDocument>(
      "saveMigration", descriptor.asset, documents, diagnostics,
    );
    if (migration === undefined) continue;
    if (Buffer.byteLength(canonicalJson(migration), "utf8") >
        contractDocumentByteLimit("saveMigration")) {
      diagnostics.push(issue("PXBUILD1074",
        "Save migration exceeds the native 4 MiB document limit",
        descriptor.asset));
      continue;
    }
    if (migration.id !== descriptor.id ||
        versionKey(migration.from) !== versionKey(descriptor.from) ||
        versionKey(migration.to) !== versionKey(descriptor.to)) {
      const field = migration.id !== descriptor.id ? "id" :
        versionKey(migration.from) !== versionKey(descriptor.from) ? "from" : "to";
      diagnostics.push(locatedIssue("PXBUILD1007",
        "Save migration document does not match its project descriptor",
        descriptor.asset, `/${field}`, migration[field], descriptor.id));
      continue;
    }
    const source = versionKey(descriptor.from);
    if (migrationSources.has(source)) {
      diagnostics.push(locatedIssue("PXBUILD1008", "Save migration chain is ambiguous",
        projectPath, `/saveMigrations/${index}/from`, descriptor.from, source));
      continue;
    }
    migrationSources.set(source, descriptor);
    saveMigrationDocuments.set(descriptor.asset, migration);
  }
  const migrationTarget = versionKey({contentVersion: project.contentVersion, saveVersion: project.saveVersion});
  for (const [index, descriptor] of (project.saveMigrations ?? []).entries()) {
    const visited = new Set<string>();
    let current = versionKey(descriptor.from);
    for (let step = 0; current !== migrationTarget; step += 1) {
      if (step >= 64 || visited.has(current)) {
        diagnostics.push(locatedIssue("PXBUILD1009",
          "Save migration chain contains a cycle or exceeds 64 steps",
          descriptor.asset, `/saveMigrations/${index}/from`, descriptor.from,
          undefined, projectPath));
        break;
      }
      visited.add(current);
      const next = migrationSources.get(current);
      if (next === undefined) {
        diagnostics.push(locatedIssue("PXBUILD1010",
          "Save migration chain does not reach the current content/save version",
          descriptor.asset, `/saveMigrations/${index}/from`, descriptor.from,
          current, projectPath));
        break;
      }
      current = versionKey(next.to);
    }
  }
  const uiDocuments = new Map<string, JsonValue>();
  for (const [id, path] of Object.entries(project.uiEntryPoints)) {
    const ui = documentAt<JsonValue>("ui", path, documents, diagnostics);
    if (ui === undefined) diagnostics.push(locatedIssue("PXBUILD1002",
      `UI entry point ${id} is unavailable`, path,
      `/uiEntryPoints/${pointerSegment(id)}`, path, id, projectPath));
    else {
      if (input.uiTypeRegistry !== undefined)
        diagnostics.push(...validateUiPropertyUsage(ui, input.uiTypeRegistry,
          path, input.documentSources?.[path]));
      diagnostics.push(...validateUiActionUsage(ui, extensions, path,
        input.documentSources?.[path], hostActions));
      if (game !== undefined)
        diagnostics.push(...validateUiInputBindings(ui, game, path,
          input.documentSources?.[path]));
      uiDocuments.set(path, ui);
    }
  }
  const effectDocuments = new Map<string, CustomEffectDocument>();
  const effectIds = new Set<string>();
  for (const [index, descriptor] of (project.effects ?? []).entries()) {
    const effect = documentAt<CustomEffectDocument>(
      "effect", descriptor.source, documents, diagnostics,
    );
    if (effect === undefined) continue;
    if (project.graphicsTier !== "gpu-effects") {
      diagnostics.push(locatedIssue("PXBUILD1020", "Custom effects require graphicsTier gpu-effects",
        descriptor.source, project.graphicsTier === undefined
          ? `/effects/${index}/source` : "/graphicsTier",
        project.graphicsTier ?? descriptor.source, undefined, projectPath));
      continue;
    }
    if (effect.id !== descriptor.id || !effectIds.add(effect.id)) {
      diagnostics.push(locatedIssue("PXBUILD1021",
        "Custom effect identity is duplicated or disagrees with its project descriptor",
        descriptor.source, "/id", effect.id, descriptor.id));
      continue;
    }
    if (!effect.shader.endsWith(".hlsl") || sourceFiles[effect.shader] === undefined) {
      diagnostics.push(locatedIssue("PXBUILD1022",
        "Custom effect HLSL source is missing or has the wrong extension",
        effect.shader, "/shader", effect.shader, undefined, descriptor.source));
      continue;
    }
    if (effect.schemaRevision === 2 && effect.targetLayer !== "stage") {
      diagnostics.push(locatedIssue("PXBUILD1024",
        "Effect schema revision 2 only supports the Stage post-processing target",
        descriptor.source, "/targetLayer", effect.targetLayer, effect.id));
      continue;
    }
    const uniformNames = new Set<string>();
    const uniformSlots = new Set<number>();
    for (const [uniformIndex, uniform] of effect.uniforms.entries()) {
      const minimum = uniform.minimum ?? 0;
      const maximum = uniform.maximum ?? 1;
      const finiteNumber = (value: unknown): value is number =>
        typeof value === "number" && Number.isFinite(value);
      const components = uniform.type === "number"
        ? [uniform.default]
        : Array.isArray(uniform.default) ? uniform.default : [];
      const expectedComponents = uniform.type === "number" ? 1
        : uniform.type === "vec2" ? 2 : 4;
      const validDefault = components.length === expectedComponents &&
        components.every((component) => finiteNumber(component)) &&
        (uniform.type !== "color" ||
          components.every((component) => (component as number) >= 0 &&
            (component as number) <= 1));
      const validRange = finiteNumber(minimum) && finiteNumber(maximum) &&
        Math.abs(minimum) <= 3.402823466e38 &&
        Math.abs(maximum) <= 3.402823466e38 && minimum <= maximum;
      const defaultInRange = validDefault && validRange &&
        components.every((component) => (component as number) >= minimum &&
          (component as number) <= maximum);
      if (!uniformNames.add(uniform.name) || !uniformSlots.add(uniform.slot) ||
          !validRange || !defaultInRange) {
        diagnostics.push(locatedIssue("PXBUILD1023",
          "Custom effect uniforms must have unique names/slots and valid ranges",
          descriptor.source, `/uniforms/${uniformIndex}`, uniform, uniform.name));
      }
    }
    effectDocuments.set(descriptor.source, effect);
  }
  const uiComponentDocuments = new Map<string, JsonValue>();
  const uiComponentIds = new Set<string>();
  for (const [index, descriptor] of (project.uiComponents ?? []).entries()) {
    const component = documentAt<JsonValue>("uiComponent", descriptor.source,
      documents, diagnostics);
    if (component === undefined) {
      diagnostics.push(locatedIssue("PXBUILD1014", "UI component document is missing or invalid",
        descriptor.source, `/uiComponents/${index}/source`, descriptor.source,
        descriptor.id, projectPath));
      continue;
    }
    if ((component as Record<string, JsonValue>).id !== descriptor.id) {
      diagnostics.push(locatedIssue("PXBUILD1017",
        "UI component identity disagrees with its project descriptor",
        descriptor.source, "/id", (component as Record<string, JsonValue>).id,
        descriptor.id));
      continue;
    }
    if (!uiComponentIds.add(descriptor.id)) {
      diagnostics.push(locatedIssue("PXBUILD1018", "UI component id is duplicated",
        projectPath, `/uiComponents/${index}/id`, descriptor.id, descriptor.id));
      continue;
    }
    if (input.uiTypeRegistry !== undefined)
      diagnostics.push(...validateUiPropertyUsage(component, input.uiTypeRegistry,
        descriptor.source, input.documentSources?.[descriptor.source]));
    diagnostics.push(...validateUiActionUsage(component, extensions,
      descriptor.source, input.documentSources?.[descriptor.source], hostActions));
    if (game !== undefined)
      diagnostics.push(...validateUiInputBindings(component, game,
        descriptor.source, input.documentSources?.[descriptor.source]));
    uiComponentDocuments.set(descriptor.source, component);
  }
  // The Player publishes hover.<name> only for names declared as UI data;
  // declare every one a UI binds so tooltips need no manifest entry.
  if (project.settings?.["authoring.uiContract"] === undefined) {
    const hovered = new Set<string>();
    for (const document of [...uiDocuments.values(), ...uiComponentDocuments.values()]) {
      const nodes = (document as {nodes?: unknown}).nodes;
      for (const node of Array.isArray(nodes) ? nodes : []) {
        const bindings = (node as {bindings?: Record<string, {path?: unknown}>}).bindings ?? {};
        for (const binding of Object.values(bindings))
          if (typeof binding?.path === "string" && /^hover\.[A-Za-z_][A-Za-z0-9_]*$/u.test(binding.path))
            hovered.add(binding.path);
      }
    }
    const declared = (project.settings?.["player.uiData"] ?? {}) as Record<string, JsonValue>;
    const missing = [...hovered].filter((path) => !Object.hasOwn(declared, path)).sort();
    if (missing.length > 0)
      project = {...project, settings: {...project.settings, "player.uiData": {
        ...declared, ...Object.fromEntries(missing.map((path) => [path, false]))}}};
  }
  diagnostics.push(...validatePlayerUiBindings(project,
    new Map([...uiDocuments, ...uiComponentDocuments]), input.uiTypeRegistry,
    input.documentSources, input.projectSource,
    game?.variables.filter((variable) => variable.scope === "profile")
      .map((variable) => variable.name),
    [...new Set(Object.values(locales).flatMap((locale) => {
      const strings = (locale as {strings?: unknown} | null)?.strings;
      return strings !== null && typeof strings === "object" ? Object.keys(strings) : [];
    }))]));
  if (storyIndex !== undefined && storyIndex.entryScene !== project.entry.story) {
    diagnostics.push(locatedIssue("PXBUILD1005", "Project entry.story must match storyIndex.entryScene",
      project.storyIndex, "/entry/story", project.entry.story, project.entry.story,
      projectPath));
  }
  const localeDocuments = new Map<string, JsonValue>();
  for (const [index, locale] of project.supportedLocales.entries()) {
    const source = locales[locale];
    const path = `Content/Localization/${locale}.json`;
    if (source === undefined) {
      diagnostics.push(locatedIssue("PXBUILD1003", `Locale document ${locale} is missing`,
        path, `/supportedLocales/${index}`, locale, locale, projectPath));
      continue;
    }
    const parsed = parseValue<JsonValue>("locale", source, path);
    diagnostics.push(...parsed.diagnostics);
    if (parsed.valid && parsed.value !== undefined) {
      const localeValue = parsed.value as {locale?: JsonValue};
      if (localeValue.locale !== locale) diagnostics.push(locatedIssue("PXBUILD1006",
        "Locale document identity does not match its project key", path,
        "/locale", localeValue.locale, locale));
      else localeDocuments.set(locale, parsed.value);
    }
  }
  if (!localeDocuments.has(project.defaultLocale)) {
    diagnostics.push(locatedIssue("PXBUILD1004", "The default locale document is unavailable",
      projectPath, "/defaultLocale", project.defaultLocale, project.defaultLocale));
  }
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error") || storyIndex === undefined || game === undefined) {
    return {diagnostics, valid: false};
  }

  const resourcePaths: Record<string, string> = {};
  const resourceAssets: ProjectAssetDescriptor[] = [];
  for (const value of project.assets ?? []) {
    if (value !== null && typeof value === "object" && !Array.isArray(value)) {
      const asset = value as Record<string, JsonValue>;
      if (typeof asset.id === "string" && typeof asset.source === "string" &&
          typeof asset.kind === "string") {
        resourcePaths[asset.id] = `asset:${asset.id.toLowerCase()}`;
        resourceAssets.push({
          id: asset.id,
          source: asset.source,
          kind: asset.kind,
          ...(typeof asset.name === "string" ? {name: asset.name} : {}),
        });
      }
    }
  }
  // Short Story names (`[bg forest]`) resolve only to files the build
  // already packages: resourceIncludes and cataloged asset sources.
  const storyAssets = normalized.value.storyAssets === undefined ? undefined
    : createStoryAssetIndex(normalized.value.storyAssets, [
      ...includedResources.map((reference) => reference.value),
      ...resourceAssets.map((asset) => asset.source),
    ]);
  // Shared macro files are parsed once for every scene and locale.
  let storyMacros: StoryMacroLibrary | undefined;
  if (normalized.value.storyMacros !== undefined) {
    const files: {path: string; source: string}[] = [];
    for (const [index, path] of normalized.value.storyMacros.entries()) {
      const source = sourceFiles[path];
      if (source === undefined)
        diagnostics.push(locatedIssue("PXBUILD1121", "storyMacros file is missing",
          projectPath, `/storyMacros/${index}`, path, path));
      else files.push({path, source});
    }
    const built = createStoryMacroLibrary(files, extensions);
    diagnostics.push(...built.diagnostics);
    if (diagnostics.some((diagnostic) => diagnostic.severity === "error"))
      return {diagnostics, valid: false};
    storyMacros = built.library;
  }
  // [face] reads the layered portraits of character expressions. Load them
  // only when some Story (or shared macro file) uses the command.
  let portraits: StoryPortraitIndex | undefined;
  if (Object.entries(sourceFiles).some(([path, source]) => path.endsWith(".pxstory") &&
      /^[ \t]*\[\s*face\s/mu.test(source))) {
    const documentsByPath: Record<string, LayeredPortraitDocument> = {};
    for (const character of characters) for (const expression of character.expressions) {
      const source = canonicalResourceReference(`asset:${expression.assetId}`, resourceAssets)?.source;
      if (source === undefined || !source.endsWith(".pxportrait") || source in documentsByPath) continue;
      let text: string | undefined;
      try { text = sourceFiles[source] ?? input.resourceText?.(source); } catch { text = undefined; }
      // A missing or invalid descriptor is reported with the package graph.
      const parsed = text === undefined ? undefined
        : parseDocument<LayeredPortraitDocument>("portrait", text, source);
      if (parsed?.valid === true && parsed.value !== undefined) documentsByPath[source] = parsed.value;
    }
    portraits = createStoryPortraitIndex(documentsByPath);
  }
  const storyProgressCounts = {parse: 0, compile: 0};
  const globalStoryTotal = storyIndex.scenes.length *
    (1 + project.supportedLocales.filter((locale) => locale !== project.defaultLocale).length);
  const compileLocale = (locale: string) => {
    const document = localeDocuments.get(locale);
    const localizedResources: Record<string, string> = {};
    if (document !== null && typeof document === "object" &&
        !Array.isArray(document)) {
      const resources = (document as Record<string, JsonValue>).resources;
      if (resources !== null && typeof resources === "object" &&
          !Array.isArray(resources)) {
        for (const [id, value] of Object.entries(resources))
          if (typeof value === "string") localizedResources[id] = value;
      }
    }
    const storyContext = {
      storyIndex,
      locale,
      sourceFiles,
      storyIndexPath: project.storyIndex,
      ...(input.storyIndexSource === undefined ? {} :
        {storyIndexSource: input.storyIndexSource}),
      documentId: project.id,
      ...(input.committedRevision === undefined ? {} : {committedRevision: input.committedRevision}),
      characters,
      game,
      extensions,
      resources: {...resourcePaths, ...localizedResources},
      resourceAssets,
      ...(storyAssets === undefined ? {} : {storyAssets}),
      ...(storyMacros === undefined ? {} : {storyMacros}),
      ...(portraits === undefined ? {} : {portraits}),
      ...(input.onStoryProgress === undefined ? {} : {onProgress: (event: StoryProjectProgress) => {
        storyProgressCounts[event.phase] += 1;
        input.onStoryProgress?.({...event,
          globalCurrent: storyProgressCounts[event.phase], globalTotal: globalStoryTotal});
      }}),
    };
    return input.storyCompilationCache === undefined
      ? compileStoryProject(storyContext)
      : input.storyCompilationCache.compile(storyContext);
  };
  const compiled = compileLocale(project.defaultLocale);
  diagnostics.push(...compiled.diagnostics);
  if (!compiled.valid || compiled.runtimeIr === undefined || compiled.sourceMap === undefined) {
    return {diagnostics, valid: false};
  }
  diagnostics.push(...validateStoryEffects(
    compiled.runtimeIr, compiled.sourceMap, effectIds,
    project.graphicsTier ?? "basic",
  ));
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error")) {
    return {diagnostics, valid: false};
  }

  const localeArtifacts: Record<string, {
    runtimeIr: RuntimeIrDocument; sourceMap: RuntimeSourceMap;
    runtimeIrPath: string; sourceMapPath: string;
  }> = {};
  const customResourceParameters = new Map<string, ReadonlySet<string>>(
    extensions.map((extension) => extension.commands.map((command) => [
      command.id,
      new Set(command.parameters.filter((parameter) =>
        parameter.type === "resource").map((parameter) => parameter.name)),
    ] as const)).flat(),
  );
  const storyResourcePaths = new Set(compiled.resourceDependencies ?? []);
  const storyResourceLocations = new Map<string, SourceSpan>(
    (compiled.resourceLocations ?? []).map((item) => [item.source, item.span]));
  for (const locale of project.supportedLocales) {
    const localized = locale === project.defaultLocale ? compiled : compileLocale(locale);
    if (localized !== compiled) diagnostics.push(...localized.diagnostics);
    for (const dependency of localized.resourceDependencies ?? [])
      storyResourcePaths.add(dependency);
    for (const location of localized.resourceLocations ?? [])
      if (!storyResourceLocations.has(location.source))
        storyResourceLocations.set(location.source, location.span);
    if (!localized.valid || localized.runtimeIr === undefined ||
        localized.sourceMap === undefined) continue;
    const aligned = locale === project.defaultLocale
      ? {runtimeIr: localized.runtimeIr, sourceMap: localized.sourceMap}
      : alignLocalizedProgram(locale,
        {runtimeIr: compiled.runtimeIr, sourceMap: compiled.sourceMap},
        {runtimeIr: localized.runtimeIr, sourceMap: localized.sourceMap},
        customResourceParameters,
        diagnostics);
    if (aligned === undefined) continue;
    localeArtifacts[locale] = {
      ...aligned,
      runtimeIrPath: localeRuntimePath(locale, "pxir"),
      sourceMapPath: localeRuntimePath(locale, "pxmap"),
    };
  }
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error") ||
      Object.keys(localeArtifacts).length !== project.supportedLocales.length) {
    return {diagnostics, valid: false};
  }

  const dependencyReferences: LocatedResourceReference[] = [
    ...[...storyResourcePaths].map((value) => ({value, origin: "Story"})),
    ...includedResources,
  ];
  for (const [index, value] of (project.assets ?? []).entries()) {
    if (value === null || typeof value !== "object" || Array.isArray(value) ||
        typeof value.source !== "string") continue;
    dependencyReferences.push({value: value.source,
      origin: `project.assets[${index}].source`, sourcePath: projectPath,
      sourcePointer: `/assets/${index}/source`});
  }
  const resourceSourceDocuments = new Map<string,
    {document: unknown; text: string | undefined}>();
  for (const [path, document] of [...uiDocuments, ...uiComponentDocuments,
    ...extensionDocuments])
    resourceSourceDocuments.set(path, {document, text: input.documentSources?.[path]});
  for (const [locale, document] of localeDocuments) {
    const path = `Content/Localization/${locale}.json`;
    resourceSourceDocuments.set(path,
      {document, text: input.documentSources?.[path]});
  }
  if (input.project !== undefined) {
    try {
      const document = typeof input.project === "string"
        ? JSON.parse(input.project) as unknown : input.project;
      resourceSourceDocuments.set(projectPath,
        {document, text: input.documentSources?.[projectPath]});
    } catch { /* Project parsing diagnostics are reported by normalization. */ }
  }
  const addResourceRef = (value: unknown, origin: string,
    sourcePath?: string, sourcePointer?: string): void => {
    const location = sourcePath === undefined || sourcePointer === undefined
      ? {} : {sourcePath, sourcePointer};
    if (typeof value === "string") {
      dependencyReferences.push({value, origin, ...location});
      return;
    }
    if (value === null || typeof value !== "object" || Array.isArray(value)) return;
    const reference = value as Record<string, unknown>;
    if (typeof reference.path === "string")
      dependencyReferences.push({value: reference.path, origin,
        ...(sourcePointer === undefined ? {} : {sourcePointer: `${sourcePointer}/path`}),
        ...(sourcePath === undefined ? {} : {sourcePath})});
    else if (typeof reference.id === "string")
      dependencyReferences.push({value: reference.id, origin,
        ...(sourcePointer === undefined ? {} : {sourcePointer: `${sourcePointer}/id`}),
        ...(sourcePath === undefined ? {} : {sourcePath})});
  };
  for (const [manifestPath, extension] of extensionDocuments) {
    for (const [kind, declarations] of [
      ["commands", extension.commands], ["actions", extension.actions],
    ] as const) for (const [declarationIndex, declaration] of declarations.entries()) {
      for (const [parameterIndex, parameter] of declaration.parameters.entries()) {
        if (parameter.type === "resource" && parameter.default != null &&
            !(parameter.default === "" ||
              (typeof parameter.default === "object" && !Array.isArray(parameter.default) &&
                parameter.default.path === "")))
          addResourceRef(parameter.default,
            `${manifestPath}:${declaration.id}.${parameter.name}.default`,
            manifestPath,
            `/${kind}/${declarationIndex}/parameters/${parameterIndex}/default`);
      }
    }
  }
  const addUiResourceRefs = (document: JsonValue, origin: string): void => {
    if (document === null || typeof document !== "object" || Array.isArray(document)) return;
    const nodes = (document as Record<string, JsonValue>).nodes;
    if (!Array.isArray(nodes)) return;
    const visitProperties = (value: JsonValue, path: string,
      pointer: string): void => {
      if (value === null || typeof value !== "object") return;
      if (Array.isArray(value)) {
        for (const [index, item] of value.entries())
          visitProperties(item, `${path}[${index}]`, `${pointer}/${index}`);
        return;
      }
      for (const [key, item] of Object.entries(value)) {
        const itemPath = `${path}.${key}`;
        const itemPointer = `${pointer}/${pointerSegment(key)}`;
        if (typeof item === "string" && (key === "path" || key.endsWith("Path")))
          addResourceRef(item, `${origin}:${itemPath}`, origin, itemPointer);
        else visitProperties(item, itemPath, itemPointer);
      }
    };
    const visitActionArguments = (value: JsonValue, path: string,
      pointer: string): void => {
      if (typeof value === "string") {
        // Project actions frequently pass media paths through a generic
        // `argument` field (music galleries, replay thumbnails, custom
        // viewers). Those files are just as package-critical as typed
        // runtime *Path properties. Restrict discovery to path-shaped values
        // so route ids, labels, and ordinary action strings stay untouched.
        if (value.includes("/") && !value.includes("://") &&
            /\.[A-Za-z0-9_-]{1,16}$/u.test(value) &&
            isSafeResourcePath(value))
          addResourceRef(value, `${origin}:${path}`, origin, pointer);
        return;
      }
      if (value === null || typeof value !== "object") return;
      if (Array.isArray(value)) {
        for (const [index, item] of value.entries())
          visitActionArguments(item, `${path}[${index}]`,
            `${pointer}/${index}`);
        return;
      }
      if (value.type === "resource" && typeof value.value === "string") {
        if (value.value !== "") addResourceRef(value.value,
          `${origin}:${path}.value`, origin, `${pointer}/value`);
        return;
      }
      for (const [key, item] of Object.entries(value))
        visitActionArguments(item, `${path}.${key}`,
          `${pointer}/${pointerSegment(key)}`);
    };
    for (const [index, node] of nodes.entries()) {
      if (node === null || typeof node !== "object" || Array.isArray(node)) continue;
      const uiNode = node as Record<string, JsonValue>;
      const properties = uiNode.runtimeProperties;
      if (properties !== undefined)
        visitProperties(properties, `nodes[${index}].runtimeProperties`,
          `/nodes/${index}/runtimeProperties`);
      const onClick = uiNode.onClick;
      if (onClick !== null && typeof onClick === "object" &&
          !Array.isArray(onClick)) {
        const actionArguments = (onClick as Record<string, JsonValue>).arguments;
        if (actionArguments !== undefined)
          visitActionArguments(actionArguments,
            `nodes[${index}].onClick.arguments`,
            `/nodes/${index}/onClick/arguments`);
      }
      const instance = uiNode.componentInstance;
      if (instance !== null && typeof instance === "object" &&
          !Array.isArray(instance)) {
        const signals = (instance as Record<string, JsonValue>).publicSignals;
        if (signals !== null && typeof signals === "object" && !Array.isArray(signals)) {
          for (const [signalId, value] of Object.entries(signals)) {
            if (value === null || typeof value !== "object" || Array.isArray(value)) continue;
            const actionArguments = (value as Record<string, JsonValue>).arguments;
            if (actionArguments !== undefined)
              visitActionArguments(actionArguments,
                `nodes[${index}].componentInstance.publicSignals.${signalId}.arguments`,
                `/nodes/${index}/componentInstance/publicSignals/${pointerSegment(signalId)}/arguments`);
          }
        }
      }
    }
    const graph = (document as Record<string, JsonValue>).behaviorGraph;
    if (graph !== null && typeof graph === "object" && !Array.isArray(graph)) {
      const graphNodes = (graph as Record<string, JsonValue>).nodes;
      if (Array.isArray(graphNodes)) for (const [index, value] of graphNodes.entries()) {
        if (value === null || typeof value !== "object" || Array.isArray(value)) continue;
        const node = value as Record<string, JsonValue>;
        if (node.kind === "action" && node.arguments !== undefined)
          visitActionArguments(node.arguments,
            `behaviorGraph.nodes[${index}].arguments`,
            `/behaviorGraph/nodes/${index}/arguments`);
      }
    }
  };
  for (const [path, document] of uiDocuments) addUiResourceRefs(document, path);
  for (const [path, document] of uiComponentDocuments) addUiResourceRefs(document, path);
  const settings = project.settings ?? {};
  const presentationTransitions = settings["player.presentationTransitions"];
  if (presentationTransitions !== null && typeof presentationTransitions === "object" &&
      !Array.isArray(presentationTransitions)) {
    const overrides = (presentationTransitions as Record<string, JsonValue>).routeOverrides;
    if (Array.isArray(overrides)) {
      const availableRoutes = new Set(["title", "hud", "backlog", "save", "load",
        "gallery", "settings", "video", ...Object.keys(project.uiEntryPoints)]);
      const seenPairs = new Set<string>();
      for (const [index, value] of overrides.entries()) {
        if (value === null || typeof value !== "object" || Array.isArray(value)) continue;
        const {from, to} = value as Record<string, JsonValue>;
        if (typeof from !== "string" || typeof to !== "string") continue;
        if (!availableRoutes.has(from) || !availableRoutes.has(to) || from === to) {
          const field = !availableRoutes.has(from) ? "from" : "to";
          diagnostics.push(locatedIssue("PXBUILD1057",
            "Route transition must name two distinct registered Player routes",
            projectPath,
            `/settings/player.presentationTransitions/routeOverrides/${index}/${field}`,
            field === "from" ? from : to,
            `settings.player.presentationTransitions.routeOverrides[${index}]: ${from} -> ${to}`));
        }
        const pair = `${from}\n${to}`;
        if (seenPairs.has(pair))
          diagnostics.push(locatedIssue("PXBUILD1058", "Duplicate Player route transition pair",
            projectPath, `/settings/player.presentationTransitions/routeOverrides/${index}`,
            value, `settings.player.presentationTransitions.routeOverrides[${index}]: ${from} -> ${to}`));
        seenPairs.add(pair);
      }
    }
  }
  const playerOptions = settings["player.options"];
  const optionNames = playerOptions !== null && typeof playerOptions === "object" &&
    !Array.isArray(playerOptions) ? Object.keys(playerOptions) : [];
  const confirmations = settings["player.confirmations"];
  if (confirmations !== null && typeof confirmations === "object" && !Array.isArray(confirmations)) {
    for (const [operation, confirm] of Object.entries(confirmations)) {
      if (confirm === null || typeof confirm !== "object" || Array.isArray(confirm)) continue;
      const option = (confirm as Record<string, JsonValue>).dontAskOption;
      if (typeof option === "string" && !optionNames.includes(option))
        diagnostics.push(locatedIssue("PXBUILD1104",
          "Confirmation dontAskOption must name an option declared in player.options",
          projectPath, `/settings/player.confirmations/${pointerSegment(operation)}/dontAskOption`,
          option, `settings.player.confirmations.${operation}.dontAskOption: ${option}`));
    }
  }
  const boundBy = new Map<string, string>();
  for (const name of optionNames) {
    const spec = (playerOptions as Record<string, JsonValue>)[name];
    if (spec === null || typeof spec !== "object" || Array.isArray(spec)) continue;
    const binds = (spec as Record<string, JsonValue>).binds;
    if (typeof binds !== "string") continue;
    const earlier = boundBy.get(binds);
    // Both options would drive one behaviour, and whichever the player set
    // last would win.
    if (earlier !== undefined)
      diagnostics.push({...locatedIssue("PXBUILD1105",
        `Options ${earlier} and ${name} both bind ${binds}`,
        projectPath, `/settings/player.options/${pointerSegment(name)}/binds`, binds,
        `settings.player.options.${name}.binds: ${binds}`), severity: "warning"});
    else boundBy.set(binds, name);
  }
  // Every path the Player reads from settings must be packaged: a missing
  // music track stops the Player at boot, and a missing font or title theme
  // would otherwise be dropped silently in favour of the default.
  const titleBgm = settings["player.titleBgm"];
  if (Array.isArray(titleBgm))
    for (const [index, rule] of titleBgm.entries())
      if (rule !== null && typeof rule === "object" && !Array.isArray(rule))
        addResourceRef((rule as Record<string, JsonValue>).path,
          `settings.player.titleBgm[${index}].path`, projectPath,
          `/settings/player.titleBgm/${index}/path`);
  // Older shapes that still work but have a more general replacement.
  const replacedSettings: Record<string, string> = {
    "player.titleBgmAfter": "use player.titleBgm rules: [{\"when\": flag, \"path\": ...}, {\"path\": ...}]",
    "player.choiceLayout": "set choiceLayout on the HUD's ItemsPlaced that lists dialogue.choices",
    "player.dockArea": "put a DockArea control in the HUD",
  };
  for (const [key, replacement] of Object.entries(replacedSettings))
    if (key in settings)
      diagnostics.push({...locatedIssue("PXBUILD1109", `${key} is deprecated; ${replacement}`,
        projectPath, `/settings/${pointerSegment(key)}`, key, `settings.${key}`),
      severity: "warning"});
  const knownPlayerSettings = new Set(Object.keys((contractSchemas["project.schema.json"] as {
    properties: {settings: {properties: Record<string, unknown>}};
  }).properties.settings.properties).filter((key) => key.startsWith("player.")));
  for (const key of Object.keys(settings))
    if (key.startsWith("player.") && !knownPlayerSettings.has(key)) {
      const suggestion = closestName(key, [...knownPlayerSettings]);
      diagnostics.push({...locatedIssue("PXBUILD1110",
        `The Player does not read ${key}${suggestion === undefined ? "" : `; did you mean ${suggestion}?`}`,
        projectPath, `/settings/${pointerSegment(key)}`, key, `settings.${key}`), severity: "warning"});
    }
  for (const key of ["player.titleBgm", "player.uiHoverSound", "player.uiActivateSound",
    "player.uiCancelSound", "player.uiFont"]) {
    const value = settings[key];
    if (typeof value === "string") addResourceRef(value, `settings.${key}`,
      projectPath, `/settings/${pointerSegment(key)}`);
  }
  for (const key of ["player.fonts", "player.musicTracks"]) {
    const values = settings[key];
    if (Array.isArray(values))
      for (const [index, value] of values.entries())
        addResourceRef(value, `settings.${key}[${index}]`,
          projectPath, `/settings/${pointerSegment(key)}/${index}`);
  }
  const titleBgmAfter = settings["player.titleBgmAfter"];
  if (titleBgmAfter !== null && typeof titleBgmAfter === "object" && !Array.isArray(titleBgmAfter))
    addResourceRef((titleBgmAfter as Record<string, JsonValue>).path,
      "settings.player.titleBgmAfter.path", projectPath,
      "/settings/player.titleBgmAfter/path");
  const titleVoices = settings["player.titleVoiceVariants"];
  if (Array.isArray(titleVoices)) {
    for (const [index, voice] of titleVoices.entries())
      addResourceRef(voice, `settings.player.titleVoiceVariants[${index}]`,
        projectPath, `/settings/player.titleVoiceVariants/${index}`);
  }
  const splashes = settings["player.splashes"];
  if (Array.isArray(splashes)) {
    const authoredUiScenes = new Set(Object.values(project.uiEntryPoints));
    for (const [index, splash] of splashes.entries()) {
      if (splash === null || typeof splash !== "object" || Array.isArray(splash)) continue;
      const entry = splash as Record<string, JsonValue>;
      if (typeof entry.scene === "string" && !authoredUiScenes.has(entry.scene))
        diagnostics.push(locatedIssue("PXBUILD1055", "Splash scene must be an authored UI entry point",
          projectPath, `/settings/player.splashes/${index}/scene`, entry.scene,
          `settings.player.splashes[${index}].scene: ${entry.scene}`));
      const minimumDuration = typeof entry.minimumDuration === "number" ? entry.minimumDuration : 2;
      const skipAllowedAfter = typeof entry.skipAllowedAfter === "number" ? entry.skipAllowedAfter : 0.5;
      if (skipAllowedAfter > minimumDuration)
        diagnostics.push(locatedIssue("PXBUILD1056", "Splash skipAllowedAfter exceeds minimumDuration",
          projectPath, `/settings/player.splashes/${index}`,
          entry, `settings.player.splashes[${index}]`));
      addResourceRef(entry.audio, `settings.player.splashes[${index}].audio`,
        projectPath, `/settings/player.splashes/${index}/audio`);
      if (Array.isArray(entry.audioVariants)) {
        for (const [variantIndex, voice] of entry.audioVariants.entries())
          addResourceRef(voice,
            `settings.player.splashes[${index}].audioVariants[${variantIndex}]`,
            projectPath,
            `/settings/player.splashes/${index}/audioVariants/${variantIndex}`);
      }
    }
  }
  for (const [index, item] of (game.gallery ?? []).entries()) {
    if (item === null || typeof item !== "object" || Array.isArray(item)) continue;
    const gallery = item as Record<string, JsonValue>;
    addResourceRef(gallery.image, `${project.gameCatalog}:gallery[${index}].image`,
      project.gameCatalog, `/gallery/${index}/image`);
    addResourceRef(gallery.thumbnail, `${project.gameCatalog}:gallery[${index}].thumbnail`,
      project.gameCatalog, `/gallery/${index}/thumbnail`);
  }
  resourceSourceDocuments.set(project.gameCatalog,
    {document: game, text: input.documentSources?.[project.gameCatalog]});
  for (const [locale, value] of localeDocuments) {
    if (value === null || typeof value !== "object" || Array.isArray(value)) continue;
    const localeValue = value as Record<string, JsonValue>;
    const fontChain = localeValue.fontChain;
    if (Array.isArray(fontChain)) {
      for (const [index, font] of fontChain.entries())
        addResourceRef(font, `Content/Localization/${locale}.json:fontChain`,
          `Content/Localization/${locale}.json`, `/fontChain/${index}`);
    }
    const resources = localeValue.resources;
    if (resources !== null && typeof resources === "object" &&
        !Array.isArray(resources)) {
      for (const [id, resource] of Object.entries(resources))
        addResourceRef(resource,
          `Content/Localization/${locale}.json:resources.${id}`,
          `Content/Localization/${locale}.json`,
          `/resources/${pointerSegment(id)}`);
    }
  }
  // A portrait can be referenced by Story, UI, a locale, a dynamic include, or
  // a catalog identity. Expand every reachable descriptor, not just Story
  // dependencies, before collecting the final package graph.
  const portraitPaths = new Set(resourceAssets.map((asset) => asset.source)
    .filter((path) => path.endsWith(".pxportrait")));
  for (const reference of dependencyReferences) {
    const resolved = canonicalResourceReference(reference.value, resourceAssets);
    if (resolved?.source.endsWith(".pxportrait")) portraitPaths.add(resolved.source);
  }
  for (const descriptorPath of portraitPaths) {
    let authored: unknown = sourceFiles[descriptorPath];
    try { authored ??= input.resourceText?.(descriptorPath); }
    catch (error) {
      diagnostics.push(issue("PXBUILD1045", "Could not read layered portrait descriptor",
        descriptorPath, String(error)));
      continue;
    }
    if (authored === undefined) {
      diagnostics.push(issue("PXBUILD1045", "Layered portrait descriptor is missing",
        descriptorPath));
      continue;
    }
    const parsed = typeof authored === "string"
      ? parseDocument<LayeredPortraitDocument>("portrait", authored, descriptorPath)
      : validateDocument<LayeredPortraitDocument>("portrait", authored, descriptorPath);
    if (!parsed.valid || parsed.value === undefined) {
      diagnostics.push(issue("PXBUILD1045", "Layered portrait descriptor is invalid",
        descriptorPath, parsed.diagnostics.slice(0, 3)
          .map((diagnostic) => diagnostic.details ?? diagnostic.message).join("; ")));
      continue;
    }
    resourceSourceDocuments.set(descriptorPath, {document: parsed.value,
      text: typeof authored === "string" ? authored :
        input.documentSources?.[descriptorPath]});
    for (const [index, layer] of parsed.value.layers.entries())
      addResourceRef(layer.path, `${descriptorPath}:layers[${index}].path`,
        descriptorPath, `/layers/${index}/path`);
  }
  // A motion descriptor is itself a package resource; every image referenced
  // by it must join the dependency graph before ownership/fingerprint checks.
  const motionPaths = new Set(resourceAssets.map((asset) => asset.source)
    .filter((path) => path.endsWith(".pxmotion")));
  for (const reference of dependencyReferences) {
    const resolved = canonicalResourceReference(reference.value, resourceAssets);
    if (resolved?.source.endsWith(".pxmotion")) motionPaths.add(resolved.source);
  }
  for (const descriptorPath of motionPaths) {
    let authored: unknown = sourceFiles[descriptorPath];
    try { authored ??= input.resourceText?.(descriptorPath); }
    catch (error) {
      diagnostics.push(issue("PXBUILD1060", "Could not read motion descriptor",
        descriptorPath, String(error)));
      continue;
    }
    if (authored === undefined) {
      diagnostics.push(issue("PXBUILD1060", "Motion descriptor is missing", descriptorPath));
      continue;
    }
    const parsed = typeof authored === "string"
      ? parseDocument<MotionDocument>("motion", authored, descriptorPath)
      : validateDocument<MotionDocument>("motion", authored, descriptorPath);
    if (!parsed.valid || parsed.value === undefined) {
      diagnostics.push(issue("PXBUILD1060", "Motion descriptor is invalid",
        descriptorPath, parsed.diagnostics.slice(0, 3)
          .map((diagnostic) => diagnostic.details ?? diagnostic.message).join("; ")));
      continue;
    }
    resourceSourceDocuments.set(descriptorPath, {document: parsed.value,
      text: typeof authored === "string" ? authored :
        input.documentSources?.[descriptorPath]});
    for (const [index, layer] of parsed.value.layers.entries())
      if (layer.kind !== "group")
        addResourceRef(layer.image, `${descriptorPath}:layers[${index}].image`,
          descriptorPath, `/layers/${index}/image`);
    for (const [index, marker] of (parsed.value.markers ?? []).entries())
      addResourceRef(marker.audio, `${descriptorPath}:markers[${index}].audio`,
        descriptorPath, `/markers/${index}/audio`);
  }
  const dependencyCollection = collectProjectDependencies({
    assets: resourceAssets,
    references: dependencyReferences,
    extensions: [...extensionDocuments].map(([manifestPath, extension]) => ({
      manifestPath,
      entry: extension.entry,
      ...(extension.modules === undefined ? {} : {modules: extension.modules}),
    })),
  });
  diagnostics.push(...dependencyCollection.diagnostics);
  if (input.resourceExists !== undefined) {
    let locationsBySource: Map<string, LocatedResourceReference[]> | undefined;
    const sourceSpans = new Map<string, ReturnType<typeof jsonPointerSpans>>();
    const missingLocation = (source: string) => {
      if (locationsBySource === undefined) {
        locationsBySource = new Map();
        // Most builds have no missing resource. Resolve author locations only
        // after the first failure, so large successful graphs pay no extra scan.
        for (const reference of dependencyReferences) {
          if (reference.sourcePath === undefined ||
              reference.sourcePointer === undefined) continue;
          const resolved = canonicalResourceReference(reference.value, resourceAssets);
          if (resolved === undefined) continue;
          const locations = locationsBySource.get(resolved.source) ?? [];
          locations.push(reference);
          locationsBySource.set(resolved.source, locations);
        }
      }
      const candidates = locationsBySource.get(source) ?? [];
      for (const candidate of candidates) {
        const sourcePath = candidate.sourcePath!;
        const context = resourceSourceDocuments.get(sourcePath);
        if (context?.text === undefined) continue;
        let spans = sourceSpans.get(sourcePath);
        if (spans === undefined) {
          spans = jsonPointerSpans(context.text, context.document, sourcePath);
          sourceSpans.set(sourcePath, spans);
        }
        const span = spans.get(candidate.sourcePointer!);
        if (span !== undefined) return {span, origin: candidate.origin};
      }
      const storySpan = storyResourceLocations.get(source);
      if (storySpan !== undefined) return {span: storySpan, origin: "Story"};
      return {origin: candidates[0]?.origin};
    };
    for (const dependency of dependencyCollection.dependencies) {
      if (!input.resourceExists(dependency.source)) {
        const location = missingLocation(dependency.source);
        diagnostics.push({...issue("PXBUILD1044",
          `Project dependency source is missing: ${dependency.source}`,
          dependency.source,
          dependency.logicalPath),
          ...(location.span === undefined ? {} : {span: location.span}),
          ...(location.origin === undefined ? {} :
            {hint: `Referenced from ${location.origin}`})});
      }
    }
  }
  if (!dependencyCollection.valid ||
      diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return {diagnostics, valid: false};

  const runtimeIrPath = "Runtime/main.pxir";
  const sourceMapPath = "Runtime/main.pxmap";
  const files: Record<string, string> = {
    [projectPath]: canonicalJson(project),
    [project.storyIndex]: serialized(documents[project.storyIndex]),
    [project.gameCatalog]: gameDeclaredInStory ? canonicalJson(game)
      : serialized(documents[project.gameCatalog]),
    [runtimeIrPath]: canonicalJson(compiled.runtimeIr),
    [sourceMapPath]: canonicalJson(compiled.sourceMap),
  };
  for (const localized of Object.values(localeArtifacts)) {
    files[localized.runtimeIrPath] = canonicalJson(localized.runtimeIr);
    files[localized.sourceMapPath] = canonicalJson(localized.sourceMap);
  }
  for (const [path, text] of Object.entries(files)) {
    if ((path.endsWith(".pxir") || path.endsWith(".pxmap")) &&
        Buffer.byteLength(text, "utf8") > contractDocumentByteLimit(path.endsWith(".pxir") ? "runtimeIr" : "sourceMap"))
      diagnostics.push(issue("PXPROJECT1050", "Compiled artifact exceeds the native 64 MiB document limit", path,
        "Reduce this project's aggregate content before previewing or packaging; no artifact was published."));
  }
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error")) return {diagnostics, valid: false};
  for (const descriptor of project.characters ?? []) files[descriptor.source] = serialized(documents[descriptor.source]);
  for (const path of project.extensions) files[path] = serialized(documents[path]);
  for (const [path, value] of saveMigrationDocuments) files[path] = canonicalJson(value);
  for (const path of Object.values(project.uiEntryPoints)) files[path] = serialized(documents[path]);
  for (const [path, value] of uiComponentDocuments) files[path] = canonicalJson(value);
  for (const [path, value] of effectDocuments) {
    files[path] = canonicalJson(value);
    files[value.shader] = sourceFiles[value.shader]!;
  }
  for (const [locale, value] of localeDocuments) files[`Content/Localization/${locale}.json`] = canonicalJson(value);
  for (const [path, value] of Object.entries(sourceFiles)) {
    // Identity maps are build-time author metadata. Only the compiled IR and
    // source map, not the sidecar itself, belong in the distributed artifact.
    if (!path.endsWith(".pxstory.pxids.json")) files[path] = value;
  }
  for (const reference of includedResources) {
    if (files[reference.value] !== undefined)
      diagnostics.push(issue("PXBUILD1054",
        "resourceIncludes file collides with a generated or authored document",
        projectPath, reference.value));
  }
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error"))
    return {diagnostics, valid: false};

  return {
    artifact: {
      engineVersion: ENGINE_VERSION,
      project,
      runtimeIr: compiled.runtimeIr,
      sourceMap: compiled.sourceMap,
      runtimeIrPath,
      sourceMapPath,
      localeArtifacts,
      files,
      dependencies: dependencyCollection.dependencies,
    },
    diagnostics,
    valid: true,
  };
}
