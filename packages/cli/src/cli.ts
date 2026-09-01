#!/usr/bin/env node

import {createDecipheriv, createHash} from "node:crypto";
import {spawn} from "node:child_process";
import {
  copyFile, mkdir, mkdtemp, readFile, readdir, rename, rm, stat, writeFile,
} from "node:fs/promises";
import {tmpdir} from "node:os";
import {basename, dirname, extname, isAbsolute, join, relative, resolve, sep} from "node:path";
import {pathToFileURL} from "node:url";
import * as ts from "typescript-compiler";

import {
  canonicalJson,
  compileJsxUi,
  compileProject,
  migrateDocument,
  validateDocument,
  type AuthoringDiagnostic,
  type BuildArtifact,
  type ProjectDocument,
  type SourceSpan,
} from "@prismatix/authoring-sdk";

interface Options {readonly values: Map<string, string>; readonly flags: Set<string>; readonly positional: string[]}
interface LoadedProject {readonly root: string; readonly projectPath: string; readonly project: unknown; readonly documents: Record<string, unknown>; readonly sourceFiles: Record<string, string>; readonly locales: Record<string, unknown>}
interface BuildResult {readonly root: string; readonly artifact: BuildArtifact; readonly output: string}
interface MigrationChange {readonly path: string; readonly before: string; readonly after: string; readonly reason: string}

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

function jsonPointerSegment(value: string): string {
  return value.replaceAll("~", "~0").replaceAll("/", "~1");
}

// JSON.parse intentionally exposes no syntax tree. Migration diagnostics still
// need exact locations, so index the already-bounded JSON with a small strict
// scanner and retain the complete value range for every JSON Pointer.
function indexJsonRanges(text: string): ReadonlyMap<string, TextRange> {
  const ranges = new Map<string, TextRange>();
  let cursor = 0;
  const whitespace = (): void => {
    while (cursor < text.length && /\s/u.test(text[cursor]!)) cursor += 1;
  };
  const stringToken = (): {value: string; range: TextRange} => {
    whitespace();
    const start = cursor;
    if (text[cursor] !== '"') throw new SyntaxError("Expected a JSON string");
    cursor += 1;
    while (cursor < text.length) {
      if (text[cursor] === "\\") { cursor += 2; continue; }
      if (text[cursor] === '"') { cursor += 1; break; }
      cursor += 1;
    }
    const range = {start, end: cursor};
    return {value: JSON.parse(text.slice(start, cursor)) as string, range};
  };
  const value = (pointer: string): void => {
    whitespace();
    const start = cursor;
    if (text[cursor] === "{") {
      cursor += 1; whitespace();
      while (cursor < text.length && text[cursor] !== "}") {
        const key = stringToken().value;
        whitespace();
        if (text[cursor] !== ":") throw new SyntaxError("Expected ':'");
        cursor += 1;
        value(`${pointer}/${jsonPointerSegment(key)}`);
        whitespace();
        if (text[cursor] !== ",") break;
        cursor += 1; whitespace();
      }
      if (text[cursor] === "}") cursor += 1;
    } else if (text[cursor] === "[") {
      cursor += 1; whitespace();
      let index = 0;
      while (cursor < text.length && text[cursor] !== "]") {
        value(`${pointer}/${index}`); index += 1; whitespace();
        if (text[cursor] !== ",") break;
        cursor += 1; whitespace();
      }
      if (text[cursor] === "]") cursor += 1;
    } else if (text[cursor] === '"') {
      stringToken();
    } else {
      while (cursor < text.length && !/[\s,\]}]/u.test(text[cursor]!)) cursor += 1;
    }
    ranges.set(pointer, {start, end: cursor});
  };
  try { value(""); } catch { /* JSON.parse reports syntax errors separately. */ }
  return ranges;
}

function pointerFromDiagnostic(value: AuthoringDiagnostic): string {
  const match = /^(\/[^:]*|\/):/u.exec(value.details ?? "");
  return match?.[1] === "/" || match?.[1] === undefined ? "" : match[1];
}

function diagnosticsWithSourceSpans(values: readonly AuthoringDiagnostic[],
                                    text: string, path: string): AuthoringDiagnostic[] {
  const ranges = indexJsonRanges(text);
  return values.map((value) => {
    if (value.span !== undefined) return value;
    let pointer = pointerFromDiagnostic(value);
    while (!ranges.has(pointer) && pointer.includes("/"))
      pointer = pointer.slice(0, pointer.lastIndexOf("/"));
    const range = ranges.get(pointer) ?? {start: 0, end: text.length};
    return {...value, path: value.path ?? path, span: sourceSpan(text, path, range)};
  });
}

function firstLineSpan(text: string, path: string): SourceSpan {
  const start = text.search(/\S/u);
  const boundedStart = start < 0 ? 0 : start;
  const newline = text.indexOf("\n", boundedStart);
  return sourceSpan(text, path, {start: boundedStart,
    end: newline < 0 ? text.length : newline});
}

function jsonParseFailureSpan(text: string, path: string, error: unknown): SourceSpan {
  const match = /position\s+(\d+)/iu.exec(String(error));
  const start = match === null ? Math.max(0, text.length - 1) : Number(match[1]);
  return sourceSpan(text, path, {start, end: Math.min(text.length, start + 1)});
}

type LegacyTypedKind = "pxproject" | "pxresource" | "pxscene" | "pxmeta";
type LegacyTypedSpecialKind = "vec2" | "rect" | "color" | "uuid" | "resource" | "token";
interface LegacyTypedSpecial {readonly typed: LegacyTypedSpecialKind; readonly values: readonly unknown[]}
interface LegacyTypedNode {
  readonly id: string; readonly parentId: string | null; readonly name: string;
  readonly type: string; readonly properties: Record<string, unknown>;
  readonly propertyRanges: Map<string, TextRange>; readonly range: TextRange;
}
interface LegacyTypedDocument {
  readonly kind: LegacyTypedKind; readonly version: number; readonly id: string;
  readonly type: string; readonly properties: Record<string, unknown>;
  readonly propertyRanges: Map<string, TextRange>;
  readonly nodes: readonly LegacyTypedNode[]; readonly headerRange: TextRange;
}

