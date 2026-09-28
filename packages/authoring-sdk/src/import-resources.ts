import {isSafeResourcePath} from "./resources.js";
import type {Result} from "./types.js";

export type ImportedResourceResolution =
  | {readonly kind: "found"; readonly path: string; readonly match: "path" | "name"}
  | {readonly kind: "missing"; readonly query: string}
  | {readonly kind: "ambiguous"; readonly query: string; readonly candidates: readonly string[]}
  | {readonly kind: "invalid"; readonly query: string};

export interface ImportedResourceResolver {
  readonly size: number;
  /** Exact project-relative paths take priority. Bare names resolve only when
   * one candidate remains after matching an explicit extension, if present. */
  resolve(query: string): ImportedResourceResolution;
}

function validPath(path: string): boolean {
  return isSafeResourcePath(path) && !path.includes(":") &&
    !/[\u0000-\u001f\u007f]/u.test(path) &&
    path.split("/").every((part) => part.length > 0 && part !== ".");
}

function parts(path: string): {stem: string; extension: string} {
  const base = path.slice(path.lastIndexOf("/") + 1);
  const dot = base.lastIndexOf(".");
  return dot > 0 ? {stem: base.slice(0, dot), extension: base.slice(dot)}
    : {stem: base, extension: ""};
}

function fold(value: string): string {
  return value.toLocaleLowerCase("en-US");
}

/** Build an import-time lookup from already enumerated project-relative media.
 * This does not register Runtime assets or read the filesystem. Importers can
 * stream their source format and reuse the index without silently choosing a
 * similarly named asset from another directory. */
export function createImportedResourceResolver(
  paths: readonly string[],
): Result<ImportedResourceResolver> {
  const invalid = paths.filter((path) => !validPath(path));
  if (invalid.length > 0) return {valid: false, diagnostics: invalid.map((path) => ({
    severity: "error" as const, code: "PXIMPORT1001",
    message: "Imported resource must be a safe project-relative path", path,
  }))};
  const unique = [...new Set(paths)];
  const byPath = new Map<string, string[]>();
  const byStem = new Map<string, string[]>();
  for (const path of unique) {
    const full = fold(path);
    byPath.set(full, [...byPath.get(full) ?? [], path]);
    const stem = fold(parts(path).stem);
    byStem.set(stem, [...byStem.get(stem) ?? [], path]);
  }
  const answer = (query: string, candidates: readonly string[],
    match: "path" | "name"): ImportedResourceResolution => {
    if (candidates.length === 0) return {kind: "missing", query};
    if (candidates.length === 1) return {kind: "found", path: candidates[0]!, match};
    return {kind: "ambiguous", query,
      candidates: [...candidates].sort()};
  };
  return {valid: true, diagnostics: [], value: {
    size: unique.length,
    resolve(query) {
      if (!validPath(query)) return {kind: "invalid", query};
      const exact = byPath.get(fold(query));
      if (exact !== undefined) return answer(query, exact, "path");
      if (query.includes("/")) return {kind: "missing", query};
      const wanted = parts(query);
      const candidates = byStem.get(fold(wanted.stem)) ?? [];
      return answer(query, wanted.extension.length === 0 ? candidates
        : candidates.filter((path) => fold(parts(path).extension) === fold(wanted.extension)),
      "name");
    },
  }};
}
