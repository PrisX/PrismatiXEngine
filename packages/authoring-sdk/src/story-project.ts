import {compileStory, parseStory} from "./story.js";
import {fnv1a64 as stableId} from "./fnv1a64.js";
import {applyStoryIdentityMap, storyRequiresIdentityMap} from "./story-external-identity.js";
import {jsonPointerSpans} from "./json-source-spans.js";
import type {ProjectAssetDescriptor} from "./resources.js";
import type {StoryAssetIndex} from "./story-assets.js";
import type {StoryMacroLibrary} from "./story-macros.js";
import type {StoryPortraitIndex} from "./portrait.js";
import type {
  AuthoringDiagnostic,
  CharacterDocument,
  ExtensionManifest,
  GameDocument,
  RuntimeIrOperation,
  SourceMapEntry,
  RuntimeSourceMap,
  SourceSpan,
  StoryArgument,
  StoryCompileResult,
  StoryDocument,
  StoryNode,
} from "./types.js";

export interface StoryIndexChapter {
  readonly id: string;
  readonly title: string;
  readonly scenes: readonly string[];
}

export interface StoryIndexScene {
  readonly id: string;
  readonly title?: string;
  // Keeps explicit [id] operation anchors stable when authored content is
  // moved between scenes. Shared namespaces require globally unique ids.
  readonly identityNamespace?: string;
  readonly sources: Readonly<Record<string, string>>;
  readonly requiredLabels?: readonly string[];
}

export interface StoryIndexDocument {
  readonly format: "PrismatiXStoryIndex";
  readonly schemaRevision: 1;
  readonly id: string;
  readonly entryScene: string;
  readonly chapters: readonly StoryIndexChapter[];
  readonly scenes: readonly StoryIndexScene[];
}

/** Synchronous, per-locale scene progress. A skipped or invalid scene still
 * advances the count so consumers can distinguish work from a stalled build. */
export interface StoryProjectProgress {
  readonly phase: "parse" | "compile";
  readonly locale: string;
  readonly sceneId: string;
  readonly sourcePath?: string;
  readonly current: number;
  readonly total: number;
  /** The phase reused a previously verified parse or compile result. */
  readonly cached?: boolean;
}

export interface StoryProjectCompileContext {
  readonly storyIndex: StoryIndexDocument;
  readonly locale: string;
  // Source text keyed by the exact locale-specific path stored in story.pxindex.
  // Keeping IO outside the compiler makes the same API usable from Node, a
  // browser Editor, tests, and future non-filesystem project providers.
  readonly sourceFiles: Readonly<Record<string, string>>;
  readonly storyIndexPath?: string;
  /** Exact JSON text used to parse storyIndex, for index declaration spans. */
  readonly storyIndexSource?: string;
  readonly documentId?: string;
  readonly committedRevision?: number;
  readonly characters?: readonly CharacterDocument[];
  readonly game?: GameDocument;
  readonly extensions?: readonly ExtensionManifest[];
  readonly resources?: Readonly<Record<string, string>>;
  readonly resourceAssets?: readonly ProjectAssetDescriptor[];
  readonly storyAssets?: StoryAssetIndex;
  /** Project-wide macros every scene may call (`storyMacros` in prismatix.json). */
  readonly storyMacros?: StoryMacroLibrary;
  /** Layered portrait descriptors `[face]` reads. */
  readonly portraits?: StoryPortraitIndex;
  readonly onProgress?: (event: StoryProjectProgress) => void;
}

interface StoryCompilationSnapshot {
  readonly inputs: string;
  readonly sources: readonly (readonly [string, string | undefined, string | undefined])[];
}

function storyCompilationSnapshot(context: StoryProjectCompileContext): StoryCompilationSnapshot {
  // These are all non-source inputs read by compileStoryProject/compileStory.
  // Retain exact source strings rather than a short hash: a cache collision
  // must never silently reuse save identities or cross-scene control flow.
  const inputs = JSON.stringify([
    context.storyIndex, context.locale,
    context.storyIndexPath ?? "Story/story.pxindex",
    context.storyIndexSource,
    context.documentId ?? context.storyIndex.id,
    context.characters, context.game, context.extensions,
    context.resources, context.resourceAssets, context.storyAssets?.fingerprint,
    context.storyMacros?.fingerprint, context.portraits?.fingerprint,
  ]);
  const sources = context.storyIndex.scenes.map((scene) => {
    const path = scene.sources[context.locale] ?? "";
    return [path, context.sourceFiles[path], context.sourceFiles[`${path}.pxids.json`]] as const;
  });
  return {inputs, sources};
}

