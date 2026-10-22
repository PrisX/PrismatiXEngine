// `prismatix extension pack`: one registered extension as a reproducible
// .pxext package (docs/extensions-sharing.md). TypeScript is bundled and
// type-checked like a build; JavaScript entries and modules are copied. The
// same sources always give the same bytes, so anyone can rebuild a published
// package and compare its SHA-256.

import {createHash, randomUUID} from "node:crypto";
import {existsSync} from "node:fs";
import {mkdir, readdir, readFile, rename, rm, writeFile} from "node:fs/promises";
import {basename, isAbsolute, join, relative, resolve, sep} from "node:path";

import {
  bundledExtensionManifest,
  EXTENSION_INSTALL_FOLDER,
  EXTENSION_PACKAGE_FILE,
  EXTENSION_TYPES_FILE,
  extensionPackageFileName,
  extensionTypeDeclarations,
  extensionUsesTypeScript,
  isSafePackagePath,
  parseExtensionPackageId,
  readExtensionPackage,
  validateDocument,
  writeExtensionPackage,
  type AuthoringDiagnostic,
  type ExtensionManifest,
  type ExtensionPackageFile,
  type ExtensionPackageMetadata,
} from "@prismatix/authoring-sdk";

import {buildTypeScriptExtensions, engineCapability} from "./extension-build.js";
import {directoryOf, findExtension, projectExtensions, readObject, type LocatedExtension} from "./extension-scaffold.js";

export interface ExtensionPackResult {
  /** Absolute path of the written .pxext. */
  readonly file: string;
  readonly id: string;
  readonly version: string;
  readonly sha256: string;
  readonly size: number;
  /** Package paths, sorted. */
  readonly files: readonly string[];
  /** Project files pack wrote (pxext.json when --owner created it). */
  readonly created: readonly string[];
  readonly warnings: readonly AuthoringDiagnostic[];
}

export class ExtensionPackFailure extends Error {
  constructor(readonly diagnostics: readonly AuthoringDiagnostic[]) {
    super(diagnostics.filter((item) => item.severity === "error")
      .map((item) => `${item.code} ${item.message}${item.path ? ` (${item.path})` : ""}${item.details ? `: ${item.details}` : ""}`).join("; "));
    this.name = "ExtensionPackFailure";
  }
}

function within(directory: string, path: string): string {
  return directory === "" ? path : `${directory}/${path}`;
}

function absolute(root: string, path: string): string {
  return join(root, ...path.split("/"));
}

function text(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

/** The extension to pack: the one named, or the project's only authored extension. */
async function chooseExtension(root: string, reference: string | undefined): Promise<LocatedExtension> {
  if (reference !== undefined) return findExtension(root, reference);
  const authored = (await projectExtensions(root))
    .filter((item) => !item.manifestPath.startsWith(`${EXTENSION_INSTALL_FOLDER}/`));
  if (authored.length === 1) return authored[0]!;
  throw new Error(authored.length === 0 ? "The project has no extension of its own to pack; create one with prismatix extension create <id>"
    : `Name the extension to pack: ${authored.map((item) => item.manifest.id).join(", ")}`);
}

// Regex scan of a JavaScript entry for Engine calls whose capability the manifest lacks.
function javascriptCapabilityFindings(source: string, path: string, capabilities: ReadonlySet<string>,
  manifestName: string): AuthoringDiagnostic[] {
  const findings: AuthoringDiagnostic[] = [];
  const seen = new Set<string>();
  for (const match of source.matchAll(/\bEngine\s*\.\s*([A-Za-z_$][\w$]*)/gu)) {
    const capability = engineCapability(match[1]!);
    if (capabilities.has(capability) || seen.has(`${match[1]}:${capability}`)) continue;
    seen.add(`${match[1]}:${capability}`);
    findings.push({severity: "warning", code: "PXCLIEXT2004",
      message: `Engine.${match[1]} needs the "${capability}" capability, which ${manifestName} does not declare`, path,
      hint: `Add "${capability}" to "capabilities" in ${manifestName}; the Player refuses the call otherwise.`});
  }
  return findings;
}

const skippedSourceFolders = new Set(["node_modules", "dist", ".git"]);

async function sourceFiles(directory: string, prefix = ""): Promise<string[]> {
  const found: string[] = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    if (entry.name.startsWith(".")) continue;
    const path = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
    if (entry.isDirectory()) {
      if (!skippedSourceFolders.has(entry.name)) found.push(...await sourceFiles(join(directory, entry.name), path));
    } else if (entry.isFile() && !entry.name.endsWith(".pxext")) found.push(path);
  }
  return found.sort();
}

