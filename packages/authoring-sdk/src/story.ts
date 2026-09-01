import {canonicalJson} from "./canonical-json.js";
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

interface SourceLine {
  readonly text: string;
  readonly line: number;
  readonly offset: number;
}

function sourceLines(text: string): SourceLine[] {
  const result: SourceLine[] = [];
  let offset = 0;
  let line = 1;
  while (offset < text.length) {
    const newline = text.indexOf("\n", offset);
    const end = newline < 0 ? text.length : newline;
    const raw = text.slice(offset, end).replace(/\r$/u, "");
    result.push({text: raw, line, offset});
    if (newline < 0) break;
    offset = newline + 1;
    line += 1;
  }
  if (text.length === 0 || text.endsWith("\n")) result.push({text: "", line, offset: text.length});
  return result;
}

function span(path: string, line: SourceLine, startColumn: number, endColumn: number): SourceSpan {
  return {
    path,
    start: {line: line.line, column: startColumn, offset: line.offset + startColumn - 1},
    end: {line: line.line, column: endColumn, offset: line.offset + endColumn - 1},
  };
}

function stableId(input: string): string {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(input)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 0x100000001b3n);
  }
  return hash.toString(16).padStart(16, "0");
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

function parseArguments(path: string, line: SourceLine, tokens: readonly Token[], diagnostics: AuthoringDiagnostic[]): StoryArgument[] {
  return tokens.map((token) => {
    const equals = token.raw.indexOf("=");
    const name = equals > 0 ? token.raw.slice(0, equals) : undefined;
    const raw = equals > 0 ? token.raw.slice(equals + 1) : token.raw;
    const start = line.text.indexOf("[") + 2 + token.start + (equals > 0 ? equals + 1 : 0);
    const argumentSpan = span(path, line, start, start + raw.length);
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

export function parseStory(text: string, path = "Story/untitled.pxstory"): StoryDocument {
  const diagnostics: AuthoringDiagnostic[] = [];
  const nodes: StoryNode[] = [];
  if (Buffer.byteLength(text, "utf8") > maximumStoryBytes) {
    diagnostics.push(error("PXSTORY1001", "Story exceeds the 16 MiB limit"));
    return {path, nodes, diagnostics};
  }
  if (text.startsWith("\uFEFF")) {
    diagnostics.push(error("PXSTORY1002", "Story must be UTF-8 without a BOM"));
    text = text.slice(1);
  }

  let activeSpeaker: string | undefined;
  for (const line of sourceLines(text)) {
    const trimmed = line.text.trim();
    if (trimmed.length === 0) {
      activeSpeaker = undefined;
      continue;
    }
    const startColumn = line.text.indexOf(trimmed) + 1;
    const lineSpan = span(path, line, startColumn, startColumn + trimmed.length);
    // Deterministic fallback for unannotated source. Published save/seen
    // identity can be pinned independently of path, offset, and prose with
    // an [id ...] directive immediately before an observable operation.
    const id = `story-${stableId(`${path}:${line.offset}:${trimmed}`)}`;
    if (trimmed.startsWith(";")) {
      nodes.push({id, kind: "comment", span: lineSpan, raw: line.text, text: trimmed.slice(1).trim()});
      continue;
    }
    if (trimmed.startsWith("*")) {
      const name = trimmed.slice(1).trim();
      if (!identifierPattern.test(name)) diagnostics.push(error("PXSTORY1010", "Label must be a stable identifier", lineSpan));
      nodes.push({id, kind: "label", span: lineSpan, raw: line.text, name});
      continue;
    }
    if (trimmed.startsWith("@")) {
      const name = trimmed.slice(1).trim();
      if (!identifierPattern.test(name)) diagnostics.push(error("PXSTORY1011", "Speaker must be a character alias", lineSpan));
      activeSpeaker = name;
      nodes.push({id, kind: "speaker", span: lineSpan, raw: line.text, name});
      continue;
    }
    if (trimmed.startsWith("[")) {
      if (!trimmed.endsWith("]")) {
        diagnostics.push(error("PXSTORY1012", "Command is missing a closing bracket", lineSpan));
        continue;
      }
      const commandText = trimmed.slice(1, -1).trim();
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
        raw: line.text,
        name: command!.raw,
        arguments: parseArguments(path, line, argumentTokens, diagnostics),
      });
      continue;
    }
    nodes.push({
      id,
      kind: activeSpeaker === undefined ? "narration" : "dialogue",
      span: lineSpan,
      raw: line.text,
      text: trimmed,
      ...(activeSpeaker === undefined ? {} : {name: activeSpeaker}),
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
  const authoredSourceIds = new Set<string>();
  let pendingAuthoredSourceId: string | undefined;

  const emit = (node: StoryNode, kind: string, args: Record<string, string>, text = node.text ?? node.raw.trim()): void => {
    const sourceId = pendingAuthoredSourceId === undefined
      ? node.id
      : `story-${stableId(`${context.documentId}:${pendingAuthoredSourceId}`)}`;
    pendingAuthoredSourceId = undefined;
    const operationId = `op-${stableId(`${context.documentId}:${sourceId}:${kind}`)}`;
    operations.push({operationId, sourceId, sourceLine: node.span.start.line, kind, text, arguments: args});
    mappings.push({operationId, sourceId, sourceUri: document.path, startLine: node.span.start.line, startColumn: node.span.start.column, endLine: node.span.end.line, endColumn: node.span.end.column});
  };

  for (const node of document.nodes) {
    if (node.kind === "comment" || node.kind === "speaker") continue;
    if (node.kind === "label") {
      if (node.name !== undefined) emit(node, "label", {target: node.name});
      continue;
    }
    if (node.kind === "dialogue") {
      const character = node.name === undefined ? undefined : characters.get(node.name);
      if ((context.characters?.length ?? 0) > 0 && character === undefined) diagnostics.push(error("PXSTORY1102", `Unknown character alias: ${node.name ?? ""}`, node.span));
      emit(node, "dialogue", {speaker: character?.displayName ?? node.name ?? "", character: character?.id ?? node.name ?? "", text: node.text ?? ""}, node.text ?? "");
      continue;
    }
    if (node.kind === "narration") {
      emit(node, "narration", {text: node.text ?? ""}, node.text ?? "");
      continue;
    }
    if (node.kind !== "command" || node.name === undefined) continue;
    const args = argumentMap(node);
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
      case "bg": {
        const assetName = named("asset") ?? positional(0);
        if (assetName === undefined) diagnostics.push(error("PXSTORY1110", "bg requires a resource", node.span));
        else {
          const resolved = context.resources?.[assetName] ?? assetName;
          if (context.resources !== undefined && context.resources[assetName] === undefined) diagnostics.push(error("PXSTORY1111", `Unknown background resource: ${assetName}`, node.span));
          emit(node, "background", {asset: resolved});
        }
        break;
      }
      case "show": {
        const alias = named("character") ?? positional(0);
        const character = alias === undefined ? undefined : characters.get(alias);
        if (alias === undefined) diagnostics.push(error("PXSTORY1112", "show requires a character alias", node.span));
        else if ((context.characters?.length ?? 0) > 0 && character === undefined) diagnostics.push(error("PXSTORY1113", `Unknown character alias: ${alias}`, node.span));
        else {
          const expressionAlias = named("expression");
          const expression = character?.expressions.find((value) => value.id === expressionAlias || value.aliases.includes(expressionAlias ?? ""));
          if (expressionAlias !== undefined && character !== undefined && expression === undefined) diagnostics.push(error("PXSTORY1114", `Unknown expression ${expressionAlias} for ${alias}`, node.span));
          emit(node, "showCharacter", {
            character: character?.id ?? alias,
            ...(expression === undefined ? {} : {expression: expression.id, sprite: `asset:${expression.assetId}`}),
            ...(named("position") === undefined ? {} : {position: named("position")!}),
          });
        }
        break;
      }
      case "hide": {
        const alias = named("character") ?? positional(0);
        const character = alias === undefined ? undefined : characters.get(alias);
        if (alias === undefined) diagnostics.push(error("PXSTORY1115", "hide requires a character alias", node.span));
        else emit(node, "hideCharacter", {character: character?.id ?? alias});
        break;
      }
      case "choice":
        if (target === undefined || named("text") === undefined) diagnostics.push(error("PXSTORY1116", "choice requires text and goto", node.span));
        else {
          if (!labels.has(target)) diagnostics.push(error("PXSTORY1117", `Unknown choice target: ${target}`, node.span));
          emit(node, "choiceOption", {text: named("text")!, target}, named("text")!);
        }
        break;
      case "jump":
        if (target === undefined || !labels.has(target)) diagnostics.push(error("PXSTORY1118", `Unknown jump target: ${target ?? ""}`, node.span));
        else emit(node, "jump", {target});
        break;
      case "call":
        if (target === undefined || !labels.has(target)) diagnostics.push(error("PXSTORY1119", `Unknown call target: ${target ?? ""}`, node.span));
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
          else emit(node, "setVariable", {name: variableName, value});
        }
        break;
      }
      case "if": emit(node, "condition", {expression: (node.arguments ?? []).map((value) => value.raw).join(" ")}); break;
      case "else": emit(node, "else", {}); break;
      case "endif": emit(node, "endCondition", {}); break;
      case "voice":
      case "bgm":
      case "se": {
        const asset = named("asset") ?? positional(0);
        if (asset === undefined) diagnostics.push(error("PXSTORY1124", `${node.name} requires a resource`, node.span));
        else emit(node, node.name === "se" ? "soundEffect" : node.name, {asset: context.resources?.[asset] ?? asset});
        break;
      }
      case "timeline": {
        const timeline = named("timeline") ?? positional(0);
        if (timeline === undefined) diagnostics.push(error("PXSTORY1125", "timeline requires a clip", node.span));
        else emit(node, "timeline", {timeline});
        break;
      }
      case "ui": emit(node, "ui", {route: named("route") ?? positional(0) ?? "", operation: named("operation") ?? "push"}); break;
      case "effect": {
        const value = named("value") ?? positional(0);
        if (value === undefined || value.length === 0)
          diagnostics.push(error("PXSTORY1126", "effect requires a preset", node.span));
        else emit(node, "effect", {value});
        break;
      }
      case "camera": {
        const value = named("value") ?? positional(0);
        if (value === undefined || value.length === 0)
          diagnostics.push(error("PXSTORY1127", "camera requires a preset", node.span));
        else emit(node, "camera", {value});
        break;
      }
      default: {
        const descriptor = extensions.get(node.name);
        if (descriptor === undefined) {
          diagnostics.push(error("PXSTORY1199", `Unknown Story command: ${node.name}`, node.span));
          break;
        }
        const normalized = normalizeExtensionArguments(node, descriptor, diagnostics);
        if (normalized !== undefined) emit(node, "customNode", {type: descriptor.id, value: canonicalJson(normalized, false).trim()});
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
  if (!valid) return {diagnostics, valid: false};
  return {
    runtimeIr: {format: "PrismatiXRuntimeIR", schemaRevision: 2, documentId: context.documentId, committedRevision: context.committedRevision ?? 0, operations},
    sourceMap: {format: "PrismatiXSourceMap", schemaRevision: 2, documentId: context.documentId, mappings},
    diagnostics,
    valid: true,
  };
}

export interface StoryCommandCompletion {
  readonly id: string;
  readonly parameters: readonly {readonly name: string; readonly required: boolean; readonly values?: readonly JsonValue[]}[];
}

export function storyCommandCompletions(extensions: readonly ExtensionCommand[] = []): readonly StoryCommandCompletion[] {
  const builtins = ["id", "bg", "show", "hide", "choice", "choice.wait", "jump", "call", "return", "end", "wait", "set", "if", "else", "endif", "voice", "bgm", "se", "timeline", "ui", "effect", "camera"];
  return [
    ...builtins.map((id) => ({id, parameters: []})),
    ...extensions.map((command) => ({id: command.id, parameters: command.parameters.map((parameter) => ({name: parameter.name, required: parameter.required === true, ...(parameter.enum === undefined ? {} : {values: parameter.enum})}))})),
  ];
}