function sameStorySnapshot(left: StoryCompilationSnapshot, right: StoryCompilationSnapshot): boolean {
  return left.inputs === right.inputs && left.sources.length === right.sources.length &&
    left.sources.every((entry, index) => {
      const other = right.sources[index];
      return other !== undefined && entry[0] === other[0] &&
        entry[1] === other[1] && entry[2] === other[2];
    });
}

interface SceneCompileSnapshot {
  readonly sourcePath: string;
  readonly source: string | undefined;
  readonly identityMap: string | undefined;
  readonly descriptor: string;
  readonly compileInputs: string;
  readonly labels: string;
}

function sameSceneCompileSnapshot(left: SceneCompileSnapshot, right: SceneCompileSnapshot): boolean {
  return left.sourcePath === right.sourcePath && left.source === right.source &&
    left.identityMap === right.identityMap && left.descriptor === right.descriptor &&
    left.compileInputs === right.compileInputs && left.labels === right.labels;
}

interface CachedSceneCompilation {
  readonly snapshot: SceneCompileSnapshot;
  readonly labels: ReadonlySet<string>;
  readonly stubSourceIds: ReadonlySet<string>;
  readonly result: StoryCompileResult;
}

function validCachedResourceLocations(value: unknown, sourcePath: string,
  sourceByteLength: number): boolean {
  if (!Array.isArray(value) || value.length > 1_000_000) return false;
  return value.every((entry: unknown) => {
    const location = entry as {source?: unknown; span?: SourceSpan} | null;
    const span = location?.span;
    if (typeof location?.source !== "string" ||
        span?.path !== sourcePath) return false;
    for (const position of [span.start, span.end]) {
      if (!Number.isSafeInteger(position?.line) || position.line < 1 ||
          !Number.isSafeInteger(position?.column) || position.column < 1 ||
          !Number.isSafeInteger(position?.offset) || position.offset < 0 ||
          position.offset > sourceByteLength) return false;
    }
    return span.start.offset <= span.end.offset;
  });
}

// Exact source strings are compared, so a short hash cannot reuse save
// identities after a collision. Keep labels and compiled output, but never
// retain parsed ASTs between builds: commercial projects can have hundreds of
// thousands of nodes.
class SceneCompilationCache {
  private readonly entries = new Map<string, Map<string, CachedSceneCompilation>>();
  private generation = 0;
  private persistedGeneration = 0;

  needsPersistence(): boolean { return this.generation !== this.persistedGeneration; }
  markPersisted(): void { this.persistedGeneration = this.generation; }

  exportSnapshot(): unknown {
    return {format: "PrismatiXStorySceneCache/2", locales: [...this.entries].map(
      ([locale, scenes]) => {
        const latest = [...scenes.values()].at(-1)?.snapshot;
        return {locale, compileInputs: latest?.compileInputs ?? "",
          labels: latest?.labels ?? "", scenes: [...scenes].filter(([, entry]) =>
            entry.snapshot.compileInputs === latest?.compileInputs &&
            entry.snapshot.labels === latest?.labels).map(([id, entry]) => ({
            id, sourcePath: entry.snapshot.sourcePath, source: entry.snapshot.source,
            identityMap: entry.snapshot.identityMap, descriptor: entry.snapshot.descriptor,
            labels: [...entry.labels], stubSourceIds: [...entry.stubSourceIds],
            result: entry.result,
          }))};
      })};
  }

