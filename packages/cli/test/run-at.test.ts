import assert from "node:assert/strict";
import test from "node:test";

import type {BuildArtifact} from "@prismatix/authoring-sdk";
import {PlayerRequestError, type PlayerState, type WaitCondition} from "../src/qa-client.js";
import {storyLandingOperations} from "../src/preview-target.js";
import {reachStoryPosition, type StoryDriver} from "../src/run-at.js";

// The first-light starter: lines 7-8, a two-option [choice], *stars
// (bg, happy line 17) and *story (talk line 22), a shared ending.
const source = "Story/main.pxstory";
const ops: [string, string, number, Record<string, string>][] = [
  ["o0", "jump", 1, {target: "entry"}], ["o1", "label", 1, {target: "entry"}],
  ["o2", "background", 4, {}], ["o3", "showCharacter", 5, {}],
  ["o4", "dialogue", 7, {}], ["o5", "dialogue", 8, {}],
  ["o6", "choiceOption", 11, {target: "stars"}], ["o7", "choiceOption", 12, {target: "story"}],
  ["o8", "label", 15, {target: "stars"}], ["o9", "background", 16, {}],
  ["o10", "showCharacter", 17, {}], ["o11", "dialogue", 17, {}], ["o12", "jump", 19, {target: "finish"}],
  ["o13", "label", 21, {target: "story"}], ["o14", "dialogue", 22, {}],
  ["o15", "label", 26, {target: "finish"}], ["o16", "narration", 27, {}], ["o17", "endStory", 28, {}],
];
const artifact = {
  project: {storyIndex: "Story/story.pxindex", defaultLocale: "zh-TW", entry: {story: "main"}},
  files: {[source]: "*start\n"},
  runtimeIr: {operations: ops.map(([operationId, kind, , args]) => ({operationId, kind, arguments: args}))},
  sourceMap: {mappings: ops.map(([operationId, , startLine]) => ({operationId, sourceUri: source, startLine}))},
} as unknown as BuildArtifact;

test("run --at lands where the game shows the line's text", () => {
  // Line 16 is a [bg]; the game rests on line 17's text after it.
  assert.deepEqual(storyLandingOperations(artifact, 9), {operationIds: ["o9", "o10", "o11"], choice: false});
  assert.deepEqual(storyLandingOperations(artifact, 11), {operationIds: ["o11"], choice: false});
  // A [choice] line rests on the option block.
  assert.deepEqual(storyLandingOperations(artifact, 6), {operationIds: ["o6", "o7", "o8"], choice: true});
  // A jump is followed to where the story waits next.
  assert.deepEqual(storyLandingOperations(artifact, 12), {operationIds: ["o12", "o15", "o16"], choice: false});
});

/** Plays the fixture story the way the QA Player's advanceUntil does. */
class FakePlayer implements StoryDriver {
  readonly log: string[] = [];
  private index = 0;
  private choosing = false;

  private current(): PlayerState {
    const operation = ops[this.index]!;
    return {appState: "game", choices: this.choosing ? [{index: 0, text: "a"}, {index: 1, text: "b"}] : [],
      story: {operationId: operation[0], source: {uri: source, line: operation[2]}}} as unknown as PlayerState;
  }
  private holds(condition: WaitCondition): boolean {
    const state = this.current();
    const ids = condition.operation === undefined ? undefined
      : typeof condition.operation === "string" ? [condition.operation] : condition.operation;
    return (ids === undefined || ids.includes(state.story.operationId)) &&
      (condition.choices === undefined || (state.choices.length > 0) === condition.choices);
  }
  /** Runs to the next operation the story waits at. */
  private run(): void {
    for (;;) {
      const [, kind, , args] = ops[this.index]!;
      if (kind === "jump") this.index = ops.findIndex(([, k, , a]) => k === "label" && a.target === args.target);
      else if (kind === "choiceOption") { this.choosing = true; return; }
      else if (["dialogue", "narration", "endStory"].includes(kind)) return;
      else this.index += 1;
    }
  }
  async state(): Promise<PlayerState> { return this.current(); }
  async waitUntil(predicate: (state: PlayerState) => boolean): Promise<PlayerState> {
    assert.ok(predicate({appState: "title"} as PlayerState));
    return this.current();
  }
  async jump(target: string): Promise<PlayerState> {
    this.log.push(`jump ${target}`);
    this.index = 0; this.choosing = false; this.run();
    return this.current();
  }
  async choose(index: number): Promise<{readonly text: string}> {
    this.log.push(`choose ${index}`);
    this.choosing = false;
    this.index = ops.findIndex(([, k, , a]) => k === "label" && a.target === ops[6 + index]![3].target);
    this.run();
    return {text: String(index)};
  }
  async advanceUntil(condition: WaitCondition): Promise<PlayerState & {readonly advancedLines: number}> {
    for (let lines = 0; ; lines += 1) {
      if (this.holds(condition)) return {...this.current(), advancedLines: lines};
      if (this.choosing) throw new PlayerRequestError("advanceUntil", "PXQA1013", "a choice is waiting; choose first");
      if (ops[this.index]![1] === "endStory")
        throw new PlayerRequestError("advanceUntil", "PXQA1013", "the story ended and the condition does not hold");
      this.index += 1; this.run();
    }
  }
}

test("run --at plays from the start, choosing the options that lead to the line", async () => {
  const stars = new FakePlayer();
  const reached = await reachStoryPosition(stars, artifact, "Story/main.pxstory:17");
  assert.equal(reached.reached, true);
  assert.deepEqual(reached.branchPath, [0]);
  assert.equal(reached.state?.story.operationId, "o11");
  assert.deepEqual(stars.log, ["jump main", "choose 0"]);

  const story = new FakePlayer();
  const second = await reachStoryPosition(story, artifact, "Story/main.pxstory:22");
  assert.deepEqual(second.branchPath, [1]);
  assert.deepEqual(story.log, ["jump main", "choose 1"]);

  // Before any choice, no option is chosen.
  const early = new FakePlayer();
  assert.deepEqual((await reachStoryPosition(early, artifact, "Story/main.pxstory:8")).branchPath, []);
  assert.deepEqual(early.log, ["jump main"]);
});

test("run --at reports honestly when no played path reaches the line", async () => {
  // Pretend the story's choices lead nowhere near line 22.
  const broken = new FakePlayer();
  broken.choose = async (index: number) => {
    broken.log.push(`choose ${index}`);
    (broken as unknown as {choosing: boolean}).choosing = false;
    (broken as unknown as {index: number}).index = 16;
    return {text: String(index)};
  };
  const outcome = await reachStoryPosition(broken, artifact, "Story/main.pxstory:22");
  assert.equal(outcome.reached, false);
  assert.equal(outcome.attempts, 1);
  assert.match(outcome.reason ?? "", /story ended/);
  assert.equal(outcome.line, 22);
});
