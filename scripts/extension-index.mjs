#!/usr/bin/env node
// Builds a static PrismatiX extension registry: the GET routes of the
// registry API v1 as plain files, for GitHub Pages or any static host.
//
//   node scripts/extension-index.mjs <entries-dir> <out-dir>
//
// <entries-dir> holds one JSON file per extension (any folder layout), each
//   {"id": "owner/name", "versions": [{"url": "https://.../owner-name-1.0.0.pxext",
//     "sha256": "<optional pin>", "yanked": false, "yankedReason": "...", "publishedAt": "2026-10-22T00:00:00Z"}]}
// A version's url may be relative to its entry file (a package kept beside it).
// A native extension's version also has its native packs: by default the
// `<owner>-<name>-<version>-native-<platform>.pxext` beside its url, or
// "native": {"<platform>": {"url": "...", "sha256": "<optional pin>"}}.
//
// Every package is downloaded, checked with validateExtensionPackage (the
// check `prismatix extension add` and a registry run) and hashed; its id and
// version come from the package. <out-dir> receives
//   v1/search/index.json
//   v1/extensions/<owner>/<name>/index.json            (+ icon.png, screenshots/)
//   v1/extensions/<owner>/<name>/versions/<version>/index.json
// docs/extensions-sharing.md, "Static index", describes the layout. Needs only
// @prismatix/authoring-sdk, so an index repository's CI can run it as is.

import {mkdir, readdir, readFile, rm, writeFile} from "node:fs/promises";
import {dirname, join, relative, resolve} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";

import {createHash} from "node:crypto";

import {
  compareSemVer,
  extensionSides,
  manifestPlatforms,
  nativeExtensionPlatforms,
  nativePackFileName,
  parseExtensionPackageId,
  parseSemVer,
  readExtensionNativePack,
  validateExtensionPackage,
  validateRegistryDocument,
} from "@prismatix/authoring-sdk";

const usage = "Use node scripts/extension-index.mjs <entries-dir> <out-dir>";
const loopback = new Set(["localhost", "127.0.0.1", "[::1]"]);

async function jsonFiles(directory) {
  const found = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) found.push(...await jsonFiles(path));
    else if (entry.isFile() && entry.name.endsWith(".json")) found.push(path);
  }
  return found.sort();
}

async function download(url) {
  if (url.protocol === "file:") return new Uint8Array(await readFile(fileURLToPath(url)));
  if (url.protocol !== "https:" && !(url.protocol === "http:" && loopback.has(url.hostname)))
    throw new Error(`${url.href}: packages are served over https`);
  const response = await fetch(url, {redirect: "follow"});
  if (!response.ok) throw new Error(`${url.href} answered ${response.status} ${response.statusText}`);
  return new Uint8Array(await response.arrayBuffer());
}

async function write(path, value) {
  await mkdir(dirname(path), {recursive: true});
  await writeFile(path, typeof value === "string" || value instanceof Uint8Array ? value : `${JSON.stringify(value, null, 2)}\n`);
}

function checked(kind, value, where) {
  const result = validateRegistryDocument(kind, value, where);
  if (!result.valid) throw new Error(`${where}: ${result.diagnostics.map((item) => item.details ?? item.message).join("; ")}`);
  return value;
}

