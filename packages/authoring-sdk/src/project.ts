import {canonicalJson} from "./canonical-json.js";
import {parseDocument, validateDocument} from "./contracts.js";
import {compileStoryProject, type StoryIndexDocument} from "./story-project.js";
import type {
  AuthoringDiagnostic,
  CharacterDocument,
  ExtensionManifest,
  GameDocument,
  JsonValue,
  RuntimeIrDocument,
  RuntimeSourceMap,
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
  readonly schemaRevision: 2;
  readonly id: string;
  readonly targetLayer: "stage";
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

export interface SaveMigrationDocument {
  readonly format: "PrismatiXSaveMigration";
  readonly schemaRevision: 2;
  readonly id: string;
  readonly from: SaveMigrationVersion;
  readonly to: SaveMigrationVersion;
  readonly anchor: JsonValue;
  readonly operations: readonly JsonValue[];
}

export interface CompileProjectInput {
  readonly project: unknown;
  /** Canonical JSON documents keyed by project-relative path. */
  readonly documents: Readonly<Record<string, unknown>>;
  /** Story source text keyed by the exact paths used by story.pxindex. */
  readonly sourceFiles: Readonly<Record<string, string>>;
  /** Locale documents keyed by canonical locale tag. */
  readonly locales: Readonly<Record<string, unknown>>;
  readonly projectPath?: string;
  readonly committedRevision?: number;
}

export interface BuildArtifact {
  readonly engineVersion: "0.2.0";
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

const builtInScreenEffects = new Set([
  "shake", "flash", "fade", "zoom", "pan", "blur", "vignette",
  "color-grade", "rule-dissolve",
]);
const gpuOnlyScreenEffects = new Set(["blur", "vignette", "color-grade"]);

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
    if (!builtInScreenEffects.has(preset) && !customEffects.has(preset)) {
      diagnostics.push({
        ...issue("PXBUILD1024", "Story references an unknown screen effect preset", path, preset),
        documentId: runtimeIr.documentId,
        sourceId: operation.sourceId,
        hint: "Use a built-in Screen preset or declare a validated project effect.",
      });
    } else if ((gpuOnlyScreenEffects.has(preset) || customEffects.has(preset)) &&
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

function localizedArguments(operation: RuntimeIrDocument["operations"][number]):
Readonly<Record<string, string>> {
  if (!["dialogue", "narration", "choiceOption"].includes(operation.kind))
    return operation.arguments;
  const result: Record<string, string> = {...operation.arguments};
  delete result.text;
  return result;
}

function alignLocalizedProgram(
  locale: string,
  base: {readonly runtimeIr: RuntimeIrDocument; readonly sourceMap: RuntimeSourceMap},
  localized: {readonly runtimeIr: RuntimeIrDocument; readonly sourceMap: RuntimeSourceMap},
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
        canonicalJson(localizedArguments(operation), false) !==
          canonicalJson(localizedArguments(canonical), false)) {
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
  const projectPath = input.projectPath ?? "project.pxproject";
  const parsedProject = parseValue<ProjectDocument>("project", input.project, projectPath);
  diagnostics.push(...parsedProject.diagnostics);
  const project = parsedProject.value;
  if (!parsedProject.valid || project === undefined) return {diagnostics, valid: false};

  const storyIndex = documentAt<StoryIndexDocument>("storyIndex", project.storyIndex, input.documents, diagnostics);
  const game = documentAt<GameDocument>("game", project.gameCatalog, input.documents, diagnostics);
  const characters: CharacterDocument[] = [];
  for (const descriptor of project.characters ?? []) {
    const character = documentAt<CharacterDocument>("character", descriptor.source, input.documents, diagnostics);
    if (character !== undefined) characters.push(character);
  }
  const extensions: ExtensionManifest[] = [];
  for (const path of project.extensions) {
    const extension = documentAt<ExtensionManifest>("extension", path, input.documents, diagnostics);
    if (extension !== undefined) {
      const compatible = satisfiesVersionRange(extension.requiredEngineVersion);
      if (parseVersion(extension.version) === undefined)
        diagnostics.push(issue("PXBUILD1011", "Extension version is not SemVer", path, extension.version));
      else if (compatible === undefined)
        diagnostics.push(issue("PXBUILD1012", "Extension requiredEngineVersion is not a supported SemVer range", path, extension.requiredEngineVersion));
      else if (!compatible)
        diagnostics.push(issue("PXBUILD1013", "Extension is incompatible with engine 0.2.0", path, extension.requiredEngineVersion));
      else extensions.push(extension);
    }
  }
  const saveMigrationDocuments = new Map<string, SaveMigrationDocument>();
  const migrationSources = new Map<string, SaveMigrationDescriptor>();
  const versionKey = (version: SaveMigrationVersion) =>
    `${version.contentVersion}\n${version.saveVersion}`;
  for (const descriptor of project.saveMigrations ?? []) {
    const migration = documentAt<SaveMigrationDocument>(
      "saveMigration", descriptor.asset, input.documents, diagnostics,
    );
    if (migration === undefined) continue;
    if (migration.id !== descriptor.id ||
        versionKey(migration.from) !== versionKey(descriptor.from) ||
        versionKey(migration.to) !== versionKey(descriptor.to)) {
      diagnostics.push(issue("PXBUILD1007", "Save migration document does not match its project descriptor", descriptor.asset, descriptor.id));
      continue;
    }
    const source = versionKey(descriptor.from);
    if (migrationSources.has(source)) {
      diagnostics.push(issue("PXBUILD1008", "Save migration chain is ambiguous", projectPath, source));
      continue;
    }
    migrationSources.set(source, descriptor);
    saveMigrationDocuments.set(descriptor.asset, migration);
  }
  const migrationTarget = versionKey({contentVersion: project.contentVersion, saveVersion: project.saveVersion});
  for (const descriptor of project.saveMigrations ?? []) {
    const visited = new Set<string>();
    let current = versionKey(descriptor.from);
    for (let step = 0; current !== migrationTarget; step += 1) {
      if (step >= 64 || visited.has(current)) {
        diagnostics.push(issue("PXBUILD1009", "Save migration chain contains a cycle or exceeds 64 steps", descriptor.asset));
        break;
      }
      visited.add(current);
      const next = migrationSources.get(current);
      if (next === undefined) {
        diagnostics.push(issue("PXBUILD1010", "Save migration chain does not reach the current content/save version", descriptor.asset, current));
        break;
      }
      current = versionKey(next.to);
    }
  }
  for (const [id, path] of Object.entries(project.uiEntryPoints)) {
    const ui = documentAt<JsonValue>("ui", path, input.documents, diagnostics);
    if (ui === undefined) diagnostics.push(issue("PXBUILD1002", `UI entry point ${id} is unavailable`, path, id));
  }
  const effectDocuments = new Map<string, CustomEffectDocument>();
  const effectIds = new Set<string>();
  for (const descriptor of project.effects ?? []) {
    const effect = documentAt<CustomEffectDocument>(
      "effect", descriptor.source, input.documents, diagnostics,
    );
    if (effect === undefined) continue;
    if (project.graphicsTier !== "gpu-effects") {
      diagnostics.push(issue("PXBUILD1020", "Custom effects require graphicsTier gpu-effects", descriptor.source));
      continue;
    }
    if (effect.id !== descriptor.id || !effectIds.add(effect.id)) {
      diagnostics.push(issue("PXBUILD1021", "Custom effect identity is duplicated or disagrees with its project descriptor", descriptor.source, descriptor.id));
      continue;
    }
    if (!effect.shader.endsWith(".hlsl") || input.sourceFiles[effect.shader] === undefined) {
      diagnostics.push(issue("PXBUILD1022", "Custom effect HLSL source is missing or has the wrong extension", effect.shader));
      continue;
    }
    const uniformNames = new Set<string>();
    const uniformSlots = new Set<number>();
    for (const uniform of effect.uniforms) {
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
        diagnostics.push(issue("PXBUILD1023", "Custom effect uniforms must have unique names/slots and valid ranges", descriptor.source, uniform.name));
      }
    }
    effectDocuments.set(descriptor.source, effect);
  }
  const uiComponentDocuments = new Map<string, JsonValue>();
  const uiComponentIds = new Set<string>();
  for (const descriptor of project.uiComponents ?? []) {
    const component = documentAt<JsonValue>("uiComponent", descriptor.source,
      input.documents, diagnostics);
    if (component === undefined) {
      diagnostics.push(issue("PXBUILD1014", "UI component document is missing or invalid",
        descriptor.source, descriptor.id));
      continue;
    }
    if ((component as Record<string, JsonValue>).id !== descriptor.id) {
      diagnostics.push(issue("PXBUILD1017",
        "UI component identity disagrees with its project descriptor",
        descriptor.source, descriptor.id));
      continue;
    }
    if (!uiComponentIds.add(descriptor.id)) {
      diagnostics.push(issue("PXBUILD1018", "UI component id is duplicated",
        projectPath, descriptor.id));
      continue;
    }
    uiComponentDocuments.set(descriptor.source, component);
  }
  if (storyIndex !== undefined && storyIndex.entryScene !== project.entry.story) {
    diagnostics.push(issue("PXBUILD1005", "Project entry.story must match storyIndex.entryScene", project.storyIndex, project.entry.story));
  }
  const localeDocuments = new Map<string, JsonValue>();
  for (const locale of project.supportedLocales) {
    const source = input.locales[locale];
    const path = `Content/Localization/${locale}.json`;
    if (source === undefined) {
      diagnostics.push(issue("PXBUILD1003", `Locale document ${locale} is missing`, path, locale));
      continue;
    }
    const parsed = parseValue<JsonValue>("locale", source, path);
    diagnostics.push(...parsed.diagnostics);
    if (parsed.valid && parsed.value !== undefined) {
      const localeValue = parsed.value as {locale?: JsonValue};
      if (localeValue.locale !== locale) diagnostics.push(issue("PXBUILD1006", "Locale document identity does not match its project key", path, locale));
      else localeDocuments.set(locale, parsed.value);
    }
  }
  if (!localeDocuments.has(project.defaultLocale)) {
    diagnostics.push(issue("PXBUILD1004", "The default locale document is unavailable", projectPath, project.defaultLocale));
  }
  if (diagnostics.some((diagnostic) => diagnostic.severity === "error") || storyIndex === undefined || game === undefined) {
    return {diagnostics, valid: false};
  }

  const resourcePaths: Record<string, string> = {};
  for (const value of project.assets ?? []) {
    if (value !== null && typeof value === "object" && !Array.isArray(value)) {
      const asset = value as Record<string, JsonValue>;
      if (typeof asset.id === "string" && typeof asset.source === "string") resourcePaths[asset.id] = asset.source;
    }
  }
  const compileLocale = (locale: string) => compileStoryProject({
      storyIndex,
      locale,
      sourceFiles: input.sourceFiles,
      storyIndexPath: project.storyIndex,
      documentId: project.id,
      ...(input.committedRevision === undefined ? {} : {committedRevision: input.committedRevision}),
      characters,
      game,
      extensions,
      resources: resourcePaths,
    });
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
  for (const locale of project.supportedLocales) {
    const localized = locale === project.defaultLocale ? compiled : compileLocale(locale);
    if (localized !== compiled) diagnostics.push(...localized.diagnostics);
    if (!localized.valid || localized.runtimeIr === undefined ||
        localized.sourceMap === undefined) continue;
    const aligned = locale === project.defaultLocale
      ? {runtimeIr: localized.runtimeIr, sourceMap: localized.sourceMap}
      : alignLocalizedProgram(locale,
        {runtimeIr: compiled.runtimeIr, sourceMap: compiled.sourceMap},
        {runtimeIr: localized.runtimeIr, sourceMap: localized.sourceMap},
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

  const runtimeIrPath = "Runtime/main.pxir";
  const sourceMapPath = "Runtime/main.pxmap";
  const files: Record<string, string> = {
    [projectPath]: canonicalJson(project),
    [project.storyIndex]: serialized(input.documents[project.storyIndex]),
    [project.gameCatalog]: serialized(input.documents[project.gameCatalog]),
    [runtimeIrPath]: canonicalJson(compiled.runtimeIr),
    [sourceMapPath]: canonicalJson(compiled.sourceMap),
  };
  for (const localized of Object.values(localeArtifacts)) {
    files[localized.runtimeIrPath] = canonicalJson(localized.runtimeIr);
    files[localized.sourceMapPath] = canonicalJson(localized.sourceMap);
  }
  for (const descriptor of project.characters ?? []) files[descriptor.source] = serialized(input.documents[descriptor.source]);
  for (const path of project.extensions) files[path] = serialized(input.documents[path]);
  for (const [path, value] of saveMigrationDocuments) files[path] = canonicalJson(value);
  for (const path of Object.values(project.uiEntryPoints)) files[path] = serialized(input.documents[path]);
  for (const [path, value] of uiComponentDocuments) files[path] = canonicalJson(value);
  for (const [path, value] of effectDocuments) {
    files[path] = canonicalJson(value);
    files[value.shader] = input.sourceFiles[value.shader]!;
  }
  for (const [locale, value] of localeDocuments) files[`Content/Localization/${locale}.json`] = canonicalJson(value);
  for (const [path, value] of Object.entries(input.sourceFiles)) files[path] = value;

  return {
    artifact: {
      engineVersion: "0.2.0",
      project,
      runtimeIr: compiled.runtimeIr,
      sourceMap: compiled.sourceMap,
      runtimeIrPath,
      sourceMapPath,
      localeArtifacts,
      files,
    },
    diagnostics,
    valid: true,
  };
}
