import {readFile, realpath} from "node:fs/promises";
import {join} from "node:path";
import {pathToFileURL} from "node:url";

import * as bundledSdk from "@prismatix/authoring-sdk";

export type Sdk = typeof bundledSdk;

/** Which Authoring SDK compiles a project's Story. `auto` prefers the exact
 * SDK the project pins in its own node_modules, so editor diagnostics match
 * `prismatix validate`, and falls back to the SDK bundled with the extension. */
export type SdkMode = "auto" | "bundled";

export interface LoadedSdk {
  readonly sdk: Sdk;
  readonly source: "bundled" | "project";
  /** File URL of the project SDK entry; undefined for the bundled SDK. */
  readonly url?: string;
  readonly version?: string;
  /** Why the project SDK was not used, when it exists but failed to load. */
  readonly note?: string;
}

const requiredFunctions = [
  "parseStory", "compileStory", "compileStoryProject", "normalizeProject",
  "createStoryAssetIndex", "storyCommandCompletions", "validateDocument",
] as const;

const projectSdks = new Map<string, Promise<Sdk>>();

function usable(module: Record<string, unknown>): module is Record<string, unknown> & Sdk {
  return requiredFunctions.every((name) => typeof module[name] === "function");
}

async function projectSdkEntry(root: string): Promise<{url: string; version?: string} | undefined> {
  const packageRoot = join(root, "node_modules", "@prismatix", "authoring-sdk");
  let manifest: {version?: unknown; main?: unknown; exports?: unknown};
  try {
    manifest = JSON.parse(await readFile(join(packageRoot, "package.json"), "utf8")) as typeof manifest;
  } catch {
    return undefined;
  }
  const exports = manifest.exports as Record<string, unknown> | undefined;
  const dot = exports?.["."] as Record<string, unknown> | string | undefined;
  const relative = typeof dot === "string" ? dot
    : typeof dot?.import === "string" ? dot.import
      : typeof dot?.default === "string" ? dot.default
        : typeof manifest.main === "string" ? manifest.main : undefined;
  if (relative === undefined) return undefined;
  const entry = await realpath(join(packageRoot, ...relative.split("/")));
  return {url: pathToFileURL(entry).href,
    ...(typeof manifest.version === "string" ? {version: manifest.version} : {})};
}

/** Imports an SDK entry by URL; used by the language server and the project
 * validation worker so both compile with the same SDK. */
export async function importSdk(url: string | undefined): Promise<Sdk> {
  if (url === undefined) return bundledSdk;
  let pending = projectSdks.get(url);
  if (pending === undefined) {
    pending = import(url).then((module: Record<string, unknown>) => {
      if (!usable(module))
        throw new Error("the installed SDK lacks the Story editor API " +
          `(${requiredFunctions.join(", ")})`);
      return module;
    });
    projectSdks.set(url, pending);
    pending.catch(() => projectSdks.delete(url));
  }
  return pending;
}

export async function loadSdk(root: string, mode: SdkMode): Promise<LoadedSdk> {
  if (mode === "bundled") return {sdk: bundledSdk, source: "bundled"};
  let entry: Awaited<ReturnType<typeof projectSdkEntry>>;
  try {
    entry = await projectSdkEntry(root);
  } catch (error) {
    return {sdk: bundledSdk, source: "bundled",
      note: `project SDK could not be resolved: ${String(error)}`};
  }
  if (entry === undefined) return {sdk: bundledSdk, source: "bundled"};
  try {
    return {sdk: await importSdk(entry.url), source: "project", url: entry.url,
      ...(entry.version === undefined ? {} : {version: entry.version})};
  } catch (error) {
    return {sdk: bundledSdk, source: "bundled",
      note: `project SDK ${entry.url} failed to load: ${error instanceof Error ? error.message : String(error)}`};
  }
}

export {bundledSdk};
