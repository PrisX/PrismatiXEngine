#!/usr/bin/env node

import {createDecipheriv, createHash, randomUUID} from "node:crypto";
import {spawn, type ChildProcessWithoutNullStreams} from "node:child_process";
import {createReadStream, existsSync} from "node:fs";
import {
  copyFile, mkdir, mkdtemp, readFile, readdir, rename, rm, stat, writeFile,
} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, extname, isAbsolute, join, relative, resolve, sep} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";
import * as ts from "typescript-compiler";

import {
  discoverRuntimeTool, RuntimeDiscoveryFailure, runtimeDiscoveryHint,
  type RuntimeDiscoveryOptions,
} from "./runtime-discovery.js";
import {migrateCommand} from "./migration.js";

import {
  canonicalJson,
  assignStableSourceIds,
  compileJsxUi,
  compileProject,
  stableSourceIdReport,
  validateDocument,
  type AuthoringDiagnostic,
  type BuildArtifact,
  type ProjectDocument,
  type SourceSpan,
} from "@prismatix/authoring-sdk";

interface Options {readonly values: Map<string, string>; readonly flags: Set<string>; readonly positional: string[]}
interface LoadedProject {readonly root: string; readonly projectPath: string; readonly project: unknown; readonly documents: Record<string, unknown>; readonly sourceFiles: Record<string, string>; readonly locales: Record<string, unknown>; readonly dependencies: ReadonlySet<string>; readonly storyPath?: string}
interface BuildResult {readonly root: string; readonly artifact: BuildArtifact; readonly output: string; readonly dependencies: ReadonlySet<string>}

class PackagerFailure extends Error {
  constructor(readonly diagnostics: readonly AuthoringDiagnostic[]) {
    super(diagnostics[0]?.message ?? "Packager failed");
    this.name = "PackagerFailure";
  }
}

class JsxAuthoringFailure extends Error {
  constructor(readonly diagnostics: readonly AuthoringDiagnostic[]) {
    super(diagnostics[0]?.message ?? "JSX UI authoring failed");
    this.name = "JsxAuthoringFailure";
  }
}

const maxDocumentBytes = 16 * 1024 * 1024;

function parseOptions(args: readonly string[]): Options {
  const values = new Map<string, string>();
  const flags = new Set<string>();
  const positional: string[] = [];
  for (let index = 0; index < args.length; index += 1) {
    const value = args[index]!;
    if (!value.startsWith("--")) { positional.push(value); continue; }
    const equals = value.indexOf("=");
    if (equals >= 0) { values.set(value.slice(2, equals), value.slice(equals + 1)); continue; }
    const next = args[index + 1];
    if (next !== undefined && !next.startsWith("--")) { values.set(value.slice(2), next); index += 1; }
    else flags.add(value.slice(2));
  }
  return {values, flags, positional};
}

function diagnostic(code: string, message: string, path?: string, hint?: string): AuthoringDiagnostic {
  return {severity: "error", code, message,
    ...(path === undefined ? {} : {path}), ...(hint === undefined ? {} : {hint})};
}

interface TextRange {readonly start: number; readonly end: number}

function sourcePosition(text: string, utf16Offset: number) {
  const bounded = Math.max(0, Math.min(text.length, utf16Offset));
  let line = 1; let column = 1;
  for (let index = 0; index < bounded; index += 1) {
    if (text[index] === "\n") { line += 1; column = 1; }
    else column += 1;
  }
  return {line, column, offset: Buffer.byteLength(text.slice(0, bounded), "utf8")};
}

function sourceSpan(text: string, path: string, range: TextRange): SourceSpan {
  const start = Math.max(0, Math.min(text.length, range.start));
  const end = Math.max(start, Math.min(text.length, range.end));
  return {path, start: sourcePosition(text, start), end: sourcePosition(text, end)};
}

function firstLineSpan(text: string, path: string): SourceSpan {
  const start = text.search(/\S/u);
  const boundedStart = start < 0 ? 0 : start;
  const newline = text.indexOf("\n", boundedStart);
  return sourceSpan(text, path, {start: boundedStart,
    end: newline < 0 ? text.length : newline});
}

function printDiagnostics(values: readonly AuthoringDiagnostic[]): void {
  for (const value of values) process.stderr.write(`${JSON.stringify(value)}\n`);
}

function safeVirtualPath(value: string): boolean {
  return value.length > 0 && !value.startsWith("/") && !value.includes("\\") &&
    !value.includes(":") && !value.includes("\0") &&
    value.split("/").every((part) => part.length > 0 && part !== "." && part !== "..");
}

function projectFile(root: string, virtualPath: string): string {
  if (!safeVirtualPath(virtualPath)) throw new Error(`Unsafe project path: ${virtualPath}`);
  const target = resolve(root, ...virtualPath.split("/"));
  const prefix = root.endsWith(sep) ? root : `${root}${sep}`;
  if (target !== root && !target.startsWith(prefix)) throw new Error(`Project path escapes root: ${virtualPath}`);
  return target;
}

const starterTemplateRoot = fileURLToPath(new URL("../../templates/minimal", import.meta.url));

function jsonTemplateValue(value: string): string {
  return JSON.stringify(value).slice(1, -1);
}

function starterProjectId(value: string): string {
  const normalized = value.trim().toLowerCase().replace(/[^a-z0-9._-]+/gu, "-")
    .replace(/^[^a-z0-9]+/u, "").replace(/-+$/u, "");
  return normalized.length > 0 ? normalized.slice(0, 256) : "my-prismatix-game";
}

function integerOption(options: Options, name: string, fallback: number,
  minimum: number, maximum: number): number {
  const raw = options.values.get(name);
  if (raw === undefined) return fallback;
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum)
    throw new RangeError(`--${name} must be an integer from ${minimum} to ${maximum}`);
  return value;
}

async function createCommand(options: Options): Promise<number> {
  const target = resolve(options.positional[0] ?? "my-prismatix-game");
  try {
    let existing: string[] = [];
    try { existing = await readdir(target); } catch { /* Created below. */ }
    if (existing.length > 0) throw new Error(`Destination is not empty: ${target}`);
    const name = options.values.get("name") ?? "My PrismatiX Game";
    if (name.trim().length === 0 || name.length > 512) throw new Error("--name must not be empty or exceed 512 characters");
    const id = options.values.get("id") ?? starterProjectId(basename(target));
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/u.test(id) || id.length > 256)
      throw new Error("--id must be a stable identifier using letters, digits, '.', '_' or '-'");
    const locale = options.values.get("locale") ?? "en";
    if (!/^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$/u.test(locale))
      throw new Error("--locale must be a BCP 47 language tag such as en or zh-TW");
    const width = integerOption(options, "width", 1280, 1, 16384);
    const height = integerOption(options, "height", 720, 1, 16384);
    const replacements: Readonly<Record<string, string>> = {
      "{{PROJECT_ID}}": jsonTemplateValue(id),
      "{{PROJECT_NAME}}": jsonTemplateValue(name),
      "{{LOCALE}}": jsonTemplateValue(locale),
      "{{WIDTH}}": String(width), "{{HEIGHT}}": String(height),
      "{{SCENE_ID}}": randomUUID(), "{{ROOT_ID}}": randomUUID(),
      "{{TITLE_ID}}": randomUUID(), "{{START_ID}}": randomUUID(),
    };
    for (const virtualPath of await listFiles(starterTemplateRoot)) {
      let text = await readBoundedText(projectFile(starterTemplateRoot, virtualPath));
      for (const [token, replacement] of Object.entries(replacements))
        text = text.replaceAll(token, replacement);
      const destination = projectFile(target, virtualPath);
      await mkdir(dirname(destination), {recursive: true});
      await writeFile(destination, text, "utf8");
    }
    await mkdir(join(target, "Assets"), {recursive: true});
    const project = join(target, "prismatix.json");
    process.stdout.write(`${JSON.stringify({created: true, project, next: [
      `cd ${basename(target)}`, "prismatix dev",
    ]})}\n`);
    return 0;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI1501", "Project could not be created", target, String(error))]);
    return 1;
  }
}

