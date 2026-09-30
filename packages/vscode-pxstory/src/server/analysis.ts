import type {StoryArgument, StoryDocument, StoryNode} from "@prismatix/authoring-sdk";

import type {LabelInfo} from "./project.js";
import type {Sdk} from "./sdk.js";
import {argumentSlots, namedArgument, positionalArgument} from "./story-roles.js";

/** Zero-based [start, end) character offsets on one line. */
export type Columns = readonly [number, number];

export interface MacroInfo {
  readonly name: string;
  readonly parameters: readonly {readonly name: string; readonly defaultValue?: string}[];
  /** Zero-based header and [endmacro] lines. */
  readonly line: number;
  readonly endLine: number;
  readonly body: string;
}

/** One Story document parsed by the SDK, plus the tables editor features use. */
export interface ParsedDocument {
  readonly path: string;
  readonly text: string;
  readonly lines: readonly string[];
  readonly document: StoryDocument;
  readonly labels: readonly LabelInfo[];
  readonly macros: readonly MacroInfo[];
  readonly imageLayers: readonly string[];
}

export function splitLines(text: string): string[] {
  return text.split(/\r?\n/u);
}

export function parseDocument(sdk: Sdk, text: string, path: string): ParsedDocument {
  const document = sdk.parseStory(text, path);
  const labels: LabelInfo[] = [];
  const macros: MacroInfo[] = [];
  const imageLayers = new Set<string>();
  for (const node of document.nodes) {
    if (node.kind === "label" && node.name !== undefined) {
      labels.push({name: node.name, line: node.span.start.line - 1});
    } else if (node.kind === "command" && node.name === "macro") {
      const [first, ...rest] = node.arguments ?? [];
      if (first === undefined || first.name !== undefined || typeof first.value !== "string") continue;
      macros.push({
        name: first.value,
        parameters: rest.map((argument) => argument.name === undefined
          ? {name: argument.raw} : {name: argument.name, defaultValue: argument.raw}),
        line: node.span.start.line - 1,
        endLine: node.span.end.line - 1,
        body: node.text ?? "",
      });
    } else if (node.kind === "command" && node.name === "image") {
      const name = namedArgument(node.arguments ?? [], "name") ?? positionalArgument(node.arguments ?? [], 0);
      if (name !== undefined) imageLayers.add(name);
    }
  }
  return {path, text, lines: splitLines(text), document, labels, macros,
    imageLayers: [...imageLayers].sort()};
}

const speakerTerminators = new Set(["[", "「", "『", "（", "(", "“"]);
const leadingTextTag = /^\[(?:\/?[bi]|br|\/(?:font|size|color|ruby)|(?:font|size|color|ruby)=[^\]]*)\]/u;

function argumentColumns(argument: StoryArgument, offset: number): Columns {
  const start = argument.span.start.column - 1 + offset;
  const end = argument.span.end.column - 1 + offset;
  return [argument.name === undefined ? start : start - argument.name.length - 1, end];
}

/** Parses one physical line on its own. Macro bodies and continued lines are
 * source text the SDK only parses at expansion, so features work per line. */
function parseLine(sdk: Sdk, lineText: string): StoryNode | undefined {
  return sdk.parseStory(lineText, "Story/line.pxstory").nodes[0];
}

/** Something under the cursor that hover and go-to-definition understand. */
export type LineTarget =
  | {readonly kind: "commandName"; readonly command: string; readonly columns: Columns;
    readonly args: readonly StoryArgument[]}
  | {readonly kind: "argument"; readonly command: string; readonly slot: string;
    readonly argument: StoryArgument; readonly args: readonly StoryArgument[]; readonly columns: Columns}
  | {readonly kind: "speakerName"; readonly name: string; readonly literal: boolean; readonly columns: Columns}
  | {readonly kind: "speakerOption"; readonly speaker: string; readonly slot: string;
    readonly argument: StoryArgument; readonly columns: Columns}
  | {readonly kind: "label"; readonly name: string; readonly columns: Columns};

