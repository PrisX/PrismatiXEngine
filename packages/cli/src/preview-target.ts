import type {BuildArtifact} from "@prismatix/authoring-sdk";

/**
 * Resolves `prismatix dev --at` to the Runtime IR operation Preview seeks to.
 * Accepted forms: `<scene>`, `<scene>:<line>`, `<scene>:<label>` and
 * `<Story/.../file.pxstory>:<line>`, all in the default locale.
 */
/** The parts of a build a Story position is resolved from. */
export type StoryArtifact = Pick<BuildArtifact, "project" | "runtimeIr" | "sourceMap" | "files">;

export interface PreviewTarget {
  readonly operationIndex: number;
  readonly sourceUri: string;
  readonly line: number;
}

function sceneSource(artifact: StoryArtifact, scene: string): string {
  const text = artifact.files[artifact.project.storyIndex];
  if (text === undefined) {
    // A single-story project has no index; its only scene is the entry.
    const story = Object.keys(artifact.files).find((path) => path.endsWith(".pxstory"));
    if (story !== undefined && (scene === artifact.project.entry.story || scene === "main")) return story;
    throw new Error(`Unknown scene ${scene}`);
  }
  const index = JSON.parse(text) as {scenes?: {id: string; sources: Record<string, string>}[]};
  const entry = index.scenes?.find((candidate) => candidate.id === scene);
  if (entry === undefined) throw new Error(`The story index has no scene ${scene}`);
  const source = entry.sources[artifact.project.defaultLocale] ?? Object.values(entry.sources)[0];
  if (source === undefined) throw new Error(`Scene ${scene} has no source`);
  return source;
}

export function resolvePreviewTarget(artifact: StoryArtifact, target: string): PreviewTarget {
  const separator = target.lastIndexOf(":");
  const head = separator < 0 ? target : target.slice(0, separator);
  const tail = separator < 0 ? undefined : target.slice(separator + 1);
  const sourceUri = head.endsWith(".pxstory") ? head.replaceAll("\\", "/") : sceneSource(artifact, head);
  let line = 1;
  if (tail !== undefined && /^\d+$/u.test(tail)) line = Number(tail);
  else if (tail !== undefined && tail.length > 0) {
    const text = artifact.files[sourceUri];
    if (text === undefined) throw new Error(`${sourceUri} is not part of this build`);
    const found = text.split(/\r?\n/u).findIndex((value) =>
      new RegExp(`^\\*${tail.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&")}(?:\\s|$)`, "u").test(value));
    if (found < 0) throw new Error(`${sourceUri} has no label *${tail}`);
    line = found + 1;
  }
  const lines = new Map(artifact.sourceMap.mappings
    .filter((mapping) => mapping.sourceUri === sourceUri)
    .map((mapping) => [mapping.operationId, mapping.startLine]));
  if (lines.size === 0) throw new Error(`${sourceUri} has no compiled Story operations`);
  let best: {index: number; line: number} | undefined;
  artifact.runtimeIr.operations.forEach((operation, index) => {
    const start = lines.get(operation.operationId);
    if (start === undefined || start < line) return;
    if (best === undefined || start < best.line) best = {index, line: start};
  });
  if (best === undefined) throw new Error(`${sourceUri} has no Story operation at or after line ${line}`);
  const found = best as {index: number; line: number};
  return {operationIndex: found.index, sourceUri, line: found.line};
}

/**
 * Choice paths (option indices from the story start, or from `from`) that can reach
 * `operationIndex`, shortest first. Preview's story seek needs the path
 * explicitly when the target lies behind a [choice]. This walks the Runtime
 * IR's control flow — choice blocks, jumps, conditions (both ways) and
 * fragment calls — so it may also list paths a condition rules out at run
 * time; Preview verifies each one.
 */
