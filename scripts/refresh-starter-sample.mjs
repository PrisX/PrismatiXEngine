#!/usr/bin/env node
// Regenerates Samples/Starter: the project PrismatiX Studio's New project
// dialog creates with its default settings (built-in look, ADV template,
// sample story off, zh-TW, 1920x1080).
//
//   node scripts/refresh-starter-sample.mjs --studio ../PrismatiXStudio [--out Samples/Starter]
//
// It follows the Studio's own path (src/components/shell/Home.tsx): this
// checkout's `prismatix create ... --template adv --empty`, then the Studio's
// starter background (src/components/library/starter.ts), using the Studio's
// own transforms from src/lib/library/starter.ts and its library picture and
// provenance. Then it removes what belongs to one machine (node_modules, the
// lockfile with its `link:` paths, caches), pins the tools to this release,
// adds the sample's README header and writes SAMPLE_PROVENANCE.json.
//
// Only this script reads the Studio checkout; nothing in the engine's build or
// tests depends on it. Run it when the Studio's default project changes, then
// review the diff (UUIDs and save identities are new on every run) and run
// `node scripts/check-sample-provenance.mjs`.

import {spawnSync} from "node:child_process";
import {createHash, randomUUID} from "node:crypto";
import {copyFile, cp, mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {dirname, join, relative, resolve, sep} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
const option = (name) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 ? args[index + 1] : undefined;
};
const studio = option("studio");
if (studio === undefined) {
  process.stderr.write("usage: node scripts/refresh-starter-sample.mjs --studio <PrismatiX Studio checkout> [--out Samples/Starter]\n");
  process.exit(2);
}
const studioRoot = resolve(studio);
const output = resolve(option("out") ?? join(repositoryRoot, "Samples", "Starter"));
const cli = join(repositoryRoot, "packages", "cli", "dist", "src", "cli.js");
const version = JSON.parse(await readFile(join(repositoryRoot, "packages", "cli", "package.json"), "utf8")).version;

// The New project dialog's defaults (Home.tsx CreateDialog).
const id = "prismatix-starter";
const name = "PrismatiX Starter";
const locale = "zh-TW";
const [width, height] = [1920, 1080];