class LegacyTypedError extends Error {
  constructor(message: string, readonly range: TextRange) { super(message); }
}

function splitLegacyArguments(body: string): string[] {
  if (body.trim().length === 0) return [];
  const values: string[] = [];
  let start = 0; let depth = 0; let quoted = false; let escaped = false;
  for (let index = 0; index <= body.length; index += 1) {
    const character = body[index];
    if (index < body.length && quoted) {
      if (escaped) escaped = false;
      else if (character === "\\") escaped = true;
      else if (character === '"') quoted = false;
    } else if (index < body.length) {
      if (character === '"') quoted = true;
      else if (character === "(") depth += 1;
      else if (character === ")") depth -= 1;
    }
    if (depth < 0) throw new SyntaxError("Unbalanced typed value");
    if (index === body.length || (character === "," && depth === 0 && !quoted)) {
      values.push(body.slice(start, index).trim()); start = index + 1;
    }
  }
  if (quoted || depth !== 0 || values.some((value) => value.length === 0))
    throw new SyntaxError("Malformed typed value arguments");
  return values;
}

function legacyQuoted(value: string): string {
  const parsed = JSON.parse(value) as unknown;
  if (typeof parsed !== "string") throw new SyntaxError("Expected a quoted string");
  return parsed;
}

function parseLegacyValue(source: string): unknown {
  const value = source.trim();
  if (value === "null") return null;
  if (value === "true") return true;
  if (value === "false") return false;
  if (value.startsWith('"')) return legacyQuoted(value);
  if (/^-?(?:0|[1-9]\d*)$/u.test(value)) return Number(value);
  if (/^-?(?:0|[1-9]\d*)\.\d+(?:[eE][+-]?\d+)?$/u.test(value) ||
      /^-?(?:0|[1-9]\d*)[eE][+-]?\d+$/u.test(value)) {
    const number = Number(value);
    if (Number.isFinite(number)) return number;
  }
  const call = /^([a-z][A-Za-z0-9]*)\(([\s\S]*)\)$/u.exec(value);
  if (call === null) throw new SyntaxError("Unsupported typed value");
  const name = call[1]!; const arguments_ = splitLegacyArguments(call[2]!);
  if (name === "array") return arguments_.map(parseLegacyValue);
  if (name === "object") {
    if (arguments_.length % 2 !== 0) throw new SyntaxError("object() requires key/value pairs");
    const result: Record<string, unknown> = {};
    for (let index = 0; index < arguments_.length; index += 2) {
      const key = legacyQuoted(arguments_[index]!);
      if (Object.hasOwn(result, key)) throw new SyntaxError(`Duplicate object key: ${key}`);
      result[key] = parseLegacyValue(arguments_[index + 1]!);
    }
    return result;
  }
  if (name === "vec2" || name === "rect" || name === "color") {
    const expected = name === "vec2" ? 2 : name === "rect" ? 4 : undefined;
    const values = arguments_.map((argument) => Number(argument));
    if ((expected !== undefined && values.length !== expected) ||
        (name === "color" && values.length !== 3 && values.length !== 4) ||
        values.some((number) => !Number.isFinite(number)))
      throw new SyntaxError(`${name}() has invalid numeric arguments`);
    return {typed: name, values} satisfies LegacyTypedSpecial;
  }
  if (name === "uuid" || name === "token") {
    if (arguments_.length !== 1) throw new SyntaxError(`${name}() requires one argument`);
    return {typed: name, values: [legacyQuoted(arguments_[0]!)]} satisfies LegacyTypedSpecial;
  }
  if (name === "res") {
    if (arguments_.length !== 2) throw new SyntaxError("res() requires UUID and path");
    return {typed: "resource", values: arguments_.map(legacyQuoted)} satisfies LegacyTypedSpecial;
  }
  throw new SyntaxError(`Unsupported typed constructor: ${name}`);
}

function legacySpecial(value: unknown, kind?: LegacyTypedSpecialKind): LegacyTypedSpecial | undefined {
  const record = object(value);
  if (record === undefined || typeof record.typed !== "string" || !Array.isArray(record.values)) return undefined;
  const result = value as LegacyTypedSpecial;
  return kind === undefined || result.typed === kind ? result : undefined;
}

function plainLegacyValue(value: unknown): unknown {
  const special = legacySpecial(value);
  if (special !== undefined) {
    if (special.typed === "uuid" || special.typed === "token") return special.values[0];
    if (special.typed === "resource") return {id: special.values[0], path: special.values[1]};
    if (special.typed === "vec2") return {x: special.values[0], y: special.values[1]};
    if (special.typed === "rect")
      return {x: special.values[0], y: special.values[1], width: special.values[2], height: special.values[3]};
    const channels = special.values.map((channel) => Math.max(0, Math.min(255, Number(channel))));
    if (channels.length === 3) channels.push(255);
    return `#${channels.map((channel) => Math.round(channel).toString(16).padStart(2, "0")).join("")}`;
  }
  if (Array.isArray(value)) return value.map(plainLegacyValue);
  const record = object(value);
  if (record === undefined) return value;
  return Object.fromEntries(Object.entries(record).map(([key, item]) => [key, plainLegacyValue(item)]));
}