export function previewChoicePaths(artifact: StoryArtifact, operationIndex: number,
  options: {readonly maxDepth?: number; readonly maxPaths?: number; readonly from?: number} = {}): number[][] {
  const operations = artifact.runtimeIr.operations;
  const maxDepth = options.maxDepth ?? 8;
  const maxPaths = options.maxPaths ?? 16;
  if (operationIndex < 0 || operationIndex >= operations.length) return [];
  const labels = new Map<string, number>();
  operations.forEach((operation, index) => {
    if (operation.kind === "label" && operation.arguments.target !== undefined &&
        !labels.has(operation.arguments.target)) labels.set(operation.arguments.target, index);
  });
  // Where a false condition (or the end of a true branch at `else`) continues.
  const conditionExit = new Map<number, number>();
  const open: {condition: number; branch: number}[] = [];
  operations.forEach((operation, index) => {
    if (operation.kind === "condition") open.push({condition: index, branch: index});
    else if (operation.kind === "else" && open.length > 0) {
      const current = open[open.length - 1]!;
      conditionExit.set(current.branch, index + 1);
      current.branch = index;
    } else if (operation.kind === "endCondition" && open.length > 0) {
      conditionExit.set(open.pop()!.branch, index + 1);
    }
  });
  interface State { readonly index: number; readonly path: readonly number[]; readonly calls: readonly number[] }
  const queue: State[] = [{index: options.from ?? 0, path: [], calls: []}];
  const seen = new Set<string>();
  const found: number[][] = [];
  const foundKeys = new Set<string>();
  while (queue.length > 0 && found.length < maxPaths && seen.size < 100_000) {
    const state = queue.shift()!;
    if (state.index < 0 || state.index >= operations.length) continue;
    const key = `${state.index}|${state.path.join(",")}|${state.calls.join(",")}`;
    if (seen.has(key)) continue;
    seen.add(key);
    if (state.index === operationIndex) {
      const pathKey = state.path.join(",");
      if (!foundKeys.has(pathKey)) { foundKeys.add(pathKey); found.push([...state.path]); }
      continue;
    }
    const operation = operations[state.index]!;
    const next = (index: number, path = state.path, calls = state.calls): void => {
      queue.push({index, path, calls});
    };
    const label = (name: string | undefined): number => name === undefined ? -1 : labels.get(name) ?? -1;
    switch (operation.kind) {
      case "choiceOption": {
        if (state.path.length >= maxDepth) break;
        // The options of one [choice] are consecutive; only the first is an entry.
        let end = state.index;
        while (end < operations.length && operations[end]!.kind === "choiceOption") end += 1;
        for (let option = state.index; option < end; option += 1)
          next(label(operations[option]!.arguments.target), [...state.path, option - state.index]);
        break;
      }
      case "jump": next(label(operation.arguments.target)); break;
      case "condition":
        next(state.index + 1);
        next(conditionExit.get(state.index) ?? state.index + 1);
        break;
      case "else": next(conditionExit.get(state.index) ?? state.index + 1); break;
      case "callFragment":
        if (state.calls.length < 16) next(label(operation.arguments.target), state.path, [...state.calls, state.index + 1]);
        break;
      case "return":
        if (state.calls.length > 0) next(state.calls[state.calls.length - 1]!, state.path, state.calls.slice(0, -1));
        break;
      case "endStory": break;
      default: next(state.index + 1);
    }
  }
  // Fewest choices first, then the earlier options.
  return found.sort((left, right) => left.length - right.length ||
    left.join(",").localeCompare(right.join(","), "en", {numeric: true}));
}

export interface StorySeekOutcome {
  readonly accepted: boolean;
  /** The choice path that reached the target, when accepted. */
  readonly branchPath?: readonly number[];
  /** Preview's last response: the acceptance, or the failure that ended the search. */
  readonly response: Readonly<Record<string, unknown>>;
  readonly attempts: number;
}

// A different choice path may still reach the target after these failures.
const retryableSeekCodes = new Set(["PXPREVIEW1103", "PXPREVIEW1104", "PXPREVIEW1107"]);

function seekFailureCodes(response: Readonly<Record<string, unknown>>): string[] {
  const codes = Array.isArray(response.diagnostics)
    ? response.diagnostics.flatMap((entry: unknown) => {
      const code = (entry as {code?: unknown} | null)?.code;
      return typeof code === "string" ? [code] : [];
    }) : [];
  return typeof response.code === "string" ? [...codes, response.code] : codes;
}

/**
 * Seeks Preview to `operationIndex`, trying choice paths when the target is
 * behind a [choice]: first the paths the Runtime IR's control flow allows,
 * then the first options breadth-first (for choices whose options a
 * condition hides). Stops at the first acceptance or at a failure no other
 * path can fix, such as an operation that is unsafe to replay.
 */
export async function seekStoryTarget(artifact: BuildArtifact, operationIndex: number,
  seek: (branchPath: readonly number[]) => Promise<Readonly<Record<string, unknown>>>,
  maxAttempts = 40): Promise<StorySeekOutcome> {
  const tried = new Set<string>();
  const planned = [[], ...previewChoicePaths(artifact, operationIndex)];
  const breadthFirst: number[][] = [[0], [1], [2], [3]];
  let response: Readonly<Record<string, unknown>> = {};
  let attempts = 0;
  const nextPath = (): number[] | undefined => {
    while (planned.length > 0) {
      const path = planned.shift()!;
      if (!tried.has(path.join(","))) return path;
    }
    while (breadthFirst.length > 0) {
      const path = breadthFirst.shift()!;
      if (path.length < 3) breadthFirst.push(...[0, 1, 2, 3].map((choice) => [...path, choice]));
      if (!tried.has(path.join(","))) return path;
    }
    return undefined;
  };
  for (let path = nextPath(); path !== undefined && attempts < maxAttempts; path = nextPath()) {
    tried.add(path.join(","));
    attempts += 1;
    response = await seek(path);
    if (response.type === "storySeekAccepted") return {accepted: true, branchPath: path, response, attempts};
    if (!seekFailureCodes(response).some((code) => retryableSeekCodes.has(code))) break;
  }
  return {accepted: false, response, attempts};
}
