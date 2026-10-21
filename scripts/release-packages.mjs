// Stages and packs the three public npm packages exactly as they are
// published: each package's `files` plus package.json, and for the CLI the
// native runtime of every included platform under runtime/<platform>-<arch>/.
// Used by the clean-install acceptance (one platform) and by release assembly
// (every platform).

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {cp, mkdir, readFile, rm, stat} from "node:fs/promises";
import {basename, dirname, extname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

export const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

/** Publish order: each package only depends on the ones before it. */
export const publicPackages = [
  {name: "@prismatix/authoring-sdk", root: join(repositoryRoot, "packages", "authoring-sdk")},
  {name: "@prismatix/runtime", root: join(repositoryRoot, "runtime-sdk")},
  {name: "@prismatix/cli", root: join(repositoryRoot, "packages", "cli")},
];

/** The desktop runtimes a release CLI carries: Node's `${platform}-${arch}` and the bundle's target triple. */
export const releasePlatforms = [
  {id: "win32-x64", triple: "x86_64-pc-windows-msvc", executableSuffix: ".exe"},
  {id: "linux-x64", triple: "x86_64-unknown-linux-gnu", executableSuffix: ""},
  {id: "darwin-x64", triple: "x86_64-apple-darwin", executableSuffix: ""},
  {id: "darwin-arm64", triple: "aarch64-apple-darwin", executableSuffix: ""},
];

export async function readJson(path) {
  return JSON.parse(await readFile(path, "utf8"));
}

/** The release version: the CLI's package.json (verify-release-version.mjs checks every other copy). */
export async function releaseVersion() {
  return (await readJson(join(repositoryRoot, "packages", "cli", "package.json"))).version;
}

async function exists(path) {
  return stat(path).then(() => true, () => false);
}

function npmInvocation() {
  // Under `pnpm run`, npm_execpath is pnpm; packing uses npm's own tarball format.
  const candidate = process.env.npm_execpath;
  if (candidate !== undefined && candidate.length > 0 && extname(candidate).toLowerCase() !== ".ps1" &&
      !basename(candidate).toLowerCase().startsWith("pnpm") && basename(candidate).toLowerCase().startsWith("npm"))
    return [process.execPath, candidate];
  const nodeRoot = dirname(process.execPath);
  return [process.execPath, process.platform === "win32"
    ? join(nodeRoot, "node_modules", "npm", "bin", "npm-cli.js")
    : join(nodeRoot, "..", "lib", "node_modules", "npm", "bin", "npm-cli.js")];
}

export function runProcess(command, args, {cwd, env, timeoutMs = 300_000} = {}) {
  return new Promise((accept, reject) => {
    const child = spawn(command, args, {cwd, env, shell: false, windowsHide: true, stdio: ["ignore", "pipe", "pipe"]});
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (value) => { stdout += value; });
    child.stderr.on("data", (value) => { stderr += value; });
    child.on("error", reject);
    const timer = setTimeout(() => { child.kill(); reject(new Error(`Timed out: ${command} ${args.join(" ")}`)); }, timeoutMs);
    child.on("close", (code) => {
      clearTimeout(timer);
      if (code !== 0) reject(new Error(`${command} ${args.join(" ")} failed (${code})\n${stdout}\n${stderr}`));
      else accept({stdout, stderr});
    });
  });
}

/** Checks one runtime bundle belongs to this release and platform before it is packed. */
export async function checkRuntimeBundle(runtimeRoot, platformId, version) {
  const platform = releasePlatforms.find((item) => item.id === platformId);
  const manifestPath = join(runtimeRoot, "share", "PrismatiX", "runtime-manifest.json");
  assert.ok(await exists(manifestPath), `${platformId} runtime has no share/PrismatiX/runtime-manifest.json`);
  const manifest = await readJson(manifestPath);
  if (!manifest.testOnly) {
    assert.equal(manifest.version, version, `${platformId} runtime is ${manifest.version}, not ${version}`);
    if (platform !== undefined)
      assert.ok(manifest.target === platform.triple || manifest.target === platform.id,
        `${platformId} runtime targets ${manifest.target}`);
    for (const relative of ["LICENSE", "THIRD_PARTY_NOTICES.md", "ASSET_PROVENANCE.json", "release-inventory.json",
      join("licenses", "fonts", "OFL-1.1.txt"), join("docs", "releases", "delivery-contract.md")])
      assert.ok(await exists(join(runtimeRoot, "share", "PrismatiX", relative)), `${platformId} runtime is missing ${relative}`);
  }
  const bin = resolve(dirname(manifestPath), manifest.binDir);
  for (const tool of Object.values(manifest.tools))
    assert.ok(await exists(join(bin, tool)), `${platformId} runtime is missing ${tool}`);
  return manifest;
}

