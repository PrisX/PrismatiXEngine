import assert from "node:assert/strict";
import {mkdir, mkdtemp, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import type {StoryArtifact} from "../src/preview-target.js";
import {readStoryArtifact, storyStartPlan} from "../src/run-at.js";

// The first-light starter: lines 7-8, a two-option [choice], *stars
// (bg, happy line 17) and *story (talk line 22), a shared ending. A second
// scene, side.pxstory, is reached only from a screen (no jump leads there).
const main = "Story/main.pxstory";
const side = "Story/side.pxstory";
const ops: [string, string, number, Record<string, string>, string?][] = [
  ["o0", "jump", 1, {target: "pxscene.entry.main"}], ["o1", "label", 1, {target: "pxscene.entry.main"}],
  ["o2", "background", 4, {}], ["o3", "showCharacter", 5, {}],
  ["o4", "dialogue", 7, {}], ["o5", "dialogue", 8, {}],
  ["o6", "choiceOption", 11, {target: "stars"}], ["o7", "choiceOption", 12, {target: "story"}],
  ["o8", "label", 15, {target: "stars"}], ["o9", "background", 16, {}],
  ["o10", "showCharacter", 17, {}], ["o11", "dialogue", 17, {}], ["o12", "jump", 19, {target: "finish"}],
  ["o13", "label", 21, {target: "story"}], ["o14", "dialogue", 22, {}],
  ["o15", "label", 26, {target: "finish"}], ["o16", "narration", 27, {}], ["o17", "endStory", 28, {}],
  ["o18", "label", 1, {target: "pxscene.entry.side"}, side], ["o19", "background", 3, {}, side],
  ["o20", "dialogue", 5, {}, side], ["o21", "endStory", 6, {}, side],
];
const project = {storyIndex: "Story/story.pxindex", defaultLocale: "zh-TW", entry: {story: "main"}};
const runtimeIr = {operations: ops.map(([operationId, kind, , args]) => ({operationId, kind, arguments: args}))};
const sourceMap = {mappings: ops.map(([operationId, , startLine, , uri]) => ({operationId, sourceUri: uri ?? main, startLine}))};
const artifact = {project, files: {[main]: "*start\n", [side]: "\n\n\n*there\n"}, runtimeIr, sourceMap} as unknown as StoryArtifact;

test("run --at replays from the story's start along the options that lead to the line", () => {
  // Line 17 is first a character change; the replay continues to its text.
  assert.deepEqual(storyStartPlan(artifact, `${main}:17`), {sourceUri: main, line: 17, operation: "o10",
    attempts: [{from: "o0", choices: [0]}, {from: "o1", choices: [0]}]});
  assert.deepEqual(storyStartPlan(artifact, `${main}:22`).attempts,
    [{from: "o0", choices: [1]}, {from: "o1", choices: [1]}]);
  // Before any choice, no option is chosen.
  assert.deepEqual(storyStartPlan(artifact, `${main}:8`).attempts, [{from: "o0", choices: []}, {from: "o1", choices: []}]);
});

test("run --at starts a scene no jump reaches at its own first line", () => {
  assert.deepEqual(storyStartPlan(artifact, `${side}:5`), {sourceUri: side, line: 5, operation: "o20",
    attempts: [{from: "o18", choices: []}]});
});

test("run --at resolves a position from a build left on disk", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-run-at-"));
  try {
    await mkdir(join(root, "Runtime"));
    await mkdir(join(root, "Story"));
    await writeFile(join(root, "project.pxproject"), JSON.stringify(project));
    await writeFile(join(root, "Runtime", "main.pxir"), JSON.stringify(runtimeIr));
    await writeFile(join(root, "Runtime", "main.pxmap"), JSON.stringify(sourceMap));
    await writeFile(join(root, side), "\n\n\n*there\n");
    const plan = storyStartPlan(readStoryArtifact(root), `${side}:there`);
    assert.equal(plan.operation, "o20");
    assert.equal(plan.line, 5);
    assert.throws(() => storyStartPlan(readStoryArtifact(root), `${side}:nowhere`), /no label \*nowhere/u);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
