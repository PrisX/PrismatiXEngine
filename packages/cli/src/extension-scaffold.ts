// `prismatix extension`: create a runtime extension (TypeScript by default),
// add commands/Actions to it, regenerate its typed declarations and list the
// project's extensions. PrismatiX Studio's New Extension dialog runs the same
// command, so the CLI and Studio scaffold identical files.

import {existsSync} from "node:fs";
import {mkdir, readFile, writeFile} from "node:fs/promises";
import {join, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";

import {
  ENGINE_VERSION,
  EXTENSION_TYPES_FILE,
  extensionTypeDeclarations,
  extensionUsesTypeScript,
  validateDocument,
  type ExtensionAction,
  type ExtensionCommand,
  type ExtensionManifest,
} from "@prismatix/authoring-sdk";

export const extensionHelp = [
  "Create and maintain runtime extensions: Story commands and UI Actions written in TypeScript or JavaScript.",
  "prismatix extension create <id> [--lang ts|js] [--command <id,...>] [--action <id,...>] [--dir <folder>]",
  "    [--project <dir>] [--no-register]",
  "prismatix extension add-command <extension> <id> [--await] [--project <dir>]",
  "prismatix extension add-action <extension> <id> [--project <dir>]",
  "prismatix extension types [extension] [--project <dir>]",
  "prismatix extension list [--project <dir>]",
  "",
  "create writes <dir>/<id>.pxextension and its entry script (default dir Content/Extensions/<id>) and",
  "registers the manifest in the project's `extensions`; it never overwrites a file. TypeScript (the",
  "default) also writes tsconfig.json and prismatix-extension.d.ts, which types each handler's arguments",
  "from the manifest. Builds bundle main.ts with its imports (\"@prismatix/runtime\", relative files,",
  "pure-JavaScript npm packages) into main.js and report TypeScript errors as PXCLIEXT2001 warnings.",
  "A command or Action id without a dot gets the extension's namespace (`--command greet` in `my-fx`",
  "declares myFx.greet). <extension> is an extension id or its manifest path. add-command/add-action",
  "declare one more id and append a handler stub to the entry; types rewrites prismatix-extension.d.ts",
  "after a hand edit of the manifest. --json prints {created, updated, manifest, entry, extension}.",
];

const identifier = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;
const safety = {previewSafe: true, deterministic: true, seekSafe: true, rollbackSafe: true} as const;

export interface ExtensionScaffoldResult {
  readonly created: readonly string[];
  readonly updated: readonly string[];
  readonly manifest: string;
  readonly entry: string;
  readonly extension: ExtensionManifest;
  readonly notes: readonly string[];
}

type Json = Record<string, unknown>;

function slash(path: string): string {
  return path.split(sep).join("/");
}

function safeFolder(value: string): boolean {
  return value.length > 0 && !value.startsWith("/") && !/^[A-Za-z]:/u.test(value) &&
    !value.includes("\\") && !value.split("/").some((part) => part === ".." || part === "." || part === "");
}

/** The namespace an extension's ids share: `my-effects` -> `myEffects`. Studio uses the same rule. */
export function extensionNamespace(id: string): string {
  const parts = id.split(/[^A-Za-z0-9]+/u).filter(Boolean);
  if (parts.length === 0) return "ext";
  const [first, ...rest] = parts;
  const name = [first!.toLowerCase(), ...rest.map((part) => part[0]!.toUpperCase() + part.slice(1).toLowerCase())].join("");
  return /^[0-9]/u.test(name) ? `x${name}` : name;
}

/** The range a new extension asks for: this minor line of the engine. */
function engineRange(version: string): string {
  const [major = 0, minor = 2] = version.split(".").map((part) => Number.parseInt(part, 10));
  return major === 0 ? `>=0.${minor}.0 <0.${minor + 1}.0` : `>=${major}.${minor}.0 <${major + 1}.0.0`;
}

function qualified(namespace: string, name: string): string {
  return name.includes(".") ? name : `${namespace}.${name}`;
}

function list(value: string | undefined): string[] {
  return (value ?? "").split(",").map((item) => item.trim()).filter(Boolean);
}

function exampleCommand(namespace: string): ExtensionCommand {
  return {id: `${namespace}.notify`, displayName: "Notify",
    description: "Logs a message and tells the extension's listeners; the Story waits one frame.",
    await: true, rollback: "reversible", capabilities: ["runtime"],
    parameters: [{name: "message", type: "string", required: true, editorHint: "multiline"}]};
}

function exampleAction(namespace: string): ExtensionAction {
  return {id: `${namespace}.openScreen`, displayName: "Open screen",
    description: "Opens a UI route as a modal; use it as a button's action.",
    reentry: "ignoreWhileRunning", capabilities: ["ui"],
    parameters: [{name: "route", type: "string", required: true, editorHint: "route"}]};
}

function plainCommand(id: string, awaits = false): ExtensionCommand {
  return {id, displayName: id.slice(id.lastIndexOf(".") + 1), await: awaits, rollback: "reversible",
    capabilities: ["runtime"], parameters: []};
}

function plainAction(id: string): ExtensionAction {
  return {id, displayName: id.slice(id.lastIndexOf(".") + 1), reentry: "ignoreWhileRunning",
    capabilities: ["ui"], parameters: []};
}

function argumentPattern(declaration: ExtensionCommand | ExtensionAction): string {
  const names = declaration.parameters.map((parameter) => parameter.name);
  return names.length === 0 ? "()" : `({${names.join(", ")}})`;
}

/** A handler skeleton for one declaration, in the entry's language. */
export function extensionHandlerStub(kind: "command" | "action", declaration: ExtensionCommand | ExtensionAction,
  typescript: boolean): string {
  const names = declaration.parameters.map((parameter) => parameter.name);
  const awaits = kind === "command" && (declaration as ExtensionCommand).await === true;
  const id = JSON.stringify(declaration.id);
  if (typescript) {
    const define = kind === "command" ? "defineCommand" : "defineAction";
    const parameters = kind === "action"
      ? `(${names.length === 0 ? "_args" : `{${names.join(", ")}}`}, context)` : argumentPattern(declaration);
    const body = declaration.id.endsWith(".notify") && names.includes("message")
      ? [`  ctx.log.info(${id}, message);`, `  await ctx.events.emit(${id}, {message});`, "  await ctx.time.wait(0);"]
      : kind === "action" && names.length === 1 && names[0] === "route"
        ? ["  if (!context.preview) ctx.ui.showModal(route);"]
        : [`  ctx.log.info(${[id, ...names, ...(kind === "action" ? ["context.route"] : [])].join(", ")});`,
          ...(awaits ? ["  await ctx.time.wait(0);"] : [])];
    return [`${define}(${id}, ${awaits ? "async " : ""}${parameters} => {`, ...body, "});", ""].join("\n");
  }
  const register = kind === "command" ? "Engine.RegisterCommand" : "Engine.RegisterAction";
  const parameters = kind === "action"
    ? `(${names.length === 0 ? "_args" : `{${names.join(", ")}}`}, context)` : argumentPattern(declaration);
  const body = declaration.id.endsWith(".notify") && names.includes("message")
    ? [`  Engine.log(${id}, message);`, `  await Engine.emit(${id}, {message});`, "  await Engine.WaitSeconds(0);"]
    : kind === "action" && names.length === 1 && names[0] === "route"
      ? ["  if (!context.preview) Engine.ShowModal(route);"]
      : [`  Engine.log(${[id, ...names, ...(kind === "action" ? ["context.route"] : [])].join(", ")});`,
        ...(awaits ? ["  await Engine.WaitSeconds(0);"] : [])];
  return [`${register}(${id}, ${awaits ? "async " : ""}${parameters} => {`, ...body, "});", ""].join("\n");
}

function entryTemplate(manifest: ExtensionManifest, manifestFile: string, typescript: boolean): string {
  const handlers = [
    ...manifest.commands.map((command) => extensionHandlerStub("command", command, typescript)),
    ...manifest.actions.map((action) => extensionHandlerStub("action", action, typescript)),
  ];
  const imports = [
    ...(handlers.length > 0 || typescript ? ["createPrismatiXContext"] : []),
    ...(manifest.actions.length > 0 ? ["defineAction"] : []),
    ...(manifest.commands.length > 0 ? ["defineCommand"] : []),
  ];
  if (typescript) return [
    `// ${manifest.id}: a PrismatiX runtime extension.`,
    "//",
    "// The build bundles this file and what it imports into one JavaScript module;",
    "// the game runs it in its sandboxed JavaScript host (QuickJS) in the native Player",
    "// and Preview alike. There is no Node.js or DOM there (files, network, timers),",
    "// Date is undefined and Math.random throws: wait with ctx.time and keep saved",
    "// state in ctx.state so saves and rollback stay exact.",
    `// Every command and Action declared in ${manifestFile} needs a handler below;`,
    `// ${EXTENSION_TYPES_FILE} types their arguments (\`prismatix extension types\``,
    "// refreshes it after you edit the manifest by hand).",
    `import {${imports.join(", ")}} from "@prismatix/runtime";`,
    "",
    "const ctx = createPrismatiXContext();",
    "",
    ...handlers,
  ].join("\n");
  return [
    `// ${manifest.id}: a PrismatiX runtime extension.`,
    "//",
    "// This file runs as is in the game's sandboxed JavaScript host (QuickJS), in the",
    "// native Player and Preview alike, through the `Engine` global. There is no",
    "// Node.js or DOM there; Date is undefined and Math.random throws.",
    `// Every command and Action declared in ${manifestFile} needs a handler below.`,
    "",
    ...handlers,
  ].join("\n");
}

const tsconfig = `${JSON.stringify({
  compilerOptions: {
    target: "ES2022", module: "ESNext", moduleResolution: "Bundler", lib: ["ES2023"],
    types: ["@prismatix/runtime/sandbox"], strict: true, noEmit: true, isolatedModules: true,
    allowImportingTsExtensions: true, skipLibCheck: true,
  },
  include: ["**/*.ts"],
}, null, 2)}\n`;

async function readObject(path: string): Promise<Json> {
  const text = await readFile(path, "utf8");
  const value = JSON.parse(text.startsWith("﻿") ? text.slice(1) : text) as unknown;
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error(`${path} must contain a JSON object`);
  return value as Json;
}

/** Adds one path to a JSON manifest's `extensions` array, keeping the rest of its text. */
function registerText(text: string, manifestPath: string): string {
  const value = JSON.parse(text) as Json;
  const registered = Array.isArray(value.extensions) ? value.extensions as unknown[] : undefined;
  if (registered?.includes(manifestPath)) return text;
  const quoted = JSON.stringify(manifestPath);
  const match = /"extensions"\s*:\s*\[([^\]]*)\]/u.exec(text);
  let patched: string | undefined;
  if (match !== null && registered !== undefined) {
    const inner = match[1]!;
    const replacement = inner.trim().length === 0 ? `"extensions": [${quoted}]`
      : `"extensions": [${inner.replace(/\s*$/u, "")}, ${quoted}${/\n\s*$/u.test(inner) ? inner.slice(inner.search(/\n\s*$/u)) : ""}]`;
    patched = text.slice(0, match.index) + replacement + text.slice(match.index + match[0].length);
  } else if (registered === undefined) {
    const close = text.lastIndexOf("}");
    const body = text.slice(0, close).replace(/\s*$/u, "");
    patched = `${body}${body.endsWith("{") ? "" : ","}\n  "extensions": [${quoted}]\n}${text.slice(close + 1)}`;
  }
  try {
    const parsed = patched === undefined ? undefined : JSON.parse(patched) as Json;
    if (Array.isArray(parsed?.extensions) && (parsed.extensions as unknown[]).includes(manifestPath)) return patched!;
  } catch { /* fall back to a full rewrite below */ }
  value.extensions = [...registered ?? [], manifestPath];
  return `${JSON.stringify(value, null, 2)}\n`;
}

