#!/usr/bin/env node
// Builds and packages the shipping Web Player (both renderers by default).
// Runs on Windows, Linux and macOS. See docs/web-export.md.
//
//   node scripts/build-wasm-player.mjs [--renderer webgl2|webgpu|all]
//     [--package-root <dir>] [--build-dir-webgl2 <dir>] [--build-dir-webgpu <dir>]
//     [--skip-build] [--no-host-tools] [--jobs <n>]
//     [--native-build-root <dir>] [--vcpkg-installed <dir>] [--triplet <triplet>]
//     [--shadercross <exe>] [--spirv-cross <exe>] [--naga <exe>]
//     [--update-builtin-wgsl] [--allow-emscripten <version>]
//
// Needs an active Emscripten SDK (emsdk_env), CMake, Ninja, the built CLI
// (pnpm run cli:build), and the host shader tools: shadercross and
// SPIRV-Cross from a native vcpkg install, Naga 27.0.0 (cargo install
// naga-cli --version 27.0.0 --locked --root out/tools/naga).
//
// The package (default out/package/wasm-player):
//   web-player.json             schema revision 2: version, renderers, SHA-256 of every file
//   webgl2/prismatix-player.{mjs,wasm}, webgl2/Notices/SDL/...
//   webgpu/prismatix-player.{mjs,wasm}, webgpu/Notices/{SDL,Dawn}/...
//   web-shaders.json, Notices/..., RuntimeSource.tar.gz, FriBidiSource.tar.gz (shared)
//   tools/, ToolNotices/       host converters (omitted with --no-host-tools)
// `prismatix pack --target web --web-renderer <renderer>` copies the shared
// files and one renderer's files into an export.

import {spawn} from "node:child_process";
import {createHash} from "node:crypto";
import {createReadStream} from "node:fs";
import {copyFile, mkdir, readFile, readdir, rm, stat, writeFile, chmod} from "node:fs/promises";
import {homedir} from "node:os";
import {delimiter, dirname, join, resolve} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";
import {collectRendererNotices, collectSharedNotices, collectToolNotices} from "./collect-web-notices.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const windows = process.platform === "win32";
const exe = windows ? ".exe" : "";
/** CI's pin (.github/workflows/ci.yml); the WebGPU build's emdawnwebgpu pin comes with it. */
export const pinnedEmscripten = "6.0.5";
export const renderers = {
  webgl2: {preset: "wasm-player", build: "out/build/wasm-player",
    sdl: {repository: "https://github.com/libsdl-org/SDL", commit: "8e37db5e797b6167f3a00d697d816a684bd259c7", version: "3.4.10"}},
  webgpu: {preset: "wasm-player-webgpu", build: "out/build/wasm-player-webgpu",
    sdl: {repository: "https://github.com/TheeStickmahn/SDL_wgpu", commit: "09026288bffe2bf9d7451255b766256a34bd8963",
      version: "3.5.0-dev", proposal: "https://github.com/libsdl-org/SDL/pull/16020"},
    emdawnwebgpu: "v20260423.175430"},
};

function parse(argv) {
  const values = new Map(), flags = new Set();
  const valued = new Set(["renderer", "package-root", "build-dir-webgl2", "build-dir-webgpu", "jobs", "native-build-root",
    "vcpkg-installed", "triplet", "shadercross", "spirv-cross", "naga", "allow-emscripten"]);
  for (let i = 0; i < argv.length; i++) {
    const argument = argv[i];
    if (!argument.startsWith("--")) throw new Error(`Unexpected argument: ${argument}`);
    const [name, inline] = argument.slice(2).split(/=(.*)/su);
    if (valued.has(name)) {
      const value = inline ?? argv[++i];
      if (value === undefined) throw new Error(`--${name} needs a value`);
      values.set(name, value);
    } else if (["skip-build", "no-host-tools", "update-builtin-wgsl", "help"].includes(name)) flags.add(name);
    else throw new Error(`Unknown option --${name}`);
  }
  return {values, flags};
}

