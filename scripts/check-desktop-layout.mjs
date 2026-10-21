#!/usr/bin/env node
// Packs Samples/Starter with branding (a PNG icon, product name, version) and
// checks this platform's distribution layout (docs/shipping.md, Distribution
// layout): the Windows flat and bundled layouts, the macOS .app bundle
// (Info.plist, .icns, Frameworks, ad-hoc signature) or the Linux lib/ folder,
// .desktop entry and icon theme sizes (with ldd finding every library).
//
//   node scripts/check-desktop-layout.mjs --packager <PrismatiXPackager> --player <PrismatiXPlayer> [--work <dir>]
//
// CI runs it with the installed runtime bundle of each desktop job.

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {copyFile, lstat, mkdtemp, readFile, readdir, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

const repository = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const argument = (name) => {
  const index = process.argv.indexOf(`--${name}`);
  return index > 0 ? process.argv[index + 1] : undefined;
};
const packager = resolve(argument("packager") ?? "");
const player = resolve(argument("player") ?? "");
if (!argument("packager") || !argument("player")) {
  process.stderr.write("usage: node scripts/check-desktop-layout.mjs --packager <exe> --player <exe> [--work <dir>]\n");
  process.exit(2);
}
const work = argument("work") ? resolve(argument("work")) : await mkdtemp(join(tmpdir(), "prismatix-layout-"));

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {encoding: "utf8", ...options});
  if (result.status !== 0)
    throw new Error(`${command} ${args.join(" ")} failed (${result.status ?? result.error}):\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}
const isFile = async (path) => (await lstat(path).catch(() => undefined))?.isFile() === true;
async function expectFiles(root, paths) {
  for (const path of paths) assert.ok(await isFile(join(root, path)), `missing ${path} in ${root}`);
}

const project = join(work, "starter");
run(process.execPath, [join(repository, "scripts", "stage-starter-sample.mjs"), project]);
await copyFile(join(repository, "Resources", "Branding", "PrismatiXEngine_Logo.png"), join(project, "icon.png"));
const manifestPath = join(project, "prismatix.json");
const manifest = JSON.parse(await readFile(manifestPath, "utf8"));

async function pack(distribution, output) {
  await writeFile(manifestPath, `${JSON.stringify({...manifest, distribution}, null, 2)}\n`);
  run(process.execPath, [join(repository, "packages", "cli", "dist", "src", "cli.js"), "pack", project,
    "--packager", packager, "--player", player, "--output", output], {cwd: repository});
  return output;
}

const branding = {executableName: "StarterGame", icon: "icon.png", productName: "PrismatiX Starter",
  companyName: "PrismatiX CI", copyright: "© 2026 PrismatiX CI", version: "1.2.3-beta.1",
  macos: {bundleIdentifier: "dev.prismatix.ci-starter"}};

if (process.platform === "win32") {
  const flat = await pack(branding, join(work, "flat"));
  await expectFiles(flat, ["StarterGame.exe", "Content.pdx", "Package/manifest.json", "SDL3.dll"]);
  const bundled = await pack({...branding, layout: "bundled"}, join(work, "bundled"));
  // .prismatix-output.json is the CLI's output ownership marker.
  assert.deepEqual((await readdir(bundled)).filter(entry => !entry.startsWith(".")).sort(),
    ["Engine", "StarterGame.exe"]);
  await expectFiles(bundled, ["Engine/Engine.manifest", "Engine/SDL3.dll", "Engine/Content.pdx",
    "Engine/Package/manifest.json"]);
  for (const exe of [join(flat, "StarterGame.exe"), join(bundled, "StarterGame.exe")]) {
    const bytes = await readFile(exe);
    const pe = bytes.readUInt32LE(0x3c);
    assert.equal(bytes.readUInt16LE(pe + 24 + 68), 2, `${exe} is a GUI (WIN32 subsystem) program`);
    const version = run("powershell", ["-NoProfile", "-Command",
      `(Get-Item -LiteralPath '${exe}').VersionInfo | ForEach-Object { $_.ProductName + '|' + $_.FileVersion + '|' + $_.CompanyName }`]);
    assert.equal(version.trim(), "PrismatiX Starter|1.2.3.0|PrismatiX CI", `${exe} version information`);
  }
  run(process.execPath, [join(repository, "packages", "cli", "dist", "src", "cli.js"), "verify-package", bundled,
    "--packager", packager], {cwd: repository});
} else if (process.platform === "darwin") {
  const output = await pack(branding, join(work, "mac"));
  assert.deepEqual(await readdir(output).then(entries => entries.filter(entry => !entry.startsWith("."))),
    ["StarterGame.app"], "a macOS distribution is one .app");
  const app = join(output, "StarterGame.app");
  await expectFiles(app, ["Contents/Info.plist", "Contents/PkgInfo", "Contents/MacOS/StarterGame",
    "Contents/Resources/StarterGame.icns", "Contents/Resources/Content.pdx",
    "Contents/Resources/Package/manifest.json"]);
  run("plutil", ["-lint", join(app, "Contents", "Info.plist")]);
  const plist = JSON.parse(run("plutil", ["-convert", "json", "-o", "-", join(app, "Contents", "Info.plist")]));
  assert.equal(plist.CFBundleIdentifier, "dev.prismatix.ci-starter");
  assert.equal(plist.CFBundleExecutable, "StarterGame");
  assert.equal(plist.CFBundleName, "PrismatiX Starter");
  assert.equal(plist.CFBundleShortVersionString, "1.2.3");
  assert.equal(plist.CFBundleIconFile, "StarterGame.icns");
  assert.equal(plist.NSHighResolutionCapable, true);
  assert.match(plist.LSMinimumSystemVersion, /^\d+\.\d+/u);
  assert.equal((await lstat(join(app, "Contents", "MacOS", "StarterGame"))).mode & 0o111, 0o111, "executable bit");
  run("codesign", ["--verify", "--strict", "--verbose=2", app]);
  // Every non-system library resolves inside the bundle.
  const libraries = run("otool", ["-L", join(app, "Contents", "MacOS", "StarterGame")]).split("\n").slice(1)
    .map(line => line.trim().split(" (")[0]).filter(Boolean);
  const outside = libraries.filter(path => !path.startsWith("/usr/lib/") && !path.startsWith("/System/") &&
    !path.startsWith("@rpath/") && !path.startsWith("@executable_path/"));
  assert.deepEqual(outside, [], "the Player links only system and bundled libraries");
  process.stdout.write(`${run("ls", ["-lR", app])}\n`);
} else {
  const output = await pack(branding, join(work, "linux"));
  await expectFiles(output, ["StarterGame", "StarterGame.desktop", "Content.pdx", "Package/manifest.json",
    ...[16, 24, 32, 48, 64, 128, 256].map(size => `icons/hicolor/${size}x${size}/apps/StarterGame.png`)]);
  assert.equal((await lstat(join(output, "StarterGame"))).mode & 0o111, 0o111, "executable bit");
  const desktop = await readFile(join(output, "StarterGame.desktop"), "utf8");
  assert.match(desktop, /^Exec="StarterGame"$/mu);
  assert.match(desktop, /^Icon=StarterGame$/mu);
  assert.match(desktop, /^Name=PrismatiX Starter$/mu);
  assert.match(run("readelf", ["-d", join(output, "StarterGame")]), /\$ORIGIN\/lib/u, "the Player searches lib/");
  const ldd = run("ldd", [join(output, "StarterGame")]);
  assert.doesNotMatch(ldd, /not found/u, `every library resolves:\n${ldd}`);
  if ((await readdir(join(output, "lib")).catch(() => [])).length > 0)
    assert.ok(ldd.includes(`${join(output, "lib")}/`), `bundled libraries load from lib/:\n${ldd}`);
  process.stdout.write(`${ldd}\n`);
}
if (!argument("work")) await rm(work, {recursive: true, force: true});
process.stdout.write(`Desktop layout checked (${process.platform})\n`);