/** Builds the index; returns the extension ids written. */
export async function buildExtensionIndex(entriesDirectory, outputDirectory, log = (message) => process.stderr.write(`${message}\n`)) {
  const entries = await jsonFiles(entriesDirectory);
  const extensions = [];
  const seen = new Set();
  for (const file of entries) {
    const where = relative(entriesDirectory, file);
    const entry = JSON.parse((await readFile(file, "utf8")).replace(/^\uFEFF/u, ""));
    const parsed = typeof entry.id === "string" ? parseExtensionPackageId(entry.id) : undefined;
    if (parsed === undefined || !Array.isArray(entry.versions) || entry.versions.length === 0)
      throw new Error(`${where}: an entry is {"id": "owner/name", "versions": [{"url": ...}, ...]}`);
    if (seen.has(entry.id)) throw new Error(`${where}: ${entry.id} is listed twice`);
    seen.add(entry.id);
    const versions = [];
    for (const item of entry.versions) {
      if (typeof item?.url !== "string") throw new Error(`${where}: every version needs a url`);
      const url = new URL(item.url, pathToFileURL(file));
      log(`Checking ${entry.id} ${url.protocol === "file:" ? relative(entriesDirectory, fileURLToPath(url)) : url.href}`);
      const bytes = await download(url);
      const validation = await validateExtensionPackage(bytes);
      if (!validation.valid)
        throw new Error(`${where}: ${url.href} is not a valid package: ${validation.diagnostics.filter((d) => d.severity === "error")
          .map((d) => `${d.code} ${d.message}${d.path ? ` (${d.path})` : ""}`).join("; ")}`);
      if (validation.id !== entry.id) throw new Error(`${where}: ${url.href} is ${validation.id}, not ${entry.id}`);
      if (item.sha256 !== undefined && item.sha256 !== validation.sha256)
        throw new Error(`${where}: ${url.href} has SHA-256 ${validation.sha256}, not the pinned ${item.sha256}`);
      if (versions.some((other) => other.validation.version === validation.version))
        throw new Error(`${where}: version ${validation.version} is listed twice`);
      for (const warning of validation.diagnostics.filter((d) => d.severity === "warning"))
        log(`  warning ${warning.code}: ${warning.message}`);
      // Native packs, checked against the hashes the package's manifest pins.
      const natives = [];
      for (const platform of nativeExtensionPlatforms) {
        if (validation.manifest.native?.libraries[platform] === undefined) continue;
        const name = nativePackFileName(entry.id, validation.version, platform);
        const pinned = item.native?.[platform];
        const packUrl = new URL(pinned?.url ?? name, pinned?.url === undefined ? url : pathToFileURL(file));
        const packBytes = await download(packUrl);
        const pack = await readExtensionNativePack(packBytes, validation.manifest, platform);
        if (!pack.valid) throw new Error(`${where}: ${packUrl.href} is not a valid ${platform} native pack: ` +
          pack.diagnostics.map((d) => `${d.code} ${d.message}`).join("; "));
        const sha256 = createHash("sha256").update(packBytes).digest("hex");
        if (pinned?.sha256 !== undefined && pinned.sha256 !== sha256)
          throw new Error(`${where}: ${packUrl.href} has SHA-256 ${sha256}, not the pinned ${pinned.sha256}`);
        natives.push({platform, name, url: packUrl, bytes: packBytes, sha256});
      }
      versions.push({item, url, bytes, validation, natives});
    }
    versions.sort((left, right) => compareSemVer(right.validation.version, left.validation.version));
    extensions.push({id: entry.id, ...parsed, versions});
  }

  await rm(join(outputDirectory, "v1"), {recursive: true, force: true});
  const results = [];
  for (const extension of extensions) {
    const base = join(outputDirectory, "v1", "extensions", extension.owner, extension.name);
    const stable = extension.versions.filter((item) => !item.item.yanked && parseSemVer(item.validation.version).prerelease.length === 0);
    const shown = stable[0] ?? extension.versions.find((item) => !item.item.yanked) ?? extension.versions[0];
    const metadata = shown.validation.metadata;
    const manifest = shown.validation.manifest;
    // Absent `platforms` means every platform (a JavaScript-only extension).
    const platformsOf = (value) => value.platforms === undefined && value.native === undefined ? {}
      : {platforms: manifestPlatforms(value)};
    // A Studio side is announced; a game-only extension leaves `sides` out.
    const sidesOf = (value) => value.studio === undefined ? {}
      : {sides: extensionSides(value), requiredStudioVersion: value.studio.requiredStudioVersion};
    for (const item of extension.versions) {
      // A package kept beside its entry is published next to its version document.
      if (item.url.protocol === "file:") {
        const name = `${extension.owner}-${extension.name}-${item.validation.version}.pxext`;
        await write(join(base, "versions", item.validation.version, name), item.bytes);
      }
      const platformDownloads = {};
      for (const native of item.natives) {
        if (native.url.protocol === "file:")
          await write(join(base, "versions", item.validation.version, native.name), native.bytes);
        platformDownloads[native.platform] = {download: native.url.protocol === "file:" ? native.name : native.url.href,
          sha256: native.sha256, size: native.bytes.length};
      }
      const document = checked("version", {
        id: extension.id, version: item.validation.version, requiredEngineVersion: item.validation.manifest.requiredEngineVersion,
        sha256: item.validation.sha256, size: item.validation.size,
        download: item.url.protocol === "file:" ? `${extension.owner}-${extension.name}-${item.validation.version}.pxext` : item.url.href,
        manifest: item.validation.manifest, metadata: item.validation.metadata, ...platformsOf(item.validation.manifest),
        ...(item.natives.length === 0 ? {} : {platformDownloads}),
        yanked: item.item.yanked === true,
        ...(typeof item.item.yankedReason === "string" && item.item.yanked ? {yankedReason: item.item.yankedReason} : {}),
        ...(typeof item.item.publishedAt === "string" ? {publishedAt: item.item.publishedAt} : {}),
      }, `${extension.id}@${item.validation.version}`);
      await write(join(base, "versions", item.validation.version, "index.json"), document);
    }
    const media = {};
    const icon = shown.validation.files.get("icon.png");
    if (metadata.icon !== undefined && icon !== undefined) {
      await write(join(base, "icon.png"), icon);
      media.icon = "icon.png";
    }
    if (metadata.screenshots !== undefined) {
      for (const shot of metadata.screenshots) await write(join(base, ...shot.split("/")), shown.validation.files.get(shot));
      media.screenshots = metadata.screenshots;
    }
    const details = checked("extension", {
      id: extension.id, owner: extension.owner, name: extension.name,
      ...(metadata.displayName === undefined ? {} : {displayName: metadata.displayName}),
      ...(metadata.description ? {description: metadata.description} : {}),
      ...(metadata.author === undefined ? {} : {author: metadata.author}),
      ...(metadata.license === undefined ? {} : {license: metadata.license}),
      ...(metadata.homepage === undefined ? {} : {homepage: metadata.homepage}),
      ...(metadata.repository === undefined ? {} : {repository: metadata.repository}),
      ...(metadata.keywords === undefined ? {} : {keywords: metadata.keywords}),
      ...media, capabilities: manifest.capabilities ?? [], ...platformsOf(manifest), ...sidesOf(manifest),
      ...(manifest.native === undefined ? {} : {native: true}),
      latestVersion: stable[0]?.validation.version ?? null,
      versions: extension.versions.map((item) => ({version: item.validation.version,
        requiredEngineVersion: item.validation.manifest.requiredEngineVersion, yanked: item.item.yanked === true,
        sha256: item.validation.sha256, size: item.validation.size, ...platformsOf(item.validation.manifest),
        ...(typeof item.item.publishedAt === "string" ? {publishedAt: item.item.publishedAt} : {})})),
    }, extension.id);
    await write(join(base, "index.json"), details);
    if (stable[0] !== undefined) results.push({id: extension.id, owner: extension.owner, name: extension.name,
      ...(metadata.displayName === undefined ? {} : {displayName: metadata.displayName}),
      ...(metadata.description ? {description: metadata.description} : {}),
      latestVersion: stable[0].validation.version, requiredEngineVersion: manifest.requiredEngineVersion,
      capabilities: manifest.capabilities ?? [], ...platformsOf(manifest), ...sidesOf(manifest),
      ...(manifest.native === undefined ? {} : {native: true}),
      ...(metadata.keywords === undefined ? {} : {keywords: metadata.keywords}),
      ...(media.icon === undefined ? {} : {icon: `../extensions/${extension.owner}/${extension.name}/icon.png`}),
      ...(typeof stable[0].item.publishedAt === "string" ? {updatedAt: stable[0].item.publishedAt} : {})});
  }
  results.sort((left, right) => left.id < right.id ? -1 : left.id > right.id ? 1 : 0);
  await write(join(outputDirectory, "v1", "search", "index.json"),
    checked("searchResponse", {results, total: results.length, page: 1, pageSize: Math.max(1, results.length)}, "search"));
  return extensions.map((extension) => extension.id);
}

if (process.argv[1] !== undefined && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [entries, output, ...rest] = process.argv.slice(2);
  if (entries === undefined || output === undefined || rest.length > 0) {
    process.stderr.write(`${usage}\n`);
    process.exit(2);
  }
  try {
    const ids = await buildExtensionIndex(resolve(entries), resolve(output));
    process.stdout.write(`Indexed ${ids.length} extension(s) into ${resolve(output)}/v1\n`);
  } catch (error) {
    process.stderr.write(`error: ${error instanceof Error ? error.message : String(error)}\n`);
    process.exit(1);
  }
}