function run(command, args, {cwd = root, capture = false, env = process.env} = {}) {
  return new Promise((accept, reject) => {
    const child = spawn(command, args, {cwd, env, windowsHide: true, stdio: ["ignore", capture ? "pipe" : "inherit", capture ? "pipe" : "inherit"]});
    let output = "", errors = "";
    child.stdout?.on("data", data => { output += data; });
    child.stderr?.on("data", data => { errors = (errors + data).slice(-8192); });
    child.on("error", reject);
    child.on("close", code => code === 0 ? accept(output)
      : reject(new Error(`${command} ${args.join(" ")} exited ${code}${errors ? `\n${errors}` : ""}`)));
  });
}
const exists = path => stat(path).then(() => true, () => false);
async function sha256(path) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return hash.digest("hex");
}
async function findOnPath(names) {
  for (const directory of (process.env.PATH ?? "").split(delimiter).filter(Boolean))
    for (const name of names) if (await exists(join(directory, name))) return join(directory, name);
  return undefined;
}

/** The active Emscripten (emsdk_env): its root, cache and version. */
async function emscripten(allowed) {
  let rootPath;
  if (process.env.EMSDK && await exists(join(process.env.EMSDK, "upstream", "emscripten")))
    rootPath = join(process.env.EMSDK, "upstream", "emscripten");
  else {
    const emcc = await findOnPath(windows ? ["emcc.bat", "emcc"] : ["emcc"]);
    if (emcc) rootPath = dirname(emcc);
  }
  if (!rootPath) throw new Error("Emscripten is not active. Activate the emsdk first (emsdk_env.sh / emsdk_env.ps1).");
  const version = JSON.parse(await readFile(join(rootPath, "emscripten-version.txt"), "utf8"));
  if (version !== (allowed ?? pinnedEmscripten))
    throw new Error(`The shipping Web Player is built with Emscripten ${pinnedEmscripten}; this is ${version}` +
      " (--allow-emscripten <version> overrides, for experiments only)");
  return {root: rootPath, cache: process.env.EM_CACHE ?? join(rootPath, "cache"), version,
    toolchain: join(rootPath, "cmake", "Modules", "Platform", "Emscripten.cmake")};
}

function defaultTriplet() {
  if (windows) return "x64-windows";
  if (process.platform === "darwin") return process.arch === "arm64" ? "arm64-osx" : "x64-osx";
  return process.arch === "arm64" ? "arm64-linux" : "x64-linux";
}

async function buildRenderer(name, buildDir, em, jobs) {
  const {preset} = renderers[name];
  // What emcmake does: the Emscripten toolchain, and Node.js to run any
  // configure-time test executables.
  await run("cmake", ["--preset", preset, "-B", buildDir, `-DPRISMATIX_WEB_RENDERER=${name}`, `-DCMAKE_TOOLCHAIN_FILE=${em.toolchain}`,
    `-DCMAKE_CROSSCOMPILING_EMULATOR=${process.execPath}`]);
  await run("cmake", ["--build", buildDir, "--target", "PrismatiXPlayerWeb", ...(jobs ? ["--parallel", jobs] : [])]);
}

/** Built-in compositor/rule shaders: native HLSL → SPIR-V → GLSL ES/WGSL (web-shaders.json). */
async function builtInShaders(tools, updateWgsl) {
  const {convertWebShader, sdlGpuWgsl} = await import(pathToFileURL(join(root, "packages/cli/dist/src/web-shader.js")).href)
    .catch(() => { throw new Error("Build the CLI first: pnpm run cli:build"); });
  const directory = join(root, "out/package/web-shaders");
  await mkdir(directory, {recursive: true});
  const shaders = {};
  const drift = [];
  for (const stem of ["compositor", "rule"]) {
    const spirv = join(directory, `${stem}.spv`);
    await run(tools.shadercross, [join(root, "PrismatiXEngine/Shaders/Builtin", `${stem}.frag.hlsl`),
      "--source", "HLSL", "--dest", "SPIRV", "--stage", "fragment", "--entrypoint", "main", "--output", spirv]);
    const shader = await convertWebShader(spirv, join(directory, stem), tools.spirvCross, tools.naga);
    await writeFile(join(directory, `${stem}.json`), JSON.stringify(shader));
    shaders[stem] = shader;
    // The WebGPU renderer embeds these at compile time; they must match this conversion.
    const committed = join(root, "PrismatiXEngine/Shaders/Builtin/webgpu", `${stem}.frag.wgsl`);
    const expected = sdlGpuWgsl(shader.wgsl);
    if ((await readFile(committed, "utf8").catch(() => "")) !== expected) {
      if (updateWgsl) await writeFile(committed, expected);
      else drift.push(committed);
    }
  }
  if (drift.length) throw new Error(`Built-in WGSL is out of date: ${drift.join(", ")}\n` +
    "Run with --update-builtin-wgsl and commit the result.");
  return shaders;
}

