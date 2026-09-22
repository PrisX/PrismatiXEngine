#!/usr/bin/env node
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdtemp, mkdir, readFile, readdir, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
assert(args.get('--player') && args.get('--fixture-writer'), '--player and --fixture-writer are required');
const player = resolve(args.get('--player'));
const writer = resolve(args.get('--fixture-writer'));
const root = args.has('--output') ? resolve(args.get('--output')) : await mkdtemp(join(tmpdir(), 'px-inspect-'));
await mkdir(root, {recursive: true});
const generated = spawnSync(writer, [], {cwd: root, encoding: 'utf8',
  env: {...process.env, PRISMATIX_TEST_INSPECT_FIXTURE: root}});
assert.equal(generated.status, 0, generated.stdout + generated.stderr);
const cwd = join(root, 'readonly-inspection-cwd');
await mkdir(cwd);
const manifest = join(root, 'manifest.json');
const plain = join(root, 'plaintext/save_0.pxsav');
const original = await readFile(plain);
const protectedInputs = new Map(await Promise.all([
  plain, join(root, 'current/save_0.pxsav'), join(root, 'legacy/save_0.pxsav'), manifest,
].map(async path => [path, await readFile(path)])));
const cases = [];
function run(path, options = [], success = true, cli = false) {
  const command = cli ? process.execPath : player;
  const argv = cli
    ? [resolve(args.get('--cli')), 'inspect-save', path, '--player', player, ...options]
    : ['--inspect-save', path, ...options];
  const result = spawnSync(command, argv, {cwd, encoding: 'utf8', timeout: 30000, maxBuffer: 64 * 1024 * 1024});
  assert.equal(result.status === 0, success, result.stdout + result.stderr);
  if (success) {
    const object = JSON.parse(result.stdout);
    assert.equal(object.format, 'PrismatiXSave');
    assert.match(result.stdout, /"timestamp"\s*:\s*9007199254740993/);
    assert.match(result.stdout, /"playtimeMs"\s*:\s*18446744073709551615/);
    if (options.includes('--json')) assert.match(result.stdout, /"floatZero"\s*:\s*0\.0/);
  } else {
    assert.equal(result.stdout, '', 'failure must not contaminate stdout');
    assert(result.stderr.length, 'failure has stderr diagnostics');
  }
  cases.push({path, options, cli, success, exitCode: result.status});
}
run(plain);
run(plain, ['--json']);
run(join(root, 'current/save_0.pxsav'), ['--package', manifest, '--json']);
run(join(root, 'legacy/save_0.pxsav'), ['--package', manifest]);
run(join(root, 'legacy/save_0.pxsav'), ['--secret', 'fixture-legacy-secret', '--json']);
run(join(root, 'legacy/save_0.pxsav'), ['--secret', 'incorrect'], false);
const tampered = join(root, 'tampered.pxsav');
await writeFile(tampered, original.toString().replace('Chapter 1', 'Chapter X'));
run(tampered, [], false);
const duplicate = join(root, 'duplicate.pxsav');
await writeFile(duplicate, original.toString().replace('{', '{"format":"PrismatiXSave",'));
run(duplicate, [], false);
const unicodePlain = join(root, '中文 空白 😀.pxsav');
const unicodeEncrypted = join(root, '加密 存檔 😀.pxsav');
const unicodeLegacy = join(root, '舊版 存檔 😀.pxsav');
const unicodeManifest = join(root, '套件 清單 😀.json');
await writeFile(unicodePlain, original);
await writeFile(unicodeEncrypted, await readFile(join(root, 'current/save_0.pxsav')));
await writeFile(unicodeLegacy, await readFile(join(root, 'legacy/save_0.pxsav')));
await writeFile(unicodeManifest, await readFile(manifest));
run(unicodePlain, ['--json']);
run(unicodeEncrypted, ['--package', unicodeManifest, '--json']);
run(unicodeLegacy, ['--package', unicodeManifest]);
run(unicodeLegacy, ['--secret', 'fixture-legacy-secret']);
if (args.has('--cli')) {
  run(plain, ['--json'], true, true);
  run(join(root, 'current/save_0.pxsav'), ['--package', manifest], true, true);
  run(tampered, [], false, true);
  run(unicodeEncrypted, ['--package', unicodeManifest, '--json'], true, true);
}
assert.deepEqual(await readFile(plain), original, 'inspection preserves source bytes');
for (const [path, bytes] of protectedInputs)
  assert.deepEqual(await readFile(path), bytes, 'inspection preserves every plaintext/encrypted input and manifest');
assert.deepEqual(await readdir(cwd), [], 'headless inspection creates no logs, Save, or SDL artifacts');
await writeFile(join(root, 'inspection-results.json'), JSON.stringify({
  passed: true, cases, plaintextSha256: createHash('sha256').update(original).digest('hex'),
}, null, 2));
console.log(`Native save inspection passed ${cases.length} cases at ${root}`);
