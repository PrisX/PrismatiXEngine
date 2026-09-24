#!/usr/bin/env node

import {createHash, randomUUID} from "node:crypto";
import {spawn, type ChildProcessWithoutNullStreams} from "node:child_process";
import {constants, existsSync, readFileSync, watch} from "node:fs";
import {
  copyFile, link, lstat, mkdir, mkdtemp, readFile, readdir, realpath, rename, rm, stat, writeFile,
} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, extname, isAbsolute, join, relative, resolve, sep} from "node:path";
import {fileURLToPath, pathToFileURL} from "node:url";
import * as ts from "typescript-compiler";

import {
  discoverRuntimeTool, discoverRuntimeBundle, RuntimeDiscoveryFailure, runtimeDiscoveryHint,
  type RuntimeDiscoveryOptions,
} from "./runtime-discovery.js";
import {fingerprintFile, publishDirectory, publishedInputs, retirePublishedDirectory,
  verifiedPublishedInputs, type PublishedInput} from "./output-safety.js";
import {augmentArtifact} from "./artifact-augment.js";
import {assessPackageDiskSpace, availableBytesForOutput, formatDiskBytes} from "./disk-space.js";
import {createStarter} from "./starter.js";
import {migrateCommand} from "./migration.js";
import {inspectSaveWithRuntime} from "./save-inspection.js";
import {planPackagePatch} from "./package-diff.js";
import {applyDistributionPatch, createDistributionPatch,
  type DistributionPatchProgress} from "./distribution-patch.js";

import {
  assertLocalBuildFresh,
  canonicalJson,
  applyStoryIdentityMap,
  assignStableSourceIds,
  compileJsxUi,
  compileProject,
  externalizeStoryIds,
  parseStory,
  storyRequiresIdentityMap,
  StoryProjectCompilationCache,
  stableSourceIdReport,
  synchronizeStoryIdentityMap,
  validateDocument,
  type AuthoringDiagnostic,
  type BuildArtifact,
  type CompileProjectInput,
  type InputKeyRegistryMetadata,
  type ProjectDocument,
  type SourceSpan,
  type UiTypeRegistryMetadata,
} from "@prismatix/authoring-sdk";

assertLocalBuildFresh(fileURLToPath(new URL("../..", import.meta.url)), "@prismatix/cli");

interface Options {readonly values: Map<string, string>; readonly flags: Set<string>; readonly positional: string[]}
interface LoadedProject {readonly root: string; readonly projectPath: string; readonly project: unknown; readonly documents: Record<string, unknown>; readonly sourceFiles: Record<string, string>; readonly locales: Record<string, unknown>; readonly dependencies: ReadonlySet<string>; readonly includedResources: Readonly<Record<string, readonly string[]>>; readonly storyPath?: string}
interface BuildResult {readonly root: string; readonly artifact: BuildArtifact; readonly output: string; readonly dependencies: ReadonlySet<string>; readonly protectedPaths: readonly string[]}

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
const machineOutput = process.argv.includes("--json");

const commandOptions: Readonly<Record<string, {values: string; flags: string}>> = {
  create: {values: "name id locale width height", flags: ""},
  dev: {values: "runtime preview-host debounce ui-registry", flags: "once smoke"},
  validate: {values: "runtime ui-registry", flags: ""},
  build: {values: "output artifact runtime packager player compression archive-shards ui-registry", flags: "artifact-only distribution encrypt ignore-disk-space"},
  pack: {values: "output artifact runtime packager player compression archive-shards ui-registry", flags: "encrypt ignore-disk-space"},
  run: {values: "output artifact runtime packager player compression archive-shards ui-registry", flags: "encrypt ignore-disk-space"},
  augment: {values: "artifact manifest", flags: ""},
  ids: {values: "document-id prefix backup", flags: "write check external inline sync"},
  migrate: {values: "report backup entry-story entry-ui supported-locales save-version id name version content-version default-locale story-index game-catalog", flags: "write"},
  "inspect-save": {values: "package secret runtime player", flags: ""},
  "plan-patch": {values: "base target runtime packager", flags: "verify-contents"},
  "verify-package": {values: "runtime packager", flags: ""},
  "create-patch": {values: "base target output runtime packager", flags: ""},
  "apply-patch": {values: "base patch output runtime packager", flags: "in-place"},
};
function parseOptions(command: string, args: readonly string[]): Options {
  const schema = commandOptions[command];
  if (schema === undefined) throw new Error(`Unknown command: ${command}`);
  const valueNames = new Set(schema.values.split(" ").filter(Boolean));
  const flagNames = new Set([...schema.flags.split(" ").filter(Boolean), "json", "help"]);
  const values = new Map<string, string>();
  const flags = new Set<string>();
  const positional: string[] = [];
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index]!;
    if (argument === "--") { positional.push(...args.slice(index + 1)); break; }
    if (!argument.startsWith("--")) { positional.push(argument); continue; }
    const equals = argument.indexOf("=");
    const name = argument.slice(2, equals < 0 ? undefined : equals);
    if (values.has(name) || flags.has(name)) throw new Error(`Duplicate option: --${name}`);
    if (flagNames.has(name)) {
      if (equals >= 0) throw new Error(`--${name} is a flag and does not accept a value`);
      flags.add(name); continue;
    }
    if (!valueNames.has(name)) throw new Error(`Unknown option for ${command}: --${name}`);
    const value = equals >= 0 ? argument.slice(equals + 1) : args[++index];
    if (value === undefined || value.length === 0 || value.startsWith("--")) throw new Error(`--${name} requires a value`);
    values.set(name, value);
  }
  if (positional.length > 1) throw new Error("Expected one project or save path; quote paths containing spaces");
  if (["plan-patch", "create-patch", "apply-patch"].includes(command) &&
      positional.length !== 0)
    throw new Error(`${command} uses named paths, not a positional project path`);
  if (flags.has("artifact-only") && flags.has("distribution")) throw new Error("--artifact-only and --distribution cannot be combined");
  if (flags.has("artifact-only") && values.has("artifact")) throw new Error("--artifact-only and --artifact cannot be combined");
  if (flags.has("artifact-only") && values.has("archive-shards"))
    throw new Error("--archive-shards requires a distribution build");
  if (values.has("artifact") && values.has("ui-registry")) throw new Error("--ui-registry requires a fresh build; it cannot be combined with --artifact");
  if (values.has("compression") && !["none", "fast", "balanced", "maximum"].includes(values.get("compression")!))
    throw new Error("--compression must be none, fast, balanced, or maximum");
  if (values.has("archive-shards") &&
      (!/^(?:[1-9]|[1-5][0-9]|6[0-4])$/u.test(values.get("archive-shards")!)))
    throw new Error("--archive-shards must be an integer from 1 to 64");
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
  for (const value of values) {
    if (machineOutput) process.stderr.write(`${JSON.stringify(value)}\n`);
    else {
      const at = value.span ? `${value.span.path}:${value.span.start.line}:${value.span.start.column}` : value.path;
      process.stderr.write(`${value.severity} ${value.code}${at ? ` (${at})` : ""}: ${value.message}\n`);
      if (value.hint ?? value.details) process.stderr.write(`  ${value.hint ?? value.details}\n`);
    }
  }
}

function progress(value: Readonly<Record<string, unknown>>): void {
  process.stderr.write(machineOutput ? `${JSON.stringify(value)}\n` : `${String(value.message)}\n`);
}

function buildProgress(phase: string, current: number, total: number | undefined,
  bytes = 0, done = false, reuse?: {readonly files: number; readonly bytes: number}): void {
  const labels: Readonly<Record<string, string>> = {
    load: "Loading project", compile: "Compiling Story and UI",
    copy: "Copying artifact inputs", "verify-existing": "Checking existing output",
    "clone-existing": "Cloning owned artifact",
    "fingerprint-output": "Fingerprinting artifact output",
    "verify-before-publish": "Rechecking existing output",
    "verify-reuse-source": "Checking reused media revision", publish: "Publishing artifact",
  };
  const label = labels[phase] ?? phase;
  const count = total === undefined ? `${current} files` : `${current}/${total}`;
  const amount = bytes > 0 ? `, ${(bytes / 1024 ** 2).toFixed(1)} MiB` : "";
  const reused = reuse?.files ? `, ${reuse.files} reused` : "";
  progress({event: "progress", severity: "information", code: "PXCLI1102", phase,
    current, ...(total === undefined ? {} : {total}), bytes, done,
    ...(reuse === undefined ? {} : {reusedFiles: reuse.files, reusedBytes: reuse.bytes}),
    message: `${label}: ${count}${amount}${reused}${done ? " (done)" : ""}`});
}