/** Reads, creates (with --owner) and checks the extension's pxext.json. */
async function packageMetadata(root: string, located: LocatedExtension, owner: string | undefined,
  created: string[]): Promise<ExtensionPackageMetadata> {
  const directory = directoryOf(located.manifestPath);
  const path = within(directory, EXTENSION_PACKAGE_FILE);
  const name = located.manifest.id;
  if (!existsSync(absolute(root, path))) {
    if (owner === undefined)
      throw new Error(`${path} is missing. Run pack once with --owner <your registry user or organization> to create it, ` +
        "then fill in its description, author, license and links.");
    const id = `${owner}/${name}`;
    if (parseExtensionPackageId(id) === undefined)
      throw new Error(`${id} is not a registry id: the owner is lowercase letters, digits and single dashes (at most 39), ` +
        "and the extension id is lowercase letters, digits, \".\", \"_\" and \"-\" (at most 64)");
    const metadata: ExtensionPackageMetadata = {format: "PrismatiXExtensionPackage", formatVersion: 1, id, displayName: name,
      description: ""};
    await writeFile(absolute(root, path), `${JSON.stringify(metadata, null, 2)}\n`, {encoding: "utf8", flag: "wx"});
    created.push(path);
    const readme = within(directory, "README.md");
    if (!existsSync(absolute(root, readme))) {
      await writeFile(absolute(root, readme), [`# ${name}`, "", "What this extension adds, and how to use its Story commands and UI Actions.",
        "", ...located.manifest.commands.map((command) => `- \`[${command.id}]\`${command.description ? `: ${command.description}` : ""}`),
        ...located.manifest.actions.map((action) => `- Action \`${action.id}\`${action.description ? `: ${action.description}` : ""}`), ""].join("\n"),
        {encoding: "utf8", flag: "wx"});
      created.push(readme);
    }
  }
  const value = await readObject(absolute(root, path));
  const result = validateDocument<ExtensionPackageMetadata>("extensionPackage", value, path);
  if (!result.valid) throw new ExtensionPackFailure(result.diagnostics);
  const metadata = result.value!;
  const parsed = parseExtensionPackageId(metadata.id)!;
  if (owner !== undefined && parsed.owner !== owner)
    throw new Error(`${path} names owner ${parsed.owner}, not ${owner}; edit its id to change the owner`);
  if (parsed.name !== name)
    throw new Error(`${path} names ${metadata.id}, but the manifest id is ${name}; the package name is the manifest id`);
  return metadata;
}

