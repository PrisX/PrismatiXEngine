import {compileStory, parseStory} from "./story.js";
import type {
  AuthoringDiagnostic,
  CharacterDocument,
  ExtensionManifest,
  GameDocument,
  RuntimeIrOperation,
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

export interface StoryProjectCompileContext {
  readonly storyIndex: StoryIndexDocument;
  readonly locale: string;
  // Source text keyed by the exact locale-specific path stored in story.pxindex.
  // Keeping IO outside the compiler makes the same API usable from Node, a
  // browser Editor, tests, and future non-filesystem project providers.
  readonly sourceFiles: Readonly<Record<string, string>>;
  readonly storyIndexPath?: string;
  readonly documentId?: string;
  readonly committedRevision?: number;
  readonly characters?: readonly CharacterDocument[];
  readonly game?: GameDocument;
  readonly extensions?: readonly ExtensionManifest[];
  readonly resources?: Readonly<Record<string, string>>;
}

interface ParsedScene {
  readonly descriptor: StoryIndexScene;
  readonly path: string;
  readonly document: StoryDocument;
  readonly labels: ReadonlySet<string>;
}

interface RewrittenScene {
  readonly document: StoryDocument;
  readonly stubSourceIds: ReadonlySet<string>;
}

function issue(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details}),
  };
}

function stableId(input: string): string {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(input)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 0x100000001b3n);
  }
  return hash.toString(16).padStart(16, "0");
}

function fallbackSpan(path: string): SourceSpan {
  return {
    path,
    start: {line: 1, column: 1, offset: 0},
    end: {line: 1, column: 1, offset: 0},
  };
}

// These names deliberately remain valid .pxstory identifiers. The authoring
// source never sees them; they exist only while bundling locale-specific scene
// files into the single Runtime IR program consumed by the native Runtime.
function sceneEntryTarget(sceneId: string): string {
  return `pxscene.${sceneId}.entry`;
}

function sceneLabelTarget(sceneId: string, label: string): string {
  return `pxscene.${sceneId}.label.${label}`;
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
  scenesById: ReadonlyMap<string, ParsedScene>,
  extraDiagnostics: readonly AuthoringDiagnostic[],
): RewrittenScene {
  const diagnostics: AuthoringDiagnostic[] = [...scene.document.diagnostics, ...extraDiagnostics];
  const externalTargets = new Set<string>();
  const firstSpan = scene.document.nodes[0]?.span ?? fallbackSpan(scene.path);
  const lastSpan = scene.document.nodes.at(-1)?.span ?? firstSpan;
  const nodes: StoryNode[] = [
    syntheticLabel(scene.descriptor.id, sceneEntryTarget(scene.descriptor.id), "entry", firstSpan),
  ];

  for (const node of scene.document.nodes) {
    if (node.kind === "label" && node.name !== undefined) {
      nodes.push({...node, name: sceneLabelTarget(scene.descriptor.id, node.name)});
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
      diagnostics.push(issue("PXSTORY1304", `Unknown Story scene: ${targetSceneId}`, node.span.path, targetSceneId));
      nodes.push(node);
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
          node.span.path,
          `${targetSceneId}:${authoredTarget}`,
        ));
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

export function compileStoryProject(context: StoryProjectCompileContext): StoryCompileResult {
  const diagnostics: AuthoringDiagnostic[] = [];
  const indexPath = context.storyIndexPath ?? "Story/story.pxindex";
  const documentId = context.documentId ?? context.storyIndex.id;
  const scenesById = new Map<string, ParsedScene>();

  for (const descriptor of context.storyIndex.scenes) {
    if (scenesById.has(descriptor.id)) {
      diagnostics.push(issue("PXSTORY1301", `Duplicate Story scene id: ${descriptor.id}`, indexPath, descriptor.id));
      continue;
    }
    const sourcePath = descriptor.sources[context.locale];
    if (sourcePath === undefined) {
      diagnostics.push(issue("PXSTORY1302", `Story scene ${descriptor.id} has no ${context.locale} source`, indexPath, descriptor.id));
      continue;
    }
    const source = context.sourceFiles[sourcePath];
    if (source === undefined) {
      diagnostics.push(issue("PXSTORY1303", `Story source is unavailable: ${sourcePath}`, sourcePath, descriptor.id));
      continue;
    }
    const document = parseStory(source, sourcePath);
    const labels = new Set(
      document.nodes
        .filter((node) => node.kind === "label" && node.name !== undefined)
        .map((node) => node.name as string),
    );
    scenesById.set(descriptor.id, {descriptor, path: sourcePath, document, labels});
  }

  const entryScene = scenesById.get(context.storyIndex.entryScene);
  if (entryScene === undefined) {
    diagnostics.push(issue("PXSTORY1306", `Story entryScene is unavailable: ${context.storyIndex.entryScene}`, indexPath));
  }

  const operations: RuntimeIrOperation[] = [];
  const mappings: RuntimeSourceMap["mappings"][number][] = [];

  for (const descriptor of context.storyIndex.scenes) {
    const scene = scenesById.get(descriptor.id);
    if (scene === undefined) continue;
    const sceneDiagnostics: AuthoringDiagnostic[] = [];
    for (const requiredLabel of descriptor.requiredLabels ?? []) {
      if (!scene.labels.has(requiredLabel)) {
        sceneDiagnostics.push(issue(
          "PXSTORY1307",
          `Story scene ${descriptor.id} is missing required label ${requiredLabel}`,
          scene.path,
          requiredLabel,
        ));
      }
    }

    const rewritten = rewriteScene(scene, scenesById, sceneDiagnostics);
    const compiled = compileStory(rewritten.document, {
      documentId: `${documentId}:${descriptor.id}`,
      ...(context.committedRevision === undefined ? {} : {committedRevision: context.committedRevision}),
      ...(context.characters === undefined ? {} : {characters: context.characters}),
      ...(context.game === undefined ? {} : {game: context.game}),
      ...(context.extensions === undefined ? {} : {extensions: context.extensions}),
      ...(context.resources === undefined ? {} : {resources: context.resources}),
    });
    diagnostics.push(...compiled.diagnostics);
    if (!compiled.valid || compiled.runtimeIr === undefined || compiled.sourceMap === undefined) continue;

    operations.push(...compiled.runtimeIr.operations.filter((operation) => !rewritten.stubSourceIds.has(operation.sourceId)));
    mappings.push(...compiled.sourceMap.mappings.filter((mapping) => !rewritten.stubSourceIds.has(mapping.sourceId)));
  }

  if (diagnostics.some((diagnostic) => diagnostic.severity === "error") || entryScene === undefined) {
    return {diagnostics, valid: false};
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
    diagnostics,
    valid: true,
  };
}