function storyProgressReporter(): NonNullable<CompileProjectInput["onStoryProgress"]> {
  const lastHumanReport = {parse: 0, compile: 0};
  return (value) => {
    const {phase, globalCurrent: current, globalTotal: total} = value;
    const step = Math.max(1, Math.ceil(total / 20));
    if (!machineOutput && current !== 1 && current !== total &&
        current - lastHumanReport[phase] < step) return;
    lastHumanReport[phase] = current;
    const label = value.cached ? "Reusing Story cache"
      : phase === "parse" ? "Parsing Story" : "Compiling Story";
    progress({event: "progress", severity: "information", code: "PXCLI1103",
      phase: `story-${phase}`, current, total, done: current === total,
      ...(value.cached === undefined ? {} : {cached: value.cached}),
      locale: value.locale, sceneId: value.sceneId,
      ...(value.sourcePath === undefined ? {} : {sourcePath: value.sourcePath}),
      message: `${label}: ${current}/${total} scenes (${value.locale}: ${value.sceneId})`});
  };
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

function starterProjectId(value: string): string {
  return value.trim().toLowerCase().replace(/[^a-z0-9._-]+/gu, "-")
    .replace(/^[^a-z0-9]+/u, "").replace(/-+$/u, "").slice(0, 256) || "my-prismatix-game";
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
    const result = await createStarter(target, {
      ...(options.values.has("name") ? {name: options.values.get("name")!} : {}),
      ...(options.values.has("id") ? {id: options.values.get("id")!} : {}),
      ...(options.values.has("locale") ? {locale: options.values.get("locale")!} : {}),
      width: integerOption(options, "width", 1280, 1, 16384),
      height: integerOption(options, "height", 720, 1, 16384),
    });
    process.stdout.write(machineOutput ? `${JSON.stringify({created: true, ...result,
      next: [`prismatix dev ${JSON.stringify(target)}`]})}\n` :
      `Created ${result.root}\nEdit Story/main.pxstory, then run: prismatix dev ${JSON.stringify(target)}\n`);
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
  const uiSourceToRuntime = new Map<string, string>();
  for (const [id, value] of Object.entries(authoredEntryPoints)) {
    if (typeof value === "string" && isJsxUiPath(value)) {
      const runtimeSource = await loadJsxDocument(value, "scene");
      runtimeEntryPoints[id] = runtimeSource;
      uiSourceToRuntime.set(value, runtimeSource);
    } else await loadDocument(value);
  }
  // Startup settings may refer to the same TSX source the author put in
  // uiEntryPoints. Publish its compiled route, never a build-time .tsx path.
  const runtimeSplashes = object(raw.settings)?.["player.splashes"];
  if (Array.isArray(runtimeSplashes)) {
    for (const value of runtimeSplashes) {
      const splash = object(value);
      if (typeof splash?.scene !== "string") continue;
      splash.scene = uiSourceToRuntime.get(splash.scene) ?? splash.scene;
    }
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
  for (const path of Object.keys(sourceFiles).filter((value) => value.endsWith(".pxstory"))) {
    const mapPath = `${path}.pxids.json`;
    const absolute = projectFile(root, mapPath);
    if (await regularFile(absolute)) sourceFiles[mapPath] = await readBoundedText(absolute);
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
  const includedResources: Record<string, readonly string[]> = Object.create(null) as Record<string, readonly string[]>;
  const includeRoots = Array.isArray(authoredRaw.resourceIncludes) ? authoredRaw.resourceIncludes : [];
  const canonicalRoot = await realpath(root);
  let includedCount = 0;
  for (const includeRoot of includeRoots) {
    // The SDK reports malformed entries. Never use them to access the filesystem.
    if (typeof includeRoot !== "string" || !safeVirtualPath(includeRoot) ||
        [".prismatix", ".git", "node_modules"].includes(includeRoot.split("/")[0]!.toLowerCase()))
      continue;
    const absolute = projectFile(root, includeRoot);
    dependencies.add(absolute);
    let info: Awaited<ReturnType<typeof lstat>>;
    try { info = await lstat(absolute); }
    catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
      includedResources[includeRoot] = [];
      continue;
    }
    if (info.isSymbolicLink()) throw new Error(`resourceIncludes root is a symbolic link: ${includeRoot}`);
    const real = await realpath(absolute);
    const relativeReal = relative(canonicalRoot, real);
    if (relativeReal === ".." || relativeReal.startsWith(`..${sep}`) || isAbsolute(relativeReal))
      throw new Error(`resourceIncludes root escapes the project: ${includeRoot}`);
    const files: string[] = [];
    const visit = async (directory: string): Promise<void> => {
      for (const entry of await readdir(directory, {withFileTypes: true})) {
        const path = join(directory, entry.name);
        if (entry.isSymbolicLink()) throw new Error(`resourceIncludes contains a symbolic link: ${path}`);
        if (entry.isDirectory()) await visit(path);
        else if (entry.isFile()) {
          files.push(relative(root, path).split(sep).join("/"));
          if (++includedCount > 1_000_000)
            throw new Error("resourceIncludes expands beyond one million files");
        } else throw new Error(`resourceIncludes contains a special file: ${path}`);
      }
    };
    if (info.isDirectory()) await visit(absolute);
    else if (info.isFile()) {
      files.push(includeRoot);
      if (++includedCount > 1_000_000)
        throw new Error("resourceIncludes expands beyond one million files");
    } else throw new Error(`resourceIncludes root is not a regular file or directory: ${includeRoot}`);
    files.sort((left, right) => left.localeCompare(right, "en"));
    includedResources[includeRoot] = files;
    for (const path of files) dependencies.add(projectFile(root, path));
  }
  return {root, projectPath, project, documents, sourceFiles, locales, dependencies, includedResources,
    ...(storyPath === undefined ? {} : {storyPath})};
}

async function loadUiRegistry(override?: string, runtimeHint?: string,
  packagerPath?: string): Promise<UiTypeRegistryMetadata | undefined> {
  const bundled = fileURLToPath(new URL(
    `../../runtime/${process.platform}-${process.arch}/share/PrismatiX/PrismatiXUiTypeRegistry.json`, import.meta.url));
  let path: string | undefined;
  if (override !== undefined) path = resolve(override);
  else if (runtimeHint !== undefined || packagerPath !== undefined) {
    const hint = resolve(runtimeHint ?? packagerPath!);
    const location = runtimeHint === undefined ? dirname(hint)
      : (await stat(hint)).isDirectory() ? hint : dirname(hint);
    const candidates = [join(location, "PrismatiXUiTypeRegistry.json"),
      join(location, "share", "PrismatiX", "PrismatiXUiTypeRegistry.json"),
      join(location, "..", "share", "PrismatiX", "PrismatiXUiTypeRegistry.json")];
    path = candidates.find((candidate) => existsSync(candidate));
    if (path === undefined && runtimeHint !== undefined)
      throw new Error(`UI TypeRegistry manifest was not found in --runtime ${hint}; use --ui-registry or build PrismatiXUiTypeRegistryManifest`);
  } else if (existsSync(bundled)) path = bundled;
  if (path === undefined) return undefined;
  const emitter = join(dirname(path), process.platform === "win32"
    ? "PrismatiXUiTypeManifest.exe" : "PrismatiXUiTypeManifest");
  if (existsSync(emitter) && (await stat(emitter)).mtimeMs > (await stat(path)).mtimeMs + 1000)
    throw new Error(`UI TypeRegistry manifest is older than its native exporter: ${path}; rebuild PrismatiXUiTypeRegistryManifest`);
  const source = object(await readJson(path));
  if (source?.format !== "PrismatiXUiTypeRegistry" || source.schemaRevision !== 2 ||
      !Array.isArray(source.controls) || !source.controls.every((entry) => {
        const control = object(entry);
        return typeof control?.runtimeType === "string" && Array.isArray(control.properties) &&
          control.properties.every((value: unknown) => {
            const property = object(value);
            return typeof property?.id === "string" && typeof property.valueType === "string" &&
              typeof property.writable === "boolean" && typeof property.bindable === "boolean" &&
              typeof property.animatable === "boolean";
          });
      })) throw new Error(`UI TypeRegistry manifest is invalid: ${path}`);
  return source as unknown as UiTypeRegistryMetadata;
}

async function loadInputKeyRegistry(runtimeHint?: string,
  packagerPath?: string): Promise<InputKeyRegistryMetadata | undefined> {
  const filename = "PrismatiXInputKeyRegistry.json";
  const bundled = fileURLToPath(new URL(
    `../../runtime/${process.platform}-${process.arch}/share/PrismatiX/${filename}`, import.meta.url));
  let path: string | undefined;
  let location: string | undefined;
  if (runtimeHint !== undefined || packagerPath !== undefined) {
    const hint = resolve(runtimeHint ?? packagerPath!);
    location = runtimeHint === undefined ? dirname(hint)
      : (await stat(hint)).isDirectory() ? hint : dirname(hint);
    path = [join(location, filename),
      join(location, "share", "PrismatiX", filename),
      join(location, "..", "share", "PrismatiX", filename)]
      .find((candidate) => existsSync(candidate));
    const exporter = join(location, process.platform === "win32"
      ? "PrismatiXInputKeyManifest.exe" : "PrismatiXInputKeyManifest");
    if (path === undefined && existsSync(exporter))
      throw new Error(`Input key manifest was not found beside the selected native Runtime: ${hint}; rebuild PrismatiXInputKeyRegistryManifest`);
  } else if (existsSync(bundled)) path = bundled;
  if (path === undefined) return undefined;
  const emitter = join(location ?? dirname(path), process.platform === "win32"
    ? "PrismatiXInputKeyManifest.exe" : "PrismatiXInputKeyManifest");
  if (existsSync(emitter) && (await stat(emitter)).mtimeMs > (await stat(path)).mtimeMs + 1000)
    throw new Error(`Input key manifest is older than its native exporter: ${path}; rebuild PrismatiXInputKeyRegistryManifest`);
  const source = object(await readJson(path));
  if (source?.format !== "PrismatiXInputKeyRegistry" || source.schemaRevision !== 1 ||
      !Array.isArray(source.keys) || source.keys.length === 0 ||
      !source.keys.every((key: unknown) => typeof key === "string" && key.length > 0) ||
      new Set(source.keys).size !== source.keys.length)
    throw new Error(`Input key manifest is invalid: ${path}`);
  return source as unknown as InputKeyRegistryMetadata;
}

async function compileLoaded(loaded: LoadedProject, committedRevision?: number,
  registryOverride?: string, runtimeHint?: string, packagerPath?: string,
  onStoryProgress?: NonNullable<CompileProjectInput["onStoryProgress"]>,
  storyCompilationCache?: StoryProjectCompilationCache) {
  const uiTypeRegistry = await loadUiRegistry(registryOverride, runtimeHint, packagerPath);
  const inputKeyRegistry = await loadInputKeyRegistry(runtimeHint, packagerPath);
  const result = compileProject({project: loaded.project, documents: loaded.documents,
    sourceFiles: loaded.sourceFiles, locales: loaded.locales,
    projectPath: "project.pxproject",
    ...(loaded.storyPath === undefined ? {} : {storyPath: loaded.storyPath}),
    resourceExists: (path) => {
      try { return existsSync(projectFile(loaded.root, path)); } catch { return false; }
    },
    resourceText: (path) => {
      try { return readFileSync(projectFile(loaded.root, path), "utf8"); }
      catch { return undefined; }
    },
    resolveResourceInclude: (path) => loaded.includedResources[path],
    ...(onStoryProgress === undefined ? {} : {onStoryProgress}),
    ...(storyCompilationCache === undefined ? {} : {storyCompilationCache}),
    ...(committedRevision === undefined ? {} : {committedRevision}),
    ...(uiTypeRegistry === undefined ? {} : {uiTypeRegistry}),
    ...(inputKeyRegistry === undefined ? {} : {inputKeyRegistry})});
  const warnings: AuthoringDiagnostic[] = [];
  if (uiTypeRegistry === undefined &&
      Object.keys(loaded.documents).some((path) => /\.pxui(?:component)?$/u.test(path)))
    warnings.push({severity: "warning", code: "PXCLI1042",
      message: "Native UI property compatibility was not checked; select --runtime or --ui-registry"});
  if (inputKeyRegistry === undefined && Object.values(loaded.documents).some((document) => {
    const game = object(document);
    return game?.format === "PrismatiXGame" &&
      ((Array.isArray(game.inputActions) && game.inputActions.some((entry: unknown) => {
        const keys = object(entry)?.keys;
        return Array.isArray(keys) && keys.length > 0;
      })) ||
      (Array.isArray(game.inputBindings) && game.inputBindings.length > 0));
  })) warnings.push({severity: "warning", code: "PXCLI1043",
    message: "Native input key names were not checked; select a Runtime with PrismatiXInputKeyRegistry.json"});
  return warnings.length === 0 ? result
    : {...result, diagnostics: [...result.diagnostics, ...warnings]};
}

async function validateCommand(options: Options): Promise<number> {
  try {
    const loaded = await loadProject(options.positional[0]);
    const result = await compileLoaded(loaded, undefined, options.values.get("ui-registry"),
      options.values.get("runtime"), undefined, storyProgressReporter());
    printDiagnostics(result.diagnostics);
    if (!result.valid) return 1;
    const includedFiles = new Set(Object.values(loaded.includedResources).flat());
    const projectAssets = result.artifact!.project.assets;
    const catalogIncludeOverlap = (Array.isArray(projectAssets) ? projectAssets : []).filter((asset) => {
      const source = object(asset)?.source;
      return typeof source === "string" && includedFiles.has(source);
    }).length;
    process.stdout.write(`${JSON.stringify({valid: true, engineVersion: "0.2.0",
      project: loaded.projectPath, resourceFiles: result.artifact!.dependencies.length,
      includeRoots: Object.keys(loaded.includedResources).length,
      includedFiles: Object.values(loaded.includedResources).reduce((sum, files) => sum + files.length, 0),
      catalogIncludeOverlap})}\n`);
    return 0;
  } catch (error) {
    if (error instanceof JsxAuthoringFailure) {
      printDiagnostics(error.diagnostics);
      return 1;
    }
    printDiagnostics([diagnostic(
      "PXCLI1001",
      "Project could not be loaded",
      options.positional[0],
      error instanceof Error ? (error.stack ?? String(error)) : String(error),
    )]);
    return 1;
  }
}

interface ReusableArtifactInputs {
  readonly root: string;
  readonly byUri: ReadonlyMap<string, PublishedInput>;
}

async function copyRuntimeInputs(loaded: LoadedProject, artifact: BuildArtifact, staging: string,
  reuse?: ReusableArtifactInputs): Promise<{readonly files: number; readonly bytes: number}> {
  const total = Object.keys(artifact.files).length + artifact.dependencies.length;
  const readyDirectories = new Map<string, Promise<void>>();
  const ensureDirectory = (directory: string): Promise<void> => {
    let ready = readyDirectories.get(directory);
    if (ready === undefined) {
      ready = mkdir(directory, {recursive: true}).then(() => undefined);
      readyDirectories.set(directory, ready);
    }
    return ready;
  };
  let current = 0;
  let bytes = 0;
  let reusedFiles = 0;
  let reusedBytes = 0;
  let lastReport = Date.now();
  let lastCount = 0;
  buildProgress("copy", 0, total);
  const report = (): void => {
    const now = Date.now();
    if (current !== total && current - lastCount < 512 && now - lastReport < 2000) return;
    buildProgress("copy", current, total, bytes, current === total,
      {files: reusedFiles, bytes: reusedBytes});
    lastReport = now;
    lastCount = current;
  };
  for (const [virtualPath, text] of Object.entries(artifact.files)) {
    const destination = projectFile(staging, virtualPath);
    await ensureDirectory(dirname(destination));
    await writeFile(destination, text, "utf8");
    bytes += Buffer.byteLength(text, "utf8");
    current += 1;
    report();
  }
  // A small batch keeps media copies off the one-file-at-a-time path without
  // opening thousands of files at once. Await every task in a failed batch
  // before publication removes staging, so no copy can outlive its directory.
  const copyBatchSize = 8;
  for (let start = 0; start < artifact.dependencies.length; start += copyBatchSize) {
    const batch = artifact.dependencies.slice(start, start + copyBatchSize);
    const results = await Promise.allSettled(batch.map(async (dependency) => {
      const destination = projectFile(staging, dependency.logicalPath);
      await ensureDirectory(dirname(destination));
      const source = projectFile(loaded.root, dependency.source);
      const previous = reuse?.byUri.get(dependency.logicalPath);
      let reused = false;
      if (previous !== undefined) {
        const sourceInfo = await stat(source);
        if (sourceInfo.isFile() && sourceInfo.size === previous.size &&
            await fingerprintFile(source) === previous.fingerprint) {
          const oldPath = projectFile(reuse!.root, dependency.logicalPath);
          try { await link(oldPath, destination); reused = true; }
          catch (error) {
            if (!["EXDEV", "EPERM", "EOPNOTSUPP", "ENOSYS", "ENOENT"].includes(
              (error as NodeJS.ErrnoException).code ?? "")) throw error;
          }
        }
      }
      if (!reused) await copyFile(source, destination);
      const size = reused ? previous!.size : (await stat(destination)).size;
      bytes += size;
      if (reused) { reusedFiles += 1; reusedBytes += size; }
      current += 1;
      report();
    }));
    const failed = results.find((result) => result.status === "rejected");
    if (failed?.status === "rejected") throw failed.reason;
  }
  if (total === 0) buildProgress("copy", 0, 0, 0, true);
  return {files: reusedFiles, bytes: reusedBytes};
}

function protectedInputs(loaded: LoadedProject, artifact: BuildArtifact): string[] {
  // Catalog assets and resourceIncludes appear in both dependency sets. A
  // large VN can otherwise realpath the same tens of thousands of sources
  // twice at each output-safety boundary.
  return [...new Set([loaded.projectPath, ...loaded.dependencies,
    ...["Story", "Assets", "Content", "Scripts", "Extensions"].map((name) => join(loaded.root, name)),
    ...artifact.dependencies.map((dependency) => projectFile(loaded.root, dependency.source))])];
}

async function buildProject(projectArgument: string | undefined, outputArgument: string | undefined,
  committedRevision?: number, registryOverride?: string, runtimeHint?: string,
  packagerPath?: string, storyCompilationCache?: StoryProjectCompilationCache,
  previousBuild?: BuildResult): Promise<BuildResult> {
  buildProgress("load", 0, 1);
  const loaded = await loadProject(projectArgument);
  buildProgress("load", 1, 1, 0, true);
  buildProgress("compile", 0, 1);
  const compiled = await compileLoaded(loaded, committedRevision, registryOverride,
    runtimeHint, packagerPath, storyProgressReporter(), storyCompilationCache);
  printDiagnostics(compiled.diagnostics);
  if (!compiled.valid || compiled.artifact === undefined) throw new Error("Project validation or compilation failed");
  buildProgress("compile", 1, 1, 0, true);
  const output = resolve(outputArgument ?? join(loaded.root, ".prismatix", "build"));
  let reuse: ReusableArtifactInputs | undefined;
  const reuseRoot = previousBuild?.output ?? output;
  if (existsSync(reuseRoot)) {
    try {
      const inputs = await publishedInputs(reuseRoot);
      reuse = {root: reuseRoot,
        byUri: new Map(inputs.map((input) => [input.uri, input]))};
    } catch (error) {
      printDiagnostics([{severity: "warning", code: "PXCLI1610",
        message: "Previous artifact cannot be reused; copying source resources",
        path: reuseRoot, hint: String(error)}]);
    }
  }
  let reused: {readonly files: number; readonly bytes: number} = {files: 0, bytes: 0};
  const protectedPaths = protectedInputs(loaded, compiled.artifact);
  buildProgress("publish", 0, 1);
  await publishDirectory({output, projectRoot: loaded.root,
    verifyExistingBeforeBuild: false,
    beforePromotion: async () => {
      // publishDirectory checks the destination immediately after this hook.
      // When it is also the reuse source, another full hash here would read
      // multi-GiB media twice without strengthening the publication boundary.
      if (reuse !== undefined && reused.files > 0 && resolve(reuse.root) !== output)
        await verifiedPublishedInputs(reuse.root,
          (value) => buildProgress(value.phase, value.files, undefined,
            value.bytes, value.done));
    },
    protectedPaths,
    onProgress: (value) => buildProgress(value.phase, value.files, undefined,
      value.bytes, value.done)}, async (staging) => {
    reused = await copyRuntimeInputs(loaded, compiled.artifact!, staging, reuse);
  });
  buildProgress("publish", 1, 1, 0, true);
  const dependencies = new Set(loaded.dependencies);
  for (const dependency of compiled.artifact.dependencies) {
    try { dependencies.add(projectFile(loaded.root, dependency.source)); }
    catch { /* Compiler paths are advisory. */ }
  }
  return {root: loaded.root, artifact: compiled.artifact, output, dependencies,
    protectedPaths};
}

async function buildCommand(options: Options): Promise<number> {
  if (!options.flags.has("artifact-only")) {
    try {
      const packaged = await packProject(options);
      process.stdout.write(machineOutput ? `${JSON.stringify({built: true, distribution: true, ...packaged})}\n` : `Built playable game: ${packaged.output}\n`);
      return 0;
    } catch (error) {
      printDiagnostics(error instanceof PackagerFailure || error instanceof JsxAuthoringFailure
        ? error.diagnostics : [diagnostic("PXCLI1101", "Build failed", options.positional[0], String(error))]);
      return 1;
    }
  }
  try {
    const built = await buildProject(options.positional[0], options.values.get("output"),
      undefined, options.values.get("ui-registry"), options.values.get("runtime"));
    process.stdout.write(machineOutput ? `${JSON.stringify({built: true, output: built.output,
      runtimeIr: built.artifact.runtimeIrPath, sourceMap: built.artifact.sourceMapPath})}\n` : `Built Runtime artifact: ${built.output}\n`);
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

async function augmentCommand(options: Options): Promise<number> {
  try {
    const artifact = options.values.get("artifact");
    const manifestOption = options.values.get("manifest");
    if (artifact === undefined || manifestOption === undefined)
      throw new Error("augment requires --artifact <published-directory> and --manifest <json-file>");
    const location = await resolveProjectPath(options.positional[0]);
    const manifestPath = resolve(manifestOption);
    const info = await stat(manifestPath);
    if (!info.isFile() || info.size > maxDocumentBytes)
      throw new Error(`Augmentation manifest must be a regular file under ${maxDocumentBytes / 1024 / 1024} MiB`);
    const manifest: unknown = JSON.parse(await readFile(manifestPath, "utf8"));
    const result = await augmentArtifact({projectRoot: location.root,
      artifactRoot: resolve(artifact), manifest, manifestPath,
      onProgress: (value) => buildProgress(value.phase, value.files, undefined,
        value.bytes, value.done)});
    process.stdout.write(machineOutput ? `${JSON.stringify({augmented: true, ...result})}\n`
      : `Augmented artifact: ${result.artifactRoot} (${result.added} added, ${result.replaced} replaced)\n`);
    return 0;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI1801", "Artifact augmentation failed",
      options.values.get("artifact"), String(error))]);
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

async function runProcess(executable: string, args: readonly string[], input?: string,
  cwd?: string, stream = false, onStdoutChunk?: (chunk: string) => void): Promise<{code: number; stdout: string; stderr: string}> {
  return new Promise((accept, reject) => {
    const script = [".js", ".mjs", ".cjs"].includes(extname(executable).toLowerCase());
    const child = spawn(script ? process.execPath : executable,
      script ? [executable, ...args] : [...args],
      {cwd, stdio: ["pipe", "pipe", "pipe"], windowsHide: true});
    let stdout = ""; let stderr = "";
    let outputError: Error | undefined;
    const limit = 16 * 1024 * 1024;
    const capture = (previous: string, value: string, observed = false): string => {
      const combined = previous + value;
      if (combined.length <= limit) return combined;
      if (!stream && !observed) {
        outputError = new Error("Child process output exceeds the 16 MiB capture limit");
        child.kill();
      }
      return combined.slice(-limit);
    };
    const interrupt = (): void => { child.kill(); };
    process.once("SIGINT", interrupt); process.once("SIGTERM", interrupt);
    const cleanup = (): void => {
      process.removeListener("SIGINT", interrupt); process.removeListener("SIGTERM", interrupt);
    };
    child.stdout.setEncoding("utf8"); child.stderr.setEncoding("utf8");
    child.stdout.on("data", (value: string) => {
      stdout = capture(stdout, value, onStdoutChunk !== undefined);
      onStdoutChunk?.(value);
      if (stream && !process.stdout.write(value)) { child.stdout.pause(); process.stdout.once("drain", () => child.stdout.resume()); }
    });
    child.stderr.on("data", (value: string) => {
      stderr = capture(stderr, value);
      if (stream && !process.stderr.write(value)) { child.stderr.pause(); process.stderr.once("drain", () => child.stderr.resume()); }
    });
    child.stdin.on("error", (error) => { outputError = error; child.kill(); });
    child.on("error", (error) => { cleanup(); reject(error); });
    child.on("close", (code) => {
      cleanup();
      if (outputError !== undefined) reject(outputError);
      else accept({code: code ?? 1, stdout, stderr});
    });
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

function packageOutputObserver(activity = "Packaging", streamMachineOutput = true): (chunk: string) => void {
  if (machineOutput && streamMachineOutput) return (chunk) => { process.stdout.write(chunk); };
  let pending = "";
  let skipLongLine = false;
  const reported = new Map<string, {current: number; time: number}>();
  return (chunk) => {
    pending += chunk;
    for (;;) {
      const newline = pending.indexOf("\n");
      if (newline < 0) break;
      const line = pending.slice(0, newline).trim();
      pending = pending.slice(newline + 1);
      if (skipLongLine) { skipLongLine = false; continue; }
      let event: Record<string, unknown>;
      try { event = JSON.parse(line) as Record<string, unknown>; } catch { continue; }
      if (event.event !== "progress" || typeof event.phase !== "string" ||
          typeof event.current !== "number" || typeof event.total !== "number") continue;
      const previous = reported.get(event.phase);
      const now = Date.now();
      if (previous !== undefined && event.current !== 0 && event.current < event.total &&
          event.current - previous.current < 512 && now - previous.time < 2000) continue;
      reported.set(event.phase, {current: event.current, time: now});
      progress({event: "progress", severity: "information", code: "PXCLI1210",
        phase: event.phase, current: event.current, total: event.total,
        message: `${activity} ${event.phase}: ${event.current}/${event.total} ` +
          String(event.message ?? "").slice(0, 240)});
    }
    // Native progress records should be short. Do not retain an unbounded
    // partial line from a malformed or unrelated tool override.
    if (pending.length > 1024 * 1024) { pending = ""; skipLongLine = true; }
  };
}

async function packProject(options: Options): Promise<{output: string; player: string}> {
  let packager: string;
  let player: string;
  try {
    const bundle = await discoverRuntimeBundle(["packager", "player"] as const, runtimeOptions(options));
    packager = bundle.packager.path;
    player = bundle.player.path;
    progress({severity: "information", code: "PXCLI1205",
      message: "Using Runtime tools", packagerSource: bundle.packager.source, playerSource: bundle.player.source});
  } catch (error) {
    if (error instanceof RuntimeDiscoveryFailure) {
      throw new PackagerFailure([discoveryDiagnostic("PXCLI1204",
        `PrismatiX ${error.tool} could not be discovered`, error, options.positional[0])]);
    }
    throw error;
  }
  const temp = await mkdtemp(join(tmpdir(), "prismatix-pack-"));
  try {
    const suppliedArtifact = options.values.get("artifact");
    let root: string;
    let artifactRoot: string;
    let project: ProjectDocument;
    let protectedPaths: string[];
    let startScript = "Runtime/main.pxir";
    let sourceMap = "Runtime/main.pxmap";
    if (suppliedArtifact === undefined) {
      const built = await buildProject(options.positional[0], join(temp, "artifact"),
        undefined, options.values.get("ui-registry"), options.values.get("runtime"),
        packager);
      root = built.root;
      artifactRoot = built.output;
      project = built.artifact.project;
      startScript = built.artifact.runtimeIrPath;
      sourceMap = built.artifact.sourceMapPath;
      protectedPaths = [...built.protectedPaths, artifactRoot];
    } else {
      // A published artifact is already compiled. In particular, do not
      // re-parse a possibly edited author project when distributing it.
      const location = await resolveProjectPath(options.positional[0]);
      root = location.root;
      artifactRoot = resolve(suppliedArtifact);
      const validated = validateDocument<ProjectDocument>("project",
        await readJson(projectFile(artifactRoot, "project.pxproject")),
        "project.pxproject");
      if (!validated.valid || validated.value === undefined)
        throw new PackagerFailure(validated.diagnostics);
      project = validated.value;
      protectedPaths = [location.projectPath, artifactRoot,
        ...["Story", "Assets", "Content", "Scripts", "Extensions"].map((name) => join(root, name))];
    }
    const inputs = await publishedInputs(artifactRoot, (value) => progress({
      event: "progress", severity: "information", code: "PXCLI1212",
      phase: "scan-artifact", current: value.files, bytes: value.bytes, done: value.done,
      message: `Scanning artifact: ${value.files} files, ` +
        `${(value.bytes / 1024 ** 2).toFixed(1)} MiB indexed${value.done ? " (done)" : ""}`,
    }));
    for (const required of ["project.pxproject", startScript, sourceMap]) {
      if (!inputs.some((input) => input.uri === required))
        throw new Error(`Compiled artifact is missing ${required}: ${artifactRoot}`);
    }
    const output = resolve(options.values.get("output") ?? join(root, "dist", project.id));
    const compression = options.values.get("compression") ?? "balanced";
    if (!options.flags.has("ignore-disk-space")) {
      const available = await availableBytesForOutput(output);
      if (available !== undefined) {
        const inputBytes = inputs.reduce((sum, input) => sum + BigInt(input.size), 0n);
        const assessment = assessPackageDiskSpace(inputBytes, available, compression);
        if (assessment !== "sufficient") {
          const message = `Output volume for ${output} has ${formatDiskBytes(available)} free; ` +
            `artifact inputs total ${formatDiskBytes(inputBytes)} ` +
            `(compression: ${compression}).`;
          if (assessment === "insufficient") {
            throw new PackagerFailure([diagnostic("PXCLI1213", message, output,
              "Choose --output on a larger volume, or use --ignore-disk-space if you have verified the package will fit.")]);
          }
          progress({severity: "warning", code: "PXCLI1214",
            message: `Warning: ${message} The final archive may need more or less space; consider a larger output volume.`});
        }
      }
    }
    await publishDirectory({output, projectRoot: root,
      protectedPaths: [...protectedPaths, packager, player],
      onProgress: (value) => progress({event: "progress", severity: "information",
        code: "PXCLI1211", phase: value.phase, current: value.files,
        bytes: value.bytes, done: value.done,
        message: `Checking distribution ${value.phase}: ${value.files} files, ` +
          `${(value.bytes / 1024 ** 2).toFixed(1)} MiB${value.done ? " (done)" : ""}`})}, async (staging) => {
    const request = {
      format: "PrismatiXPackageRequest", schemaRevision: 2,
      requestId: `cli-${process.pid}`, gameId: project.id,
      projectRoot: artifactRoot, outputDir: staging, cacheSourceDir: output,
      retireCacheSourceAfterPromotion: true,
      playerExecutable: player, title: project.name,
      width: project.resolution.width, height: project.resolution.height,
      startScript, sourceMap,
      startRoute: project.entry.ui,
      routes: Object.entries(project.uiEntryPoints).map(([id, scene]) => ({id, scene})),
      contentVersion: project.contentVersion, saveVersion: project.saveVersion,
      graphicsTier: project.graphicsTier ?? "basic",
      saveMigrations: project.saveMigrations ?? [],
      extensions: project.extensions,
      encryption: options.flags.has("encrypt"),
      compression,
      archiveShards: Number(options.values.get("archive-shards") ?? "1"),
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
      undefined, root, false, packageOutputObserver(),
    );
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
    });
    return {output, player: join(output, basename(player))};
  } finally {
    await rm(temp, {recursive: true, force: true});
  }
}

async function packCommand(options: Options): Promise<number> {
  try { const result = await packProject(options); process.stdout.write(machineOutput ? `${JSON.stringify({packed: true, ...result})}\n` : `Packaged playable game: ${result.output}\n`); return 0; }
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
    readonly documentId: string;
    readonly revision: number;
  }>();
  private outputBuffer = "";
  private exited = false;
  private readonly closed: Promise<void>;

  private constructor(private readonly child: ChildProcessWithoutNullStreams) {
    this.closed = new Promise((accept) => child.once("close", () => accept()));
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => {
      this.outputBuffer += chunk;
      if (Buffer.byteLength(this.outputBuffer, "utf8") > maxDocumentBytes) {
        this.outputBuffer = "";
        this.fail(new Error("PreviewHost protocol line exceeds 16 MiB"));
        child.kill();
        return;
      }
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
              if (waiter !== undefined && message.event === undefined &&
                  message.protocol === "PrismatiXPreviewProtocol" && message.schemaRevision === 2 &&
                  message.protocolVersion === 2 && message.sessionId === this.sessionId &&
                  message.documentId === waiter.documentId && message.revision === waiter.revision) {
                clearTimeout(waiter.timer); this.waiters.delete(requestId); waiter.accept(message);
              }
            }
            if (machineOutput && !process.stdout.write(`${line}\n`)) {
              child.stdout.pause(); process.stdout.once("drain", () => child.stdout.resume());
            } else if (!machineOutput && typeof message?.message === "string" && message.code !== undefined) {
              if (!process.stderr.write(`${String(message.code)}: ${message.message}\n`)) {
                child.stdout.pause(); process.stderr.once("drain", () => child.stdout.resume());
              }
            }
          } catch { /* Non-protocol stdout must not contaminate machine output. */ }
        }
        newline = this.outputBuffer.indexOf("\n");
      }
    });
    child.stderr.on("data", (chunk: string) => {
      if (!process.stderr.write(chunk)) { child.stderr.pause(); process.stderr.once("drain", () => child.stderr.resume()); }
    });
    child.stdin.on("error", (error) => this.fail(error));
    child.on("error", (error) => this.fail(error));
    child.on("exit", (code, signal) => {
      this.exited = true;
      const error = new Error(`PreviewHost exited (${code ?? signal ?? "unknown"})`);
      this.fail(error);
    });
  }

  private fail(error: Error): void {
    for (const [id, waiter] of this.waiters) {
      clearTimeout(waiter.timer); waiter.reject(error); this.waiters.delete(id);
    }
  }

  static async start(executable: string, cwd: string): Promise<PreviewClient> {
    const script = [".js", ".mjs", ".cjs"].includes(extname(executable).toLowerCase());
    const child = spawn(script ? process.execPath : executable, script ? [executable] : [],
      {cwd, stdio: ["pipe", "pipe", "pipe"], windowsHide: true});
    await new Promise<void>((accept, reject) => {
      child.once("spawn", accept); child.once("error", reject);
    });
    const client = new PreviewClient(child);
    try {
      const ready = await client.request("hello", "", 0, {});
      if (ready.type !== "ready" || ready.runtimeIrSchemaRevision !== 2 || ready.uiSchemaRevision !== 2)
        throw new Error(`PreviewHost handshake has incompatible IR/UI contracts: ${JSON.stringify(ready)}`);
    } catch (error) {
      await client.close();
      throw error;
    }
    return client;
  }

  get alive(): boolean { return !this.exited && this.child.exitCode === null; }

  request(type: string, documentId: string, revision: number,
    fields: Readonly<Record<string, unknown>>, timeoutMs = 20_000): Promise<PreviewMessage> {
    if (!this.alive) return Promise.reject(new Error("PreviewHost is not running"));
    const requestId = `cli-${randomUUID()}`;
    const request = {protocol: "PrismatiXPreviewProtocol", schemaRevision: 2,
      protocolVersion: 2, type, sessionId: this.sessionId, requestId,
      documentId, revision, ...fields};
    return new Promise((accept, reject) => {
      const timer = setTimeout(() => {
        this.waiters.delete(requestId);
        reject(new Error(`PreviewHost timed out waiting for ${type}`));
      }, timeoutMs);
      this.waiters.set(requestId, {accept, reject, timer, documentId, revision});
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
    if (this.alive) {
      try { await this.request("shutdown", "", 0, {}, 2_000); } catch { /* Force termination below. */ }
      let grace: NodeJS.Timeout | undefined;
      await Promise.race([this.closed, new Promise<void>((accept) => { grace = setTimeout(accept, 2_000); })]);
      if (grace !== undefined) clearTimeout(grace);
      if (this.alive) this.child.kill();
    }
    let timer: NodeJS.Timeout | undefined;
    try {
      await Promise.race([this.closed, new Promise<never>((_accept, reject) => {
        timer = setTimeout(() => reject(new Error("PreviewHost did not exit; its revision is retained")), 5_000);
      })]);
    } finally { if (timer !== undefined) clearTimeout(timer); }
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
  // Native apply includes schema parsing, command lowering and session setup.
  // A fixed 20-second budget rejects valid commercial-scale projects near the
  // one-million-operation contract limit, so grant one additional millisecond
  // per operation while retaining a hard upper bound.
  const applyTimeoutMs = Math.min(300_000,
    20_000 + built.artifact.runtimeIr.operations.length);
  const fields = {projectRoot: built.output, documentId,
    committedRevision: revision, irPath: built.artifact.runtimeIrPath};
  let response = await client.request(incremental ? "patchRuntimeIr" : "applyRuntimeIr",
    documentId, revision, fields, applyTimeoutMs);
  if (incremental && response.type === "resyncRequired") {
    progress({severity: "information", code: "PXCLI1604",
      message: "Preview requested a full resync after a revision gap", reason: response.reason});
    response = await client.request("applyRuntimeIr", documentId, revision, fields, applyTimeoutMs);
  }
  if (response.type !== "runtimeIrApplied" || response.appliedRevision !== revision)
    throw previewResponseError(incremental ? "Runtime patch" : "Runtime apply", response);
  if (response.type === "runtimeIrApplied") {
    progress({severity: "information", code: "PXCLI1603",
      message: incremental ? "Preview updated" : "Preview started",
      applyMode: response.applyMode, revision});
  }
}

function waitFor(milliseconds: number): Promise<void> {
  return new Promise((accept) => setTimeout(accept, milliseconds));
}

async function devCommand(options: Options): Promise<number> {
  let client: PreviewClient | undefined;
  let watcher: ReturnType<typeof watch> | undefined;
  let stopped = false;
  const stop = (): void => { stopped = true; };
  process.once("SIGINT", stop); process.once("SIGTERM", stop);
  try {
    const previewHost = await discoverRuntimeTool("previewHost", runtimeOptions(options));
    const debounce = integerOption(options, "debounce", 150, 50, 10_000);
    const projectArgument = options.positional[0];
    const location = await resolveProjectPath(projectArgument);
    // Subscribe before the first build, including files not yet in a successful
    // dependency graph. Failed imports/assets and atomic saves remain observable.
    let changes = 1;
    watcher = watch(location.root, {recursive: true}, (_event, filename) => {
      const parts = String(filename ?? "").split(/[\\/]/u);
      if ([".prismatix", ".git", "node_modules", "dist"].includes(parts[0] ?? "")) return;
      changes += 1;
    });
    watcher.on("error", (error) => {
      printDiagnostics([diagnostic("PXCLI1607", "Project watcher failed", location.root, String(error))]);
      stopped = true;
    });
    const revisionRoot = join(location.root, ".prismatix", "dev", randomUUID());
    const storyCompilationCache = new StoryProjectCompilationCache();
    let revision = 0;
    let attempted = 0;
    let built: BuildResult | undefined;
    const once = options.flags.has("once") || options.flags.has("smoke");
    const retire = async (obsolete: BuildResult): Promise<void> => {
      try {
        await retirePublishedDirectory({output: obsolete.output, projectRoot: location.root,
          protectedPaths: [...obsolete.dependencies]});
      } catch (error) {
        printDiagnostics([{severity: "warning", code: "PXCLI1608", message: "Unused preview revision retained",
          path: obsolete.output, hint: String(error)}]);
      }
    };
    while (!stopped) {
      if (changes === attempted && client?.alive) { await waitFor(debounce); continue; }
      if (changes === attempted && built === undefined) { await waitFor(debounce); continue; }
      const observed = changes;
      let candidate: PreviewClient | undefined;
      let updated: BuildResult | undefined;
      try {
        updated = changes === attempted && built !== undefined
          ? built : await buildProject(projectArgument, join(revisionRoot, String(++revision)),
            revision, options.values.get("ui-registry"), options.values.get("runtime"),
            undefined, storyCompilationCache, built);
        candidate = await PreviewClient.start(previewHost.path, updated.root);
        await applyPreviewBuild(candidate, updated, false);
        const played = await candidate.request("play", updated.artifact.runtimeIr.documentId,
          updated.artifact.runtimeIr.committedRevision, {showTitle: built === undefined});
        if (played.type !== "playAccepted") throw previewResponseError("Play", played);
        const previous = client;
        const previousBuild = built;
        client = candidate;
        candidate = undefined;
        built = updated;
        if (previous !== undefined) {
          try {
            await previous.close();
            if (previousBuild !== undefined && previousBuild.output !== built.output) await retire(previousBuild);
          } catch (error) {
            printDiagnostics([{severity: "warning", code: "PXCLI1608",
              message: "New preview committed; previous host has not closed and its revision is retained",
              path: previousBuild?.output ?? location.root, hint: String(error)}]);
          }
        }
        progress({severity: "information", code: "PXCLI1602",
          message: "Watching project; successful edits restart the scene after candidate playback is acknowledged",
          revision, output: built.output, previewHost: previewHost.path, discoveredFrom: previewHost.source});
        if (once) return 0;
      } catch (error) {
        // A failed start can also mean its host could not be closed. Retain that
        // revision unless this scope owns a client and confirms its full close.
        if (candidate !== undefined) {
          try {
            await candidate.close();
            if (updated !== undefined && updated.output !== built?.output) await retire(updated);
          } catch (closeError) {
            printDiagnostics([{severity: "warning", code: "PXCLI1608",
              message: "Rejected preview host has not closed; its revision is retained",
              path: updated?.output ?? location.root, hint: String(closeError)}]);
          }
        }
        printDiagnostics(error instanceof JsxAuthoringFailure ? error.diagnostics :
          [diagnostic("PXCLI1606", "Development build or preview failed; previous preview retained",
            projectArgument, String(error))]);
        if (once) return 1;
        // An exited host is retried on a source change, avoiding a crash loop.
        if (client !== undefined && !client.alive) { client = undefined; built = undefined; }
      }
      attempted = observed;
      await waitFor(debounce);
    }
    return 0;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI1600", "Development preview failed", options.positional[0],
      error instanceof RuntimeDiscoveryFailure ? runtimeDiscoveryHint(error) : String(error))]);
    return 1;
  } finally {
    watcher?.close();
    if (client !== undefined) await client.close();
    process.removeListener("SIGINT", stop); process.removeListener("SIGTERM", stop);
    // Keep the last acknowledged revision after shutdown for diagnostics.
    // Superseded revisions are retired only after their host has fully closed.
  }
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

async function syncExternalIdsCommand(collection: Awaited<ReturnType<typeof identitySources>>,
  options: Options): Promise<number> {
  if (options.values.has("prefix"))
    throw new Error("--prefix applies to inline ids, not external identity maps");
  const digest = (source: string) => createHash("sha256").update(source).digest("hex");
  const plans: Array<{value: StoryIdentitySource; mapPath: string; beforeHash: string;
    updatedText?: string; preserved: number; added: number; removed: readonly string[];
    addedOperations: ReturnType<typeof synchronizeStoryIdentityMap>["addedOperations"];
    removedOperations: ReturnType<typeof synchronizeStoryIdentityMap>["removedOperations"]}> = [];
  const diagnostics: AuthoringDiagnostic[] = [];
  for (const value of collection.sources) {
    const mapPath = `${value.absolutePath}.pxids.json`;
    if (!await regularFile(mapPath)) {
      diagnostics.push(diagnostic("PXSTORY1411",
        "Story identity sidecar is missing; use ids --write for a new Story",
        `${value.virtualPath}.pxids.json`));
      continue;
    }
    let source: string;
    let map: unknown;
    try {
      source = await readBoundedText(mapPath);
      map = JSON.parse(source) as unknown;
    } catch (error) {
      diagnostics.push(diagnostic("PXSTORY1403", "Story identity sidecar could not be read",
        `${value.virtualPath}.pxids.json`, String(error)));
      continue;
    }
    const synced = synchronizeStoryIdentityMap(value.source, map,
      {path: value.virtualPath, mapPath: `${value.virtualPath}.pxids.json`});
    if (!synced.valid || synced.map === undefined) {
      diagnostics.push(...synced.diagnostics);
      continue;
    }
    const changed = JSON.stringify(synced.map) !== JSON.stringify(map);
    plans.push({value, mapPath, beforeHash: digest(source),
      ...(changed ? {updatedText: `${JSON.stringify(synced.map)}\n`} : {}),
      preserved: synced.preserved, added: synced.added, removed: synced.removed,
      addedOperations: synced.addedOperations, removedOperations: synced.removedOperations});
  }
  printDiagnostics(diagnostics);
  if (diagnostics.some((item) => item.severity === "error")) {
    process.stdout.write(`${JSON.stringify({sync: true, dryRun: !options.flags.has("write"),
      changed: 0, diagnostics}, null, 2)}\n`);
    return 1;
  }
  const changes = plans.filter((plan) => plan.updatedText !== undefined);
  const write = options.flags.has("write");
  let backup: string | undefined;
  if (write && changes.length > 0) {
    // Preflight the entire batch before modifying any source. A second check
    // immediately before replacement protects against ordinary concurrent edits.
    for (const plan of changes) {
      if (await readBoundedText(plan.value.absolutePath) !== plan.value.source ||
          digest(await readBoundedText(plan.mapPath)) !== plan.beforeHash)
        throw new Error(`Story or identity map changed during sync: ${plan.value.virtualPath}`);
    }
    backup = resolve(options.values.get("backup") ?? join(collection.root, ".prismatix",
      `identity-backup-${new Date().toISOString().replaceAll(":", "-")}-${randomUUID().slice(0, 8)}`));
    for (const plan of changes) {
      const relativePath = relative(collection.root, plan.mapPath);
      if (relativePath === ".." || relativePath.startsWith(`..${sep}`) || isAbsolute(relativePath))
        throw new Error(`Story identity map is outside the project backup root: ${plan.mapPath}`);
      const backupPath = join(backup, relativePath);
      await mkdir(dirname(backupPath), {recursive: true});
      await copyFile(plan.mapPath, backupPath, constants.COPYFILE_EXCL);
    }
    for (const plan of changes) {
      if (await readBoundedText(plan.value.absolutePath) !== plan.value.source ||
          digest(await readBoundedText(plan.mapPath)) !== plan.beforeHash)
        throw new Error(`Story or identity map changed during sync: ${plan.value.virtualPath}`);
      await replaceTextWithBackup(plan.mapPath, plan.updatedText!);
    }
  }
  const result = {sync: true, dryRun: !write, scanned: plans.length,
    changed: changes.length,
    preserved: plans.reduce((sum, plan) => sum + plan.preserved, 0),
    added: plans.reduce((sum, plan) => sum + plan.added, 0),
    removed: plans.flatMap((plan) => plan.removed.map((identity) =>
      ({path: plan.value.virtualPath, identity}))),
    addedOperations: plans.flatMap((plan) => plan.addedOperations.map((operation) =>
      ({path: plan.value.virtualPath, ...operation}))),
    removedOperations: plans.flatMap((plan) => plan.removedOperations.map((operation) =>
      ({path: plan.value.virtualPath, ...operation}))),
    ...(backup === undefined ? {} : {backup}),
    reports: changes.map((plan) => ({path: plan.value.virtualPath,
      preserved: plan.preserved, added: plan.added, removed: plan.removed,
      addedOperations: plan.addedOperations,
      removedOperations: plan.removedOperations})),
  };
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  return options.flags.has("check") && !write && changes.length > 0 ? 1 : 0;
}

async function externalIdsCommand(collection: Awaited<ReturnType<typeof identitySources>>,
  options: Options): Promise<number> {
  const documentId = options.values.get("document-id") ?? collection.documentId;
  if (options.values.has("prefix"))
    throw new Error("--prefix applies to inline generated ids; external maps preserve existing source ids");
  const summarize = (report: ReturnType<typeof stableSourceIdReport>) => ({
    path: report.path, total: report.total, anchored: report.anchored,
    fallback: report.fallback, valid: report.valid,
  });
  const plans: Array<{value: StoryIdentitySource; mapPath: string; previousMap: boolean;
    assigned: number; before: ReturnType<typeof summarize>; after: ReturnType<typeof summarize>}> = [];
  const diagnostics: AuthoringDiagnostic[] = [];
  let invalid = false;
  // Process one source at a time: commercial imports can have hundreds of
  // thousands of operations, and retaining every parsed node/map is wasteful.
  for (const value of collection.sources) {
    const mapPath = `${value.absolutePath}.pxids.json`;
    const previousMap = await regularFile(mapPath);
    if (previousMap) {
      const map = JSON.parse(await readBoundedText(mapPath)) as unknown;
      const applied = applyStoryIdentityMap(parseStory(value.source, value.virtualPath), map,
        `${value.virtualPath}.pxids.json`);
      const report = stableSourceIdReport(applied, {documentId});
      diagnostics.push(...report.diagnostics);
      if (!report.valid) invalid = true;
      plans.push({value, mapPath, previousMap, assigned: 0,
        before: summarize(report), after: summarize(report)});
      continue;
    }
    const before = stableSourceIdReport(value.source, {path: value.virtualPath, documentId});
    const converted = externalizeStoryIds(value.source,
      {path: value.virtualPath, documentId});
    const after = converted.map === undefined || converted.source === undefined
      ? before : stableSourceIdReport(applyStoryIdentityMap(
        parseStory(converted.source, value.virtualPath), converted.map), {documentId});
    diagnostics.push(...converted.diagnostics, ...after.diagnostics);
    if (!converted.valid || !after.valid) invalid = true;
    plans.push({value, mapPath, previousMap, assigned: converted.assigned,
      before: summarize(before), after: summarize(after)});
  }
  printDiagnostics(diagnostics);
  if (invalid) {
    process.stdout.write(`${JSON.stringify({external: true, dryRun: !options.flags.has("write"),
      assigned: 0, diagnostics}, null, 2)}\n`);
    return 1;
  }
  const write = options.flags.has("write");
  let backup: string | undefined;
  if (write && plans.some((plan) => !plan.previousMap)) {
    backup = resolve(options.values.get("backup") ?? join(collection.root, ".prismatix",
      `identity-backup-${new Date().toISOString().replaceAll(":", "-")}`));
    for (const plan of plans) {
      if (plan.previousMap) continue;
      if (await readBoundedText(plan.value.absolutePath) !== plan.value.source)
        throw new Error(`Story source changed during identity map preparation: ${plan.value.absolutePath}`);
      const relativePath = relative(collection.root, plan.value.absolutePath);
      if (relativePath === ".." || relativePath.startsWith(`..${sep}`) || isAbsolute(relativePath))
        throw new Error(`Story source is outside the project backup root: ${plan.value.absolutePath}`);
      const backupPath = join(backup, relativePath);
      await mkdir(dirname(backupPath), {recursive: true});
      await copyFile(plan.value.absolutePath, backupPath);
    }
    for (const plan of plans) {
      if (plan.previousMap) continue;
      const converted = externalizeStoryIds(plan.value.source,
        {path: plan.value.virtualPath, documentId});
      if (!converted.valid || converted.map === undefined || converted.source === undefined)
        throw new Error(`Story identity map changed during preparation: ${plan.value.virtualPath}`);
      await writeFile(plan.mapPath, `${JSON.stringify(converted.map)}\n`, {encoding: "utf8", flag: "wx"});
      try {
        if (converted.source !== plan.value.source)
          await replaceTextWithBackup(plan.value.absolutePath, converted.source);
      } catch (error) {
        // Only remove the sidecar created by this invocation. The original
        // source and independent backup remain recoverable.
        await rm(plan.mapPath, {force: true});
        throw error;
      }
    }
  }
  const reports = plans.map((plan) => write ? plan.after : plan.before);
  const result = {external: true, dryRun: !write,
    assigned: plans.reduce((sum, plan) => sum + plan.assigned, 0),
    total: reports.reduce((sum, report) => sum + report.total, 0),
    anchored: reports.reduce((sum, report) => sum + report.anchored, 0),
    fallback: reports.reduce((sum, report) => sum + report.fallback, 0),
    ...(backup === undefined ? {} : {backup}), reports};
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  return options.flags.has("check") && result.fallback > 0 ? 1 : 0;
}

async function idsCommand(options: Options): Promise<number> {
  try {
    if (options.flags.has("inline") && options.flags.has("external"))
      throw new Error("--inline and --external are mutually exclusive");
    if (options.flags.has("inline") && options.flags.has("sync"))
      throw new Error("--sync updates external identity maps; omit --inline");
    const collection = await identitySources(options.positional[0]);
    if (options.flags.has("sync")) return syncExternalIdsCommand(collection, options);
    const mapsExist = (await Promise.all(collection.sources.map((value) =>
      regularFile(`${value.absolutePath}.pxids.json`)))).some(Boolean);
    if (options.flags.has("inline") && mapsExist)
      throw new Error("This project has external Story identity maps; omit --inline");
    if (options.flags.has("inline") && collection.sources.some((value) =>
      storyRequiresIdentityMap(value.source)))
      throw new Error("A Story requires a missing external identity map; restore its sidecar before changing identity mode");
    if (!options.flags.has("inline")) {
      return externalIdsCommand(collection, options);
    }
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
    if (options.positional[0] === undefined) throw new Error("inspect-save requires a save file path");
    const packagePath = options.values.get("package");
    const secret = options.values.get("secret");
    const result = await inspectSaveWithRuntime({path, json: machineOutput,
      runtime: runtimeOptions(options),
      ...(packagePath === undefined ? {} : {packagePath}),
      ...(secret === undefined ? {} : {secret})});
    process.stdout.write(result.output);
    return 0;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI1401", "Save inspection failed", path,
      error instanceof RuntimeDiscoveryFailure ? runtimeDiscoveryHint(error) : String(error))]);
    return 1;
  }
}
function usage(): void {
  process.stderr.write("Usage: prismatix <create|dev|validate|build|pack|run|augment|ids|migrate|inspect-save|plan-patch|verify-package|create-patch|apply-patch> [project-or-save-or-distribution] [options]\n");
}

