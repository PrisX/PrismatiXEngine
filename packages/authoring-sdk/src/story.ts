import {canonicalJson} from "./canonical-json.js";
import {canonicalResourceReference} from "./resources.js";
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

function validAudioDuration(value: string): boolean {
  if (!audioDurationPattern.test(value)) return false;
  const seconds = value.endsWith("s") && !value.endsWith("ms");
  const number = Number(value.replace(/(?:ms|s)$/u, ""));
  const milliseconds = number * (seconds ? 1000 : 1);
  return Number.isFinite(milliseconds) && milliseconds >= 0 && milliseconds <= 2_147_483_647;
}

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

function parseArguments(path: string, line: SourceLine, tokens: readonly Token[], diagnostics: AuthoringDiagnostic[], namedArguments = true): StoryArgument[] {
  return tokens.map((token) => {
    // `if` is an expression, not a command's name=value argument list.
    // Preserve operators and quoted '=' characters for the expression parser.
    const equals = namedArguments ? token.raw.indexOf("=") : -1;
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

  let activeSpeaker: {readonly name: string; readonly literal: boolean} | undefined;
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
      const authored = trimmed.slice(1).trim();
      if (authored.startsWith('"')) {
        let literal: unknown;
        try { literal = JSON.parse(authored) as unknown; }
        catch { literal = undefined; }
        if (typeof literal !== "string" || literal.trim().length === 0 ||
            Buffer.byteLength(literal, "utf8") > 512 ||
            /[\u0000-\u001f\u007f]/u.test(literal)) {
          diagnostics.push(error("PXSTORY1015",
            "Literal speaker must be a non-empty quoted name of at most 512 UTF-8 bytes", lineSpan));
          activeSpeaker = undefined;
        } else {
          activeSpeaker = {name: literal, literal: true};
          nodes.push({id, kind: "speaker", span: lineSpan, raw: line.text,
            name: literal, literalSpeaker: true});
        }
      } else {
        if (!identifierPattern.test(authored))
          diagnostics.push(error("PXSTORY1011", "Speaker must be a character alias", lineSpan));
        activeSpeaker = {name: authored, literal: false};
        nodes.push({id, kind: "speaker", span: lineSpan, raw: line.text,
          name: authored});
      }
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
        arguments: parseArguments(path, line, argumentTokens, diagnostics, command!.raw !== "if"),
      });
      continue;
    }
    nodes.push({
      id,
      kind: activeSpeaker === undefined ? "narration" : "dialogue",
      span: lineSpan,
      raw: line.text,
      text: trimmed,
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
  const authoredSourceIds = new Set<string>();
  let pendingAuthoredSourceId: string | undefined;

  const resolveResource = (node: StoryNode, authored: string): string | undefined => {
    const aliased = context.resources?.[authored] ?? authored;
    const resolved = canonicalResourceReference(aliased, context.resourceAssets ?? []);
    if (resolved === undefined) {
      diagnostics.push(error(
        "PXSTORY1111",
        `Resource must be a safe project-relative path or a cataloged asset identity: ${authored}`,
        node.span,
      ));
      return undefined;
    }
    resourceDependencies.add(resolved.source);
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
        resourceDependencies.add(resolved.source);
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
      resourceDependencies.add(resolved.source);
    }
    return value;
  };

  const emit = (node: StoryNode, kind: string, args: Record<string, string>, text = node.text ?? node.raw.trim()): void => {
    const sourceId = pendingAuthoredSourceId === undefined
      ? node.externalSourceId ?? node.id
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
      const character = node.literalSpeaker || node.name === undefined ?
        undefined : characters.get(node.name);
      if (!node.literalSpeaker && (context.characters?.length ?? 0) > 0 &&
          character === undefined)
        diagnostics.push(error("PXSTORY1102", `Unknown character alias: ${node.name ?? ""}`, node.span));
      const localizedName = context.locale === undefined ? undefined :
        character?.displayNames?.[context.locale];
      emit(node, "dialogue", {speaker: localizedName ?? character?.displayName ?? node.name ?? "", character: node.literalSpeaker ? "" : character?.id ?? node.name ?? "", text: node.text ?? ""}, node.text ?? "");
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
    const characterPosition = (value: string | undefined): string | undefined => {
      if (value === undefined) return undefined;
      const normalized = value.trim().toLowerCase();
      const positions: Readonly<Record<string, string>> = {
        left:"1", center:"2", centre:"2", right:"3", "1":"1", "2":"2", "3":"3",
      };
      return positions[normalized];
    };
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
          const resolved = resolveResource(node, assetName);
          const authoredRule = named("rule");
          const rule = authoredRule === undefined ? undefined : resolveResource(node, authoredRule);
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
        const alias = named("character") ?? positional(0);
        const character = alias === undefined ? undefined : characters.get(alias);
        const authoredAsset = named("asset") ?? named("sprite");
        if (alias === undefined) diagnostics.push(error("PXSTORY1112", "show requires a character alias", node.span));
        else if ((context.characters?.length ?? 0) > 0 && character === undefined) diagnostics.push(error("PXSTORY1113", `Unknown character alias: ${alias}`, node.span));
        else {
          const expressionAlias = named("expression");
          const expression = character?.expressions.find((value) => value.id === expressionAlias || value.aliases.includes(expressionAlias ?? ""));
          if (expressionAlias !== undefined && character !== undefined && expression === undefined) diagnostics.push(error("PXSTORY1114", `Unknown expression ${expressionAlias} for ${alias}`, node.span));
          const authoredPosition = named("position");
          const position = characterPosition(authoredPosition);
          const transition = named("transition");
          if (authoredPosition !== undefined && position === undefined) diagnostics.push(error("PXSTORY1128", `Unknown character position: ${authoredPosition}; expected left, center, right, 1, 2, or 3`, node.span));
          if (transition !== undefined && transition !== "true" && transition !== "false" && transition !== "auto") diagnostics.push(error("PXSTORY1145", "show transition must be true, false, or auto", node.span));
          const sprite = authoredAsset === undefined
            ? (expression === undefined ? undefined : `asset:${expression.assetId}`)
            : resolveResource(node, authoredAsset);
          if ((authoredAsset === undefined || sprite !== undefined) &&
              (transition === undefined || transition === "true" || transition === "false" || transition === "auto")) {
            emit(node, "showCharacter", {
              character: character?.id ?? alias,
              ...(expression === undefined ? {} : {expression: expression.id}),
              ...(sprite === undefined ? {} : {sprite}),
              ...(position === undefined ? {} : {position}),
              ...(named("x") === undefined ? {} : {x: named("x")!}),
              ...(named("y") === undefined ? {} : {y: named("y")!}),
              ...(named("scale") === undefined ? {} : {scale: named("scale")!}),
              ...(transition === undefined ? {} : {transition}),
            });
            if (authoredAsset === undefined && expression !== undefined && context.resourceAssets !== undefined) {
              resolveResource(node, `asset:${expression.assetId}`);
            }
          }
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
          const resolved = resolveResource(node, asset);
          const resolvedIntro = intro === undefined ? undefined : resolveResource(node, intro);
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
          const resolved = resolveResource(node, asset);
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
          if (resolved !== undefined) emit(node, "timeline", {timeline: resolved});
        }
        break;
      }
      case "ui": emit(node, "ui", {route: named("route") ?? positional(0) ?? "", operation: named("operation") ?? "push"}); break;
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
        if (duration === undefined) diagnostics.push(error("PXSTORY1138", "shake requires a duration", node.span));
        else emit(node, "shake", {
          duration,
          ...(amplitude === undefined ? {} : {amplitude}),
        });
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
  if (!valid) return {resourceDependencies: [...resourceDependencies].sort(), diagnostics, valid: false};
  return {
    runtimeIr: {format: "PrismatiXRuntimeIR", schemaRevision: 2, documentId: context.documentId, committedRevision: context.committedRevision ?? 0, operations},
    sourceMap: {format: "PrismatiXSourceMap", schemaRevision: 2, documentId: context.documentId, mappings},
    resourceDependencies: [...resourceDependencies].sort(),
    diagnostics,
    valid: true,
  };
}