function run(command, commandArgs, cwd) {
  const result = spawnSync(command, commandArgs, {cwd, encoding: "utf8", stdio: "pipe", windowsHide: true});
  if (result.status !== 0)
    throw new Error(`${commandArgs.join(" ")} failed (${result.status ?? result.signal})\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}
const exists = (path) => stat(path).then(() => true, () => false);

const {starterBackground, withOpeningBackground, withStarterBackground, withTitleBackdrop} =
  await import(pathToFileURL(join(studioRoot, "src", "lib", "library", "starter.ts")).href);
const library = join(studioRoot, "src", "lib", "library", "assets");

const parent = await mkdtemp(join(tmpdir(), "prismatix-starter-"));
const root = join(parent, id);
try {
  // 1. create (what the dialog runs through the bridge).
  run(process.execPath, [cli, "create", id, "--name", name, "--id", id, "--locale", locale,
    "--width", String(width), "--height", String(height), "--template", "adv", "--empty",
    "--package-manager", "pnpm"], parent);

  // 2. addStarterBackground(root, {titleBackdrop: true}).
  const abs = (path) => join(root, ...path.split("/"));
  const manifestPath = abs("prismatix.json");
  const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
  await mkdir(dirname(abs(starterBackground.path)), {recursive: true});
  await copyFile(join(library, ...starterBackground.key.split("/")), abs(starterBackground.path));
  const notice = await readFile(join(library, ...starterBackground.provenanceKey.split("/")), "utf8");
  const provenancePath = abs(starterBackground.provenance);
  const existing = await exists(provenancePath) ? await readFile(provenancePath, "utf8") : null;
  if (existing === null) await writeFile(provenancePath, notice, "utf8");
  else if (!existing.includes(starterBackground.path))
    await writeFile(provenancePath, `${existing.trimEnd()}\n\n${notice.replace(/^# /u, "## ")}`, "utf8");
  const next = withStarterBackground(manifest, {background: randomUUID(), provenance: randomUUID()});
  await writeFile(manifestPath, `${JSON.stringify(next, null, 2)}\n`, "utf8");
  const storyPath = abs("Story/main.pxstory");
  const story = await readFile(storyPath, "utf8");
  const opened = withOpeningBackground(story);
  if (opened !== story) {
    await writeFile(storyPath, opened, "utf8");
    run(process.execPath, [cli, "ids", root, "--sync", "--write", "--json"], root);
  }
  const titlePath = abs(manifest.uiEntryPoints?.title ?? "Content/UI/Title.tsx");
  const title = await readFile(titlePath, "utf8");
  const backed = withTitleBackdrop(title);
  if (backed !== title) await writeFile(titlePath, backed, "utf8");

  // 3. What belongs to this machine goes; the tools pin this release.
  for (const path of ["node_modules", "pnpm-lock.yaml", "package-lock.json", ".prismatix", ".prismatix-studio", "dist"])
    await rm(join(root, path), {recursive: true, force: true});
  const packagePath = abs("package.json");
  const pkg = JSON.parse(await readFile(packagePath, "utf8"));
  for (const dependency of Object.keys(pkg.devDependencies ?? {}))
    if (dependency.startsWith("@prismatix/")) pkg.devDependencies[dependency] = version;
  // The pnpm this repository documents, not whichever one this machine has.
  pkg.packageManager = JSON.parse(await readFile(join(repositoryRoot, "package.json"), "utf8")).packageManager;
  await writeFile(packagePath, `${JSON.stringify(pkg, null, 2)}\n`, "utf8");

  // 4. The sample's README: how to run it, then the project's own README.
  const readmePath = abs("README.md");
  const projectReadme = (await readFile(readmePath, "utf8")).replace(/^# .*$/mu, "## About this project");
  await writeFile(readmePath, `# PrismatiX Starter sample

The project PrismatiX Studio creates with the New project dialog's default
settings: the built-in look (title, HUD, save, load, settings and backlog
screens as editable TSX), the "Made with PrismatiX" startup screen, an empty
Traditional Chinese Story that opens on a street background, and no
characters. \`scripts/refresh-starter-sample.mjs\` in the engine repository
regenerates it.

Copy this folder somewhere outside the engine repository, then:

\`\`\`bash
pnpm install          # the pinned @prismatix/cli, authoring-sdk and runtime
pnpm run dev          # live preview; edit Story/main.pxstory and save
pnpm run build        # a desktop build with the Player, in dist/
\`\`\`

The street picture's licence is not settled yet (see \`ASSET_PROVENANCE.md\`):
replace \`Assets/Backgrounds/street.jpg\` before you release a game made from
this sample. \`SAMPLE_PROVENANCE.json\` lists the origin and licence of every
file here.

${projectReadme.trimEnd()}
`, "utf8");

  // 5. Nothing may name this machine.
  const files = [];
  const walk = async (directory) => {
    for (const entry of await readdir(directory, {withFileTypes: true})) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) await walk(path); else files.push(path);
    }
  };
  await walk(root);
  const binary = /\.(?:png|jpe?g|webp|ttf|otf|wav|ogg|opus)$/iu;
  for (const path of files.filter((value) => !binary.test(value))) {
    const text = await readFile(path, "utf8");
    for (const local of [parent, repositoryRoot, studioRoot, tmpdir()])
      for (const form of [local, local.split(sep).join("/")])
        if (text.includes(form)) throw new Error(`${relative(root, path)} names a local path (${form})`);
    if (/\b(?:link|file):/u.test(text) && path.endsWith("package.json"))
      throw new Error(`${relative(root, path)} still links local packages`);
  }

  // 6. Replace the sample and write its provenance inventory.
  // An earlier inventory keeps what it says about files that did not change
  // (a licence confirmed since, say).
  const inventory = await readFile(join(output, "SAMPLE_PROVENANCE.json")).catch(() => undefined);
  await rm(output, {recursive: true, force: true, maxRetries: 5, retryDelay: 100});
  await cp(root, output, {recursive: true});
  if (inventory !== undefined) await writeFile(join(output, "SAMPLE_PROVENANCE.json"), inventory);
  run(process.execPath, [join(repositoryRoot, "scripts", "check-sample-provenance.mjs"), "--write", output], repositoryRoot);
  process.stdout.write(`Refreshed ${relative(repositoryRoot, output) || output} from ${studioRoot}\n`);
} finally {
  await rm(parent, {recursive: true, force: true, maxRetries: 5, retryDelay: 100});
}
