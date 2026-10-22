import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import test from "node:test";

import {
  compareSemVer,
  extensionFilesDigest,
  extensionPackageFileName,
  isExactVersion,
  isSafePackagePath,
  maxSatisfyingVersion,
  parseExtensionPackageId,
  readExtensionPackage,
  resolveExtensionVersion,
  satisfiesVersionRange,
  validateDocument,
  validateExtensionPackage,
  validateRegistryDocument,
  writeExtensionPackage,
} from "../src/index.js";

const encoder = new TextEncoder();
const manifest = {format: "PrismatiXExtension", schemaRevision: 1, language: "javascript", id: "glow", version: "1.2.0",
  requiredEngineVersion: ">=0.1.0 <0.2.0", entry: "dist/main.js", capabilities: ["runtime"],
  safety: {previewSafe: true, deterministic: true, seekSafe: true, rollbackSafe: true},
  commands: [{id: "glow.shine", parameters: []}], actions: []};
const metadata = {format: "PrismatiXExtensionPackage", formatVersion: 1, id: "owen/glow", version: "1.2.0",
  manifest: "glow.pxextension", license: "MIT"};

function files(overrides: Record<string, unknown> = {}, extra: {path: string; data: Uint8Array}[] = []) {
  return [
    {path: "pxext.json", data: encoder.encode(JSON.stringify({...metadata, ...overrides}))},
    {path: "glow.pxextension", data: encoder.encode(JSON.stringify(manifest))},
    {path: "dist/main.js", data: encoder.encode("Engine.RegisterCommand('glow.shine', () => {});\n")},
    {path: "README.md", data: encoder.encode("# Glow\n")},
    {path: "LICENSE", data: encoder.encode("MIT\n")},
    ...extra,
  ];
}

test("SemVer ranges follow npm for the forms extension ranges use", () => {
  const cases: [string, string, boolean][] = [
    ["^1.2.0", "1.9.9", true], ["^1.2.0", "2.0.0", false], ["^0.2.1", "0.2.9", true], ["^0.2.1", "0.3.0", false],
    ["~1.2", "1.2.7", true], ["~1.2", "1.3.0", false], ["1", "1.4.0", true], ["1.x", "2.0.0", false], ["*", "3.0.0", true],
    [">=0.1.0 <0.2.0", "0.1.5", true], [">= 0.1.0 < 0.2.0", "0.2.0", false], ["1.0.0 || ^2", "2.3.0", true],
    ["*", "1.0.0-beta.1", false], [">=1.0.0-beta.1", "1.0.0-beta.2", true], ["1.0.0-beta.1", "1.0.0-beta.1", true],
  ];
  for (const [range, version, expected] of cases) assert.equal(satisfiesVersionRange(version, range), expected, `${version} in ${range}`);
  assert.equal(satisfiesVersionRange("1.0.0", "not a range"), undefined);
  assert.equal(compareSemVer("1.0.0-alpha", "1.0.0-alpha.1") < 0, true);
  assert.equal(compareSemVer("1.0.0-beta.11", "1.0.0-beta.2") > 0, true);
  assert.equal(compareSemVer("1.0.0", "1.0.0-rc.1") > 0, true);
  assert.equal(maxSatisfyingVersion(["1.0.0", "1.4.2", "2.0.0", "1.10.0"], "^1"), "1.10.0");
  assert.equal(isExactVersion("=1.2.3"), true);
  assert.equal(isExactVersion("^1.2.3"), false);
});

test("resolveExtensionVersion skips yanked and engine-incompatible versions", () => {
  const versions = [
    {version: "1.0.0", requiredEngineVersion: "^0.1.0", yanked: false},
    {version: "1.0.1", requiredEngineVersion: "^0.1.0", yanked: true},
    {version: "1.1.0", requiredEngineVersion: "^0.2.0", yanked: false},
  ];
  assert.equal(resolveExtensionVersion(versions, "^1", "0.1.0").version?.version, "1.0.0");
  assert.equal(resolveExtensionVersion(versions, "^1", "0.2.0").version?.version, "1.1.0");
  assert.equal(resolveExtensionVersion(versions, "1.0.1", "0.1.0").version?.version, "1.0.1", "an exact pin installs a yanked version");
  assert.deepEqual(resolveExtensionVersion(versions, "1.1.0", "0.1.0").problem, "engine");
  assert.deepEqual(resolveExtensionVersion(versions, "^2", "0.1.0").problem, "no-match");
  assert.deepEqual(resolveExtensionVersion(versions, "nope", "0.1.0").problem, "invalid-range");
});

