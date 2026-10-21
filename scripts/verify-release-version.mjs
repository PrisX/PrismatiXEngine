#!/usr/bin/env node
// Checks that every place stating the release version agrees, and with
// --tag vX.Y.Z that the tag names that version. The CLI, Authoring SDK and
// runtime SDK are one release: they carry the same version and pin each other
// exactly, and the native runtime (CMake) reports it too.
//
//   node scripts/verify-release-version.mjs [--tag v0.1.0]

import {readFile, stat} from "node:fs/promises";
import {join} from "node:path";
import {publicPackages, readJson, releaseVersion, repositoryRoot} from "./release-packages.mjs";

const problems = [];
const expect = (condition, message) => { if (!condition) problems.push(message); };
const text = (path) => readFile(join(repositoryRoot, path), "utf8");
const exists = (path) => stat(join(repositoryRoot, path)).then(() => true, () => false);

const args = process.argv.slice(2);
const tagIndex = args.indexOf("--tag");
const tag = tagIndex >= 0 ? args[tagIndex + 1] : args.find((value) => value.startsWith("--tag="))?.slice(6);

const version = await releaseVersion();
expect(/^\d+\.\d+\.\d+$/u.test(version), `release version ${version} is not MAJOR.MINOR.PATCH`);
if (tag !== undefined) expect(tag === `v${version}`, `tag ${tag} does not match the package version v${version}`);

for (const {name, root} of publicPackages) {
  const manifest = await readJson(join(root, "package.json"));
  expect(manifest.version === version, `${name} is ${manifest.version}, expected ${version}`);
  expect(manifest.license === "MIT", `${name} has no MIT license field`);
  expect(manifest.publishConfig?.access === "public", `${name} is not publishConfig.access public`);
  for (const [dependency, range] of Object.entries(manifest.dependencies ?? {}))
    if (dependency.startsWith("@prismatix/"))
      expect(range === version, `${name} depends on ${dependency}@${range}, expected exactly ${version}`);
}

const template = await readJson(join(repositoryRoot, "packages", "cli", "templates", "minimal", "package.json"));
for (const [dependency, range] of Object.entries(template.devDependencies ?? {}))
  if (dependency.startsWith("@prismatix/"))
    expect(range === version, `starter template pins ${dependency}@${range}, expected ${version}`);
const sample = await readJson(join(repositoryRoot, "Samples", "Starter", "package.json"));
for (const [dependency, range] of Object.entries(sample.devDependencies ?? {}))
  if (dependency.startsWith("@prismatix/"))
    expect(range === version, `Samples/Starter pins ${dependency}@${range}, expected ${version}`);

const vscode = await readJson(join(repositoryRoot, "packages", "vscode-pxstory", "package.json"));
expect(vscode.version === version, `vscode-pxstory is ${vscode.version}, expected ${version}`);
expect(vscode.devDependencies?.["@prismatix/authoring-sdk"] === version, "vscode-pxstory does not pin the release Authoring SDK");

const cmake = (await text("cmake/PrismatiXContractVersions.cmake")).match(/set\(PRISMATIX_ENGINE_VERSION ([^)\s]+)\)/u)?.[1];
expect(cmake === version, `CMake PRISMATIX_ENGINE_VERSION is ${cmake}, expected ${version}`);
const vcpkg = await readJson(join(repositoryRoot, "vcpkg.json"));
expect(vcpkg.version === version, `vcpkg.json is ${vcpkg.version}, expected ${version}`);
const provenance = await readJson(join(repositoryRoot, "ASSET_PROVENANCE.json"));
expect(provenance.release === version, `ASSET_PROVENANCE.json release is ${provenance.release}, expected ${version}`);

expect(await exists(join("docs", "releases", `${version}.md`)), `docs/releases/${version}.md (the release notes) is missing`);
const changelog = await text("CHANGELOG.md").catch(() => "");
expect(new RegExp(`^## \\[${version.replaceAll(".", "\\.")}\\]`, "mu").test(changelog), `CHANGELOG.md has no [${version}] section`);
expect((await text("docs/releases/delivery-contract.md")).includes(`exact \`${version}\``),
  `docs/releases/delivery-contract.md does not state the exact ${version} pin`);

if (problems.length > 0) {
  process.stderr.write(`Release version check failed for ${version}:\n${problems.map((problem) => `  - ${problem}`).join("\n")}\n`);
  process.exit(1);
}
process.stdout.write(`release version ${version} is consistent${tag !== undefined ? ` with tag ${tag}` : ""}\n`);
