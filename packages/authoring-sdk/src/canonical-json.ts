import type {JsonValue} from "./types.js";

function compareKeys(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

export function sortJson(value: unknown): JsonValue {
  if (Array.isArray(value)) return value.map(sortJson);
  if (value !== null && typeof value === "object") {
    const result: {[key: string]: JsonValue} = {};
    const source = value as Record<string, unknown>;
    for (const key of Object.keys(source).sort(compareKeys)) result[key] = sortJson(source[key]);
    return result;
  }
  if (typeof value === "number" && !Number.isFinite(value)) {
    throw new TypeError("Canonical JSON cannot encode a non-finite number");
  }
  if (value === null || typeof value === "boolean" || typeof value === "string" || typeof value === "number") return value;
  throw new TypeError(`Canonical JSON cannot encode ${typeof value}`);
}

export function canonicalJson(value: unknown, pretty = true): string {
  return `${JSON.stringify(sortJson(value), null, pretty ? 2 : undefined)}\n`;
}
