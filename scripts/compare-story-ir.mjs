#!/usr/bin/env node
// Compile the Story of two project directories with the SDK and compare the
// Runtime IR operations: operation and source IDs, kinds, arguments and source
// lines, plus the text of dialogue, narration, labels and choices. Use it to
// prove that a Story rewrite (an importer emitting shorter command forms)
// leaves what the Player runs and what saves point at unchanged. Usage:
//   node scripts/compare-story-ir.mjs <before-project> <after-project>
// Command operations whose authored text alone differs are counted, not failed:
// their text is the command's source spelling, which the Player does not read.
import {readFile, readdir} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import {compileStoryProject, createStoryAssetIndex, loadStorySourceFiles}
  from '../packages/authoring-sdk/dist/src/index.js';

const textKinds = new Set(['dialogue', 'narration', 'label', 'choiceOption']);

async function json(path) {
  const text = await readFile(path, 'utf8');
  return JSON.parse(text.charCodeAt(0) === 0xfeff ? text.slice(1) : text);
}

async function files(root, directory, found) {
  let entries;
  try { entries = await readdir(join(root, directory), {withFileTypes: true}); }
  catch { return; }
  for (const entry of entries) {
    const path = `${directory}/${entry.name}`;
    if (entry.isDirectory()) await files(root, path, found);
    else found.push(path);
  }
}

async function compile(projectDirectory) {
  const root = resolve(projectDirectory);
  const manifest = await json(join(root, 'prismatix.json'));
  const storyIndex = await json(join(root, manifest.storyIndex));
  const sourceFiles = await loadStorySourceFiles(root, storyIndex);
  const characters = [];
  for (const descriptor of manifest.characters ?? []) {
    const document = await json(join(root, descriptor.source));
    characters.push({...document, id: document.id ?? descriptor.alias});
  }
  const game = manifest.gameCatalog === undefined ? undefined
    : await json(join(root, manifest.gameCatalog));
  const roots = new Set(Object.values(manifest.storyAssets ?? {}).flat());
  const paths = [];
  for (const directory of roots) await files(root, directory, paths);
  const storyAssets = createStoryAssetIndex(manifest.storyAssets ?? {}, paths);
  const result = {};
  for (const locale of manifest.supportedLocales ?? [manifest.defaultLocale]) {
    const compiled = compileStoryProject({storyIndex, locale, sourceFiles,
      storyIndexPath: manifest.storyIndex, documentId: manifest.id, characters,
      ...(game === undefined ? {} : {game}), storyAssets});
    const errors = compiled.diagnostics.filter((item) => item.severity === 'error');
    if (compiled.runtimeIr === undefined)
      throw new Error(`${root} ${locale}: no Runtime IR (${errors.length} errors, first: ` +
        `${JSON.stringify(errors[0])})`);
    result[locale] = {operations: compiled.runtimeIr.operations, errors: errors.length,
      warnings: compiled.diagnostics.length - errors.length};
  }
  return result;
}

const [beforeDirectory, afterDirectory] = process.argv.slice(2);
if (afterDirectory === undefined) {
  console.error('usage: compare-story-ir.mjs <before-project> <after-project>');
  process.exit(2);
}
const before = await compile(beforeDirectory);
const after = await compile(afterDirectory);
let failures = 0;
for (const locale of new Set([...Object.keys(before), ...Object.keys(after)])) {
  const left = before[locale]?.operations ?? [];
  const right = after[locale]?.operations ?? [];
  const differences = [];
  let commandText = 0;
  if (left.length !== right.length)
    differences.push(`operation count ${left.length} -> ${right.length}`);
  for (let index = 0; index < Math.min(left.length, right.length); index += 1) {
    const a = left[index];
    const b = right[index];
    const same = a.operationId === b.operationId && a.sourceId === b.sourceId &&
      a.kind === b.kind && a.sourceLine === b.sourceLine &&
      JSON.stringify(a.arguments) === JSON.stringify(b.arguments);
    if (!same || (textKinds.has(a.kind) && a.text !== b.text))
      differences.push(`#${index} ${a.sourceId} ${a.kind}: ${JSON.stringify(a)} -> ${JSON.stringify(b)}`);
    else if (a.text !== b.text) commandText += 1;
  }
  console.log(`${locale}: ${left.length} operations before, ${right.length} after; ` +
    `${differences.length} differ; ${commandText} command spellings changed; ` +
    `diagnostics before ${before[locale]?.errors ?? 0} errors/${before[locale]?.warnings ?? 0} warnings, ` +
    `after ${after[locale]?.errors ?? 0}/${after[locale]?.warnings ?? 0}`);
  for (const line of differences.slice(0, 10)) console.log(`  ${line}`);
  failures += differences.length;
}
process.exitCode = failures === 0 ? 0 : 1;
