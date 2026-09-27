import type {SourceSpan} from "./types.js";

/** Locate effective JSON values by pointer in the exact text that was parsed.
 * Duplicate keys resolve to their last occurrence, as they do in JSON.parse.
 * A stale or unavailable source never supplies a misleading position. */
export function jsonPointerSpans(source: string | undefined, expected: unknown,
  path: string): ReadonlyMap<string, SourceSpan> {
  const spans = new Map<string, SourceSpan>();
  if (source === undefined) return spans;
  try {
    if (JSON.stringify(JSON.parse(source) as unknown) !== JSON.stringify(expected))
      return spans;
    let cursor = 0;
    const whitespace = (): void => {
      while (/[ \t\r\n]/u.test(source[cursor] ?? "")) cursor += 1;
    };
    const stringToken = (): string => {
      whitespace();
      const start = cursor++;
      while (cursor < source.length) {
        if (source[cursor] === "\\") { cursor += 2; continue; }
        if (source[cursor++] === '"') break;
      }
      return JSON.parse(source.slice(start, cursor)) as string;
    };
    const lineStarts = [0];
    for (let index = 0; index < source.length; index += 1)
      if (source[index] === "\n") lineStarts.push(index + 1);
    let locatedUtf16 = 0;
    let locatedBytes = 0;
    const position = (offset: number) => {
      // JSON values are visited in source order: each parent start is located
      // before its children, and its end after them. Count each UTF-8 byte
      // once without allocating a source-sized offset table.
      if (offset < locatedUtf16) throw new Error("JSON source offsets are not ordered");
      locatedBytes += Buffer.byteLength(source.slice(locatedUtf16, offset), "utf8");
      locatedUtf16 = offset;
      let low = 0;
      let high = lineStarts.length;
      while (low + 1 < high) {
        const middle = (low + high) >>> 1;
        if (lineStarts[middle]! <= offset) low = middle;
        else high = middle;
      }
      return {line: low + 1, column: offset - lineStarts[low]! + 1,
        offset: locatedBytes};
    };
    const value = (pointer: string): void => {
      whitespace();
      const start = cursor;
      const startPosition = position(start);
      if (source[cursor] === "{") {
        cursor += 1; whitespace();
        while (source[cursor] !== "}") {
          const key = stringToken();
          whitespace(); cursor += 1;
          value(`${pointer}/${key.replaceAll("~", "~0").replaceAll("/", "~1")}`);
          whitespace();
          if (source[cursor] !== ",") break;
          cursor += 1; whitespace();
        }
        cursor += 1;
      } else if (source[cursor] === "[") {
        cursor += 1; whitespace();
        let index = 0;
        while (source[cursor] !== "]") {
          value(`${pointer}/${index++}`);
          whitespace();
          if (source[cursor] !== ",") break;
          cursor += 1;
        }
        cursor += 1;
      } else if (source[cursor] === '"') {
        stringToken();
      } else {
        while (cursor < source.length && !/[ \t\r\n,\]}]/u.test(source[cursor]!))
          cursor += 1;
      }
      spans.set(pointer, {path, start: startPosition, end: position(cursor)});
    };
    value("");
  } catch {
    spans.clear();
  }
  return spans;
}
