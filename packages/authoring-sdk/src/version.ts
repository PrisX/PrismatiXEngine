import {readFileSync} from "node:fs";

/** Engine release line; package.json is the single source for the TypeScript packages. */
export const ENGINE_VERSION: string =
  (JSON.parse(readFileSync(new URL("../../package.json", import.meta.url), "utf8")) as {version: string}).version;
