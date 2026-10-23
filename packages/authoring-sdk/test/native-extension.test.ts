import assert from "node:assert/strict";
import {createHash, createPublicKey, generateKeyPairSync, sign} from "node:crypto";
import test from "node:test";

import {
  manifestPlatforms,
  nativeManifestDiagnostics,
  nativeSignatureDiagnostics,
  nativeSignatureMessage,
  readExtensionNativePack,
  resolveExtensionVersion,
  validateDocument,
  verifyNativeSignature,
  writeExtensionPackage,
  type ExtensionManifest,
} from "../src/index.js";

const sha = (text: string) => createHash("sha256").update(text).digest("hex");

function manifest(extra: Partial<ExtensionManifest> = {}): ExtensionManifest {
  return {format: "PrismatiXExtension", schemaRevision: 1, language: "javascript", id: "glow", version: "1.2.0",
    requiredEngineVersion: "^0.1.0", entry: "dist/main.js", capabilities: ["runtime", "native"], commands: [], actions: [],
    native: {abi: 1, services: ["glow.b", "glow.a"], libraries: {
      "win32-x64": {path: "native/win32-x64/glow.dll", sha256: sha("dll"), files: [{path: "native/win32-x64/zlib.dll", sha256: sha("zlib")}]},
    }}, ...extra};
}

function keyPair() {
  const {privateKey} = generateKeyPairSync("ec", {namedCurve: "prime256v1"});
  const jwk = createPublicKey(privateKey).export({format: "jwk"});
  const hex = (value: string) => Buffer.from(value, "base64url").toString("hex").padStart(64, "0");
  return {privateKey, publicKey: `04${hex(jwk.x!)}${hex(jwk.y!)}`};
}

test("the manifest contract accepts raw capabilities, platforms and native libraries", () => {
  const result = validateDocument<ExtensionManifest>("extension", {...manifest(), platforms: ["win32-x64"],
    capabilities: ["runtime", "raw.mesh", "raw.texture", "raw.target", "native"]}, "glow.pxextension");
  assert.ok(result.valid, JSON.stringify(result.diagnostics));
  const unknown = validateDocument<ExtensionManifest>("extension", {...manifest(), platforms: ["amiga"]}, "glow.pxextension");
  assert.equal(unknown.valid, false);
});

test("native manifest checks: capability, ABI, platforms and what pack fills", () => {
  assert.deepEqual(nativeManifestDiagnostics(manifest(), "glow.pxextension", false), []);
  const codes = (value: ExtensionManifest, packaged = false) =>
    nativeManifestDiagnostics(value, "glow.pxextension", packaged).map((item) => item.code);
  assert.ok(codes(manifest({capabilities: ["runtime"]})).includes("PXEXTNAT1001"));
  assert.ok(codes(manifest({native: {...manifest().native!, abi: 2}})).includes("PXEXTNAT1002"));
  assert.ok(codes(manifest({platforms: ["win32-x64", "web"]})).includes("PXEXTNAT1003"), "native code never runs on the Web");
  assert.ok(codes(manifest({platforms: ["win32-x64", "linux-x64"]})).includes("PXEXTNAT1003"), "a listed platform needs its library");
  assert.ok(codes(manifest({native: {...manifest().native!, libraries: {"win32-x64": {path: "native/win32-x64/glow.so"}}}}))
    .includes("PXEXTNAT1004"), "a Windows library is a .dll");
  assert.ok(codes(manifest(), true).includes("PXEXTNAT1005"), "a package is signed");
  assert.deepEqual(manifestPlatforms(manifest()), ["win32-x64"]);
  assert.deepEqual(manifestPlatforms({}), ["win32-x64", "linux-x64", "darwin-x64", "darwin-arm64", "android-arm64", "web"]);
});

