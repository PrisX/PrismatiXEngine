import {readFile, realpath, stat} from "node:fs/promises";
import {isAbsolute, relative, resolve, sep} from "node:path";

import type {StoryIndexDocument} from "./story-project.js";

const maxDocumentBytes = 16 * 1024 * 1024;

function projectRelativePath(path: string): boolean {
  return path.length > 0 && path.length <= 4096 && !path.startsWith("/") &&
    !path.includes("\\") && !path.includes(":") && !path.includes("\0") &&
    path.split("/").every(part => part.length > 0 && part !== "." && part !== "..");
}

function missing(error: unknown): boolean {
  return typeof error === "object" && error !== null &&
    "code" in error && error.code === "ENOENT";
}

/** Node filesystem adapter for the pure Story project compiler. It loads each
 * indexed source and its adjacent, author-only identity map when present. */
export async function loadStorySourceFiles(
  projectRoot: string, storyIndex: StoryIndexDocument,
): Promise<Record<string, string>> {
  const root = await realpath(resolve(projectRoot));
  const sourceFiles = Object.create(null) as Record<string, string>;
  const sourcePaths = new Set<string>();
  for (const scene of storyIndex.scenes)
    for (const path of Object.values(scene.sources)) sourcePaths.add(path);

  const readBounded = async (path: string, optional: boolean): Promise<string | undefined> => {
    if (!projectRelativePath(path)) throw new Error(`Unsafe Story source path: ${path}`);
    const lexical = resolve(root, ...path.split("/"));
    let canonical: string;
    try { canonical = await realpath(lexical); }
    catch (error) { if (optional && missing(error)) return undefined; throw error; }
    const back = relative(root, canonical);
    if (back === ".." || back.startsWith(`..${sep}`) || isAbsolute(back))
      throw new Error(`Story source escapes project root: ${path}`);
    const info = await stat(canonical);
    if (!info.isFile() || info.size > maxDocumentBytes)
      throw new Error(`Story source is not a file or exceeds 16 MiB: ${path}`);
    const text = await readFile(canonical, "utf8");
    if (Buffer.byteLength(text, "utf8") > maxDocumentBytes)
      throw new Error(`Story source exceeds 16 MiB: ${path}`);
    return text;
  };

  for (const path of sourcePaths) {
    sourceFiles[path] = (await readBounded(path, false))!;
    const mapPath = `${path}.pxids.json`;
    const map = await readBounded(mapPath, true);
    if (map !== undefined) sourceFiles[mapPath] = map;
  }
  return sourceFiles;
}