export async function packExtension(root: string, reference: string | undefined, options: {
  readonly owner?: string; readonly output?: string; readonly includeSource?: boolean;
} = {}): Promise<ExtensionPackResult> {
  const located = await chooseExtension(root, reference);
  const {manifest, manifestPath} = located;
  const directory = directoryOf(manifestPath);
  const checked = validateDocument<ExtensionManifest>("extension", manifest, manifestPath);
  if (!checked.valid) throw new ExtensionPackFailure(checked.diagnostics);
  const created: string[] = [];
  const metadata = await packageMetadata(root, located, options.owner, created);
  const name = manifest.id;
  const manifestFile = `${name}.pxextension`;
  const warnings: AuthoringDiagnostic[] = [];
  const files: ExtensionPackageFile[] = [];

  // dist/: the bundle of a TypeScript extension, or the JavaScript entry and modules.
  let packaged: ExtensionManifest;
  if (extensionUsesTypeScript(manifest)) {
    const build = await buildTypeScriptExtensions(root, {[manifestPath]: manifest}, {cache: false});
    const errors = build.diagnostics.filter((item) => item.severity === "error");
    if (errors.length > 0) throw new ExtensionPackFailure(errors);
    warnings.push(...build.diagnostics);
    const bundle = build.builds[manifestPath];
    if (bundle === undefined) throw new Error(`${manifestPath}: the TypeScript entry ${manifest.entry} was not bundled`);
    files.push({path: `dist/${bundle.entry}`, data: text(bundle.code)});
    packaged = bundledExtensionManifest(manifest, `dist/${bundle.entry}`);
  } else {
    for (const script of [manifest.entry, ...manifest.modules ?? []]) {
      const path = within(directory, script);
      if (!existsSync(absolute(root, path))) throw new Error(`${manifestPath} lists ${script}, which does not exist`);
      const source = await readFile(absolute(root, path), "utf8");
      warnings.push(...javascriptCapabilityFindings(source, path, new Set(manifest.capabilities), basename(manifestPath)));
      files.push({path: `dist/${script}`, data: text(source)});
    }
    packaged = {...manifest, entry: `dist/${manifest.entry}`,
      ...(manifest.modules === undefined ? {} : {modules: manifest.modules.map((module) => `dist/${module}`)})};
  }
  files.push({path: manifestFile, data: text(`${JSON.stringify(packaged, null, 2)}\n`)});
  files.push({path: `types/${EXTENSION_TYPES_FILE}`, data: text(extensionTypeDeclarations(manifest, manifestFile))});

  // README, LICENSE, CHANGELOG, icon and screenshots from the extension's folder.
  for (const file of ["README.md", "LICENSE", "LICENSE.md", "LICENSE.txt", "CHANGELOG.md"]) {
    const path = absolute(root, within(directory, file));
    if (existsSync(path)) files.push({path: file, data: new Uint8Array(await readFile(path))});
  }
  const packagedMetadata: Record<string, unknown> = {format: metadata.format, formatVersion: metadata.formatVersion,
    id: metadata.id, version: manifest.version, manifest: manifestFile};
  for (const [key, value] of Object.entries(metadata))
    if (!(key in packagedMetadata) && key !== "icon" && key !== "screenshots" && !(key === "description" && value === ""))
      packagedMetadata[key] = value;
  if (metadata.icon !== undefined) {
    if (!/\.png$/iu.test(metadata.icon)) throw new Error(`${EXTENSION_PACKAGE_FILE} icon must be a .png file: ${metadata.icon}`);
    const path = absolute(root, within(directory, metadata.icon));
    if (!existsSync(path)) throw new Error(`The icon ${within(directory, metadata.icon)} does not exist`);
    files.push({path: "icon.png", data: new Uint8Array(await readFile(path))});
    packagedMetadata.icon = "icon.png";
  }
  if (metadata.screenshots !== undefined && metadata.screenshots.length > 0) {
    const shots: string[] = [];
    for (const shot of metadata.screenshots) {
      const path = absolute(root, within(directory, shot));
      if (!existsSync(path)) throw new Error(`The screenshot ${within(directory, shot)} does not exist`);
      const target = `screenshots/${basename(shot)}`;
      if (shots.includes(target)) throw new Error(`Two screenshots are named ${basename(shot)}`);
      shots.push(target);
      files.push({path: target, data: new Uint8Array(await readFile(path))});
    }
    packagedMetadata.screenshots = shots;
  }
  files.push({path: EXTENSION_PACKAGE_FILE, data: text(`${JSON.stringify(packagedMetadata, null, 2)}\n`)});

  if (options.includeSource === true) {
    const folder = absolute(root, directory);
    for (const path of await sourceFiles(folder)) {
      if (!isSafePackagePath(`src/${path}`))
        throw new Error(`${within(directory, path)} cannot be packaged as source: package paths use letters, digits, "_", "-" and "." only`);
      files.push({path: `src/${path}`, data: new Uint8Array(await readFile(join(folder, ...path.split("/"))))});
    }
  }
  for (const file of files)
    if (!isSafePackagePath(file.path)) throw new Error(`${file.path} cannot be packaged: package paths use letters, digits, "_", "-" and "." only`);

  const bytes = writeExtensionPackage(files);
  const contents = readExtensionPackage(bytes);
  if (!contents.valid) throw new ExtensionPackFailure(contents.diagnostics);
  warnings.push(...contents.diagnostics);

  const output = options.output === undefined ? join(root, "dist", "extensions")
    : isAbsolute(options.output) ? options.output : resolve(options.output);
  await mkdir(output, {recursive: true});
  const file = join(output, extensionPackageFileName(metadata.id, manifest.version));
  const partial = join(output, `.${basename(file)}.${randomUUID()}.partial`);
  try {
    await writeFile(partial, bytes);
    await rm(file, {force: true});
    await rename(partial, file);
  } finally {
    await rm(partial, {force: true});
  }
  return {file, id: metadata.id, version: manifest.version, sha256: createHash("sha256").update(bytes).digest("hex"),
    size: bytes.length, files: [...contents.files.keys()], created, warnings};
}

/** A project-relative path for messages when the file is inside the project. */
export function displayPath(root: string, file: string): string {
  const path = relative(root, file);
  return path.startsWith("..") || isAbsolute(path) ? file : path.split(sep).join("/");
}