async function verifyPackagedContents(directory: string, options: Options): Promise<{
  readonly archives: number; readonly resources: number; readonly rawBytes: number;
}> {
  const packager = await discoverRuntimeTool("packager", runtimeOptions(options));
  const result = await runProcess(packager.path, ["--verify-package", resolve(directory)],
    undefined, undefined, false, packageOutputObserver("Verifying", false));
  const events = packageEvents(result.stdout);
  const failed = failedPackageDiagnostics(events);
  if (failed.length > 0) throw new PackagerFailure(failed);
  const verified = events.find((event) => event.event === "verified");
  if (result.code !== 0 || verified === undefined ||
      !Number.isSafeInteger(verified.archives) ||
      !Number.isSafeInteger(verified.resources) ||
      !Number.isSafeInteger(verified.rawBytes) ||
      Number(verified.archives) < 1 || Number(verified.resources) < 1 ||
      Number(verified.rawBytes) < 0)
    throw new Error(`Native package verification failed or returned an invalid result: ${result.stderr || result.stdout}`);
  return {archives: Number(verified.archives), resources: Number(verified.resources),
    rawBytes: Number(verified.rawBytes)};
}

async function verifyPackageCommand(options: Options): Promise<number> {
  const directory = options.positional[0];
  if (directory === undefined) {
    printDiagnostics([diagnostic("PXCLI1503", "verify-package requires a distribution path")]);
    return 2;
  }
  try {
    const verified = await verifyPackagedContents(directory, options);
    process.stdout.write(machineOutput
      ? `${JSON.stringify({verified: true, distribution: resolve(directory), ...verified})}\n`
      : `Verified ${verified.resources} resources in ${verified.archives} PDX archive(s) (${verified.rawBytes} raw bytes).\n`);
    return 0;
  } catch (error) {
    printDiagnostics(error instanceof PackagerFailure ? error.diagnostics :
      error instanceof RuntimeDiscoveryFailure ? [discoveryDiagnostic("PXCLI1504",
        "Packager required for package verification was not found", error, directory)] :
      [diagnostic("PXCLI1504", "Package content verification failed", directory, String(error))]);
    return 1;
  }
}

