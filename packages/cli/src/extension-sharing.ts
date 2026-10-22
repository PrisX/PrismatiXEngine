// `prismatix extension pack | add | remove | update | outdated | verify |
// inspect | search | info | publish | yank | registry`: sharing extensions as
// .pxext packages. PrismatiX Studio's Extensions workspace runs these same
// commands with --json.

import {existsSync} from "node:fs";
import {readFile, writeFile} from "node:fs/promises";
import {join, resolve} from "node:path";

import {
  ENGINE_VERSION,
  EXTENSION_PACKAGE_SUFFIX,
  parseExtensionPackageId,
  parseSemVer,
  resolveExtensionVersion,
  validateExtensionPackage,
  type AuthoringDiagnostic,
} from "@prismatix/authoring-sdk";

import {
  addExtension,
  describeSummary,
  fetchExtensionPackage,
  outdatedExtensions,
  removeExtension,
  summarizePackage,
  updateExtensions,
  verifyExtensions,
  type InstallOptions,
} from "./extension-install.js";
import {displayPath, packExtension} from "./extension-pack.js";
import {
  locateRegistry,
  noRegistryMessage,
  REGISTRY_SETTING,
  REGISTRY_TOKEN_ENVIRONMENT,
  RegistryClient,
  requireRegistry,
  resolutionMessage,
} from "./extension-registry.js";
import {projectManifest} from "./extension-scaffold.js";
import * as tui from "./tui.js";

export const sharingSubcommands = new Set(["pack", "add", "remove", "update", "outdated", "verify", "inspect", "search",
  "info", "publish", "yank", "registry"]);

/** Subcommands that also work outside a project. */
export const projectlessSubcommands = new Set(["search", "info", "publish", "yank", "inspect"]);

/** Allowed positional counts (including the subcommand). */
export const sharingArity: Readonly<Record<string, readonly number[]>> = {
  pack: [1, 2], add: [2], remove: [2], update: [1, 2], outdated: [1], verify: [1], inspect: [2], search: [1, 2],
  info: [2], publish: [1, 2], yank: [2], registry: [1, 2],
};

export const sharingHelp = [
  "",
  "Sharing (docs/extensions-sharing.md):",
  "prismatix extension pack [extension] [--owner <owner>] [--output <dir>] [--include-source]",
  "prismatix extension add <owner/name[@range] | ./file.pxext | https://.../file.pxext | npm:<package>[@range]>",
  "    [--yes] [--sha256 <hex>] [--registry <url>]",
  "prismatix extension remove <owner/name>",
  "prismatix extension update [owner/name] [--to <range>] [--yes]",
  "prismatix extension outdated | verify",
  "prismatix extension inspect <source>",
  "prismatix extension search [query] [--capability <name>] [--page <n>]",
  "prismatix extension info <owner/name>",
  "prismatix extension publish [file.pxext | extension] [--owner <owner>]   (token: PRISMATIX_REGISTRY_TOKEN)",
  "prismatix extension yank <owner/name@version> [--undo] [--reason <text>]",
  "prismatix extension registry [<url> | none]",
  "",
  "pack writes dist/extensions/<owner>-<name>-<version>.pxext from the extension's folder and its pxext.json",
  "(--owner creates pxext.json). add checks the package's SHA-256, form and engine range, shows what it may",
  "do and installs it into Extensions/<owner>/<name>/, registered and recorded in prismatix-extensions.lock;",
  "nothing in a package runs at install time. The registry is --registry, PRISMATIX_EXTENSION_REGISTRY or",
  `the project's settings["${REGISTRY_SETTING}"]; there is no default.`,
];

interface CommandOptions {
  readonly values: ReadonlyMap<string, string>;
  readonly flags: ReadonlySet<string>;
  readonly positional: readonly string[];
}

export interface SharingOutput {
  readonly text: string;
  readonly status: number;
  readonly warnings?: readonly AuthoringDiagnostic[];
}