async function readBoundedText(path: string): Promise<string> {
  const info = await stat(path);
  if (!info.isFile() || info.size > maxDocumentBytes) throw new Error(`Document is missing or exceeds 16 MiB: ${path}`);
  return readFile(path, "utf8");
}

async function readJson(path: string): Promise<unknown> {
  const text = await readBoundedText(path);
  if (text.startsWith("\uFEFF")) throw new Error(`Canonical JSON contains a BOM: ${path}`);
  return JSON.parse(text) as unknown;
}

function isJsxUiPath(path: string): boolean {
  return /\.(?:jsx|tsx)$/iu.test(path);
}

function canonicalUiPath(path: string, kind: "scene" | "component"): string {
  return path.replace(/\.(?:jsx|tsx)$/iu,
    kind === "scene" ? ".pxui" : ".pxuicomponent");
}

function sdkModuleUrl(specifier: "@prismatix/authoring-sdk" |
  "@prismatix/authoring-sdk/jsx-runtime" |
  "@prismatix/authoring-sdk/jsx-dev-runtime"): string {
  return import.meta.resolve(specifier);
}

type SdkModuleSpecifier = Parameters<typeof sdkModuleUrl>[0];

function isSdkModuleSpecifier(value: string): value is SdkModuleSpecifier {
  return value === "@prismatix/authoring-sdk" ||
    value === "@prismatix/authoring-sdk/jsx-runtime" ||
    value === "@prismatix/authoring-sdk/jsx-dev-runtime";
}

function sdkDeclarationPath(specifier: SdkModuleSpecifier): string {
  return fileURLToPath(sdkModuleUrl(specifier)).replace(/\.js$/u, ".d.ts");
}

function rewriteSdkImports(output: string): string {
  let result = output;
  for (const specifier of [
    "@prismatix/authoring-sdk",
    "@prismatix/authoring-sdk/jsx-runtime",
    "@prismatix/authoring-sdk/jsx-dev-runtime",
  ] as const) {
    const escaped = specifier.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
    result = result.replace(new RegExp(`(["'])${escaped}\\1`, "gu"),
      JSON.stringify(sdkModuleUrl(specifier)));
  }
  return result;
}

function projectSourcePath(root: string, path: string): string | undefined {
  const value = relative(root, resolve(path)).replaceAll("\\", "/");
  return value === "" || value === ".." || value.startsWith("../") || isAbsolute(value)
    ? undefined : value;
}

function typescriptDiagnostics(values: readonly ts.Diagnostic[], root: string,
  entryPath: string, entryText: string): AuthoringDiagnostic[] {
  return values.filter((value) => value.category === ts.DiagnosticCategory.Error)
    .map((value) => {
      const source = value.file;
      const path = source === undefined
        ? entryPath : projectSourcePath(root, source.fileName) ?? source.fileName;
      const text = source?.text ?? entryText;
      const start = value.start ?? 0;
      const end = start + (value.length ?? 1);
      return {
        ...diagnostic("PXCLIJSX1001", "JSX/TypeScript type-check failed", path),
        details: `TS${value.code}: ${ts.flattenDiagnosticMessageText(value.messageText, "\n")}`,
        ...(source === undefined ? {} : {span: sourceSpan(text, path, {start, end})}),
      };
    });
}

function jsxCompilerOptions(): ts.CompilerOptions {
  return {
    target: ts.ScriptTarget.ES2023,
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    jsx: ts.JsxEmit.ReactJSX,
    jsxImportSource: "@prismatix/authoring-sdk",
    strict: true,
    noEmit: true,
    allowJs: true,
    checkJs: true,
    allowImportingTsExtensions: true,
    isolatedModules: true,
    verbatimModuleSyntax: true,
    skipLibCheck: true,
  };
}

interface JsxProgram {
  readonly program: ts.Program;
  readonly options: ts.CompilerOptions;
  readonly host: ts.CompilerHost;
  readonly sourceFiles: readonly ts.SourceFile[];
}

function createJsxProgram(root: string, sourcePath: string): JsxProgram {
  const options = jsxCompilerOptions();
  const host = ts.createCompilerHost(options);
  const cache = ts.createModuleResolutionCache(root,
    (value) => ts.sys.useCaseSensitiveFileNames ? value : value.toLowerCase(), options);
  host.resolveModuleNames = (names, containingFile) => names.map((name) => {
    if (isSdkModuleSpecifier(name)) {
      return {
        resolvedFileName: sdkDeclarationPath(name),
        extension: ts.Extension.Dts,
        isExternalLibraryImport: true,
      };
    }
    return ts.resolveModuleName(name, containingFile, options, host, cache).resolvedModule;
  });
  const program = ts.createProgram({rootNames: [sourcePath], options, host});
  const sourceFiles = program.getSourceFiles().filter((source) =>
    !source.isDeclarationFile && projectSourcePath(root, source.fileName) !== undefined);
  return {program, options, host, sourceFiles};
}

function moduleOutputPath(temp: string, root: string, sourcePath: string): string {
  const virtualPath = projectSourcePath(root, sourcePath);
  if (virtualPath === undefined) throw new Error(`Imported authoring module is outside the project: ${sourcePath}`);
  return join(temp, ...virtualPath.replace(/\.(?:[cm]?[jt]sx?)$/iu, ".mjs").split("/"));
}

function pathKey(value: string): string {
  const absolute = resolve(value);
  return ts.sys.useCaseSensitiveFileNames ? absolute : absolute.toLowerCase();
}

function rewriteModuleImport(output: string, specifier: string, replacement: string): string {
  const escaped = specifier.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
  return output.replace(new RegExp(`(["'])${escaped}\\1`, "gu"), JSON.stringify(replacement));
}

function executableRelativeImport(from: string, to: string): string {
  const value = relative(dirname(from), to).replaceAll("\\", "/");
  return value.startsWith(".") ? value : `./${value}`;
}

