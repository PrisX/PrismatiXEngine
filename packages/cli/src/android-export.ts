// `prismatix pack --target android`: turns a packed distribution plus a
// prebuilt Android Player (libmain.so and the FFmpeg shared libraries) into
// an APK, and for signed release builds an AAB, through the Gradle template
// in templates/android.

import {spawn} from "node:child_process";
import {copyFile, cp, link, lstat, mkdir, open, readdir, readFile, rm, stat, writeFile} from "node:fs/promises";
import {basename, delimiter, dirname, extname, isAbsolute, join, relative, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";

import type {ProjectAndroidDistribution} from "@prismatix/authoring-sdk";

export const androidTemplateRoot = fileURLToPath(new URL("../../templates/android", import.meta.url));

/** Marks a folder as owned by the Android exporter so reruns may clean it. */
const ownerMarker = ".prismatix-android-output";

export class AndroidExportError extends Error {
  constructor(message: string, readonly hint?: string) {
    super(message);
    this.name = "AndroidExportError";
  }
}

export interface AndroidPlayer {
  /** Folder that holds libmain.so. */
  readonly directory: string;
  /** Every shared library there (libmain.so first), as file names. */
  readonly libraries: readonly string[];
  readonly sdlVersion?: string;
}

const aarch64Machine = 183;

async function elfMachine(path: string): Promise<number | undefined> {
  const handle = await open(path, "r");
  try {
    const header = Buffer.alloc(20);
    const {bytesRead} = await handle.read(header, 0, header.length, 0);
    if (bytesRead < 20 || header.readUInt32BE(0) !== 0x7f454c46) return undefined;
    return header[5] === 2 ? header.readUInt16BE(18) : header.readUInt16LE(18);
  } finally { await handle.close(); }
}

/**
 * Accepts the arm64-v8a folder the android-arm64 preset writes
 * (out/build/android-arm64/android/shipped/arm64-v8a) or its parent.
 */
export async function resolveAndroidPlayer(argument: string): Promise<AndroidPlayer> {
  const candidates = [resolve(argument), join(resolve(argument), "arm64-v8a")];
  let directory: string | undefined;
  for (const candidate of candidates) {
    const info = await lstat(join(candidate, "libmain.so")).catch(() => undefined);
    if (info?.isFile()) { directory = candidate; break; }
  }
  if (directory === undefined)
    throw new AndroidExportError(`No libmain.so in ${resolve(argument)}`,
      "Pass the folder the android-arm64 build writes, e.g. " +
      "out/build/android-arm64/android/shipped/arm64-v8a");
  const libraries: string[] = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    if (!entry.isFile() || !/^lib[^/\\]+\.so$/u.test(entry.name)) continue;
    if (await elfMachine(join(directory, entry.name)) !== aarch64Machine)
      throw new AndroidExportError(`${entry.name} in ${directory} is not an arm64-v8a ELF library`);
    libraries.push(entry.name);
  }
  libraries.sort((left, right) => left === "libmain.so" ? -1 : right === "libmain.so" ? 1
    : left.localeCompare(right));
  let sdlVersion: string | undefined;
  const infoPath = join(directory, "prismatix-player.json");
  const info = await readFile(infoPath, "utf8").then((text) => JSON.parse(text) as unknown,
    (error: NodeJS.ErrnoException) => { if (error.code === "ENOENT") return undefined; throw error; });
  if (info !== undefined) {
    const record = info as Record<string, unknown>;
    if (record.variant === "qa")
      throw new AndroidExportError(`${directory} holds the QA Player, which must never ship`,
        "Use android/shipped/arm64-v8a instead of android/qa/arm64-v8a");
    if (typeof record.sdlVersion === "string") sdlVersion = record.sdlVersion;
  }
  return {directory, libraries, ...(sdlVersion === undefined ? {} : {sdlVersion})};
}