async function projectManifest(root: string): Promise<string> {
  for (const name of ["prismatix.json", "project.pxproject"])
    if (existsSync(join(root, name))) return name;
  throw new Error(`${root} has no prismatix.json or project.pxproject`);
}

/** The project's own @prismatix/runtime, pinned like its CLI, so editors resolve the import. */
async function pinRuntime(root: string): Promise<string | undefined> {
  const path = join(root, "package.json");
  if (!existsSync(path)) return undefined;
  const text = await readFile(path, "utf8");
  const value = JSON.parse(text) as Json;
  const sections = ["dependencies", "devDependencies"].map((name) => value[name] as Json | undefined);
  if (sections.some((section) => section?.["@prismatix/runtime"] !== undefined)) return undefined;
  const cli = sections.map((section) => section?.["@prismatix/cli"]).find((spec) => typeof spec === "string") as string | undefined;
  const runtimeRoot = fileURLToPath(new URL("../../../../runtime-sdk/", import.meta.url));
  const own = JSON.parse(await readFile(fileURLToPath(new URL("../../package.json", import.meta.url)), "utf8")) as {version: string};
  const linked = cli?.match(/^(file|link):/u)?.[1];
  const spec = linked !== undefined && existsSync(join(runtimeRoot, "package.json"))
    ? `${linked}:${slash(resolve(runtimeRoot)).replace(/\/$/u, "")}` : own.version;
  const devDependencies = {...(value.devDependencies as Json | undefined) ?? {}, "@prismatix/runtime": spec};
  value.devDependencies = Object.fromEntries(Object.entries(devDependencies).sort(([left], [right]) => left.localeCompare(right)));
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  return "package.json";
}