function installOptions(options: CommandOptions, progress: (message: string) => void): InstallOptions {
  return {
    ...(options.values.has("registry") ? {registry: options.values.get("registry")!} : {}),
    ...(options.values.has("sha256") ? {sha256: options.values.get("sha256")!} : {}),
    yes: options.flags.has("yes"),
    onProgress: progress,
    // Without a terminal (scripts, CI, --json, Studio) only --yes installs.
    ...(tui.canPrompt() ? {confirm: async (summary: string) => {
      process.stderr.write(`${summary}\n`);
      const answer = await tui.confirm({message: "Install it?", initialValue: false});
      return answer === true;
    }} : {}),
  };
}

function json(value: unknown): string {
  return `${JSON.stringify(value)}\n`;
}

async function setRegistry(root: string, url: string | undefined): Promise<string> {
  const file = await projectManifest(root);
  const path = join(root, file);
  const value = JSON.parse((await readFile(path, "utf8")).replace(/^﻿/u, "")) as {settings?: Record<string, unknown>};
  const settings = {...value.settings ?? {}};
  if (url === undefined) delete settings[REGISTRY_SETTING];
  else settings[REGISTRY_SETTING] = url;
  if (Object.keys(settings).length === 0) delete value.settings;
  else value.settings = settings;
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  return file;
}

