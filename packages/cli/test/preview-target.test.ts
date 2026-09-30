import assert from "node:assert/strict";
import test from "node:test";

import type {BuildArtifact} from "@prismatix/authoring-sdk";
import {resolvePreviewTarget} from "../src/preview-target.js";

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
