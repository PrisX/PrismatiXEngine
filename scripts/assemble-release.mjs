#!/usr/bin/env node
// Release assembly, run by .github/workflows/release.yml and locally.
//
//   node scripts/assemble-release.mjs strip-runtime --runtime <installed PrismatiXRuntime prefix> --symbols <dir>
//     Strips debug information and symbol tables from the bundle's Linux
//     (ELF) and macOS (Mach-O) binaries in place and writes what it removed to
//     prismatix-native-runtime-<version>-<platform>-<arch>-debug-symbols.tar.gz:
//     a .debug file per ELF binary (linked back with .gnu_debuglink) or the
//     unstripped Mach-O binary (same LC_UUID, for lldb and atos). A Windows
//     Release build carries no debug information, so it has nothing to strip.
//
//   node scripts/assemble-release.mjs archive-runtime --runtime <installed PrismatiXRuntime prefix> --out <dir>
//     Checks one platform's runtime bundle and writes
//     prismatix-native-runtime-<version>-<platform>-<arch>.tar.gz (tar keeps
//     executable bits, which workflow artifacts would drop).
//
//   node scripts/assemble-release.mjs assemble --runtimes <dir of runtime archives> --out <dir>
//       [--web-player <Web Player package>] [--allow-partial]
//     Unpacks every platform's runtime archive and packs the npm packages in
//     publish order: @prismatix/native-<platform>-<arch> for each runtime,
//     @prismatix/web-player, then @prismatix/authoring-sdk, @prismatix/runtime
//     and @prismatix/cli (whose optionalDependencies name every native
//     package). Checks what each tarball contains, reports each tarball's
//     size, and writes SHA256SUMS over the npm tarballs, the runtime archives
//     and their debug-symbol archives. Without --allow-partial all release
//     platforms and the Web Player are required.

import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {createReadStream} from "node:fs";
import {copyFile, lstat, mkdir, mkdtemp, open, readdir, rm, stat, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, join, relative, resolve} from "node:path";
import {checkPackedContents, checkRuntimeBundle, checkWebPlayer, packPublicPackages, releasePlatforms, releaseVersion,
  runProcess} from "./release-packages.mjs";

const tar = process.platform === "win32" ? join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe") : "tar";
const runtimeArchive = (version, platformId) => `prismatix-native-runtime-${version}-${platformId}.tar.gz`;
const symbolsArchive = (version, platformId) => `prismatix-native-runtime-${version}-${platformId}-debug-symbols.tar.gz`;
const megabytes = (bytes) => `${(bytes / 1e6).toFixed(1)} MB`;

function option(args, name) {
  const index = args.indexOf(`--${name}`);
  if (index >= 0) return args[index + 1];
  return args.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
}

async function sha256(path) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return hash.digest("hex");
}

async function exists(path) {
  return stat(path).then(() => true, () => false);
}

/** "elf", "macho-executable", "macho-library" or undefined, from the file's first bytes. */
async function binaryKind(path) {
  const handle = await open(path, "r");
  try {
    const header = Buffer.alloc(16);
    const {bytesRead} = await handle.read(header, 0, 16, 0);
    if (bytesRead < 16) return undefined;
    if (header.readUInt32BE(0) === 0x7f454c46) return "elf";
    const magic = header.readUInt32LE(0);
    if (magic === 0xfeedfacf || magic === 0xfeedface) return header.readUInt32LE(12) === 2 ? "macho-executable" : "macho-library";
    if (header.readUInt32BE(0) === 0xcafebabe) return "macho-executable";
    return undefined;
  } finally {
    await handle.close();
  }
}

async function regularFiles(directory) {
  const found = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) found.push(...await regularFiles(path));
    else if (entry.isFile()) found.push(path);
  }
  return found;
}

async function stripRuntime(args) {
  const runtime = resolve(option(args, "runtime") ?? assert.fail("--runtime <prefix> is required"));
  const symbolsOut = resolve(option(args, "symbols") ?? assert.fail("--symbols <dir> is required"));
  const platformId = option(args, "platform") ?? `${process.platform}-${process.arch}`;
  const version = await releaseVersion();
  await checkRuntimeBundle(runtime, platformId, version);
  if (platformId.startsWith("win32-")) {
    process.stdout.write(`${platformId}: a Release build installs no debug information; nothing to strip\n`);
    return;
  }
  const scratch = await mkdtemp(join(tmpdir(), "prismatix-symbols-"));
  try {
    const symbolsRoot = join(scratch, basename(symbolsArchive(version, platformId), ".tar.gz"));
    let before = 0;
    let after = 0;
    for (const path of await regularFiles(runtime)) {
      if ((await lstat(path)).isSymbolicLink()) continue;
      const kind = await binaryKind(path);
      if (kind === undefined) continue;
      const name = relative(runtime, path);
      const size = (await stat(path)).size;
      await mkdir(dirname(join(symbolsRoot, name)), {recursive: true});
      if (kind === "elf") {
        // The .debug file keeps the debug sections and the full symbol table;
        // gdb finds it by the .gnu_debuglink name next to the binary or under
        // /usr/lib/debug.
        const debug = join(symbolsRoot, `${name}.debug`);
        await runProcess("objcopy", ["--only-keep-debug", path, debug]);
        await runProcess("objcopy", ["--strip-debug", "--strip-unneeded", `--add-gnu-debuglink=${debug}`, path]);
      } else {
        // The unstripped binary carries the same LC_UUID; lldb and atos
        // symbolicate a crash report with it.
        await copyFile(path, join(symbolsRoot, name));
        await runProcess("strip", kind === "macho-executable" ? [path] : ["-x", path]);
        // Stripping invalidates the linker's ad-hoc signature, which arm64 macOS requires.
        await runProcess("codesign", ["--force", "--sign", "-", path]);
      }
      const stripped = (await stat(path)).size;
      before += size;
      after += stripped;
      process.stdout.write(`${name}: ${megabytes(size)} -> ${megabytes(stripped)}\n`);
    }
    await mkdir(symbolsOut, {recursive: true});
    const archive = join(symbolsOut, symbolsArchive(version, platformId));
    await runProcess(tar, ["-czf", archive, "-C", scratch, basename(symbolsRoot)]);
    process.stdout.write(`${platformId} binaries: ${megabytes(before)} -> ${megabytes(after)}; ` +
      `symbols in ${archive} (${megabytes((await stat(archive)).size)})\n`);
  } finally {
    await rm(scratch, {recursive: true, force: true});
  }
}

