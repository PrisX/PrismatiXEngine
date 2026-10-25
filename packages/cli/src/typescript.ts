import {statSync} from "node:fs";
import {createRequire} from "node:module";
import type * as TypeScript from "typescript-compiler";

let loaded: typeof TypeScript | undefined;

/**
 * The TypeScript compiler, loaded on first use. It is about 9 MB of
 * CommonJS, and importing it eagerly cost every `prismatix` command over a
 * second of startup (twice when the global CLI hands a project command to its
 * pinned local CLI) although only TSX screens and TypeScript extensions need it.
 */
export const ts: typeof TypeScript = new Proxy({} as typeof TypeScript, {
  get: (_target, property) =>
    Reflect.get(loaded ??= createRequire(import.meta.url)("typescript-compiler") as typeof TypeScript, property),
});

/** Which TypeScript compiler `ts` would load (its file, size and time), without loading it. */
export function typescriptIdentity(): string {
  const path = createRequire(import.meta.url).resolve("typescript-compiler");
  const info = statSync(path);
  return `${path}:${info.size}:${info.mtimeMs}`;
}