async function emitJsxProgram(root: string, sourcePath: string, value: JsxProgram,
  temp: string): Promise<string> {
  const outputs = new Map(value.sourceFiles.map((source) =>
    [pathKey(source.fileName), moduleOutputPath(temp, root, source.fileName)]));
  for (const source of value.sourceFiles) {
    const outputPath = outputs.get(pathKey(source.fileName))!;
    const transpiled = ts.transpileModule(source.text, {
      fileName: source.fileName,
      reportDiagnostics: true,
      compilerOptions: {
        target: ts.ScriptTarget.ES2023,
        module: ts.ModuleKind.ESNext,
        moduleResolution: ts.ModuleResolutionKind.Bundler,
        jsx: ts.JsxEmit.ReactJSX,
        jsxImportSource: "@prismatix/authoring-sdk",
        isolatedModules: true,
        verbatimModuleSyntax: true,
      },
    });
    const emittedDiagnostics = typescriptDiagnostics(transpiled.diagnostics ?? [], root,
      projectSourcePath(root, sourcePath)!, value.program.getSourceFile(sourcePath)?.text ?? "");
    if (emittedDiagnostics.length > 0) throw new JsxAuthoringFailure(emittedDiagnostics);
    let output = rewriteSdkImports(transpiled.outputText);
    for (const imported of ts.preProcessFile(source.text, true, true).importedFiles) {
      if (!imported.fileName.startsWith(".")) continue;
      const resolvedModule = ts.resolveModuleName(imported.fileName, source.fileName,
        value.options, value.host).resolvedModule;
      if (resolvedModule === undefined) continue;
      const target = outputs.get(pathKey(resolvedModule.resolvedFileName));
      if (target === undefined) {
        throw new TypeError(`Local authoring import is outside the emitted module graph: ${imported.fileName}`);
      }
      output = rewriteModuleImport(output, imported.fileName,
        executableRelativeImport(outputPath, target));
    }
    await mkdir(dirname(outputPath), {recursive: true});
    await writeFile(outputPath, output, "utf8");
  }
  const entry = outputs.get(pathKey(sourcePath));
  if (entry === undefined) throw new Error(`JSX entry module was not emitted: ${sourcePath}`);
  return entry;
}

async function loadJsxUiDocument(root: string, virtualPath: string,
  kind: "scene" | "component", dependencies?: Set<string>): Promise<unknown> {
  const sourcePath = projectFile(root, virtualPath);
  const source = await readBoundedText(sourcePath);
  const program = createJsxProgram(root, sourcePath);
  for (const dependency of program.sourceFiles) dependencies?.add(resolve(dependency.fileName));
  const checked = typescriptDiagnostics(ts.getPreEmitDiagnostics(program.program),
    root, virtualPath, source);
  if (checked.length > 0) throw new JsxAuthoringFailure(checked);
  const temp = await mkdtemp(join(tmpdir(), "prismatix-jsx-"));
  try {
    const executablePath = await emitJsxProgram(root, sourcePath, program, temp);
    const module = await import(`${pathToFileURL(executablePath).href}?build=${Date.now()}`) as {
      readonly default?: unknown;
    };
    let authored = module.default;
    if (typeof authored === "function") authored = (authored as () => unknown)();
    authored = await Promise.resolve(authored);
    if (authored === undefined) {
      throw new TypeError("JSX UI modules must provide a default Scene or Component export");
    }
    const compiled = compileJsxUi(authored as Parameters<typeof compileJsxUi>[0],
      {path: virtualPath, kind});
    if (!compiled.valid || compiled.value === undefined)
      throw new JsxAuthoringFailure(compiled.diagnostics);
    return compiled.value;
  } catch (error) {
    if (error instanceof JsxAuthoringFailure) throw error;
    throw new JsxAuthoringFailure([{
      ...diagnostic("PXCLIJSX1002", "JSX UI module could not be evaluated", virtualPath),
      details: String(error),
      span: firstLineSpan(source, virtualPath),
    }]);
  } finally {
    await rm(temp, {recursive: true, force: true});
  }
}

function object(value: unknown): Record<string, unknown> | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : undefined;
}

function packageDiagnostic(value: unknown): AuthoringDiagnostic | undefined {
  const source = object(value);
  if (source === undefined || typeof source.code !== "string" ||
      typeof source.message !== "string") return undefined;
  const span = object(source.span);
  const start = object(span?.start);
  const end = object(span?.end);
  const validPosition = (position: Record<string, unknown> | undefined) =>
    position !== undefined && typeof position.line === "number" &&
    typeof position.column === "number" && typeof position.offset === "number";
  return {
    severity: source.severity === "warning" || source.severity === "information"
      ? source.severity : "error",
    code: source.code,
    message: source.message,
    ...(typeof source.documentId === "string" && source.documentId.length > 0
      ? {documentId: source.documentId} : {}),
    ...(typeof source.sourceId === "string" && source.sourceId.length > 0
      ? {sourceId: source.sourceId} : {}),
    ...(span !== undefined && typeof span.path === "string" &&
        validPosition(start) && validPosition(end)
      ? {path: span.path, span: {path: span.path,
          start: {line: start!.line as number, column: start!.column as number,
            offset: start!.offset as number},
          end: {line: end!.line as number, column: end!.column as number,
            offset: end!.offset as number}}} : {}),
    ...(typeof source.hint === "string" && source.hint.length > 0
      ? {hint: source.hint} : {}),
    ...(typeof source.cause === "string" && source.cause.length > 0
      ? {cause: source.cause} : {}),
  };
}

function packageEvents(output: string): readonly Record<string, unknown>[] {
  const events: Record<string, unknown>[] = [];
  for (const line of output.split(/\r?\n/u)) {
    if (line.trim().length === 0) continue;
    try {
      const parsed = object(JSON.parse(line) as unknown);
      if (parsed !== undefined) events.push(parsed);
    } catch {
      // A native tool that emits non-protocol output is rejected below when
      // the required completed/failed event cannot be found.
    }
  }
  return events;
}

function failedPackageDiagnostics(events: readonly Record<string, unknown>[]): readonly AuthoringDiagnostic[] {
  const failed = [...events].reverse().find((event) => event.event === "failed");
  if (failed === undefined) return [];
  const values = Array.isArray(failed.diagnostics) ? failed.diagnostics :
    failed.diagnostic === undefined ? [failed] : [failed.diagnostic];
  return values.map(packageDiagnostic).filter((value): value is AuthoringDiagnostic => value !== undefined);
}

async function regularFile(path: string): Promise<boolean> {
  try { return (await stat(path)).isFile(); } catch { return false; }
}

async function resolveProjectPath(projectArgument?: string): Promise<{
  readonly root: string; readonly projectPath: string; readonly manifestExists: boolean;
}> {
  const candidate = resolve(projectArgument ?? ".");
  let directory = false;
  try { directory = (await stat(candidate)).isDirectory(); } catch { directory = false; }
  if (!directory) return {root: dirname(candidate), projectPath: candidate,
    manifestExists: await regularFile(candidate)};
  const beginner = join(candidate, "prismatix.json");
  const canonical = join(candidate, "project.pxproject");
  if (await regularFile(beginner)) return {root: candidate, projectPath: beginner, manifestExists: true};
  if (await regularFile(canonical)) return {root: candidate, projectPath: canonical, manifestExists: true};
  const conventionalStory = join(candidate, "Story", "main.pxstory");
  if (await regularFile(conventionalStory))
    return {root: candidate, projectPath: beginner, manifestExists: false};
  throw new Error(`No prismatix.json, project.pxproject, or Story/main.pxstory was found in ${candidate}`);
}

