import assert from "node:assert/strict";
import {mkdir, mkdtemp, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {distributionDataRoot, finishMacAppBundle, macBundleFixups, otoolDependencies,
  otoolRpaths} from "../src/distribution-layout.js";
import {reusedArchiveInputs} from "../src/package-retain.js";

test("tools find game data in flat, engine-folder and .app distributions", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-layout-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const manifest = async (directory: string) => {
    await mkdir(join(directory, "Package"), {recursive: true});
    await writeFile(join(directory, "Package", "manifest.json"), "{}");
  };
  const flat = join(root, "flat");
  await manifest(flat);
  assert.equal(await distributionDataRoot(flat), flat);
  const bundled = join(root, "bundled");
  await mkdir(join(bundled, "Save"), {recursive: true});
  await manifest(join(bundled, "Engine"));
  await writeFile(join(bundled, "Engine", "Engine.manifest"), "<assembly/>");
  assert.equal(await distributionDataRoot(bundled), join(bundled, "Engine"));
  const mac = join(root, "mac");
  await manifest(join(mac, "Hikari.app", "Contents", "Resources"));
  assert.equal(await distributionDataRoot(mac), join(mac, "Hikari.app", "Contents", "Resources"));
  const ambiguous = join(root, "ambiguous");
  for (const name of ["One", "Two"]) {
    await manifest(join(ambiguous, name));
    await writeFile(join(ambiguous, name, `${name}.manifest`), "<assembly/>");
  }
  const parent = join(root, "parent");
  await manifest(join(parent, "My Game"));
  assert.equal(await distributionDataRoot(parent), parent, "a folder holding a flat distribution is not one");
  assert.equal(await distributionDataRoot(ambiguous), ambiguous, "two candidates are not guessed between");
  assert.equal(await distributionDataRoot(join(root, "missing")), join(root, "missing"));
});

test("reused archives map into the distribution's data folder", () => {
  const input = {uri: "Engine/Content.pdx", size: 3, fingerprint: "a".repeat(64)};
  const events = [{event: "progress", phase: "archive-write", message: "Reused unchanged Content.pdx"}];
  assert.deepEqual([...reusedArchiveInputs(events, [input], "Engine").keys()], ["Engine/Content.pdx"]);
  assert.equal(reusedArchiveInputs(events, [input]).size, 0, "a flat lookup does not cross layouts");
});

test("otool output parses into dependencies and rpaths", () => {
  assert.deepEqual(otoolDependencies([
    "/tmp/Hikari.app/Contents/MacOS/Hikari:",
    "\t/opt/homebrew/opt/ffmpeg/lib/libavcodec.62.dylib (compatibility version 62.0.0, current version 62.1.0)",
    "\t@rpath/libSDL3.0.dylib (compatibility version 3.0.0, current version 3.4.0)",
    "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)",
  ].join("\n")), ["/opt/homebrew/opt/ffmpeg/lib/libavcodec.62.dylib", "@rpath/libSDL3.0.dylib",
    "/usr/lib/libSystem.B.dylib"]);
  assert.deepEqual(otoolRpaths([
    "Load command 20", "          cmd LC_RPATH", "      cmdsize 32", "         path @loader_path (offset 12)",
    "Load command 21", "          cmd LC_RPATH", "      cmdsize 48",
    "         path @executable_path/../Frameworks (offset 12)",
  ].join("\n")), ["@loader_path", "@executable_path/../Frameworks"]);
});

test("bundled libraries are relinked through @rpath", () => {
  const exe = "/a/Hikari.app/Contents/MacOS/Hikari";
  const avcodec = "/a/Hikari.app/Contents/Frameworks/libavcodec.62.dylib";
  const avutil = "/a/Hikari.app/Contents/Frameworks/libavutil.60.dylib";
  const commands = macBundleFixups(
    {path: exe, rpaths: ["@loader_path"], dependencies: [
      "/opt/homebrew/lib/libavcodec.62.dylib", "@rpath/libavutil.60.dylib", "/usr/lib/libSystem.B.dylib"]},
    [{path: avcodec, rpaths: [], dependencies: ["/opt/homebrew/lib/libavcodec.62.dylib",
      "/opt/homebrew/lib/libavutil.60.dylib"]},
    {path: avutil, rpaths: ["@loader_path"], dependencies: ["@rpath/libavutil.60.dylib"]}]);
  assert.deepEqual(commands, [
    ["install_name_tool", "-id", "@rpath/libavcodec.62.dylib",
      "-change", "/opt/homebrew/lib/libavutil.60.dylib", "@rpath/libavutil.60.dylib",
      "-add_rpath", "@loader_path", avcodec],
    ["install_name_tool", "-id", "@rpath/libavutil.60.dylib", avutil],
    ["install_name_tool", "-change", "/opt/homebrew/lib/libavcodec.62.dylib", "@rpath/libavcodec.62.dylib",
      "-add_rpath", "@executable_path/../Frameworks", exe],
  ]);
  assert.deepEqual(macBundleFixups({path: exe, rpaths: [], dependencies: ["/usr/lib/libSystem.B.dylib"]}, []), [],
    "a bundle without libraries needs no relinking");
});

test("finishing a .app signs libraries first, then the bundle", async t => {
  const root = await mkdtemp(join(tmpdir(), "prismatix-app-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const app = join(root, "Hikari.app");
  await mkdir(join(app, "Contents", "Frameworks"), {recursive: true});
  await writeFile(join(app, "Contents", "Frameworks", "libfoo.dylib"), "");
  const calls: string[] = [];
  const warnings = await finishMacAppBundle(app, join(app, "Contents", "MacOS", "Hikari"), async (command, args) => {
    calls.push([command, ...args].join(" "));
    return {code: 0, stdout: "", stderr: ""};
  });
  assert.deepEqual(warnings, []);
  const signs = calls.filter(call => call.startsWith("codesign"));
  assert.deepEqual(signs, [
    `codesign --force --sign - --timestamp=none ${join(app, "Contents", "Frameworks", "libfoo.dylib")}`,
    `codesign --force --sign - --timestamp=none ${app}`]);
  const missing = await finishMacAppBundle(app, join(app, "Contents", "MacOS", "Hikari"), async () => {
    throw Object.assign(new Error("spawn codesign ENOENT"), {code: "ENOENT"});
  });
  assert.equal(missing.length, 2, "missing Xcode tools are warnings, not failures");
});
