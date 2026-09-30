import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {createHash} from "node:crypto";
import {mkdtemp, mkdir, readFile, readdir, rm, symlink, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join, resolve} from "node:path";
import test from "node:test";
import {fileURLToPath} from "node:url";
import {createStarter} from "../src/starter.js";
const packageRoot = fileURLToPath(new URL("../../", import.meta.url));

const prepared = async (directory: string): Promise<void> => {
  const pkg = JSON.parse(await readFile(join(directory, "package.json"), "utf8"));
  assert.equal(pkg.devDependencies["@prismatix/cli"], "0.2.0");
  assert.equal(pkg.devDependencies["@prismatix/authoring-sdk"], "0.2.0");
  await writeFile(join(directory, "package-lock.json"), JSON.stringify({lockfileVersion: 3}));
  await mkdir(join(directory, "node_modules", "@prismatix"), {recursive: true});
  await symlink(packageRoot, join(directory, "node_modules", "@prismatix", "cli"), process.platform === "win32" ? "junction" : "dir");
};

test("starter prepares a compilable branch story and preserves binary font/image bytes", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-starter-"));
  try {
    const target = join(root, "繁中 story");
    const result = await createStarter(target, {name: 'My "story"', install: prepared});
    assert.equal(result.project, join(target, "prismatix.json"));
    const manifest = JSON.parse(await readFile(result.project, "utf8"));
    assert.equal(manifest.name, 'My "story"');
    assert.equal(manifest.defaultLocale, "zh-TW");
    for (const file of ["Content/Fonts/NotoSansTC-Bold.ttf", "Assets/window.png", "Assets/guide.png"]) {
      const actual = await readFile(join(target, file));
      const expected = await readFile(join(packageRoot, "templates/minimal", file));
      assert.deepEqual(actual, expected);
    }
    const font = await readFile(join(target, "Content/Fonts/NotoSansTC-Bold.ttf"));
    assert.equal(createHash("sha256").update(font).digest("hex"), "ed4ae6ed781e34d0beee896e9be5d835a938ac7adabbcc1b6b6904290006f372");
    const built = spawnSync(process.execPath, [join(packageRoot, "dist/src/cli.js"), "validate", target], {encoding: "utf8"});
    assert.equal(built.status, 0, built.stdout + built.stderr);
    const storyPath = join(target, "Story/main.pxstory");
    const story = await readFile(storyPath, "utf8");
    const identities = JSON.parse(await readFile(`${storyPath}.pxids.json`, "utf8"));
    assert.match(story, /^; @prismatix-ids\r?\n/u);
    assert.doesNotMatch(story, /\[id\s/u);
    // The starter shows the shortest forms: storyAssets names, a choice
    // block and positional jump targets.
    assert.match(story, /\[bg window\]/u);
    assert.match(story, /\[choice\]\r?\n- 一起看看星星 -> stars\r?\n/u);
    assert.match(story, /\[jump finish\]/u);
    assert.equal(identities.format, "PrismatiXStoryIdentityMap");
    assert.ok(identities.entries.length > 5);
    await writeFile(storyPath, story.replace("你好，我是小光。這是你的第一個故事。",
      "你好，我是小光。故事從這裡開始。"), "utf8");
    const revised = spawnSync(process.execPath,
      [join(packageRoot, "dist/src/cli.js"), "validate", target], {encoding: "utf8"});
    assert.equal(revised.status, 0, revised.stdout + revised.stderr);
    const rewritten = spawnSync(process.execPath,
      [join(packageRoot, "dist/src/cli.js"), "ids", target, "--sync", "--write"], {encoding: "utf8"});
    assert.equal(rewritten.status, 0, rewritten.stdout + rewritten.stderr);
    await writeFile(storyPath, (await readFile(storyPath, "utf8")).replace(
      "故事從這裡開始。", "故事從這裡開始。\n這是新增的一句。"), "utf8");
    const unsynced = spawnSync(process.execPath,
      [join(packageRoot, "dist/src/cli.js"), "validate", target], {encoding: "utf8"});
    assert.equal(unsynced.status, 1);
    assert.match(unsynced.stdout + unsynced.stderr, /PXSTORY1405/u);
    assert.match(unsynced.stdout + unsynced.stderr, /--sync/u);
    const synced = spawnSync(process.execPath,
      [join(packageRoot, "dist/src/cli.js"), "ids", target, "--sync", "--write"], {encoding: "utf8"});
    assert.equal(synced.status, 0, synced.stdout + synced.stderr);
    const expanded = spawnSync(process.execPath,
      [join(packageRoot, "dist/src/cli.js"), "validate", target], {encoding: "utf8"});
    assert.equal(expanded.status, 0, expanded.stdout + expanded.stderr);
    assert.ok(await readFile(join(target, "package-lock.json"), "utf8"));
    assert.match(await readFile(join(target, ".gitignore"), "utf8"), /node_modules\//u);
    assert.deepEqual(await readdir(root), ["繁中 story"]);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("failed preparation leaves an existing empty destination intact and removes only staging", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-starter-fail-"));
  try {
    const target = join(root, "game");
    await mkdir(target);
    await assert.rejects(createStarter(target, {install: async () => { throw new Error("registry unavailable"); }}), /registry unavailable/u);
    assert.deepEqual(await readdir(target), []);
    assert.deepEqual(await readdir(root), ["game"]);
    await writeFile(join(target, "author.txt"), "keep me");
    await assert.rejects(createStarter(target, {install: prepared}), /empty directory/u);
    assert.equal(await readFile(join(target, "author.txt"), "utf8"), "keep me");
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("a file appearing at destination during install is never removed", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-starter-race-"));
  try {
    const target = join(root, "game");
    await mkdir(target);
    await assert.rejects(createStarter(target, {install: async () => {
      await writeFile(join(target, "author.txt"), "concurrent edit");
    }}), /empty directory/u);
    assert.equal(await readFile(join(target, "author.txt"), "utf8"), "concurrent edit");
    assert.deepEqual(await readdir(root), ["game"]);
  } finally { await rm(root, {recursive: true, force: true}); }
});