async function conventionalStory(root: string): Promise<string | undefined> {
  if (await regularFile(projectFile(root, "Story/main.pxstory"))) return "Story/main.pxstory";
  try {
    const paths = (await listFiles(join(root, "Story")))
      .filter((path) => path.toLowerCase().endsWith(".pxstory"));
    return paths.length === 1 ? `Story/${paths[0]}` : undefined;
  } catch { return undefined; }
}

async function loadProject(projectArgument?: string): Promise<LoadedProject> {
  const resolvedProject = await resolveProjectPath(projectArgument);
  const {root, projectPath} = resolvedProject;
  const authoredProject = resolvedProject.manifestExists ? await readJson(projectPath) : {};
  const authoredRaw = object(authoredProject);
  if (authoredRaw === undefined) throw new Error(`${basename(projectPath)} must be a JSON object`);
  const project = structuredClone(authoredProject);
  const raw = object(project);
  if (raw === undefined) throw new Error(`${basename(projectPath)} must be a JSON object`);
  const documents: Record<string, unknown> = {};
  const documentSources = new Map<string, string>();
  const sourceFiles: Record<string, string> = {};
  const locales: Record<string, unknown> = {};
  const dependencies = new Set<string>([projectPath]);
  const loadDocument = async (virtualPath: unknown): Promise<void> => {
    if (typeof virtualPath !== "string") return;
    const owner = documentSources.get(virtualPath);
    if (owner === virtualPath) return;
    if (owner !== undefined) throw new Error(`${virtualPath} is already produced by ${owner}`);
    const path = projectFile(root, virtualPath);
    documents[virtualPath] = await readJson(path);
    dependencies.add(path);
    documentSources.set(virtualPath, virtualPath);
  };
  const loadJsxDocument = async (virtualPath: string,
    kind: "scene" | "component"): Promise<string> => {
    const outputPath = canonicalUiPath(virtualPath, kind);
    const owner = documentSources.get(outputPath);
    if (owner === virtualPath) return outputPath;
    if (owner !== undefined) throw new Error(`${outputPath} is already produced by ${owner}`);
    documents[outputPath] = await loadJsxUiDocument(root, virtualPath, kind, dependencies);
    documentSources.set(outputPath, virtualPath);
    return outputPath;
  };
  await loadDocument(raw.storyIndex);
  await loadDocument(raw.gameCatalog);
  for (const value of Array.isArray(raw.extensions) ? raw.extensions : []) await loadDocument(value);
  const authoredEntryPoints = object(authoredRaw.uiEntryPoints) ?? {};
  const runtimeEntryPoints = object(raw.uiEntryPoints) ?? {};
  for (const [id, value] of Object.entries(authoredEntryPoints)) {
    if (typeof value === "string" && isJsxUiPath(value)) {
      runtimeEntryPoints[id] = await loadJsxDocument(value, "scene");
    } else await loadDocument(value);
  }
  for (const value of Array.isArray(raw.characters) ? raw.characters : []) await loadDocument(object(value)?.source);
  const authoredComponents = Array.isArray(authoredRaw.uiComponents) ? authoredRaw.uiComponents : [];
  const runtimeComponents = Array.isArray(raw.uiComponents) ? raw.uiComponents : [];
  for (let index = 0; index < authoredComponents.length; index += 1) {
    const source = object(authoredComponents[index])?.source;
    const runtimeDescriptor = object(runtimeComponents[index]);
    if (typeof source === "string" && isJsxUiPath(source)) {
      const outputPath = await loadJsxDocument(source, "component");
      if (runtimeDescriptor !== undefined) runtimeDescriptor.source = outputPath;
    } else await loadDocument(source);
  }
  for (const value of Array.isArray(raw.saveMigrations) ? raw.saveMigrations : []) await loadDocument(object(value)?.asset);
  for (const value of Array.isArray(raw.effects) ? raw.effects : []) await loadDocument(object(value)?.source);

  const index = typeof raw.storyIndex === "string" ? object(documents[raw.storyIndex]) : undefined;
  for (const scene of Array.isArray(index?.scenes) ? index.scenes : []) {
    for (const source of Object.values(object(object(scene)?.sources) ?? {})) {
      if (typeof source === "string" && sourceFiles[source] === undefined)
        sourceFiles[source] = await readBoundedText(projectFile(root, source));
    }
  }
  let storyPath: string | undefined;
  if (index === undefined) {
    storyPath = await conventionalStory(root);
    if (storyPath !== undefined)
      sourceFiles[storyPath] = await readBoundedText(projectFile(root, storyPath));
  }
  for (const value of Array.isArray(raw.effects) ? raw.effects : []) {
    const descriptor = object(value);
    const manifest = typeof descriptor?.source === "string"
      ? object(documents[descriptor.source]) : undefined;
    if (typeof manifest?.shader === "string" && sourceFiles[manifest.shader] === undefined)
      sourceFiles[manifest.shader] = await readBoundedText(projectFile(root, manifest.shader));
  }
  for (const locale of Array.isArray(raw.supportedLocales) ? raw.supportedLocales : []) {
    if (typeof locale !== "string") continue;
    const localePath = projectFile(root, `Content/Localization/${locale}.json`);
    locales[locale] = await readJson(localePath);
    dependencies.add(localePath);
  }
  for (const path of Object.keys(sourceFiles)) dependencies.add(projectFile(root, path));
  for (const value of Array.isArray(raw.assets) ? raw.assets : []) {
    const source = object(value)?.source;
    if (typeof source === "string") dependencies.add(projectFile(root, source));
  }
  for (const extensionPath of Array.isArray(raw.extensions) ? raw.extensions : []) {
    if (typeof extensionPath !== "string") continue;
    const extension = object(documents[extensionPath]);
    const prefix = extensionPath.includes("/")
      ? extensionPath.slice(0, extensionPath.lastIndexOf("/") + 1) : "";
    for (const module of [extension?.entry,
      ...(Array.isArray(extension?.modules) ? extension.modules : [])]) {
      if (typeof module === "string") dependencies.add(projectFile(root, `${prefix}${module}`));
    }
  }
  return {root, projectPath, project, documents, sourceFiles, locales, dependencies,
    ...(storyPath === undefined ? {} : {storyPath})};
}

async function compileLoaded(loaded: LoadedProject, committedRevision?: number) {
  return compileProject({project: loaded.project, documents: loaded.documents,
    sourceFiles: loaded.sourceFiles, locales: loaded.locales,
    projectPath: "project.pxproject",
    ...(loaded.storyPath === undefined ? {} : {storyPath: loaded.storyPath}),
    resourceExists: (path) => {
      try { return existsSync(projectFile(loaded.root, path)); } catch { return false; }
    },
    ...(committedRevision === undefined ? {} : {committedRevision})});
}

async function validateCommand(options: Options): Promise<number> {
  try {
    const loaded = await loadProject(options.positional[0]);
    const result = await compileLoaded(loaded);
    printDiagnostics(result.diagnostics);
    if (!result.valid) return 1;
    process.stdout.write(`${JSON.stringify({valid: true, engineVersion: "0.2.0", project: loaded.projectPath})}\n`);
    return 0;
  } catch (error) {
    if (error instanceof JsxAuthoringFailure) {
      printDiagnostics(error.diagnostics);
      return 1;
    }
    printDiagnostics([diagnostic("PXCLI1001", "Project could not be loaded", options.positional[0], String(error))]);
    return 1;
  }
}

