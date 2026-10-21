#!/usr/bin/env node
// The sample provenance gate (delivery contract, gate 6). Every folder under
// Samples/ must carry SAMPLE_PROVENANCE.json listing every file in it with its
// SHA-256, origin and licence; a file copied from the engine's own inventory
// (ASSET_PROVENANCE.json) must be byte-identical to it.
//
//   node scripts/check-sample-provenance.mjs            check; report pending licences
//   node scripts/check-sample-provenance.mjs --release  check; a pending licence fails
//   node scripts/check-sample-provenance.mjs --write Samples/Starter
//                                                       (re)write one inventory
//
// A licence that is not settled is recorded as "license": "TBD" with
// "status": "pending" and the reason; it never passes a release check.
// The project owner's own pictures for samples are recorded as
// "license": "sample-only" with "status": "sample-only": they pass, but only
// with the exact bytes listed in `sampleOnly` below. Text files are hashed
// with LF line endings, so a CRLF checkout verifies too.

import {spawnSync} from "node:child_process";
import {createHash} from "node:crypto";
import {readFile, readdir, stat, writeFile} from "node:fs/promises";
import {basename, dirname, join, relative, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const samplesRoot = join(repositoryRoot, "Samples");
const inventoryName = "SAMPLE_PROVENANCE.json";
const licenses = new Set(["MIT", "OFL-1.1", "project-branding-restricted", "sample-only", "TBD"]);
const statuses = new Set(["pending", "sample-only"]);

// Pictures the project owner supplied for samples only (PrismatiX Studio's
// new-project background and cast), by SHA-256. "sample-only" passes a
// release check for these bytes and for nothing else.
const sampleOnlyNote = "AI-generated image supplied by the project owner (Owen Liu) for use in samples only. Replace it before releasing a game.";
const sampleOnly = new Map([
  ["4c134ca7dda9cd5b2e386cf3e0574f9ba648aa21dece660cd78c65688ce90116",
    "the street background PrismatiX Studio puts in every new project"],
  ["7fdf790ad270134cc32bf6be23cf06117f01f2076636d06c0eb79e42f27b77d7",
    "the portrait of 白音 (Shirone), one of the two characters PrismatiX Studio puts in every new project"],
  ["3b8c6b014f3304fcf40220eb3ab78af965f66608d5414f91956912be07644655",
    "the portrait of 小春 (Koharu), one of the two characters PrismatiX Studio puts in every new project"],
]);
const binary = /\.(?:png|jpe?g|webp|gif|ttf|otf|woff2?|wav|ogg|opus|mp3|flac|mp4|webm)$/iu;
const posix = (path) => path.split(sep).join("/");

async function digest(path) {
  let bytes = await readFile(path);
  if (!binary.test(path)) bytes = Buffer.from(bytes.toString("utf8").replaceAll("\r\n", "\n"), "utf8");
  return createHash("sha256").update(bytes).digest("hex");
}

/** The sample's files: tracked or new, never ignored (node_modules, caches). */
async function sampleFiles(root) {
  const listed = spawnSync("git", ["ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "."],
    {cwd: root, encoding: "utf8", windowsHide: true});
  let files;
  if (listed.status === 0) {
    const present = await Promise.all(listed.stdout.split("\0").filter(Boolean).map(async (path) =>
      (await stat(join(root, path)).then((info) => info.isFile(), () => false)) ? path : undefined));
    files = present.filter((path) => path !== undefined);
  } else {
    files = [];
    const walk = async (directory) => {
      for (const entry of await readdir(directory, {withFileTypes: true})) {
        if (["node_modules", ".prismatix", ".prismatix-studio", ".prismatix-release", "dist"].includes(entry.name)) continue;
        const path = join(directory, entry.name);
        if (entry.isDirectory()) await walk(path); else files.push(posix(relative(root, path)));
      }
    };
    await walk(root);
  }
  return files.map(posix).filter((path) => path !== inventoryName).sort();
}

async function engineInventory() {
  const document = JSON.parse(await readFile(join(repositoryRoot, "ASSET_PROVENANCE.json"), "utf8"));
  return new Map(document.assets.map((asset) => [asset.sha256, asset]));
}

// How --write describes a file it has no earlier entry for.
const starterOrigin = "Written by `prismatix create --template adv --empty` (packages/cli/templates/minimal) and PrismatiX Studio's new-project step; see scripts/refresh-starter-sample.mjs";
function describe(path, sha256, engine) {
  // Before engine copies: the CLI's sample template carries the same pictures.
  if (sampleOnly.has(sha256))
    return {origin: `Supplied by the project owner (Owen Liu): ${sampleOnly.get(sha256)}`,
      license: "sample-only", status: "sample-only", note: sampleOnlyNote};
  const copied = engine.get(sha256);
  if (copied !== undefined)
    return {origin: `Copy of the engine's ${copied.path} (${copied.origin})`, license: copied.license, copyOf: copied.path};
  if (path === "Content/Fonts/OFL-1.1.txt")
    return {origin: "SIL Open Font License 1.1 text, as in the engine's Resources/Fonts/OFL-1.1.txt", license: "OFL-1.1"};
  if (!binary.test(path)) return {origin: starterOrigin, license: "MIT"};
  throw new Error(`${path}: no known origin; add it to ${inventoryName} by hand`);
}

async function write(root) {
  const engine = await engineInventory();
  const path = join(root, inventoryName);
  const previous = await readFile(path, "utf8").then((text) => JSON.parse(text), () => undefined);
  const before = new Map((previous?.files ?? []).map((entry) => [entry.path, entry]));
  const files = [];
  for (const file of await sampleFiles(root)) {
    const sha256 = await digest(join(root, file));
    // The owner's sample pictures are always described afresh, so an older
    // entry (a pending licence, say) does not outlive their settled terms.
    const kept = sampleOnly.has(sha256) ? undefined : before.get(file);
    const {path: _path, sha256: _sha, ...description} = kept?.sha256 === sha256 ? kept : describe(file, sha256, engine);
    files.push({path: file, sha256, ...description});
  }
  await writeFile(path, `${JSON.stringify({format: "PrismatiXSampleProvenance", schemaRevision: 1,
    sample: basename(root), files}, null, 2)}\n`, "utf8");
  process.stdout.write(`wrote ${posix(relative(repositoryRoot, path))} (${files.length} files)\n`);
}

async function check(release) {
  const engine = await engineInventory();
  const problems = [];
  const pending = [];
  let ownerSupplied = 0;
  const samples = (await readdir(samplesRoot, {withFileTypes: true})).filter((entry) => entry.isDirectory());
  if (samples.length === 0) problems.push("Samples/ has no sample");
  let checked = 0;
  for (const sample of samples) {
    const root = join(samplesRoot, sample.name);
    const label = `Samples/${sample.name}`;
    let inventory;
    try { inventory = JSON.parse(await readFile(join(root, inventoryName), "utf8")); }
    catch { problems.push(`${label} has no readable ${inventoryName}`); continue; }
    if (inventory.format !== "PrismatiXSampleProvenance" || inventory.schemaRevision !== 1)
      problems.push(`${label}/${inventoryName} is not a PrismatiXSampleProvenance revision 1 document`);
    const entries = new Map((inventory.files ?? []).map((entry) => [entry.path, entry]));
    const files = await sampleFiles(root);
    for (const file of files) {
      const entry = entries.get(file);
      entries.delete(file);
      if (entry === undefined) { problems.push(`${label}/${file} is not in ${inventoryName}`); continue; }
      checked += 1;
      if (entry.sha256 !== await digest(join(root, file)))
        problems.push(`${label}/${file} does not match its recorded SHA-256`);
      if (typeof entry.origin !== "string" || entry.origin.trim() === "")
        problems.push(`${label}/${file} has no origin`);
      if (!licenses.has(entry.license))
        problems.push(`${label}/${file} has an unknown licence ${JSON.stringify(entry.license)}`);
      if (entry.copyOf !== undefined && engine.get(entry.sha256)?.path !== entry.copyOf)
        problems.push(`${label}/${file} is not byte-identical to the engine's ${entry.copyOf}`);
      if (entry.status !== undefined && !statuses.has(entry.status))
        problems.push(`${label}/${file} has an unknown status ${JSON.stringify(entry.status)}`);
      if (entry.license === "sample-only" || entry.status === "sample-only") {
        if (entry.license !== "sample-only" || entry.status !== "sample-only")
          problems.push(`${label}/${file} must record both "license" and "status" as "sample-only"`);
        else if (!sampleOnly.has(entry.sha256))
          problems.push(`${label}/${file} is marked sample-only but is not one of the owner-supplied sample pictures`);
        else ownerSupplied += 1;
      }
      if (entry.license === "TBD" || entry.status === "pending")
        pending.push(`${label}/${file}: ${entry.note ?? "licence pending"}`);
    }
    for (const stale of entries.keys()) problems.push(`${label}/${inventoryName} lists ${stale}, which is not in the sample`);
  }
  for (const item of pending)
    process.stderr.write(`${release ? "error" : "warning"}: PENDING LICENCE (release-blocking) ${item}\n`);
  if (release && pending.length > 0)
    problems.push(`${pending.length} sample file(s) have a pending licence; a release cannot ship them`);
  if (problems.length > 0) {
    process.stderr.write(`Sample provenance check failed:\n${problems.map((problem) => `  - ${problem}`).join("\n")}\n`);
    process.exit(1);
  }
  process.stdout.write(`sample provenance: ${checked} files in ${samples.length} sample(s) listed and verified` +
    `${ownerSupplied > 0 ? `; ${ownerSupplied} owner-supplied picture(s) for sample use only` : ""}` +
    `${pending.length > 0 ? `; ${pending.length} licence(s) pending (release-blocking)` : ""}\n`);
}

const args = process.argv.slice(2);
if (args[0] === "--write") {
  if (args[1] === undefined) throw new Error("--write needs a sample folder");
  await write(resolve(args[1]));
} else await check(args.includes("--release"));
