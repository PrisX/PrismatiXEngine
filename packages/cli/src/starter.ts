import {spawn} from "node:child_process";
import {randomUUID} from "node:crypto";
import {copyFile, lstat, mkdir, mkdtemp, readFile, readdir, rename, rm, rmdir, writeFile} from "node:fs/promises";
import {basename, dirname, extname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {externalizeStoryIds, serializeStoryIdentityMap} from "@prismatix/authoring-sdk";

export interface StarterOptions {
  readonly name?: string;
  readonly id?: string;
  readonly locale?: string;
  readonly width?: number;
  readonly height?: number;
  /**
   * Start without the sample story's art: no character, no backgrounds, a
   * plain title backdrop and a one-line Story to write over.
   */
  readonly empty?: boolean;
  readonly npmRegistry?: string;
  readonly npmCache?: string;
  /**
   * Where the project's pinned tools come from: this CLI's registry version,
   * or `file:` links to the engine checkout it runs from. Default: the
   * checkout when there is one.
   */
  readonly toolSource?: "registry" | "checkout";
  /**
   * Installs the pinned tools with npm (package-lock.json) or pnpm
   * (pnpm-lock.yaml). Default: whichever package manager ran this CLI.
   */
  readonly packageManager?: PackageManager;
  /**
   * Captures the package manager's output and shows it only in the error of a
   * failed install, for interactive create, which draws its own progress.
   */
  readonly quiet?: boolean;
  /** Called when the install starts, with the package manager that runs it. */
  readonly onInstall?: (manager: PackageManager) => void;
  /** Dependency-injection seam for isolated tests, not a CLI skip-install option. */
  readonly install?: (directory: string) => Promise<void>;
}

export type PackageManager = "npm" | "pnpm";

/** The package manager that launched this process (`pnpm dlx`, `pnpm exec`, ...), else npm. */
export function detectPackageManager(environment: NodeJS.ProcessEnv = process.env): PackageManager {
  return (environment.npm_config_user_agent ?? "").startsWith("pnpm/") ? "pnpm" : "npm";
}

const templateRoot = fileURLToPath(new URL("../../templates/minimal", import.meta.url));
const textExtensions = new Set([".json", ".pxstory", ".pxui", ".tsx", ".pxcharacter", ".md", ".txt"]);
const jsonText = (value: string): string => JSON.stringify(value).slice(1, -1);

async function emptyDestination(target: string): Promise<boolean> {
  let info;
  try { info = await lstat(target); }
  catch (error) { if ((error as NodeJS.ErrnoException).code === "ENOENT") return false; throw error; }
  if (info.isSymbolicLink() || !info.isDirectory() || (await readdir(target)).length !== 0)
    throw new Error(`Destination must be an empty directory, not a link or existing project: ${target}`);
  return true;
}

const isFile = (path: string): Promise<boolean> => lstat(path).then((info) => info.isFile(), () => false);

/**
 * Finds npm's JS entry beside Node.js, so Windows needs no shell command
 * composition. Returns the argv prefix that runs it.
 */
async function npmCommand(): Promise<string[]> {
  const nodeRoot = dirname(process.execPath);
  const candidates = [process.env.npm_execpath,
    join(nodeRoot, "node_modules", "npm", "bin", "npm-cli.js"),
    join(nodeRoot, "..", "lib", "node_modules", "npm", "bin", "npm-cli.js"),
    join(nodeRoot, "..", "node_modules", "npm", "bin", "npm-cli.js")];
  for (const candidate of candidates) {
    if (candidate === undefined || extname(candidate) !== ".js" || basename(candidate).startsWith("pnpm")) continue;
    if (await isFile(candidate)) return [process.execPath, candidate];
  }
  throw new Error("npm was not found beside Node.js. Install a supported Node.js distribution including npm, then retry create.");
}

/**
 * Finds pnpm without a shell: the pnpm that launched this process, else the
 * first pnpm on PATH, as a standalone executable, an npm-installed package
 * or a Corepack shim.
 */
async function pnpmCommand(): Promise<string[]> {
  const launcher = process.env.npm_execpath;
  if (launcher !== undefined && /^pnpm(\.c?js)?$/u.test(basename(launcher)) && await isFile(launcher))
    return extname(launcher) === "" ? [launcher] : [process.execPath, launcher];
  const windows = process.platform === "win32";
  for (const directory of (process.env.PATH ?? process.env.Path ?? "").split(windows ? ";" : ":")) {
    if (!directory) continue;
    if (windows && await isFile(join(directory, "pnpm.exe"))) return [join(directory, "pnpm.exe")];
    if (!windows && await isFile(join(directory, "pnpm"))) return [join(directory, "pnpm")];
    const shim = windows ? await isFile(join(directory, "pnpm.cmd")) : false;
    if (!windows || !shim) continue;
    for (const script of [join(directory, "node_modules", "pnpm", "bin", "pnpm.cjs"),
      join(directory, "node_modules", "corepack", "dist", "pnpm.js")])
      if (await isFile(script)) return [process.execPath, script];
  }
  throw new Error("pnpm was not found on PATH. Install pnpm (https://pnpm.io/installation) or enable it with `corepack enable pnpm`, then retry create, or pass --package-manager npm.");
}

/** Whether pnpm can be found, so interactive create offers it. */
export async function pnpmInstalled(): Promise<boolean> {
  return pnpmCommand().then(() => true, () => false);
}

/** Runs a package manager in `directory`; a failure leaves the destination unchanged. */
async function runPackageManager(command: string[], args: string[], directory: string,
  failure: string, quiet = false): Promise<string> {
  return new Promise<string>((accept, reject) => {
    const [executable, ...prefix] = command;
    const capture = quiet || args[0] === "--version";
    const child = spawn(executable!, [...prefix, ...args], {cwd: directory, shell: false, windowsHide: true,
      stdio: ["ignore", capture ? "pipe" : "inherit", quiet ? "pipe" : "inherit"]});
    let output = "";
    let log = "";
    const keep = (chunk: string): void => { log = (log + chunk).slice(-4000); };
    child.stdout?.setEncoding("utf8").on("data", (chunk: string) => { output += chunk; keep(chunk); });
    child.stderr?.setEncoding("utf8").on("data", keep);
    let cancelled = false;
    const cancel = () => { cancelled = true; child.kill(); };
    const timer = setTimeout(cancel, 180_000);
    const cleanup = () => {
      clearTimeout(timer);
      process.removeListener("SIGINT", cancel); process.removeListener("SIGTERM", cancel);
    };
    process.once("SIGINT", cancel); process.once("SIGTERM", cancel);
    child.once("error", (error) => { cleanup(); reject(error); });
    child.once("close", (code, signal) => {
      cleanup();
      if (code === 0 && !cancelled) accept(output.trim());
      else reject(new Error(`${failure} (${code ?? signal}).${quiet && log.trim() ? `\n${log.trim()}` : ""}`));
    });
  });
}

const preparationFailure = (manager: PackageManager): string =>
  `${manager} dependency preparation failed. Check registry/proxy access and retry create; the destination was not changed`;

async function installDependencies(directory: string, options: StarterOptions,
  manager: PackageManager, command: string[]): Promise<void> {
  if (options.onInstall) options.onInstall(manager);
  else process.stderr.write(`Preparing the project's pinned PrismatiX tools with ${manager}…\n`);
  const args = manager === "npm"
    ? ["install", "--ignore-scripts", "--no-audit", "--no-fund"]
    // A project created inside a pnpm workspace is still its own project.
    : ["install", "--ignore-scripts", "--ignore-workspace", "--config.confirmModulesPurge=false"];
  if (options.npmRegistry !== undefined) args.push("--registry", options.npmRegistry);
  if (options.npmCache !== undefined) args.push(...manager === "npm"
    ? ["--cache", options.npmCache]
    : ["--store-dir", join(options.npmCache, "pnpm-store"), "--cache-dir", join(options.npmCache, "pnpm-cache")]);
  await runPackageManager(command, args, directory, preparationFailure(manager), options.quiet);
}

/**
 * pnpm links node_modules with absolute junctions on Windows, which the move
 * from the staging directory breaks; relinking from the store needs no network.
 */
async function relinkDependencies(directory: string, options: StarterOptions, command: string[]): Promise<void> {
  const args = ["install", "--offline", "--frozen-lockfile", "--ignore-scripts", "--ignore-workspace",
    "--config.confirmModulesPurge=false"];
  if (options.npmCache !== undefined)
    args.push("--store-dir", join(options.npmCache, "pnpm-store"), "--cache-dir", join(options.npmCache, "pnpm-cache"));
  await runPackageManager(command, args, directory,
    "pnpm could not relink the created project's node_modules from its store; run pnpm install in the project",
    options.quiet);
}

/**
 * Pins the project's tools to this CLI's own version. A CLI running from an
 * engine source checkout links the checkout's packages instead (`file:` for
 * npm, `link:` for pnpm, whose `file:` would copy them), so `create` works
 * offline and the project follows the engine you build. A pnpm project also
 * records the pnpm version that created it.
 */
async function pinTools(packagePath: string, id: string, source: "registry" | "checkout" | undefined,
  manager: PackageManager, managerVersion: string | undefined): Promise<void> {
  const cliRoot = fileURLToPath(new URL("../../", import.meta.url));
  const sdkRoot = join(cliRoot, "..", "authoring-sdk");
  const own = JSON.parse(await readFile(join(cliRoot, "package.json"), "utf8")) as {version: string};
  const isCheckout = source === "checkout" || (source === undefined &&
    await lstat(join(cliRoot, "src")).then(() => true, () => false) &&
    await lstat(join(sdkRoot, "package.json")).then(() => true, () => false));
  const link = (path: string) => `${manager === "pnpm" ? "link" : "file"}:${resolve(path).split("\\").join("/")}`;
  const project = JSON.parse(await readFile(packagePath, "utf8")) as Record<string, unknown>;
  project.name = id.toLowerCase().replace(/[^a-z0-9._-]+/gu, "-");
  project.devDependencies = isCheckout
    ? {"@prismatix/cli": link(cliRoot), "@prismatix/authoring-sdk": link(sdkRoot)}
    : {"@prismatix/cli": own.version, "@prismatix/authoring-sdk": own.version};
  if (manager === "pnpm" && managerVersion !== undefined && /^\d+\.\d+\.\d+$/u.test(managerVersion))
    project.packageManager = `pnpm@${managerVersion}`;
  await writeFile(packagePath, `${JSON.stringify(project, null, 2)}\n`, "utf8");
}

const emptyStory: Record<string, string> = {
  zh: "; 在這裡寫故事。直接打字就是旁白，@名字 讓角色說話。\n*start\n故事從這裡開始。\n[end]\n",
  ja: "; ここに物語を書きます。そのまま書けば地の文、@名前 でキャラクターの台詞です。\n*start\n物語はここから始まる。\n[end]\n",
  en: "; Write the story here. Plain lines are narration; @name makes a character speak.\n*start\nThe story begins here.\n[end]\n",
};

/** Removes the sample art and story from a copied template (`create --empty`). */
async function emptyStarter(root: string, locale: string): Promise<void> {
  for (const entry of await readdir(join(root, "Assets"))) await rm(join(root, "Assets", entry), {force: true});
  await rm(join(root, "Characters"), {recursive: true, force: true});
  await rm(join(root, "ASSET_PROVENANCE.md"), {force: true});
  const manifestPath = join(root, "prismatix.json");
  const manifest = (await readFile(manifestPath, "utf8")).split("\n")
    .filter((line) => !/"kind":"(background|provenance)"|^\s*"characters":/u.test(line)).join("\n")
    .replace(/,(\s*\n\s*[\]}])/gu, "$1");
  JSON.parse(manifest);
  await writeFile(manifestPath, manifest, "utf8");
  const titlePath = join(root, "Content", "UI", "Title.tsx");
  const title = (await readFile(titlePath, "utf8"))
    .replace("import {Button, ColorRect, Group, Image, Scene, Text, actions}", "import {Button, ColorRect, Group, Scene, Text, actions}")
    .replace(/<Image name="Backdrop"[^>]*\/>/u, "<ColorRect name=\"Backdrop\" fill color=\"#101014\" />");
  await writeFile(titlePath, title, "utf8");
  const language = locale.toLowerCase().split("-")[0]!;
  await writeFile(join(root, "Story", "main.pxstory"), emptyStory[language] ?? emptyStory.en!, "utf8");
  const readmePath = join(root, "README.md");
  await writeFile(readmePath, (await readFile(readmePath, "utf8")).replace(
    "This short Traditional Chinese story includes a guide, a background, two choices and two endings.",
    "The project starts empty: a title screen and a one-line story to write over."), "utf8");
}