async function copyRuntimeInputs(loaded: LoadedProject, artifact: BuildArtifact, staging: string): Promise<void> {
  for (const [virtualPath, text] of Object.entries(artifact.files)) {
    const destination = projectFile(staging, virtualPath);
    await mkdir(dirname(destination), {recursive: true});
    await writeFile(destination, text, "utf8");
  }
  for (const dependency of artifact.dependencies) {
    const destination = projectFile(staging, dependency.logicalPath);
    await mkdir(dirname(destination), {recursive: true});
    await copyFile(projectFile(loaded.root, dependency.source), destination);
  }
}

async function promoteDirectory(staging: string, output: string): Promise<void> {
  if (output === resolve(output).split(sep)[0] || output === dirname(output)) throw new Error("Refusing to replace a filesystem root");
  const backup = `${output}.backup-${process.pid}`;
  await rm(backup, {recursive: true, force: true});
  let hadOutput = false;
  try { hadOutput = (await stat(output)).isDirectory(); } catch { hadOutput = false; }
  if (hadOutput) await rename(output, backup);
  try {
    await rename(staging, output);
    if (hadOutput) await rm(backup, {recursive: true, force: true});
  } catch (error) {
    if (hadOutput) await rename(backup, output);
    throw error;
  }
}

async function buildProject(projectArgument: string | undefined, outputArgument: string | undefined,
  committedRevision?: number): Promise<BuildResult> {
  const loaded = await loadProject(projectArgument);
  const compiled = await compileLoaded(loaded, committedRevision);
  printDiagnostics(compiled.diagnostics);
  if (!compiled.valid || compiled.artifact === undefined) throw new Error("Project validation or compilation failed");
  const output = resolve(outputArgument ?? join(loaded.root, ".prismatix", "build"));
  if (output === loaded.root || loaded.root.startsWith(`${output}${sep}`)) throw new Error("Build output cannot equal or contain the project root");
  await mkdir(dirname(output), {recursive: true});
  const staging = await mkdtemp(join(dirname(output), `.${basename(output)}.staging-`));
  try {
    await copyRuntimeInputs(loaded, compiled.artifact, staging);
    await promoteDirectory(staging, output);
  } catch (error) {
    await rm(staging, {recursive: true, force: true});
    throw error;
  }
  const dependencies = new Set(loaded.dependencies);
  for (const dependency of compiled.artifact.dependencies) {
    try { dependencies.add(projectFile(loaded.root, dependency.source)); }
    catch { /* Compiler paths are advisory. */ }
  }
  return {root: loaded.root, artifact: compiled.artifact, output, dependencies};
}

async function buildCommand(options: Options): Promise<number> {
  if (!options.flags.has("artifact-only") &&
      (!options.values.has("output") || options.flags.has("distribution"))) {
    try {
      const packaged = await packProject(options);
      process.stdout.write(`${JSON.stringify({built: true, distribution: true, ...packaged})}\n`);
      return 0;
    } catch (error) {
      printDiagnostics(error instanceof PackagerFailure || error instanceof JsxAuthoringFailure
        ? error.diagnostics : [diagnostic("PXCLI1101", "Build failed", options.positional[0], String(error))]);
      return 1;
    }
  }
  try {
    const built = await buildProject(options.positional[0], options.values.get("output"));
    process.stdout.write(`${JSON.stringify({built: true, output: built.output,
      runtimeIr: built.artifact.runtimeIrPath, sourceMap: built.artifact.sourceMapPath})}\n`);
    return 0;
  } catch (error) {
    if (error instanceof JsxAuthoringFailure) {
      printDiagnostics(error.diagnostics);
      return 1;
    }
    printDiagnostics([diagnostic("PXCLI1101", "Build failed", options.positional[0], String(error))]);
    return 1;
  }
}

async function listFiles(root: string, directory = root): Promise<string[]> {
  const output: string[] = [];
  for (const entry of await readdir(directory, {withFileTypes: true})) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) output.push(...await listFiles(root, path));
    else if (entry.isFile()) output.push(relative(root, path).split(sep).join("/"));
  }
  return output.sort();
}

async function sha256(path: string): Promise<string> {
  return new Promise((accept, reject) => {
    const hash = createHash("sha256");
    const input = createReadStream(path, {highWaterMark: 256 * 1024});
    input.on("data", (chunk) => hash.update(chunk));
    input.on("error", reject);
    input.on("end", () => accept(hash.digest("hex")));
  });
}

async function mapBounded<T, U>(values: readonly T[], concurrency: number,
  operation: (value: T, index: number) => Promise<U>): Promise<U[]> {
  const output = new Array<U>(values.length);
  let cursor = 0;
  const worker = async (): Promise<void> => {
    while (cursor < values.length) {
      const index = cursor++;
      output[index] = await operation(values[index]!, index);
    }
  };
  await Promise.all(Array.from({length: Math.min(concurrency, values.length)}, worker));
  return output;
}

async function runProcess(executable: string, args: readonly string[], input?: string,
  cwd?: string, stream = false): Promise<{code: number; stdout: string; stderr: string}> {
  return new Promise((accept, reject) => {
    const script = [".js", ".mjs", ".cjs"].includes(extname(executable).toLowerCase());
    const child = spawn(script ? process.execPath : executable,
      script ? [executable, ...args] : [...args],
      {cwd, stdio: ["pipe", "pipe", "pipe"], windowsHide: true});
    let stdout = ""; let stderr = "";
    child.stdout.setEncoding("utf8"); child.stderr.setEncoding("utf8");
    child.stdout.on("data", (value: string) => { stdout += value; if (stream) process.stdout.write(value); });
    child.stderr.on("data", (value: string) => { stderr += value; if (stream) process.stderr.write(value); });
    child.on("error", reject);
    child.on("close", (code) => accept({code: code ?? 1, stdout, stderr}));
    if (input !== undefined) child.stdin.end(input); else child.stdin.end();
  });
}

function runtimeOptions(options: Options): RuntimeDiscoveryOptions {
  const runtime = options.values.get("runtime");
  const previewHost = options.values.get("preview-host");
  const packager = options.values.get("packager");
  const player = options.values.get("player");
  return {
    ...(runtime === undefined ? {} : {runtime}),
    ...(previewHost === undefined ? {} : {previewHost}),
    ...(packager === undefined ? {} : {packager}),
    ...(player === undefined ? {} : {player}),
  };
}

function discoveryDiagnostic(code: string, message: string, error: RuntimeDiscoveryFailure,
  path?: string): AuthoringDiagnostic {
  return diagnostic(code, message, path, runtimeDiscoveryHint(error));
}