function parseLegacyTypedDocument(text: string): LegacyTypedDocument {
  const properties: Record<string, unknown> = {}; const propertyRanges = new Map<string, TextRange>();
  const nodes: LegacyTypedNode[] = []; let active: LegacyTypedNode | undefined;
  let header: Omit<LegacyTypedDocument, "properties" | "propertyRanges" | "nodes"> | undefined;
  const lines = [...text.matchAll(/[^\r\n]*(?:\r\n|\n|\r|$)/gu)];
  for (const match of lines) {
    const raw = match[0]!.replace(/[\r\n]+$/u, "");
    if (raw.length === 0 && match.index === text.length) continue;
    const leading = /^\s*/u.exec(raw)?.[0].length ?? 0;
    const current = raw.trim(); const start = (match.index ?? 0) + leading;
    const range = {start, end: (match.index ?? 0) + raw.length};
    if (current.length === 0 || current.startsWith("#")) continue;
    if (header === undefined) {
      const parsed = /^@(pxproject|pxresource|pxscene|pxmeta)\s+(\d+)\s+([0-9a-fA-F-]{36})(?:\s+(.+))?$/u.exec(current);
      if (parsed === null || Number(parsed[2]) < 1 || Number(parsed[2]) > 4)
        throw new LegacyTypedError("Unsupported TypedDocument header", range);
      header = {kind: parsed[1] as LegacyTypedKind, version: Number(parsed[2]),
        id: parsed[3]!, type: parsed[4]?.trim() ?? "", headerRange: range};
      continue;
    }
    if (current.startsWith("[node ") && current.endsWith("]")) {
      try {
        const arguments_ = splitLegacyArguments(current.slice(6, -1));
        if (arguments_.length !== 4) throw new SyntaxError("Node requires id, parent, name, and type");
        const id = legacyQuoted(arguments_[0]!); const parent = legacyQuoted(arguments_[1]!);
        active = {id, parentId: parent.length === 0 ? null : parent,
          name: legacyQuoted(arguments_[2]!), type: legacyQuoted(arguments_[3]!),
          properties: {}, propertyRanges: new Map(), range};
        nodes.push(active); continue;
      } catch (error) { throw new LegacyTypedError(String(error), range); }
    }
    const equals = current.indexOf("=");
    if (equals <= 0) throw new LegacyTypedError("Expected a TypedDocument property assignment", range);
    const key = current.slice(0, equals).trim(); const rawValue = current.slice(equals + 1).trim();
    const valueStart = start + current.indexOf(rawValue, equals + 1);
    const valueRange = {start: valueStart, end: valueStart + rawValue.length};
    const target = active?.properties ?? properties;
    const ranges = active?.propertyRanges ?? propertyRanges;
    if (Object.hasOwn(target, key)) throw new LegacyTypedError(`Duplicate property: ${key}`, valueRange);
    try { target[key] = parseLegacyValue(rawValue); }
    catch (error) { throw new LegacyTypedError(String(error), valueRange); }
    ranges.set(key, valueRange);
  }
  if (header === undefined) throw new LegacyTypedError("TypedDocument header is missing", {start: 0, end: Math.min(text.length, 1)});
  return {...header, properties, propertyRanges, nodes};
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

function typescriptDiagnostics(values: readonly ts.Diagnostic[], text: string,
  path: string): AuthoringDiagnostic[] {
  return values.filter((value) => value.category === ts.DiagnosticCategory.Error)
    .map((value) => {
      const start = value.start ?? 0;
      const end = start + (value.length ?? 1);
      return {
        ...diagnostic("PXCLIJSX1001", "JSX/TypeScript source could not be transpiled", path),
        details: ts.flattenDiagnosticMessageText(value.messageText, "\n"),
        span: sourceSpan(text, path, {start, end}),
      };
    });
}

async function loadJsxUiDocument(root: string, virtualPath: string,
  kind: "scene" | "component"): Promise<unknown> {
  const sourcePath = projectFile(root, virtualPath);
  const source = await readBoundedText(sourcePath);
  const transpiled = ts.transpileModule(source, {
    fileName: sourcePath,
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
  const syntaxDiagnostics = typescriptDiagnostics(transpiled.diagnostics ?? [],
    source, virtualPath);
  if (syntaxDiagnostics.length > 0) throw new JsxAuthoringFailure(syntaxDiagnostics);

  const fingerprint = createHash("sha256").update(sourcePath).update("\0")
    .update(source).digest("hex").slice(0, 16);
  const executablePath = join(dirname(sourcePath),
    `.${basename(sourcePath)}.prismatix-${fingerprint}.mjs`);
  try {
    await writeFile(executablePath, rewriteSdkImports(transpiled.outputText), "utf8");
    const module = await import(`${pathToFileURL(executablePath).href}?build=${fingerprint}-${Date.now()}`) as {
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
    await rm(executablePath, {force: true});
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

async function loadProject(projectArgument?: string): Promise<LoadedProject> {
  const projectPath = resolve(projectArgument ?? "project.pxproject");
  const root = dirname(projectPath);
  const authoredProject = await readJson(projectPath);
  const authoredRaw = object(authoredProject);
  if (authoredRaw === undefined) throw new Error("project.pxproject must be a JSON object");
  const project = structuredClone(authoredProject);
  const raw = object(project);
  if (raw === undefined) throw new Error("project.pxproject must be a JSON object");
  const documents: Record<string, unknown> = {};
  const documentSources = new Map<string, string>();
  const sourceFiles: Record<string, string> = {};
  const locales: Record<string, unknown> = {};
  const loadDocument = async (virtualPath: unknown): Promise<void> => {
    if (typeof virtualPath !== "string") return;
    const owner = documentSources.get(virtualPath);
    if (owner === virtualPath) return;
    if (owner !== undefined) throw new Error(`${virtualPath} is already produced by ${owner}`);
    documents[virtualPath] = await readJson(projectFile(root, virtualPath));
    documentSources.set(virtualPath, virtualPath);
  };
  const loadJsxDocument = async (virtualPath: string,
    kind: "scene" | "component"): Promise<string> => {
    const outputPath = canonicalUiPath(virtualPath, kind);
    const owner = documentSources.get(outputPath);
    if (owner === virtualPath) return outputPath;
    if (owner !== undefined) throw new Error(`${outputPath} is already produced by ${owner}`);
    documents[outputPath] = await loadJsxUiDocument(root, virtualPath, kind);
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
  for (const value of Array.isArray(raw.effects) ? raw.effects : []) {
    const descriptor = object(value);
    const manifest = typeof descriptor?.source === "string"
      ? object(documents[descriptor.source]) : undefined;
    if (typeof manifest?.shader === "string" && sourceFiles[manifest.shader] === undefined)
      sourceFiles[manifest.shader] = await readBoundedText(projectFile(root, manifest.shader));
  }
  for (const locale of Array.isArray(raw.supportedLocales) ? raw.supportedLocales : []) {
    if (typeof locale !== "string") continue;
    locales[locale] = await readJson(projectFile(root, `Content/Localization/${locale}.json`));
  }
  return {root, projectPath, project, documents, sourceFiles, locales};
}

async function compileLoaded(loaded: LoadedProject) {
  return compileProject({project: loaded.project, documents: loaded.documents,
    sourceFiles: loaded.sourceFiles, locales: loaded.locales,
    projectPath: basename(loaded.projectPath)});
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
  const project = artifact.project;
  for (const value of project.assets ?? []) {
    const asset = object(value);
    if (typeof asset?.source !== "string") continue;
    const destination = projectFile(staging, asset.source);
    await mkdir(dirname(destination), {recursive: true});
    await copyFile(projectFile(loaded.root, asset.source), destination);
  }
  for (const extensionPath of project.extensions) {
    const extension = object(loaded.documents[extensionPath]);
    if (typeof extension?.entry !== "string") continue;
    const prefix = extensionPath.includes("/") ? extensionPath.slice(0, extensionPath.lastIndexOf("/") + 1) : "";
    const modules = Array.isArray(extension.modules) ? extension.modules : [];
    for (const relativePath of [extension.entry, ...modules]) {
      if (typeof relativePath !== "string") continue;
      const modulePath = `${prefix}${relativePath}`;
      const destination = projectFile(staging, modulePath);
      await mkdir(dirname(destination), {recursive: true});
      await copyFile(projectFile(loaded.root, modulePath), destination);
    }
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

async function buildProject(projectArgument: string | undefined, outputArgument: string | undefined): Promise<BuildResult> {
  const loaded = await loadProject(projectArgument);
  const compiled = await compileLoaded(loaded);
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
  return {root: loaded.root, artifact: compiled.artifact, output};
}

async function buildCommand(options: Options): Promise<number> {
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
  return createHash("sha256").update(await readFile(path)).digest("hex");
}

async function runProcess(executable: string, args: readonly string[], input?: string, cwd?: string): Promise<{code: number; stdout: string; stderr: string}> {
  return new Promise((accept, reject) => {
    const child = spawn(executable, [...args], {cwd, stdio: ["pipe", "pipe", "pipe"], windowsHide: true});
    let stdout = ""; let stderr = "";
    child.stdout.setEncoding("utf8"); child.stderr.setEncoding("utf8");
    child.stdout.on("data", (value: string) => { stdout += value; });
    child.stderr.on("data", (value: string) => { stderr += value; });
    child.on("error", reject);
    child.on("close", (code) => accept({code: code ?? 1, stdout, stderr}));
    if (input !== undefined) child.stdin.end(input); else child.stdin.end();
  });
}

async function packProject(options: Options): Promise<{output: string; player: string}> {
  const packager = options.values.get("packager");
  const player = options.values.get("player");
  if (packager === undefined || player === undefined) throw new Error("pack requires --packager and --player");
  const temp = await mkdtemp(join(tmpdir(), "prismatix-pack-"));
  try {
    const built = await buildProject(options.positional[0], join(temp, "artifact"));
    const project = built.artifact.project;
    const inputs = await Promise.all((await listFiles(built.output)).map(async (uri) => {
      const path = projectFile(built.output, uri);
      return {uri, fingerprint: await sha256(path), size: (await stat(path)).size};
    }));
    const output = resolve(options.values.get("output") ?? join(built.root, "dist", project.id));
    const request = {
      format: "PrismatiXPackageRequest", schemaRevision: 2,
      requestId: `cli-${process.pid}`, gameId: project.id,
      projectRoot: built.output, outputDir: output,
      playerExecutable: resolve(player), title: project.name,
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
    const packageTool = resolve(packager);
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
    const child = await runProcess(result.player, [], undefined, result.output);
    if (child.stdout.length > 0) process.stdout.write(child.stdout);
    if (child.stderr.length > 0) process.stderr.write(child.stderr);
    return child.code;
  } catch (error) {
    printDiagnostics(error instanceof PackagerFailure || error instanceof JsxAuthoringFailure ? error.diagnostics :
      [diagnostic("PXCLI1301", "Run failed", options.positional[0], String(error))]);
    return 1;
  }
}

function optionOr(raw: Record<string, unknown>, options: Options, field: string, option: string): unknown {
  return raw[field] ?? options.values.get(option);
}

function typedMigrationDiagnostic(code: string, message: string, path: string,
                                  text: string, range: TextRange,
                                  hint?: string): AuthoringDiagnostic {
  return {...diagnostic(code, message, path, hint), span: sourceSpan(text, path, range)};
}

function typedProjectProperties(document: LegacyTypedDocument, path: string,
                                text: string, diagnostics: AuthoringDiagnostic[]): Record<string, unknown> | undefined {
  if (document.kind !== "pxproject" || document.type !== "PrismatiXProject" || document.nodes.length > 0) {
    const range = document.nodes[0]?.range ?? document.headerRange;
    diagnostics.push(typedMigrationDiagnostic("PXMIG2001",
      "TypedDocument project is not a flat PrismatiXProject manifest", path, text, range,
      "Move scene/resource nodes into canonical files before retrying migration."));
    return undefined;
  }
  const raw = Object.fromEntries(Object.entries(document.properties)
    .map(([key, value]) => [key, plainLegacyValue(value)]));
  raw.id ??= document.id;
  return raw;
}

function typedGameDocument(document: LegacyTypedDocument, path: string, text: string,
                           diagnostics: AuthoringDiagnostic[]): unknown | undefined {
  if (document.kind !== "pxresource" || document.type !== "GameCatalog") {
    diagnostics.push(typedMigrationDiagnostic("PXMIG2010",
      "Typed resource is not a GameCatalog", path, text, document.headerRange));
    return undefined;
  }
  const allowedRoot = new Set(["progressionFlags", "unlockables", "extensions"]);
  for (const key of Object.keys(document.properties)) {
    if (allowedRoot.has(key)) continue;
    diagnostics.push(typedMigrationDiagnostic("PXMIG2011",
      `GameCatalog property cannot be migrated losslessly: ${key}`, path, text,
      document.propertyRanges.get(key) ?? document.headerRange));
  }
  const variables: unknown[] = []; const gallery: unknown[] = []; const inputBindings: unknown[] = [];
  const strings = (node: LegacyTypedNode, key: string, required = true): string | undefined => {
    const value = node.properties[key];
    if (typeof value === "string" && (!required || value.length > 0)) return value;
    if (!required && value === undefined) return undefined;
    diagnostics.push(typedMigrationDiagnostic("PXMIG2012",
      `${node.type}.${key} must be a string`, path, text,
      node.propertyRanges.get(key) ?? node.range));
    return undefined;
  };
  for (const node of document.nodes) {
    if (node.parentId !== null) {
      diagnostics.push(typedMigrationDiagnostic("PXMIG2013",
        `${node.type} is nested and cannot be represented in the 0.2 game catalog`,
        path, text, node.range)); continue;
    }
    if (node.type === "Variable") {
      const name = strings(node, "name"); const value = plainLegacyValue(node.properties.default ?? 0);
      const type = typeof value === "boolean" ? "boolean" : typeof value === "string" ? "string" :
        typeof value === "number" && Number.isInteger(value) ? "integer" : typeof value === "number" ? "number" : undefined;
      if (type === undefined) {
        diagnostics.push(typedMigrationDiagnostic("PXMIG2014",
          "Variable.default cannot be represented by a typed 0.2 variable", path, text,
          node.propertyRanges.get("default") ?? node.range)); continue;
      }
      if (typeof node.properties.persistent !== "boolean" && node.properties.persistent !== undefined) {
        diagnostics.push(typedMigrationDiagnostic("PXMIG2015",
          "Variable.persistent must be boolean", path, text,
          node.propertyRanges.get("persistent") ?? node.range)); continue;
      }
      if (name !== undefined) variables.push({name, type, default: value,
        scope: node.properties.persistent === true ? "profile" : "session"});
      continue;
    }
    if (node.type === "GalleryItem") {
      const id = strings(node, "id"); const title = strings(node, "title", false);
      const reference = (key: string, required: boolean): unknown | undefined => {
        const authored = node.properties[key];
        if (!required && authored === undefined) return undefined;
        const resource = legacySpecial(authored, "resource");
        if (resource !== undefined) return {id: resource.values[0], path: resource.values[1]};
        if (typeof authored === "string" && authored.length > 0) return authored;
        diagnostics.push(typedMigrationDiagnostic("PXMIG2016",
          `GalleryItem.${key} must be a path or ResourceRef`, path, text,
          node.propertyRanges.get(key) ?? node.range)); return undefined;
      };
      const image = reference("image", true); const thumbnail = reference("thumbnail", false);
      if (id !== undefined && image !== undefined)
        gallery.push({id, ...(title === undefined ? {} : {title}), image,
          ...(thumbnail === undefined ? {} : {thumbnail})});
      continue;
    }
    if (node.type === "InputBinding") {
      const key = strings(node, "key"); const command = strings(node, "command");
      const argument = strings(node, "argument", false);
      if (key !== undefined && command !== undefined)
        inputBindings.push({key, command, ...(argument === undefined ? {} : {argument})});
      continue;
    }
    diagnostics.push(typedMigrationDiagnostic("PXMIG2017",
      `GameCatalog node cannot be migrated losslessly: ${node.type}`, path, text, node.range,
      node.type.startsWith("Character")
        ? "Export characters to canonical .pxcharacter resources and declare them in the project."
        : undefined));
  }
  if (diagnostics.some((value) => value.path === path && value.severity === "error")) return undefined;
  return {format: "PrismatiXGame", schemaRevision: 2, variables, gallery,
    unlockables: plainLegacyValue(document.properties.unlockables ?? []),
    ...(document.properties.progressionFlags === undefined ? {} :
      {progressionFlags: plainLegacyValue(document.properties.progressionFlags)}),
    ...(inputBindings.length === 0 ? {} : {inputBindings}),
    ...(document.properties.extensions === undefined ? {} :
      {extensions: plainLegacyValue(document.properties.extensions)})};
}

function uiNodeKind(type: string): string {
  if (type === "Control") return "control";
  if (type === "Label" || type === "RichTextLabel") return "label";
  if (type === "Button" || type === "IconButton") return "button";
  if (type === "TextureRect" || type === "NinePatchRect" || type === "VideoRect") return "image";
  if (type === "StackContainer") return "stack";
  if (type === "HBoxContainer") return "hbox";
  if (type === "VBoxContainer") return "vbox";
  if (type === "GridContainer") return "grid";
  if (type === "Panel" || type.endsWith("Container")) return "group";
  return "leaf";
}

function typedUiValue(value: unknown): unknown {
  const special = legacySpecial(value);
  if (special !== undefined) {
    if (special.typed === "vec2") return {type: "vec2", x: special.values[0], y: special.values[1]};
    if (special.typed === "rect") return {type: "rect", x: special.values[0], y: special.values[1],
      width: special.values[2], height: special.values[3]};
    if (special.typed === "color") return {type: "color", value: plainLegacyValue(special)};
    if (special.typed === "uuid") return {type: "uuid", value: special.values[0]};
    if (special.typed === "resource") return {type: "resource", value: special.values[0]};
    return {type: "token", value: special.values[0]};
  }
  if (Array.isArray(value)) return value.map(typedUiValue);
  const record = object(value);
  if (record === undefined) return value;
  return Object.fromEntries(Object.entries(record).map(([key, item]) => [key, typedUiValue(item)]));
}

function typedUiDocument(document: LegacyTypedDocument, path: string, text: string,
                         diagnostics: AuthoringDiagnostic[]): unknown | undefined {
  if (document.kind !== "pxscene" || (document.type !== "UIScene" && document.type !== "UIComponent")) {
    diagnostics.push(typedMigrationDiagnostic("PXMIG2020",
      "Typed UI resource must be an @pxscene UIScene or UIComponent", path, text,
      document.headerRange)); return undefined;
  }
  const canvas = legacySpecial(document.properties.canvasSize, "vec2");
  if (canvas === undefined || !canvas.values.every((value) => typeof value === "number") ||
      Number(canvas.values[0]) <= 0 || Number(canvas.values[1]) <= 0) {
    diagnostics.push(typedMigrationDiagnostic("PXMIG2021",
      "Typed UI requires a positive canvasSize vec2", path, text,
      document.propertyRanges.get("canvasSize") ?? document.headerRange));
  }
  for (const key of Object.keys(document.properties)) {
    if (key === "canvasSize" || key === "uiSchemaVersion") continue;
    diagnostics.push(typedMigrationDiagnostic("PXMIG2022",
      `Typed UI document property cannot be migrated losslessly: ${key}`, path, text,
      document.propertyRanges.get(key) ?? document.headerRange));
  }
  const ids = new Set<string>();
  for (const node of document.nodes) {
    if (ids.has(node.id)) diagnostics.push(typedMigrationDiagnostic("PXMIG2023",
      `Duplicate UI node id: ${node.id}`, path, text, node.range));
    ids.add(node.id);
  }
  const roots = document.nodes.filter((node) => node.parentId === null);
  if (roots.length !== 1) diagnostics.push(typedMigrationDiagnostic("PXMIG2024",
    "Typed UI must contain exactly one root", path, text, document.headerRange));
  const byId = new Map(document.nodes.map((node) => [node.id, node]));
  for (const node of document.nodes)
    if (node.parentId !== null && !byId.has(node.parentId))
      diagnostics.push(typedMigrationDiagnostic("PXMIG2025",
        `UI node references a missing parent: ${node.parentId}`, path, text, node.range));

  const layouts = new Map<string, Record<string, unknown>>(); const visiting = new Set<string>();
  const layout = (node: LegacyTypedNode): Record<string, unknown> | undefined => {
    const existing = layouts.get(node.id); if (existing !== undefined) return existing;
    if (!visiting.add(node.id)) {
      diagnostics.push(typedMigrationDiagnostic("PXMIG2026", "UI parent cycle cannot be migrated", path, text, node.range));
      return undefined;
    }
    const parent = node.parentId === null ? undefined : byId.get(node.parentId);
    const parentLayout = parent === undefined ? undefined : layout(parent);
    const anchors = legacySpecial(node.properties.anchors, "rect")?.values ?? [0, 0, 0, 0];
    const offsets = legacySpecial(node.properties.offsets, "rect")?.values;
    if (node.parentId !== null && offsets === undefined) {
      diagnostics.push(typedMigrationDiagnostic("PXMIG2027",
        "A non-root Typed UI node needs explicit offsets for lossless conversion", path, text,
        node.propertyRanges.get("offsets") ?? node.range)); visiting.delete(node.id); return undefined;
    }
    const parentWidth = Number(parentLayout?.width ?? canvas?.values[0] ?? 0);
    const parentHeight = Number(parentLayout?.height ?? canvas?.values[1] ?? 0);
    const values = offsets ?? [0, 0, canvas?.values[0] ?? 1, canvas?.values[1] ?? 1];
    const spanX = Number(anchors[2]) - Number(anchors[0]);
    const spanY = Number(anchors[3]) - Number(anchors[1]);
    const width = spanX === 0 ? Number(values[2]) : parentWidth * spanX + Number(values[2]) - Number(values[0]);
    const height = spanY === 0 ? Number(values[3]) : parentHeight * spanY + Number(values[3]) - Number(values[1]);
    if (![...anchors, ...values, width, height].every((value) => Number.isFinite(Number(value))) || width <= 0 || height <= 0) {
      diagnostics.push(typedMigrationDiagnostic("PXMIG2028",
        "Typed UI anchors/offsets produce an invalid rectangle", path, text,
        node.propertyRanges.get("offsets") ?? node.range)); visiting.delete(node.id); return undefined;
    }
    const managed = parent !== undefined && ["stack", "hbox", "vbox", "grid"].includes(uiNodeKind(parent.type));
    const result = {mode: managed ? "container" : "free", x: Number(values[0]), y: Number(values[1]),
      width, height, anchorX: Number(anchors[0]), anchorY: Number(anchors[1]),
      anchorRight: Number(anchors[2]), anchorBottom: Number(anchors[3]), pivotX: 0, pivotY: 0,
      margin: 0, alignment: "start", sizeRule: "fixed"};
    layouts.set(node.id, result); visiting.delete(node.id); return result;
  };
  const nodes = document.nodes.map((node, index) => {
    const ignored = new Set(["anchors", "offsets", "bindings", "triggers", "editorLocked", "visibility"]);
    let visible = true;
    if (node.properties.visibility !== undefined) {
      if (node.properties.visibility === "Visible") visible = true;
      else if (node.properties.visibility === "Hidden") visible = false;
      else diagnostics.push(typedMigrationDiagnostic("PXMIG2029",
        "Collapsed visibility has no lossless 0.2 UI representation", path, text,
        node.propertyRanges.get("visibility") ?? node.range));
    }
    const runtimeProperties = Object.fromEntries(Object.entries(node.properties)
      .filter(([key]) => !ignored.has(key)).map(([key, value]) => [key, typedUiValue(value)]));
    const bindings = object(plainLegacyValue(node.properties.bindings)) ?? {};
    let onClick: unknown;
    const triggers = object(plainLegacyValue(node.properties.triggers));
    if (triggers !== undefined) {
      const entries = Object.entries(triggers);
      if (entries.length === 1 && (entries[0]![0] === "activated" || entries[0]![0] === "clicked")) {
        const binding = object(entries[0]![1]); const arguments_ = object(binding?.arguments) ?? {};
        if (binding?.kind === "action" && typeof binding.action === "string")
          onClick = {id: binding.action, arguments: arguments_};
        else diagnostics.push(typedMigrationDiagnostic("PXMIG2030",
          "Typed UI trigger is not a direct action", path, text,
          node.propertyRanges.get("triggers") ?? node.range));
      } else diagnostics.push(typedMigrationDiagnostic("PXMIG2031",
        "Typed UI trigger set cannot be represented by the canonical onClick action", path, text,
        node.propertyRanges.get("triggers") ?? node.range));
    }
    return {id: node.id, parentId: node.parentId,
      order: document.nodes.slice(0, index).filter((candidate) => candidate.parentId === node.parentId).length,
      kind: uiNodeKind(node.type), runtimeType: node.type, name: node.name, visible,
      locked: node.properties.editorLocked === true, layout: layout(node), runtimeProperties, bindings,
      ...(onClick === undefined ? {} : {onClick})};
  });
  if (diagnostics.some((value) => value.path === path && value.severity === "error")) return undefined;
  const common = {schemaRevision: 2, id: document.id, revision: 1,
    name: basename(path, extname(path)), width: Math.round(Number(canvas!.values[0])),
    height: Math.round(Number(canvas!.values[1])), rootId: roots[0]!.id, nodes, theme: []};
  return document.type === "UIComponent"
    ? {format: "PrismatiXUIComponent", ...common,
      componentInterface: {properties: [], signals: [], slots: []}}
    : {format: "PrismatiXUIScene", ...common,
      behaviorGraph: {nodes: [], links: [], groups: []}, behaviorTriggers: [], visualStateGroups: []};
}

async function migrateCommand(options: Options): Promise<number> {
  const projectPath = resolve(options.positional[0] ?? "project.pxproject");
  const root = dirname(projectPath);
  const diagnostics: AuthoringDiagnostic[] = [];
  const changes: MigrationChange[] = [];
  let activePath = projectPath;
  let activeText = "";
  try {
    const original = await readBoundedText(projectPath);
    activeText = original;
    let raw: Record<string, unknown> | undefined;
    let typedProject: LegacyTypedDocument | undefined;
    if (original.trimStart().startsWith("@")) {
      try {
        typedProject = parseLegacyTypedDocument(original);
        raw = typedProjectProperties(typedProject, projectPath, original, diagnostics);
      } catch (error) {
        const range = error instanceof LegacyTypedError ? error.range : {start: 0, end: original.length};
        diagnostics.push(typedMigrationDiagnostic("PXMIG2000",
          `TypedDocument project is malformed: ${String(error)}`, projectPath, original, range));
      }
    } else {
      const parsed = JSON.parse(original) as unknown;
      const already = validateDocument<ProjectDocument>("project", parsed, projectPath);
      if (already.valid) {
        process.stdout.write(`${JSON.stringify({dryRun: !options.flags.has("write"), changes: [], diagnostics: []}, null, 2)}\n`);
        return 0;
      }
      raw = object(parsed);
      if (raw === undefined) throw new Error("Legacy project must be a JSON object");
    }
    if (raw !== undefined) {
      const recognizedProjectFields = new Set([
        "format", "schemaRevision", "id", "name", "version", "contentVersion",
        "saveVersion", "graphicsTier", "effects", "saveMigrations",
        "engineCompatibility", "resolution", "width", "height", "entry",
        "defaultLocale", "supportedLocales", "storyIndex", "gameCatalog",
        "extensions", "uiEntryPoints", "assets", "characters", "uiComponents",
        "settings",
      ]);
      const projectRanges = indexJsonRanges(original);
      for (const key of Object.keys(raw).filter((key) => !recognizedProjectFields.has(key))) {
        const pointer = `/${jsonPointerSegment(key)}`;
        const range = typedProject?.propertyRanges.get(key) ?? projectRanges.get(pointer) ??
          projectRanges.get("") ?? typedProject?.headerRange ?? {start: 0, end: original.length};
        diagnostics.push({...diagnostic("PXMIG2004",
          `Legacy project field cannot be migrated losslessly: ${key}`,
          projectPath, "Remove the field only after moving its behavior to a canonical 0.2 document."),
          span: sourceSpan(original, projectPath, range)});
      }
      const resolution = object(raw.resolution) ??
        (typeof raw.width === "number" && typeof raw.height === "number" ? {width: raw.width, height: raw.height} : undefined);
      const entry = object(raw.entry) ?? {
        story: options.values.get("entry-story"), ui: options.values.get("entry-ui"),
      };
      const supportedLocales = Array.isArray(raw.supportedLocales) ? raw.supportedLocales :
        (options.values.get("supported-locales")?.split(",").filter(Boolean));
      const canonical: Record<string, unknown> = {
        format: "PrismatiXProject", schemaRevision: 2,
        id: optionOr(raw, options, "id", "id"),
        name: optionOr(raw, options, "name", "name"),
        version: optionOr(raw, options, "version", "version"),
        contentVersion: optionOr(raw, options, "contentVersion", "content-version"),
        saveVersion: raw.saveVersion ?? Number(options.values.get("save-version")),
        resolution, entry,
        defaultLocale: optionOr(raw, options, "defaultLocale", "default-locale"),
        supportedLocales,
        storyIndex: optionOr(raw, options, "storyIndex", "story-index"),
        gameCatalog: optionOr(raw, options, "gameCatalog", "game-catalog"),
        extensions: raw.extensions ?? [], uiEntryPoints: raw.uiEntryPoints,
        ...(raw.graphicsTier === undefined ? {} : {graphicsTier: raw.graphicsTier}),
        ...(raw.effects === undefined ? {} : {effects: raw.effects}),
        ...(raw.saveMigrations === undefined ? {} : {saveMigrations: raw.saveMigrations}),
        ...(raw.engineCompatibility === undefined ? {} : {engineCompatibility: raw.engineCompatibility}),
        ...(raw.assets === undefined ? {} : {assets: raw.assets}),
        ...(raw.characters === undefined ? {} : {characters: raw.characters}),
        ...(raw.uiComponents === undefined ? {} : {uiComponents: raw.uiComponents}),
        ...(raw.settings === undefined ? {} : {settings: raw.settings}),
      };
      const validation = validateDocument<ProjectDocument>("project", canonical, projectPath);
      diagnostics.push(...validation.diagnostics.map((value) => {
        if (typedProject === undefined) return diagnosticsWithSourceSpans([value], original, projectPath)[0]!;
        const pointer = pointerFromDiagnostic(value);
        const key = pointer.split("/")[1]?.replaceAll("~1", "/").replaceAll("~0", "~") ?? "";
        return {...value, path: value.path ?? projectPath,
          span: sourceSpan(original, projectPath,
            typedProject.propertyRanges.get(key) ?? typedProject.headerRange)};
      }));
      if (validation.valid) {
        const after = canonicalJson(canonical);
        if (after !== original) changes.push({path: projectPath, before: original, after, reason: "canonical-project"});
        const project = validation.value!;
        const candidates: Array<{virtualPath: string; contract: "storyIndex" | "game" | "extension" | "ui" | "character" | "uiComponent" | "saveMigration"}> = [
          {virtualPath: project.storyIndex, contract: "storyIndex"},
          {virtualPath: project.gameCatalog, contract: "game"},
          ...project.extensions.map((virtualPath) => ({virtualPath, contract: "extension" as const})),
          ...Object.values(project.uiEntryPoints).map((virtualPath) => ({virtualPath, contract: "ui" as const})),
          ...(project.characters ?? []).map((value) => ({virtualPath: value.source, contract: "character" as const})),
          ...(project.uiComponents ?? []).map((value) => ({virtualPath: value.source, contract: "uiComponent" as const})),
          ...(project.saveMigrations ?? []).map((value) => ({virtualPath: value.asset, contract: "saveMigration" as const})),
        ];
        for (const candidate of candidates) {
          const {virtualPath, contract} = candidate;
          const path = projectFile(root, virtualPath);
          let before: string;
          try { before = await readBoundedText(path); } catch { continue; }
          activePath = path; activeText = before;
          let value: unknown;
          if (before.trimStart().startsWith("@")) {
            try {
              const typed = parseLegacyTypedDocument(before);
              value = contract === "game"
                ? typedGameDocument(typed, path, before, diagnostics)
                : contract === "ui" || contract === "uiComponent"
                  ? typedUiDocument(typed, path, before, diagnostics)
                  : undefined;
              if (value === undefined && contract !== "game" && contract !== "ui" && contract !== "uiComponent")
                diagnostics.push(typedMigrationDiagnostic("PXMIG2002",
                  `TypedDocument ${contract} resource has no lossless 0.2 representation`, path, before,
                  typed.headerRange));
            } catch (error) {
              const range = error instanceof LegacyTypedError ? error.range : {start: 0, end: before.length};
              diagnostics.push(typedMigrationDiagnostic("PXMIG2002",
                `TypedDocument resource is malformed: ${String(error)}`, path, before, range));
              value = undefined;
            }
          } else {
            value = JSON.parse(before) as unknown;
          }
          if (value === undefined) continue;
          const migrated = migrateDocument(contract, value);
          diagnostics.push(...diagnosticsWithSourceSpans(migrated.diagnostics, before, path));
          if (migrated.valid) {
            const transformed = canonicalJson(migrated.value);
            if (transformed !== before) changes.push({path, before, after: transformed, reason: `${contract}-migration`});
          }
        }
        for (const locale of project.supportedLocales) {
          const path = projectFile(root, `Content/Localization/${locale}.json`);
          let before: string;
          try { before = await readBoundedText(path); } catch { continue; }
          activePath = path; activeText = before;
          const value = JSON.parse(before) as unknown;
          if (object(value)?.format === "PrismatiXLocale") {
            const migrated = migrateDocument("locale", value);
            diagnostics.push(...diagnosticsWithSourceSpans(migrated.diagnostics, before, path));
            if (migrated.valid) {
              const transformed = canonicalJson(migrated.value);
              if (transformed !== before) changes.push({path, before, after: transformed, reason: "locale-migration"});
            }
            continue;
          }
          const localeObject = object(value);
          const invalidLocaleEntry = localeObject === undefined ? undefined :
            Object.entries(localeObject).find(([, item]) => typeof item !== "string");
          if (localeObject === undefined || invalidLocaleEntry !== undefined) {
            const pointer = invalidLocaleEntry === undefined ? "" :
              `/${jsonPointerSegment(invalidLocaleEntry[0])}`;
            const ranges = indexJsonRanges(before);
            const range = ranges.get(pointer) ?? ranges.get("") ??
              {start: 0, end: before.length};
            diagnostics.push({...diagnostic("PXMIG2003", "Legacy locale is not a string map", path),
              span: sourceSpan(before, path, range)});
            continue;
          }
          const transformed = canonicalJson({format: "PrismatiXLocale", schemaRevision: 2, locale, strings: value});
          changes.push({path, before, after: transformed, reason: "locale-envelope"});
        }
      }
    }
    const failed = diagnostics.some((value) => value.severity === "error");
    const report = {dryRun: !options.flags.has("write"), changes: changes.map(({path, reason}) => ({path, reason})), diagnostics};
    const reportPath = options.values.get("report");
    if (reportPath !== undefined) await writeFile(resolve(reportPath), `${JSON.stringify(report, null, 2)}\n`, "utf8");
    process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
    if (failed) return 1;
    if (options.flags.has("write")) {
      const backup = resolve(options.values.get("backup") ?? join(root, `.prismatix-migration-backup-${Date.now()}`));
      for (const change of changes) {
        const backupPath = join(backup, relative(root, change.path));
        await mkdir(dirname(backupPath), {recursive: true});
        await writeFile(backupPath, change.before, "utf8");
      }
      for (const change of changes) await writeFile(change.path, change.after, "utf8");
      process.stdout.write(`${JSON.stringify({written: changes.length, backup})}\n`);
    }
    return 0;
  } catch (error) {
    const failure: AuthoringDiagnostic = {
      ...diagnostic("PXMIG2099", "Migration failed before commit", activePath,
        String(error)),
      span: jsonParseFailureSpan(activeText, activePath, error),
    };
    const report = {dryRun: !options.flags.has("write"), changes: [],
      diagnostics: [failure]};
    const reportPath = options.values.get("report");
    if (reportPath !== undefined)
      await writeFile(resolve(reportPath), `${JSON.stringify(report, null, 2)}\n`, "utf8");
    printDiagnostics([failure]);
    return 1;
  }
}

function stable(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stable).join(",")}]`;
  const record = object(value);
  if (record !== undefined) return `{${Object.keys(record).sort().map((key) => `${JSON.stringify(key)}:${stable(record[key])}`).join(",")}}`;
  return JSON.stringify(value);
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
  process.stderr.write("Usage: prismatix <validate|build|pack|run|migrate|inspect-save> [project-or-save] [options]\n");
}

async function main(): Promise<number> {
  const [command, ...args] = process.argv.slice(2);
  const options = parseOptions(args);
  if (command === "validate") return validateCommand(options);
  if (command === "build") return buildCommand(options);
  if (command === "pack") return packCommand(options);
  if (command === "run") return runCommand(options);
  if (command === "migrate") return migrateCommand(options);
  if (command === "inspect-save") return inspectSaveCommand(options);
  usage(); return 2;
}

process.exitCode = await main();
