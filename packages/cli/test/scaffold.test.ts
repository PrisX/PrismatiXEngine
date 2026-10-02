import assert from "node:assert/strict";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {addCharacter, addScene} from "../src/scaffold.js";

async function project(files: Record<string, unknown>): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), "prismatix-scaffold-"));
  for (const [path, value] of Object.entries(files)) {
    await mkdir(join(root, path, ".."), {recursive: true});
    await writeFile(join(root, path), typeof value === "string" ? value : JSON.stringify(value), "utf8");
  }
  return root;
}

const json = async (root: string, path: string) =>
  JSON.parse(await readFile(join(root, path), "utf8")) as Record<string, unknown>;

test("add character registers a folder of expression images", async () => {
  const root = await project({
    "prismatix.json": {name: "Game", id: "game", characters: [
      {alias: "guide", displayName: "小光", source: "Characters/guide.pxcharacter"}]},
    "Assets/Portraits/kei/smile.png": "png", "Assets/Portraits/kei/angry.webp": "webp",
    "Assets/Portraits/kei/notes.txt": "ignored",
  });
  try {
    const result = await addCharacter(root, "kei", {from: "Assets/Portraits/kei", name: "圭"});
    assert.deepEqual(result, {created: ["Characters/kei.pxcharacter"], updated: ["prismatix.json"]});
    const character = await json(root, "Characters/kei.pxcharacter");
    assert.deepEqual(character.expressions, [
      {name: "angry", aliases: ["default"], assetPath: "Assets/Portraits/kei/angry.webp", thumbnailAssetId: null},
      {name: "smile", aliases: [], assetPath: "Assets/Portraits/kei/smile.png", thumbnailAssetId: null},
    ]);
    assert.deepEqual((await json(root, "prismatix.json")).characters, [
      {alias: "guide", displayName: "小光", source: "Characters/guide.pxcharacter"},
      {alias: "kei", displayName: "圭", source: "Characters/kei.pxcharacter"}]);
    await assert.rejects(addCharacter(root, "guide2", {from: "Assets/Portraits/kei", name: "小光"}),
      /already registers guide/u);
    await assert.rejects(addCharacter(root, "bad alias", {from: "Assets/Portraits/kei"}), /identifier/u);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("add scene writes one source per locale and lists it in a chapter", async () => {
  const root = await project({
    "prismatix.json": {name: "Game", id: "game", defaultLocale: "zh-TW",
      supportedLocales: ["zh-TW", "ja-JP"], storyIndex: "Story/story.pxindex"},
    "Story/story.pxindex": {format: "PrismatiXStoryIndex", schemaRevision: 1, id: "main",
      entryScene: "prologue", chapters: [{id: "chapter-1", title: "Chapter 1", scenes: ["prologue"]}],
      scenes: [{id: "prologue", sources: {"zh-TW": "Story/zh-TW/prologue.pxstory"}}]},
  });
  try {
    const result = await addScene(root, "chapter2", {title: "放學後"});
    assert.deepEqual(result.created, ["Story/zh-TW/chapter2.pxstory", "Story/ja-JP/chapter2.pxstory"]);
    const index = await json(root, "Story/story.pxindex");
    assert.deepEqual(index.chapters, [{id: "chapter-1", title: "Chapter 1", scenes: ["prologue", "chapter2"]}]);
    assert.deepEqual((index.scenes as unknown[]).at(-1), {id: "chapter2", title: "放學後",
      sources: {"zh-TW": "Story/zh-TW/chapter2.pxstory", "ja-JP": "Story/ja-JP/chapter2.pxstory"}});
    assert.match(await readFile(join(root, "Story/ja-JP/chapter2.pxstory"), "utf8"), /^\*top\n/u);
    await addScene(root, "epilogue", {chapter: "chapter-2"});
    assert.deepEqual((await json(root, "Story/story.pxindex")).chapters, [
      {id: "chapter-1", title: "Chapter 1", scenes: ["prologue", "chapter2"]},
      {id: "chapter-2", title: "chapter-2", scenes: ["epilogue"]}]);
    await assert.rejects(addScene(root, "chapter2", {}), /already has scene/u);
  } finally { await rm(root, {recursive: true, force: true}); }
});
