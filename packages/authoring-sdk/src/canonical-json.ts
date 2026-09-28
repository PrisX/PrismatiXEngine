import type {JsonValue} from "./types.js";

function compareKeys(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

export function sortJson(value: unknown): JsonValue {
  if (Array.isArray(value)) return value.map(sortJson);
  if (value !== null && typeof value === "object") {
    const result: {[key: string]: JsonValue} = {};
    const source = value as Record<string, unknown>;
    const keys = Object.keys(source);
    // Runtime IR and source maps contain many objects whose keys are already
    // canonical. Avoid sorting those objects while preserving the exact output
    // for authored objects with arbitrary key order.
    for (let index = 1; index < keys.length; index += 1) {
      if (compareKeys(keys[index - 1]!, keys[index]!) > 0) {
        keys.sort(compareKeys);
        break;
      }
    }
    for (const key of keys) {
      const item = sortJson(source[key]);
      // Assignment would change the prototype instead of preserving this JSON key.
      if (key === "__proto__") Object.defineProperty(result, key,
        {value: item, enumerable: true, writable: true, configurable: true});
      else result[key] = item;
    }
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