/** Prepare everything in a sibling directory and publish only after the install succeeds. */
export async function createStarter(targetArgument: string, options: StarterOptions = {}): Promise<{root: string; project: string}> {
  const target = resolve(targetArgument);
  await emptyDestination(target);
  const name = options.name ?? "My PrismatiX Game";
  const id = options.id ?? (basename(target).toLowerCase().replace(/[^a-z0-9._-]+/gu, "-").replace(/^[^a-z0-9]+/u, "").slice(0, 256) || "my-game");
  const locale = options.locale ?? "zh-TW";
  const width = options.width ?? 1280;
  const height = options.height ?? 720;
  if (!name.trim() || name.length > 512) throw new Error("Project name must contain 1–512 characters");
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,255}$/u.test(id)) throw new Error("Project id must use letters, digits, '.', '_' or '-'");
  if (!/^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$/u.test(locale)) throw new Error("Locale must be a BCP 47 language tag");
  if (![width, height].every((value) => Number.isSafeInteger(value) && value >= 1 && value <= 16384)) throw new Error("Width and height must be integers from 1 to 16384");
  const replacements: Record<string, string> = {
    "{{PROJECT_ID}}": jsonText(id), "{{PROJECT_NAME}}": jsonText(name),
    "{{LOCALE}}": jsonText(locale), "{{WIDTH}}": String(width), "{{HEIGHT}}": String(height),
  };
  for (const token of ["SCENE_ID", "ROOT_ID", "TITLE_ID", "START_ID", "LOAD_ID"])
    replacements[`{{${token}}}`] = randomUUID();
  await mkdir(dirname(target), {recursive: true});
  const lock = join(dirname(target), `.${basename(target)}.prismatix-create.lock`);
  try { await mkdir(lock); }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === "EEXIST")
      throw new Error(`Another create owns ${target}. Wait for it to finish. If it was interrupted, inspect and remove the empty lock directory ${lock} before retrying.`);
    throw error;
  }
  let staging: string;
  try { staging = await mkdtemp(join(dirname(target), `.${basename(target)}.prismatix-create-`)); }
  catch (error) { await rmdir(lock); throw error; }
  let removedEmptyTarget = false;
  try {
    const copy = async (source: string, destination: string): Promise<void> => {
      await mkdir(destination, {recursive: true});
      for (const entry of await readdir(source, {withFileTypes: true})) {
        const from = join(source, entry.name);
        const filename = entry.name === "locale.json" ? `${locale}.json`
          : entry.name === "gitignore.txt" ? ".gitignore" : entry.name;
        const to = join(destination, filename);
        if (entry.isSymbolicLink()) throw new Error(`Starter template contains a symbolic link: ${from}`);
        if (entry.isDirectory()) { await copy(from, to); continue; }
        if (textExtensions.has(extname(entry.name)) || entry.name === ".gitignore") {
          let content = await readFile(from, "utf8");
          for (const [token, value] of Object.entries(replacements)) content = content.replaceAll(token, value);
          await writeFile(to, content, "utf8");
        } else await copyFile(from, to);
      }
    };
    await copy(templateRoot, staging);
    if (options.empty === true) await emptyStarter(staging, locale);
    const manager = options.packageManager ?? detectPackageManager();
    const command = options.install === undefined
      ? manager === "pnpm" ? await pnpmCommand() : await npmCommand() : [];
    const managerVersion = manager === "pnpm" && options.install === undefined
      ? await runPackageManager(command, ["--version"], staging, "pnpm --version failed") : undefined;
    await pinTools(join(staging, "package.json"), id, options.toolSource, manager, managerVersion);
    const storyPath = join(staging, "Story", "main.pxstory");
    const story = await readFile(storyPath, "utf8");
    const identities = externalizeStoryIds(story, {path: "Story/main.pxstory"});
    if (!identities.valid || identities.source === undefined || identities.map === undefined)
      throw new Error(`Starter Story identity preparation failed: ${identities.diagnostics
        .map((item) => `${item.code}: ${item.message}`).join("; ")}`);
    await writeFile(storyPath, identities.source, "utf8");
    await writeFile(`${storyPath}.pxids.json`, serializeStoryIdentityMap(identities.map), "utf8");
    await (options.install ?? ((directory) => installDependencies(directory, options, manager, command)))(staging);
    if (await emptyDestination(target)) { await rmdir(target); removedEmptyTarget = true; }
    await rename(staging, target);
    // The project is complete once moved; a failed relink only needs `pnpm install`.
    if (manager === "pnpm" && options.install === undefined)
      await relinkDependencies(target, options, command).catch((error: unknown) => {
        process.stderr.write(`Warning: ${error instanceof Error ? error.message : String(error)}
`);
      });
    return {root: target, project: join(target, "prismatix.json")};
  } catch (error) {
    if (removedEmptyTarget) await mkdir(target).catch(() => {});
    throw error;
  } finally {
    try {
      // Windows may report ENOTEMPTY while another process briefly holds a
      // freshly written template entry. Retry cleanup before releasing ownership.
      await rm(staging, {recursive: true, force: true,
        maxRetries: 5, retryDelay: 50});
    } finally {
      await rmdir(lock);
    }
  }
}