interface LocatedExtension {
  readonly manifestPath: string;
  readonly manifest: ExtensionManifest;
}

async function projectExtensions(root: string): Promise<LocatedExtension[]> {
  const project = await readObject(join(root, await projectManifest(root)));
  const located: LocatedExtension[] = [];
  for (const path of Array.isArray(project.extensions) ? project.extensions : []) {
    if (typeof path !== "string" || !safeFolder(path)) continue;
    try { located.push({manifestPath: path, manifest: await readObject(join(root, ...path.split("/"))) as unknown as ExtensionManifest}); }
    catch { /* validate reports unreadable manifests */ }
  }
  return located;
}

async function findExtension(root: string, reference: string): Promise<LocatedExtension> {
  const all = await projectExtensions(root);
  const found = all.find((item) => item.manifestPath === reference || item.manifest.id === reference);
  if (found === undefined)
    throw new Error(`No registered extension is ${reference}; registered: ${all.map((item) => item.manifest.id).join(", ") || "none"}`);
  return found;
}

function directoryOf(manifestPath: string): string {
  const index = manifestPath.lastIndexOf("/");
  return index < 0 ? "" : manifestPath.slice(0, index);
}

function within(directory: string, path: string): string {
  return directory === "" ? path : `${directory}/${path}`;
}