export interface StoryCommandCompletion {
  readonly id: string;
  readonly parameters: readonly {readonly name: string; readonly required: boolean; readonly values?: readonly JsonValue[]}[];
}

export function storyCommandCompletions(extensions: readonly ExtensionCommand[] = []): readonly StoryCommandCompletion[] {
  const builtins = ["id", "bg", "show", "move", "hide", "choice", "choice.wait", "jump", "call", "return", "end", "wait", "set", "if", "else", "endif", "voice", "bgm", "bgm.stop", "se", "ambience", "ambience.stop", "video", "timeline", "ui", "unlock", "effect", "shake", "camera"];
  const builtinParameters: Readonly<Record<string, StoryCommandCompletion["parameters"]>> = {
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
    bgm: [{name:"asset", required:true}, {name:"fade", required:false}],
    "bgm.stop": [{name:"fade", required:false}],
    ambience: [{name:"asset", required:true}, {name:"fade", required:false}],
    "ambience.stop": [{name:"fade", required:false}],
    video: [{name:"asset", required:true}, {name:"skippable", required:false, values:[true, false]}],
    shake: [{name:"duration", required:true}, {name:"amplitude", required:false}],
    unlock: [{name:"kind", required:true}, {name:"id", required:true}],
  };
  return [
    ...builtins.map((id) => ({id, parameters: builtinParameters[id] ?? []})),
    ...extensions.map((command) => ({id: command.id, parameters: command.parameters.map((parameter) => ({name: parameter.name, required: parameter.required === true, ...(parameter.enum === undefined ? {} : {values: parameter.enum})}))})),
  ];
}