async function planPatchCommand(options: Options): Promise<number> {
  const base = options.values.get("base");
  const target = options.values.get("target");
  if (base === undefined || target === undefined) {
    printDiagnostics([diagnostic("PXCLI1501",
      "plan-patch requires --base <old-distribution> and --target <new-distribution>")]);
    return 2;
  }
  try {
    if (options.flags.has("verify-contents")) {
      await verifyPackagedContents(base, options);
      if (resolve(base) !== resolve(target))
        await verifyPackagedContents(target, options);
    }
    const plan = await planPackagePatch(base, target);
    if (machineOutput) {
      process.stdout.write(`${JSON.stringify({patchPlan: plan, patchWritten: false,
        contentsVerified: options.flags.has("verify-contents")})}\n`);
    } else {
      process.stdout.write(`Patch plan ${plan.baseContentVersion} → ${plan.targetContentVersion}: ` +
        `${plan.added.length} added, ${plan.modified.length} modified, ` +
        `${plan.removed.length} removed, ${plan.unchangedCount} unchanged; ` +
        `${plan.affectedTargetArchives.length} target archive(s) affected, ` +
        `${plan.changedRawBytes} changed raw bytes. ` +
        `${options.flags.has("verify-contents") ? "PDX contents verified. " : ""}No patch was written.\n`);
    }
    return 0;
  } catch (error) {
    printDiagnostics(error instanceof PackagerFailure ? error.diagnostics :
      error instanceof RuntimeDiscoveryFailure ? [discoveryDiagnostic("PXCLI1502",
        "Packager required for content verification was not found", error)] :
      [diagnostic("PXCLI1502", "Package patch plan failed", undefined, String(error))]);
    return 1;
  }
}

