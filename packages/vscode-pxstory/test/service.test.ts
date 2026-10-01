import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {join} from "node:path";
import {test} from "node:test";

import type {CompletionItem, Hover, MarkupContent} from "vscode-languageserver";

import {validateProjectStories} from "../src/server/project-validation.js";
import {PxstoryService} from "../src/server/service.js";
import {endOf, fixtureDocument, minimalRoot, positionOf, scenesRoot, uriPath} from "./helpers.js";

const service = new PxstoryService({sdkMode: "auto"});
const intro = readFileSync(join(scenesRoot, "Story", "intro.pxstory"), "utf8");

function labels(items: readonly CompletionItem[]): string[] {
  return items.map((item) => item.label);
}

function hoverText(hover: Hover | null): string {
  assert.ok(hover !== null, "expected a hover");
  return (hover.contents as MarkupContent).value;
}

/** Completion at the end of `text`, as if the author just typed it. */
async function complete(text: string, path = "Story/intro.pxstory") {
  const document = fixtureDocument(scenesRoot, path, text);
  return service.completion(document, endOf(document));
}

test("a valid scene has no diagnostics and cross-scene jumps resolve", async () => {
  const result = await service.diagnostics(fixtureDocument(scenesRoot, "Story/intro.pxstory"));
  assert.deepEqual(result.diagnostics, []);
  const park = await service.diagnostics(fixtureDocument(scenesRoot, "Story/park.pxstory"));
  assert.deepEqual(park.diagnostics, []);
});

test("diagnostics carry SDK codes and exact spans", async () => {
  const text = [
    "*start",
    "[bg sky]",
    "[bg Assets/none.png]",
    "@nobody hi",
    "[jump scene=park target=missing]",
    "[jump scene=nope]",
    "[show guide grin]",
  ].join("\n");
  const document = fixtureDocument(scenesRoot, "Story/intro.pxstory", text);
  const {diagnostics} = await service.diagnostics(document);
  const byCode = new Map(diagnostics.map((diagnostic) => [diagnostic.code, diagnostic]));
  assert.deepEqual(byCode.get("PXSTORY1161")?.range, {start: {line: 1, character: 4}, end: {line: 1, character: 7}});
  assert.match(byCode.get("PXSTORY1161")!.message as string, /Assets\/bg\/a\/sky\.png, Assets\/bg\/b\/sky\.png/u);
  assert.deepEqual(byCode.get("PXBUILD1044")?.range.start, {line: 2, character: 4});
  assert.equal(byCode.get("PXSTORY1102")?.range.start.line, 3);
  assert.deepEqual(byCode.get("PXSTORY1305")?.range, {start: {line: 4, character: 24}, end: {line: 4, character: 31}});
  assert.equal(byCode.get("PXSTORY1304")?.range.start.line, 5);
  assert.equal(byCode.get("PXSTORY1114")?.range.start.line, 6);
  // The compiler's internal echo of a bad cross-scene target is dropped.
  assert.ok(!diagnostics.some((diagnostic) => diagnostic.code === "PXSTORY1118" || diagnostic.code === "PXSTORY1215"));
});

test("renaming a label reports jumps from other scenes against the new name", async () => {
  const renamed = intro.replace("*stay", "*home");
  const introResult = await service.diagnostics(fixtureDocument(scenesRoot, "Story/intro.pxstory", renamed));
  assert.equal(introResult.labelsChanged, true);
  const park = await service.diagnostics(fixtureDocument(scenesRoot, "Story/park.pxstory"));
  assert.deepEqual(park.diagnostics.map((diagnostic) => [diagnostic.code, diagnostic.range.start.line]),
    [["PXSTORY1305", 5]]);
  await service.diagnostics(fixtureDocument(scenesRoot, "Story/intro.pxstory", intro));
  service.closeDocument(fixtureDocument(scenesRoot, "Story/intro.pxstory").uri);
});

