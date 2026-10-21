#!/usr/bin/env node
// Release assembly, run by .github/workflows/release.yml and locally.
//
//   node scripts/assemble-release.mjs archive-runtime --runtime <installed PrismatiXRuntime prefix> --out <dir>
//     Checks one platform's runtime bundle and writes
//     prismatix-native-runtime-<version>-<platform>-<arch>.tar.gz (tar keeps
//     executable bits, which workflow artifacts would drop).
//
//   node scripts/assemble-release.mjs assemble --runtimes <dir of runtime archives> --out <dir> [--allow-partial]
//     Unpacks every platform's runtime archive, packs the three npm packages
//     (the CLI carrying all runtimes under runtime/<platform>-<arch>/), checks
//     what each tarball contains, and writes SHA256SUMS over the npm tarballs
//     and runtime archives. Without --allow-partial all release platforms are
//     required.

import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {createReadStream} from "node:fs";
import {copyFile, mkdir, mkdtemp, readdir, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, join, resolve} from "node:path";
import {checkPackedContents, checkRuntimeBundle, packPublicPackages, releasePlatforms, releaseVersion, runProcess}
  from "./release-packages.mjs";

const tar = process.platform === "win32" ? join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe") : "tar";
const runtimeArchive = (version, platformId) => `prismatix-native-runtime-${version}-${platformId}.tar.gz`;

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
  const version = await releaseVersion();
  const scratch = await mkdtemp(join(tmpdir(), "prismatix-release-"));
  try {
    const available = new Set(await readdir(runtimes));
    const included = {};
    for (const {id} of releasePlatforms) {
      const archive = runtimeArchive(version, id);
      if (!available.has(archive)) {
        assert.ok(allowPartial, `missing ${archive} in ${runtimes}`);
        process.stderr.write(`warning: no ${id} runtime; the CLI tarball will not support ${id}\n`);
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
    const packed = await packPublicPackages(npmDirectory, included);
    for (const {name, files} of packed) checkPackedContents(name, files, Object.keys(included));
    const runtimeDirectory = join(out, "runtime");
    await mkdir(runtimeDirectory, {recursive: true});
    for (const id of Object.keys(included))
      await copyFile(join(runtimes, runtimeArchive(version, id)), join(runtimeDirectory, runtimeArchive(version, id)));
    const assets = [...packed.map(({tarball}) => tarball),
      ...Object.keys(included).map((id) => join(runtimeDirectory, runtimeArchive(version, id)))];
    const sums = [];
    for (const asset of assets) sums.push(`${await sha256(asset)}  ${basename(asset)}`);
    await writeFile(join(out, "SHA256SUMS"), `${sums.join("\n")}\n`);
    await writeFile(join(out, "release.json"), `${JSON.stringify({
      version, platforms: Object.keys(included),
      npm: packed.map(({name, tarball}) => ({name, tarball: `npm/${basename(tarball)}`})),
    }, null, 2)}\n`);
    process.stdout.write(`${sums.join("\n")}\n`);
  } finally {
    await rm(scratch, {recursive: true, force: true});
  }
}

const [command, ...rest] = process.argv.slice(2);
if (command === "archive-runtime") await archiveRuntime(rest);
else if (command === "assemble") await assemble(rest);
else {
  process.stderr.write("usage: assemble-release.mjs archive-runtime --runtime <prefix> --out <dir> | assemble --runtimes <dir> --out <dir> [--allow-partial]\n");
  process.exit(2);
}
