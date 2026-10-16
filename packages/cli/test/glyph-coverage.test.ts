import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";

import type {BuildArtifact} from "@prismatix/authoring-sdk";

import {glyphWarnings} from "../src/glyph-coverage.js";

const fonts = new URL("../../../../Resources/Fonts/", import.meta.url);
const read = async (path: string) => {
  if (path.startsWith("Fonts/")) return readFile(new URL(path.slice(6), fonts));
  throw new Error(`no ${path}`);
};

function artifact(settings: Record<string, unknown> = {}): BuildArtifact {
  return {
    project: {settings},
    localeArtifacts: {"zh-TW": {
      runtimeIr: {operations: [
        {operationId: "op-1", kind: "dialogue", arguments: {speaker: "凪", text: "早安。"}},
      ]},
      sourceMap: {mappings: [{operationId: "op-1", sourceUri: "Story/zh-TW/01.pxstory", startLine: 3}]},
    }},
  } as unknown as BuildArtifact;
}

const scene = {format: "PrismatiXUIScene", nodes: [
  {name: "Root", kind: "group"},
  {name: "Start", kind: "button", runtimeType: "Button", text: "開始 한"},
  {name: "Name", kind: "control", runtimeType: "LineEdit", runtimeProperties: {placeholder: "名前を入力"}},
  {name: "Score", kind: "control", runtimeType: "SpriteText", runtimeProperties: {text: "한한"}},
  {name: "Calm", kind: "label", runtimeType: "Label", text: "凪",
    runtimeProperties: {fontPath: "Fonts/NotoSerifJP-Bold.ttf"}},
]};

test("PXCLI1060 checks Story, UI text and locale strings against the locale's fonts", async () => {
  const locales = {"zh-TW": {fontChain: ["Fonts/NotoSansTC-Bold.ttf"],
    strings: {"save.error": "诶，存檔失敗", "title": "標題"}}};
  const [warning, ...rest] = await glyphWarnings(artifact(), locales, read, {
    documents: {"Content/UI/Title.pxui": scene},
    nodeSpans: new Map([["Content/UI/Title.pxui", new Map([[1, {path: "Content/UI/Title.tsx",
      start: {line: 12}}]])]]),
  });
  assert.equal(rest.length, 0);
  assert.equal(warning?.code, "PXCLI1060");
  assert.match(warning!.message, /^3 character\(s\) of Story, UI or locale strings/u);
  assert.deepEqual(warning!.details!.split("\n"), [
    "凪 U+51EA first at Story/zh-TW/01.pxstory:3",
    "한 U+D55C first at Content/UI/Title.tsx:12",
    "诶 U+8BF6 first at strings.save.error",
  ]);
});

test("a node's own font and player.uiFont count before the fontChain", async () => {
  const locales = {"zh-TW": {fontChain: ["Fonts/NotoSansTC-Bold.ttf"], strings: {}}};
  const ui = {documents: {"Content/UI/Title.pxui": {format: "PrismatiXUIScene", nodes: [
    {name: "Calm", runtimeType: "Label", text: "凪", runtimeProperties: {fontPath: "Fonts/NotoSerifJP-Bold.ttf"}},
  ]}}};
  const story = (await glyphWarnings(artifact(), locales, read, ui))[0]!;
  // 凪 in the UI is drawn by its own font; the Story's 凪 still is not.
  assert.equal(story.details, "凪 U+51EA first at Story/zh-TW/01.pxstory:3");
  const uiOnly = {...ui, documents: {"Content/UI/Title.pxui": {format: "PrismatiXUIScene",
    nodes: [{name: "Calm", runtimeType: "Label", text: "凪"}]}}};
  const withUiFont = artifact({"player.uiFont": "Fonts/NotoSerifJP-Bold.ttf"});
  const covered = {...withUiFont, localeArtifacts: {"zh-TW": {
    ...withUiFont.localeArtifacts["zh-TW"]!, runtimeIr: {operations: []}}}} as unknown as BuildArtifact;
  assert.deepEqual(await glyphWarnings(covered, locales, read, uiOnly), []);
  assert.equal((await glyphWarnings({...covered, project: {settings: {}}} as unknown as BuildArtifact,
    locales, read, uiOnly))[0]?.details, "凪 U+51EA first at Content/UI/Title.pxui Calm");
});
