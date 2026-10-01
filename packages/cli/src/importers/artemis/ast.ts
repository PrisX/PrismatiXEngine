import {readFile} from "node:fs/promises";

/**
 * Reads Artemis `.ast` scenario files and `.tbl` system tables: Lua chunks of
 * `name = literal` assignments. Only literal tables, strings, numbers,
 * booleans and nil are accepted; no game code is ever evaluated.
 *
 * A table with only positional entries becomes an array. A table with named
 * or bracketed keys becomes a Map whose positional entries use keys 1..n,
 * in source order with the fields after them (as Lua numbers them).
 */
export type LuaValue = string | number | boolean | null | LuaValue[] | LuaTable;
export type LuaTable = Map<string | number, LuaValue>;

export class ArtemisSyntaxError extends Error {}

type TokenKind = "{" | "}" | "," | "[" | "]" | "=" | ";" | "string" | "number" | "identifier" | "eof";
interface Token {
  readonly kind: TokenKind;
  readonly value: string | number;
  readonly line: number;
  readonly column: number;
}

// Python's str.isspace(), which the original importer used: Unicode
// whitespace plus the ASCII separators U+001C-U+001F and U+0085, without U+FEFF.
const space = /^[\s\u001c-\u001f\u0085]$/u;
export function isSourceSpace(character: string): boolean {
  return character !== "﻿" && space.test(character);
}
const letter = /^[\p{L}_]$/u;
const wordCharacter = /^[\p{L}\p{N}_]$/u;
const digit = /^[0-9]$/u;
const escapes: Readonly<Record<string, string>> = {n: "\n", r: "\r", t: "\t"};

function tokenize(source: string): Token[] {
  const tokens: Token[] = [];
  const characters = Array.from(source);
  let index = 0;
  let line = 1;
  let column = 1;
  const advance = (character: string): void => {
    if (character === "\n") { line += 1; column = 1; } else column += 1;
  };
  const at = (offset = 0): string => characters[index + offset] ?? "";
  while (index < characters.length) {
    const character = at();
    if (isSourceSpace(character)) { advance(character); index += 1; continue; }
    if (character === "-" && at(1) === "-") {
      while (index < characters.length && at() !== "\n") { advance(at()); index += 1; }
      continue;
    }
    const startLine = line;
    const startColumn = column;
    if ("{},[]=;".includes(character)) {
      tokens.push({kind: character as TokenKind, value: character, line, column});
      advance(character); index += 1; continue;
    }
    if (character === "\"" || character === "'") {
      const quote = character;
      index += 1; advance(character);
      let text = "";
      let closed = false;
      while (index < characters.length) {
        const next = at();
        if (next === quote) { index += 1; advance(next); closed = true; break; }
        if (next === "\\") {
          index += 1; advance("\\");
          if (index >= characters.length)
            throw new ArtemisSyntaxError(`Unterminated escape at ${startLine}:${startColumn}`);
          const escaped = at();
          text += escapes[escaped] ?? escaped;
          advance(escaped); index += 1; continue;
        }
        text += next; advance(next); index += 1;
      }
      if (!closed) throw new ArtemisSyntaxError(`Unterminated string at ${startLine}:${startColumn}`);
      tokens.push({kind: "string", value: text, line: startLine, column: startColumn});
      continue;
    }
    if (letter.test(character)) {
      let text = "";
      while (index < characters.length && wordCharacter.test(at())) { text += at(); advance(at()); index += 1; }
      tokens.push({kind: "identifier", value: text, line: startLine, column: startColumn});
      continue;
    }
    if (digit.test(character) || (character === "-" && digit.test(at(1)))) {
      let text = "";
      if (character === "-") { text += "-"; advance(character); index += 1; }
      while (digit.test(at())) { text += at(); advance(at()); index += 1; }
      if (at() === "." && digit.test(at(1))) {
        text += "."; advance("."); index += 1;
        while (digit.test(at())) { text += at(); advance(at()); index += 1; }
      }
      tokens.push({kind: "number", value: Number(text), line: startLine, column: startColumn});
      continue;
    }
    throw new ArtemisSyntaxError(`Unexpected ${JSON.stringify(character)} at ${line}:${column}`);
  }
  tokens.push({kind: "eof", value: "", line, column});
  return tokens;
}

