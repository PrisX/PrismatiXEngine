import assert from "node:assert/strict";
import test from "node:test";

import type {BuildArtifact} from "@prismatix/authoring-sdk";
import {previewChoicePaths, resolvePreviewTarget, seekStoryTarget} from "../src/preview-target.js";

const source = "Story/zh-TW/chapter2.pxstory";
const artifact = {
  project: {storyIndex: "Story/story.pxindex", defaultLocale: "zh-TW", entry: {story: "prologue"}},
  files: {
    "Story/story.pxindex": JSON.stringify({scenes: [
      {id: "prologue", sources: {"zh-TW": "Story/zh-TW/prologue.pxstory"}},
      {id: "chapter2", sources: {"zh-TW": source}}]}),
    [source]: "*top\n第一句。\n\n*after_school\n第二句。\n第三句。\n",
  },
  runtimeIr: {operations: [
    {operationId: "p1"}, {operationId: "c1"}, {operationId: "c2"}, {operationId: "c3"},
    {operationId: "c4"}]},
  sourceMap: {mappings: [
    {operationId: "p1", sourceUri: "Story/zh-TW/prologue.pxstory", startLine: 1},
    {operationId: "c1", sourceUri: source, startLine: 1},
    {operationId: "c2", sourceUri: source, startLine: 2},
    {operationId: "c3", sourceUri: source, startLine: 4},
    {operationId: "c4", sourceUri: source, startLine: 6}]},
} as unknown as BuildArtifact;

test("dev --at resolves scenes, lines, labels and source paths to operations", () => {
  assert.deepEqual(resolvePreviewTarget(artifact, "chapter2"), {operationIndex: 1, sourceUri: source, line: 1});
  assert.equal(resolvePreviewTarget(artifact, "chapter2:5").operationIndex, 4);
  assert.deepEqual(resolvePreviewTarget(artifact, "chapter2:after_school"),
    {operationIndex: 3, sourceUri: source, line: 4});
  assert.equal(resolvePreviewTarget(artifact, `${source}:2`).operationIndex, 2);
  assert.throws(() => resolvePreviewTarget(artifact, "missing"), /no scene missing/u);
  assert.throws(() => resolvePreviewTarget(artifact, "chapter2:nowhere"), /no label \*nowhere/u);
  assert.throws(() => resolvePreviewTarget(artifact, "chapter2:99"), /at or after line 99/u);
});

// The first-light starter: a two-option [choice] whose first branch jumps
// over the second to a shared ending.
const op = (kind: string, args: Record<string, string> = {}) => ({kind, arguments: args});
const branching = {runtimeIr: {operations: [
  op("jump", {target: "entry"}), op("label", {target: "entry"}),     // 0, 1
  op("background"), op("dialogue"),                                  // 2, 3
  op("choiceOption", {target: "stars"}), op("choiceOption", {target: "story"}), // 4, 5
  op("label", {target: "stars"}), op("background"), op("dialogue"),  // 6, 7, 8
  op("jump", {target: "finish"}),                                    // 9
  op("label", {target: "story"}), op("dialogue"),                    // 10, 11
  op("condition"), op("choiceOption", {target: "deep"}),            // 12, 13
  op("choiceOption", {target: "finish"}), op("endCondition"),       // 14, 15
  op("label", {target: "finish"}), op("narration"), op("endStory"), // 16, 17, 18
  op("label", {target: "deep"}), op("dialogue"), op("endStory"),    // 19, 20, 21
]}} as unknown as BuildArtifact;

test("dev --at plans the choice paths that reach an operation", () => {
  assert.deepEqual(previewChoicePaths(branching, 3), [[]]);
  assert.deepEqual(previewChoicePaths(branching, 8), [[0]]);
  assert.deepEqual(previewChoicePaths(branching, 11), [[1]]);
  // Shared ending: either option, and the conditional choice either way.
  assert.deepEqual(previewChoicePaths(branching, 17), [[0], [1], [1, 1]]);
  assert.deepEqual(previewChoicePaths(branching, 20), [[1, 0]]);
  assert.deepEqual(previewChoicePaths(branching, 99), []);
});

test("dev --at tries planned paths until Preview accepts one", async () => {
  const requested: string[] = [];
  const accepted = await seekStoryTarget(branching, 11, async (path) => {
    requested.push(path.join(","));
    return path.join(",") === "1" ? {type: "storySeekAccepted", state: "waitingClick"}
      : {type: "error", code: "story-seek-rejected", diagnostics: [{code: "PXPREVIEW1104"}]};
  });
  assert.deepEqual(requested, ["", "1"]);
  assert.equal(accepted.accepted, true);
  assert.deepEqual(accepted.branchPath, [1]);

  // A path the plan did not foresee (a condition hides an option) is still
  // found breadth-first, and an unreachable path does not end the search.
  const fallback: string[] = [];
  const found = await seekStoryTarget(branching, 8, async (path) => {
    fallback.push(path.join(","));
    if (path.join(",") === "2") return {type: "storySeekAccepted"};
    return {type: "error", diagnostics: [{code: path.length === 0 ? "PXPREVIEW1104" : "PXPREVIEW1107"}]};
  });
  assert.deepEqual(fallback, ["", "0", "1", "2"]);
  assert.equal(found.accepted, true);

  // A failure no choice can fix is reported at once.
  const unsafe = await seekStoryTarget(branching, 8, async () =>
    ({type: "error", code: "story-seek-rejected", diagnostics: [{code: "PXPREVIEW1105", message: "unsafe"}]}));
  assert.equal(unsafe.accepted, false);
  assert.equal(unsafe.attempts, 1);
  assert.equal(
    (unsafe.response.diagnostics as {code: string}[])[0]?.code, "PXPREVIEW1105");

  const exhausted = await seekStoryTarget(branching, 8, async () =>
    ({type: "error", diagnostics: [{code: "PXPREVIEW1104"}]}), 5);
  assert.equal(exhausted.accepted, false);
  assert.equal(exhausted.attempts, 5);
});