async function writeTypes(root: string, located: LocatedExtension): Promise<string | undefined> {
  if (!extensionUsesTypeScript(located.manifest)) return undefined;
  const directory = directoryOf(located.manifestPath);
  const path = within(directory, EXTENSION_TYPES_FILE);
  const text = extensionTypeDeclarations(located.manifest, located.manifestPath.slice(directory.length === 0 ? 0 : directory.length + 1));
  const absolute = join(root, ...path.split("/"));
  if (existsSync(absolute) && (await readFile(absolute, "utf8")).replaceAll("\r\n", "\n") === text) return undefined;
  await writeFile(absolute, text, "utf8");
  return path;
}

function checked(manifest: ExtensionManifest, path: string): void {
  const result = validateDocument<ExtensionManifest>("extension", manifest, path);
  const errors = result.diagnostics.filter((item) => item.severity === "error");
  if (errors.length > 0)
    throw new Error(`The manifest would be invalid: ${errors.map((item) => `${item.code} ${item.message}${item.details ? ` (${item.details})` : ""}`).join("; ")}`);
}

export async function createExtension(root: string, id: string, options: {
  readonly language?: string; readonly commands?: readonly string[]; readonly actions?: readonly string[];
  readonly directory?: string; readonly register?: boolean;
} = {}): Promise<ExtensionScaffoldResult> {
  if (!identifier.test(id) || id.length > 256)
    throw new Error(`Extension id must start with a letter or digit and use only letters, digits, ".", "_" and "-": ${id}`);
  const language = (options.language ?? "ts").toLowerCase();
  if (!["ts", "typescript", "js", "javascript"].includes(language))
    throw new Error(`--lang must be ts or js, not ${options.language}`);
  const typescript = language.startsWith("t");
  const directory = (options.directory ?? `Content/Extensions/${id}`).replace(/\\/gu, "/").replace(/\/+$/u, "");
  if (!safeFolder(directory)) throw new Error(`--dir must be a folder inside the project: ${options.directory}`);
  const namespace = extensionNamespace(id);
  const commandIds = (options.commands ?? []).map((name) => qualified(namespace, name));
  const actionIds = (options.actions ?? []).map((name) => qualified(namespace, name));
  const explicit = commandIds.length > 0 || actionIds.length > 0;
  const commands = explicit ? commandIds.map((value) => plainCommand(value)) : [exampleCommand(namespace)];
  const actions = explicit ? actionIds.map(plainAction) : [exampleAction(namespace)];
  const entry = typescript ? "main.ts" : "main.js";
  const manifest: ExtensionManifest = {
    format: "PrismatiXExtension", schemaRevision: 1, language: typescript ? "typescript" : "javascript",
    id, version: "1.0.0", requiredEngineVersion: engineRange(ENGINE_VERSION), entry,
    capabilities: actions.length > 0 ? ["runtime", "ui"] : ["runtime"],
    safety: {...safety}, commands, actions,
  };
  const manifestFile = `${id}.pxextension`;
  const manifestPath = within(directory, manifestFile);
  checked(manifest, manifestPath);
  const projectFile = await projectManifest(root);
  const project = await readObject(join(root, projectFile));
  const registered = Array.isArray(project.extensions) ? project.extensions as unknown[] : [];
  const others = await projectExtensions(root);
  const taken = others.find((item) => item.manifest.id === id);
  if (taken !== undefined) throw new Error(`Extension ${id} already exists: ${taken.manifestPath}`);
  for (const declaration of [...commands, ...actions]) {
    const owner = others.find((item) => [...item.manifest.commands ?? [], ...item.manifest.actions ?? []]
      .some((other) => other.id === declaration.id));
    if (owner !== undefined) throw new Error(`${declaration.id} is already declared by ${owner.manifestPath}`);
  }
  if (typescript && others.some((item) => extensionUsesTypeScript(item.manifest) &&
      directoryOf(item.manifestPath) === directory))
    throw new Error(`${directory} already holds a TypeScript extension; give each TypeScript extension its own folder (--dir)`);
  const files: [string, string][] = [
    [manifestPath, `${JSON.stringify(manifest, null, 2)}\n`],
    [within(directory, entry), entryTemplate(manifest, manifestFile, typescript)],
    ...(typescript ? [
      [within(directory, "tsconfig.json"), tsconfig],
      [within(directory, EXTENSION_TYPES_FILE), extensionTypeDeclarations(manifest, manifestFile)],
    ] as [string, string][] : []),
  ];
  for (const [path] of files)
    if (existsSync(join(root, ...path.split("/")))) throw new Error(`${path} already exists; nothing was written`);
  await mkdir(join(root, ...directory.split("/")), {recursive: true});
  for (const [path, text] of files) await writeFile(join(root, ...path.split("/")), text, {encoding: "utf8", flag: "wx"});
  const updated: string[] = [];
  const notes: string[] = [];
  if (options.register !== false && !registered.includes(manifestPath)) {
    const text = await readFile(join(root, projectFile), "utf8");
    await writeFile(join(root, projectFile), registerText(text, manifestPath), "utf8");
    updated.push(projectFile);
  } else if (options.register === false)
    notes.push(`Not registered: add "${manifestPath}" to ${projectFile} "extensions" to load it.`);
  if (typescript) {
    const pinned = await pinRuntime(root);
    if (pinned !== undefined) {
      updated.push(pinned);
      notes.push("Added @prismatix/runtime to package.json: install packages so editors resolve its types (builds do not need it).");
    }
  }
  return {created: files.map(([path]) => path), updated, manifest: manifestPath,
    entry: within(directory, entry), extension: manifest, notes};
}

