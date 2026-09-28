import {createHash} from "node:crypto";

import {isSafeResourcePath} from "./resources.js";

/** Story commands whose resource arguments may use short, root-relative names. */
export const storyAssetCategories = [
  "bg", "rule", "sprite", "voice", "bgm", "se", "ambience", "video",
] as const;

export type StoryAssetCategory = typeof storyAssetCategories[number];

/** Author-only `prismatix.json` field: search roots per Story resource kind. */
export type StoryAssetRoots = Readonly<Partial<Record<StoryAssetCategory, readonly string[]>>>;

export type StoryAssetLookup =
  | {readonly kind: "resolved"; readonly path: string}
  | {readonly kind: "ambiguous"; readonly candidates: readonly string[]}
  | {readonly kind: "missing"; readonly roots: readonly string[]};

export interface StoryAssetIndex {
  /** Changes whenever a root or a file visible through a root changes. */
  readonly fingerprint: string;
  resolve(category: StoryAssetCategory, name: string): StoryAssetLookup | undefined;
}

const maximumRootsPerCategory = 64;

export function isStoryAssetCategory(value: string): value is StoryAssetCategory {
  return (storyAssetCategories as readonly string[]).includes(value);
}

/** Returns a reason when an author-supplied `storyAssets` value is malformed. */
export function storyAssetRootsError(value: unknown): string | undefined {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    return "storyAssets must be an object keyed by Story resource kind";
  for (const [category, roots] of Object.entries(value)) {
    if (!isStoryAssetCategory(category))
      return `storyAssets.${category} is not a Story resource kind; expected ${storyAssetCategories.join(", ")}`;
    if (!Array.isArray(roots) || roots.length === 0 || roots.length > maximumRootsPerCategory ||
        !roots.every((root: unknown) => typeof root === "string" && isSafeResourcePath(root) &&
          !root.includes(":") && root.split("/").every((part) => part.length > 0 && part !== ".")))
      return `storyAssets.${category} must list 1-${maximumRootsPerCategory} safe project-relative directories`;
  }
  return undefined;
}

interface RootIndex {
  readonly root: string;
  // Lookup precedence inside one root: exact relative path, relative path
  // without extension, file name, then file name without extension.
  readonly levels: readonly Map<string, string[]>[];
}

function withoutExtension(path: string): string | undefined {
  const slash = path.lastIndexOf("/");
  const dot = path.lastIndexOf(".");
  return dot > slash + 1 ? path.slice(0, dot) : undefined;
}

function add(map: Map<string, string[]>, key: string | undefined, path: string): void {
  if (key === undefined) return;
  const existing = map.get(key);
  if (existing === undefined) map.set(key, [path]);
  else if (!existing.includes(path)) existing.push(path);
}

/**
 * Builds the name lookup used by Story commands such as `[bg forest]`.
 * `paths` are project-relative files already known to the build; the index
 * never touches the filesystem, so a name resolves only to a file that will
 * also be packaged.
 */
export function createStoryAssetIndex(roots: StoryAssetRoots,
  paths: Iterable<string>): StoryAssetIndex {
  const files = [...new Set(paths)].sort();
  const categories = new Map<StoryAssetCategory, RootIndex[]>();
  const hash = createHash("sha256");
  for (const category of storyAssetCategories) {
    const authored = roots[category];
    if (authored === undefined) continue;
    const indexes: RootIndex[] = [];
    for (const root of authored) {
      const prefix = `${root.replace(/\/+$/u, "")}/`;
      const levels = [new Map<string, string[]>(), new Map<string, string[]>(),
        new Map<string, string[]>(), new Map<string, string[]>()];
      hash.update(`${category}\0${prefix}\0`);
      for (const path of files) {
        if (!path.startsWith(prefix)) continue;
        const relative = path.slice(prefix.length);
        const fileName = relative.slice(relative.lastIndexOf("/") + 1);
        add(levels[0]!, relative, path);
        add(levels[1]!, withoutExtension(relative), path);
        add(levels[2]!, fileName, path);
        add(levels[3]!, withoutExtension(fileName), path);
        hash.update(`${relative}\0`);
      }
      indexes.push({root: prefix.slice(0, -1), levels});
    }
    categories.set(category, indexes);
  }
  return {
    fingerprint: hash.digest("hex"),
    resolve(category, name) {
      const indexes = categories.get(category);
      if (indexes === undefined || name.length === 0) return undefined;
      for (const index of indexes) {
        for (const level of index.levels) {
          const matches = level.get(name);
          if (matches === undefined) continue;
          return matches.length === 1
            ? {kind: "resolved", path: matches[0]!}
            : {kind: "ambiguous", candidates: [...matches].sort()};
        }
      }
      return {kind: "missing", roots: indexes.map((index) => index.root)};
    },
  };
}