test("command names, macros and inline tags complete after [", async () => {
  const list = await complete(`${intro}\n[`);
  const names = labels(list.items);
  for (const expected of ["bg", "show", "bgm.stop", "choice.wait", "greet", "b", "ruby="])
    assert.ok(names.includes(expected), `missing ${expected}`);
  const bg = list.items.find((item) => item.label === "bg")!;
  assert.match(bg.detail!, /^\[bg asset=… rule\?/u);
});

test("storyAssets short names complete per kind using the SDK resolution rules", async () => {
  const bg = await complete("[bg ");
  assert.deepEqual(labels(bg.items).filter((label) => !label.endsWith("=")).sort(),
    ["a/sky", "b/sky", "night/street", "school", "street"]);
  const street = bg.items.find((item) => item.label === "night/street")!;
  assert.equal(street.detail, "Assets/bg/night/street.png");
  assert.ok(labels(bg.items).includes("rule="));
  const rule = await complete("[bg street rule=");
  assert.deepEqual(labels(rule.items), ["wipe-left"]);
  const bgm = await complete("[bgm th");
  assert.ok(labels(bgm.items).includes("theme"));
  assert.deepEqual(bgm.items.find((item) => item.label === "theme")!.textEdit,
    {range: {start: {line: 0, character: 5}, end: {line: 0, character: 7}}, newText: "theme"});
  const voice = await complete("@guide[voice=");
  assert.deepEqual(labels(voice.items), ["g001"]);
});

test("characters, display names and expressions complete", async () => {
  const speakers = await complete("@");
  assert.deepEqual(labels(speakers.items).sort(), ["guide", "naoya", "小光", "直哉"].sort());
  const show = await complete("[show ");
  assert.ok(["guide", "naoya", "直哉"].every((name) => labels(show.items).includes(name)));
  const expression = await complete("[show guide ");
  assert.ok(["smile", "sad", "left", "center", "right"].every((name) => labels(expression.items).includes(name)));
  const byDisplayName = await complete("[show 直哉 ");
  assert.ok(labels(byDisplayName.items).includes("normal"));
  const option = await complete("@guide[s");
  assert.ok(labels(option.items).includes("smile"));
  assert.ok(labels(option.items).includes("voice="));
  const resolved = await service.resolveCompletion(expression.items.find((item) => item.label === "smile")!);
  assert.match((resolved.documentation as MarkupContent).value, /guide-smile\.png\|width=240\)/u);
});

test("labels complete in the current scene and as scene:label targets", async () => {
  const local = await complete(`${intro}\n[jump target=`);
  const names = labels(local.items);
  for (const expected of ["start", "walk", "stay", "park:", "park:gate", "park:bench"])
    assert.ok(names.includes(expected), `missing ${expected}`);
  const crossScene = local.items.find((item) => item.label === "park:gate")!;
  assert.equal((crossScene.textEdit as {newText: string}).newText, "scene=park target=gate");
  const other = await complete("[jump scene=park target=");
  assert.deepEqual(labels(other.items), ["gate", "bench"]);
  const scenes = await complete("[call scene=");
  assert.deepEqual(labels(scenes.items), ["intro", "park"]);
  const choice = await complete(`${intro}\n[choice text="x" goto=w`);
  assert.ok(labels(choice.items).includes("walk"));
});

test("variables and inline tag values complete", async () => {
  const set = await complete("[set ");
  assert.ok(labels(set.items).includes("affection="));
  const tag = await complete("文字[");
  assert.ok(labels(tag.items).includes("ruby="));
});

test("go to definition finds labels, scenes, macros, characters and assets", async () => {
  const document = fixtureDocument(scenesRoot, "Story/intro.pxstory");
  const at = (needle: string, offset?: number) => service.definition(document, positionOf(document, needle, offset));
  const gate = await at("target=gate", 8);
  assert.match(uriPath(gate![0]!.uri), /story\/park\.pxstory$/u);
  assert.equal(gate![0]!.range.start.line, 0);
  const walk = await at("goto=walk", 6);
  assert.equal(walk![0]!.range.start.line, 16);
  const scene = await at("scene=park", 7);
  assert.match(uriPath(scene![0]!.uri), /story\/park\.pxstory$/u);
  const macro = await at("[greet", 2);
  assert.equal(macro![0]!.range.start.line, 8);
  const character = await at("@直哉", 1);
  assert.match(uriPath(character![0]!.uri), /characters\/naoya\.pxcharacter$/u);
  assert.equal(character![0]!.range.start.line, 3);
  const expression = await at("guide smile", 7);
  assert.match(uriPath(expression![0]!.uri), /characters\/guide\.pxcharacter$/u);
  assert.equal(expression![0]!.range.start.line, 6);
  const asset = await at("[bg street", 5);
  assert.match(uriPath(asset![0]!.uri), /assets\/bg\/street\.png$/u);
});