async function archiveRuntime(args) {
  const runtime = resolve(option(args, "runtime") ?? assert.fail("--runtime <prefix> is required"));
  const out = resolve(option(args, "out") ?? assert.fail("--out <dir> is required"));
  const platformId = option(args, "platform") ?? `${process.platform}-${process.arch}`;
  const version = await releaseVersion();
  const manifest = await checkRuntimeBundle(runtime, platformId, version);
  await mkdir(out, {recursive: true});
  const archive = join(out, runtimeArchive(version, platformId));
  await runProcess(tar, ["-czf", archive, "-C", runtime, "."]);
  process.stdout.write(`${archive} (${manifest.target})\n`);
}

async function assemble(args) {
  const runtimes = resolve(option(args, "runtimes") ?? assert.fail("--runtimes <dir> is required"));
  const out = resolve(option(args, "out") ?? assert.fail("--out <dir> is required"));
  const allowPartial = args.includes("--allow-partial");
  const webPlayer = option(args, "web-player") === undefined ? undefined : resolve(option(args, "web-player"));
  const version = await releaseVersion();
  assert.ok(webPlayer !== undefined || allowPartial, "--web-player <dir> is required (a release publishes @prismatix/web-player)");
  if (webPlayer !== undefined) await checkWebPlayer(webPlayer, version);
  const scratch = await mkdtemp(join(tmpdir(), "prismatix-release-"));
  try {
    const available = new Set(await readdir(runtimes));
    const included = {};
    for (const {id} of releasePlatforms) {
      const archive = runtimeArchive(version, id);
      if (!available.has(archive)) {
        assert.ok(allowPartial, `missing ${archive} in ${runtimes}`);
        process.stderr.write(`warning: no ${id} runtime; this release would have no @prismatix/native-${id}\n`);
        continue;
      }
      const unpacked = join(scratch, "runtime", id);
      await mkdir(unpacked, {recursive: true});
      await runProcess(tar, ["-xzf", join(runtimes, archive), "-C", unpacked]);
      await checkRuntimeBundle(unpacked, id, version);
      included[id] = unpacked;
    }
    assert.ok(Object.keys(included).length > 0, "no runtime archives to assemble");
    await rm(out, {recursive: true, force: true});
    const npmDirectory = join(out, "npm");
    const packed = await packPublicPackages(npmDirectory, included, process.env, {webPlayer});
    for (const {name, files, manifest} of packed) checkPackedContents(name, files, manifest);
    for (const {name, size, unpackedSize, files} of packed)
      process.stdout.write(`${name}: ${size} bytes packed (${megabytes(size)}), ${unpackedSize} bytes unpacked ` +
        `(${megabytes(unpackedSize)}), ${files.length} files\n`);
    const runtimeDirectory = join(out, "runtime");
    await mkdir(runtimeDirectory, {recursive: true});
    const archives = [];
    for (const id of Object.keys(included))
      for (const filename of [runtimeArchive(version, id), symbolsArchive(version, id)]) {
        if (!(await exists(join(runtimes, filename)))) continue;
        await copyFile(join(runtimes, filename), join(runtimeDirectory, filename));
        archives.push(join(runtimeDirectory, filename));
      }
    const assets = [...packed.map(({tarball}) => tarball), ...archives];
    const sums = [];
    for (const asset of assets) sums.push(`${await sha256(asset)}  ${basename(asset)}`);
    await writeFile(join(out, "SHA256SUMS"), `${sums.join("\n")}\n`);
    await writeFile(join(out, "release.json"), `${JSON.stringify({
      version, platforms: Object.keys(included), webPlayer: webPlayer !== undefined,
      // Publish order: each package only depends on the ones before it.
      npm: packed.map(({name, tarball, size, unpackedSize}) => ({name, tarball: `npm/${basename(tarball)}`, size, unpackedSize})),
      runtime: archives.map((path) => `runtime/${basename(path)}`),
    }, null, 2)}\n`);
    process.stdout.write(`${sums.join("\n")}\n`);
  } finally {
    await rm(scratch, {recursive: true, force: true});
  }
}

const [command, ...rest] = process.argv.slice(2);
if (command === "strip-runtime") await stripRuntime(rest);
else if (command === "archive-runtime") await archiveRuntime(rest);
else if (command === "assemble") await assemble(rest);
else {
  process.stderr.write("usage: assemble-release.mjs strip-runtime --runtime <prefix> --symbols <dir> | " +
    "archive-runtime --runtime <prefix> --out <dir> | " +
    "assemble --runtimes <dir> --out <dir> [--web-player <dir>] [--allow-partial]\n");
  process.exit(2);
}