test("the signed descriptor binds id, version, platform, ABI, services and every file", async () => {
  const message = nativeSignatureMessage(manifest(), "win32-x64");
  assert.equal(message, [
    "PrismatiX native plugin v1", "id glow", "version 1.2.0", "platform win32-x64", "abi 1", "services glow.a,glow.b",
    `file ${sha("dll")} native/win32-x64/glow.dll`, `file ${sha("zlib")} native/win32-x64/zlib.dll`, ""].join("\n"));
  const {privateKey, publicKey} = keyPair();
  const signature = sign("sha256", Buffer.from(message), {key: privateKey, dsaEncoding: "ieee-p1363"}).toString("hex");
  assert.ok(await verifyNativeSignature(message, signature, publicKey));
  assert.equal(await verifyNativeSignature(message.replace("1.2.0", "1.2.1"), signature, publicKey), false);
  const signed = manifest({native: {...manifest().native!, publicKey, libraries: {
    "win32-x64": {...manifest().native!.libraries["win32-x64"]!, signature}}}});
  assert.deepEqual(await nativeSignatureDiagnostics(signed, "glow.pxextension"), []);
  const swapped = manifest({native: {...signed.native!, libraries: {
    "win32-x64": {...signed.native!.libraries["win32-x64"]!, sha256: sha("other")}}}});
  assert.deepEqual((await nativeSignatureDiagnostics(swapped, "glow.pxextension")).map((item) => item.code), ["PXEXTNAT1006"]);
});

test("a native pack holds exactly the pinned files of its platform", async () => {
  const encode = (text: string) => new TextEncoder().encode(text);
  const good = writeExtensionPackage([{path: "native/win32-x64/glow.dll", data: encode("dll")},
    {path: "native/win32-x64/zlib.dll", data: encode("zlib")}]);
  assert.ok((await readExtensionNativePack(good, manifest(), "win32-x64")).valid);
  const altered = writeExtensionPackage([{path: "native/win32-x64/glow.dll", data: encode("evil")},
    {path: "native/win32-x64/zlib.dll", data: encode("zlib")}]);
  assert.deepEqual((await readExtensionNativePack(altered, manifest(), "win32-x64")).diagnostics.map((item) => item.code), ["PXEXTPKG1016"]);
  const extra = writeExtensionPackage([{path: "native/win32-x64/glow.dll", data: encode("dll")},
    {path: "native/win32-x64/zlib.dll", data: encode("zlib")}, {path: "native/win32-x64/payload.dll", data: encode("x")}]);
  assert.equal((await readExtensionNativePack(extra, manifest(), "win32-x64")).valid, false);
  assert.equal((await readExtensionNativePack(good, manifest(), "linux-x64")).valid, false);
});

test("resolution with a platform skips versions that do not run there", () => {
  const versions = [
    {version: "2.0.0", requiredEngineVersion: "^0.1.0", yanked: false, platforms: ["win32-x64" as const]},
    {version: "1.5.0", requiredEngineVersion: "^0.1.0", yanked: false},
  ];
  assert.equal(resolveExtensionVersion(versions, "*", "0.1.0", "win32-x64").version?.version, "2.0.0");
  assert.equal(resolveExtensionVersion(versions, "*", "0.1.0", "linux-x64").version?.version, "1.5.0", "no platforms means every platform");
  assert.equal(resolveExtensionVersion(versions, "^2", "0.1.0", "web").problem, "platform");
});

test("userFiles: names the maker supplies, outside the package and the signature", () => {
  const library = manifest().native!.libraries["win32-x64"]!;
  const withUserFiles = (userFiles: unknown) => manifest({native: {...manifest().native!,
    libraries: {"win32-x64": {...library, userFiles}}}} as Partial<ExtensionManifest>);
  const cubism = [{name: "Live2DCubismCore.dll", hint: "Download the Cubism SDK for Native from live2d.com.",
    sha256s: [sha("core")]}];
  const accepted = validateDocument<ExtensionManifest>("extension", withUserFiles(cubism), "glow.pxextension");
  assert.ok(accepted.valid, JSON.stringify(accepted.diagnostics));
  for (const bad of [[{name: "../evil.dll"}], [{name: "sub/core.dll"}], [{name: ".."}], [{name: "core.dll", sha256s: ["nothex"]}],
    [{name: "core.dll", extra: true}]])
    assert.equal(validateDocument<ExtensionManifest>("extension", withUserFiles(bad), "glow.pxextension").valid, false,
      JSON.stringify(bad));
  const codes = (userFiles: unknown) => nativeManifestDiagnostics(withUserFiles(userFiles), "glow.pxextension", false)
    .map((item) => item.code);
  assert.deepEqual(codes(cubism), []);
  assert.ok(codes([{name: "ZLIB.dll"}]).includes("PXEXTNAT1004"), "a user file cannot take a shipped file's name");
  assert.ok(codes([{name: "core.dll"}, {name: "Core.dll"}]).includes("PXEXTNAT1004"), "names are unique, ignoring case");
  assert.equal(nativeSignatureMessage(withUserFiles(cubism), "win32-x64"), nativeSignatureMessage(manifest(), "win32-x64"),
    "the signature does not cover user files");
});