async function createPatchCommand(options: Options): Promise<number> {
  const base = options.values.get("base");
  const target = options.values.get("target");
  const output = options.values.get("output");
  if (base === undefined || target === undefined || output === undefined) {
    printDiagnostics([diagnostic("PXCLI1511",
      "create-patch requires --base, --target, and --output")]);
    return 2;
  }
  try {
    const report = await createDistributionPatch(base, target, output,
      (directory) => verifyPackagedContents(directory, options), patchProgress);
    process.stdout.write(machineOutput
      ? `${JSON.stringify({patchCreated: true, ...report})}\n`
      : `Created distribution patch: ${report.output} ` +
        `(${report.changedFiles} changed files, ${report.removedFiles} removed, ` +
        `${report.payloadBytes} payload bytes).\n`);
    return 0;
  } catch (error) {
    printDiagnostics(error instanceof PackagerFailure ? error.diagnostics :
      error instanceof RuntimeDiscoveryFailure ? [discoveryDiagnostic("PXCLI1512",
        "Packager required to verify patch inputs was not found", error)] :
      [diagnostic("PXCLI1512", "Distribution patch creation failed", output, String(error))]);
    return 1;
  }
}

async function applyPatchCommand(options: Options): Promise<number> {
  const base = options.values.get("base");
  const patch = options.values.get("patch");
  const inPlace = options.flags.has("in-place");
  const output = options.values.get("output") ?? (inPlace ? base : undefined);
  if (base === undefined || patch === undefined || output === undefined) {
    printDiagnostics([diagnostic("PXCLI1513",
      "apply-patch requires --base, --patch, and --output (or --in-place)")]);
    return 2;
  }
  try {
    const report = await applyDistributionPatch(base, patch, output, inPlace,
      (directory) => verifyPackagedContents(directory, options), patchProgress);
    process.stdout.write(machineOutput
      ? `${JSON.stringify({patchApplied: true, ...report})}\n`
      : `Applied distribution patch: ${report.output} ` +
        `(${report.changedFiles} changed files, ${report.removedFiles} removed).\n`);
    return 0;
  } catch (error) {
    printDiagnostics(error instanceof PackagerFailure ? error.diagnostics :
      error instanceof RuntimeDiscoveryFailure ? [discoveryDiagnostic("PXCLI1514",
        "Packager required to verify patched output was not found", error)] :
      [diagnostic("PXCLI1514", "Distribution patch installation failed", output, String(error))]);
    return 1;
  }
}