async function packProject(options: Options): Promise<{output: string; player: string}> {
  let packager: string;
  let player: string;
  try {
    [packager, player] = await Promise.all([
      discoverRuntimeTool("packager", runtimeOptions(options)).then((value) => value.path),
      discoverRuntimeTool("player", runtimeOptions(options)).then((value) => value.path),
    ]);
  } catch (error) {
    if (error instanceof RuntimeDiscoveryFailure) {
      throw new PackagerFailure([discoveryDiagnostic("PXCLI1204",
        `PrismatiX ${error.tool} could not be discovered`, error, options.positional[0])]);
    }
    throw error;
  }
  const temp = await mkdtemp(join(tmpdir(), "prismatix-pack-"));
  try {
    const built = await buildProject(options.positional[0], join(temp, "artifact"));
    const project = built.artifact.project;
    const files = await listFiles(built.output);
    const inputs = await mapBounded(files, 4, async (uri) => {
      const path = projectFile(built.output, uri);
      return {uri, fingerprint: await sha256(path), size: (await stat(path)).size};
    });
    const output = resolve(options.values.get("output") ?? join(built.root, "dist", project.id));
    const request = {
      format: "PrismatiXPackageRequest", schemaRevision: 2,
      requestId: `cli-${process.pid}`, gameId: project.id,
      projectRoot: built.output, outputDir: output,
      playerExecutable: player, title: project.name,
      width: project.resolution.width, height: project.resolution.height,
      startScript: built.artifact.runtimeIrPath,
      sourceMap: built.artifact.sourceMapPath,
      startRoute: project.entry.ui,
      routes: Object.entries(project.uiEntryPoints).map(([id, scene]) => ({id, scene})),
      contentVersion: project.contentVersion, saveVersion: project.saveVersion,
      graphicsTier: project.graphicsTier ?? "basic",
      saveMigrations: project.saveMigrations ?? [],
      extensions: project.extensions,
      encryption: options.flags.has("encrypt"),
      compression: options.values.get("compression") ?? "balanced",
      inputs, cancelFile: join(temp, ".cancel-package"),
    };
    const requestPath = join(temp, "package-request.json");
    await writeFile(requestPath, `${JSON.stringify(request)}\n`, "utf8");
    // The native Packager intentionally accepts only an absolute request file
    // so its bounded parser, diagnostics and cancellation lifecycle have one
    // wire protocol across CLI, Studio and CI.
    const packageTool = packager;
    const scriptTool = [".js", ".mjs", ".cjs"].includes(extname(packageTool).toLowerCase());
    const result = await runProcess(
      scriptTool ? process.execPath : packageTool,
      [...(scriptTool ? [packageTool] : []), "--request", requestPath],
      undefined, built.root,
    );
    if (result.stdout.length > 0) process.stdout.write(result.stdout);
    if (result.stderr.length > 0) process.stderr.write(result.stderr);
    const events = packageEvents(result.stdout);
    const failed = failedPackageDiagnostics(events);
    if (result.code !== 0 || failed.length > 0) {
      if (failed.length > 0) throw new PackagerFailure(failed);
      throw new PackagerFailure([diagnostic("PXCLI1202",
        `Packager exited with code ${result.code} without a structured failure event`,
        options.positional[0])]);
    }
    if (!events.some((event) => event.event === "completed")) {
      throw new PackagerFailure([diagnostic("PXCLI1203",
        "Packager exited successfully without a completed event",
        options.positional[0])]);
    }
    return {output, player: join(output, basename(player))};
  } finally {
    await rm(temp, {recursive: true, force: true});
  }
}

async function packCommand(options: Options): Promise<number> {
  try { const result = await packProject(options); process.stdout.write(`${JSON.stringify({packed: true, ...result})}\n`); return 0; }
  catch (error) {
    printDiagnostics(error instanceof PackagerFailure || error instanceof JsxAuthoringFailure ? error.diagnostics :
      [diagnostic("PXCLI1201", "Pack failed", options.positional[0], String(error))]);
    return 1;
  }
}

async function runCommand(options: Options): Promise<number> {
  try {
    const result = await packProject(options);
    const child = await runProcess(result.player, [], undefined, result.output, true);
    return child.code;
  } catch (error) {
    printDiagnostics(error instanceof PackagerFailure || error instanceof JsxAuthoringFailure ? error.diagnostics :
      [diagnostic("PXCLI1301", "Run failed", options.positional[0], String(error))]);
    return 1;
  }
}

type PreviewMessage = Record<string, unknown>;

class PreviewClient {
  readonly sessionId = `cli-dev-${randomUUID()}`;
  private readonly waiters = new Map<string, {
    readonly accept: (message: PreviewMessage) => void;
    readonly reject: (error: Error) => void;
    readonly timer: NodeJS.Timeout;
  }>();
  private outputBuffer = "";
  private exited = false;

  private constructor(private readonly child: ChildProcessWithoutNullStreams) {
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => {
      process.stdout.write(chunk);
      this.outputBuffer += chunk;
      let newline = this.outputBuffer.indexOf("\n");
      while (newline >= 0) {
        const line = this.outputBuffer.slice(0, newline).trim();
        this.outputBuffer = this.outputBuffer.slice(newline + 1);
        if (line.length > 0) {
          try {
            const message = object(JSON.parse(line) as unknown);
            const requestId = message?.requestId;
            if (message !== undefined && typeof requestId === "string") {
              const waiter = this.waiters.get(requestId);
              if (waiter !== undefined && message.event === undefined) {
                clearTimeout(waiter.timer); this.waiters.delete(requestId); waiter.accept(message);
              }
            }
          } catch { /* Native log lines remain visible but are not protocol responses. */ }
        }
        newline = this.outputBuffer.indexOf("\n");
      }
    });
    child.stderr.on("data", (chunk: string) => process.stderr.write(chunk));
    child.on("exit", (code, signal) => {
      this.exited = true;
      const error = new Error(`PreviewHost exited (${code ?? signal ?? "unknown"})`);
      for (const [id, waiter] of this.waiters) {
        clearTimeout(waiter.timer); waiter.reject(error); this.waiters.delete(id);
      }
    });
  }

  static async start(executable: string, cwd: string): Promise<PreviewClient> {
    const script = [".js", ".mjs", ".cjs"].includes(extname(executable).toLowerCase());
    const child = spawn(script ? process.execPath : executable, script ? [executable] : [],
      {cwd, stdio: ["pipe", "pipe", "pipe"], windowsHide: true});
    await new Promise<void>((accept, reject) => {
      child.once("spawn", accept); child.once("error", reject);
    });
    const client = new PreviewClient(child);
    const ready = await client.request("hello", "", 0, {});
    if (ready.type !== "ready") {
      await client.close();
      throw new Error(`PreviewHost handshake failed: ${JSON.stringify(ready)}`);
    }
    return client;
  }

  get alive(): boolean { return !this.exited && this.child.exitCode === null; }

  request(type: string, documentId: string, revision: number,
    fields: Readonly<Record<string, unknown>>): Promise<PreviewMessage> {
    if (!this.alive) return Promise.reject(new Error("PreviewHost is not running"));
    const requestId = `cli-${randomUUID()}`;
    const request = {protocol: "PrismatiXPreviewProtocol", schemaRevision: 2,
      protocolVersion: 2, type, sessionId: this.sessionId, requestId,
      documentId, revision, ...fields};
    return new Promise((accept, reject) => {
      const timer = setTimeout(() => {
        this.waiters.delete(requestId);
        reject(new Error(`PreviewHost timed out waiting for ${type}`));
      }, 20_000);
      this.waiters.set(requestId, {accept, reject, timer});
      this.child.stdin.write(`${JSON.stringify(request)}\n`, (error) => {
        if (error === null || error === undefined) return;
        const waiter = this.waiters.get(requestId);
        if (waiter !== undefined) {
          clearTimeout(waiter.timer); this.waiters.delete(requestId); reject(error);
        }
      });
    });
  }

  async close(): Promise<void> {
    if (!this.alive) return;
    try { await this.request("shutdown", "", 0, {}); } catch { /* Force termination below. */ }
    if (!this.alive) return;
    await Promise.race([
      new Promise<void>((accept) => this.child.once("exit", () => accept())),
      new Promise<void>((accept) => setTimeout(accept, 2_000)),
    ]);
    if (this.alive) this.child.kill();
  }
}

