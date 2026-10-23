#!/usr/bin/env node
// Scripts read project resources as text only, and a .moc3 is binary, so the
// model ships its .moc3 as base64 text beside it: haru.moc3 -> haru.moc3.b64.
// Run it on a model's folder (or on single files) whenever the .moc3 changes.
//
//   node tools/moc-to-text.mjs Assets/Live2D/Haru [more folders or files]
//
// The .moc3 itself does not need to be packed into the game any more; keep it
// out of `resourceIncludes` and pack the .b64 instead.

import {readFileSync, readdirSync, statSync, writeFileSync} from "node:fs";
import {join} from "node:path";

function* mocFiles(path) {
  if (statSync(path).isDirectory()) {
    for (const name of readdirSync(path)) yield* mocFiles(join(path, name));
  } else if (path.endsWith(".moc3")) {
    yield path;
  }
}

const targets = process.argv.slice(2);
if (targets.length === 0) {
  console.error("usage: node tools/moc-to-text.mjs <folder or .moc3> [...]");
  process.exit(2);
}

let count = 0;
for (const target of targets) {
  for (const file of mocFiles(target)) {
    const bytes = readFileSync(file);
    if (bytes.subarray(0, 4).toString("latin1") !== "MOC3") {
      console.error(`${file}: does not start with MOC3, skipped`);
      continue;
    }
    writeFileSync(`${file}.b64`, bytes.toString("base64"));
    console.log(`${file} -> ${file}.b64 (${bytes.length} bytes)`);
    count += 1;
  }
}
if (count === 0) {
  console.error("no .moc3 files found");
  process.exit(1);
}