async function main() {
  const {values, flags} = parse(process.argv.slice(2));
  if (flags.has("help")) { process.stdout.write(`${(await readFile(fileURLToPath(import.meta.url), "utf8")).split("\n\n")[0]}\n`); return; }
  const requested = values.get("renderer") ?? "all";
  if (!["webgl2", "webgpu", "all"].includes(requested)) throw new Error("--renderer must be webgl2, webgpu or all");
  const selected = requested === "all" ? ["webgl2", "webgpu"] : [requested];
  const buildDirs = Object.fromEntries(selected.map(name =>
    [name, resolve(root, values.get(`build-dir-${name}`) ?? renderers[name].build)]));
  const destination = resolve(root, values.get("package-root") ?? "out/package/wasm-player");
  const hostTools = !flags.has("no-host-tools");

  const cliVersion = JSON.parse(await readFile(join(root, "packages/cli/package.json"), "utf8")).version;
  const sdkVersion = JSON.parse(await readFile(join(root, "packages/authoring-sdk/package.json"), "utf8")).version;
  if (cliVersion !== sdkVersion) throw new Error(`CLI ${cliVersion} and authoring SDK ${sdkVersion} versions differ`);

  const nativeRoot = resolve(root, values.get("native-build-root") ?? "out/build/x64-release");
  const installed = resolve(root, values.get("vcpkg-installed") ?? join(nativeRoot, "vcpkg_installed"));
  const triplet = values.get("triplet") ?? defaultTriplet();
  const tools = {
    shadercross: resolve(root, values.get("shadercross") ?? join(installed, triplet, "tools/sdl3-shadercross", `shadercross${exe}`)),
    spirvCross: resolve(root, values.get("spirv-cross") ?? join(installed, triplet, "tools/spirv-cross", `spirv-cross${exe}`)),
    naga: resolve(root, values.get("naga") ?? join("out/tools/naga/bin", `naga${exe}`)),
  };
  for (const tool of Object.values(tools))
    if (!await exists(tool)) throw new Error(`Missing offline shader converter: ${tool}. See docs/web-export.md.`);
  const nagaVersion = (await run(tools.naga, ["--version"], {capture: true})).trim();
  if (!/^(?:naga\s+)?27\.0\.0$/u.test(nagaVersion)) throw new Error(`Web shader conversion requires naga-cli 27.0.0; found ${nagaVersion}`);

  // Shaders first: the WebGPU build embeds the checked WGSL.
  const shaders = await builtInShaders(tools, flags.has("update-builtin-wgsl"));

  const em = await emscripten(values.get("allow-emscripten"));
  if (!flags.has("skip-build"))
    for (const name of selected) {
      process.stdout.write(`\n== Building the ${name} Web Player in ${buildDirs[name]}\n`);
      await buildRenderer(name, buildDirs[name], em, values.get("jobs"));
    }

  await rm(destination, {recursive: true, force: true});
  await mkdir(destination, {recursive: true});
  const artifacts = {};
  const record = async path => { artifacts[path] = await sha256(join(destination, path)); };

  // Byte-identical to what the former PowerShell packager wrote (Set-Content
  // ends the line with CRLF), so the file keeps its hash on every platform.
  await writeFile(join(destination, "web-shaders.json"), `${JSON.stringify(shaders)}\r\n`);
  await record("web-shaders.json");
  // FriBidi, the shared dependencies and their notices are identical in both
  // builds; take them from the first one.
  const primary = buildDirs[selected[0]];
  const notices = await collectSharedNotices(primary, destination);
  for (const path of notices) await record(path);
  await run(process.execPath, [join(root, "scripts/archive-web-runtime-source.mjs"), destination, primary]);
  const sources = ["RuntimeSource.tar.gz", "FriBidiSource.tar.gz"];
  for (const path of sources) await record(path);

  const rendererEntries = {};
  for (const name of selected) {
    const build = buildDirs[name], output = join(destination, name), entry = {directory: name, artifacts: {}};
    await mkdir(output, {recursive: true});
    for (const file of ["prismatix-player.mjs", "prismatix-player.wasm"]) {
      const source = join(build, "PrismatiXEngine", file);
      if (!await exists(source)) throw new Error(`Missing Web Player artifact: ${source}`);
      await copyFile(source, join(output, file));
    }
    let dawnPackage;
    if (name === "webgpu") {
      dawnPackage = join(em.cache, "ports", "emdawnwebgpu", "emdawnwebgpu_pkg");
      const dawn = await readFile(join(dawnPackage, "VERSION.txt"), "utf8").catch(() => "");
      if (!dawn.includes(renderers.webgpu.emdawnwebgpu))
        throw new Error(`The WebGPU build did not use emdawnwebgpu ${renderers.webgpu.emdawnwebgpu} (${join(dawnPackage, "VERSION.txt")})`);
    }
    const rendererNotices = (await collectRendererNotices(build, output, {dawnPackage})).map(path => `${name}/${path}`);
    for (const path of ["prismatix-player.mjs", "prismatix-player.wasm"].map(file => `${name}/${file}`).concat(rendererNotices))
      entry.artifacts[path] = await sha256(join(destination, path));
    rendererEntries[name] = {...entry, notices: rendererNotices, sdl: renderers[name].sdl,
      ...(renderers[name].emdawnwebgpu ? {emdawnwebgpu: renderers[name].emdawnwebgpu} : {}),
      emscripten: em.version, suspend: "asyncify",
      ...(name === "webgpu" ? {requires: "WebGPU (navigator.gpu)", video: "cpu-copy"} : {requires: "WebGL 2"})};
  }

  if (hostTools) {
    await mkdir(join(destination, "tools"), {recursive: true});
    await copyFile(tools.naga, join(destination, "tools", `naga${exe}`));
    await copyFile(tools.spirvCross, join(destination, "tools", `spirv-cross${exe}`));
    if (!windows) for (const name of ["naga", "spirv-cross"]) await chmod(join(destination, "tools", name), 0o755);
    await collectToolNotices(destination, {
      cargoRegistry: join(process.env.CARGO_HOME ?? join(homedir(), ".cargo"), "registry", "src"),
      spirvCrossCopyright: join(installed, triplet, "share/spirv-cross/copyright")});
  }

  const manifest = {format: "PrismatiXWebPlayer", schemaRevision: 2, variant: "shipped", status: "experimental",
    engineVersion: cliVersion, defaultRenderer: selected.includes("webgl2") ? "webgl2" : selected[0],
    renderers: rendererEntries, artifacts, notices, sources};
  await writeFile(join(destination, "web-player.json"), `${JSON.stringify(manifest, null, 2)}\n`);
  for (const name of selected) {
    const wasm = (await stat(join(destination, name, "prismatix-player.wasm"))).size;
    process.stdout.write(`${name}: prismatix-player.wasm ${wasm} bytes, sha256 ${rendererEntries[name].artifacts[`${name}/prismatix-player.wasm`]}\n`);
  }
  process.stdout.write(`Packaged shipping Web Player ${cliVersion} (${selected.join(", ")}): ${destination}\n`);
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href)
  await main().catch(error => { process.stderr.write(`${error.stack ?? error}\n`); process.exit(1); });
