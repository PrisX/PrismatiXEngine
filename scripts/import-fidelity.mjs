#!/usr/bin/env node
// Compare an importer's output tree with files committed in a git project,
// byte for byte against the committed blobs (so checkout line-ending filters
// do not matter). Usage:
//   node scripts/import-fidelity.mjs <generated-dir> <git-project> [--rev HEAD]
//     [--path Story --path Characters ...] [--show 20]
// Exits 1 when any file differs, is missing, or is new under a listed path.
// Identity sidecars (.pxids.json) whose JSON value is unchanged count as
// "reformatted" rather than changed: their layout belongs to the SDK.
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {readFile, readdir, stat} from 'node:fs/promises';
import {join, resolve} from 'node:path';

const args = process.argv.slice(2);
const positional = [];
const paths = [];
let rev = 'HEAD';
let show = 20;
for (let index = 0; index < args.length; index += 1) {
  const arg = args[index];
  if (arg === '--rev') rev = args[++index];
  else if (arg === '--path') paths.push(args[++index].replace(/\\/g, '/').replace(/\/$/, ''));
  else if (arg === '--show') show = Number(args[++index]);
  else positional.push(arg);
}
if (positional.length !== 2) {
  console.error('usage: import-fidelity.mjs <generated-dir> <git-project> [--rev REV] [--path P]... [--show N]');
  process.exit(2);
}
const generated = resolve(positional[0]);
const project = resolve(positional[1]);
if (paths.length === 0) paths.push('Story', 'Characters', 'Content/game.pxgame',
  'docs/conversion-report.json', 'prismatix.json');

const listing = execFileSync('git', ['-C', project, 'ls-tree', '-r', '-z', rev, '--', ...paths],
  {encoding: 'utf8', maxBuffer: 256 * 1024 * 1024});
const expected = new Map();
for (const entry of listing.split('\0')) {
  if (!entry) continue;
  const tab = entry.indexOf('\t');
  expected.set(entry.slice(tab + 1), entry.slice(0, tab).split(' ')[2]);
}

async function walk(relative, found) {
  const full = join(generated, relative);
  let info;
  try { info = await stat(full); } catch { return; }
  if (info.isFile()) { found.push(relative); return; }
  for (const child of await readdir(full)) await walk(`${relative}/${child}`, found);
}
const actual = [];
for (const path of paths) await walk(path, actual);

const blob = (bytes) => createHash('sha1')
  .update(`blob ${bytes.length}\0`).update(bytes).digest('hex');
const changed = [];
const missing = [];
const extra = [];
const reformatted = [];
let same = 0;
const committed = (hash) => execFileSync('git', ['-C', project, 'cat-file', 'blob', hash],
  {encoding: 'utf8', maxBuffer: 256 * 1024 * 1024});
const sameJson = (left, right) => {
  try { return JSON.stringify(JSON.parse(left)) === JSON.stringify(JSON.parse(right)); }
  catch { return false; }
};
for (const path of actual) {
  const want = expected.get(path);
  if (want === undefined) { extra.push(path); continue; }
  const bytes = await readFile(join(generated, path));
  if (blob(bytes) === want) same += 1;
  else if (path.endsWith('.pxids.json') && sameJson(bytes.toString('utf8'), committed(want)))
    reformatted.push(path);
  else changed.push(path);
}
const present = new Set(actual);
for (const path of expected.keys()) if (!present.has(path)) missing.push(path);
console.log(`${same} identical, ${reformatted.length} reformatted sidecars, ` +
  `${changed.length} changed, ${missing.length} missing, ` +
  `${extra.length} new (against ${project} ${rev})`);
for (const [label, list] of [['changed', changed], ['missing', missing], ['new', extra],
  ['reformatted', reformatted]])
  for (const path of list.slice(0, show)) console.log(`  ${label}: ${path}`);
process.exitCode = changed.length + missing.length + extra.length === 0 ? 0 : 1;