function patchProgress(value: DistributionPatchProgress): void {
  progress({event: "progress", severity: "information", code: "PXCLI1510",
    phase: value.phase, current: value.files, bytes: value.bytes,
    done: value.done,
    ...(value.linkedFiles === undefined ? {} :
      {linkedFiles: value.linkedFiles, linkedBytes: value.linkedBytes}),
    message: `${value.phase}: ${value.files} files, ` +
      `${(value.bytes / 1024 ** 2).toFixed(1)} MiB` +
      (value.linkedFiles === undefined ? "" :
        ` (${value.linkedFiles} linked, ${((value.linkedBytes ?? 0) / 1024 ** 2).toFixed(1)} MiB)`) +
      `${value.done ? " (done)" : ""}`});
}

async function projectTool(command: string, options: Options, args: string[]): Promise<number | undefined> {
  if (launcherStopRequested) return 130;
  if (["create", "inspect-save", "plan-patch", "verify-package",
    "create-patch", "apply-patch"].includes(command)) return undefined;
  const location = await resolveProjectPath(options.positional[0]);
  const manifestPath = join(location.root, "package.json");
  if (!await regularFile(manifestPath)) return launcherStopRequested ? 130 : undefined;
  const manifest = object(await readJson(manifestPath));
  const pinned = object(manifest?.devDependencies)?.["@prismatix/cli"] ?? object(manifest?.dependencies)?.["@prismatix/cli"];
  if (pinned === undefined) return launcherStopRequested ? 130 : undefined; // Existing pre-lock projects retain their public API.
  const exactVersion = typeof pinned === "string" && /^\d+\.\d+\.\d+$/u.test(pinned);
  const localPackage = typeof pinned === "string" && pinned.startsWith("file:");
  if (!exactVersion && !localPackage)
    throw new Error("Project @prismatix/cli must use an exact version or a local file: dependency, not a range");
  const localRoot = join(location.root, "node_modules", "@prismatix", "cli");
  const entry = join(localRoot, "dist", "src", "cli.js");
  if (!await regularFile(entry)) throw new Error("The project's pinned CLI is missing. Restore dependencies with npm ci in the project directory.");
  const installed = object(await readJson(join(localRoot, "package.json")));
  if (exactVersion && installed?.version !== pinned)
    throw new Error(`Project requires CLI ${pinned}; installed version is ${String(installed?.version)}. Restore the lock with npm ci.`);
  const isLocalTool = await realpath(entry) === await realpath(fileURLToPath(import.meta.url));
  if (launcherStopRequested) return 130;
  if (isLocalTool) return undefined;
  // Directly inherit stdio: the local CLI owns its protocol and long-lived preview.
  return new Promise<number>((accept, reject) => {
    const child = spawn(process.execPath, [entry, command, ...args], {
      stdio: ["inherit", "inherit", "inherit", "ipc"], windowsHide: true});
    let stopping = false;
    let escalation: NodeJS.Timeout | undefined;
    const stop = (): void => {
      if (stopping) return;
      stopping = true;
      // Windows kill() terminates Node without running its finally blocks.
      // Let the pinned CLI shut down its PreviewHost first through IPC.
      if (child.connected) child.send({type: "prismatix-launcher-shutdown"}, () => {});
      escalation = setTimeout(() => { child.kill(); }, 35_000);
    };
    process.once("SIGINT", stop); process.once("SIGTERM", stop);
    const cleanup = (): void => {
      if (escalation) clearTimeout(escalation);
      process.removeListener("SIGINT", stop); process.removeListener("SIGTERM", stop);
    };
    child.once("error", (error) => { cleanup(); reject(error); });
    child.once("close", (code) => { cleanup(); accept(code ?? 1); });
  });
}