/** Runs one sharing subcommand; `root` is undefined outside a project. */
export async function extensionSharingCommand(root: string | undefined, options: CommandOptions, machine: boolean,
  progress: (message: string) => void): Promise<SharingOutput> {
  const [subcommand, first] = options.positional;
  const project = (): string => {
    if (root === undefined) throw new Error(`extension ${subcommand} runs in a project (--project <dir>)`);
    return root;
  };
  const engine = ENGINE_VERSION;

  if (subcommand === "pack") {
    const result = await packExtension(project(), first, {
      ...(options.values.has("owner") ? {owner: options.values.get("owner")!} : {}),
      ...(options.values.has("output") ? {output: options.values.get("output")!} : {}),
      includeSource: options.flags.has("include-source")});
    return {status: 0, warnings: result.warnings, text: machine ? json(result) : [
      ...result.created.map((path) => `Created ${path}`),
      ...(result.created.length > 0 ? ["  Fill in pxext.json's description, author, license and links, and README.md, before you publish."] : []),
      `Packed ${result.id}@${result.version}: ${displayPath(project(), result.file)}`,
      `  ${result.files.length} files, ${result.size} bytes, SHA-256 ${result.sha256}`, ""].join("\n")};
  }

  if (subcommand === "inspect") {
    const fetched = await fetchExtensionPackage(root ?? process.cwd(), first!, installOptions(options, progress));
    const summary = root === undefined ? undefined : await summarizePackage(root, fetched);
    const value = summary ?? {id: fetched.contents.metadata!.id, version: fetched.contents.metadata!.version,
      sha256: fetched.sha256, size: fetched.bytes.length, source: fetched.source, capabilities: fetched.contents.manifest!.capabilities,
      requiredEngineVersion: fetched.contents.manifest!.requiredEngineVersion};
    return {status: 0, text: machine ? json({...value, metadata: fetched.contents.metadata, files: [...fetched.contents.files.keys()]})
      : `${summary === undefined ? JSON.stringify(value, null, 2) : describeSummary(summary)}\n` +
        `${summary !== undefined && summary.conflicts.length > 0 ? `Conflicts: ${summary.conflicts.map((item) =>
          `${item.kind} ${item.id} (${item.with})`).join(", ")}\n` : ""}`};
  }

  if (subcommand === "add") {
    const result = await addExtension(project(), first!, installOptions(options, progress));
    return {status: 0, warnings: result.summary.warnings, text: machine ? json(result) : [
      `${result.previous === undefined ? "Installed" : `Updated ${result.previous} ->`} ${result.id}@${result.version} into ${result.folder}`,
      ...result.summary.notes.map((note) => `Note: ${note}`),
      `Updated ${result.updated.filter((path) => path !== result.folder).join(", ")}`,
      ...result.summary.commands.slice(0, 1).map((command) => `Use a command in Story: [${command}]`), ""].join("\n")};
  }

  if (subcommand === "remove") {
    const result = await removeExtension(project(), first!);
    return {status: 0, text: machine ? json(result)
      : `Removed ${result.id}@${result.version} (${result.folder}); updated ${result.updated.filter((path) => path !== result.folder).join(", ")}\n`};
  }

  if (subcommand === "update") {
    const outcomes = await updateExtensions(project(), first, {...installOptions(options, progress),
      ...(options.values.has("to") ? {to: options.values.get("to")!} : {})});
    return {status: 0, text: machine ? json({updates: outcomes}) : outcomes.length === 0 ? "No installed extensions.\n"
      : `${outcomes.map((item) => item.status === "updated" ? `Updated ${item.id} ${item.from} -> ${item.to}`
        : item.status === "current" ? `${item.id} ${item.from} is current${item.reason ? ` (${item.reason})` : ""}`
          : `Skipped ${item.id}: ${item.reason}`).join("\n")}\n`};
  }

  if (subcommand === "outdated") {
    const entries = await outdatedExtensions(project(), installOptions(options, progress));
    const stale = entries.filter((item) => item.latest !== undefined && item.latest !== item.current ||
      item.wanted !== undefined && item.wanted !== item.current);
    return {status: 0, text: machine ? json({extensions: entries}) : entries.length === 0 ? "No installed extensions.\n"
      : [`${"extension".padEnd(32)} ${"current".padEnd(10)} ${"wanted".padEnd(10)} latest`,
        ...entries.map((item) => `${item.id.padEnd(32)} ${item.current.padEnd(10)} ${(item.wanted ?? "-").padEnd(10)} ${item.latest ?? "-"}` +
          `${item.problem ? `  (${item.problem})` : item.source.startsWith("registry+") ? "" : `  (${item.source})`}`),
        stale.length === 0 ? "Everything is current." : "Run prismatix extension update to take the wanted versions.", ""].join("\n")};
  }

  if (subcommand === "verify") {
    const result = await verifyExtensions(project());
    const bad = result.entries.filter((item) => item.status !== "ok");
    const status = bad.length > 0 || result.conflicts.length > 0 ? 1 : 0;
    return {status, text: machine ? json({...result, valid: status === 0}) : [
      ...result.entries.map((item) => `${item.status === "ok" ? "ok      " : `${item.status.padEnd(8)}`} ${item.id}${item.version ? `@${item.version}` : ""}` +
        `${item.details ? `: ${item.details}` : ""}`),
      ...result.conflicts.map((conflict) => `conflict ${conflict.kind} ${conflict.id}: ${conflict.with}`),
      result.entries.length === 0 ? "No installed extensions." : status === 0 ? "Every installed extension matches the lock." : "",
      ""].join("\n")};
  }

  if (subcommand === "registry") {
    if (first !== undefined) {
      const url = first === "none" ? undefined : first;
      if (url !== undefined) new RegistryClient(url);
      const file = await setRegistry(project(), url);
      return {status: 0, text: machine ? json({url: url ?? null, file})
        : url === undefined ? `Removed settings["${REGISTRY_SETTING}"] from ${file}\n` : `Set settings["${REGISTRY_SETTING}"] in ${file} to ${url}\n`};
    }
    const location = await locateRegistry(root, options.values.get("registry"));
    return {status: 0, text: machine ? json({url: location?.url ?? null, source: location?.source ?? null})
      : location === undefined ? `${noRegistryMessage}\n` : `${location.url} (from ${location.source})\n`};
  }

  if (subcommand === "search") {
    const location = await requireRegistry(root, options.values.get("registry"));
    const client = new RegistryClient(location.url);
    const page = options.values.has("page") ? Number(options.values.get("page")) : 1;
    if (!Number.isInteger(page) || page < 1) throw new Error("--page must be a positive integer");
    const result = await client.search({...(first === undefined ? {} : {query: first}), engine,
      ...(options.values.has("capability") ? {capability: options.values.get("capability")!} : {}), page});
    return {status: 0, text: machine ? json({registry: location.url, engine, ...result}) : result.results.length === 0
      ? `Nothing on ${location.url} matches${first ? ` "${first}"` : ""} for engine ${engine}.\n`
      : `${result.results.map((item) => `${item.id}@${item.latestVersion}  ${item.displayName ?? ""}` +
        `${item.description ? `\n    ${item.description}` : ""}\n    capabilities: ${item.capabilities.join(", ") || "none"}`).join("\n")}\n` +
        `${result.total > result.page * result.pageSize ? `Page ${result.page}; more with --page ${result.page + 1}\n` : ""}`};
  }

  if (subcommand === "info") {
    const location = await requireRegistry(root, options.values.get("registry"));
    const client = new RegistryClient(location.url);
    const details = await client.extension(first!);
    const resolved = resolveExtensionVersion(details.value.versions, "*", engine);
    const value = {...details.value, compatibleVersion: resolved.version?.version ?? null,
      ...(resolved.version === undefined ? {problem: resolutionMessage(first!, "*", engine, resolved)} : {})};
    return {status: 0, text: machine ? json(value) : [
      `${details.value.displayName ?? details.value.id} (${details.value.id})`,
      ...(details.value.description ? [`  ${details.value.description}`] : []),
      `  capabilities: ${details.value.capabilities.join(", ") || "none"}`,
      `  versions: ${details.value.versions.map((item) => `${item.version}${item.yanked ? " (yanked)" : ""}`).join(", ")}`,
      resolved.version === undefined ? `  ${value.problem}` : `  installs ${resolved.version.version} on engine ${engine}`,
      ""].join("\n")};
  }

  if (subcommand === "publish") {
    const token = process.env[REGISTRY_TOKEN_ENVIRONMENT];
    let file = first !== undefined && first.toLowerCase().endsWith(EXTENSION_PACKAGE_SUFFIX) ? resolve(first) : undefined;
    let warnings: readonly AuthoringDiagnostic[] = [];
    if (file === undefined) {
      const packed = await packExtension(project(), first, {
        ...(options.values.has("owner") ? {owner: options.values.get("owner")!} : {}),
        ...(options.values.has("output") ? {output: options.values.get("output")!} : {})});
      file = packed.file;
      warnings = packed.warnings;
      progress(`Packed ${packed.id}@${packed.version}`);
    } else if (!existsSync(file)) throw new Error(`${first} does not exist`);
    const bytes = new Uint8Array(await readFile(file));
    const validation = await validateExtensionPackage(bytes);
    if (!validation.valid)
      throw new Error(`${file} is not a valid package: ${validation.diagnostics.filter((item) => item.severity === "error")
        .map((item) => `${item.code} ${item.message}${item.path ? ` (${item.path})` : ""}`).join("; ")}`);
    if (parseSemVer(validation.version!) === undefined) throw new Error(`${validation.version} is not a version`);
    const location = await requireRegistry(root, options.values.get("registry"));
    const client = new RegistryClient(location.url, token === undefined ? {} : {token});
    const published = await client.publish(bytes, validation, progress);
    return {status: 0, warnings: [...warnings, ...validation.diagnostics], text: machine
      ? json({registry: location.url, file, id: validation.id, ...published.value})
      : `Published ${validation.id}@${published.value.version} to ${location.url} (SHA-256 ${published.value.sha256})\n`};
  }

  if (subcommand === "yank") {
    const at = first!.lastIndexOf("@");
    const id = at < 0 ? first! : first!.slice(0, at);
    const version = at < 0 ? undefined : first!.slice(at + 1);
    if (parseExtensionPackageId(id) === undefined || version === undefined || parseSemVer(version) === undefined)
      throw new Error("Use prismatix extension yank <owner/name@version>");
    const token = process.env[REGISTRY_TOKEN_ENVIRONMENT];
    const location = await requireRegistry(root, options.values.get("registry"));
    const client = new RegistryClient(location.url, token === undefined ? {} : {token});
    const undo = options.flags.has("undo");
    const result = await client.yank(id, version, !undo, options.values.get("reason"));
    return {status: 0, text: machine ? json({registry: location.url, id, ...result})
      : `${undo ? "Restored" : "Yanked"} ${id}@${version} on ${location.url}\n`};
  }
  throw new Error(`Unknown extension subcommand: ${subcommand}`);
}