class Parser {
  private position = 0;
  constructor(private readonly tokens: readonly Token[]) {}

  private peek(offset = 0): Token { return this.tokens[Math.min(this.position + offset, this.tokens.length - 1)]!; }

  private pop(kind?: TokenKind): Token {
    const token = this.peek();
    if (kind !== undefined && token.kind !== kind)
      throw new ArtemisSyntaxError(`Expected ${kind}, got ${token.kind} at ${token.line}:${token.column}`);
    this.position += 1;
    return token;
  }

  value(): LuaValue {
    const token = this.peek();
    if (token.kind === "{") return this.table();
    if (token.kind === "string" || token.kind === "number") return this.pop().value;
    if (token.kind === "identifier" && (token.value === "true" || token.value === "false" || token.value === "nil")) {
      this.pop();
      return token.value === "nil" ? null : token.value === "true";
    }
    throw new ArtemisSyntaxError(`Expected literal at ${token.line}:${token.column}`);
  }

  private table(): LuaValue[] | LuaTable {
    this.pop("{");
    const entries: LuaValue[] = [];
    const fields: LuaTable = new Map();
    while (this.peek().kind !== "}") {
      if (this.peek().kind === "eof") throw new ArtemisSyntaxError("Unterminated table");
      if (this.peek().kind === "identifier" && this.peek(1).kind === "=") {
        const key = this.pop().value as string;
        this.pop("=");
        fields.set(key, this.value());
      } else if (this.peek().kind === "[") {
        this.pop("[");
        const key = this.value();
        this.pop("]");
        this.pop("=");
        if (typeof key !== "string" && typeof key !== "number")
          throw new ArtemisSyntaxError(`Unsupported table key at ${this.peek().line}:${this.peek().column}`);
        fields.set(key, this.value());
      } else entries.push(this.value());
      if (this.peek().kind === "," || this.peek().kind === ";") this.pop();
      else if (this.peek().kind !== "}") {
        const token = this.peek();
        throw new ArtemisSyntaxError(`Expected separator at ${token.line}:${token.column}`);
      }
    }
    this.pop("}");
    if (fields.size === 0) return entries;
    if (entries.length === 0) return fields;
    const merged: LuaTable = new Map(entries.map((value, index) => [index + 1, value]));
    for (const [key, value] of fields) merged.set(key, value);
    return merged;
  }

  document(): Map<string, LuaValue> {
    const document = new Map<string, LuaValue>();
    while (this.peek().kind !== "eof") {
      const key = String(this.pop("identifier").value);
      this.pop("=");
      document.set(key, this.value());
      if (this.peek().kind === ";") this.pop();
    }
    return document;
  }
}

/** Parse Artemis table text (BOM and CRLF tolerated). */
export function parseArtemisTables(text: string): Map<string, LuaValue> {
  const source = (text.charCodeAt(0) === 0xfeff ? text.slice(1) : text).replace(/\r\n?/gu, "\n");
  return new Parser(tokenize(source)).document();
}

export async function readArtemisTables(path: string): Promise<Map<string, LuaValue>> {
  try { return parseArtemisTables(await readFile(path, "utf8")); }
  catch (error) {
    if (error instanceof ArtemisSyntaxError) throw new ArtemisSyntaxError(`${path}: ${error.message}`);
    throw error;
  }
}

export function isTable(value: LuaValue | undefined): value is LuaTable {
  return value instanceof Map;
}

/** Positional entries of a table, in index order. */
export function numbered(value: LuaValue | undefined): LuaValue[] {
  if (Array.isArray(value)) return value;
  if (!isTable(value)) return [];
  return [...value.keys()].filter((key): key is number => typeof key === "number" && Number.isInteger(key))
    .sort((left, right) => left - right).map((key) => value.get(key)!);
}

/** `table[key]` with Lua nil (and a missing key) as null. */
export function field(value: LuaValue | undefined, key: string | number): LuaValue {
  return isTable(value) ? value.get(key) ?? null : null;
}
