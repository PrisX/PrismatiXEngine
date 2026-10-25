// `prismatix run --at`: the Player opens at a Story position. It skips its
// splashes and title and replays the story there at once, with nothing drawn
// (PRISMATIX_START_AT), so background, characters, music and variables are
// what playing there produces. The replay starts at the story's start and
// takes the choice paths that lead there; a scene the story reaches only from
// a screen (a chapter menu) is replayed from its own first line instead, and
// when nothing reaches the line the scene plays from its first line.

import {readFileSync} from "node:fs";
import {join} from "node:path";

import {previewChoicePaths, resolvePreviewTarget, type StoryArtifact} from "./preview-target.js";

export interface StoryStartAttempt {
  /** The operation the replay starts at. */
  readonly from: string;
  /** The options to choose on the way (indices from 0). */
  readonly choices: readonly number[];
}

export interface StoryStartPlan {
  readonly sourceUri: string;
  readonly line: number;
  /** The operation the game opens at. */
  readonly operation: string;
  /** Tried in order; the last starts at the target's scene. */
  readonly attempts: readonly StoryStartAttempt[];
}

/** What the Player reports once it has opened (written to the plan's result file). */
export interface StoryStartOutcome {
  readonly reached: boolean;
  readonly from?: string;
  readonly choices?: readonly number[];
  readonly reason?: string;
  readonly durationMs?: number;
}

const sceneEntryPrefix = "pxscene.entry.";

export function storyStartPlan(artifact: StoryArtifact, at: string,
  options: {readonly maxPaths?: number} = {}): StoryStartPlan {
  const target = resolvePreviewTarget(artifact, at);
  const operations = artifact.runtimeIr.operations;
  const maxPaths = options.maxPaths ?? 8;
  const attempts: StoryStartAttempt[] = [];
  const add = (from: number, choices: readonly number[]): void => {
    const id = operations[from]!.operationId;
    if (!attempts.some((attempt) => attempt.from === id && attempt.choices.join(",") === choices.join(",")))
      attempts.push({from: id, choices});
  };
  for (const path of previewChoicePaths(artifact, target.operationIndex, {maxPaths})) add(0, path);
  // The target's scene starts at the nearest scene entry label before it.
  let scene = target.operationIndex;
  while (scene > 0 && !(operations[scene]!.kind === "label" &&
    operations[scene]!.arguments.target?.startsWith(sceneEntryPrefix))) scene -= 1;
  const scenePaths = previewChoicePaths(artifact, target.operationIndex, {from: scene, maxPaths: 4});
  // Even when nothing reaches the line, the game opens at the scene's first line.
  for (const path of scenePaths.length > 0 ? scenePaths : [[]]) add(scene, path);
  return {sourceUri: target.sourceUri, line: target.line, operation: operations[target.operationIndex]!.operationId,
    attempts};
}

/**
 * The parts of a compiled artifact folder a Story position is resolved from,
 * for a run that starts the game without compiling it again. Story sources
 * and the story index are read when asked for.
 */
export function readStoryArtifact(root: string): StoryArtifact {
  const json = (path: string): unknown => JSON.parse(readFileSync(join(root, path), "utf8"));
  const read = (path: string): string | undefined => {
    try { return readFileSync(join(root, path), "utf8"); } catch { return undefined; }
  };
  const files = new Proxy({} as Record<string, string>, {
    get: (_, key) => typeof key === "string" ? read(key) : undefined,
  });
  return {project: json("project.pxproject"), runtimeIr: json("Runtime/main.pxir"),
    sourceMap: json("Runtime/main.pxmap"), files} as StoryArtifact;
}
