import assert from "node:assert/strict";
import {mkdtemp, readdir, readFile, rm} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {displayWidth} from "../src/tui.js";
import {driveCli, finalScreen, keys} from "./interactive-driver.js";

test("display width counts CJK as two columns and ignores colour codes", () => {
  assert.equal(displayWidth("abc"), 3);
  assert.equal(displayWidth("星燈"), 4);
  assert.equal(displayWidth("\x1b[32m◇\x1b[39m  ok"), 5);
});

test("create asks for what it needs, creates the project and shows the next steps", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-wizard-"));
  try {
    // Title, default folder, minimal template, 1920x1080, English (the
    // initial choice for an English system), then "not now" for Preview.
    const run = await driveCli(["create", "--package-manager", "npm"],
      ["Star Lantern", keys.enter, keys.enter, keys.enter, keys.down, keys.enter, keys.enter, "n"],
      {cwd: root, env: {npm_config_offline: "true"}});
    const screen = finalScreen(run.stderr);
    assert.equal(run.code, 0, screen);
    assert.match(screen, /◇  What is your game called\?\n│  Star Lantern/u);
    assert.match(screen, /◇  Resolution\n│  1920 × 1080/u);
    assert.match(screen, /Created star-lantern/u);
    assert.match(screen, /cd star-lantern/u);
    assert.match(screen, /npm run dev/u);
    assert.match(screen, /◇  Open the preview now\?\n│  Not now/u);
    assert.equal(run.stdout, "", "interactive output stays on stderr");
    const manifest = JSON.parse(await readFile(join(root, "star-lantern", "prismatix.json"), "utf8"));
    assert.equal(manifest.name, "Star Lantern");
    assert.equal(manifest.defaultLocale, "en");
    assert.deepEqual(manifest.resolution, {width: 1920, height: 1080});

    // The menu inside the project runs the chosen command: "Check the project".
    const menu = await driveCli([], [keys.down, keys.enter], {cwd: join(root, "star-lantern")});
    const menuScreen = finalScreen(menu.stderr);
    assert.equal(menu.code, 0, menuScreen);
    assert.match(menuScreen, /Star Lantern/u);
    assert.match(menuScreen, /◇  What would you like to do\?\n│  Check the project/u);
    assert.match(menu.stdout + menu.stderr, /"valid":true|valid/u);
  } finally { await rm(root, {recursive: true, force: true, maxRetries: 5, retryDelay: 100}); }
});

test("cancelling the wizard creates nothing", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-wizard-cancel-"));
  try {
    const run = await driveCli(["create"], ["Half", keys.ctrlC], {cwd: root});
    const screen = finalScreen(run.stderr);
    assert.equal(run.code, 130, screen);
    assert.match(screen, /■  What is your game called\?/u);
    assert.match(screen, /Cancelled; nothing was created\./u);
    assert.deepEqual(await readdir(root), []);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("a typed value is validated before the wizard moves on", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-wizard-validate-"));
  try {
    // A custom size that is not WxH is refused with a hint; Esc then cancels.
    const run = await driveCli(["create", "--name", "Kept"],
      [keys.enter, keys.enter, keys.down, keys.down, keys.enter, "abc", keys.enter, keys.escape], {cwd: root});
    const screen = finalScreen(run.stderr);
    assert.equal(run.code, 130, screen);
    assert.match(run.stderr, /For example 1600x900/u);
    assert.match(screen, /■  Width × height in pixels/u);
    assert.doesNotMatch(screen, /What is your game called/u, "--name is not asked again");
    assert.deepEqual(await readdir(root), []);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("outside a project the menu offers create and help", async () => {
  const root = await mkdtemp(join(tmpdir(), "px-menu-"));
  try {
    const run = await driveCli([], [keys.down, keys.enter], {cwd: root});
    const screen = finalScreen(run.stderr);
    assert.equal(run.code, 0, screen);
    assert.match(screen, /This folder is not a PrismatiX project\./u);
    assert.match(screen, /Usage: prismatix/u);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("without a terminal the CLI never prompts", async () => {
  const {spawnSync} = await import("node:child_process");
  const {fileURLToPath} = await import("node:url");
  const cli = fileURLToPath(new URL("../src/cli.js", import.meta.url));
  const run = spawnSync(process.execPath, [cli], {encoding: "utf8", input: ""});
  assert.equal(run.status, 0);
  assert.match(run.stderr, /^Usage: prismatix/u);
});