  importSnapshot(value: unknown): void {
    const object = value as {format?: unknown; locales?: unknown};
    if (object === null || object.format !== "PrismatiXStorySceneCache/2" ||
        !Array.isArray(object.locales) || object.locales.length > 32)
      throw new Error("Invalid Story scene cache envelope");
    const entries = new Map<string, Map<string, CachedSceneCompilation>>();
    for (const rawLocale of object.locales) {
      const group = rawLocale as {locale?: unknown; compileInputs?: unknown;
        labels?: unknown; scenes?: unknown};
      if (group === null || typeof group.locale !== "string" ||
          typeof group.compileInputs !== "string" || typeof group.labels !== "string" ||
          !Array.isArray(group.scenes) || group.scenes.length > 4096 ||
          entries.has(group.locale)) throw new Error("Invalid Story scene cache locale");
      const scenes = new Map<string, CachedSceneCompilation>();
      for (const rawScene of group.scenes) {
        const scene = rawScene as {id?: unknown; sourcePath?: unknown; source?: unknown;
          identityMap?: unknown; descriptor?: unknown; labels?: unknown;
          stubSourceIds?: unknown; result?: unknown};
        const result = scene?.result as StoryCompileResult | undefined;
        if (scene === null || typeof scene.id !== "string" || scenes.has(scene.id) ||
            typeof scene.sourcePath !== "string" || typeof scene.source !== "string" ||
            (scene.identityMap !== undefined && typeof scene.identityMap !== "string") ||
            typeof scene.descriptor !== "string" || !Array.isArray(scene.labels) ||
            !scene.labels.every((label: unknown) => typeof label === "string") ||
            !Array.isArray(scene.stubSourceIds) ||
            !scene.stubSourceIds.every((id: unknown) => typeof id === "string") ||
            result?.valid !== true || !Array.isArray(result.diagnostics) ||
            !Array.isArray(result.runtimeIr?.operations) ||
            !Array.isArray(result.sourceMap?.mappings) ||
            !validCachedResourceLocations(result.resourceLocations,
              scene.sourcePath, Buffer.byteLength(scene.source, "utf8")))
          throw new Error("Invalid Story scene cache entry");
        scenes.set(scene.id, {snapshot: {sourcePath: scene.sourcePath,
          source: scene.source, identityMap: scene.identityMap,
          descriptor: scene.descriptor, compileInputs: group.compileInputs,
          labels: group.labels}, labels: new Set(scene.labels),
          stubSourceIds: new Set(scene.stubSourceIds), result});
      }
      entries.set(group.locale, scenes);
    }
    this.entries.clear();
    for (const [locale, scenes] of entries) this.entries.set(locale, scenes);
    this.generation = 0;
    this.persistedGeneration = 0;
  }

  prune(locale: string, sceneIds: ReadonlySet<string>): void {
    const scenes = this.entries.get(locale);
    if (scenes === undefined) return;
    for (const id of scenes.keys()) if (!sceneIds.has(id)) {
      scenes.delete(id);
      this.generation += 1;
    }
  }

  hasEntries(locale: string): boolean {
    return (this.entries.get(locale)?.size ?? 0) > 0;
  }

  labels(locale: string, sceneId: string, path: string, source: string,
    identityMap: string | undefined): ReadonlySet<string> | undefined {
    const previous = this.entries.get(locale)?.get(sceneId);
    return previous !== undefined && previous.snapshot.sourcePath === path &&
      previous.snapshot.source === source &&
      previous.snapshot.identityMap === identityMap ? previous.labels : undefined;
  }

  compiled(locale: string, sceneId: string,
    snapshot: SceneCompileSnapshot): CachedSceneCompilation | undefined {
    const previous = this.entries.get(locale)?.get(sceneId);
    return previous !== undefined && sameSceneCompileSnapshot(previous.snapshot, snapshot)
      ? previous : undefined;
  }

  remember(locale: string, sceneId: string, snapshot: SceneCompileSnapshot,
    labels: ReadonlySet<string>, stubSourceIds: ReadonlySet<string>,
    result: StoryCompileResult): void {
    if (!result.valid || result.runtimeIr === undefined || result.sourceMap === undefined) return;
    let localeEntries = this.entries.get(locale);
    if (localeEntries === undefined) {
      localeEntries = new Map();
      this.entries.set(locale, localeEntries);
    }
    localeEntries.set(sceneId, {snapshot, labels, stubSourceIds, result});
    this.generation += 1;
  }
}

/** Optional, in-process cache for repeated project builds. Unchanged projects
 * reuse the full result; a Story edit can reuse unaffected compiled scenes.
 * Invalid Story is never cached, and a new revision only replaces the Runtime
 * IR envelope; operations and save identities are kept. */
export class StoryProjectCompilationCache {
  private readonly scenes = new SceneCompilationCache();
  private readonly entries = new Map<string, {
    readonly snapshot: StoryCompilationSnapshot;
    readonly result: StoryCompileResult;
  }>();

  /** Persist only successful per-scene results. The caller must version and
   * bound the storage envelope; every reuse still checks exact source text and
   * all shared compilation inputs. */
  exportScenes(): unknown { return this.scenes.exportSnapshot(); }

  /** Import an advisory cache. A damaged snapshot is rejected atomically. */
  importScenes(value: unknown): void { this.scenes.importSnapshot(value); }

  needsPersistence(): boolean { return this.scenes.needsPersistence(); }
  markPersisted(): void { this.scenes.markPersisted(); }