export async function addDeclaration(root: string, reference: string, kind: "command" | "action",
  name: string, options: {readonly await?: boolean} = {}): Promise<ExtensionScaffoldResult> {
  const located = await findExtension(root, reference);
  const manifest = located.manifest;
  const id = qualified(extensionNamespace(manifest.id), name);
  const others = await projectExtensions(root);
  const owner = others.find((item) => [...item.manifest.commands ?? [], ...item.manifest.actions ?? []]
    .some((other) => other.id === id));
  if (owner !== undefined) throw new Error(`${id} is already declared by ${owner.manifestPath}`);
  const declaration = kind === "command" ? plainCommand(id, options.await === true) : plainAction(id);
  const capabilities = kind === "action" && !manifest.capabilities.includes("ui")
    ? [...manifest.capabilities, "ui" as const] : manifest.capabilities;
  const next: ExtensionManifest = {...manifest, capabilities,
    ...(kind === "command" ? {commands: [...manifest.commands, declaration as ExtensionCommand]}
      : {actions: [...manifest.actions, declaration as ExtensionAction]})};
  checked(next, located.manifestPath);
  const directory = directoryOf(located.manifestPath);
  const entry = within(directory, manifest.entry);
  const entryPath = join(root, ...entry.split("/"));
  const typescript = /\.ts$/iu.test(manifest.entry);
  const script = existsSync(entryPath) ? await readFile(entryPath, "utf8") : undefined;
  await writeFile(join(root, ...located.manifestPath.split("/")), `${JSON.stringify(next, null, 2)}\n`, "utf8");
  const updated = [located.manifestPath];
  if (script !== undefined) {
    let stub = extensionHandlerStub(kind, declaration, typescript);
    let text = script;
    const define = kind === "command" ? "defineCommand" : "defineAction";
    // A TypeScript entry needs the define function the stub calls.
    if (typescript && !new RegExp(`\\b${define}\\b`, "u").test(script)) {
      const imported = /import\s*\{([^}]*)\}\s*from\s*["']@prismatix\/runtime["'];?/u.exec(script);
      if (imported !== null)
        text = script.replace(imported[0], `import {${[...imported[1]!.split(",").map((item) => item.trim()).filter(Boolean), define].sort().join(", ")}} from "@prismatix/runtime";`);
      else stub = `import {${define}} from "@prismatix/runtime";\n${stub}`;
    }
    if (typescript && /\bctx\./u.test(stub) && !/\bconst ctx\b/u.test(text))
      stub = stub.replaceAll("ctx.log.info", "console.log").replaceAll("await ctx.time.wait(0)", "await Engine.WaitSeconds(0)");
    await writeFile(entryPath, `${text.replace(/\s*$/u, "")}\n\n${stub}`, "utf8");
    updated.push(entry);
  }
  const types = await writeTypes(root, {manifestPath: located.manifestPath, manifest: next});
  if (types !== undefined) updated.push(types);
  return {created: [], updated, manifest: located.manifestPath, entry, extension: next, notes: []};
}

