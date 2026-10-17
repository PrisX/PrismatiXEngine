// `prismatix run --at`: brings a running QA Player to a Story position the
// way a player gets there. Jumping to a label would restart the story with an
// empty stage, so the game starts at the story entry and clicks through
// (several lines per frame, text shown at once), choosing the options that
// lead to the target, until the line is current. Background, characters,
// expressions and music are then what playing there produces.

import type {BuildArtifact} from "@prismatix/authoring-sdk";

import {PlayerRequestError, type PlayerState, type PrismatixTestClient, type WaitCondition} from "./qa-client.js";
import {previewChoicePaths, resolvePreviewTarget, storyLandingOperations} from "./preview-target.js";

export type StoryDriver = Pick<PrismatixTestClient, "state" | "jump" | "advanceUntil" | "choose" | "waitUntil">;

export interface StoryPositionOutcome {
  readonly reached: boolean;
  readonly sourceUri: string;
  readonly line: number;
  readonly operationIndex: number;
  /** The options chosen on the way (indices from 0), when reached. */
  readonly branchPath?: readonly number[];
  /** How many choice paths were played. */
  readonly attempts: number;
  /** The Player's state where the search ended. */
  readonly state?: PlayerState;
  /** Why the position was not reached. */
  readonly reason?: string;
}

export interface StoryPositionOptions {
  /** Most choice paths to play before giving up (default 8). */
  readonly maxPaths?: number;
  /** Per-path time limit for clicking through (default 120 s). */
  readonly timeoutMs?: number;
}

export async function reachStoryPosition(player: StoryDriver, artifact: BuildArtifact, at: string,
  options: StoryPositionOptions = {}): Promise<StoryPositionOutcome> {
  const target = resolvePreviewTarget(artifact, at);
  const base = {sourceUri: target.sourceUri, line: target.line, operationIndex: target.operationIndex};
  const paths = previewChoicePaths(artifact, target.operationIndex);
  if (paths.length === 0)
    return {...base, reached: false, attempts: 0,
      reason: "no path from the start of the story reaches this line"};
  const landing = storyLandingOperations(artifact, target.operationIndex);
  const condition: WaitCondition = {operation: landing.operationIds, ...(landing.choice ? {choices: true} : {})};
  await player.waitUntil((state) => state.appState === "title" || state.appState === "game",
    {timeoutMs: 60_000, message: "the game did not reach its title screen"});
  let attempts = 0;
  let reason = "";
  let last: PlayerState | undefined;
  for (const path of paths.slice(0, options.maxPaths ?? 8)) {
    attempts += 1;
    await player.jump(artifact.project.entry.story);
    let consumed = 0;
    for (;;) {
      try {
        const state = await player.advanceUntil(condition, {timeoutMs: options.timeoutMs ?? 120_000,
          maxLines: 1_000_000});
        return {...base, reached: true, branchPath: path.slice(0, consumed), attempts, state};
      } catch (error) {
        if (!(error instanceof PlayerRequestError)) throw error;
        last = await player.state();
        const choice = path[consumed];
        if (last.choices.length > 0 && choice !== undefined && choice < last.choices.length) {
          await player.choose(choice);
          consumed += 1;
          continue;
        }
        reason = error.message;
        break;
      }
    }
  }
  return {...base, reached: false, attempts, reason, ...(last === undefined ? {} : {state: last})};
}