  compile(context: StoryProjectCompileContext): StoryCompileResult {
    const snapshot = storyCompilationSnapshot(context);
    const previous = this.entries.get(context.locale);
    if (previous !== undefined && sameStorySnapshot(previous.snapshot, snapshot)) {
      for (const phase of ["parse", "compile"] as const) {
        for (const [index, scene] of context.storyIndex.scenes.entries()) {
          const sourcePath = scene.sources[context.locale];
          context.onProgress?.({phase, locale: context.locale, sceneId: scene.id,
            ...(sourcePath === undefined ? {} : {sourcePath}),
            current: index + 1, total: context.storyIndex.scenes.length, cached: true});
        }
      }
      const runtimeIr = previous.result.runtimeIr;
      return runtimeIr === undefined ? previous.result : {
        ...previous.result,
        runtimeIr: {...runtimeIr, committedRevision: context.committedRevision ?? 0},
      };
    }

    // The progress callback is user code. Compile the exact source values
    // captured in the cache key even if that callback edits its input map.
    const sourceFiles: Record<string, string> = Object.create(null) as Record<string, string>;
    for (const [path, source, identityMap] of snapshot.sources) {
      if (path === "") continue;
      if (source !== undefined) sourceFiles[path] = source;
      if (identityMap !== undefined) sourceFiles[`${path}.pxids.json`] = identityMap;
    }
    const result = compileStoryProjectInternal({...context, sourceFiles}, this.scenes);
    if (result.valid && result.runtimeIr !== undefined && result.sourceMap !== undefined) {
      this.scenes.prune(context.locale,
        new Set(context.storyIndex.scenes.map((scene) => scene.id)));
      this.entries.set(context.locale, {snapshot, result});
    }
    return result;
  }
}

interface ParsedScene {
  readonly descriptor: StoryIndexScene;
  readonly path: string;
  readonly source: string;
  readonly identityMap: string | undefined;
  readonly document?: StoryDocument;
  readonly labels: ReadonlySet<string>;
}

interface RewrittenScene {
  readonly document: StoryDocument;
  readonly stubSourceIds: ReadonlySet<string>;
}

function issue(code: string, message: string, location?: string | SourceSpan,
  details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    ...(location === undefined ? {} :
      {path: typeof location === "string" ? location : location.path}),
    ...(typeof location === "object" ? {span: location} : {}),
    ...(details === undefined ? {} : {details}),
  };
}

function fallbackSpan(path: string): SourceSpan {
  return {
    path,
    start: {line: 1, column: 1, offset: 0},
    end: {line: 1, column: 1, offset: 0},
  };
}

function mappedSpan(source: string, mapping: SourceMapEntry): SourceSpan {
  const offsetAt = (line: number, column: number): number => {
    let offset = 0;
    for (let current = 1; current < line; current += 1) {
      const newline = source.indexOf("\n", offset);
      if (newline < 0) return Buffer.byteLength(source, "utf8");
      offset = newline + 1;
    }
    return Buffer.byteLength(source.slice(0,
      Math.min(source.length, offset + column - 1)), "utf8");
  };
  return {path: mapping.sourceUri,
    start: {line: mapping.startLine, column: mapping.startColumn,
      offset: offsetAt(mapping.startLine, mapping.startColumn)},
    end: {line: mapping.endLine, column: mapping.endColumn,
      offset: offsetAt(mapping.endLine, mapping.endColumn)}};
}

function storyIndexSpans(source: string | undefined, index: StoryIndexDocument,
  path: string): ReadonlyMap<string, SourceSpan> {
  return jsonPointerSpans(source, index, path);
}

// Internal bundle labels intentionally do not embed author-controlled ids.
// Hashing keeps them valid .pxstory identifiers while avoiding collisions
// between ids such as `chapter.one` and author labels that contain dots.
function sceneEntryTarget(sceneId: string): string {
  return `pxscene.entry.${stableId(`scene-entry:${sceneId}`)}`;
}

function sceneLabelTarget(sceneId: string, label: string): string {
  return `pxscene.label.${stableId(`scene-label:${sceneId}:${label}`)}`;
}

function argumentText(argument: StoryArgument | undefined): string | undefined {
  if (argument === undefined || argument.value === null || typeof argument.value === "object") return undefined;
  return String(argument.value);
}

interface FlowArguments {
  readonly scene: StoryArgument | undefined;
  readonly target: StoryArgument | undefined;
  readonly targetWasPositional: boolean;
}

function flowArguments(node: StoryNode): FlowArguments {
  let scene: StoryArgument | undefined;
  let target: StoryArgument | undefined;
  let firstPositional: StoryArgument | undefined;
  for (const argument of node.arguments ?? []) {
    if (argument.name === "scene") scene = argument;
    else if (argument.name === "target" || argument.name === "goto") target ??= argument;
    else if (argument.name === undefined && firstPositional === undefined) firstPositional = argument;
  }
  if (target !== undefined) return {scene, target, targetWasPositional: false};
  if (firstPositional !== undefined) return {scene, target: firstPositional, targetWasPositional: true};
  return {scene, target: undefined, targetWasPositional: false};
}

