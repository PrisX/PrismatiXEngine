// Stages and packs the public npm packages exactly as they are published:
//
//   @prismatix/native-<platform>-<arch>  one platform's native runtime bundle
//                                        (Player, Packager, Preview Host,
//                                        notices) with os/cpu fields
//   @prismatix/web-player                the experimental Web Player (both
//                                        renderers); installed on demand
//   @prismatix/authoring-sdk, @prismatix/runtime, @prismatix/cli
//                                        each package's `files` plus package.json
//
// The CLI lists every native package as an exact-version optionalDependency
// (added here when it is staged, so the checkout's own installs never ask
// the registry for packages that only exist after a release), and npm and
// pnpm install the one matching the author's machine. The Web Player is not
// a dependency of the CLI.
// Used by the clean-install acceptance (one platform) and by release assembly
// (every platform).

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {createHash} from "node:crypto";
import {createReadStream} from "node:fs";
import {cp, mkdir, readFile, readdir, rm, stat, writeFile} from "node:fs/promises";
import {basename, dirname, extname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

export const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

/** The packages whose package.json lives in this repository, in publish order. */
export const publicPackages = [
  {name: "@prismatix/authoring-sdk", root: join(repositoryRoot, "packages", "authoring-sdk")},
  {name: "@prismatix/runtime", root: join(repositoryRoot, "runtime-sdk")},
  {name: "@prismatix/cli", root: join(repositoryRoot, "packages", "cli")},
];

/**
 * The desktop platforms a release ships a native package for: Node's
 * `${platform}-${arch}`, the npm os/cpu (and libc) fields and the bundle's
 * target triple.
 */
export const releasePlatforms = [
  {id: "win32-x64", label: "Windows x64", os: "win32", cpu: "x64", triple: "x86_64-pc-windows-msvc", executableSuffix: ".exe"},
  {id: "linux-x64", label: "Linux x64 (glibc)", os: "linux", cpu: "x64", libc: "glibc", triple: "x86_64-unknown-linux-gnu", executableSuffix: ""},
  {id: "darwin-x64", label: "macOS Intel", os: "darwin", cpu: "x64", triple: "x86_64-apple-darwin", executableSuffix: ""},
  {id: "darwin-arm64", label: "macOS Apple silicon", os: "darwin", cpu: "arm64", triple: "aarch64-apple-darwin", executableSuffix: ""},
];

/** `@prismatix/native-<platform>-<arch>`: "native" so it never reads as a build of the `@prismatix/runtime` SDK. */
export const nativePackageName = (platformId) => `@prismatix/native-${platformId}`;
export const webPlayerPackageName = "@prismatix/web-player";
/** The npm pack file name of a package: `@prismatix/cli` 0.1.0 is prismatix-cli-0.1.0.tgz. */
export const tarballName = (name, version) => `${name.slice(1).replace("/", "-")}-${version}.tgz`;

/** Every package name of a release in publish order: each only depends on the ones before it. */
export function releasePackageNames(platformIds = releasePlatforms.map(({id}) => id), {webPlayer = true} = {}) {
  return [...platformIds.map(nativePackageName), ...(webPlayer ? [webPlayerPackageName] : []),
    ...publicPackages.map(({name}) => name)];
}

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

/** The Web Player renderers the @prismatix/web-player package carries. */
export const webPlayerRenderers = ["webgl2", "webgpu"];

/**
 * Checks a Web Player package (scripts/build-wasm-player.mjs) belongs to this
 * release: schema revision 2, this version, both renderers, every listed file
 * matching its SHA-256 and no host tools (they are per-platform executables).
 */
export async function checkWebPlayer(directory, version) {
  const manifest = await readJson(join(directory, "web-player.json"));
  assert.equal(manifest.format, "PrismatiXWebPlayer");
  assert.equal(manifest.schemaRevision, 2, "Web Player schema revision");
  assert.equal(manifest.variant, "shipped");
  assert.equal(manifest.engineVersion, version, `Web Player is ${manifest.engineVersion}, not ${version}`);
  const hashed = {...manifest.artifacts};
  for (const renderer of webPlayerRenderers) {
    const entry = manifest.renderers?.[renderer];
    assert.ok(entry, `Web Player has no ${renderer} renderer`);
    for (const file of ["prismatix-player.mjs", "prismatix-player.wasm"])
      assert.ok(entry.artifacts?.[`${entry.directory}/${file}`], `Web Player ${renderer} lists no ${file}`);
    Object.assign(hashed, entry.artifacts);
  }
  for (const [path, expected] of Object.entries(hashed)) {
    const hash = createHash("sha256");
    for await (const chunk of createReadStream(join(directory, ...path.split("/")))) hash.update(chunk);
    assert.equal(hash.digest("hex"), expected, `Web Player checksum mismatch: ${path}`);
  }
  assert.ok(!(await exists(join(directory, "tools"))), "a release Web Player must not carry host tools (build it with --no-host-tools)");
  return manifest;
}

const packageMetadata = {
  license: "MIT",
  author: "owen0924",
  homepage: "https://github.com/PrisX/PrismatiXEngine#readme",
  bugs: {url: "https://github.com/PrisX/PrismatiXEngine/issues"},
  publishConfig: {access: "public", provenance: true},
};
const repository = (directory) => ({type: "git", url: "git+https://github.com/PrisX/PrismatiXEngine.git", directory});
const generatedFiles = ["package.json", "README.md", "LICENSE"];

/** The package.json of one platform's native package; `files` are the bundle's top-level entries. */
export function nativePackageManifest(platformId, version, entries) {
  const platform = releasePlatforms.find((item) => item.id === platformId);
  assert.ok(platform, `${platformId} is not a release platform`);
  return {
    name: nativePackageName(platformId), version,
    description: `The PrismatiX native runtime for ${platform.label}: the Player, Packager and Preview Host that ` +
      "@prismatix/cli runs. @prismatix/cli installs it; do not depend on it directly.",
    keywords: ["prismatix", "visual-novel", "runtime", platform.os, platform.cpu],
    ...packageMetadata, repository: repository("packages/cli"),
    os: [platform.os], cpu: [platform.cpu], ...(platform.libc ? {libc: [platform.libc]} : {}),
    // Package managers that would keep it zipped must extract it: the CLI starts these executables from disk.
    preferUnplugged: true,
    files: [...entries.filter((entry) => !generatedFiles.includes(entry)).sort(), "README.md", "LICENSE"],
  };
}

function nativeReadme(platformId, version) {
  const platform = releasePlatforms.find((item) => item.id === platformId);
  return [
    `# ${nativePackageName(platformId)}`,
    "",
    `The native [PrismatiX](https://github.com/PrisX/PrismatiXEngine) runtime for ${platform.label}: the`,
    "Player, Packager and Preview Host that",
    "[`@prismatix/cli`](https://www.npmjs.com/package/@prismatix/cli) runs to preview, build and pack a game.",
    "",
    "You do not install this package yourself. `@prismatix/cli` lists the native",
    "package of every supported platform as an optional dependency of exactly its",
    "own version, and npm and pnpm install only the one for your operating system",
    "and processor. If `prismatix` says this package is missing, reinstall",
    "`@prismatix/cli` without `--no-optional` or `--omit=optional`.",
    "",
    `This is version ${version}; it only works with \`@prismatix/cli@${version}\`.`,
    "",
    "## License",
    "",
    "MIT. The runtime includes third-party components under their own licences;",
    "see `share/PrismatiX/THIRD_PARTY_NOTICES.md` and `share/PrismatiX/licenses/`.",
    "A packed game copies them next to the Player under `Notices/`.",
    "",
  ].join("\n");
}

/** The package.json of the Web Player package; `files` are the Web Player's top-level entries. */
export function webPlayerPackageManifest(version, entries) {
  return {
    name: webPlayerPackageName, version,
    description: "The experimental PrismatiX Web Player (WebGL2 and WebGPU WebAssembly builds) that " +
      "`prismatix pack --target web` exports games with.",
    keywords: ["prismatix", "visual-novel", "webassembly", "webgl2", "webgpu"],
    ...packageMetadata, repository: repository("packages/cli"),
    files: [...entries.filter((entry) => !generatedFiles.includes(entry)).sort(), "README.md", "LICENSE"],
  };
}

function webPlayerReadme(version) {
  return [
    "# @prismatix/web-player",
    "",
    "The experimental [PrismatiX](https://github.com/PrisX/PrismatiXEngine) Web Player:",
    "the C++ Player built for WebAssembly, with a WebGL2 and a WebGPU renderer.",
    "`prismatix pack --target web` exports a game with it.",
    "",
    "It is not part of `@prismatix/cli`. Add it to a project that exports for the",
    "web, at exactly the version of the project's `@prismatix/cli`:",
    "",
    "```bash",
    `pnpm add -D @prismatix/web-player@${version}`,
    `# npm: npm install -D --save-exact @prismatix/web-player@${version}`,
    "```",
    "",
    "See [Web export](https://github.com/PrisX/PrismatiXEngine/blob/master/docs/web-export.md).",
    "",
    "## License",
    "",
    "MIT. The Web Player includes third-party components under their own licences:",
    "`Notices/` holds them, and `RuntimeSource.tar.gz` and `FriBidiSource.tar.gz`",
    "are the corresponding sources. An exported game carries the notices of the",
    "renderer it uses.",
    "",
  ].join("\n");
}

/** The CLI's optionalDependencies: every release platform's native package at exactly `version`. */
export function cliOptionalDependencies(version) {
  return Object.fromEntries(releasePlatforms.map(({id}) => [nativePackageName(id), version]));
}

async function npmPack(stage, packDirectory, name, environment) {
  const [command, ...prefix] = npmInvocation();
  const packed = await runProcess(command, [...prefix, "pack", "--json", "--ignore-scripts", "--pack-destination", packDirectory],
    {cwd: stage, env: environment});
  const report = JSON.parse(packed.stdout);
  assert.ok(Array.isArray(report) && typeof report[0]?.filename === "string", `npm pack returned an unexpected report for ${name}`);
  const tarball = join(packDirectory, report[0].filename);
  assert.ok(await exists(tarball), `npm pack did not create ${tarball}`);
  return {tarball, files: report[0].files.map((file) => file.path), size: report[0].size, unpackedSize: report[0].unpackedSize};
}

/** Stages a generated package: `content` at the package root plus package.json, README.md and LICENSE. */
async function stageGenerated(stage, content, manifestFor, readme) {
  await rm(stage, {recursive: true, force: true});
  await cp(content, stage, {recursive: true});
  const manifest = manifestFor(await readdir(stage));
  await writeFile(join(stage, "package.json"), `${JSON.stringify(manifest, null, 2)}\n`);
  await writeFile(join(stage, "README.md"), readme);
  await cp(join(repositoryRoot, "LICENSE"), join(stage, "LICENSE"));
  return manifest;
}

/**
 * Stages and packs with `npm pack`: a native package for each given runtime
 * ({platformId: runtime directory}), the Web Player package when given, then
 * the repository's packages, the CLI with optionalDependencies on every
 * release platform's native package (whether or not this run packs it).
 * Returns [{name, manifest, tarball, files, size, unpackedSize}] in publish order.
 */
export async function packPublicPackages(packDirectory, runtimes, environment = process.env, {webPlayer} = {}) {
  await mkdir(packDirectory, {recursive: true});
  const staging = join(packDirectory, "staging");
  await rm(staging, {recursive: true, force: true});
  for (const id of Object.keys(runtimes))
    assert.ok(releasePlatforms.some((platform) => platform.id === id), `${id} is not a release platform`);
  const version = await releaseVersion();
  const results = [];
  const packGenerated = async (directory, content, manifestFor, readme) => {
    const stage = join(staging, directory);
    const manifest = await stageGenerated(stage, content, manifestFor, readme);
    results.push({name: manifest.name, manifest, ...await npmPack(stage, packDirectory, manifest.name, environment)});
    await rm(stage, {recursive: true, force: true});
  };
  for (const {id} of releasePlatforms)
    if (runtimes[id] !== undefined)
      await packGenerated(`native-${id}`, runtimes[id], (entries) => nativePackageManifest(id, version, entries),
        nativeReadme(id, version));
  if (webPlayer !== undefined)
    await packGenerated("web-player", webPlayer, (entries) => webPlayerPackageManifest(version, entries), webPlayerReadme(version));
  for (const {name, root} of publicPackages) {
    const manifest = await readJson(join(root, "package.json"));
    assert.equal(manifest.name, name);
    const stage = join(staging, basename(root));
    await mkdir(stage, {recursive: true});
    for (const path of manifest.files ?? ["dist"]) {
      assert.ok(await exists(join(root, path)), `${name} lists ${path} in files but it does not exist (build first)`);
      await cp(join(root, path), join(stage, path), {recursive: true});
    }
    if (name === "@prismatix/cli") {
      assert.equal(manifest.optionalDependencies, undefined,
        "packages/cli/package.json must not list optionalDependencies; release-packages.mjs adds the native packages");
      manifest.optionalDependencies = cliOptionalDependencies(manifest.version);
    }
    await writeFile(join(stage, "package.json"), `${JSON.stringify(manifest, null, 2)}\n`);
    results.push({name, manifest, ...await npmPack(stage, packDirectory, name, environment)});
  }
  await rm(staging, {recursive: true, force: true});
  return results;
}

/** Asserts what a published tarball must contain; `files` are the paths npm pack reported. */
export function checkPackedContents(name, files, manifest) {
  const has = (path) => files.includes(path);
  for (const required of ["package.json", "LICENSE", "README.md"])
    assert.ok(has(required), `${name} tarball is missing ${required}`);
  assert.ok(!files.some((path) => /(^|\/)(src|test)\/.*\.ts$/u.test(path) && !path.endsWith(".d.ts")),
    `${name} tarball contains TypeScript sources`);
  if (manifest !== undefined) assert.equal(manifest.name, name);
  if (name === "@prismatix/authoring-sdk") assert.ok(has("dist/src/index.js"), `${name} tarball has no dist/src/index.js`);
  if (name === "@prismatix/runtime") assert.ok(has("dist/src/index.js") && has("sandbox.d.ts"), `${name} tarball is incomplete`);
  if (name === "@prismatix/cli") {
    assert.ok(has("dist/src/cli.js"), `${name} tarball has no dist/src/cli.js`);
    assert.ok(has("templates/minimal/package.json"), `${name} tarball has no starter template`);
    assert.ok(has("templates/minimal/Content/Fonts/OFL-1.1.txt"), `${name} tarball has no starter font licence`);
    assert.ok(!files.some((path) => path.startsWith("runtime/")), `${name} tarball carries runtime/ (the native packages hold it)`);
    if (manifest !== undefined) {
      assert.deepEqual(manifest.optionalDependencies, cliOptionalDependencies(manifest.version),
        `${name} must list every native package at exactly ${manifest.version} as optionalDependencies`);
      assert.ok(manifest.dependencies?.[webPlayerPackageName] === undefined, `${name} must not depend on the Web Player`);
    }
  }
  const platform = releasePlatforms.find(({id}) => nativePackageName(id) === name);
  if (platform !== undefined) {
    for (const relative of ["share/PrismatiX/runtime-manifest.json", "share/PrismatiX/LICENSE",
      "share/PrismatiX/THIRD_PARTY_NOTICES.md", "share/PrismatiX/ASSET_PROVENANCE.json"])
      assert.ok(has(relative), `${name} tarball is missing ${relative}`);
    for (const tool of ["PrismatiXPlayer", "PrismatiXPackager", "PrismatiXPreviewHost"])
      assert.ok(has(`bin/${tool}${platform.executableSuffix}`), `${name} tarball is missing bin/${tool}${platform.executableSuffix}`);
    if (manifest !== undefined) {
      assert.deepEqual(manifest.os, [platform.os], `${name} os`);
      assert.deepEqual(manifest.cpu, [platform.cpu], `${name} cpu`);
    }
  }
  if (name === webPlayerPackageName) {
    for (const relative of ["web-player.json", "web-shaders.json", "RuntimeSource.tar.gz", "FriBidiSource.tar.gz",
      ...webPlayerRenderers.flatMap((renderer) => [`${renderer}/prismatix-player.mjs`, `${renderer}/prismatix-player.wasm`])])
      assert.ok(has(relative), `${name} tarball is missing ${relative}`);
    assert.ok(!files.some((path) => path.startsWith("tools/")), `${name} tarball carries Web Player host tools`);
  }
}
