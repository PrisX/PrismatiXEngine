import {canonicalJson} from "./canonical-json.js";
import {fnv1a64 as stableId} from "./fnv1a64.js";
import {assetIdFromReference, canonicalResourceReference} from "./resources.js";
import type {StoryAssetCategory} from "./story-assets.js";
import {parseStoryExpression} from "./story-expression.js";
import type {
  AuthoringDiagnostic,
  CharacterDocument,
  ExtensionCommand,
  ExtensionParameter,
  JsonValue,
  RuntimeIrOperation,
  RuntimeSourceMap,
  SourceSpan,
  StoryArgument,
  StoryCompileContext,
  StoryCompileResult,
  StoryDocument,
  StoryNode,
} from "./types.js";

const maximumStoryBytes = 16 * 1024 * 1024;
const identifierPattern = /^[A-Za-z0-9][A-Za-z0-9._-]*$/u;
const audioDurationPattern = /^(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?(?:ms|s)?$/u;
const positiveNumberPattern = /^(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/u;
// Built-in command parameters are closed. `set` accepts a variable name as a
// key, `if` is an expression, and extension commands have their own contract.
const builtinNamedArguments: Readonly<Record<string, readonly string[]>> = {
  id: ["value"],
  "choice.wait": [],
  chapter: ["title"],
  say: ["speaker", "text", "character"],
  narrate: ["text"],
  bg: ["asset", "rule", "duration", "vague"],
  show: ["character", "expression", "asset", "sprite", "position", "x", "y", "scale", "transition"],
  move: ["character", "position", "x", "y", "scale", "duration", "ease", "wait"],
  hide: ["character", "transition"],
  choice: ["text", "goto", "target", "scene"],
  jump: ["goto", "target", "scene"],
  call: ["goto", "target", "scene"],
  return: [], end: [], wait: ["duration"],
  add: ["name", "amount"],
  voice: ["asset", "fade", "loop", "intro"],
  bgm: ["asset", "fade", "loop", "intro"],
  ambience: ["asset", "fade", "loop", "intro"],
  se: ["asset", "fade", "loop", "intro"],
  "bgm.stop": ["fade"], "ambience.stop": ["fade"], "se.stop": ["fade"], "voice.stop": [],
  act: ["character", "preset", "size", "time", "loop", "ease"],
  video: ["asset", "skippable"],
  timeline: ["timeline", "await", "speed"],
  motion: ["asset", "await", "speed"], "motion.stop": [],
  ui: ["route", "operation"],
  hud: ["visible"],
  unlock: ["kind", "id"],
  effect: ["value"], shake: ["duration", "amplitude", "target"],
  camera: ["value", "x", "y", "zoom", "duration", "ease", "wait"],
  else: [], endif: [],
};

function durationMilliseconds(value: string): number | undefined {
  if (!audioDurationPattern.test(value)) return undefined;
  const seconds = value.endsWith("s") && !value.endsWith("ms");
  const milliseconds = Number(value.replace(/(?:ms|s)$/u, "")) * (seconds ? 1000 : 1);
  return Number.isFinite(milliseconds) && milliseconds >= 0 && milliseconds <= 2_147_483_647
    ? milliseconds : undefined;
}

function validAudioDuration(value: string): boolean {
  return durationMilliseconds(value) !== undefined;
}

interface SourceLine {
  readonly text: string;
  readonly line: number;
  /** Legacy UTF-16 index remains part of generated operation identities. */
  readonly offset: number;
  readonly byteOffset: number;
}

function sourceLines(text: string): SourceLine[] {
  const result: SourceLine[] = [];
  let offset = 0;
  let byteOffset = 0;
  let line = 1;
  while (offset < text.length) {
    const newline = text.indexOf("\n", offset);
    const end = newline < 0 ? text.length : newline;
    const raw = text.slice(offset, end).replace(/\r$/u, "");
    result.push({text: raw, line, offset, byteOffset});
    if (newline < 0) break;
    byteOffset += Buffer.byteLength(text.slice(offset, newline + 1), "utf8");
    offset = newline + 1;
    line += 1;
  }
  if (text.length === 0 || text.endsWith("\n"))
    result.push({text: "", line, offset: text.length, byteOffset});
  return result;
}

function span(path: string, line: SourceLine, startColumn: number, endColumn: number): SourceSpan {
  const byteAt = (column: number) => line.byteOffset +
    Buffer.byteLength(line.text.slice(0, column - 1), "utf8");
  return {
    path,
    start: {line: line.line, column: startColumn, offset: byteAt(startColumn)},
    end: {line: line.line, column: endColumn, offset: byteAt(endColumn)},
  };
}

function error(code: string, message: string, sourceSpan?: SourceSpan, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    ...(sourceSpan === undefined ? {} : {path: sourceSpan.path, span: sourceSpan}),
    ...(details === undefined ? {} : {details}),
  };
}

interface Token {
  readonly raw: string;
  readonly start: number;
  readonly end: number;
}

function tokenizeCommand(value: string): {tokens: Token[]; error?: string} {
  const tokens: Token[] = [];
  let index = 0;
  while (index < value.length) {
    while (index < value.length && /\s/u.test(value[index]!)) index += 1;
    if (index >= value.length) break;
    const start = index;
    let quote = "";
    let escaped = false;
    let squareDepth = 0;
    let objectDepth = 0;
    while (index < value.length) {
      const character = value[index]!;
      if (escaped) {
        escaped = false;
      } else if (character === "\\" && quote !== "") {
        escaped = true;
      } else if (quote !== "") {
        if (character === quote) quote = "";
      } else if (character === "\"" || character === "'") {
        quote = character;
      } else if (character === "[") {
        squareDepth += 1;
      } else if (character === "]") {
        squareDepth -= 1;
        if (squareDepth < 0) return {tokens, error: "Unbalanced structured command argument"};
      } else if (character === "{") {
        objectDepth += 1;
      } else if (character === "}") {
        objectDepth -= 1;
        if (objectDepth < 0) return {tokens, error: "Unbalanced structured command argument"};
      } else if (/\s/u.test(character) && squareDepth === 0 && objectDepth === 0) {
        break;
      }
      index += 1;
    }
    if (quote !== "") return {tokens, error: "Unterminated quoted command argument"};
    if (squareDepth !== 0 || objectDepth !== 0) return {tokens, error: "Unbalanced structured command argument"};
    tokens.push({raw: value.slice(start, index), start, end: index});
  }
  return {tokens};
}

interface DecodedValue {
  readonly value: JsonValue;
  readonly error?: string;
}

function decodeValue(raw: string): DecodedValue {
  if (raw === "true") return {value: true};
  if (raw === "false") return {value: false};
  if (raw === "null") return {value: null};
  if (/^-?(?:0|[1-9]\d*)$/u.test(raw)) return {value: Number.parseInt(raw, 10)};
  if (/^-?(?:0|[1-9]\d*)\.\d+(?:[eE][+-]?\d+)?$/u.test(raw)) return {value: Number.parseFloat(raw)};
  if ((raw.startsWith("[") && raw.endsWith("]")) || (raw.startsWith("{") && raw.endsWith("}"))) {
    try {
      return {value: JSON.parse(raw) as JsonValue};
    } catch {
      return {value: raw, error: "Structured Story command arguments must use valid JSON"};
    }
  }
  if (raw.length >= 2 && raw.startsWith("\"") && raw.endsWith("\"")) {
    try {
      return {value: JSON.parse(raw) as string};
    } catch {
      return {value: raw.slice(1, -1)};
    }
  }
  if (raw.length >= 2 && raw.startsWith("'") && raw.endsWith("'")) {
    return {value: raw.slice(1, -1).replace(/\\'/gu, "'").replace(/\\\\/gu, "\\")};
  }
  return {value: raw};
}

function parseArguments(path: string, line: SourceLine, tokens: readonly Token[],
  diagnostics: AuthoringDiagnostic[], argumentBaseColumn: number,
  namedArguments = true): StoryArgument[] {
  const seenNames = new Set<string>();
  return tokens.map((token) => {
    // `if` is an expression, not a command's name=value argument list.
    // Preserve operators and quoted '=' characters for the expression parser.
    const equals = namedArguments ? token.raw.indexOf("=") : -1;
    const name = equals > 0 ? token.raw.slice(0, equals) : undefined;
    const raw = equals > 0 ? token.raw.slice(equals + 1) : token.raw;
    const start = argumentBaseColumn + token.start + (equals > 0 ? equals + 1 : 0);
    const argumentSpan = span(path, line, start, start + raw.length);
    if (name !== undefined) {
      if (seenNames.has(name)) diagnostics.push(error("PXSTORY1016",
        `Duplicate command argument: ${name}`, argumentSpan));
      seenNames.add(name);
    }
    const decoded = decodeValue(raw);
    if (decoded.error !== undefined) diagnostics.push(error("PXSTORY1014", decoded.error, argumentSpan));
    return {
      ...(name === undefined ? {} : {name}),
      raw,
      value: decoded.value,
      span: argumentSpan,
    };
  });
}

const speakerTerminators = new Set(["[", "「", "『", "（", "(", "“"]);

function validLiteralSpeaker(value: string): boolean {
  return value.trim().length > 0 && Buffer.byteLength(value, "utf8") <= 512 &&
    !/[\u0000-\u001f\u007f]/u.test(value);
}

/** Length of a bracketed `[...]` group starting at `start`, quote-aware. */
function bracketLength(value: string, start: number): number | undefined {
  let depth = 0;
  let quote = "";
  let escaped = false;
  for (let index = start; index < value.length; index += 1) {
    const character = value[index]!;
    if (escaped) escaped = false;
    else if (character === "\\" && quote !== "") escaped = true;
    else if (quote !== "") { if (character === quote) quote = ""; }
    else if (character === "\"" || character === "'") quote = character;
    else if (character === "[") depth += 1;
    else if (character === "]" && --depth === 0) return index - start + 1;
  }
  return undefined;
}

/** Prose ending in one unescaped backslash continues on the next line. */
function continues(text: string): boolean {
  const trailing = /\\+$/u.exec(text)?.[0].length ?? 0;
  return trailing % 2 === 1;
}

function isProse(trimmed: string): boolean {
  return !trimmed.startsWith(";") && !trimmed.startsWith("*") && !trimmed.startsWith("[");
}

export function parseStory(text: string, path = "Story/untitled.pxstory"): StoryDocument {
  const diagnostics: AuthoringDiagnostic[] = [];
  const nodes: StoryNode[] = [];
  if (Buffer.byteLength(text, "utf8") > maximumStoryBytes) {
    diagnostics.push(error("PXSTORY1001", "Story exceeds the 16 MiB limit"));
    return {path, nodes, diagnostics};
  }
  if (text.startsWith("﻿")) {
    diagnostics.push(error("PXSTORY1002", "Story must be UTF-8 without a BOM"));
    text = text.slice(1);
  }

  let activeSpeaker: {readonly name: string; readonly literal: boolean} | undefined;
  const lines = sourceLines(text);
  for (let lineIndex = 0; lineIndex < lines.length; lineIndex += 1) {
    const line = lines[lineIndex]!;
    let trimmed = line.text.trim();
    if (trimmed.length === 0) {
      activeSpeaker = undefined;
      continue;
    }
    const startColumn = line.text.indexOf(trimmed) + 1;
    let lineSpan = span(path, line, startColumn, startColumn + trimmed.length);
    let raw = line.text;
    // One displayed line may span several source lines: `text\` joins the
    // next non-blank line with a newline, so long prose stays readable.
    let prose = trimmed;
    if (isProse(trimmed) && continues(trimmed)) {
      const parts = [trimmed.slice(0, -1).trimEnd()];
      let last = line;
      while (continues(last.text.trim()) && lineIndex + 1 < lines.length &&
             lines[lineIndex + 1]!.text.trim().length > 0) {
        lineIndex += 1;
        last = lines[lineIndex]!;
        const next = last.text.trim();
        parts.push(continues(next) ? next.slice(0, -1).trimEnd() : next);
        raw += `\n${last.text}`;
      }
      prose = parts.join("\n");
      trimmed = raw.trim();
      const lastText = last.text.trimEnd();
      lineSpan = {path, start: lineSpan.start,
        end: span(path, last, lastText.length + 1, lastText.length + 1).end};
    }
    // Deterministic fallback for unannotated source. Published save/seen
    // identity can be pinned independently of path, offset, and prose with
    // an [id ...] directive immediately before an observable operation.
    const id = `story-${stableId(`${path}:${line.offset}:${trimmed}`)}`;
    if (trimmed.startsWith(";")) {
      nodes.push({id, kind: "comment", span: lineSpan, raw, text: trimmed.slice(1).trim()});
      continue;
    }
    if (trimmed.startsWith("*")) {
      const name = trimmed.slice(1).trim();
      if (!identifierPattern.test(name)) diagnostics.push(error("PXSTORY1010", "Label must be a stable identifier", lineSpan));
      nodes.push({id, kind: "label", span: lineSpan, raw, name});
      continue;
    }
    if (trimmed.startsWith("@")) {
      // `@alias` starts a speaker block; `@alias text` speaks exactly one
      // line. Quoted and non-identifier names (`@"A B"`, `@直哉`) are
      // display names rather than character aliases.
      const body = prose.slice(1);
      let cursor = body.length - body.trimStart().length;
      let name: string | undefined;
      let literal = false;
      if (body[cursor] === "\"") {
        let end = cursor + 1;
        while (end < body.length && body[end] !== "\"") end += body[end] === "\\" ? 2 : 1;
        let parsed: unknown;
        try { parsed = JSON.parse(body.slice(cursor, end + 1)) as unknown; }
        catch { parsed = undefined; }
        if (typeof parsed === "string" && validLiteralSpeaker(parsed)) name = parsed;
        literal = true;
        cursor = end + 1;
      } else {
        const begin = cursor;
        while (cursor < body.length && !/\s/u.test(body[cursor]!) &&
               !speakerTerminators.has(body[cursor]!)) cursor += 1;
        const authored = body.slice(begin, cursor);
        literal = !identifierPattern.test(authored);
        if (authored.length > 0 && (!literal || validLiteralSpeaker(authored))) name = authored;
      }
      if (name === undefined) {
        diagnostics.push(error(literal ? "PXSTORY1015" : "PXSTORY1011", literal
          ? "Literal speaker must be a non-empty name of at most 512 UTF-8 bytes"
          : "Speaker must be a character alias or display name", lineSpan));
        activeSpeaker = undefined;
        continue;
      }
      let options: StoryArgument[] | undefined;
      if (body[cursor] === "[") {
        const length = bracketLength(body, cursor);
        const tokenized = length === undefined ? undefined
          : tokenizeCommand(body.slice(cursor + 1, cursor + length - 1));
        if (length === undefined || tokenized === undefined || tokenized.error !== undefined) {
          diagnostics.push(error("PXSTORY1013",
            tokenized?.error ?? "Speaker options are missing a closing bracket", lineSpan));
          activeSpeaker = undefined;
          continue;
        }
        options = parseArguments(path, line, tokenized.tokens, diagnostics,
          startColumn + cursor + 2);
        cursor += length;
      }
      const spoken = body.slice(cursor).trim();
      const speakerFields = {name, ...(literal ? {literalSpeaker: true} : {})};
      if (spoken.length === 0) {
        if (options !== undefined) diagnostics.push(error("PXSTORY1018",
          "Speaker options apply to one line; write its text after them", lineSpan));
        activeSpeaker = {name, literal};
        nodes.push({id, kind: "speaker", span: lineSpan, raw, ...speakerFields});
      } else {
        activeSpeaker = undefined;
        nodes.push({id, kind: "dialogue", span: lineSpan, raw, text: spoken,
          ...speakerFields, ...(options === undefined ? {} : {options})});
      }
      continue;
    }
    if (trimmed.startsWith("[")) {
      if (!trimmed.endsWith("]")) {
        diagnostics.push(error("PXSTORY1012", "Command is missing a closing bracket", lineSpan));
        continue;
      }
      const inside = trimmed.slice(1, -1);
      const leadingWhitespace = inside.length - inside.trimStart().length;
      const commandText = inside.trim();
      const tokenized = tokenizeCommand(commandText);
      if (tokenized.error !== undefined || tokenized.tokens.length === 0) {
        diagnostics.push(error("PXSTORY1013", tokenized.error ?? "Command name is required", lineSpan));
        continue;
      }
      const [command, ...argumentTokens] = tokenized.tokens;
      nodes.push({
        id,
        kind: "command",
        span: lineSpan,
        raw,
        name: command!.raw,
        arguments: parseArguments(path, line, argumentTokens, diagnostics,
          startColumn + 1 + leadingWhitespace, command!.raw !== "if"),
      });
      continue;
    }
    nodes.push({
      id,
      kind: activeSpeaker === undefined ? "narration" : "dialogue",
      span: lineSpan,
      raw,
      text: prose,
      ...(activeSpeaker === undefined ? {} : {
        name: activeSpeaker.name,
        ...(activeSpeaker.literal ? {literalSpeaker: true} : {}),
      }),
    });
  }
  return {path, nodes, diagnostics};
}

function argumentMap(node: StoryNode): Map<string, StoryArgument> {
  const result = new Map<string, StoryArgument>();
  let positional = 0;
  for (const argument of node.arguments ?? []) {
    const name = argument.name ?? `$${positional++}`;
    result.set(name, argument);
  }
  return result;
}

function stringValue(argument: StoryArgument | undefined): string | undefined {
  if (argument === undefined || argument.value === null || typeof argument.value === "object") return undefined;
  return String(argument.value);
}

/** Relative character actions implemented by the native Stage. */
export const storyActorActions = [
  "hop", "jump", "startle", "yawn", "bow", "nod", "nods", "tremble",
  "shake-head", "sway", "float", "wiggle", "dip", "lift", "nudge-right",
  "nudge-left", "bob", "waver", "stop",
] as const;

function characterPosition(value: string | undefined): string | undefined {
  if (value === undefined) return undefined;
  const positions: Readonly<Record<string, string>> = {
    left:"1", center:"2", centre:"2", right:"3", "1":"1", "2":"2", "3":"3",
  };
  const key = value.trim().toLowerCase();
  return Object.hasOwn(positions, key) ? positions[key] : undefined;
}

function characterIndex(characters: readonly CharacterDocument[]): Map<string, CharacterDocument> {
  const result = new Map<string, CharacterDocument>();
  for (const character of characters) {
    result.set(character.id, character);
    for (const alias of character.aliases) result.set(alias, character);
  }
  return result;
}

function finiteNumbers(value: JsonValue, length: number): value is number[] {
  return Array.isArray(value) && value.length === length && value.every((item) => typeof item === "number" && Number.isFinite(item));
}

function typeMatches(value: JsonValue, parameter: ExtensionParameter): boolean {
  switch (parameter.type) {
    case "null": return value === null;
    case "boolean": return typeof value === "boolean";
    case "integer": return typeof value === "number" && Number.isInteger(value);
    case "number": return typeof value === "number" && Number.isFinite(value);
    case "string":
    case "token":
    case "uuid": return typeof value === "string";
    case "resource":
      return typeof value === "string" || (value !== null && !Array.isArray(value) && typeof value === "object" &&
        (typeof value.id === "string" || typeof value.uuid === "string" || typeof value.path === "string"));
    case "vec2":
      return finiteNumbers(value, 2) || (value !== null && !Array.isArray(value) && typeof value === "object" &&
        typeof value.x === "number" && Number.isFinite(value.x) && typeof value.y === "number" && Number.isFinite(value.y));
    case "rect": return finiteNumbers(value, 4);
    case "color": return finiteNumbers(value, 4) && value.every((channel) => Number.isInteger(channel) && channel >= 0 && channel <= 255);
    case "array": return Array.isArray(value);
    case "object": return value !== null && !Array.isArray(value) && typeof value === "object";
  }
}

function normalizeExtensionArguments(node: StoryNode, descriptor: ExtensionCommand, diagnostics: AuthoringDiagnostic[]): Record<string, JsonValue> | undefined {
  const authored = argumentMap(node);
  const normalized: Record<string, JsonValue> = {};
  const known = new Set(descriptor.parameters.map((parameter) => parameter.name));
  const initialErrors = diagnostics.filter((item) => item.severity === "error").length;
  for (const parameter of descriptor.parameters) {
    const argument = authored.get(parameter.name);
    const value = argument?.value ?? parameter.default;
    if (value === undefined) {
      if (parameter.required === true) diagnostics.push(error("PXSTORY1201", `Command ${descriptor.id} requires argument ${parameter.name}`, node.span));
      continue;
    }
    if (!typeMatches(value, parameter)) {
      diagnostics.push(error("PXSTORY1202", `Command ${descriptor.id} argument ${parameter.name} has the wrong type`, argument?.span ?? node.span));
      continue;
    }
    if (parameter.enum !== undefined && !parameter.enum.some((candidate) => canonicalJson(candidate, false) === canonicalJson(value, false))) {
      diagnostics.push(error("PXSTORY1203", `Command ${descriptor.id} argument ${parameter.name} is not an allowed value`, argument?.span ?? node.span));
    }
    if (typeof value === "number") {
      if (parameter.range?.minimum !== undefined && value < parameter.range.minimum) diagnostics.push(error("PXSTORY1204", `Command ${descriptor.id} argument ${parameter.name} is below its minimum`, argument?.span ?? node.span));
      if (parameter.range?.maximum !== undefined && value > parameter.range.maximum) diagnostics.push(error("PXSTORY1205", `Command ${descriptor.id} argument ${parameter.name} exceeds its maximum`, argument?.span ?? node.span));
    }
    normalized[parameter.name] = value;
  }
  for (const [name, argument] of authored) {
    if (name.startsWith("$") || known.has(name)) continue;
    if (descriptor.allowAdditionalParameters !== true) {
      diagnostics.push(error("PXSTORY1206", `Command ${descriptor.id} does not declare argument ${name}`, argument.span));
    } else {
      normalized[name] = argument.value;
    }
  }
  const finalErrors = diagnostics.filter((item) => item.severity === "error").length;
  return finalErrors === initialErrors ? normalized : undefined;
}

export function compileStory(document: StoryDocument, context: StoryCompileContext): StoryCompileResult {
  const diagnostics = [...document.diagnostics];
  const labels = new Map<string, StoryNode>();
  for (const node of document.nodes) {
    if (node.kind !== "label" || node.name === undefined) continue;
    if (labels.has(node.name)) diagnostics.push(error("PXSTORY1101", `Duplicate label: ${node.name}`, node.span));
    else labels.set(node.name, node);
  }
  const characters = characterIndex(context.characters ?? []);
  const variables = new Map((context.game?.variables ?? []).map((value) => [value.name, value]));
  const extensions = new Map<string, ExtensionCommand>();
  for (const extension of context.extensions ?? []) for (const command of extension.commands) extensions.set(command.id, command);
  const operations: RuntimeIrOperation[] = [];
  const mappings: RuntimeSourceMap["mappings"][number][] = [];
  const resourceDependencies = new Set<string>();
  const resourceLocations = new Map<string, StoryNode["span"]>();
  const rememberResource = (source: string, node: StoryNode,
    span = node.span): void => {
    resourceDependencies.add(source);
    if (!resourceLocations.has(source)) resourceLocations.set(source, span);
  };
  const authoredSourceIds = new Set<string>();
  let pendingAuthoredSourceId: string | undefined;

  const resolveResource = (node: StoryNode, authored: string,
    category?: StoryAssetCategory): string | undefined => {
    const argumentSpan = [...node.arguments ?? [], ...node.options ?? []]
      .find((argument) => argument.value === authored)?.span;
    let aliased = context.resources?.[authored];
    const lookup = aliased !== undefined || category === undefined ||
      assetIdFromReference(authored) !== undefined
      ? undefined : context.storyAssets?.resolve(category, authored);
    if (lookup?.kind === "resolved") aliased = lookup.path;
    else if (lookup?.kind === "ambiguous") {
      diagnostics.push(error("PXSTORY1161",
        `Story ${category} name ${authored} matches several files; write more of the path`,
        argumentSpan ?? node.span, lookup.candidates.join(", ")));
      return undefined;
    } else if (lookup?.kind === "missing" && !authored.includes("/") &&
               !/\.[A-Za-z0-9]+$/u.test(authored)) {
      // A bare name is only meaningful through storyAssets roots; do not
      // silently reinterpret a typo as a file at the project root.
      diagnostics.push(error("PXSTORY1160",
        `No ${category} asset named ${authored} under ${lookup.roots.join(", ")}`,
        argumentSpan ?? node.span));
      return undefined;
    }
    aliased ??= authored;
    const resolved = canonicalResourceReference(aliased, context.resourceAssets ?? []);
    if (resolved === undefined) {
      diagnostics.push(error(
        "PXSTORY1111",
        `Resource must be a safe project-relative path or a cataloged asset identity: ${authored}`,
        argumentSpan ?? node.span,
      ));
      return undefined;
    }
    rememberResource(resolved.source, node, argumentSpan);
    return resolved.reference;
  };

  const collectStructuredResource = (node: StoryNode, value: JsonValue): JsonValue | undefined => {
    if (typeof value === "string") return resolveResource(node, value);
    if (value === null || Array.isArray(value) || typeof value !== "object") return value;
    const identity = typeof value.id === "string" ? value.id
      : typeof value.uuid === "string" ? value.uuid : undefined;
    const fallbackPath = typeof value.path === "string" ? value.path : undefined;
    if (identity !== undefined) {
      const resolved = canonicalResourceReference(identity, context.resourceAssets ?? []);
      if (resolved !== undefined) {
        rememberResource(resolved.source, node);
        return value;
      }
      if (fallbackPath === undefined) {
        diagnostics.push(error("PXSTORY1111",
          `Resource identity is missing from the project asset catalog: ${identity}`,
          node.span));
        return undefined;
      }
    }
    if (fallbackPath !== undefined) {
      const resolved = canonicalResourceReference(fallbackPath, context.resourceAssets ?? []);
      if (resolved === undefined) {
        diagnostics.push(error("PXSTORY1111",
          `Resource path is not a safe project-relative path: ${fallbackPath}`,
          node.span));
        return undefined;
      }
      rememberResource(resolved.source, node);
    }
    return value;
  };

  // One authored line may emit several operations (for example an inline
  // expression change before dialogue). They share the line's source id and
  // stay distinct through their operation kind.
  const claimSourceId = (node: StoryNode): string => {
    const sourceId = pendingAuthoredSourceId === undefined
      ? node.externalSourceId ?? node.id
      : `story-${stableId(`${context.documentId}:${pendingAuthoredSourceId}`)}`;
    pendingAuthoredSourceId = undefined;
    return sourceId;
  };
  const emit = (node: StoryNode, kind: string, args: Record<string, string>,
    text = node.text ?? node.raw.trim(), sourceId = claimSourceId(node)): void => {
    const operationId = `op-${stableId(`${context.documentId}:${sourceId}:${kind}`)}`;
    operations.push({operationId, sourceId, sourceLine: node.span.start.line, kind, text, arguments: args});
    mappings.push({operationId, sourceId, sourceUri: document.path, startLine: node.span.start.line, startColumn: node.span.start.column, endLine: node.span.end.line, endColumn: node.span.end.column});
  };

  // Shared by [act] and inline `@alias[act=nod]` dialogue.
  const actionArguments = (node: StoryNode, alias: string, preset: string | undefined,
    options: {readonly size?: string; readonly time?: string; readonly loop?: string;
      readonly ease?: string}): Record<string, string> | undefined => {
    const character = characters.get(alias);
    if ((context.characters?.length ?? 0) > 0 && character === undefined) {
      diagnostics.push(error("PXSTORY1113", `Unknown character alias: ${alias}`, node.span));
      return undefined;
    }
    if (preset === undefined || !(storyActorActions as readonly string[]).includes(preset)) {
      diagnostics.push(error("PXSTORY1170",
        `act requires a preset: ${storyActorActions.join(", ")}`, node.span));
      return undefined;
    }
    if (options.size !== undefined && (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/u.test(options.size) ||
        Math.abs(Number(options.size)) > 4096)) {
      diagnostics.push(error("PXSTORY1171", "act size must be a number of at most 4096", node.span));
      return undefined;
    }
    if (options.time !== undefined && (durationMilliseconds(options.time) ?? Infinity) > 60_000) {
      diagnostics.push(error("PXSTORY1172", "act time must be a duration of at most 60s", node.span));
      return undefined;
    }
    if (options.loop !== undefined && (!/^(?:-1|[1-9]\d{0,2}|1000)$/u.test(options.loop))) {
      diagnostics.push(error("PXSTORY1173", "act loop must be 1-1000, or -1 to repeat until [act ... stop]", node.span));
      return undefined;
    }
    return {
      character: character?.id ?? alias, preset,
      ...(options.size === undefined ? {} : {size: options.size}),
      ...(options.time === undefined ? {} : {time: options.time}),
      ...(options.loop === undefined ? {} : {loop: options.loop}),
      ...(options.ease === undefined ? {} : {ease: options.ease}),
    };
  };

  // Shared by [show] and inline `@alias[expression position]` dialogue.
  const showArguments = (node: StoryNode, alias: string,
    authored: {readonly expression?: string; readonly position?: string;
      readonly asset?: string}): Record<string, string> | undefined => {
    const character = characters.get(alias);
    if ((context.characters?.length ?? 0) > 0 && character === undefined) {
      diagnostics.push(error("PXSTORY1113", `Unknown character alias: ${alias}`, node.span));
      return undefined;
    }
    const expression = authored.expression === undefined ? undefined
      : character?.expressions.find((value) => value.id === authored.expression ||
        value.aliases.includes(authored.expression!));
    let valid = true;
    if (authored.expression !== undefined && character !== undefined && expression === undefined) {
      diagnostics.push(error("PXSTORY1114", `Unknown expression ${authored.expression} for ${alias}`, node.span));
      valid = false;
    }
    const position = characterPosition(authored.position);
    if (authored.position !== undefined && position === undefined) {
      diagnostics.push(error("PXSTORY1128", `Unknown character position: ${authored.position}; expected left, center, right, 1, 2, or 3`, node.span));
      valid = false;
    }
    const sprite = authored.asset === undefined
      ? (expression === undefined ? undefined : `asset:${expression.assetId}`)
      : resolveResource(node, authored.asset, "sprite");
    if (!valid || (authored.asset !== undefined && sprite === undefined)) return undefined;
    if (authored.asset === undefined && expression !== undefined && context.resourceAssets !== undefined)
      resolveResource(node, `asset:${expression.assetId}`);
    return {
      character: character?.id ?? alias,
      ...(expression === undefined ? {} : {expression: expression.id}),
      ...(sprite === undefined ? {} : {sprite}),
      ...(position === undefined ? {} : {position}),
    };
  };

  for (const node of document.nodes) {
    if (node.kind === "comment" || node.kind === "speaker") continue;
    if (node.kind === "label") {
      if (node.name !== undefined) emit(node, "label", {target: node.name});
      continue;
    }
    if (node.kind === "dialogue") {
      const character = node.literalSpeaker || node.name === undefined ?
        undefined : characters.get(node.name);
      if (!node.literalSpeaker && (context.characters?.length ?? 0) > 0 &&
          character === undefined)
        diagnostics.push(error("PXSTORY1102", `Unknown character alias: ${node.name ?? ""}`, node.span));
      const localizedName = context.locale === undefined ? undefined :
        character?.displayNames?.[context.locale];
      // `@alias[smile left voice=line042] text`: positional options are an
      // expression or a stage position; `voice` plays with this line only.
      let expression: string | undefined;
      let position: string | undefined;
      let voice: string | undefined;
      let act: string | undefined;
      let optionsValid = true;
      for (const option of node.options ?? []) {
        const value = stringValue(option);
        const slot = option.name ?? (characterPosition(value) === undefined ? "expression" : "position");
        if (value === undefined || !["expression", "position", "voice", "act"].includes(slot)) {
          diagnostics.push(error("PXSTORY1017", `Unknown speaker option: ${option.name ?? option.raw}`, option.span));
          optionsValid = false;
        } else if ((slot === "expression" && expression !== undefined) ||
                   (slot === "position" && position !== undefined) ||
                   (slot === "voice" && voice !== undefined) ||
                   (slot === "act" && act !== undefined)) {
          diagnostics.push(error("PXSTORY1016", `Duplicate speaker option: ${slot}`, option.span));
          optionsValid = false;
        } else if (slot === "expression") expression = value;
        else if (slot === "position") position = value;
        else if (slot === "act") act = value;
        else voice = value;
      }
      if (!optionsValid) continue;
      const sourceId = claimSourceId(node);
      if (expression !== undefined || position !== undefined || act !== undefined) {
        if (node.literalSpeaker || node.name === undefined) {
          diagnostics.push(error("PXSTORY1162",
            "Speaker expression, position, and act options require a character alias", node.span));
          continue;
        }
      }
      if (expression !== undefined || position !== undefined) {
        if (node.name === undefined) continue;
        const shown = showArguments(node, node.name, {
          ...(expression === undefined ? {} : {expression}),
          ...(position === undefined ? {} : {position}),
        });
        if (shown === undefined) continue;
        emit(node, "showCharacter", shown, node.raw.trim(), sourceId);
      }
      if (act !== undefined && node.name !== undefined) {
        const action = actionArguments(node, node.name, act, {});
        if (action === undefined) continue;
        emit(node, "actorAction", action, node.raw.trim(), sourceId);
      }
      const resolvedVoice = voice === undefined ? undefined : resolveResource(node, voice, "voice");
      if (voice !== undefined && resolvedVoice === undefined) continue;
      emit(node, "dialogue", {speaker: localizedName ?? character?.displayName ?? node.name ?? "", character: node.literalSpeaker ? "" : character?.id ?? node.name ?? "", text: node.text ?? "",
        ...(resolvedVoice === undefined ? {} : {voice: resolvedVoice})}, node.text ?? "", sourceId);
      continue;
    }
    if (node.kind === "narration") {
      emit(node, "narration", {text: node.text ?? ""}, node.text ?? "");
      continue;
    }
    if (node.kind !== "command" || node.name === undefined) continue;
    const args = argumentMap(node);
    const allowed = Object.hasOwn(builtinNamedArguments, node.name)
      ? builtinNamedArguments[node.name] : undefined;
    if (allowed !== undefined) {
      let unknown = false;
      for (const [name, argument] of args) {
        if (name.startsWith("$") || allowed.includes(name)) continue;
        diagnostics.push(error("PXSTORY1017",
          `Unknown ${node.name} argument: ${name}`, argument.span));
        unknown = true;
      }
      if (unknown) continue;
    }
    const positional = (index: number): string | undefined => stringValue(args.get(`$${index}`));
    const named = (name: string): string | undefined => stringValue(args.get(name));
    const target = named("target") ?? named("goto") ?? positional(0);
    switch (node.name) {
      case "id": {
        const identity = named("value") ?? positional(0);
        if (identity === undefined || !identifierPattern.test(identity)) {
          diagnostics.push(error("PXSTORY1210", "id requires a stable identifier", node.span));
        } else if (pendingAuthoredSourceId !== undefined) {
          diagnostics.push(error("PXSTORY1211", "id must be followed by one observable Story operation", node.span));
        } else if (authoredSourceIds.has(identity)) {
          diagnostics.push(error("PXSTORY1212", `Duplicate authored Story id: ${identity}`, node.span));
        } else {
          authoredSourceIds.add(identity);
          pendingAuthoredSourceId = identity;
        }
        break;
      }
      case "choice.wait": break;
      case "say":
      case "narrate": {
        const authoredText = args.get("text")?.value;
        const authoredSpeaker = args.get("speaker")?.value;
        const authoredCharacter = args.get("character")?.value;
        if (typeof authoredText !== "string" || authoredText.length === 0 ||
            (node.name === "say" &&
             (typeof authoredSpeaker !== "string" || authoredSpeaker.trim().length === 0)) ||
            (authoredCharacter !== undefined && typeof authoredCharacter !== "string")) {
          diagnostics.push(error("PXSTORY1149",
            `${node.name} requires nonempty string text${node.name === "say" ? " and speaker" : ""}`,
            node.span));
          break;
        }
        if (node.name === "say") {
          const character = typeof authoredCharacter === "string" && authoredCharacter.length > 0
            ? characters.get(authoredCharacter) : undefined;
          if (authoredCharacter !== undefined && authoredCharacter !== "" &&
              (context.characters?.length ?? 0) > 0 && character === undefined) {
            diagnostics.push(error("PXSTORY1102", `Unknown character alias: ${authoredCharacter}`, node.span));
            break;
          }
          emit(node, "dialogue", {speaker: authoredSpeaker as string,
            character: character?.id ?? (authoredCharacter as string | undefined) ?? "",
            text: authoredText}, authoredText);
        } else emit(node, "narration", {text: authoredText}, authoredText);
        break;
      }
      case "chapter": {
        const title = named("title") ?? positional(0);
        if (!title) diagnostics.push(error("PXSTORY1144", "chapter requires a title", node.span));
        else emit(node, "scene", {title});
        break;
      }
      case "bg": {
        const assetName = named("asset") ?? positional(0);
        if (assetName === undefined) diagnostics.push(error("PXSTORY1110", "bg requires a resource", node.span));
        else {
          const resolved = resolveResource(node, assetName, "bg");
          const authoredRule = named("rule");
          const rule = authoredRule === undefined ? undefined : resolveResource(node, authoredRule, "rule");
          if (resolved !== undefined && (authoredRule === undefined || rule !== undefined)) emit(node, "background", {
            asset: resolved,
            ...(rule === undefined ? {} : {rule}),
            ...(named("duration") === undefined ? {} : {duration: named("duration")!}),
            ...(named("vague") === undefined ? {} : {vague: named("vague")!}),
          });
        }
        break;
      }
      case "show": {
        // [show kei smile left]: after the alias, each positional token is a
        // stage position keyword or the expression, in any order.
        const alias = named("character") ?? positional(0);
        const authoredAsset = named("asset") ?? named("sprite");
        let expressionAlias = named("expression");
        let authoredPosition = named("position");
        let extrasValid = true;
        for (let index = named("character") === undefined ? 1 : 0; args.has(`$${index}`); index += 1) {
          const value = positional(index);
          const isPosition = characterPosition(value) !== undefined;
          if (value !== undefined && isPosition && authoredPosition === undefined) authoredPosition = value;
          else if (value !== undefined && !isPosition && expressionAlias === undefined) expressionAlias = value;
          else {
            diagnostics.push(error("PXSTORY1016", `Duplicate show ${isPosition ? "position" : "expression"}: ${value ?? ""}`,
              args.get(`$${index}`)!.span));
            extrasValid = false;
          }
        }
        const transition = named("transition");
        if (alias === undefined) diagnostics.push(error("PXSTORY1112", "show requires a character alias", node.span));
        else if (transition !== undefined && transition !== "true" && transition !== "false" && transition !== "auto")
          diagnostics.push(error("PXSTORY1145", "show transition must be true, false, or auto", node.span));
        else if (extrasValid) {
          const shown = showArguments(node, alias, {
            ...(expressionAlias === undefined ? {} : {expression: expressionAlias}),
            ...(authoredPosition === undefined ? {} : {position: authoredPosition}),
            ...(authoredAsset === undefined ? {} : {asset: authoredAsset}),
          });
          if (shown !== undefined) emit(node, "showCharacter", {
            ...shown,
            ...(named("x") === undefined ? {} : {x: named("x")!}),
            ...(named("y") === undefined ? {} : {y: named("y")!}),
            ...(named("scale") === undefined ? {} : {scale: named("scale")!}),
            ...(transition === undefined ? {} : {transition}),
          });
        }
        break;
      }
      case "move": {
        const alias = named("character") ?? positional(0);
        const character = alias === undefined ? undefined : characters.get(alias);
        const authoredPosition = named("position");
        const position = characterPosition(authoredPosition);
        if (alias === undefined) diagnostics.push(error("PXSTORY1129", "move requires a character alias", node.span));
        else if ((context.characters?.length ?? 0) > 0 && character === undefined) diagnostics.push(error("PXSTORY1130", `Unknown character alias: ${alias}`, node.span));
        else if (authoredPosition !== undefined && position === undefined) diagnostics.push(error("PXSTORY1131", `Unknown character position: ${authoredPosition}; expected left, center, right, 1, 2, or 3`, node.span));
        else if ([authoredPosition, named("x"), named("y"), named("scale")].every((value) => value === undefined)) diagnostics.push(error("PXSTORY1132", "move requires position, x, y, or scale", node.span));
        else emit(node, "timeline", {
          mode: "move",
          target: character?.id ?? alias,
          ...(position === undefined ? {} : {position}),
          ...(named("x") === undefined ? {} : {x: named("x")!}),
          ...(named("y") === undefined ? {} : {y: named("y")!}),
          ...(named("scale") === undefined ? {} : {scale: named("scale")!}),
          ...(named("duration") === undefined ? {} : {duration: named("duration")!}),
          ...(named("ease") === undefined ? {} : {ease: named("ease")!}),
          ...(named("wait") === undefined ? {} : {wait: named("wait")!}),
        });
        break;
      }
      case "hide": {
        const alias = named("character") ?? positional(0);
        const character = alias === undefined ? undefined : characters.get(alias);
        const transition = named("transition");
        if (alias === undefined) diagnostics.push(error("PXSTORY1115", "hide requires a character alias", node.span));
        else if (transition !== undefined && transition !== "true" && transition !== "false") {
          diagnostics.push(error("PXSTORY1145", "hide transition must be true or false", node.span));
        } else if (named("character") === undefined && positional(0) === "all") {
          emit(node, "hideCharacters", transition === undefined ? {} : {transition});
        } else {
          emit(node, "hideCharacter", {character: character?.id ?? alias,
            ...(transition === undefined ? {} : {transition})});
        }
        break;
      }
      case "choice":
        if (args.has("scene")) diagnostics.push(error("PXSTORY1215", "Cross-scene choice requires compileStoryProject; compileStory only resolves labels in one document", node.span));
        else if (target === undefined || named("text") === undefined) diagnostics.push(error("PXSTORY1116", "choice requires text and goto", node.span));
        else {
          if (!labels.has(target)) diagnostics.push(error("PXSTORY1117", `Unknown choice target: ${target}`, node.span));
          emit(node, "choiceOption", {text: named("text")!, target}, named("text")!);
        }
        break;
      case "jump":
        if (args.has("scene")) diagnostics.push(error("PXSTORY1215", "Cross-scene jump requires compileStoryProject; compileStory only resolves labels in one document", node.span));
        else if (target === undefined || !labels.has(target)) diagnostics.push(error("PXSTORY1118", `Unknown jump target: ${target ?? ""}`, node.span));
        else emit(node, "jump", {target});
        break;
      case "call":
        if (args.has("scene")) diagnostics.push(error("PXSTORY1215", "Cross-scene call requires compileStoryProject; compileStory only resolves labels in one document", node.span));
        else if (target === undefined || !labels.has(target)) diagnostics.push(error("PXSTORY1119", `Unknown call target: ${target ?? ""}`, node.span));
        else emit(node, "callFragment", {target});
        break;
      case "return": emit(node, "return", {}); break;
      case "end": emit(node, "endStory", {}); break;
      case "wait": {
        const duration = named("duration") ?? positional(0);
        if (duration === undefined) diagnostics.push(error("PXSTORY1120", "wait requires a duration", node.span));
        else emit(node, "wait", {duration});
        break;
      }
      case "set": {
        let variableName = named("name");
        let value = named("value");
        if (variableName === undefined) {
          const authored = [...args.entries()].find(([name]) => !name.startsWith("$"));
          variableName = authored?.[0];
          value = authored === undefined ? undefined : stringValue(authored[1]);
        }
        const descriptor = variableName === undefined ? undefined : variables.get(variableName);
        if (variableName === undefined || value === undefined) diagnostics.push(error("PXSTORY1121", "set requires a variable and value", node.span));
        else if (context.game !== undefined && descriptor === undefined) diagnostics.push(error("PXSTORY1122", `Unknown game variable: ${variableName}`, node.span));
        else {
          const parsed = decodeValue(value).value;
          const matches = descriptor === undefined || (descriptor.type === "integer" ? typeof parsed === "number" && Number.isInteger(parsed) : typeof parsed === descriptor.type);
          if (!matches) diagnostics.push(error("PXSTORY1123", `Value for ${variableName} does not match ${descriptor!.type}`, node.span));
          else emit(node, "setVariable", {
            name: variableName,
            value,
            ...(descriptor === undefined ? {} : {scope: descriptor.scope, type: descriptor.type}),
          });
        }
        break;
      }
      case "add": {
        const variableName = named("name") ?? positional(0);
        const amount = named("amount") ?? positional(1);
        const descriptor = variableName === undefined ? undefined : variables.get(variableName);
        if (variableName === undefined || amount === undefined ||
            !/^-?\d+$/u.test(amount) || !Number.isSafeInteger(Number(amount)))
          diagnostics.push(error("PXSTORY1150", "add requires a variable and a safe integer amount", node.span));
        else if (context.game !== undefined && descriptor === undefined)
          diagnostics.push(error("PXSTORY1122", `Unknown game variable: ${variableName}`, node.span));
        else if (descriptor !== undefined && descriptor.type !== "integer")
          diagnostics.push(error("PXSTORY1151", `add requires an integer variable: ${variableName}`, node.span));
        else emit(node, "setVariable", {
          name: variableName,
          add: amount,
          ...(descriptor === undefined ? {} : {scope: descriptor.scope}),
        });
        break;
      }
      case "if": {
        const source = (node.arguments ?? []).map((value) => value.raw).join(" ");
        const expression = parseStoryExpression(source, context.game?.variables);
        if (expression.error !== undefined || expression.encoded === undefined) {
          diagnostics.push(error(
            "PXSTORY1133",
            `Invalid condition expression: ${expression.error ?? "unknown error"}`,
            node.span,
          ));
        } else {
          emit(node, "condition", {expression: expression.encoded});
        }
        break;
      }
      case "else": emit(node, "else", {}); break;
      case "endif": emit(node, "endCondition", {}); break;
      case "voice":
      case "bgm":
      case "ambience":
      case "se": {
        const asset = named("asset") ?? positional(0);
        const fade = named("fade");
        const loop = named("loop");
        const intro = named("intro");
        const durable = node.name === "bgm" || node.name === "ambience";
        if (asset === undefined) diagnostics.push(error("PXSTORY1124", `${node.name} requires a resource`, node.span));
        else if (fade !== undefined && (!durable || !validAudioDuration(fade)))
          diagnostics.push(error("PXSTORY1146", `${node.name} fade must be a nonnegative duration of at most 2147483647ms; voice and se do not support fade`, node.span));
        else if (loop !== undefined && (!durable || (loop !== "true" && loop !== "false")))
          diagnostics.push(error("PXSTORY1147", `${node.name} loop must be true or false; voice and se do not support loop`, node.span));
        else if (intro !== undefined && (node.name !== "bgm" || intro.length === 0 || loop === "false"))
          diagnostics.push(error("PXSTORY1148", "intro requires bgm with a looping body", node.span));
        else {
          const category = node.name === "bgm" || node.name === "ambience" || node.name === "voice"
            ? node.name : "se";
          const resolved = resolveResource(node, asset, category);
          const resolvedIntro = intro === undefined ? undefined : resolveResource(node, intro, category);
          if (resolved !== undefined && (intro === undefined || resolvedIntro !== undefined)) {
            emit(node, node.name === "se" ? "soundEffect" : node.name, {
              asset: resolved,
              ...(fade === undefined ? {} : {fade}),
              ...(loop === undefined ? {} : {loop}),
              ...(resolvedIntro === undefined ? {} : {intro: resolvedIntro}),
            });
          }
        }
        break;
      }
      case "act": {
        const alias = named("character") ?? positional(0);
        const preset = named("preset") ?? positional(named("character") === undefined ? 1 : 0);
        if (alias === undefined) {
          diagnostics.push(error("PXSTORY1112", "act requires a character alias", node.span));
          break;
        }
        const action = actionArguments(node, alias, preset, {
          ...(named("size") === undefined ? {} : {size: named("size")!}),
          ...(named("time") === undefined ? {} : {time: named("time")!}),
          ...(named("loop") === undefined ? {} : {loop: named("loop")!}),
          ...(named("ease") === undefined ? {} : {ease: named("ease")!}),
        });
        if (action !== undefined) emit(node, "actorAction", action);
        break;
      }
      case "se.stop": {
        const fade = named("fade") ?? positional(0);
        if (fade !== undefined && !validAudioDuration(fade))
          diagnostics.push(error("PXSTORY1146", "se.stop fade must be a nonnegative duration of at most 2147483647ms", node.span));
        else emit(node, "stopSoundEffect", fade === undefined ? {} : {fade});
        break;
      }
      case "voice.stop": emit(node, "stopVoice", {}); break;
      case "bgm.stop":
      case "ambience.stop": {
        const fade = named("fade") ?? positional(0);
        if (fade !== undefined && !validAudioDuration(fade))
          diagnostics.push(error("PXSTORY1146", `${node.name} fade must be a nonnegative duration of at most 2147483647ms`, node.span));
        else emit(node, node.name === "bgm.stop" ? "stopBgm" : "stopAmbience", {
          ...(fade === undefined ? {} : {fade}),
        });
        break;
      }
      case "video": {
        const asset = named("asset") ?? positional(0);
        if (asset === undefined) diagnostics.push(error("PXSTORY1136", "video requires a resource", node.span));
        else {
          const resolved = resolveResource(node, asset, "video");
          if (resolved !== undefined) {
            const skippable = named("skippable");
            if (skippable !== undefined && skippable !== "true" && skippable !== "false") {
              diagnostics.push(error("PXSTORY1137", "video skippable must be true or false", node.span));
            } else {
              emit(node, "video", {
                asset: resolved,
                ...(skippable === undefined ? {} : {skippable}),
              });
            }
          }
        }
        break;
      }
      case "timeline": {
        const timeline = named("timeline") ?? positional(0);
        if (timeline === undefined) diagnostics.push(error("PXSTORY1125", "timeline requires a clip", node.span));
        else {
          const resolved = resolveResource(node, timeline);
          const awaitPlayback = named("await");
          const speed = named("speed");
          if (awaitPlayback !== undefined && awaitPlayback !== "true" && awaitPlayback !== "false")
            diagnostics.push(error("PXSTORY1147", "timeline await must be true or false", node.span));
          else if (speed !== undefined && (!positiveNumberPattern.test(speed) ||
              !Number.isFinite(Math.fround(Number(speed))) || Math.fround(Number(speed)) <= 0))
            diagnostics.push(error("PXSTORY1148", "timeline speed must be a positive finite playback rate", node.span));
          else if (resolved !== undefined) emit(node, "timeline", {
            timeline: resolved,
            ...(awaitPlayback === undefined ? {} : {await: awaitPlayback}),
            ...(speed === undefined ? {} : {speed}),
          });
        }
        break;
      }
      case "motion": {
        const asset = named("asset") ?? positional(0);
        if (asset === undefined) diagnostics.push(error("PXSTORY1220", "motion requires a .pxmotion resource", node.span));
        else {
          const resolved = resolveResource(node, asset);
          const awaitPlayback = named("await");
          const speed = named("speed");
          const source = canonicalResourceReference(
            context.resources?.[asset] ?? asset, context.resourceAssets ?? [],
          )?.source;
          if (source !== undefined && !source.endsWith(".pxmotion"))
            diagnostics.push(error("PXSTORY1221", "motion asset must be a .pxmotion resource", node.span));
          else if (awaitPlayback !== undefined && awaitPlayback !== "true" && awaitPlayback !== "false")
            diagnostics.push(error("PXSTORY1222", "motion await must be true or false", node.span));
          else if (speed !== undefined && (!positiveNumberPattern.test(speed) ||
              !Number.isFinite(Number(speed)) || Number(speed) <= 0 || Number(speed) > 16))
            diagnostics.push(error("PXSTORY1223", "motion speed must be between 0 and 16", node.span));
          else if (resolved !== undefined) emit(node, "timeline", {
            mode: "stage-motion", timeline: resolved,
            ...(awaitPlayback === undefined ? {} : {await: awaitPlayback}),
            ...(speed === undefined ? {} : {speed}),
          });
        }
        break;
      }
      case "motion.stop":
        emit(node, "timeline", {mode: "stage-motion-stop"});
        break;
      case "ui": emit(node, "ui", {route: named("route") ?? positional(0) ?? "", operation: named("operation") ?? "push"}); break;
      case "hud": {
        const visible = named("visible") ?? positional(0);
        if (visible !== "true" && visible !== "false")
          diagnostics.push(error("PXSTORY1152", "hud visible must be true or false", node.span));
        else emit(node, "hudVisibility", {visible});
        break;
      }
      case "unlock": {
        const kind = named("kind") ?? positional(0);
        const id = named("id") ?? positional(1);
        if (kind === undefined || id === undefined) {
          diagnostics.push(error("PXSTORY1134", "unlock requires kind and id", node.span));
        } else if (!identifierPattern.test(kind) || !identifierPattern.test(id)) {
          diagnostics.push(error("PXSTORY1135", "unlock kind and id must be stable identifiers", node.span));
        } else {
          emit(node, "unlock", {kind, id});
        }
        break;
      }
      case "effect": {
        const value = named("value") ?? positional(0);
        if (value === undefined || value.length === 0)
          diagnostics.push(error("PXSTORY1126", "effect requires a preset", node.span));
        else emit(node, "effect", {value});
        break;
      }
      case "shake": {
        const duration = named("duration") ?? positional(0);
        const amplitude = named("amplitude") ?? positional(1);
        const target = named("target");
        if (duration === undefined) diagnostics.push(error("PXSTORY1138", "shake requires a duration", node.span));
        else if (target !== undefined && target !== "stage" && target !== "window")
          diagnostics.push(error("PXSTORY1139", "shake target must be stage or window", node.span));
        else emit(node, "shake", {
          duration,
          ...(amplitude === undefined ? {} : {amplitude}),
          ...(target === undefined || target === "stage" ? {} : {target}),
        });
        break;
      }
      case "camera": {
        const value = named("value") ?? positional(0);
        const x = named("x");
        const y = named("y");
        const zoom = named("zoom");
        const duration = named("duration");
        const ease = named("ease");
        const wait = named("wait");
        const hasCoordinates = x !== undefined || y !== undefined || zoom !== undefined;
        if (value !== undefined && hasCoordinates)
          diagnostics.push(error("PXSTORY1220", "camera preset cannot be combined with x, y, or zoom", node.span));
        else if (value !== undefined && value.length > 0) emit(node, "camera", {value});
        else if (!hasCoordinates)
          diagnostics.push(error("PXSTORY1127", "camera requires a preset or x, y, or zoom", node.span));
        else if ([x, y].some((coordinate) => coordinate !== undefined &&
          (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/u.test(coordinate) || !Number.isFinite(Number(coordinate)))) ||
          (zoom !== undefined && (!positiveNumberPattern.test(zoom) || Number(zoom) <= 0 || !Number.isFinite(Number(zoom)))))
          diagnostics.push(error("PXSTORY1221", "camera x and y must be finite numbers; zoom must be positive", node.span));
        else if (duration !== undefined && !validAudioDuration(duration))
          diagnostics.push(error("PXSTORY1222", "camera duration must be a non-negative number of ms or s", node.span));
        else if (wait !== undefined && wait !== "true" && wait !== "false")
          diagnostics.push(error("PXSTORY1223", "camera wait must be true or false", node.span));
        else emit(node, "camera", {
          ...(x === undefined ? {} : {x}), ...(y === undefined ? {} : {y}),
          ...(zoom === undefined ? {} : {zoom}),
          ...(duration === undefined ? {} : {duration}),
          ...(ease === undefined ? {} : {ease}),
          ...(wait === undefined ? {} : {wait}),
        });
        break;
      }
      default: {
        const descriptor = extensions.get(node.name);
        if (descriptor === undefined) {
          diagnostics.push(error("PXSTORY1199", `Unknown Story command: ${node.name}`, node.span));
          break;
        }
        const normalized = normalizeExtensionArguments(node, descriptor, diagnostics);
        if (normalized !== undefined) {
          for (const parameter of descriptor.parameters) {
            const value = normalized[parameter.name];
            if (parameter.type !== "resource" || value === undefined) continue;
            const collected = collectStructuredResource(node, value);
            if (collected !== undefined) normalized[parameter.name] = collected;
          }
          emit(node, "customNode", {type: descriptor.id, value: canonicalJson(normalized, false).trim()});
        }
      }
    }
  }

  if (pendingAuthoredSourceId !== undefined) {
    diagnostics.push(error(
      "PXSTORY1213",
      `Story id ${pendingAuthoredSourceId} is not followed by an observable operation`,
    ));
  }

  const valid = !diagnostics.some((item) => item.severity === "error");
  const locations = [...resourceLocations].map(([source, span]) => ({source, span}));
  if (!valid) return {resourceDependencies: [...resourceDependencies].sort(),
    resourceLocations: locations, diagnostics, valid: false};
  return {
    runtimeIr: {format: "PrismatiXRuntimeIR", schemaRevision: 2, documentId: context.documentId, committedRevision: context.committedRevision ?? 0, operations},
    sourceMap: {format: "PrismatiXSourceMap", schemaRevision: 2, documentId: context.documentId, mappings},
    resourceDependencies: [...resourceDependencies].sort(),
    resourceLocations: locations,
    diagnostics,
    valid: true,
  };
}

export interface StoryCommandCompletion {
  readonly id: string;
  readonly parameters: readonly {readonly name: string; readonly required: boolean; readonly values?: readonly JsonValue[]}[];
}

export function storyCommandCompletions(extensions: readonly ExtensionCommand[] = []): readonly StoryCommandCompletion[] {
  const builtins = ["id", "chapter", "say", "narrate", "bg", "show", "move", "hide", "choice", "choice.wait", "jump", "call", "return", "end", "wait", "set", "add", "if", "else", "endif", "voice", "bgm", "bgm.stop", "se", "ambience", "ambience.stop", "video", "timeline", "motion", "motion.stop", "ui", "unlock", "effect", "shake", "camera", "act", "se.stop", "voice.stop"];
  const builtinParameters: Readonly<Record<string, StoryCommandCompletion["parameters"]>> = {
    id: [{name:"value", required:true}],
    chapter: [{name:"title", required:true}],
    say: [{name:"speaker", required:true}, {name:"text", required:true},
      {name:"character", required:false}],
    narrate: [{name:"text", required:true}],
    add: [{name:"name", required:true}, {name:"amount", required:true}],
    bg: [
      {name:"asset", required:true}, {name:"rule", required:false},
      {name:"duration", required:false}, {name:"vague", required:false},
    ],
    show: [
      {name:"character", required:true}, {name:"expression", required:false}, {name:"asset", required:false},
      {name:"position", required:false, values:["left", "center", "right"]},
      {name:"x", required:false}, {name:"y", required:false}, {name:"scale", required:false},
      {name:"transition", required:false, values:[true, false, "auto"]},
    ],
    move: [
      {name:"character", required:true},
      {name:"position", required:false, values:["left", "center", "right"]},
      {name:"x", required:false}, {name:"y", required:false}, {name:"scale", required:false},
      {name:"duration", required:false}, {name:"ease", required:false}, {name:"wait", required:false},
    ],
    hide: [
      {name:"character", required:true}, {name:"transition", required:false, values:[true, false]},
    ],
    choice: [{name:"text", required:true}, {name:"goto", required:false},
      {name:"target", required:false}, {name:"scene", required:false}],
    wait: [{name:"duration", required:true}],
    voice: [{name:"asset", required:true}],
    se: [{name:"asset", required:true}],
    bgm: [{name:"asset", required:true}, {name:"fade", required:false},
      {name:"loop", required:false, values:[true, false]}, {name:"intro", required:false}],
    "bgm.stop": [{name:"fade", required:false}],
    ambience: [{name:"asset", required:true}, {name:"fade", required:false},
      {name:"loop", required:false, values:[true, false]}],
    "ambience.stop": [{name:"fade", required:false}],
    video: [{name:"asset", required:true}, {name:"skippable", required:false, values:[true, false]}],
    timeline: [{name:"timeline", required:true},
      {name:"await", required:false, values:[true, false]}, {name:"speed", required:false}],
    motion: [{name:"asset", required:true},
      {name:"await", required:false, values:[true, false]}, {name:"speed", required:false}],
    shake: [{name:"duration", required:true}, {name:"amplitude", required:false},
      {name:"target", required:false, values:["stage", "window"]}],
    act: [{name:"character", required:true},
      {name:"preset", required:true, values:[...storyActorActions]},
      {name:"size", required:false}, {name:"time", required:false},
      {name:"loop", required:false}, {name:"ease", required:false,
        values:["linear", "in", "out", "inout"]}],
    "se.stop": [{name:"fade", required:false}],
    unlock: [{name:"kind", required:true}, {name:"id", required:true}],
    effect: [{name:"value", required:true}],
    camera: [{name:"value", required:false}, {name:"x", required:false},
      {name:"y", required:false}, {name:"zoom", required:false},
      {name:"duration", required:false}, {name:"ease", required:false},
      {name:"wait", required:false, values:[true, false]}],
  };
  return [
    ...builtins.map((id) => ({id, parameters: builtinParameters[id] ??
      (builtinNamedArguments[id] ?? []).map((name) => ({name, required:false}))})),
    ...extensions.map((command) => ({id: command.id, parameters: command.parameters.map((parameter) => ({name: parameter.name, required: parameter.required === true, ...(parameter.enum === undefined ? {} : {values: parameter.enum})}))})),
  ];
}