test("package ids, file names and paths", () => {
  assert.deepEqual(parseExtensionPackageId("owen/glow"), {owner: "owen", name: "glow"});
  for (const id of ["Owen/glow", "owen/Glow", "owen", "owen/glow/x", "-owen/glow", "ow--en/glow", "owen/.glow"])
    assert.equal(parseExtensionPackageId(id), undefined, id);
  assert.equal(extensionPackageFileName("owen/glow", "1.2.0"), "owen-glow-1.2.0.pxext");
  for (const path of ["../x", "a/../b", "/abs", "a\\b", "C:x", "a//b", ".hidden", "CON", "nul.txt", "a/b.", "é.js", "a b"])
    assert.equal(isSafePackagePath(path), false, path);
  assert.equal(isSafePackagePath("dist/lib/util.js"), true);
});

test("writeExtensionPackage is canonical and validateExtensionPackage accepts it", async () => {
  const bytes = writeExtensionPackage(files());
  const reversed = writeExtensionPackage([...files()].reverse());
  assert.deepEqual(reversed, bytes, "order does not matter: the same files give the same bytes");
  const result = await validateExtensionPackage(bytes);
  assert.equal(result.valid, true, JSON.stringify(result.diagnostics));
  assert.deepEqual(result.diagnostics, []);
  assert.equal(result.id, "owen/glow");
  assert.equal(result.version, "1.2.0");
  assert.equal(result.sha256, createHash("sha256").update(bytes).digest("hex"));
  assert.equal(result.manifest?.entry, "dist/main.js");
  const digest = await extensionFilesDigest(result.files);
  assert.match(digest, /^[0-9a-f]{64}$/u);
  assert.notEqual(await extensionFilesDigest([...result.files].slice(1)), digest);
  assert.throws(() => writeExtensionPackage([...files(), {path: "../x", data: new Uint8Array()}]), /Unsafe/u);
  assert.throws(() => writeExtensionPackage([...files(), {path: "readme.md", data: new Uint8Array()}]), /case/u);
});

test("validateExtensionPackage reports contents a package must not have", async () => {
  const codes = async (bytes: Uint8Array) => (await validateExtensionPackage(bytes)).diagnostics.map((item) => item.code);
  assert.deepEqual(await codes(new Uint8Array([1, 2, 3])), ["PXEXTPKG1001"]);
  assert.ok((await codes(writeExtensionPackage(files().filter((file) => file.path !== "pxext.json")))).includes("PXEXTPKG1007"));
  assert.ok((await codes(writeExtensionPackage(files({id: "owen/other"})))).includes("PXEXTPKG1009"));
  assert.ok((await codes(writeExtensionPackage(files({version: "1.3.0"})))).includes("PXEXTPKG1010"));
  assert.ok((await codes(writeExtensionPackage(files().filter((file) => file.path !== "dist/main.js")))).includes("PXEXTPKG1010"));
  assert.ok((await codes(writeExtensionPackage(files({}, [{path: "install.sh", data: encoder.encode("x")}])))).includes("PXEXTPKG1011"));
  assert.ok((await codes(writeExtensionPackage(files({icon: "icon.png"}, [{path: "icon.png", data: encoder.encode("not png")}]))))
    .includes("PXEXTPKG1012"));
  assert.ok((await codes(writeExtensionPackage(files({}, [{path: "dist/extra.js", data: encoder.encode("")}])))).includes("PXEXTPKG2004"));
  const bytes = writeExtensionPackage(files());
  const damaged = bytes.slice();
  damaged[40] = damaged[40]! ^ 0xff;
  assert.ok((await codes(damaged)).some((code) => code === "PXEXTPKG1006" || code === "PXEXTPKG1001"));
  assert.equal(readExtensionPackage(damaged).files.size, 0, "a damaged archive yields no files to extract");
});

test("registry documents and the lock validate against their contracts", () => {
  const version = {id: "owen/glow", version: "1.2.0", requiredEngineVersion: ">=0.1.0 <0.2.0", sha256: "a".repeat(64), size: 10,
    download: "owen-glow-1.2.0.pxext", manifest, metadata, yanked: false, extra: "registries may add fields"};
  assert.equal(validateRegistryDocument("version", version).valid, true);
  assert.equal(validateRegistryDocument("version", {...version, sha256: "xyz"}).valid, false);
  assert.equal(validateRegistryDocument("searchResponse", {results: [{id: "owen/glow", latestVersion: "1.2.0",
    requiredEngineVersion: "*", capabilities: []}], total: 1, page: 1, pageSize: 20}).valid, true);
  assert.equal(validateRegistryDocument("error", {error: {code: "not_found", message: "no"}}).valid, true);
  assert.equal(validateDocument("extensionLock", {format: "PrismatiXExtensionLock", lockVersion: 1, extensions: {
    "owen/glow": {version: "1.2.0", sha256: "a".repeat(64), size: 10, source: "registry+https://example.com", range: "^1.2.0",
      manifest: "Extensions/owen/glow/glow.pxextension", files: "b".repeat(64)}}}).valid, true);
  assert.equal(validateDocument("extensionLock", {format: "PrismatiXExtensionLock", lockVersion: 1, extensions: {
    "Owen/Glow": {}}}).valid, false);
});
