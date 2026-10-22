#!/usr/bin/env node
// Packs Samples/Starter with branding (a PNG icon, product name, version) and
// checks this platform's distribution layout (docs/shipping.md, Distribution
// layout): the Windows default bundled layout and the flat one, the macOS .app bundle
// (Info.plist, .icns, Frameworks, ad-hoc signature) or the Linux lib/ folder,
// .desktop entry and icon theme sizes (with ldd finding every library).
//
//   node scripts/check-desktop-layout.mjs --packager <PrismatiXPackager> --player <PrismatiXPlayer> [--work <dir>]
//
// With --cross <platform>=<runtime .tar.gz or directory> (repeatable) it
// instead packs for those other platforms from this host (docs/shipping.md,
// Packing for other platforms): `prismatix add platform --from`, then
// `pack --platform ... --archive`, and checks each archive without the
// target's tools: the Windows engine folder and the executable's version
// information, icons, manifest and checksum read from the PE file; the .app's
// Info.plist, .icns, executable bit and Mach-O code signature; the Linux
// executable bits, lib/, $ORIGIN rpath, .desktop entry and icons. Each packed
// folder must also pass verify-package.
//
// CI runs it with the installed runtime bundle of each desktop job.

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {copyFile, lstat, mkdtemp, readFile, readdir, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";
import {inflateRawSync} from "node:zlib";

const repository = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const argument = (name) => {
  const index = process.argv.indexOf(`--${name}`);
  return index > 0 ? process.argv[index + 1] : undefined;
};
const crossTargets = process.argv.flatMap((value, index) => value === "--cross" ? [process.argv[index + 1] ?? ""] : [])
  .map((value) => {
    const equals = value.indexOf("=");
    if (equals <= 0) throw new Error(`--cross takes <platform>=<runtime>, not ${value}`);
    return {platform: value.slice(0, equals), runtime: resolve(value.slice(equals + 1))};
  });
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

if (crossTargets.length > 0) {
  await checkCrossPlatform();
} else if (process.platform === "win32") {
  // The default layout is "bundled" (engine folder "Engine"); "flat" is opt-in.
  const flat = await pack({...branding, layout: "flat"}, join(work, "flat"));
  await expectFiles(flat, ["StarterGame.exe", "Content.pdx", "Package/manifest.json", "SDL3.dll"]);
  const bundled = await pack(branding, join(work, "bundled"));
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
process.stdout.write(crossTargets.length > 0
  ? `Cross-platform layouts checked from ${process.platform}: ${crossTargets.map(({platform}) => platform).join(", ")}\n`
  : `Desktop layout checked (${process.platform})\n`);

/** Zip entries: name, Unix mode and contents (deflate or stored). */
function readZip(bytes) {
  const end = bytes.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  assert.ok(end >= 0, "a zip end record");
  const entries = new Map();
  let at = bytes.readUInt32LE(end + 16);
  for (let index = 0; index < bytes.readUInt16LE(end + 10); index++) {
    assert.equal(bytes.readUInt32LE(at), 0x02014b50, "a zip central directory record");
    const method = bytes.readUInt16LE(at + 10);
    const size = bytes.readUInt32LE(at + 20);
    const nameLength = bytes.readUInt16LE(at + 28);
    const local = bytes.readUInt32LE(at + 42);
    const name = bytes.toString("utf8", at + 46, at + 46 + nameLength);
    const start = local + 30 + bytes.readUInt16LE(local + 26) + bytes.readUInt16LE(local + 28);
    const raw = bytes.subarray(start, start + size);
    entries.set(name, {madeBy: bytes[at + 5], mode: bytes.readUInt32LE(at + 38) >>> 16,
      data: method === 8 ? inflateRawSync(raw) : raw});
    at += 46 + nameLength + bytes.readUInt16LE(at + 30) + bytes.readUInt16LE(at + 32);
  }
  return entries;
}

/** The PE resources this check needs, read the way the Windows loader finds them, and the checksum. */
function peResources(image) {
  const pe = image.readUInt32LE(0x3c);
  assert.equal(image.toString("latin1", pe, pe + 4), "PE\0\0");
  const optional = pe + 24;
  const plus = image.readUInt16LE(optional) === 0x20b;
  const directories = optional + (plus ? 112 : 96);
  const sections = [];
  const table = optional + image.readUInt16LE(pe + 20);
  for (let index = 0; index < image.readUInt16LE(pe + 6); index++) {
    const at = table + 40 * index;
    sections.push({address: image.readUInt32LE(at + 12), size: image.readUInt32LE(at + 16), pointer: image.readUInt32LE(at + 20)});
  }
  const offset = (rva) => {
    const section = sections.find((item) => rva >= item.address && rva < item.address + item.size);
    assert.ok(section, `RVA ${rva} lies in a section`);
    return section.pointer + rva - section.address;
  };
  const base = image.readUInt32LE(directories + 16);
  const found = [];
  const walk = (directory, path) => {
    const at = offset(base + directory);
    const count = image.readUInt16LE(at + 12) + image.readUInt16LE(at + 14);
    for (let index = 0; index < count; index++) {
      const name = image.readUInt32LE(at + 16 + 8 * index);
      const target = image.readUInt32LE(at + 20 + 8 * index);
      let key = name & 0xffff;
      if (name & 0x80000000) {
        const string = offset(base + (name & 0x7fffffff));
        key = image.toString("utf16le", string + 2, string + 2 + 2 * image.readUInt16LE(string));
      }
      if (target & 0x80000000) walk(target & 0x7fffffff, [...path, key]);
      else {
        const entry = offset(base + target);
        const data = offset(image.readUInt32LE(entry));
        found.push({type: path[0], name: path[1], language: key, data: image.subarray(data, data + image.readUInt32LE(entry + 4))});
      }
    }
  };
  walk(0, []);
  // The PE checksum: 16-bit one's-complement sum without the field, plus the length.
  const field = optional + 64;
  let sum = 0;
  for (let at = 0; at < image.length; at += 2) {
    if (at === field || at === field + 2) continue;
    sum += at + 1 < image.length ? image.readUInt16LE(at) : image[at];
    sum = (sum & 0xffff) + (sum >>> 16);
  }
  sum = (sum & 0xffff) + (sum >>> 16);
  return {resources: found, subsystem: image.readUInt16LE(optional + 68),
    checksumValid: ((sum + image.length) >>> 0) === image.readUInt32LE(field)};
}

async function checkCrossPlatform() {
  const cli = join(repository, "packages", "cli", "dist", "src", "cli.js");
  const {extractTarGz} = await import(pathToFileURL(join(repository, "packages", "cli", "dist", "src", "archive.js")).href);
  const {machOCodeSignature, machOForeignDependencies, machOSlices} =
    await import(pathToFileURL(join(repository, "packages", "cli", "dist", "src", "distribution-layout.js")).href);
  await writeFile(manifestPath, `${JSON.stringify({...manifest, distribution: branding}, null, 2)}\n`);
  // This computer's own platform packs with --player, like any pack.
  for (const {platform, runtime} of crossTargets.filter(({platform}) => platform !== `${process.platform}-${process.arch}`))
    run(process.execPath, [cli, "add", "platform", platform, "--from", runtime, "--project", project], {cwd: repository});
  const output = join(work, "cross");
  run(process.execPath, [cli, "pack", project, "--platform", crossTargets.map(({platform}) => platform).join(","),
    "--archive", "--packager", packager, "--player", player, "--output", output], {cwd: repository});
  for (const {platform} of crossTargets) {
    run(process.execPath, [cli, "verify-package", join(output, platform), "--packager", packager], {cwd: repository});
    if (platform === "win32-x64") {
      const zip = readZip(await readFile(join(output, "StarterGame-win32-x64.zip")));
      for (const path of ["StarterGame/StarterGame.exe", "StarterGame/Engine/Engine.manifest", "StarterGame/Engine/SDL3.dll",
        "StarterGame/Engine/Content.pdx", "StarterGame/Engine/Package/manifest.json"])
        assert.ok(zip.has(path), `the Windows archive has ${path}`);
      assert.deepEqual([...zip.keys()].filter((path) => /^StarterGame\/[^/]+\/?$/u.test(path)).sort(),
        ["StarterGame/Engine/", "StarterGame/StarterGame.exe"], "only the executable and the engine folder at the top");
      const exe = peResources(zip.get("StarterGame/StarterGame.exe").data);
      assert.equal(exe.subsystem, 2, "a GUI (WIN32 subsystem) program");
      assert.ok(exe.checksumValid, "the executable's PE checksum is valid");
      const version = exe.resources.find((item) => item.type === 16 && item.name === 1);
      assert.ok(version, "VERSIONINFO");
      const strings = version.data.toString("utf16le");
      for (const expected of ["ProductName", "PrismatiX Starter", "CompanyName", "PrismatiX CI", "1.2.3-beta.1", "StarterGame.exe"])
        assert.ok(strings.includes(expected), `VERSIONINFO has ${expected}`);
      assert.equal(exe.resources.filter((item) => item.type === 14).length, 1, "one icon group");
      assert.equal(exe.resources.filter((item) => item.type === 3).length, 7, "seven icon sizes");
      const manifestXml = exe.resources.find((item) => item.type === 24 && item.name === 1)?.data.toString("utf8") ?? "";
      assert.match(manifestXml, /<assemblyIdentity type="win32" name="Engine"/u, "the manifest depends on Engine/");
      const layout = exe.resources.find((item) => item.type === 10 && item.name === "PRISMATIX_LAYOUT");
      assert.equal(JSON.parse(layout?.data.toString("utf8") ?? "{}").dataDirectory, "Engine", "PRISMATIX_LAYOUT");
    } else if (platform.startsWith("darwin")) {
      const zip = readZip(await readFile(join(output, `StarterGame-${platform}.zip`)));
      assert.deepEqual([...new Set([...zip.keys()].map((path) => path.split("/")[0]))], ["StarterGame.app"],
        "a macOS archive holds one .app");
      for (const path of ["Contents/Info.plist", "Contents/PkgInfo", "Contents/MacOS/StarterGame",
        "Contents/Resources/StarterGame.icns", "Contents/Resources/Content.pdx", "Contents/Resources/Package/manifest.json"])
        assert.ok(zip.has(`StarterGame.app/${path}`), `the .app has ${path}`);
      for (const entry of zip.values()) assert.equal(entry.madeBy, 3, "zip entries carry Unix modes");
      const executable = zip.get("StarterGame.app/Contents/MacOS/StarterGame");
      assert.equal(executable.mode & 0o111, 0o111, "the executable bit survives the archive");
      const plist = zip.get("StarterGame.app/Contents/Info.plist").data.toString("utf8");
      for (const [key, value] of [["CFBundleIdentifier", "dev.prismatix.ci-starter"], ["CFBundleExecutable", "StarterGame"],
        ["CFBundleName", "PrismatiX Starter"], ["CFBundleShortVersionString", "1.2.3"], ["CFBundleIconFile", "StarterGame.icns"]])
        assert.ok(plist.includes(`<key>${key}</key>\n\t<string>${value}</string>`), `Info.plist ${key} is ${value}`);
      assert.match(plist, /<key>LSMinimumSystemVersion<\/key>\s*<string>\d+\.\d+/u);
      const icns = zip.get("StarterGame.app/Contents/Resources/StarterGame.icns").data;
      assert.equal(icns.toString("latin1", 0, 4), "icns");
      assert.equal(icns.readUInt32BE(4), icns.length, "the .icns length covers the file");
      const slices = machOSlices(executable.data);
      assert.ok(slices?.some((slice) => slice.cpu === (platform === "darwin-arm64" ? "arm64" : "x86_64")),
        `the Player has ${platform} code`);
      assert.deepEqual(machOForeignDependencies(slices), [], "the Player links only macOS and bundled libraries");
      const signature = machOCodeSignature(executable.data);
      assert.equal(signature?.state, "valid", "the Player's code signature matches its contents");
      process.stdout.write(`${platform}: ${zip.size} entries, Player signature ${signature.state}` +
        `${signature.adhoc ? " (ad hoc)" : ""}\n`);
    } else if (platform === "linux-x64") {
      const entries = await extractTarGz(join(output, "StarterGame-linux-x64.tar.gz"), join(work, "linux-extracted"));
      const modes = new Map(entries.map((entry) => [entry.path, entry.mode]));
      const root = join(work, "linux-extracted", "StarterGame");
      await expectFiles(root, ["StarterGame", "StarterGame.desktop", "Content.pdx", "Package/manifest.json",
        ...[16, 24, 32, 48, 64, 128, 256].map((size) => `icons/hicolor/${size}x${size}/apps/StarterGame.png`)]);
      assert.equal(modes.get("StarterGame/StarterGame") & 0o111, 0o111, "the executable bit survives the archive");
      for (const [path, mode] of modes) if (path.startsWith("StarterGame/lib/")) assert.equal(mode & 0o111, 0o111, path);
      const desktop = await readFile(join(root, "StarterGame.desktop"), "utf8");
      assert.match(desktop, /^Exec="StarterGame"$/mu);
      assert.match(desktop, /^Icon=StarterGame$/mu);
      const elf = await readFile(join(root, "StarterGame"));
      assert.equal(elf.toString("latin1", 0, 4), "\x7fELF");
      assert.ok(elf.includes("$ORIGIN/lib"), "the Player searches lib/");
    } else throw new Error(`no cross check for ${platform}`);
    process.stdout.write(`${platform} packed from ${process.platform} and checked\n`);
  }
}