/** The SDL version the template's Java glue was copied from. */
export async function templateSdlVersion(template = androidTemplateRoot): Promise<string | undefined> {
  const text = await readFile(join(template, "app/src/main/java/org/libsdl/app/SDLActivity.java"), "utf8")
    .catch(() => undefined);
  if (text === undefined) return undefined;
  const part = (name: string) => new RegExp(`SDL_${name}_VERSION\\s*=\\s*(\\d+)`, "u").exec(text)?.[1];
  const [major, minor, micro] = [part("MAJOR"), part("MINOR"), part("MICRO")];
  return major === undefined || minor === undefined || micro === undefined
    ? undefined : `${major}.${minor}.${micro}`;
}

/** Escapes a value for a java.util.Properties file read as UTF-8. */
export function propertiesValue(value: string): string {
  return value.replace(/[\\=:#!]/gu, (match) => `\\${match}`)
    .replace(/\r/gu, "\\r").replace(/\n/gu, "\\n").replace(/\t/gu, "\\t")
    .replace(/^ /u, "\\ ");
}

/** An Android string resource body: XML-escaped and with aapt2's specials quoted. */
export function androidStringValue(value: string): string {
  const android = value.replace(/[\\'"]/gu, (match) => `\\${match}`)
    .replace(/\n/gu, "\\n").replace(/^([@?])/u, "\\$1");
  return android.replace(/&/gu, "&amp;").replace(/</gu, "&lt;").replace(/>/gu, "&gt;");
}

export interface AndroidSettings {
  readonly applicationId: string;
  readonly versionCode: number;
  readonly versionName: string;
  readonly label: string;
  readonly orientation: "sensorLandscape" | "sensorPortrait";
  readonly icon?: string;
  readonly keystore?: {
    readonly path: string; readonly alias: string;
    readonly storePasswordEnv: string; readonly keyPasswordEnv: string;
  };
}

export interface AndroidProjectFacts {
  readonly root: string;
  readonly id: string;
  readonly name: string;
  readonly version?: string;
  readonly width: number;
  readonly height: number;
}

/** Applies defaults to prismatix.json `distribution.android`. */
export function androidSettings(project: AndroidProjectFacts,
  android: ProjectAndroidDistribution | undefined, release: boolean): AndroidSettings {
  if (android?.applicationId === undefined)
    throw new AndroidExportError("prismatix.json distribution.android.applicationId is required " +
      "for --target android", "Add for example \"distribution\": {\"android\": " +
      "{\"applicationId\": \"com.example.mygame\"}}; it is the app's permanent store identity");
  const keystore = android.keystore;
  return {
    applicationId: android.applicationId,
    versionCode: android.versionCode ?? 1,
    versionName: android.versionName ?? project.version ?? "1.0.0",
    label: android.label ?? project.name,
    orientation: project.height > project.width ? "sensorPortrait" : "sensorLandscape",
    ...(android.icon === undefined ? {} : {icon: resolve(project.root, android.icon)}),
    ...(keystore === undefined || !release ? {} : {keystore: {
      path: resolve(project.root, keystore.path), alias: keystore.alias,
      storePasswordEnv: keystore.storePasswordEnv,
      keyPasswordEnv: keystore.keyPasswordEnv ?? keystore.storePasswordEnv,
    }}),
  };
}

export function androidPropertiesFile(settings: AndroidSettings): string {
  const values: [string, string][] = [
    ["applicationId", settings.applicationId],
    ["versionCode", String(settings.versionCode)],
    ["versionName", settings.versionName],
    ["screenOrientation", settings.orientation],
  ];
  if (settings.keystore !== undefined) values.push(
    ["keystore", settings.keystore.path], ["keyAlias", settings.keystore.alias],
    ["storePasswordEnv", settings.keystore.storePasswordEnv],
    ["keyPasswordEnv", settings.keystore.keyPasswordEnv]);
  return ["# Written by prismatix pack --target android. Passwords are read from",
    "# the environment variables named here, never from this file.",
    ...values.map(([key, value]) => `${key}=${propertiesValue(value)}`), ""].join("\n");
}

async function pngSize(path: string): Promise<{width: number; height: number} | undefined> {
  const handle = await open(path, "r");
  try {
    const header = Buffer.alloc(24);
    const {bytesRead} = await handle.read(header, 0, header.length, 0);
    if (bytesRead < 24 || !header.subarray(0, 8).equals(
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])) ||
        header.toString("latin1", 12, 16) !== "IHDR") return undefined;
    return {width: header.readUInt32BE(16), height: header.readUInt32BE(20)};
  } finally { await handle.close(); }
}

async function checkIcon(path: string): Promise<void> {
  const info = await lstat(path).catch(() => undefined);
  if (info === undefined || !info.isFile() || info.size > 4 * 1024 * 1024)
    throw new AndroidExportError(`distribution.android.icon must name a regular PNG file of at most 4 MiB: ${path}`);
  const size = await pngSize(path);
  if (size === undefined || size.width !== size.height || size.width < 48 || size.width > 4096)
    throw new AndroidExportError(`distribution.android.icon must be a square PNG from 48 to 4096 pixels: ${path}`,
      "512 x 512 matches the Play Store listing icon");
}

/** Files of a packed distribution that belong in the APK's assets/. */
export function isAndroidAsset(path: string, playerExecutable: string): boolean {
  const parts = path.split("/");
  if (path === "Package/cli-package-provenance.json") return false;
  if (parts.length > 1) return true;
  const lower = path.toLowerCase();
  return path !== playerExecutable && !/\.(?:dll|dylib|exe)$/u.test(lower) &&
    !/\.so(?:\.|$)/u.test(lower) && !path.startsWith(".");
}

async function listFiles(root: string, directory = root): Promise<string[]> {
  const output: string[] = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    const path = join(directory, entry.name);
    if (entry.isSymbolicLink()) throw new AndroidExportError(`Refusing to package a symbolic link: ${path}`);
    if (entry.isDirectory()) output.push(...await listFiles(root, path));
    else if (entry.isFile()) output.push(relative(root, path).split(sep).join("/"));
  }
  return output.sort();
}

// Assets are read-only Gradle inputs, so a hard link is as good as a copy and
// avoids duplicating a multi-GiB package on the same volume.
async function linkOrCopy(source: string, target: string): Promise<void> {
  await mkdir(dirname(target), {recursive: true});
  try { await link(source, target); }
  catch { await copyFile(source, target); }
}

/** Gives the output folder to the exporter, or refuses a foreign one. */
export async function claimAndroidOutput(output: string, protectedRoots: readonly string[]): Promise<void> {
  for (const root of protectedRoots) {
    const fromOutput = relative(output, root);
    if (fromOutput === "" || (!fromOutput.startsWith("..") && !isAbsolute(fromOutput)))
      throw new AndroidExportError(`Android output ${output} would contain the project folder ${root}`);
  }
  let entries: string[];
  try { entries = await readdir(output); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    entries = [];
  }
  if (entries.length > 0 && !entries.includes(ownerMarker))
    throw new AndroidExportError(`Android output ${output} is not empty and was not created by prismatix`,
      "Choose a new or empty --output folder");
  await mkdir(output, {recursive: true});
  await writeFile(join(output, ownerMarker),
    "This folder is written by prismatix pack --target android.\n", "utf8");
}

export interface GradleProjectInput {
  readonly project: string;
  readonly template?: string;
  readonly settings: AndroidSettings;
  readonly player: AndroidPlayer;
  readonly distribution: string;
  readonly playerExecutable: string;
  readonly androidSdk: string;
}

/**
 * Refreshes the Gradle project from the template. Gradle's own caches
 * (.gradle, build, app/build) survive so a rebuild is incremental.
 */
export async function stageGradleProject(input: GradleProjectInput): Promise<{assets: number}> {
  const template = input.template ?? androidTemplateRoot;
  const project = input.project;
  await mkdir(project, {recursive: true});
  const keep = new Set([".gradle", "build", "app"]);
  for (const name of await readdir(project)) if (!keep.has(name)) await rm(join(project, name), {recursive: true, force: true});
  const app = join(project, "app");
  await mkdir(app, {recursive: true});
  for (const name of await readdir(app)) if (name !== "build") await rm(join(app, name), {recursive: true, force: true});
  await cp(template, project, {recursive: true, force: true,
    filter: (source) => basename(source) !== ".gitkeep"});

  const main = join(app, "src", "main");
  await writeFile(join(project, "prismatix.properties"), androidPropertiesFile(input.settings), "utf8");
  await writeFile(join(project, "local.properties"),
    `sdk.dir=${propertiesValue(input.androidSdk)}\n`, "utf8");
  await writeFile(join(main, "res", "values", "strings.xml"), [
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>",
    "<resources>",
    `    <string name="app_name">${androidStringValue(input.settings.label)}</string>`,
    "</resources>", ""].join("\n"), "utf8");
  if (input.settings.icon !== undefined) {
    // A bitmap launcher icon replaces the template's adaptive placeholder;
    // Android scales the xxxhdpi bitmap for other densities.
    await rm(join(main, "res", "mipmap-anydpi-v26"), {recursive: true, force: true});
    const target = join(main, "res", "mipmap-xxxhdpi", "ic_launcher.png");
    await mkdir(dirname(target), {recursive: true});
    await copyFile(input.settings.icon, target);
  }
  const jniLibs = join(main, "jniLibs", "arm64-v8a");
  await mkdir(jniLibs, {recursive: true});
  for (const library of input.player.libraries)
    await copyFile(join(input.player.directory, library), join(jniLibs, library));
  let assets = 0;
  for (const path of await listFiles(input.distribution)) {
    if (!isAndroidAsset(path, input.playerExecutable)) continue;
    await linkOrCopy(join(input.distribution, ...path.split("/")), join(main, "assets", ...path.split("/")));
    assets += 1;
  }
  if (!assets || !(await lstat(join(main, "assets", "Package", "manifest.json")).catch(() => undefined))?.isFile())
    throw new AndroidExportError(`The packed distribution ${input.distribution} has no Package/manifest.json`);
  return {assets};
}

async function executableOnPath(names: readonly string[]): Promise<string | undefined> {
  for (const directory of (process.env.PATH ?? "").split(delimiter).filter(Boolean)) {
    for (const name of names) {
      const candidate = join(directory, name);
      if ((await stat(candidate).catch(() => undefined))?.isFile()) return candidate;
    }
  }
  return undefined;
}

/** --gradle, then GRADLE_HOME/bin, then gradle on PATH. */
export async function findGradle(option: string | undefined): Promise<string> {
  const names = process.platform === "win32" ? ["gradle.bat", "gradle.cmd", "gradle.exe"] : ["gradle"];
  if (option !== undefined) {
    const path = resolve(option);
    const info = await stat(path).catch(() => undefined);
    if (info?.isFile()) return path;
    if (info?.isDirectory()) {
      for (const name of names) for (const candidate of [join(path, name), join(path, "bin", name)])
        if ((await stat(candidate).catch(() => undefined))?.isFile()) return candidate;
    }
    throw new AndroidExportError(`--gradle does not name a Gradle executable: ${path}`);
  }
  const home = process.env.GRADLE_HOME;
  if (home !== undefined && home.length > 0) {
    for (const name of names) {
      const candidate = join(home, "bin", name);
      if ((await stat(candidate).catch(() => undefined))?.isFile()) return candidate;
    }
  }
  const found = await executableOnPath(names);
  if (found === undefined)
    throw new AndroidExportError("Gradle could not be found",
      "Pass --gradle <gradle executable or installation>, set GRADLE_HOME, or put gradle on PATH (Gradle 8.9 or newer)");
  return found;
}

/** --android-sdk, then ANDROID_HOME, then ANDROID_SDK_ROOT. */
export async function findAndroidSdk(option: string | undefined): Promise<string> {
  const value = option ?? process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
  if (value === undefined || value.length === 0)
    throw new AndroidExportError("The Android SDK could not be found",
      "Pass --android-sdk <dir> or set ANDROID_HOME (needs platforms;android-35 and build-tools)");
  const path = resolve(value);
  if (!(await stat(join(path, "platforms")).catch(() => undefined))?.isDirectory())
    throw new AndroidExportError(`${path} is not an Android SDK (no platforms folder)`);
  return path;
}

export function checkKeystoreEnvironment(settings: AndroidSettings): void {
  if (settings.keystore === undefined) return;
  for (const name of new Set([settings.keystore.storePasswordEnv, settings.keystore.keyPasswordEnv]))
    if ((process.env[name] ?? "").length === 0)
      throw new AndroidExportError(`Environment variable ${name} must hold the keystore password`,
        "distribution.android.keystore names the variables; set them in the shell or CI secret store");
}

export interface GradleRun {
  readonly code: number;
  readonly tail: string;
}

function quoteForCmd(value: string): string {
  if (/["%!^\r\n]/u.test(value)) throw new AndroidExportError(`Unsupported character in Gradle argument: ${value}`);
  return `"${value}"`;
}

/** Runs Gradle with its output on stderr, so --json stdout stays parseable. */
export function runGradle(gradle: string, project: string, tasks: readonly string[]): Promise<GradleRun> {
  const args = ["-p", project, "--console=plain", ...tasks];
  const extension = extname(gradle).toLowerCase();
  let command: string;
  let commandArgs: string[];
  let verbatim = false;
  if ([".js", ".mjs", ".cjs"].includes(extension)) {
    command = process.execPath; commandArgs = [gradle, ...args];
  } else if (process.platform === "win32" && [".bat", ".cmd"].includes(extension)) {
    // Batch files need cmd.exe; quote every argument ourselves.
    command = process.env.ComSpec ?? "cmd.exe";
    commandArgs = ["/d", "/s", "/c", `"${[gradle, ...args].map(quoteForCmd).join(" ")}"`];
    verbatim = true;
  } else { command = gradle; commandArgs = args; }
  return new Promise((accept, reject) => {
    const child = spawn(command, commandArgs, {cwd: project, stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true, windowsVerbatimArguments: verbatim});
    let tail = "";
    const forward = (chunk: Buffer) => {
      process.stderr.write(chunk);
      tail = (tail + chunk.toString("utf8")).slice(-8192);
    };
    child.stdout.on("data", forward);
    child.stderr.on("data", forward);
    child.on("error", reject);
    child.on("close", (code) => accept({code: code ?? 1, tail}));
  });
}

export interface AndroidOutputs {
  readonly apk: string;
  readonly aab?: string;
}

/** Copies Gradle's outputs to <output>/<id>[-debug].apk and <id>.aab. */
export async function collectAndroidOutputs(project: string, output: string, id: string,
  release: boolean): Promise<AndroidOutputs> {
  const outputs = join(project, "app", "build", "outputs");
  const apkSource = release ? join(outputs, "apk", "release", "app-release.apk")
    : join(outputs, "apk", "debug", "app-debug.apk");
  if (!(await stat(apkSource).catch(() => undefined))?.isFile())
    throw new AndroidExportError(`Gradle finished without writing ${apkSource}`);
  const apk = join(output, `${id}${release ? "" : "-debug"}.apk`);
  await copyFile(apkSource, apk);
  if (!release) return {apk};
  const aabSource = join(outputs, "bundle", "release", "app-release.aab");
  if (!(await stat(aabSource).catch(() => undefined))?.isFile())
    throw new AndroidExportError(`Gradle finished without writing ${aabSource}`);
  const aab = join(output, `${id}.aab`);
  await copyFile(aabSource, aab);
  return {apk, aab};
}

/** Checks the icon and keystore files before anything is packed. */
export async function checkAndroidInputs(settings: AndroidSettings): Promise<void> {
  if (settings.icon !== undefined) await checkIcon(settings.icon);
  if (settings.keystore !== undefined &&
      !(await stat(settings.keystore.path).catch(() => undefined))?.isFile())
    throw new AndroidExportError(`distribution.android.keystore.path does not name a file: ${settings.keystore.path}`);
}
