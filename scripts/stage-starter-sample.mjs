#!/usr/bin/env node
// Copies Samples/Starter to a new folder and links this checkout's
// @prismatix/cli, authoring-sdk and runtime into its node_modules, so the
// checkout's CLI can validate, build and pack it before 0.1.0 is installable
// from the registry (the sample pins the release; a project's pinned CLI must
// be present). Used by the CLI tests, CI and release checks; an author copies
// the sample and runs `pnpm install` instead.
//
//   node scripts/stage-starter-sample.mjs <destination>

import {cp, mkdir, readdir, symlink} from "node:fs/promises";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sample = join(repositoryRoot, "Samples", "Starter");
const destination = process.argv[2];
if (destination === undefined) {
  process.stderr.write("usage: node scripts/stage-starter-sample.mjs <destination>\n");
  process.exit(2);
}
const target = resolve(destination);
const entries = await readdir(target).catch((error) => {
  if (error.code === "ENOENT") return [];
  throw error;
});
if (entries.length > 0) throw new Error(`${target} must be a new or empty folder`);

const local = new Set(["node_modules", ".prismatix", ".prismatix-studio", ".prismatix-release", "dist"]);
await cp(sample, target, {recursive: true,
  filter: (source) => !local.has(source.slice(sample.length + 1).split(/[\\/]/u)[0] ?? "")});
await mkdir(join(target, "node_modules", "@prismatix"), {recursive: true});
for (const [name, path] of [["cli", "packages/cli"], ["authoring-sdk", "packages/authoring-sdk"], ["runtime", "runtime-sdk"]])
  await symlink(join(repositoryRoot, ...path.split("/")), join(target, "node_modules", "@prismatix", name), "junction");
process.stdout.write(`${target}\n`);