test("hover documents commands, previews images and describes characters", async () => {
  const document = fixtureDocument(scenesRoot, "Story/intro.pxstory");
  const at = (needle: string, offset: number) => service.hover(document, positionOf(document, needle, offset));
  assert.match(hoverText(await at("[bg street", 2)), /Changes the background/u);
  assert.match(hoverText(await at("[bg street", 5)), /Assets\/bg\/street\.png`\n\n!\[.*\]\(file:.*street\.png\|width=240\)/u);
  const guide = hoverText(await at("[show guide", 7));
  assert.match(guide, /\*\*小光\*\* — character/u);
  assert.match(guide, /Expressions \(2\): `smile`, `sad`/u);
  assert.match(hoverText(await at("guide smile", 7)), /\*\*微笑\*\*/u);
  assert.match(hoverText(await at("[greet", 2)), /\[macro greet who line\]/u);
  assert.match(hoverText(await at("target=gate", 8)), /Label `gate` in scene `park`, line 1/u);
});

test("document symbols and folding follow labels and macros", async () => {
  const document = fixtureDocument(scenesRoot, "Story/intro.pxstory");
  const symbols = await service.documentSymbols(document);
  assert.deepEqual(symbols.map((symbol) => [symbol.name, symbol.range.start.line, symbol.range.end.line]),
    [["start", 1, 15], ["greet", 8, 10], ["walk", 16, 18], ["stay", 19, 22]]);
  const folds = await service.foldingRanges(document);
  assert.ok(folds.some((fold) => fold.startLine === 1 && fold.endLine === 14));
  assert.ok(folds.some((fold) => fold.startLine === 8 && fold.endLine === 10));
});

test("the unmodified minimal template checks cleanly", async () => {
  const document = fixtureDocument(minimalRoot, "Story/main.pxstory");
  const {diagnostics} = await service.diagnostics(document);
  assert.deepEqual(diagnostics, []);
  const show = await service.completion(document, positionOf(document, "[show ", 6));
  assert.ok(labels(show.items).includes("guide"));
  const info = await service.documentInfo(document.uri);
  assert.equal(info.sceneId, "main");
  assert.equal(info.locale, "zh-TW");
  const bg = await service.completion(document, positionOf(document, "[bg ", 4));
  assert.ok(labels(bg.items).includes("Assets/window.png"));
});

test("whole-project validation reports errors in every scene", async () => {
  const clean = await validateProjectStories({root: scenesRoot, sdkMode: "auto", overlays: {}});
  assert.deepEqual(clean.diagnostics, []);
  assert.deepEqual([...clean.sources].sort(), ["Story/intro.pxstory", "Story/park.pxstory"]);
  const broken = await validateProjectStories({root: scenesRoot, sdkMode: "auto",
    overlays: {"Story/intro.pxstory": intro.replace("*stay", "*home")}});
  assert.deepEqual(broken.diagnostics.map((diagnostic) => [diagnostic.code, diagnostic.span?.path]),
    [["PXSTORY1305", "Story/intro.pxstory"], ["PXSTORY1305", "Story/park.pxstory"]]);
});

test("shared storyMacros and Story [var] declarations are known to live checks", async () => {
  const text = "*shared\n[night_bg]\n[var mood=0 scope=session type=integer]\n[set mood=1]\n[jump scene=intro target=stay]\n";
  const result = await service.diagnostics(fixtureDocument(scenesRoot, "Story/park.pxstory", text));
  assert.deepEqual(result.diagnostics.map((diagnostic) => diagnostic.code), []);
});