function previewResponseError(action: string, response: PreviewMessage): Error {
  const code = typeof response.code === "string" ? response.code : String(response.type ?? "unknown");
  const message = typeof response.message === "string" ? response.message : JSON.stringify(response);
  return new Error(`${action} was rejected (${code}): ${message}`);
}

async function applyPreviewBuild(client: PreviewClient, built: BuildResult,
  incremental: boolean): Promise<void> {
  const documentId = built.artifact.runtimeIr.documentId;
  const revision = built.artifact.runtimeIr.committedRevision;
  const fields = {projectRoot: built.output, documentId,
    committedRevision: revision, irPath: built.artifact.runtimeIrPath};
  let response = await client.request(incremental ? "patchRuntimeIr" : "applyRuntimeIr",
    documentId, revision, fields);
  if (incremental && response.type === "resyncRequired") {
    process.stderr.write(`${JSON.stringify({severity: "information", code: "PXCLI1604",
      message: "Preview requested a full resync after a revision gap", reason: response.reason})}\n`);
    response = await client.request("applyRuntimeIr", documentId, revision, fields);
  }
  if (response.type !== "runtimeIrApplied" && response.type !== "staleRevision")
    throw previewResponseError(incremental ? "Runtime patch" : "Runtime apply", response);
  if (response.type === "runtimeIrApplied") {
    process.stderr.write(`${JSON.stringify({severity: "information", code: "PXCLI1603",
      message: incremental ? "Preview updated" : "Preview started",
      applyMode: response.applyMode, revision})}\n`);
  }
}

async function dependencySignature(paths: ReadonlySet<string>): Promise<string> {
  const values: string[] = [];
  for (const path of [...paths].sort()) {
    try {
      const info = await stat(path);
      values.push(`${path}\0${info.size}\0${info.mtimeMs}`);
    } catch { values.push(`${path}\0missing`); }
  }
  return createHash("sha256").update(values.join("\n")).digest("hex");
}

function waitFor(milliseconds: number): Promise<void> {
  return new Promise((accept) => setTimeout(accept, milliseconds));
}

async function devCommand(options: Options): Promise<number> {
  let client: PreviewClient | undefined;
  try {
    let previewHost: Awaited<ReturnType<typeof discoverRuntimeTool>>;
    try { previewHost = await discoverRuntimeTool("previewHost", runtimeOptions(options)); }
    catch (error) {
      if (error instanceof RuntimeDiscoveryFailure) {
        printDiagnostics([discoveryDiagnostic("PXCLI1601",
          "PrismatiX PreviewHost could not be discovered", error, options.positional[0])]);
        return 1;
      }
      throw error;
    }
    const rawDebounce = Number(options.values.get("debounce") ?? "150");
    if (!Number.isSafeInteger(rawDebounce) || rawDebounce < 50 || rawDebounce > 10_000)
      throw new RangeError("--debounce must be an integer from 50 to 10000 milliseconds");
    const projectArgument = options.positional[0];
    const projectLocation = await resolveProjectPath(projectArgument);
    const output = join(projectLocation.root, ".prismatix", "dev");
    let revision = 1;
    let built = await buildProject(projectArgument, output, revision);
    client = await PreviewClient.start(previewHost.path, built.root);
    await applyPreviewBuild(client, built, false);
    const played = await client.request("play", built.artifact.runtimeIr.documentId,
      revision, {});
    if (played.type !== "playAccepted") throw previewResponseError("Play", played);
    process.stderr.write(`${JSON.stringify({severity: "information", code: "PXCLI1602",
      message: "Watching the project dependency graph", dependencyCount: built.dependencies.size,
      previewHost: previewHost.path, discoveredFrom: previewHost.source})}\n`);
    if (options.flags.has("once") || options.flags.has("smoke")) {
      await client.close();
      return 0;
    }

    let stopped = false;
    const stop = (): void => { stopped = true; };
    process.once("SIGINT", stop); process.once("SIGTERM", stop);
    try {
      let signature = await dependencySignature(built.dependencies);
      while (!stopped) {
        await waitFor(rawDebounce);
        if (stopped) break;
        if (!client.alive) {
          process.stderr.write(`${JSON.stringify({severity: "warning", code: "PXCLI1605",
            message: "PreviewHost exited; restarting the current build"})}\n`);
          client = await PreviewClient.start(previewHost.path, built.root);
          await applyPreviewBuild(client, built, false);
          continue;
        }
        const nextSignature = await dependencySignature(built.dependencies);
        if (nextSignature === signature) continue;
        try {
          const previousDocumentId = built.artifact.runtimeIr.documentId;
          revision += 1;
          const updated = await buildProject(projectArgument, output, revision);
          const canPatch = updated.artifact.runtimeIr.documentId === previousDocumentId;
          await applyPreviewBuild(client, updated, canPatch);
          built = updated;
          signature = await dependencySignature(built.dependencies);
        } catch (error) {
          if (error instanceof JsxAuthoringFailure) printDiagnostics(error.diagnostics);
          else printDiagnostics([diagnostic("PXCLI1606", "Development rebuild failed",
            projectArgument, String(error))]);
          signature = nextSignature;
        }
      }
    } finally {
      process.removeListener("SIGINT", stop); process.removeListener("SIGTERM", stop);
    }
    await client.close();
    return 0;
  } catch (error) {
    if (client !== undefined) await client.close();
    printDiagnostics(error instanceof JsxAuthoringFailure ? error.diagnostics :
      [diagnostic("PXCLI1600", "Development preview failed", options.positional[0], String(error))]);
    return 1;
  }
}

