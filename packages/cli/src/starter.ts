import {spawn} from "node:child_process";
import {randomUUID} from "node:crypto";
import {copyFile, lstat, mkdir, mkdtemp, readFile, readdir, rename, rm, rmdir, writeFile} from "node:fs/promises";
import {basename, dirname, extname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {externalizeStoryIds} from "@prismatix/authoring-sdk";

export interface StarterOptions {
  readonly name?: string;
  readonly id?: string;
  readonly locale?: string;
  readonly width?: number;
  readonly height?: number;
  readonly npmRegistry?: string;
  readonly npmCache?: string;
  /** Dependency-injection seam for isolated tests, not a CLI skip-install option. */
  readonly install?: (directory: string) => Promise<void>;
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

async function installDependencies(directory: string, options: StarterOptions): Promise<void> {
  // Use npm's JS entrypoint so Windows does not require shell command composition.
  const nodeRoot = dirname(process.execPath);
  const candidates = [process.env.npm_execpath,
    join(nodeRoot, "node_modules", "npm", "bin", "npm-cli.js"),
    join(nodeRoot, "..", "lib", "node_modules", "npm", "bin", "npm-cli.js"),
    join(nodeRoot, "..", "node_modules", "npm", "bin", "npm-cli.js")];
  let npmPath: string | undefined;
  for (const candidate of candidates) {
    if (candidate === undefined || extname(candidate) !== ".js") continue;
    try { if ((await lstat(candidate)).isFile()) { npmPath = candidate; break; } } catch { /* Next supported npm layout. */ }
  }
  if (npmPath === undefined) throw new Error("npm was not found beside Node.js. Install a supported Node.js distribution including npm, then retry create.");
  process.stderr.write("Preparing the project's pinned PrismatiX tools with npm…\n");
  const args = [npmPath, "install", "--ignore-scripts", "--no-audit", "--no-fund"];
  if (options.npmRegistry !== undefined) args.push("--registry", options.npmRegistry);
  if (options.npmCache !== undefined) args.push("--cache", options.npmCache);
  await new Promise<void>((accept, reject) => {
    const child = spawn(process.execPath, args, {cwd: directory, shell: false, windowsHide: true,
      stdio: ["ignore", "inherit", "inherit"]});
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
      if (code === 0 && !cancelled) accept();
      else reject(new Error(`npm dependency preparation failed (${code ?? signal}). Check registry/proxy access and retry create; the destination was not changed.`));
    });
  });
}

/** Prepare everything in a sibling directory and publish only after npm succeeds. */
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
    const storyPath = join(staging, "Story", "main.pxstory");
    const story = await readFile(storyPath, "utf8");
    const identities = externalizeStoryIds(story, {path: "Story/main.pxstory"});
    if (!identities.valid || identities.source === undefined || identities.map === undefined)
      throw new Error(`Starter Story identity preparation failed: ${identities.diagnostics
        .map((item) => `${item.code}: ${item.message}`).join("; ")}`);
    await writeFile(storyPath, identities.source, "utf8");
    await writeFile(`${storyPath}.pxids.json`, `${JSON.stringify(identities.map)}\n`, "utf8");
    await (options.install ?? ((directory) => installDependencies(directory, options)))(staging);
    if (await emptyDestination(target)) { await rmdir(target); removedEmptyTarget = true; }
    await rename(staging, target);
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