function replaceFlowTarget(node: StoryNode, flow: FlowArguments, target: string): StoryNode {
  let positionalConsumed = false;
  const arguments_: StoryArgument[] = [];
  for (const argument of node.arguments ?? []) {
    if (argument.name === "scene" || argument.name === "target" || argument.name === "goto") continue;
    if (flow.targetWasPositional && argument.name === undefined && !positionalConsumed) {
      positionalConsumed = true;
      continue;
    }
    arguments_.push(argument);
  }
  const source = flow.target ?? flow.scene;
  arguments_.push({
    name: "target",
    raw: target,
    value: target,
    span: source?.span ?? node.span,
  });
  return {...node, arguments: arguments_};
}

function syntheticLabel(sceneId: string, target: string, suffix: string, sourceSpan: SourceSpan): StoryNode {
  return {
    id: `story-project-${stableId(`${sceneId}:${suffix}:${target}`)}`,
    kind: "label",
    span: sourceSpan,
    raw: `*${target}`,
    name: target,
  };
}

function syntheticEnd(sceneId: string, sourceSpan: SourceSpan): StoryNode {
  return {
    id: `story-project-${stableId(`${sceneId}:implicit-end`)}`,
    kind: "command",
    span: sourceSpan,
    raw: "[end]",
    name: "end",
    arguments: [],
  };
}

function rewriteScene(
  scene: ParsedScene,
  document: StoryDocument,
  scenesById: ReadonlyMap<string, ParsedScene>,
  duplicateLabels: ReadonlySet<string>,
  extraDiagnostics: readonly AuthoringDiagnostic[],
): RewrittenScene {
  const diagnostics: AuthoringDiagnostic[] = [...document.diagnostics, ...extraDiagnostics];
  const externalTargets = new Set<string>();
  const firstSpan = document.nodes[0]?.span ?? fallbackSpan(scene.path);
  const lastSpan = document.nodes.at(-1)?.span ?? firstSpan;
  const nodes: StoryNode[] = [
    syntheticLabel(scene.descriptor.id, sceneEntryTarget(scene.descriptor.id), "entry", firstSpan),
  ];

  for (const node of document.nodes) {
    if (node.kind === "label" && node.name !== undefined) {
      // RuntimeIrAdapter exposes label operation text as a second VM lookup
      // alias. Only repeated author labels need a scene prefix, preserving
      // existing external lookups for project-wide unique labels.
      nodes.push({...node, name: sceneLabelTarget(scene.descriptor.id, node.name),
        raw: duplicateLabels.has(node.name)
          ? `*${scene.descriptor.id}:${node.name}` : node.raw});
      continue;
    }
    if (node.kind !== "command" || node.name === undefined || !["choice", "jump", "call"].includes(node.name)) {
      nodes.push(node);
      continue;
    }

    const flow = flowArguments(node);
    const targetSceneId = argumentText(flow.scene) ?? scene.descriptor.id;
    const targetScene = scenesById.get(targetSceneId);
    if (targetScene === undefined) {
      diagnostics.push(issue("PXSTORY1304", `Unknown Story scene: ${targetSceneId}`,
        flow.scene?.span ?? node.span, targetSceneId));
      // Point the node at a stub so the scene compiler does not report the
      // same mistake again under an internal target name.
      const stub = sceneEntryTarget(targetSceneId);
      externalTargets.add(stub);
      nodes.push(replaceFlowTarget(node, flow, stub));
      continue;
    }

    const authoredTarget = argumentText(flow.target);
    if (authoredTarget === undefined && flow.scene === undefined) {
      // Let the ordinary per-document compiler report its established
      // missing-target diagnostic for local choice/jump/call syntax.
      nodes.push(node);
      continue;
    }

    let resolvedTarget: string;
    if (authoredTarget === undefined) {
      resolvedTarget = sceneEntryTarget(targetSceneId);
    } else {
      if (!targetScene.labels.has(authoredTarget)) {
        diagnostics.push(issue(
          "PXSTORY1305",
          `Story scene ${targetSceneId} does not define label ${authoredTarget}`,
          flow.target?.span ?? node.span,
          `${targetSceneId}:${authoredTarget}`,
        ));
        externalTargets.add(sceneLabelTarget(targetSceneId, authoredTarget));
      }
      resolvedTarget = sceneLabelTarget(targetSceneId, authoredTarget);
    }

    if (targetSceneId !== scene.descriptor.id) externalTargets.add(resolvedTarget);
    nodes.push(replaceFlowTarget(node, flow, resolvedTarget));
  }

  // Scenes are compiled into one Program. An implicit boundary prevents a
  // scene that omits an explicit jump/end/return from falling through into
  // the next scene merely because of bundle order.
  nodes.push(syntheticEnd(scene.descriptor.id, lastSpan));

  const stubSourceIds = new Set<string>();
  for (const target of externalTargets) {
    const stub = syntheticLabel(scene.descriptor.id, target, "external-stub", lastSpan);
    stubSourceIds.add(stub.id);
    nodes.push(stub);
  }

  return {
    document: {path: scene.path, nodes, diagnostics},
    stubSourceIds,
  };
}