export function targetAt(sdk: Sdk, lineText: string, character: number): LineTarget | undefined {
  const node = parseLine(sdk, lineText);
  if (node === undefined) return undefined;
  const within = ([start, end]: Columns) => character >= start && character <= end;
  if (node.kind === "label" && node.name !== undefined) {
    const start = lineText.indexOf(node.name);
    const columns: Columns = [start, start + node.name.length];
    return within(columns) ? {kind: "label", name: node.name, columns} : undefined;
  }
  if (node.kind === "command" && node.name !== undefined) {
    const args = node.arguments ?? [];
    let nameStart = lineText.indexOf("[") + 1;
    while (/\s/u.test(lineText[nameStart] ?? "")) nameStart += 1;
    const nameColumns: Columns = [nameStart, nameStart + node.name.length];
    if (within(nameColumns)) return {kind: "commandName", command: node.name, columns: nameColumns, args};
    const slots = argumentSlots(args);
    for (const [position, argument] of args.entries()) {
      const columns = argumentColumns(argument, 0);
      if (within(columns))
        return {kind: "argument", command: node.name, slot: slots[position]!, argument, args, columns};
    }
    return undefined;
  }
  if ((node.kind === "dialogue" || node.kind === "speaker") && node.name !== undefined &&
      lineText.trimStart().startsWith("@")) {
    let nameStart = lineText.indexOf("@") + 1;
    while (/\s/u.test(lineText[nameStart] ?? "")) nameStart += 1;
    const quoted = lineText[nameStart] === "\"";
    let nameEnd = nameStart;
    if (quoted) {
      nameEnd = nameStart + 1;
      while (nameEnd < lineText.length && lineText[nameEnd] !== "\"") nameEnd += lineText[nameEnd] === "\\" ? 2 : 1;
      nameEnd += 1;
    } else {
      while (nameEnd < lineText.length && !/\s/u.test(lineText[nameEnd]!) &&
             !speakerTerminators.has(lineText[nameEnd]!)) nameEnd += 1;
    }
    const nameColumns: Columns = [nameStart, nameEnd];
    if (within(nameColumns))
      return {kind: "speakerName", name: node.name, literal: node.literalSpeaker === true &&
        node.displaySpeaker !== true, columns: nameColumns};
    for (const argument of node.options ?? []) {
      const columns = argumentColumns(argument, 0);
      if (within(columns)) {
        // Mirror the compiler: a bare word is a position keyword or an expression.
        const slot = argument.name ?? (["left", "center", "centre", "right", "1", "2", "3"]
          .includes(argument.raw.toLowerCase()) ? "position" : "expression");
        return {kind: "speakerOption", speaker: node.name, slot, argument, columns};
      }
    }
  }
  return undefined;
}

export type CompletionContext =
  | {readonly kind: "commandName"; readonly prefix: string; readonly columns: Columns}
  | {readonly kind: "argument"; readonly command: string; readonly args: readonly StoryArgument[];
    /** The argument name being given a value, or undefined for a bare word. */
    readonly name?: string;
    /** For a bare word, its positional index. */
    readonly positionalIndex: number;
    readonly prefix: string;
    /** Columns replaced by a value completion (the word or the value after `=`). */
    readonly columns: Columns;
    readonly quote?: string}
  | {readonly kind: "speakerName"; readonly prefix: string; readonly columns: Columns}
  | {readonly kind: "speakerOption"; readonly speaker: string; readonly args: readonly StoryArgument[];
    readonly name?: string; readonly positionalIndex: number; readonly prefix: string;
    readonly columns: Columns; readonly quote?: string}
  | {readonly kind: "inlineTag"; readonly prefix: string; readonly columns: Columns}
  | {readonly kind: "inlineTagValue"; readonly tag: string; readonly prefix: string; readonly columns: Columns}
  | {readonly kind: "none"};

/** Quote state at the end of an in-progress command or option list. */
function openQuote(text: string): string | undefined {
  let quote = "";
  let escaped = false;
  for (const character of text) {
    if (escaped) escaped = false;
    else if (character === "\\" && quote !== "") escaped = true;
    else if (quote !== "") { if (character === quote) quote = ""; }
    else if (character === "\"" || character === "'") quote = character;
  }
  return quote === "" ? undefined : quote;
}