async function main(): Promise<number> {
  const [command, ...args] = process.argv.slice(2);
  if (command === undefined || command === "--help") { usage(); return 0; }
  let options: Options;
  try { options = parseOptions(command, args); }
  catch (error) { printDiagnostics([diagnostic("PXCLI0001", "Invalid command arguments", undefined, String(error))]); return 2; }
  if (options.flags.has("help")) { usage(); return 0; }
  try {
    const delegated = await projectTool(command, options, args);
    if (delegated !== undefined) return delegated;
  } catch (error) {
    printDiagnostics([diagnostic("PXCLI0002", "Project tool preparation failed", options.positional[0], String(error))]);
    return 1;
  }
  if (command === "create") return createCommand(options);
  if (command === "dev") return devCommand(options);
  if (command === "validate") return validateCommand(options);
  if (command === "build") return buildCommand(options);
  if (command === "pack") return packCommand(options);
  if (command === "run") return runCommand(options);
  if (command === "augment") return augmentCommand(options);
  if (command === "ids") return idsCommand(options);
  if (command === "migrate") return migrateCommand(options);
  if (command === "inspect-save") return inspectSaveCommand(options);
  if (command === "plan-patch") return planPatchCommand(options);
  if (command === "verify-package") return verifyPackageCommand(options);
  if (command === "create-patch") return createPatchCommand(options);
  if (command === "apply-patch") return applyPatchCommand(options);
  usage(); return 2;
}

// A delegated CLI must not survive a crashed/terminated global launcher. IPC
// closure works on Windows too, where child.kill does not deliver JS SIGTERM.
let launcherStopRequested = false;
const launcherDisconnected = (): void => {
  if (launcherStopRequested) return;
  launcherStopRequested = true;
  process.emit("SIGTERM");
};
const launcherMessage = (message: unknown): void => {
  if (object(message)?.type === "prismatix-launcher-shutdown") launcherDisconnected();
};
if (process.send !== undefined) {
  process.once("disconnect", launcherDisconnected);
  process.on("message", launcherMessage);
  if (!process.connected) launcherDisconnected();
}
process.exitCode = await main();
process.removeListener("disconnect", launcherDisconnected);
process.removeListener("message", launcherMessage);
if (process.connected) process.disconnect?.();
