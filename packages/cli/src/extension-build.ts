// TypeScript extensions: bundle a `.ts` entry and everything it imports into
// the one JavaScript module the Player's QuickJS host loads, and type-check it
// against `@prismatix/runtime`, the sandbox globals and declarations generated
// from the extension's own manifest.
//
// Every command that builds a project (validate, build, pack, run --dev, the
// web and Android exports) goes through `buildTypeScriptExtensions`, so the
// Player never sees TypeScript. Results are cached under
// .prismatix/cache/extensions by the content hash of every input.

import {createHash} from "node:crypto";
import {existsSync, readFileSync} from "node:fs";
import {mkdir, readFile, rename, writeFile} from "node:fs/promises";
import {builtinModules} from "node:module";
import {dirname, isAbsolute, join, relative, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";

import type {Message, Plugin} from "esbuild";
import type * as TS from "typescript-compiler";
import {ts} from "./typescript.js";

import {
  canonicalJson,
  EXTENSION_TYPES_FILE,
  extensionBundleEntry,
  extensionTypeDeclarations,
  extensionUsesTypeScript,
  type AuthoringDiagnostic,
  type ExtensionManifest,
  type SourceSpan,
} from "@prismatix/authoring-sdk";

export interface ExtensionBundle {
  /** JavaScript entry relative to the manifest. */
  readonly entry: string;
  readonly code: string;
}

export interface ExtensionBuildResult {
  /** Bundles keyed by manifest path, for `compileProject({extensionBuilds})`. */
  readonly builds: Readonly<Record<string, ExtensionBundle>>;
  /** Errors stop the build; type and sandbox findings are warnings. */
  readonly diagnostics: readonly AuthoringDiagnostic[];
  /** Project-relative source text each bundle was made from. */
  readonly sources: ReadonlyMap<string, string>;
}

const cacheFormat = "PrismatiXExtensionBuild/1";
const runtimeNamespace = "prismatix-runtime";

// esbuild and the runtime facade load only when a project has a TypeScript
// extension, so every other command starts as before.
let esbuildModule: typeof import("esbuild") | undefined;
async function esbuild(): Promise<typeof import("esbuild")> {
  esbuildModule ??= await import("esbuild");
  return esbuildModule;
}

let runtimeFiles: {entry: string; declarations: string; sandbox: string} | undefined;
function runtime(): {entry: string; declarations: string; sandbox: string} {
  if (runtimeFiles === undefined) {
    const entry = fileURLToPath(import.meta.resolve("@prismatix/runtime"));
    runtimeFiles = {entry, declarations: entry.replace(/\.js$/u, ".d.ts"),
      sandbox: join(dirname(entry), "..", "..", "sandbox.d.ts")};
  }
  return runtimeFiles;
}

function sha256(value: string | Buffer): string {
  return createHash("sha256").update(value).digest("hex");
}

let toolIdentity: string | undefined;
/** Changes whenever a tool or library that shapes the output changes. */
async function currentToolIdentity(): Promise<string> {
  const files = runtime();
  toolIdentity ??= sha256([cacheFormat, (await esbuild()).version, ts.version,
    sha256(readFileSync(files.entry)), sha256(readFileSync(files.declarations)),
    sha256(readFileSync(files.sandbox)),
    sha256(readFileSync(fileURLToPath(import.meta.url)))].join("\0"));
  return toolIdentity;
}

function slash(path: string): string {
  return path.split(sep).join("/");
}

function within(root: string, path: string): string | undefined {
  const value = relative(root, path);
  return value === "" || value.startsWith("..") || isAbsolute(value) ? undefined : slash(value);
}

function manifestDirectory(manifestPath: string): string {
  const index = manifestPath.lastIndexOf("/");
  return index < 0 ? "" : manifestPath.slice(0, index);
}

function joinVirtual(directory: string, path: string): string {
  return directory === "" ? path : `${directory}/${path}`;
}

function position(text: string, offset: number) {
  let line = 1; let column = 1;
  for (let index = 0; index < offset && index < text.length; index += 1) {
    if (text[index] === "\n") { line += 1; column = 1; } else column += 1;
  }
  return {line, column, offset};
}

/** A span from a UTF-16 offset into the file's full text. */
function span(text: string, path: string, start: number, length: number): SourceSpan {
  const bounded = Math.max(0, Math.min(text.length, start));
  return {path, start: position(text, bounded),
    end: position(text, Math.min(text.length, bounded + Math.max(1, length)))};
}

/** A span from a 1-based line and 0-based column (esbuild locations). */
function lineSpan(text: string, path: string, line: number, column: number, length: number): SourceSpan {
  const lines = text.split("\n");
  let offset = 0;
  for (let index = 0; index < line - 1 && index < lines.length; index += 1) offset += lines[index]!.length + 1;
  return span(text, path, offset + column, length);
}

function messageDiagnostic(root: string, message: Message, code: string,
  severity: AuthoringDiagnostic["severity"], fallbackPath: string): AuthoringDiagnostic {
  const location = message.location;
  const path = location === null ? fallbackPath
    : within(root, resolve(root, location.file)) ?? location.file;
  let text: string | undefined;
  try { text = location === null ? undefined : readFileSync(resolve(root, location.file), "utf8"); }
  catch { text = undefined; }
  return {severity, code, message: message.text, path,
    ...(location === null || text === undefined ? {} : {
      span: lineSpan(text, path, location.line, location.column, location.length)}),
    ...(location === null ? {} : {details: `${path}:${location.line}:${location.column + 1}: ${location.lineText.trim()}`}),
  } as AuthoringDiagnostic;
}

const nodeBuiltins = new Set([...builtinModules, ...builtinModules.map((name) => `node:${name}`)]);

function sandboxPlugin(runtimeSource: string): Plugin {
  return {
    name: "prismatix-sandbox",
    // esbuild filters are Go regular expressions: no JavaScript flags.
    setup(context) {
      context.onResolve({filter: new RegExp("^@prismatix/runtime$")}, () =>
        ({path: "@prismatix/runtime", namespace: runtimeNamespace}));
      context.onLoad({filter: new RegExp(".*"), namespace: runtimeNamespace}, () =>
        ({contents: runtimeSource, loader: "js"}));
      context.onResolve({filter: new RegExp("^@prismatix/(?:runtime/sandbox|authoring-sdk|cli)(?:/.*)?$")}, (args) => ({
        errors: [{text: `"${args.path}" is build-time tooling; the Player cannot load it. ` +
          "Import \"@prismatix/runtime\" in extension code."}]}));
      context.onResolve({filter: new RegExp("^[^./]")}, (args) => nodeBuiltins.has(args.path) ? {
        errors: [{text: `"${args.path}" is a Node.js module. Extensions run in the Player's sandbox, ` +
          "which has no filesystem, network, process or timer modules."}]} : undefined);
    },
  };
}

interface CacheEntry {
  readonly format: string;
  readonly key: string;
  readonly inputs: readonly {readonly path: string; readonly hash: string}[];
  readonly entry: string;
  readonly code: string;
  readonly map: string;
  readonly diagnostics: readonly AuthoringDiagnostic[];
}

function cacheFile(root: string, manifestPath: string): string {
  return join(root, ".prismatix", "cache", "extensions", `${sha256(manifestPath).slice(0, 24)}.json`);
}

async function readCache(root: string, manifestPath: string, key: string,
  sources: Map<string, string>): Promise<CacheEntry | undefined> {
  let entry: CacheEntry;
  try { entry = JSON.parse(await readFile(cacheFile(root, manifestPath), "utf8")) as CacheEntry; }
  catch { return undefined; }
  if (entry.format !== cacheFormat || entry.key !== key || !Array.isArray(entry.inputs)) return undefined;
  const texts = new Map<string, string>();
  for (const input of entry.inputs) {
    let text: string;
    try { text = await readFile(join(root, ...input.path.split("/")), "utf8"); }
    catch { return undefined; }
    if (sha256(text) !== input.hash) return undefined;
    texts.set(input.path, text);
  }
  for (const [path, text] of texts) sources.set(path, text);
  return entry;
}

async function writeCache(root: string, manifestPath: string, entry: CacheEntry): Promise<void> {
  const path = cacheFile(root, manifestPath);
  try {
    await mkdir(dirname(path), {recursive: true});
    const temporary = `${path}.${process.pid}.tmp`;
    await writeFile(temporary, JSON.stringify(entry), "utf8");
    await rename(temporary, path);
  } catch { /* A cache that cannot be written only costs the next build time. */ }
}

/** Globals the sandbox removes; using them fails at run time, not at type-check time. */
function sandboxFindings(program: TS.Program, root: string, files: readonly TS.SourceFile[]): AuthoringDiagnostic[] {
  const checker = program.getTypeChecker();
  const findings: AuthoringDiagnostic[] = [];
  const fromLibrary = (node: TS.Node): boolean => {
    const symbol = checker.getSymbolAtLocation(node);
    return symbol?.declarations?.some((declaration) =>
      program.isSourceFileDefaultLibrary(declaration.getSourceFile())) === true;
  };
  for (const source of files) {
    const path = within(root, resolve(source.fileName)) ?? source.fileName;
    const report = (node: TS.Node, what: string, instead: string): void => {
      findings.push({severity: "warning", code: "PXCLIEXT2002",
        message: `${what} is not available in the Player's extension sandbox`, path,
        span: span(source.text, path, node.getStart(source), node.getWidth(source)),
        hint: instead} as AuthoringDiagnostic);
    };
    const visit = (node: TS.Node): void => {
      if (ts.isIdentifier(node) && ["Date", "eval", "Function"].includes(node.text) &&
          !(ts.isPropertyAccessExpression(node.parent) && node.parent.name === node) &&
          !ts.isTypeReferenceNode(node.parent) && fromLibrary(node))
        report(node, node.text, node.text === "Date"
          ? "Date is undefined there: keep time in Engine waits and saved state."
          : "Dynamic code evaluation is disabled.");
      else if (ts.isPropertyAccessExpression(node) && node.name.text === "random" &&
          ts.isIdentifier(node.expression) && node.expression.text === "Math" && fromLibrary(node.expression))
        report(node, "Math.random", "It throws: randomness must be engine-seeded so saves and rollback replay.");
      ts.forEachChild(node, visit);
    };
    visit(source);
  }
  return findings;
}

// The capability each guarded host call needs (JavaScriptHost.cpp
// CapabilityForOperation, RegisterAction and RegisterStateProvider). Calls not
// listed need "runtime".
const contextCapabilities: Readonly<Record<string, string>> = {
  ui: "ui", audio: "audio", video: "video", animation: "animation", saves: "persistence", state: "persistence",
};
const contextMemberCapabilities: Readonly<Record<string, string>> = {
  "renderer.upsert": "render", "renderer.remove": "render", "renderer.clear": "render",
  "input.actionPressed": "input", "input.actionDown": "input",
};
export function engineCapability(name: string): string {
  if (/^(?:PushRoute|ReplaceRoute|BackRoute|ShowModal|CloseModal|SetRouteTransition|RegisterAction)$/u.test(name)) return "ui";
  if (/^(?:PlaySE|PlayVoice|StopVoice|PlayBGM|StopBGM|PlayAmbience|StopAmbience|AwaitAudio|WaitAudio)$/u.test(name) ||
      /^Set(?:BGM|SE|Voice|Ambience)Volume$/u.test(name) || /Audio(?:Status|Playback)?$/u.test(name)) return "audio";
  if (/^(?:LoadAnimation|PlayAnimation|CancelAnimation|PauseAnimation|ResumeAnimation|SeekAnimation|FinishAnimation|GetAnimationStatus|GetAnimationError)$/u.test(name)) return "animation";
  if (/Video$|^GetVideo|^AwaitVideo$|^WaitVideo$/u.test(name)) return "video";
  if (/^(?:Save|Load|Autosave|DeleteSave|QuerySave|ListSaves|RegisterStateProvider)$/u.test(name)) return "persistence";
  if (/^(?:UpsertRenderNode|RemoveRenderNode|ClearRenderNodes)$/u.test(name)) return "render";
  if (/^IsInputAction(?:Pressed|Down)$/u.test(name)) return "input";
  return "runtime";
}

/** Host calls whose capability the manifest does not declare: the Player refuses them. */
function capabilityFindings(program: TS.Program, root: string, files: readonly TS.SourceFile[],
  capabilities: ReadonlySet<string>, manifestName: string): AuthoringDiagnostic[] {
  const checker = program.getTypeChecker();
  const findings: AuthoringDiagnostic[] = [];
  const typeName = (node: TS.Node): string | undefined => {
    const type = checker.getTypeAtLocation(node);
    return (type.aliasSymbol ?? type.getSymbol())?.getName();
  };
  for (const source of files) {
    const path = within(root, resolve(source.fileName)) ?? source.fileName;
    const report = (node: TS.Node, what: string, capability: string): void => {
      findings.push({severity: "warning", code: "PXCLIEXT2004",
        message: `${what} needs the "${capability}" capability, which ${manifestName} does not declare`, path,
        span: span(source.text, path, node.getStart(source), node.getWidth(source)),
        hint: `Add "${capability}" to "capabilities" in ${manifestName}; the Player refuses the call otherwise.`} as AuthoringDiagnostic);
    };
    const visit = (node: TS.Node): void => {
      if (ts.isPropertyAccessExpression(node)) {
        const owner = typeName(node.expression);
        const name = node.name.text;
        let capability: string | undefined;
        let what = node.getText(source);
        if (owner === "PrismatiXContext") capability = contextCapabilities[name];
        else if (owner === "PrismatiXEngine") capability = engineCapability(name);
        else if (ts.isPropertyAccessExpression(node.expression) &&
            typeName(node.expression.expression) === "PrismatiXContext") {
          capability = contextMemberCapabilities[`${node.expression.name.text}.${name}`];
          what = node.getText(source);
        }
        if (capability !== undefined && !capabilities.has(capability)) report(node, what, capability);
      } else if (ts.isCallExpression(node) && ts.isIdentifier(node.expression) &&
          ["defineAction", "defineCommand", "defineScreenEffect", "defineScreenTransition"].includes(node.expression.text)) {
        const symbol = checker.getSymbolAtLocation(node.expression);
        const declaration = symbol === undefined ? undefined
          : (symbol.flags & ts.SymbolFlags.Alias) !== 0 ? checker.getAliasedSymbol(symbol).declarations?.[0]
            : symbol.declarations?.[0];
        const fromRuntime = declaration !== undefined && resolve(declaration.getSourceFile().fileName) === resolve(runtime().declarations);
        const capability = node.expression.text === "defineAction" ? "ui" : "runtime";
        if (fromRuntime && !capabilities.has(capability)) report(node.expression, node.expression.text, capability);
      }
      ts.forEachChild(node, visit);
    };
    visit(source);
  }
  return findings;
}

function typeCheck(root: string, entryPath: string, declarationsPath: string,
  declarations: string, capabilities: ReadonlySet<string>, manifestName: string): AuthoringDiagnostic[] {
  const options: TS.CompilerOptions = {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    lib: ["lib.es2023.d.ts"],
    types: [],
    strict: true,
    noEmit: true,
    skipLibCheck: true,
    isolatedModules: true,
    allowImportingTsExtensions: true,
    resolveJsonModule: true,
  };
  const host = ts.createCompilerHost(options);
  const declarationsKey = resolve(declarationsPath);
  const getSourceFile = host.getSourceFile.bind(host);
  host.getSourceFile = (fileName, language, onError, create) =>
    resolve(fileName) === declarationsKey
      ? ts.createSourceFile(fileName, declarations, language, true)
      : getSourceFile(fileName, language, onError, create);
  const fileExists = host.fileExists.bind(host);
  host.fileExists = (fileName) => resolve(fileName) === declarationsKey || fileExists(fileName);
  const readFileText = host.readFile.bind(host);
  host.readFile = (fileName) => resolve(fileName) === declarationsKey ? declarations : readFileText(fileName);
  const cache = ts.createModuleResolutionCache(root,
    (value) => ts.sys.useCaseSensitiveFileNames ? value : value.toLowerCase(), options);
  host.resolveModuleNames = (names, containingFile) => names.map((name) => name === "@prismatix/runtime"
    ? {resolvedFileName: runtime().declarations, extension: ts.Extension.Dts, isExternalLibraryImport: true}
    : ts.resolveModuleName(name, containingFile, options, host, cache).resolvedModule);
  const program = ts.createProgram({rootNames: [entryPath, declarationsPath, runtime().sandbox], options, host});
  const authored = program.getSourceFiles().filter((source) => !source.isDeclarationFile &&
    within(root, resolve(source.fileName)) !== undefined &&
    !within(root, resolve(source.fileName))!.split("/").includes("node_modules"));
  const diagnostics: AuthoringDiagnostic[] = [];
  for (const value of ts.getPreEmitDiagnostics(program)) {
    if (value.category !== ts.DiagnosticCategory.Error) continue;
    const source = value.file;
    const path = source === undefined ? undefined : within(root, resolve(source.fileName));
    // Declaration files of npm packages are not the author's to fix.
    if (source !== undefined && (path === undefined || path.split("/").includes("node_modules"))) continue;
    const message = ts.flattenDiagnosticMessageText(value.messageText, "\n");
    diagnostics.push({severity: "warning", code: "PXCLIEXT2001",
      message: `TypeScript: ${message.split("\n")[0]}`,
      ...(path === undefined ? {} : {path}),
      ...(source === undefined || path === undefined || value.start === undefined ? {} : {
        span: span(source.text, path, value.start, value.length ?? 1)}),
      details: `TS${value.code}: ${message}`} as AuthoringDiagnostic);
  }
  diagnostics.push(...sandboxFindings(program, root, authored),
    ...capabilityFindings(program, root, authored, capabilities, manifestName));
  return diagnostics;
}

/**
 * Bundles and checks every TypeScript extension of a loaded project.
 * `manifests` maps manifest paths to their parsed documents.
 */
export async function buildTypeScriptExtensions(root: string,
  manifests: Readonly<Record<string, unknown>>,
  options: {readonly cache?: boolean} = {}): Promise<ExtensionBuildResult> {
  const builds: Record<string, ExtensionBundle> = {};
  const diagnostics: AuthoringDiagnostic[] = [];
  const sources = new Map<string, string>();
  for (const [manifestPath, value] of Object.entries(manifests)) {
    if (value === null || typeof value !== "object" || Array.isArray(value)) continue;
    const manifest = value as ExtensionManifest;
    if (typeof manifest.entry !== "string" || !extensionUsesTypeScript(manifest) ||
        !/\.ts$/iu.test(manifest.entry) || !Array.isArray(manifest.commands) ||
        !Array.isArray(manifest.actions) || !Array.isArray(manifest.capabilities)) continue;
    const directory = manifestDirectory(manifestPath);
    const entryVirtual = joinVirtual(directory, manifest.entry);
    const entryPath = resolve(root, ...entryVirtual.split("/"));
    if (within(root, entryPath) === undefined) continue; // The SDK reports the unsafe path.
    const bundleEntry = extensionBundleEntry(manifest.entry);
    const manifestName = manifestPath.slice(directory.length === 0 ? 0 : directory.length + 1);
    const declarations = extensionTypeDeclarations(manifest, manifestName);
    const declarationsVirtual = joinVirtual(directory, EXTENSION_TYPES_FILE);
    const declarationsPath = resolve(root, ...declarationsVirtual.split("/"));
    const key = sha256([await currentToolIdentity(), manifestPath, canonicalJson(manifest)].join("\0"));
    if (existsSync(declarationsPath) && readFileSync(declarationsPath, "utf8").replaceAll("\r\n", "\n") !== declarations)
      diagnostics.push({severity: "warning", code: "PXCLIEXT2003",
        message: `${EXTENSION_TYPES_FILE} does not match ${manifestName}; editors show stale types`,
        path: declarationsVirtual,
        hint: "Run `prismatix extension types` (validate and build check against fresh declarations)."});

    const cached = options.cache === false ? undefined : await readCache(root, manifestPath, key, sources);
    if (cached !== undefined) {
      builds[manifestPath] = {entry: cached.entry, code: cached.code};
      diagnostics.push(...cached.diagnostics);
      continue;
    }

    const runtimeSource = readFileSync(runtime().entry, "utf8").replace(/\n\/\/# sourceMappingURL=.*\s*$/u, "\n");
    const outfile = resolve(root, ...joinVirtual(directory, bundleEntry).split("/"));
    let result;
    try {
      result = await (await esbuild()).build({
        entryPoints: [entryPath], bundle: true, write: false, metafile: true,
        format: "esm", platform: "neutral", target: "es2022",
        absWorkingDir: root, outfile, sourcemap: "external", sourcesContent: true,
        mainFields: ["module", "main"], conditions: ["import", "module", "default"],
        resolveExtensions: [".ts", ".mjs", ".js", ".json"],
        charset: "utf8", legalComments: "eof", logLevel: "silent",
        plugins: [sandboxPlugin(runtimeSource)],
      });
    } catch (error) {
      const failure = error as {errors?: Message[]};
      if (!Array.isArray(failure.errors)) throw error;
      diagnostics.push(...failure.errors.map((message) =>
        messageDiagnostic(root, message, "PXCLIEXT1001", "error", entryVirtual)));
      continue;
    }
    const output = result.outputFiles.find((file) => file.path.endsWith(".js"));
    const map = result.outputFiles.find((file) => file.path.endsWith(".js.map"))?.text ?? "";
    if (output === undefined) throw new Error(`esbuild produced no JavaScript for ${entryVirtual}`);
    const code = output.text.replace(/\n\/\/# sourceMappingURL=[^\n]*\n?$/u, "\n");
    const inputs: {path: string; hash: string}[] = [];
    for (const input of Object.keys(result.metafile.inputs).sort()) {
      if (input.includes(":")) continue; // The runtime facade (its own namespace).
      const path = within(root, resolve(root, input));
      if (path === undefined) continue;
      const text = await readFile(join(root, ...path.split("/")), "utf8");
      sources.set(path, text);
      inputs.push({path, hash: sha256(text)});
    }
    const findings = [
      ...result.warnings.map((message) => messageDiagnostic(root, message, "PXCLIEXT1002", "warning", entryVirtual)),
      ...typeCheck(root, entryPath, declarationsPath, declarations,
        new Set(manifest.capabilities), manifestName),
    ];
    diagnostics.push(...findings);
    builds[manifestPath] = {entry: bundleEntry, code};
    if (options.cache !== false)
      await writeCache(root, manifestPath, {format: cacheFormat, key, inputs, entry: bundleEntry,
        code, map, diagnostics: findings});
  }
  return {builds, diagnostics, sources};
}

// Player logs name bundle positions (`Content/Extensions/x/main.js:12:5`).
// The cached source map turns them back into the authored TypeScript line.

const base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

interface Mapping {readonly line: number; readonly column: number; readonly source: number;
  readonly sourceLine: number; readonly sourceColumn: number}

function decodeMappings(mappings: string): Mapping[][] {
  const lines: Mapping[][] = [];
  let source = 0; let sourceLine = 0; let sourceColumn = 0;
  for (const [lineIndex, text] of mappings.split(";").entries()) {
    const line: Mapping[] = [];
    let column = 0;
    for (const segment of text.split(",")) {
      if (segment.length === 0) continue;
      const values: number[] = [];
      let value = 0; let shift = 0;
      for (const character of segment) {
        const digit = base64.indexOf(character);
        if (digit < 0) return lines;
        value += (digit & 31) << shift;
        if ((digit & 32) !== 0) { shift += 5; continue; }
        values.push((value & 1) === 1 ? -(value >>> 1) : value >>> 1);
        value = 0; shift = 0;
      }
      column += values[0] ?? 0;
      if (values.length >= 4) {
        source += values[1]!; sourceLine += values[2]!; sourceColumn += values[3]!;
        line.push({line: lineIndex, column, source, sourceLine, sourceColumn});
      }
    }
    lines.push(line);
  }
  return lines;
}

/**
 * Rewrites `<bundle>.js:line[:column]` positions in Player output to the
 * TypeScript source they came from, using the build cache of `root`.
 */
export function remapExtensionStack(text: string, root: string,
  manifestPaths: readonly string[]): string {
  return extensionRemapper(root, manifestPaths)(text);
}

/**
 * `remapExtensionStack` with the source maps read once: for a stream of
 * Player output. Positions without a map stay as they are.
 */
export function extensionRemapper(root: string,
  manifestPaths: readonly string[]): (text: string) => string {
  const maps = new Map<string, {sources: string[]; lines: Mapping[][]}>();
  for (const manifestPath of manifestPaths) {
    let entry: CacheEntry;
    try { entry = JSON.parse(readFileSync(cacheFile(root, manifestPath), "utf8")) as CacheEntry; }
    catch { continue; }
    if (entry.format !== cacheFormat || entry.map.length === 0) continue;
    const directory = manifestDirectory(manifestPath);
    try {
      const map = JSON.parse(entry.map) as {sources: string[]; mappings: string};
      maps.set(joinVirtual(directory, entry.entry), {
        sources: map.sources.map((source) => source.includes(":") ? source : joinVirtual(directory, source)),
        lines: decodeMappings(map.mappings)});
    } catch { /* An unreadable map leaves positions as they are. */ }
  }
  if (maps.size === 0) return (text) => text;
  return (text) => text.replace(/([\w./@-]+\.js):(\d+)(?::(\d+))?/gu, (match, file: string, line: string, column?: string) => {
    const map = maps.get(file);
    const segments = map?.lines[Number(line) - 1];
    if (map === undefined || segments === undefined || segments.length === 0) return match;
    const wanted = column === undefined ? 0 : Number(column) - 1;
    const segment = [...segments].reverse().find((item) => item.column <= wanted) ?? segments[0]!;
    const source = map.sources[segment.source];
    if (source === undefined) return match;
    return `${source}:${segment.sourceLine + 1}:${segment.sourceColumn + 1}`;
  });
}

/** The remapper for the extensions a project declares (`prismatix.json` or `project.pxproject`). */
export async function projectExtensionRemapper(projectRoot: string): Promise<(text: string) => string> {
  for (const name of ["prismatix.json", "project.pxproject"]) {
    let project: unknown;
    try { project = JSON.parse(await readFile(join(projectRoot, name), "utf8")); }
    catch { continue; }
    if (typeof project !== "object" || project === null || Array.isArray(project)) continue;
    const declared = (project as {extensions?: unknown}).extensions;
    const extensions = Array.isArray(declared)
      ? declared.filter((path): path is string => typeof path === "string") : [];
    return extensions.length === 0 ? (text) => text : extensionRemapper(projectRoot, extensions);
  }
  return (text) => text;
}