function parseIndexedScene(source: string, sourcePath: string,
  mapSource: string | undefined): {document: StoryDocument; labels: ReadonlySet<string>} {
  let document = parseStory(source, sourcePath);
  const mapPath = `${sourcePath}.pxids.json`;
  if (mapSource === undefined && storyRequiresIdentityMap(source)) {
    document = {...document, diagnostics: [...document.diagnostics,
      issue("PXSTORY1411", "Story requires its external identity map", mapPath)]};
  } else if (mapSource !== undefined) {
    try { document = applyStoryIdentityMap(document, JSON.parse(mapSource) as unknown, mapPath); }
    catch (error) {
      document = {...document, diagnostics: [...document.diagnostics,
        issue("PXSTORY1410", "Story identity map is not valid JSON", mapPath, String(error))]};
    }
  }
  const labels = new Set(document.nodes
    .filter((node) => node.kind === "label" && node.name !== undefined)
    .map((node) => node.name as string));
  return {document, labels};
}

function compileStoryProjectInternal(context: StoryProjectCompileContext,
  sceneCache?: SceneCompilationCache): StoryCompileResult {
  const diagnostics: AuthoringDiagnostic[] = [];
  const indexPath = context.storyIndexPath ?? "Story/story.pxindex";
  let indexSpans: ReadonlyMap<string, SourceSpan> | undefined;
  const indexedSpan = (pointer: string): SourceSpan | undefined =>
    (indexSpans ??= storyIndexSpans(context.storyIndexSource,
      context.storyIndex, indexPath)).get(pointer);
  const indexLocation = (pointer: string): string | SourceSpan =>
    indexedSpan(pointer) ?? indexPath;
  const documentId = context.documentId ?? context.storyIndex.id;
  const scenesById = new Map<string, ParsedScene>();
  const seenSceneIds = new Set<string>();
  const reportParseOnDiscovery = sceneCache === undefined ||
    !sceneCache.hasEntries(context.locale);

  for (const [index, descriptor] of context.storyIndex.scenes.entries()) {
    let cached = false;
    try {
      if (seenSceneIds.has(descriptor.id)) {
        diagnostics.push(issue("PXSTORY1301", `Duplicate Story scene id: ${descriptor.id}`,
          indexLocation(`/scenes/${index}/id`), descriptor.id));
        continue;
      }
      seenSceneIds.add(descriptor.id);

      const sourcePath = descriptor.sources[context.locale];
      if (sourcePath === undefined) {
        diagnostics.push(issue("PXSTORY1302", `Story scene ${descriptor.id} has no ${context.locale} source`,
          indexLocation(`/scenes/${index}/sources`), descriptor.id));
        continue;
      }
      const source = context.sourceFiles[sourcePath];
      if (source === undefined) {
        diagnostics.push(issue("PXSTORY1303", `Story source is unavailable: ${sourcePath}`,
          indexedSpan(`/scenes/${index}/sources/${context.locale.replaceAll("~", "~0").replaceAll("/", "~1")}`)
            ?? sourcePath, descriptor.id));
        continue;
      }
      const mapSource = context.sourceFiles[`${sourcePath}.pxids.json`];
      const labels = sceneCache?.labels(context.locale, descriptor.id,
        sourcePath, source, mapSource);
      if (labels !== undefined) {
        scenesById.set(descriptor.id, {descriptor, path: sourcePath,
          source, identityMap: mapSource, labels});
        cached = true;
      } else {
        scenesById.set(descriptor.id, {descriptor, path: sourcePath,
          source, identityMap: mapSource,
          ...parseIndexedScene(source, sourcePath, mapSource)});
      }
    } finally {
      if (reportParseOnDiscovery)
        context.onProgress?.({phase: "parse", locale: context.locale,
          sceneId: descriptor.id, ...(descriptor.sources[context.locale] === undefined ? {}
            : {sourcePath: descriptor.sources[context.locale]}),
          current: index + 1, total: context.storyIndex.scenes.length,
          ...(cached ? {cached: true} : {})});
    }
  }

  const entryScene = scenesById.get(context.storyIndex.entryScene);
  if (entryScene === undefined) {
    diagnostics.push(issue("PXSTORY1306", `Story entryScene is unavailable: ${context.storyIndex.entryScene}`,
      indexLocation("/entryScene")));
  }

  const operations: RuntimeIrOperation[] = [];
  const mappings: RuntimeSourceMap["mappings"][number][] = [];
  const resourceDependencies = new Set<string>();
  const resourceLocations = new Map<string, SourceSpan>();
  const operationOwners = new Map<string, {sceneId: string; path: string; line: number}>();
  const firstLabelOwner = new Set<string>();
  const duplicateLabels = new Set<string>();
  for (const scene of scenesById.values())
    for (const label of scene.labels)
      if (firstLabelOwner.has(label)) duplicateLabels.add(label);
      else firstLabelOwner.add(label);
  const sharedSnapshot = sceneCache === undefined ? undefined : {
    compileInputs: JSON.stringify([
      documentId, context.locale, context.characters, context.game,
      context.extensions, context.resources, context.resourceAssets,
      context.storyAssets?.fingerprint, context.storyMacros?.fingerprint,
      context.portraits?.fingerprint,
    ]),
    labels: JSON.stringify([...scenesById].map(([id, scene]) =>
      [id, [...scene.labels].sort()])),
  };
  const sceneSnapshot = (scene: ParsedScene): SceneCompileSnapshot => ({
    sourcePath: scene.path,
    source: scene.source,
    identityMap: scene.identityMap,
    descriptor: JSON.stringify([scene.descriptor.id,
      scene.descriptor.identityNamespace, scene.descriptor.requiredLabels]),
    ...sharedSnapshot!,
  });

  if (!reportParseOnDiscovery) {
    // Cached labels let us resolve all cross-scene targets first. If any
    // shared input or target label changed, materialize only the documents
    // that need recompilation before reporting the parse phase.
    for (const [index, descriptor] of context.storyIndex.scenes.entries()) {
      let cached = false;
      try {
        const scene = scenesById.get(descriptor.id);
        if (scene === undefined || scene.descriptor !== descriptor) continue;
        if (scene.document === undefined &&
            sceneCache?.compiled(context.locale, descriptor.id,
              sceneSnapshot(scene)) === undefined) {
          scenesById.set(descriptor.id, {...scene,
            ...parseIndexedScene(scene.source, scene.path, scene.identityMap)});
        } else {
          cached = scene.document === undefined;
        }
      } finally {
        context.onProgress?.({phase: "parse", locale: context.locale,
          sceneId: descriptor.id, ...(descriptor.sources[context.locale] === undefined ? {}
            : {sourcePath: descriptor.sources[context.locale]}),
          current: index + 1, total: context.storyIndex.scenes.length,
          ...(cached ? {cached: true} : {})});
      }
    }
  }

  for (const [index, descriptor] of context.storyIndex.scenes.entries()) {
    let cached = false;
    try {
      const scene = scenesById.get(descriptor.id);
      if (scene === undefined || scene.descriptor !== descriptor) continue;
      const sceneDiagnostics: AuthoringDiagnostic[] = [];
      for (const [labelIndex, requiredLabel] of (descriptor.requiredLabels ?? []).entries()) {
        if (!scene.labels.has(requiredLabel)) {
          sceneDiagnostics.push(issue(
            "PXSTORY1307",
            `Story scene ${descriptor.id} is missing required label ${requiredLabel}`,
            indexedSpan(`/scenes/${index}/requiredLabels/${labelIndex}`)
              ?? scene.path,
            requiredLabel,
          ));
        }
      }

      const snapshot = sceneCache === undefined ? undefined : sceneSnapshot(scene);
      const selected = snapshot === undefined ? undefined :
        sceneCache?.compiled(context.locale, descriptor.id, snapshot);
      let compiled: StoryCompileResult;
      let stubSourceIds: ReadonlySet<string>;
      if (selected !== undefined) {
        compiled = selected.result;
        stubSourceIds = selected.stubSourceIds;
        cached = true;
      } else {
        // The second parse pass materializes documents with cache misses.
        // An uncached direct compile has already parsed every scene.
        const document = scene.document;
        if (document === undefined) throw new Error(`Story scene was not parsed: ${descriptor.id}`);
        const rewritten = rewriteScene(scene, document, scenesById,
          duplicateLabels, sceneDiagnostics);
        compiled = compileStory(rewritten.document, {
          documentId: descriptor.identityNamespace ?? `${documentId}:${descriptor.id}`,
          locale: context.locale,
          ...(context.committedRevision === undefined ? {} : {committedRevision: context.committedRevision}),
          ...(context.characters === undefined ? {} : {characters: context.characters}),
          ...(context.game === undefined ? {} : {game: context.game}),
          ...(context.extensions === undefined ? {} : {extensions: context.extensions}),
          ...(context.resources === undefined ? {} : {resources: context.resources}),
          ...(context.resourceAssets === undefined ? {} : {resourceAssets: context.resourceAssets}),
          ...(context.storyAssets === undefined ? {} : {storyAssets: context.storyAssets}),
          ...(context.storyMacros === undefined ? {} : {storyMacros: context.storyMacros}),
          ...(context.portraits === undefined ? {} : {portraits: context.portraits}),
        });
        stubSourceIds = rewritten.stubSourceIds;
        if (snapshot !== undefined) sceneCache?.remember(context.locale, descriptor.id,
          snapshot, scene.labels, stubSourceIds, compiled);
      }
      diagnostics.push(...compiled.diagnostics);
      for (const dependency of compiled.resourceDependencies ?? [])
        resourceDependencies.add(dependency);
      for (const location of compiled.resourceLocations ?? [])
        if (!resourceLocations.has(location.source))
          resourceLocations.set(location.source, location.span);
      if (!compiled.valid || compiled.runtimeIr === undefined || compiled.sourceMap === undefined) continue;

      // Do not spread a commercial-size scene into Array.push: V8 treats the
      // elements as call arguments and overflows its argument stack around the
      // same scale that our Runtime IR contract explicitly supports.
      for (const operation of compiled.runtimeIr.operations) {
        if (stubSourceIds.has(operation.sourceId)) continue;
        const owner = operationOwners.get(operation.operationId);
        if (owner !== undefined) {
          const mapping = compiled.sourceMap.mappings.find((candidate) =>
            candidate.operationId === operation.operationId);
          const location = mapping !== undefined && mapping.sourceUri === scene.path
            ? mappedSpan(scene.source, mapping) : scene.path;
          diagnostics.push(issue("PXSTORY1308",
            `Duplicate Story operation identity shared by ${owner.sceneId} and ${descriptor.id}`,
            location, `${operation.operationId}; first at ${owner.path}:${owner.line}`));
        }
        else operationOwners.set(operation.operationId, {sceneId: descriptor.id,
          path: scene.path, line: operation.sourceLine});
        operations.push(operation);
      }
      for (const mapping of compiled.sourceMap.mappings) {
        if (!stubSourceIds.has(mapping.sourceId)) mappings.push(mapping);
      }
    } finally {
      context.onProgress?.({phase: "compile", locale: context.locale,
        sceneId: descriptor.id, ...(descriptor.sources[context.locale] === undefined ? {}
          : {sourcePath: descriptor.sources[context.locale]}),
        current: index + 1, total: context.storyIndex.scenes.length,
        ...(cached ? {cached: true} : {})});
    }
  }

  if (diagnostics.some((diagnostic) => diagnostic.severity === "error") || entryScene === undefined) {
    return {resourceDependencies: [...resourceDependencies].sort(),
      resourceLocations: [...resourceLocations].map(([source, span]) => ({source, span})),
      diagnostics, valid: false};
  }

  const bootstrapSourceId = `story-project-${stableId(`${documentId}:bootstrap`)}`;
  const bootstrapOperationId = `op-${stableId(`${documentId}:bootstrap:jump`)}`;
  const bootstrapTarget = sceneEntryTarget(context.storyIndex.entryScene);
  const bootstrapOperation: RuntimeIrOperation = {
    operationId: bootstrapOperationId,
    sourceId: bootstrapSourceId,
    sourceLine: 1,
    kind: "jump",
    text: "Story entry scene",
    arguments: {target: bootstrapTarget},
  };
  const bootstrapMapping: RuntimeSourceMap["mappings"][number] = {
    operationId: bootstrapOperationId,
    sourceId: bootstrapSourceId,
    sourceUri: indexPath,
    startLine: 1,
    startColumn: 1,
    endLine: 1,
    endColumn: 1,
  };

  return {
    runtimeIr: {
      format: "PrismatiXRuntimeIR",
      schemaRevision: 1,
      documentId,
      committedRevision: context.committedRevision ?? 0,
      operations: [bootstrapOperation, ...operations],
    },
    sourceMap: {
      format: "PrismatiXSourceMap",
      schemaRevision: 1,
      documentId,
      mappings: [bootstrapMapping, ...mappings],
    },
    resourceDependencies: [...resourceDependencies].sort(),
    resourceLocations: [...resourceLocations].map(([source, span]) => ({source, span})),
    diagnostics,
    valid: true,
  };
}

export function compileStoryProject(context: StoryProjectCompileContext): StoryCompileResult {
  return compileStoryProjectInternal(context);
}
