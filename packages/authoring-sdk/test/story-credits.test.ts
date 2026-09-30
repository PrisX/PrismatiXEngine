import assert from "node:assert/strict";
import test from "node:test";

import {compileStory, createStoryAssetIndex, externalizeStoryIds, parseStory} from "../src/index.js";

const assets = createStoryAssetIndex({bg: ["Assets/bg"]}, ["Assets/bg/logo.png"]);

function compile(source: string) {
  return compileStory(parseStory(source, "Story/end.pxstory"),
    {documentId: "end", storyAssets: assets});
}

test("[credits] compiles its lines into one timeline operation", () => {
  const result = compile([
    "[hud visible=false]",
    "[credits duration=40s skippable=false size=36 color=#FFEEDD font=Content/Fonts/end.ttf]",
    "",
    "# シナリオ",
    "Owen Liu",
    "; a comment",
    "",
    "# 原画",
    "[image logo]",
    "[endcredits]",
    "fin",
    "",
  ].join("\n"));
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  const [, roll, after] = result.runtimeIr!.operations;
  assert.equal(roll!.kind, "timeline");
  assert.deepEqual({...roll!.arguments, items: JSON.parse(roll!.arguments.items!)}, {
    mode: "credits", duration: "40s", skippable: "false", size: "36", color: "255,238,221",
    font: "Content/Fonts/end.ttf",
    items: [{text: "シナリオ", heading: true}, {text: "Owen Liu"}, {text: ""},
      {text: "原画", heading: true}, {image: "Assets/bg/logo.png"}],
  });
  assert.equal(after!.arguments.text, "fin");
  assert.ok(result.resourceDependencies!.includes("Assets/bg/logo.png"));
  assert.ok(result.resourceDependencies!.includes("Content/Fonts/end.ttf"));
});

test("[credits] is one identity entry and reports its mistakes", () => {
  const externalized = externalizeStoryIds("[credits]\nA\nB\n[endcredits]\nfin\n");
  assert.equal(externalized.valid, true, JSON.stringify(externalized.diagnostics));
  assert.deepEqual(externalized.map!.entries.map(({kind, command}) => command ?? kind),
    ["credits", "narration"]);
  const codes = (source: string) => {
    const document = parseStory(source, "Story/end.pxstory");
    return compileStory(document, {documentId: "end", storyAssets: assets}).diagnostics.map(({code}) => code);
  };
  assert.deepEqual(codes("[credits]\nA\n"), ["PXSTORY1270"]);
  assert.deepEqual(codes("[endcredits]\n"), ["PXSTORY1270"]);
  assert.deepEqual(codes("[credits duration=500ms]\nA\n[endcredits]\n"), ["PXSTORY1271"]);
  assert.deepEqual(codes("[credits skippable=maybe]\nA\n[endcredits]\n"), ["PXSTORY1271"]);
  assert.deepEqual(codes("[credits size=2]\nA\n[endcredits]\n"), ["PXSTORY1271"]);
  assert.deepEqual(codes("[credits color=red]\nA\n[endcredits]\n"), ["PXSTORY1271"]);
  assert.deepEqual(codes("[credits]\n\n[endcredits]\n"), ["PXSTORY1272"]);
  assert.deepEqual(codes("[credits]\n[bg x]\n[endcredits]\n"), ["PXSTORY1272"]);
  assert.deepEqual(codes("[credits]\n[image missing]\n[endcredits]\n"), ["PXSTORY1160"]);
  assert.deepEqual(codes("[credits font=../x.ttf]\nA\n[endcredits]\n"), ["PXSTORY1183"]);
});