/**
 * Stages each public package (package.json + its `files`), adds the given
 * runtimes ({platformId: directory}) to the CLI, and packs with `npm pack`.
 * Returns [{name, manifest, tarball}] in publish order.
 */
export async function packPublicPackages(packDirectory, runtimes, environment = process.env) {
  await mkdir(packDirectory, {recursive: true});
  const staging = join(packDirectory, "staging");
  await rm(staging, {recursive: true, force: true});
  const results = [];
  for (const {name, root} of publicPackages) {
    const manifest = await readJson(join(root, "package.json"));
    assert.equal(manifest.name, name);
    const stage = join(staging, basename(root));
    await mkdir(stage, {recursive: true});
    await cp(join(root, "package.json"), join(stage, "package.json"));
    for (const path of manifest.files ?? ["dist"]) {
      if (path === "runtime") continue;
      assert.ok(await exists(join(root, path)), `${name} lists ${path} in files but it does not exist (build first)`);
      await cp(join(root, path), join(stage, path), {recursive: true});
    }
    if (name === "@prismatix/cli") {
      assert.ok(Object.keys(runtimes).length > 0, "the CLI package needs at least one runtime");
      for (const [platformId, directory] of Object.entries(runtimes))
        await cp(directory, join(stage, "runtime", platformId), {recursive: true});
    }
    const [command, ...prefix] = npmInvocation();
    const packed = await runProcess(command, [...prefix, "pack", "--json", "--ignore-scripts", "--pack-destination", packDirectory],
      {cwd: stage, env: environment});
    const report = JSON.parse(packed.stdout);
    assert.ok(Array.isArray(report) && typeof report[0]?.filename === "string", `npm pack returned an unexpected report for ${name}`);
    const tarball = join(packDirectory, report[0].filename);
    assert.ok(await exists(tarball), `npm pack did not create ${tarball}`);
    results.push({name, manifest, tarball, files: report[0].files.map((file) => file.path)});
  }
  await rm(staging, {recursive: true, force: true});
  return results;
}

/** Asserts what each published tarball must contain; `files` are the paths npm pack reported. */
export function checkPackedContents(name, files, runtimePlatformIds = []) {
  const has = (path) => files.includes(path);
  for (const required of ["package.json", "LICENSE", "README.md"])
    assert.ok(has(required), `${name} tarball is missing ${required}`);
  assert.ok(!files.some((path) => /(^|\/)(src|test)\/.*\.ts$/u.test(path) && !path.endsWith(".d.ts")),
    `${name} tarball contains TypeScript sources`);
  if (name === "@prismatix/authoring-sdk") assert.ok(has("dist/src/index.js"), `${name} tarball has no dist/src/index.js`);
  if (name === "@prismatix/runtime") assert.ok(has("dist/src/index.js") && has("sandbox.d.ts"), `${name} tarball is incomplete`);
  if (name === "@prismatix/cli") {
    assert.ok(has("dist/src/cli.js"), `${name} tarball has no dist/src/cli.js`);
    assert.ok(has("templates/minimal/package.json"), `${name} tarball has no starter template`);
    assert.ok(has("templates/minimal/Content/Fonts/OFL-1.1.txt"), `${name} tarball has no starter font licence`);
    for (const platformId of runtimePlatformIds) {
      for (const relative of ["share/PrismatiX/runtime-manifest.json", "share/PrismatiX/LICENSE",
        "share/PrismatiX/THIRD_PARTY_NOTICES.md", "share/PrismatiX/ASSET_PROVENANCE.json"])
        assert.ok(has(`runtime/${platformId}/${relative}`), `${name} tarball is missing runtime/${platformId}/${relative}`);
      const suffix = releasePlatforms.find((item) => item.id === platformId)?.executableSuffix ?? "";
      for (const tool of ["PrismatiXPlayer", "PrismatiXPackager", "PrismatiXPreviewHost"])
        assert.ok(has(`runtime/${platformId}/bin/${tool}${suffix}`), `${name} tarball is missing runtime/${platformId}/bin/${tool}${suffix}`);
    }
  }
}