function argumentContext(args: readonly StoryArgument[], before: string, offset: number,
  quote: string | undefined): {name?: string; positionalIndex: number; prefix: string;
    columns: Columns; quote?: string} {
  const character = before.length;
  const positionalCount = args.filter((argument) => argument.name === undefined).length;
  const last = args.at(-1);
  const lastEnd = last === undefined ? -1 : last.span.end.column - 1 + offset;
  if (last === undefined || (quote === undefined && lastEnd < character) || /\s$/u.test(before) && quote === undefined)
    return {positionalIndex: positionalCount, prefix: "", columns: [character, character]};
  let start = last.span.start.column - 1 + offset;
  let prefix = last.raw;
  const quoted = quote ?? (/^["']/u.test(prefix) ? prefix[0] : undefined);
  if (quoted !== undefined && prefix.startsWith(quoted)) {
    start += 1;
    prefix = prefix.slice(1).replace(new RegExp(`${quoted}$`, "u"), "");
  }
  const columns: Columns = [start, character];
  if (last.name !== undefined)
    return {name: last.name, positionalIndex: positionalCount, prefix, columns,
      ...(quoted === undefined ? {} : {quote: quoted})};
  return {positionalIndex: positionalCount - 1, prefix, columns,
    ...(quoted === undefined ? {} : {quote: quoted})};
}

function textContext(before: string): CompletionContext {
  const open = /\[([^\]\[]*)$/u.exec(before);
  if (open === null) return {kind: "none"};
  const inside = open[1]!;
  const start = open.index + 1;
  const valued = /^(font|size|color|ruby)=(.*)$/u.exec(inside);
  if (valued !== null)
    return {kind: "inlineTagValue", tag: valued[1]!, prefix: valued[2]!,
      columns: [start + valued[1]!.length + 1, before.length]};
  return {kind: "inlineTag", prefix: inside, columns: [start, before.length]};
}

/** What is being typed at `character` on `lineText`, using the SDK parser on
 * the text before the cursor (closed with the brackets the author will type). */
export function completionContext(sdk: Sdk, lineText: string, character: number): CompletionContext {
  const before = lineText.slice(0, character);
  const trimmed = before.trimStart();
  const indent = before.length - trimmed.length;
  if (trimmed.startsWith(";") || trimmed.startsWith("*")) return {kind: "none"};

  if (trimmed.startsWith("@")) {
    let nameStart = indent + 1;
    while (/\s/u.test(before[nameStart] ?? "") && nameStart < before.length) nameStart += 1;
    if (before[nameStart] === "\"") {
      const close = before.indexOf("\"", nameStart + 1);
      return close < 0 ? {kind: "none"} : shiftContext(textContext(before.slice(close + 1)), close + 1);
    }
    let nameEnd = nameStart;
    while (nameEnd < before.length && !/\s/u.test(before[nameEnd]!) &&
           !speakerTerminators.has(before[nameEnd]!)) nameEnd += 1;
    if (nameEnd === before.length)
      return {kind: "speakerName", prefix: before.slice(nameStart), columns: [nameStart, character]};
    const speaker = before.slice(nameStart, nameEnd);
    if (before[nameEnd] === "[" && !before.slice(nameEnd).includes("]")) {
      const optionsText = before.slice(nameEnd + 1);
      const quote = openQuote(optionsText);
      const node = sdk.parseStory(`@${speaker}[${optionsText}${quote ?? ""}] x`, "Story/line.pxstory").nodes[0];
      if (node?.kind !== "dialogue") return {kind: "none"};
      // Option spans are relative to the synthetic line, whose options start
      // one column later than `speaker` begins in the real line... align them.
      const offset = nameEnd + 1 - (speaker.length + 2);
      return {kind: "speakerOption", speaker, args: node.options ?? [],
        ...argumentContext(node.options ?? [], before, offset, quote)};
    }
    const textStart = before[nameEnd] === "[" ? before.indexOf("]", nameEnd) + 1 : nameEnd;
    return shiftContext(textContext(before.slice(textStart)), textStart);
  }

  if (trimmed.startsWith("[") && !leadingTextTag.test(trimmed)) {
    const inner = before.slice(indent + 1);
    const nameMatch = /^(\s*)([^\s\]]*)$/u.exec(inner);
    if (nameMatch !== null) {
      const start = indent + 1 + nameMatch[1]!.length;
      return {kind: "commandName", prefix: nameMatch[2]!, columns: [start, character]};
    }
    const quote = openQuote(inner);
    if (quote === undefined && /\]\s*$/u.test(inner)) return {kind: "none"};
    const node = sdk.parseStory(`${trimmed}${quote ?? ""}]`, "Story/line.pxstory").nodes[0];
    if (node?.kind !== "command" || node.name === undefined) return {kind: "none"};
    const args = node.arguments ?? [];
    return {kind: "argument", command: node.name, args,
      ...argumentContext(args, before, indent, quote)};
  }
  return textContext(before);
}

function shiftContext(context: CompletionContext, offset: number): CompletionContext {
  if (context.kind !== "inlineTag" && context.kind !== "inlineTagValue") return context;
  return {...context, columns: [context.columns[0] + offset, context.columns[1] + offset]};
}