function stable(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stable).join(",")}]`;
  const record = object(value);
  if (record !== undefined) return `{${Object.keys(record).sort().map((key) => `${JSON.stringify(key)}:${stable(record[key])}`).join(",")}}`;
  return JSON.stringify(value);
}

interface StoryIdentitySource {
  readonly absolutePath: string;
  readonly virtualPath: string;
  readonly source: string;
}

async function identitySources(argument?: string): Promise<{
  readonly root: string; readonly documentId: string; readonly sources: readonly StoryIdentitySource[];
}> {
  if (argument !== undefined && argument.toLowerCase().endsWith(".pxstory")) {
    const absolutePath = resolve(argument);
    return {root: dirname(absolutePath), documentId: "main", sources: [{
      absolutePath, virtualPath: basename(absolutePath), source: await readBoundedText(absolutePath),
    }]};
  }
  const loaded = await loadProject(argument);
  const raw = object(loaded.project) ?? {};
  const name = typeof raw.name === "string" ? raw.name : "PrismatiX Game";
  const documentId = typeof raw.id === "string" ? raw.id : starterProjectId(name);
  const sources = Object.entries(loaded.sourceFiles)
    .filter(([path]) => path.toLowerCase().endsWith(".pxstory"))
    .map(([virtualPath, source]) => ({absolutePath: projectFile(loaded.root, virtualPath),
      virtualPath, source}));
  if (sources.length === 0) throw new Error("Project dependency graph contains no Story source files");
  return {root: loaded.root, documentId, sources};
}

async function replaceTextWithBackup(path: string, text: string): Promise<void> {
  const temporary = `${path}.prismatix-new-${process.pid}`;
  const previous = `${path}.prismatix-old-${process.pid}`;
  await writeFile(temporary, text, "utf8");
  await rename(path, previous);
  try {
    await rename(temporary, path);
    await rm(previous, {force: true});
  } catch (error) {
    try { await rename(previous, path); } catch { /* The external backup remains recoverable. */ }
    await rm(temporary, {force: true});
    throw error;
  }
}

async function idsCommand(options: Options): Promise<number> {
  try {
    const collection = await identitySources(options.positional[0]);
    const documentId = options.values.get("document-id") ?? collection.documentId;
    const prefix = options.values.get("prefix") ?? "auto";
    const assignments = collection.sources.map((value) => ({value,
      assignment: assignStableSourceIds(value.source,
        {path: value.virtualPath, documentId, prefix})}));
    const diagnostics = assignments.flatMap(({assignment}) => assignment.diagnostics);
    printDiagnostics(diagnostics);
    if (assignments.some(({assignment}) => !assignment.valid || assignment.source === undefined)) {
      process.stdout.write(`${JSON.stringify({dryRun: !options.flags.has("write"), assigned: 0,
        diagnostics, reports: assignments.map(({assignment}) => assignment.report)}, null, 2)}\n`);
      return 1;
    }
    const write = options.flags.has("write");
    let backup: string | undefined;
    if (write && assignments.some(({assignment}) => assignment.assigned > 0)) {
      backup = resolve(options.values.get("backup") ?? join(collection.root, ".prismatix",
        `identity-backup-${new Date().toISOString().replaceAll(":", "-")}`));
      for (const {value, assignment} of assignments) {
        if (assignment.assigned === 0) continue;
        const relativePath = relative(collection.root, value.absolutePath);
        if (relativePath === ".." || relativePath.startsWith(`..${sep}`) || isAbsolute(relativePath))
          throw new Error(`Story source is outside the project backup root: ${value.absolutePath}`);
        const backupPath = join(backup, relativePath);
        await mkdir(dirname(backupPath), {recursive: true});
        await copyFile(value.absolutePath, backupPath);
      }
      for (const {value, assignment} of assignments) {
        if (assignment.assigned > 0) await replaceTextWithBackup(value.absolutePath, assignment.source!);
      }
    }
    const reports = write
      ? assignments.map(({value}) => stableSourceIdReport(
          assignments.find((entry) => entry.value.absolutePath === value.absolutePath)!.assignment.source!,
          {path: value.virtualPath, documentId}))
      : assignments.map(({value}) => stableSourceIdReport(value.source,
          {path: value.virtualPath, documentId}));
    const result = {
      dryRun: !write,
      assigned: assignments.reduce((sum, value) => sum + value.assignment.assigned, 0),
      total: reports.reduce((sum, value) => sum + value.total, 0),
      anchored: reports.reduce((sum, value) => sum + value.anchored, 0),
      fallback: reports.reduce((sum, value) => sum + value.fallback, 0),
      ...(backup === undefined ? {} : {backup}), reports,
    };
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
    return options.flags.has("check") && result.fallback > 0 ? 1 : 0;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI1701", "Stable Story identity operation failed",
      options.positional[0], String(error))]);
    return 1;
  }
}

async function inspectSaveCommand(options: Options): Promise<number> {
  const path = resolve(options.positional[0] ?? "");
  try {
    let bytes = await readFile(path);
    const packagePath = options.values.get("package");
    const secret = options.values.get("secret");
    if (packagePath !== undefined || secret !== undefined) {
      let passphrase = secret;
      if (packagePath !== undefined) {
        const manifest = object(await readJson(resolve(packagePath)));
        const encryption = object(manifest?.encryption);
        passphrase = encryption?.enabled === true && typeof encryption.archiveKey === "string"
          ? encryption.archiveKey : String(manifest?.packageFingerprint ?? "");
      }
      const key = createHash("sha256").update(`${passphrase ?? ""}|px-save`).digest();
      if (bytes.length < 28) throw new Error("Encrypted save is truncated");
      const nonce = bytes.subarray(0, 12); const tag = bytes.subarray(bytes.length - 16);
      const decipher = createDecipheriv("aes-256-gcm", key, nonce); decipher.setAuthTag(tag);
      bytes = Buffer.concat([decipher.update(bytes.subarray(12, bytes.length - 16)), decipher.final()]);
    }
    const magic = Buffer.from("PRISMATIX-PERSIST\n");
    if (!bytes.subarray(0, magic.length).equals(magic)) throw new Error("Unsupported persistence envelope or missing decryption key");
    const envelope = JSON.parse(bytes.subarray(magic.length).toString("utf8")) as unknown;
    const root = object(envelope);
    if (root?.format !== "PrismatiXSave" || root.schemaRevision !== 4) throw new Error("Unsupported save schema");
    const expected = root.integrityHash;
    const authenticated = {...root}; delete authenticated.integrityHash;
    const actual = createHash("sha256").update(stable(authenticated)).digest("hex");
    if (typeof expected !== "string" || expected !== actual) throw new Error("Save integrity hash does not match");
    const state = object(root.state);
    const output = options.flags.has("json") ? envelope : {
      format: root.format, schemaRevision: root.schemaRevision,
      engineVersion: root.engineVersion, gameId: root.gameId,
      packageFingerprint: root.packageFingerprint,
      contentVersion: root.contentVersion, saveVersion: root.saveVersion,
      anchor: root.anchor, timestamp: state?.timestamp, playtimeMs: state?.playtimeMs,
      chapter: state?.chapter,
    };
    process.stdout.write(`${JSON.stringify(output, null, 2)}\n`);
    return 0;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI1401", "Save inspection failed", path, String(error))]);
    return 1;
  }
}

function usage(): void {
  process.stderr.write("Usage: prismatix <create|dev|validate|build|pack|run|ids|migrate|inspect-save> [project-or-save] [options]\n");
}

async function main(): Promise<number> {
  const [command, ...args] = process.argv.slice(2);
  const options = parseOptions(args);
  if (command === "create") return createCommand(options);
  if (command === "dev") return devCommand(options);
  if (command === "validate") return validateCommand(options);
  if (command === "build") return buildCommand(options);
  if (command === "pack") return packCommand(options);
  if (command === "run") return runCommand(options);
  if (command === "ids") return idsCommand(options);
  if (command === "migrate") return migrateCommand(options);
  if (command === "inspect-save") return inspectSaveCommand(options);
  usage(); return 2;
}

process.exitCode = await main();
