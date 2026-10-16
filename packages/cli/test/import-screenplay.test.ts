import assert from "node:assert/strict";
import {mkdtemp, readFile, rm, writeFile, mkdir} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {compileStoryProject, type StoryIndexDocument} from "@prismatix/authoring-sdk";

import {importScreenplay, screenplayProse, screenplayScene, splitScreenplay}
  from "../src/importers/screenplay.js";

const draft = [
  "## 第一幕 教堂",
  "[text speaker=\"系統\"]",
  "九月到來。",
  "~~這一段刪掉了，",
  "整段不要。~~",
  "[text speaker=\"父親\"]",
  "「不能哭啊」。",
  "[text speaker=\"路人（遠處）\"]",
  "[哭聲]",
  "",
  "## 第二幕 信",
  "[text speaker=\"系統\"]",
  "我打開信。",
  "\"",
  "致凪：",
  "",
  "*這是第二頁。",
  "\"",
  "我把信收好。",
  "",
].join("\n");

const config = {
  speakers: {"父親": "father"}, narrators: ["系統"], entryLabel: "top",
  chapter: {id: "c1", title: "第一章"},
  scenes: [
    {id: "church", title: "教堂", chapterTitle: "第一章　教堂", start: ["[bg church]"],
      cues: [{at: "不能哭", commands: ["[show father normal center]"]},
        {after: "不能哭", commands: ["[hide father]"]}]},
    {id: "letter", end: ["[bgm.stop]"]},
  ],
};

test("a draft splits into scenes and drops cut passages", () => {
  const scenes = splitScreenplay(draft, {});
  assert.deepEqual(scenes.map((scene) => scene.heading), ["第一幕 教堂", "第二幕 信"]);
  assert.ok(!scenes[0]!.lines.some((line) => line.includes("刪掉") || line.includes("整段")));
});

test("speakers, narration, letters and staging become Story lines", () => {
  const [church, letter] = splitScreenplay(draft, config);
  const missed: string[] = [];
  assert.deepEqual(screenplayScene(church!, config.scenes[0]!, config, missed), [
    "*top", "[chapter 第一章　教堂]", "[bg church]",
    "九月到來。",
    "[show father normal center]", "@father 「不能哭啊」。", "[hide father]",
    "@\"路人\" ［哭聲］",
    "[jump scene=next]",
  ]);
  assert.deepEqual(missed, []);
  assert.deepEqual(screenplayScene(letter!, config.scenes[1]!, config), [
    "*top", "我打開信。", "[nvl]", "致凪：", "[nvl.clear]", "[i]這是第二頁。[/i]", "[adv]",
    "我把信收好。", "[bgm.stop]", "[jump scene=next]",
  ]);
  assert.equal(screenplayProse("* a bullet"), "＊ a bullet");
  assert.equal(screenplayProse("@me {x} ends \\"), "＠me ｛x｝ ends ＼");
  assert.equal(screenplayProse(";not a comment"), "；not a comment");
});

test("a speaker line pattern reads inline names", () => {
  const [scene] = splitScreenplay("# One\n凪：「你好」\n旁白：天亮了。\n", {});
  assert.deepEqual(screenplayScene(scene!, {}, {speakerLine: "^(\\S{1,8})：(.+)$",
    speakers: {"凪": "nagi"}, narrators: ["旁白"]}), ["@nagi 「你好」", "天亮了。", "[jump scene=next]"]);
});

test("import screenplay writes scenes, sidecars and the index; a re-import keeps identities", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-screenplay-"));
  try {
    await writeFile(join(root, "prismatix.json"), JSON.stringify({name: "Game", id: "game",
      defaultLocale: "zh-TW", storyIndex: "Story/story.pxindex"}));
    await writeFile(join(root, "screenplay-import.json"), JSON.stringify(config));
    await mkdir(join(root, "docs"));
    await writeFile(join(root, "docs/draft.md"), draft);
    const first = await importScreenplay({draft: join(root, "docs/draft.md"), project: root});
    assert.equal(first.scenes, 2);
    assert.equal(first.storyFiles, 5);
    const index = JSON.parse(await readFile(join(root, "Story/story.pxindex"), "utf8")) as StoryIndexDocument;
    assert.deepEqual(index.chapters, [{id: "c1", title: "第一章", scenes: ["church", "letter"]}]);
    assert.equal(index.entryScene, "church");
    const source = await readFile(join(root, "Story/zh-TW/church.pxstory"), "utf8");
    assert.match(source, /^; @prismatix-ids\n; 第一幕 教堂 — imported from docs\/draft\.md/u);
    const sourceFiles: Record<string, string> = {};
    for (const scene of ["church", "letter"]) for (const suffix of ["", ".pxids.json"])
      sourceFiles[`Story/zh-TW/${scene}.pxstory${suffix}`] =
        await readFile(join(root, `Story/zh-TW/${scene}.pxstory${suffix}`), "utf8");
    const compiled = compileStoryProject({storyIndex: index, locale: "zh-TW", sourceFiles,
      characters: [{format: "PrismatiXCharacter", schemaRevision: 1,
        id: "11111111-1111-4111-8111-111111111111", displayName: "父親", aliases: ["father"],
        defaultExpressionId: "22222222-2222-4222-8222-222222222222",
        expressions: [{id: "22222222-2222-4222-8222-222222222222", name: "normal", aliases: ["normal"],
          assetId: "33333333-3333-4333-8333-333333333333"}]}]});
    assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics.slice(0, 3)));

    const before = JSON.parse(sourceFiles["Story/zh-TW/church.pxstory.pxids.json"]!) as
      {entries: {sourceId?: string}[]};
    // The writers add a line; the lines around it keep their identities.
    await writeFile(join(root, "docs/draft.md"), draft.replace("九月到來。", "九月到來。\n風很輕。"));
    const second = await importScreenplay({draft: join(root, "docs/draft.md"), project: root});
    assert.deepEqual(second.regeneratedIdentities, []);
    assert.equal(second.storyFilesWritten, 2);
    const after = JSON.parse(await readFile(join(root, "Story/zh-TW/church.pxstory.pxids.json"), "utf8")) as
      {entries: {sourceId?: string}[]};
    assert.equal(after.entries.length, before.entries.length + 1);
    const kept = new Set(after.entries.map((entry) => entry.sourceId));
    assert.ok(before.entries.every((entry) => kept.has(entry.sourceId)));
  } finally { await rm(root, {recursive: true, force: true}); }
});