export async function writeExtensionTypes(root: string, reference?: string): Promise<{updated: string[]; extensions: string[]}> {
  const all = reference === undefined ? await projectExtensions(root) : [await findExtension(root, reference)];
  const typed = all.filter((item) => extensionUsesTypeScript(item.manifest));
  if (reference !== undefined && typed.length === 0) throw new Error(`${reference} is not a TypeScript extension`);
  const updated: string[] = [];
  for (const item of typed) {
    const path = await writeTypes(root, item);
    if (path !== undefined) updated.push(path);
  }
  return {updated, extensions: typed.map((item) => item.manifestPath)};
}

interface CommandOptions {
  readonly values: ReadonlyMap<string, string>;
  readonly flags: ReadonlySet<string>;
  readonly positional: readonly string[];
}

/** Runs `prismatix extension <subcommand>` in `root`; returns what it printed. */
export async function extensionCommand(root: string, options: CommandOptions, json: boolean): Promise<string> {
  const [subcommand, first, second] = options.positional;
  if (subcommand === "create") {
    const result = await createExtension(root, first!, {
      ...(options.values.has("lang") ? {language: options.values.get("lang")!} : {}),
      commands: list(options.values.get("command")), actions: list(options.values.get("action")),
      ...(options.values.has("dir") ? {directory: options.values.get("dir")!} : {}),
      register: !options.flags.has("no-register")});
    return json ? `${JSON.stringify(result)}\n` : [...result.created.map((path) => `Created ${path}`),
      ...result.updated.map((path) => `Updated ${path}`), ...result.notes, "",
      `Use a command in Story: [${result.extension.commands[0]?.id ?? "<command>"}${result.extension.commands[0]?.parameters
        .filter((parameter) => parameter.required).map((parameter) => ` ${parameter.name}="..."`).join("") ?? ""}]`,
      ...(result.extension.actions[0] === undefined ? [] :
        [`Give a button the Action: {"id":"${result.extension.actions[0].id}","arguments":{${result.extension.actions[0].parameters
          .filter((parameter) => parameter.required).map((parameter) => `"${parameter.name}":"..."`).join(",")}}}`]), ""].join("\n");
  }
  if (subcommand === "add-command" || subcommand === "add-action") {
    const result = await addDeclaration(root, first!, subcommand === "add-command" ? "command" : "action", second!,
      {await: options.flags.has("await")});
    return json ? `${JSON.stringify(result)}\n` : [...result.updated.map((path) => `Updated ${path}`), ""].join("\n");
  }
  if (subcommand === "types") {
    const result = await writeExtensionTypes(root, first);
    return json ? `${JSON.stringify(result)}\n` : result.updated.length === 0
      ? `Up to date: ${result.extensions.join(", ") || "no TypeScript extensions"}\n`
      : `${result.updated.map((path) => `Updated ${path}`).join("\n")}\n`;
  }
  if (subcommand === "list") {
    const all = await projectExtensions(root);
    if (json) return `${JSON.stringify({extensions: all.map((item) => ({manifest: item.manifestPath, id: item.manifest.id,
      language: extensionUsesTypeScript(item.manifest) ? "typescript" : "javascript", entry: item.manifest.entry,
      commands: (item.manifest.commands ?? []).map((command) => command.id),
      actions: (item.manifest.actions ?? []).map((action) => action.id)}))})}\n`;
    if (all.length === 0) return "No extensions are registered.\n";
    return `${all.map((item) => [`${item.manifest.id} (${extensionUsesTypeScript(item.manifest) ? "TypeScript" : "JavaScript"}) ${item.manifestPath}`,
      ...(item.manifest.commands ?? []).map((command) => `  command ${command.id}`),
      ...(item.manifest.actions ?? []).map((action) => `  action  ${action.id}`)].join("\n")).join("\n")}\n`;
  }
  throw new Error(`Unknown extension subcommand: ${subcommand ?? "(none)"}`);
}

