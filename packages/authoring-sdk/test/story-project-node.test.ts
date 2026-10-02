import assert from "node:assert/strict";
import {mkdtemp, mkdir, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join} from "node:path";
import test from "node:test";

import {compileStoryProject, externalizeStoryIds, loadStorySourceFiles,
  readBoundedUtf8File,
  type StoryIndexDocument} from "../src/index.js";

const mainPath = "Story/zh-TW/main.pxstory";
const nextPath = "Story/zh-TW/next.pxstory";
const storyIndex: StoryIndexDocument = {
  format: "PrismatiXStoryIndex", schemaRevision: 1, id: "node-loader",
  entryScene: "main",
  chapters: [{id: "chapter", title: "Chapter", scenes: ["main", "next"]}],
  scenes: [
    {id: "main", sources: {"zh-TW": mainPath}},
    {id: "next", sources: {"zh-TW": nextPath}},
  ],
};

test("Node Story source loader discovers identity sidecars without author-written ID lists", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-story-loader-"));
  try {
    const converted = externalizeStoryIds(
      "*start\nHello.\n[jump scene=next target=start]\n", {path: mainPath});
    assert.equal(converted.valid, true);
    for (const path of [mainPath, nextPath])
      await mkdir(dirname(join(root, path)), {recursive: true});
    await writeFile(join(root, mainPath), converted.source!, "utf8");
    await writeFile(join(root, `${mainPath}.pxids.json`),
      JSON.stringify(converted.map), "utf8");
    await writeFile(join(root, nextPath), "*start\nGoodbye.\n[end]\n", "utf8");

    const sourceFiles = await loadStorySourceFiles(root, storyIndex);
    assert.deepEqual(Object.keys(sourceFiles).sort(),
      [mainPath, `${mainPath}.pxids.json`, nextPath].sort());
    const compiled = compileStoryProject({storyIndex, locale: "zh-TW", sourceFiles});
    assert.equal(compiled.valid, true, JSON.stringify(compiled.diagnostics));

    await rm(join(root, `${mainPath}.pxids.json`));
    const missingMap = await loadStorySourceFiles(root, storyIndex);
    const rejected = compileStoryProject({storyIndex, locale: "zh-TW", sourceFiles: missingMap});
    assert.equal(rejected.valid, false);
    assert.ok(rejected.diagnostics.some(diagnostic => diagnostic.code === "PXSTORY1411"));
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("Node Story source loader rejects unsafe or unavailable indexed paths", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-story-loader-"));
  try {
    const unsafe: StoryIndexDocument = {...storyIndex, scenes: [
      {id: "main", sources: {"zh-TW": "../outside.pxstory"}},
    ]};
    await assert.rejects(loadStorySourceFiles(root, unsafe), /Unsafe Story source path/u);
    await assert.rejects(loadStorySourceFiles(root, storyIndex), /ENOENT/u);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("bounded UTF-8 source reading enforces the byte cap and rejects invalid encoding", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-story-loader-"));
  const path = join(root, "source.pxstory");
  try {
    await writeFile(path, "éé", "utf8");
    assert.equal(await readBoundedUtf8File(path, 4), "éé");
    await assert.rejects(readBoundedUtf8File(path, 3), /exceeds 3 bytes/u);
    await writeFile(path, Buffer.from([0xef, 0xbb, 0xbf, 0x61]));
    assert.equal(await readBoundedUtf8File(path, 4), "\uFEFFa");
    await writeFile(path, Buffer.from([0xc3, 0x28]));
    await assert.rejects(readBoundedUtf8File(path, 4), /encoded data|UTF-8/iu);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});

test("Node Story loader rejects malformed UTF-8 before compilation", async () => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-story-loader-"));
  try {
    for (const path of [mainPath, nextPath])
      await mkdir(dirname(join(root, path)), {recursive: true});
    await writeFile(join(root, mainPath), Buffer.from([0xc3, 0x28]));
    await writeFile(join(root, nextPath), "*start\n[end]\n", "utf8");
    await assert.rejects(loadStorySourceFiles(root, storyIndex), /encoded data|UTF-8/iu);
  } finally {
    await rm(root, {recursive: true, force: true});
  }
});
